#include "dds/repl/frame.h"

#include "dds/common/crc32c.h"
#include "dds/common/encoding.h"
#include "dds/common/strings.h"

namespace dds {

namespace {

bool IsKnownType(uint8_t t) {
  switch (static_cast<MessageType>(t)) {
    case MessageType::kHello:
    case MessageType::kHelloOk:
    case MessageType::kHave:
    case MessageType::kHaveReply:
    case MessageType::kHaveTrees:
    case MessageType::kHaveTreesReply:
    case MessageType::kPut:
    case MessageType::kPutAck:
    case MessageType::kCommit:
    case MessageType::kCommitOk:
    case MessageType::kError:
      return true;
  }
  return false;
}

// Checks the CRC and type of `body` (type + payload) and returns the frame.
Result<Frame> FrameFromBody(std::span<const std::byte> body, uint32_t crc) {
  if (Crc32c(body) != crc) return Status::Corruption("frame CRC32C mismatch");
  const auto type = static_cast<uint8_t>(body[0]);
  if (!IsKnownType(type)) return Status::Corruption(StrCat("unknown message type ", type));
  Frame f;
  f.type = static_cast<MessageType>(type);
  f.payload.assign(body.begin() + 1, body.end());
  return f;
}

}  // namespace

const char* MessageTypeName(MessageType type) {
  switch (type) {
    case MessageType::kHello:
      return "HELLO";
    case MessageType::kHelloOk:
      return "HELLO_OK";
    case MessageType::kHave:
      return "HAVE";
    case MessageType::kHaveReply:
      return "HAVE_REPLY";
    case MessageType::kHaveTrees:
      return "HAVE_TREES";
    case MessageType::kHaveTreesReply:
      return "HAVE_TREES_REPLY";
    case MessageType::kPut:
      return "PUT";
    case MessageType::kPutAck:
      return "PUT_ACK";
    case MessageType::kCommit:
      return "COMMIT";
    case MessageType::kCommitOk:
      return "COMMIT_OK";
    case MessageType::kError:
      return "ERROR";
  }
  return "UNKNOWN";
}

std::vector<std::byte> EncodeFrame(MessageType type, std::span<const std::byte> payload) {
  Writer w;
  w.U32(static_cast<uint32_t>(payload.size() + 1));
  const size_t body_start = w.size();
  w.U8(static_cast<uint8_t>(type));
  w.Bytes(payload);
  const uint32_t crc = Crc32c(std::span(w.buffer()).subspan(body_start));
  w.U32(crc);
  return w.Take();
}

Result<Frame> DecodeFrame(std::span<const std::byte> bytes) {
  Reader r(bytes);
  uint32_t length = 0;
  if (!r.U32(&length) || length < 1 || length > kMaxFrameLength) {
    return Status::Corruption("frame length out of range");
  }
  std::span<const std::byte> body;
  uint32_t crc = 0;
  if (!r.Bytes(length, &body) || !r.U32(&crc) || !r.empty()) {
    return Status::Corruption("frame is truncated or has trailing bytes");
  }
  return FrameFromBody(body, crc);
}

Result<size_t> WriteFrame(Connection& c, MessageType type, std::span<const std::byte> payload) {
  const auto bytes = EncodeFrame(type, payload);
  DDS_TRY(c.Send(bytes));
  return bytes.size();
}

Result<Frame> ReadFrame(Connection& c, std::chrono::milliseconds timeout) {
  std::array<std::byte, 4> head{};
  DDS_TRY(c.Receive(head, timeout));
  const uint32_t length = LoadU32(head.data());
  if (length < 1 || length > kMaxFrameLength) {
    return Status::Corruption(StrCat("frame length ", length, " out of range"));
  }
  // The length is checked before allocating, so a hostile peer cannot make this reserve more than
  // one maximal frame.
  std::vector<std::byte> rest(static_cast<size_t>(length) + 4);
  DDS_TRY(c.Receive(rest, timeout));
  return FrameFromBody(std::span(rest).first(length), LoadU32(rest.data() + length));
}

}  // namespace dds
