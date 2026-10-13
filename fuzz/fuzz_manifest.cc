// Fuzz target: snapshot manifest decoding (ON_DISK_FORMAT.md section 8; TESTING.md 3.3 step 4).
// The CRC32C makes random inputs fail early, so the seed corpus carries valid manifests and
// libFuzzer's CRC-aware mutations do the rest. Anything accepted must pass validation and be
// canonical: re-encoding reproduces the input exactly.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "dds/common/check.h"
#include "dds/snapshot/manifest.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
  auto manifest = dds::DecodeManifest(bytes);
  if (!manifest.ok()) return 0;
  DDS_CHECK(dds::ValidateManifest(*manifest).ok());
  const auto again = dds::EncodeManifest(*manifest);
  DDS_CHECK(std::equal(again.begin(), again.end(), bytes.begin(), bytes.end()));
  return 0;
}
