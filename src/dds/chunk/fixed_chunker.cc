#include "dds/chunk/fixed_chunker.h"

namespace dds {

size_t FixedChunker::NextChunkLength(std::span<const std::byte> data, bool eof) const {
  if (data.empty()) return 0;
  if (data.size() >= size_) return size_;
  return eof ? data.size() : 0;
}

}  // namespace dds
