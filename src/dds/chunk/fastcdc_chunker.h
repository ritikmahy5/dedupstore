#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "dds/chunk/chunker.h"
#include "dds/chunk/gear_table.h"

namespace dds {

// The FastCDC cut-point mask with `bits` one bits, for bits in 5..25 (D62). A position is a cut
// point when (gear hash & mask) == 0, which happens with probability 2^-bits on random input.
uint64_t FastCdcMask(int bits);

// FastCDC (Xia et al., "FastCDC: a Fast and Efficient Content-Defined Chunking Approach for Data
// Deduplication", USENIX ATC 2016), Algorithm 1, with normalized chunking (D8, D62):
// - the Gear rolling hash: fp = (fp << 1) + gear[byte], one shift and one add per byte;
// - cut-point skipping: hashing starts min_size bytes into the chunk, so no chunk is shorter;
// - normalized chunking: before avg_size the stricter mask (log2(avg) + level one bits), after
//   it the looser one (log2(avg) - level bits), which concentrates chunk sizes around avg_size;
// - a cut at max_size if no cut point appears.
// Built through MakeChunker, which validates the parameters.
class FastCdcChunker final : public Chunker {
 public:
  // `params` must be valid (ValidateChunkerParams).
  explicit FastCdcChunker(const ChunkerParams& params);

  size_t NextChunkLength(std::span<const std::byte> data, bool eof) const override;
  size_t MinSize() const override { return min_size_; }
  size_t MaxSize() const override { return max_size_; }

 private:
  GearTable gear_;
  size_t min_size_;
  size_t avg_size_;
  size_t max_size_;
  uint64_t mask_s_;  // before avg_size: more one bits, cut points rarer
  uint64_t mask_l_;  // after avg_size: fewer one bits, cut points more frequent
};

}  // namespace dds
