#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "dds/common/result.h"
#include "dds/common/unique_fd.h"
#include "dds/repl/connection.h"

namespace dds {

// "HOST:PORT", or "[IPV6]:PORT" (SPEC.md section 3: --listen and --to).
struct Endpoint {
  std::string host;
  uint16_t port = 0;
};
Result<Endpoint> ParseEndpoint(std::string_view text);

// A Connection over a TCP socket. TCP_NODELAY is set, because the protocol pipelines small frames
// (HAVE_REPLY, PUT_ACK) that Nagle's algorithm would hold back; SIGPIPE is suppressed, so a peer
// that went away is an Aborted status, not a dead process. Close shuts the socket down, which
// wakes a Receive blocked on another thread; the descriptor is closed by the destructor.
class TcpConnection final : public Connection {
 public:
  explicit TcpConnection(UniqueFd fd) : fd_(std::move(fd)) {}
  static Result<std::unique_ptr<TcpConnection>> Connect(const Endpoint& to);

  Status Send(std::span<const std::byte> data) override;
  Status Receive(std::span<std::byte> out, std::chrono::milliseconds timeout) override;
  void Close() override;

 private:
  UniqueFd fd_;
};

// A listening TCP socket.
class TcpListener {
 public:
  // Port 0 picks a free port; port() then says which.
  static Result<std::unique_ptr<TcpListener>> Listen(const Endpoint& at);
  uint16_t port() const { return port_; }
  // Waits up to `timeout` for a connection: nullptr if none came.
  Result<std::unique_ptr<TcpConnection>> Accept(std::chrono::milliseconds timeout);

  TcpListener(UniqueFd fd, uint16_t port) : fd_(std::move(fd)), port_(port) {}

 private:
  UniqueFd fd_;
  uint16_t port_;
};

}  // namespace dds
