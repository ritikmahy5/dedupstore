// Fuzz target: replication frame decoding (PROTOCOL.md section 2). Anything accepted must re-encode
// to exactly the input.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "dds/common/check.h"
#include "dds/repl/frame.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);
  auto frame = dds::DecodeFrame(bytes);
  if (!frame.ok()) return 0;
  const auto again = dds::EncodeFrame(frame->type, frame->payload);
  DDS_CHECK(std::equal(again.begin(), again.end(), bytes.begin(), bytes.end()));
  return 0;
}
