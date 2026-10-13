// Fuzz target: the `lock` record decoder (TESTING.md 3.3 step 4).

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "dds/common/check.h"
#include "dds/repo/repo_lock.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  auto record = dds::DecodeLockRecord(text);
  if (record.ok()) DDS_CHECK(dds::EncodeLockRecord(*record) == text);
  return 0;
}
