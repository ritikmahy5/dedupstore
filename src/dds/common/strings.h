#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace dds {

// Concatenates the decimal or text form of each argument. The standard library this project
// targets lacks std::format (D3), so messages are built with StrCat instead.
template <class... Args>
std::string StrCat(const Args&... args) {
  std::ostringstream out;
  // Promote 8-bit integers so they print as numbers, not characters. ([[maybe_unused]]: with no
  // arguments the fold below is empty and GCC would warn that `put` is unused.)
  [[maybe_unused]] auto put = [&out](const auto& arg) {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, uint8_t> || std::is_same_v<T, int8_t>) {
      out << static_cast<int>(arg);
    } else {
      out << arg;
    }
  };
  (put(args), ...);
  return out.str();
}

// Lowercase hexadecimal, two digits per byte.
std::string HexEncode(std::span<const std::byte> bytes);

// Decodes exactly out.size() bytes from 2 * out.size() lowercase hex digits. Returns false (and
// leaves `out` unspecified) for any other length, an uppercase digit, or a non-hex character:
// every hex field in the formats has one canonical spelling (ON_DISK_FORMAT.md section 3).
bool HexDecode(std::string_view hex, std::span<std::byte> out);

// Fixed-width lowercase hex: 8 digits for a u32, 16 for a u64.
std::string HexU32(uint32_t value);
std::string HexU64(uint64_t value);
bool ParseHexU32(std::string_view hex, uint32_t* out);
bool ParseHexU64(std::string_view hex, uint64_t* out);

// Canonical decimal (ON_DISK_FORMAT.md section 3): digits only, no sign for unsigned values, no
// leading zeros except "0" itself, no whitespace, no overflow. A negative value has one leading
// '-' and "-0" is rejected.
bool ParseCanonicalU64(std::string_view text, uint64_t* out);
bool ParseCanonicalI64(std::string_view text, int64_t* out);

}  // namespace dds
