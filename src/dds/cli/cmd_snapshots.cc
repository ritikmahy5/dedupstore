// dds snapshots <repo> [--json]

#include <cstdio>

#include "dds/cli/command.h"
#include "dds/cli/format.h"
#include "dds/cli/json.h"
#include "dds/cli/repo_access.h"

namespace dds::cli {

namespace {

constexpr char kUsage[] = "dds snapshots <repo> [--json]";

void PrintJson(const std::vector<SnapshotInfo>& snapshots) {
  JsonWriter w;
  w.BeginObject();
  w.Key("snapshots");
  w.BeginArray();
  for (const SnapshotInfo& s : snapshots) {
    w.BeginObject();
    w.Key("id");
    w.String(SnapshotIdToHex(s.id));
    w.Key("created_at_ns");
    w.Int(s.manifest.created_at_ns);
    w.Key("host");
    w.String(s.manifest.host);
    w.Key("source_path");
    w.String(s.manifest.source_path);
    w.Key("tag");
    w.String(s.manifest.tag);
    w.Key("logical_bytes");
    w.Uint(s.manifest.logical_bytes);
    w.Key("files");
    w.Uint(s.manifest.file_count);
    w.Key("directories");
    w.Uint(s.manifest.directory_count);
    w.Key("symlinks");
    w.Uint(s.manifest.symlink_count);
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
  std::printf("%s\n", w.Take().c_str());
}

int Run(const ParsedArgs& args) {
  auto repo = OpenRepository(args.positional[0], "dds snapshots");
  if (!repo.ok()) return ReportError(repo.status());
  auto snapshots = (*repo)->ListSnapshots();
  CloseRepository(**repo);
  if (!snapshots.ok()) return ReportError(snapshots.status());
  if (args.Has("json")) {
    PrintJson(*snapshots);
    return kExitOk;
  }
  std::fprintf(stderr, "%-32s  %-20s  %10s  %10s  %-12s  %s\n", "ID", "TIME", "FILES", "LOGICAL",
               "TAG", "SOURCE");
  for (const SnapshotInfo& s : *snapshots) {
    std::fprintf(stderr, "%-32s  %-20s  %10llu  %10s  %-12s  %s\n", SnapshotIdToHex(s.id).c_str(),
                 FormatTime(s.manifest.created_at_ns).c_str(),
                 static_cast<unsigned long long>(s.manifest.file_count),
                 FormatBytes(s.manifest.logical_bytes).c_str(), s.manifest.tag.c_str(),
                 s.manifest.source_path.c_str());
  }
  return kExitOk;
}

}  // namespace

Command MakeSnapshotsCommand() {
  Command cmd;
  cmd.name = "snapshots";
  cmd.usage = kUsage;
  cmd.summary = "list snapshots";
  cmd.flags = {{"json", false}};
  cmd.min_positional = 1;
  cmd.max_positional = 1;
  cmd.run = Run;
  return cmd;
}

}  // namespace dds::cli
