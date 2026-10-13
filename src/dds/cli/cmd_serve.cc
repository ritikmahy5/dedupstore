// dds serve <repo> --listen HOST:PORT [--gc-interval DURATION]

#include <atomic>
#include <csignal>
#include <cstdio>

#include "dds/cli/command.h"
#include "dds/cli/numbers.h"
#include "dds/cli/repo_access.h"
#include "dds/common/strings.h"
#include "dds/repl/server.h"

namespace dds::cli {

namespace {

constexpr char kUsage[] = "dds serve <repo> --listen HOST:PORT [--gc-interval DURATION]";

// Set by SIGINT or SIGTERM. A lock-free atomic is safe to store from a signal handler.
std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free);

extern "C" void OnStopSignal(int /*signal*/) { g_stop.store(true); }

int Run(const ParsedArgs& args) {
  auto listen_text = args.Get("listen");
  if (!listen_text) return UsageError(kUsage, "--listen HOST:PORT is required");
  auto endpoint = ParseEndpoint(*listen_text);
  if (!endpoint.ok()) return UsageError(kUsage, endpoint.status().message());
  ServerOptions options;
  if (auto text = args.Get("gc-interval")) {
    auto interval = ParseDuration(*text);
    if (!interval) {
      return UsageError(kUsage,
                        StrCat("--gc-interval must be like 30s, 10m, or 1h; got '", *text, "'"));
    }
    options.gc_interval = *interval;
  }
  auto repo = OpenRepository(args.positional[0], "dds serve");
  if (!repo.ok()) return ReportError(repo.status());
  auto listener = TcpListener::Listen(*endpoint);
  if (!listener.ok()) return ReportError(listener.status());
  std::fprintf(
      stderr, "serving %s on %s:%u%s; stop with Ctrl-C\n", args.positional[0].c_str(),
      endpoint->host.c_str(), static_cast<unsigned>((*listener)->port()),
      options.gc_interval ? StrCat(", gc every ", options.gc_interval->count(), " s").c_str() : "");
  std::signal(SIGINT, OnStopSignal);
  std::signal(SIGTERM, OnStopSignal);
  Server server(**repo, std::move(listener).value(), options);
  const Status served = server.Run([] { return g_stop.load(); });
  CloseRepository(**repo);
  if (!served.ok()) return ReportError(served);
  std::fprintf(stderr, "stopped\n");
  return kExitOk;
}

}  // namespace

Command MakeServeCommand() {
  Command cmd;
  cmd.name = "serve";
  cmd.usage = kUsage;
  cmd.summary = "accept replication connections (optionally running gc periodically)";
  cmd.flags = {{"listen", true}, {"gc-interval", true}};
  cmd.min_positional = 1;
  cmd.max_positional = 1;
  cmd.run = Run;
  return cmd;
}

}  // namespace dds::cli
