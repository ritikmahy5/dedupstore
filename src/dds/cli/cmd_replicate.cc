// dds replicate <repo> <snapshot> --to HOST:PORT [--window W] [--no-prune] [--json]

#include <cstdio>

#include "dds/cli/command.h"
#include "dds/cli/format.h"
#include "dds/cli/json.h"
#include "dds/cli/numbers.h"
#include "dds/cli/repo_access.h"
#include "dds/common/clock.h"
#include "dds/common/strings.h"
#include "dds/repl/sender.h"
#include "dds/repl/tcp.h"

namespace dds::cli {

namespace {

constexpr char kUsage[] =
    "dds replicate <repo> <snapshot> --to HOST:PORT [--window W] [--no-prune] [--json]";

void PrintJson(const SendReport& r, double seconds) {
  uint64_t wire = 0;
  for (const auto& [type, bytes] : r.wire_bytes_sent) wire += bytes;
  JsonWriter w;
  w.BeginObject();
  w.Key("id");
  w.String(SnapshotIdToHex(r.id));
  w.Key("fingerprints_queried");
  w.Uint(r.fingerprints_queried);
  w.Key("blobs_sent");
  w.Uint(r.blobs_sent);
  w.Key("payload_bytes_sent");
  w.Uint(r.payload_bytes_sent);
  w.Key("have_batches");
  w.Uint(r.have_batches);
  w.Key("put_batches");
  w.Uint(r.put_batches);
  w.Key("trees_checked");
  w.Uint(r.trees_checked);
  w.Key("subtrees_pruned");
  w.Uint(r.subtrees_pruned);
  w.Key("wire_bytes_sent");
  w.Uint(wire);
  w.Key("wire_bytes_sent_by_type");
  w.BeginObject();
  for (const auto& [type, bytes] : r.wire_bytes_sent) {
    w.Key(MessageTypeName(type));
    w.Uint(bytes);
  }
  w.EndObject();
  w.Key("wire_bytes_received");
  w.Uint(r.wire_bytes_received);
  w.Key("seconds");
  w.Double(seconds);
  w.EndObject();
  std::printf("%s\n", w.Take().c_str());
}

int Run(const ParsedArgs& args) {
  auto to = args.Get("to");
  if (!to) return UsageError(kUsage, "--to HOST:PORT is required");
  auto endpoint = ParseEndpoint(*to);
  if (!endpoint.ok()) return UsageError(kUsage, endpoint.status().message());
  SenderOptions options;
  if (auto text = args.Get("window")) {
    auto w = ParseCount(*text);
    if (!w || *w > 1024)
      return UsageError(kUsage, StrCat("--window must be 1 to 1024, got '", *text, "'"));
    options.window = static_cast<size_t>(*w);
  }
  options.prune_subtrees = !args.Has("no-prune");
  auto repo = OpenRepository(args.positional[0], "dds replicate");
  if (!repo.ok()) return ReportError(repo.status());
  auto id = (*repo)->ResolveSnapshot(args.positional[1]);
  if (!id.ok()) return ReportError(id.status());
  auto connection = TcpConnection::Connect(*endpoint);
  if (!connection.ok()) return ReportError(connection.status());
  const Clock& clock = SystemClock::Instance();
  const int64_t start = clock.MonotonicNanos();
  auto report = Replicate(**repo, *id, **connection, options);
  const double seconds = static_cast<double>(clock.MonotonicNanos() - start) / 1e9;
  CloseRepository(**repo);
  if (!report.ok()) return ReportError(report.status());
  if (args.Has("json")) {
    PrintJson(*report, seconds);
  } else {
    std::fprintf(stderr, "replicated %s to %s: %llu of %llu blobs sent (%s) in %.2f s\n",
                 SnapshotIdToHex(*id).c_str(), to->c_str(),
                 static_cast<unsigned long long>(report->blobs_sent),
                 static_cast<unsigned long long>(report->fingerprints_queried),
                 FormatBytes(report->payload_bytes_sent).c_str(), seconds);
  }
  return kExitOk;
}

}  // namespace

Command MakeReplicateCommand() {
  Command cmd;
  cmd.name = "replicate";
  cmd.usage = kUsage;
  cmd.summary = "send a snapshot to a serving repository";
  cmd.flags = {{"to", true}, {"window", true}, {"no-prune", false}, {"json", false}};
  cmd.min_positional = 2;
  cmd.max_positional = 2;
  cmd.run = Run;
  return cmd;
}

}  // namespace dds::cli
