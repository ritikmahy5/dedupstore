#include "dds/repl/tcp.h"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "dds/common/strings.h"

namespace dds {

namespace {

Status SocketError(const char* op, const std::string& what) {
  return Status::IoError(StrCat(op, " ", what, ": ", std::strerror(errno)));
}

std::string Describe(const Endpoint& e) { return StrCat(e.host, ":", e.port); }

// getaddrinfo's results, freed on scope exit.
struct AddrInfo {
  addrinfo* list = nullptr;
  ~AddrInfo() {
    if (list != nullptr) ::freeaddrinfo(list);
  }
};

Status Resolve(const Endpoint& e, bool passive, AddrInfo& out) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (passive) hints.ai_flags = AI_PASSIVE;
  const std::string port = std::to_string(e.port);
  const int rc = ::getaddrinfo(e.host.c_str(), port.c_str(), &hints, &out.list);
  if (rc != 0) {
    return Status::NotFound(StrCat("cannot resolve ", Describe(e), ": ", ::gai_strerror(rc)));
  }
  return Status::Ok();
}

void Configure(int fd) {
  const int one = 1;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#if defined(SO_NOSIGPIPE)
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));  // macOS
#endif
}

constexpr int kSendFlags =
#if defined(MSG_NOSIGNAL)
    MSG_NOSIGNAL;  // Linux: no SIGPIPE when the peer has gone
#else
    0;
#endif

uint16_t BoundPort(int fd) {
  sockaddr_storage addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
  if (addr.ss_family == AF_INET) return ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
  return ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
}

}  // namespace

Result<Endpoint> ParseEndpoint(std::string_view text) {
  const size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0) {
    return Status::InvalidArgument(StrCat("'", text, "' is not HOST:PORT"));
  }
  std::string_view host = text.substr(0, colon);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  uint64_t port = 0;
  if (!ParseCanonicalU64(text.substr(colon + 1), &port) || port > 65535) {
    return Status::InvalidArgument(StrCat("'", text, "': the port must be 0 to 65535"));
  }
  return Endpoint{std::string(host), static_cast<uint16_t>(port)};
}

Result<std::unique_ptr<TcpConnection>> TcpConnection::Connect(const Endpoint& to) {
  AddrInfo addrs;
  DDS_TRY(Resolve(to, /*passive=*/false, addrs));
  Status last = Status::IoError(StrCat("connect ", Describe(to), ": no addresses"));
  for (addrinfo* a = addrs.list; a != nullptr; a = a->ai_next) {
    UniqueFd fd(::socket(a->ai_family, a->ai_socktype, a->ai_protocol));
    if (!fd.valid()) {
      last = SocketError("socket", Describe(to));
      continue;
    }
    if (::connect(fd.get(), a->ai_addr, a->ai_addrlen) != 0) {
      last = SocketError("connect", Describe(to));  // refused, unreachable: the OS call failed
      continue;
    }
    Configure(fd.get());
    return std::make_unique<TcpConnection>(std::move(fd));
  }
  return last;
}

Status TcpConnection::Send(std::span<const std::byte> data) {
  size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::send(fd_.get(), data.data() + sent, data.size() - sent, kSendFlags);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return Status::Aborted(StrCat("tcp send: ", std::strerror(errno)));
    sent += static_cast<size_t>(n);
  }
  return Status::Ok();
}

Status TcpConnection::Receive(std::span<std::byte> out, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  size_t got = 0;
  while (got < out.size()) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) return Status::Aborted("tcp receive: timed out");
    pollfd p{fd_.get(), POLLIN, 0};
    const int ready = ::poll(&p, 1, static_cast<int>(std::min<int64_t>(left.count(), 1 << 30)));
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) return Status::Aborted(StrCat("tcp poll: ", std::strerror(errno)));
    if (ready == 0) continue;  // the loop re-checks the deadline
    const ssize_t n = ::recv(fd_.get(), out.data() + got, out.size() - got, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n == 0) return Status::Aborted("tcp receive: the peer closed the connection");
    if (n < 0) return Status::Aborted(StrCat("tcp receive: ", std::strerror(errno)));
    got += static_cast<size_t>(n);
  }
  return Status::Ok();
}

void TcpConnection::Close() { (void)::shutdown(fd_.get(), SHUT_RDWR); }

Result<std::unique_ptr<TcpListener>> TcpListener::Listen(const Endpoint& at) {
  AddrInfo addrs;
  DDS_TRY(Resolve(at, /*passive=*/true, addrs));
  Status last = Status::IoError(StrCat("listen ", Describe(at), ": no addresses"));
  for (addrinfo* a = addrs.list; a != nullptr; a = a->ai_next) {
    UniqueFd fd(::socket(a->ai_family, a->ai_socktype, a->ai_protocol));
    if (!fd.valid()) continue;
    const int one = 1;
    (void)::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(fd.get(), a->ai_addr, a->ai_addrlen) != 0 || ::listen(fd.get(), 16) != 0) {
      last = errno == EADDRINUSE
                 ? Status::AlreadyExists(StrCat("listen ", Describe(at), ": in use"))
                 : SocketError("listen", Describe(at));
      continue;
    }
    const uint16_t port = BoundPort(fd.get());
    return std::make_unique<TcpListener>(std::move(fd), port);
  }
  return last;
}

Result<std::unique_ptr<TcpConnection>> TcpListener::Accept(std::chrono::milliseconds timeout) {
  pollfd p{fd_.get(), POLLIN, 0};
  const int ready = ::poll(&p, 1, static_cast<int>(timeout.count()));
  if (ready < 0 && errno != EINTR) return SocketError("poll", "listener");
  if (ready <= 0) return std::unique_ptr<TcpConnection>();
  UniqueFd fd(::accept(fd_.get(), nullptr, nullptr));
  if (!fd.valid()) {
    if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN)
      return std::unique_ptr<TcpConnection>();
    return SocketError("accept", "listener");
  }
  Configure(fd.get());
  return std::make_unique<TcpConnection>(std::move(fd));
}

}  // namespace dds
