// Fuzz target: every replication message decoder (PROTOCOL.md section 3). The first input byte
// picks the message type; the rest is its payload. Anything a decoder accepts must re-encode to
// exactly the payload, so each message has one encoding and a peer cannot smuggle extra bytes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "dds/common/check.h"
#include "dds/repl/messages.h"

namespace {

template <typename Decode, typename Encode>
void RoundTrip(std::span<const std::byte> payload, Decode decode, Encode encode) {
  auto message = decode(payload);
  if (!message.ok()) return;
  const auto again = encode(*message);
  DDS_CHECK(std::equal(again.begin(), again.end(), payload.begin(), payload.end()));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;
  const std::span<const std::byte> payload(reinterpret_cast<const std::byte*>(data) + 1, size - 1);
  switch (data[0] % 8) {
    case 0:
      RoundTrip(payload, dds::DecodeHello, dds::EncodeHello);
      break;
    case 1:
      RoundTrip(payload, dds::DecodeHelloOk, dds::EncodeHelloOk);
      break;
    case 2:
      RoundTrip(payload, dds::DecodeHave, dds::EncodeHave);
      break;
    case 3:
      RoundTrip(payload, dds::DecodeHaveReply, dds::EncodeHaveReply);
      break;
    case 4:
      RoundTrip(payload, dds::DecodePut, dds::EncodePut);
      break;
    case 5:
      RoundTrip(payload, dds::DecodePutAck, dds::EncodePutAck);
      break;
    case 6:
      RoundTrip(payload, dds::DecodeCommitOk, dds::EncodeCommitOk);
      break;
    default:
      RoundTrip(payload, dds::DecodeError, dds::EncodeError);
      break;
  }
  return 0;
}
