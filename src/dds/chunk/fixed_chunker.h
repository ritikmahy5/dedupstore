#pragma once

#include <cstddef>
#include <span>

#include "dds/chunk/chunker.h"

namespace dds {

// Cuts every `size` bytes. Exists only as the measured baseline for FastCDC (D8, E1): an insertion
// shifts every later boundary, which is exactly what content-defined chunking avoids.
class FixedChunker final : public Chunker {
 public:
  explicit FixedChunker(size_t size) : size_(size) {}

  size_t NextChunkLength(std::span<const std::byte> data, bool eof) const override;
  size_t MinSize() const override { return size_; }
  size_t MaxSize() const override { return size_; }

 private:
  size_t size_;
};

}  // namespace dds
