// dds restore <repo> <snapshot> <dest> [--no-verify] [--json]

#include <cstdio>

#include "dds/cli/command.h"
#include "dds/cli/format.h"
#include "dds/cli/json.h"
#include "dds/cli/repo_access.h"
#include "dds/common/clock.h"
#include "dds/snapshot/restore_sink.h"

namespace dds::cli {

namespace {

constexpr char kUsage[] = "dds restore <repo> <snapshot> <dest> [--no-verify] [--json]";

void PrintJson(const SnapshotId& id, const std::string& dest, const RestoreResult& r, bool verified,
               double seconds) {
  JsonWriter w;
  w.BeginObject();
  w.Key("id");
  w.String(SnapshotIdToHex(id));
  w.Key("dest");
  w.String(dest);
  w.Key("files");
  w.Uint(r.files);
  w.Key("directories");
  w.Uint(r.directories);
  w.Key("symlinks");
  w.Uint(r.symlinks);
  w.Key("bytes");
  w.Uint(r.bytes);
  w.Key("verified");
  w.Bool(verified);
  w.Key("seconds");
  w.Double(seconds);
  w.EndObject();
  std::printf("%s\n", w.Take().c_str());
}

int Run(const ParsedArgs& args) {
  auto repo = OpenRepository(args.positional[0], "dds restore");
  if (!repo.ok()) return ReportError(repo.status());
  auto id = (*repo)->ResolveSnapshot(args.positional[1]);
  if (!id.ok()) return ReportError(id.status());
  const std::string& dest = args.positional[2];
  RestoreOptions options;
  options.verify = !args.Has("no-verify");
  PosixRestoreSink sink(dest);
  const Clock& clock = SystemClock::Instance();
  const int64_t start = clock.MonotonicNanos();
  auto result = (*repo)->Restore(*id, sink, options);
  const double seconds = static_cast<double>(clock.MonotonicNanos() - start) / 1e9;
  CloseRepository(**repo);
  if (!result.ok()) return ReportError(result.status());
  if (args.Has("json")) {
    PrintJson(*id, dest, *result, options.verify, seconds);
  } else {
    std::fprintf(stderr, "restored %llu files, %llu directories, %llu symlinks (%s) into %s%s\n",
                 static_cast<unsigned long long>(result->files),
                 static_cast<unsigned long long>(result->directories),
                 static_cast<unsigned long long>(result->symlinks),
                 FormatBytes(result->bytes).c_str(), dest.c_str(),
                 options.verify ? "" : " without verifying");
  }
  return kExitOk;
}

}  // namespace

Command MakeRestoreCommand() {
  Command cmd;
  cmd.name = "restore";
  cmd.usage = kUsage;
  cmd.summary = "restore a snapshot into a new or empty directory";
  cmd.flags = {{"no-verify", false}, {"json", false}};
  cmd.min_positional = 3;
  cmd.max_positional = 3;
  cmd.run = Run;
  return cmd;
}

}  // namespace dds::cli
