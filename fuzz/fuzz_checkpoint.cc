// Fuzz target: index checkpoint validation (TESTING.md 3.3 step 4; D104). The input is a whole
// checkpoint file of a repository with the fixed repo_id below. Anything accepted must be
// canonical: re-encoding the decoded entries and tables reproduces the input exactly.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dds/common/check.h"
#include "dds/repo/index_checkpoint_format.h"

namespace {

constexpr dds::RepoId kRepoId = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
                                 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};
constexpr uint32_t kMaxDataSize = 65536;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const std::byte> file(reinterpret_cast<const std::byte*>(data), size);
  auto decoded = dds::DecodeIndexCheckpoint(file, "fuzz.ckpt", kRepoId, kMaxDataSize);
  if (!decoded.ok()) return 0;
  std::vector<std::byte> encoded;
  for (const dds::SealedEntry& e : decoded->entries) dds::AppendCheckpointEntry(e, encoded);
  const std::vector<std::byte> tail = dds::EncodeCheckpointTail(decoded->tables, kRepoId);
  encoded.insert(encoded.end(), tail.begin(), tail.end());
  DDS_CHECK(encoded.size() == size && std::equal(encoded.begin(), encoded.end(), file.begin()));
  return 0;
}
