#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dds/common/result.h"
#include "dds/repl/connection.h"

namespace dds {

// Message types (PROTOCOL.md section 3). Values cross the wire: never renumber.
enum class MessageType : uint8_t {
  kHello = 0x01,
  kHelloOk = 0x02,
  kHave = 0x10,
  kHaveReply = 0x11,
  kHaveTrees = 0x12,       // protocol version 2 (D108)
  kHaveTreesReply = 0x13,  // protocol version 2 (D108)
  kPut = 0x20,
  kPutAck = 0x21,
  kCommit = 0x30,
  kCommitOk = 0x31,
  kError = 0x7F,
};

const char* MessageTypeName(MessageType type);

// The frame's length field counts type + payload: 1 to 65 MiB (PROTOCOL.md 2 and 6).
inline constexpr uint32_t kMaxFrameLength = 65u << 20;
inline constexpr size_t kFrameOverhead = 8;  // length and CRC32C

struct Frame {
  MessageType type = MessageType::kError;
  std::vector<std::byte> payload;
};

// length | type | payload | CRC32C(type + payload). `payload` must fit kMaxFrameLength - 1.
std::vector<std::byte> EncodeFrame(MessageType type, std::span<const std::byte> payload);

// Parses one whole encoded frame (used by the fuzz target and by ReadFrame). Corruption for a bad
// length, a CRC mismatch, an unknown type, or trailing bytes.
Result<Frame> DecodeFrame(std::span<const std::byte> bytes);

// Writes one frame. Returns the bytes put on the wire, for the wire counters (E7).
Result<size_t> WriteFrame(Connection& c, MessageType type, std::span<const std::byte> payload);

// Reads one frame, waiting at most `timeout` for it to start and finish. Aborted on disconnect or
// timeout; Corruption for anything malformed.
Result<Frame> ReadFrame(Connection& c, std::chrono::milliseconds timeout);

}  // namespace dds
