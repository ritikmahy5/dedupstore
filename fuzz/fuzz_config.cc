// Fuzz target: the `config` decoder (TESTING.md 3.3 step 4). Arbitrary bytes must never crash it,
// and anything it accepts must re-encode to exactly the input (the format is canonical).

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "dds/common/check.h"
#include "dds/repo/repo_config.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  auto config = dds::DecodeRepoConfig(text);
  if (config.ok()) DDS_CHECK(dds::EncodeRepoConfig(*config) == text);
  return 0;
}
