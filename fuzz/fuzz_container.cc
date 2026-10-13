// Fuzz target: container header, footer, and trailer validation (TESTING.md 3.3 step 4). The input
// is a whole container file, checked as container 1 of a repository with the fixed repo_id below.
// Anything accepted must be canonical: re-encoding the header and the footer and trailer from the
// decoded entries reproduces the input's metadata bytes exactly.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "dds/common/check.h"
#include "dds/common/encoding.h"
#include "dds/store/container_format.h"

namespace {

constexpr dds::RepoId kRepoId = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
                                 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};
constexpr uint32_t kMaxDataSize = 65536;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const std::byte> file(reinterpret_cast<const std::byte*>(data), size);
  auto meta = dds::DecodeContainerMetadata(file, "fuzz.ctr", 1, kRepoId, kMaxDataSize);
  if (!meta.ok()) return 0;
  // Re-encoded in the version it was decoded from: 1 and 2 are both accepted (D107).
  const auto header = dds::EncodeContainerHeader(1, kRepoId, dds::LoadU32(file.data() + 8));
  DDS_CHECK(std::equal(header.begin(), header.end(), file.begin()));
  const auto tail = dds::EncodeContainerTail(header, meta->entries);
  const size_t tail_start = dds::kContainerHeaderSize + meta->payload_bytes;
  DDS_CHECK(tail_start + tail.size() == size);
  DDS_CHECK(
      std::equal(tail.begin(), tail.end(), file.begin() + static_cast<std::ptrdiff_t>(tail_start)));
  return 0;
}
