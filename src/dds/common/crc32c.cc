#include "dds/common/crc32c.h"

#include <array>

#if defined(__x86_64__)
#include <nmmintrin.h>
#elif defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

#include "dds/common/check.h"
#include "dds/common/encoding.h"

namespace dds {

namespace {

constexpr uint32_t kPolynomial = 0x82F63B78;  // 0x1EDC6F41 bit-reversed (LSB-first CRC)

using Table = std::array<std::array<uint32_t, 256>, 8>;

// Slicing-by-8 tables (Kounavis and Berry, "A Systematic Approach to Building High Performance
// Software-based CRC Generators", ISCC 2005; the scheme Intel published as "slicing-by-8").
//
// table[0][b] is the classic one-byte table: the CRC register after shifting byte b through it.
// table[k][b] is the effect of byte b followed by k zero bytes:
//   table[k][b] = (table[k-1][b] >> 8) ^ table[0][table[k-1][b] & 0xFF].
// Because CRC is linear over GF(2), the register after 8 input bytes is the XOR of each byte's
// contribution, and the byte at position j of the block (0 = first) is followed by 7 - j more
// bytes, so it is looked up in table[7 - j]. That turns 8 dependent steps into 8 independent
// lookups.
constexpr Table MakeTable() {
  Table t{};
  for (uint32_t b = 0; b < 256; ++b) {
    uint32_t crc = b;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? kPolynomial : 0);
    t[0][b] = crc;
  }
  for (size_t k = 1; k < 8; ++k) {
    for (size_t b = 0; b < 256; ++b) t[k][b] = (t[k - 1][b] >> 8) ^ t[0][t[k - 1][b] & 0xFF];
  }
  return t;
}

constexpr Table kTable = MakeTable();

// Every Update* function works on the raw register; Crc32cExtend inverts on entry and exit.

// Processes bytes one at a time.
uint32_t UpdateBytewise(uint32_t state, const std::byte* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    state = kTable[0][(state ^ static_cast<uint32_t>(p[i])) & 0xFF] ^ (state >> 8);
  }
  return state;
}

uint32_t UpdateSlicing8(uint32_t state, const std::byte* p, size_t n) {
  while (n >= 8) {
    // Little-endian loads: the first input byte must land in the low byte of the register.
    const uint32_t lo = LoadU32(p) ^ state;
    const uint32_t hi = LoadU32(p + 4);
    state = kTable[7][lo & 0xFF] ^ kTable[6][(lo >> 8) & 0xFF] ^ kTable[5][(lo >> 16) & 0xFF] ^
            kTable[4][lo >> 24] ^ kTable[3][hi & 0xFF] ^ kTable[2][(hi >> 8) & 0xFF] ^
            kTable[1][(hi >> 16) & 0xFF] ^ kTable[0][hi >> 24];
    p += 8;
    n -= 8;
  }
  return UpdateBytewise(state, p, n);
}

// Hardware CRC32C (D103). Both instruction sets compute this exact CRC (the Castagnoli
// polynomial, reflected, on the raw register), 8 bytes per instruction, the first byte in the low
// bits of the little-endian load, as in UpdateSlicing8.
#if defined(__x86_64__)
constexpr bool kHardwareCompiled = true;
constexpr std::string_view kHardwareName = "sse4.2";

// SSE4.2 `crc32` (Intel SDM vol. 2A). The target attribute compiles only this function for SSE4.2,
// so the binary still runs on a CPU without it; Crc32cExtend calls it only when
// CpuHasHardware() said yes.
__attribute__((target("sse4.2"))) uint32_t UpdateHardware(uint32_t state, const std::byte* p,
                                                          size_t n) {
  uint64_t wide = state;
  for (; n >= 8; p += 8, n -= 8) wide = _mm_crc32_u64(wide, LoadU64(p));
  auto narrow = static_cast<uint32_t>(wide);  // the instruction leaves the upper half zero
  for (; n > 0; ++p, --n) narrow = _mm_crc32_u8(narrow, static_cast<uint8_t>(*p));
  return narrow;
}

bool CpuHasHardware() {
  __builtin_cpu_init();  // required only before constructors run, harmless after
  return __builtin_cpu_supports("sse4.2") != 0;
}
#elif defined(__ARM_FEATURE_CRC32)
constexpr bool kHardwareCompiled = true;
constexpr std::string_view kHardwareName = "armv8-crc";

// The ARMv8 CRC32 extension (ACLE __crc32cd and __crc32cb). The compiler defines
// __ARM_FEATURE_CRC32 only when every CPU it targets has the extension (all Apple arm64 CPUs do),
// so there is nothing to check at run time.
uint32_t UpdateHardware(uint32_t state, const std::byte* p, size_t n) {
  for (; n >= 8; p += 8, n -= 8) state = __crc32cd(state, LoadU64(p));
  for (; n > 0; ++p, --n) state = __crc32cb(state, static_cast<uint8_t>(*p));
  return state;
}

bool CpuHasHardware() { return true; }
#else
constexpr bool kHardwareCompiled = false;
constexpr std::string_view kHardwareName = "none";

uint32_t UpdateHardware(uint32_t state, const std::byte* p, size_t n) {
  return UpdateSlicing8(state, p, n);
}

bool CpuHasHardware() { return false; }
#endif

// Decided once per process: the CPU does not change under it. The hardware is checked against
// the standard check value before its first use, and a mismatch aborts, so a misdetected or
// miscompiled path fails loudly instead of writing wrong CRCs into containers.
bool UseHardware() {
  static const bool use = [] {
    if (!kHardwareCompiled || !CpuHasHardware()) return false;
    const std::span<const std::byte> check = AsBytes("123456789");
    DDS_CHECK(~UpdateHardware(~0u, check.data(), check.size()) == 0xE3069283u);
    return true;
  }();
  return use;
}

}  // namespace

uint32_t Crc32cExtend(uint32_t crc, std::span<const std::byte> data) {
  // The register holds the inverted CRC: invert on entry and exit (initial value and final XOR
  // 0xFFFFFFFF), which is what makes Extend(Crc32c(a), b) equal Crc32c(a ‖ b).
  if (UseHardware()) return ~UpdateHardware(~crc, data.data(), data.size());
  return ~UpdateSlicing8(~crc, data.data(), data.size());
}

uint32_t Crc32c(std::span<const std::byte> data) { return Crc32cExtend(0, data); }

uint32_t Crc32cExtendSoftware(uint32_t crc, std::span<const std::byte> data) {
  return ~UpdateSlicing8(~crc, data.data(), data.size());
}

uint32_t Crc32cBytewise(std::span<const std::byte> data) {
  return ~UpdateBytewise(~0u, data.data(), data.size());
}

std::string_view Crc32cImplementation() { return UseHardware() ? kHardwareName : "slicing-by-8"; }

}  // namespace dds
