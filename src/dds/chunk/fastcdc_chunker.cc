#include "dds/chunk/fastcdc_chunker.h"

#include <algorithm>
#include <array>
#include <bit>

#include "dds/common/check.h"

namespace dds {

namespace {

// Masks indexed by their number of one bits (D62), transcribed from the `MASKS` table of the
// fastcdc-rs crate's v2016 module (https://github.com/nlfiedler/fastcdc-rs, src/v2016/mod.rs,
// checked 2026-10-07). Its 11- and 13-bit entries equal the paper's MaskL and MaskA. The bits are
// spread over the top of a 48-bit window: bit k of the Gear hash depends on the last k + 1 bytes,
// so this makes each cut decision depend on about the last 48 bytes, like Rabin-based CDC's
// 48-byte window (paper, section 4.2).
constexpr std::array<uint64_t, 26> kMasks = {
    0,
    0,
    0,
    0,
    0,
    0x0000000001804110ULL,  // 5 bits (only with normalization 3 at 256 B)
    0x0000000001803110ULL,  // 6
    0x0000000018035100ULL,  // 7
    0x0000001800035300ULL,  // 8: 256 B
    0x0000019000353000ULL,  // 9
    0x0000590003530000ULL,  // 10
    0x0000d90003530000ULL,  // 11: the paper's MaskL
    0x0000d90103530000ULL,  // 12
    0x0000d90303530000ULL,  // 13: 8 KiB, the paper's MaskA
    0x0000d90313530000ULL,  // 14
    0x0000d90f03530000ULL,  // 15
    0x0000d90303537000ULL,  // 16
    0x0000d90703537000ULL,  // 17
    0x0000d90707537000ULL,  // 18
    0x0000d91707537000ULL,  // 19
    0x0000d91747537000ULL,  // 20
    0x0000d91767537000ULL,  // 21
    0x0000d93767537000ULL,  // 22: 4 MiB
    0x0000d93777537000ULL,  // 23
    0x0000d93777577000ULL,  // 24
    0x0000db3777577000ULL,  // 25 (only with normalization 3 at 4 MiB)
};

}  // namespace

uint64_t FastCdcMask(int bits) {
  DDS_CHECK(bits >= 5 && bits <= 25);
  return kMasks[static_cast<size_t>(bits)];
}

FastCdcChunker::FastCdcChunker(const ChunkerParams& params)
    : gear_(GenerateGearTable(params.gear_seed)),
      min_size_(params.min_size),
      avg_size_(params.avg_size),
      max_size_(params.max_size) {
  const int bits = std::countr_zero(params.avg_size);  // avg_size is a power of two
  mask_s_ = FastCdcMask(bits + params.normalization);
  mask_l_ = FastCdcMask(bits - params.normalization);
}

size_t FastCdcChunker::NextChunkLength(std::span<const std::byte> data, bool eof) const {
  if (data.empty()) return 0;
  if (!eof && data.size() < max_size_) return 0;  // Chunker contract: decide on a full window

  // From here this is Algorithm 1. n: the bytes this chunk may use.
  const size_t n = std::min(data.size(), max_size_);
  if (n <= min_size_) return n;
  const size_t normal_size = std::min(avg_size_, n);
  uint64_t fp = 0;
  size_t i = min_size_;  // cut-point skipping: the first min_size bytes are never hashed
  for (; i < normal_size; ++i) {
    fp = (fp << 1) + gear_[static_cast<uint8_t>(data[i])];
    // Like the paper and fastcdc-rs, the chunk ends before byte i, whose hash matched.
    if ((fp & mask_s_) == 0) return i;
  }
  for (; i < n; ++i) {
    fp = (fp << 1) + gear_[static_cast<uint8_t>(data[i])];
    if ((fp & mask_l_) == 0) return i;
  }
  return n;
}

}  // namespace dds
