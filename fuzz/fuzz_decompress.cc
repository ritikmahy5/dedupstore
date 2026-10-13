// Fuzz target: decompressing a stored blob's zstd frame (TESTING.md 3.3 step 4; D107). Whatever the
// bytes, DecompressZstd must return 1 to kLimit bytes of content or Corruption, and never read or
// write out of bounds. There is no canonical-encoding property: many frames decode to one content.
// Seeds come from the zstd 1.5.7 command-line tool.

#include <cstddef>
#include <cstdint>
#include <span>

#include "dds/common/check.h"
#include "dds/store/compression.h"

namespace {

constexpr uint32_t kLimit = 65536;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const std::byte> stored(reinterpret_cast<const std::byte*>(data), size);
  auto content = dds::DecompressZstd(stored, kLimit);
  if (content.ok()) {
    DDS_CHECK(!content->empty() && content->size() <= kLimit);
  } else {
    DDS_CHECK(content.status().code() == dds::StatusCode::kCorruption);
  }
  return 0;
}
