#pragma once

#include <chrono>
#include <cstddef>
#include <span>

#include "dds/common/status.h"

namespace dds {

// A reliable, ordered byte stream between a replication sender and receiver: TcpConnection over a
// socket, or one end of a SimulatedLink in tests and benchmarks (D28). One thread may Send while
// another Receives (the sender has a writer thread and a reader thread, PROTOCOL.md 4.1); Close may
// be called from any thread and wakes both.
class Connection {
 public:
  virtual ~Connection() = default;
  // Sends all of `data`. Aborted if the connection is closed or broken.
  virtual Status Send(std::span<const std::byte> data) = 0;
  // Fills `out` exactly. Aborted if the peer closed or the connection broke before `out` was full
  // (including in the middle of a message), or if `timeout` passed first (PROTOCOL.md 6).
  virtual Status Receive(std::span<std::byte> out, std::chrono::milliseconds timeout) = 0;
  // Ends the connection in both directions.
  virtual void Close() = 0;
};

}  // namespace dds
