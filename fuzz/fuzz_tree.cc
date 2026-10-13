// Fuzz target: tree blob decoding (ON_DISK_FORMAT.md section 6; TESTING.md 3.3 step 4). Trees have
// no checksum of their own, so the decoder is the only guard against hostile names (G10). Anything
// accepted must be canonical: re-encoding it reproduces the input exactly.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "dds/common/check.h"
#include "dds/snapshot/names.h"
#include "dds/snapshot/tree.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
  auto tree = dds::DecodeTree(bytes);
  if (!tree.ok()) return 0;
  for (const dds::TreeEntry& e : tree->entries) DDS_CHECK(dds::IsValidName(e.name));
  auto again = dds::EncodeTree(*tree);
  DDS_CHECK(again.ok());
  DDS_CHECK(std::equal(again->begin(), again->end(), bytes.begin(), bytes.end()));
  return 0;
}
