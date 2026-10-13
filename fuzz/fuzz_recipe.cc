// Fuzz target: recipe blob decoding (ON_DISK_FORMAT.md section 7; TESTING.md 3.3 step 4), with the
// default max_size. Anything accepted must be canonical: re-encoding reproduces the input exactly.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "dds/common/check.h"
#include "dds/snapshot/recipe.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
  auto recipe = dds::DecodeRecipe(bytes, 65536);
  if (!recipe.ok()) return 0;
  for (const dds::ChunkRef& ref : recipe->refs) DDS_CHECK(ref.length >= 1 && ref.length <= 65536);
  const auto again = dds::EncodeRecipe(*recipe);
  DDS_CHECK(std::equal(again.begin(), again.end(), bytes.begin(), bytes.end()));
  return 0;
}
