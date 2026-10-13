#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

#include "dds/common/status.h"
#include "dds/repl/receiver.h"
#include "dds/repl/tcp.h"

namespace dds {

class Repository;

struct ServerOptions {
  size_t max_connections = 16;  // PROTOCOL.md 6; one more is refused with ERROR(Busy)
  std::optional<std::chrono::seconds> gc_interval;  // `dds serve --gc-interval`
  double gc_threshold = 0.5;
  ReceiverOptions receiver;
};

// `dds serve` (SPEC.md section 3): accepts replication connections and serves each on its own
// thread as one ingest session (receiver.h), and optionally runs GC every gc_interval in the same
// process, concurrently with the sessions (ARCHITECTURE.md 6 makes that safe). Runs until `stop`
// returns true; it is checked at least every 200 ms. On stop it closes the listener and every
// open connection (each session seals what it received) and joins all threads.
class Server {
 public:
  Server(Repository& repo, std::unique_ptr<TcpListener> listener, ServerOptions options);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  Status Run(const std::function<bool()>& stop);

 private:
  struct Session;
  void GcLoop(const std::function<bool()>& stop);
  void Reap(bool all);

  Repository& repo_;
  std::unique_ptr<TcpListener> listener_;
  const ServerOptions options_;
  std::vector<std::unique_ptr<Session>> sessions_;  // only Run's thread touches this list
};

}  // namespace dds
