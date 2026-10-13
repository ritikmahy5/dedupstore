// dds rm <repo> <snapshot>... [--json]

#include <cstdio>

#include "dds/cli/command.h"
#include "dds/cli/json.h"
#include "dds/cli/repo_access.h"

namespace dds::cli {

namespace {

constexpr char kUsage[] = "dds rm <repo> <snapshot>... [--json]";

int Run(const ParsedArgs& args) {
  auto repo = OpenRepository(args.positional[0], "dds rm");
  if (!repo.ok()) return ReportError(repo.status());
  // Resolve every argument before removing anything, so a typo in the last one removes nothing.
  std::vector<SnapshotId> ids;
  for (size_t i = 1; i < args.positional.size(); ++i) {
    auto id = (*repo)->ResolveSnapshot(args.positional[i]);
    if (!id.ok()) return ReportError(id.status());
    ids.push_back(*id);
  }
  std::vector<SnapshotId> removed;
  Status status;
  for (const SnapshotId& id : ids) {
    status = (*repo)->RemoveSnapshot(id);
    if (!status.ok()) break;
    removed.push_back(id);
    if (!args.Has("json"))
      std::fprintf(stderr, "removed snapshot %s\n", SnapshotIdToHex(id).c_str());
  }
  CloseRepository(**repo);
  if (args.Has("json")) {
    JsonWriter w;
    w.BeginObject();
    w.Key("removed");
    w.BeginArray();
    for (const SnapshotId& id : removed) w.String(SnapshotIdToHex(id));
    w.EndArray();
    w.EndObject();
    std::printf("%s\n", w.Take().c_str());
  }
  if (!status.ok()) return ReportError(status);
  return kExitOk;
}

}  // namespace

Command MakeRmCommand() {
  Command cmd;
  cmd.name = "rm";
  cmd.usage = kUsage;
  cmd.summary = "remove snapshots (the next gc reclaims their space)";
  cmd.flags = {{"json", false}};
  cmd.min_positional = 2;
  cmd.run = Run;
  return cmd;
}

}  // namespace dds::cli
