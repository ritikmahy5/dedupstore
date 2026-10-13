// dds ls <repo> <snapshot> [<path-in-snapshot>] [--json]

#include <cstdio>

#include "dds/cli/command.h"
#include "dds/cli/format.h"
#include "dds/cli/json.h"
#include "dds/cli/repo_access.h"
#include "dds/common/encoding.h"
#include "dds/common/strings.h"

namespace dds::cli {

namespace {

constexpr char kUsage[] = "dds ls <repo> <snapshot> [<path-in-snapshot>] [--json]";

void PrintJson(const SnapshotId& id, const std::string& path,
               const std::vector<TreeEntry>& entries) {
  JsonWriter w;
  w.BeginObject();
  w.Key("id");
  w.String(SnapshotIdToHex(id));
  w.Key("path");
  w.String(path);
  w.Key("entries");
  w.BeginArray();
  for (const TreeEntry& e : entries) {
    w.BeginObject();
    w.Key("name");
    w.String(e.name);
    if (!IsValidUtf8(e.name)) {
      // The JSON string cannot carry these bytes (see JsonWriter::String); the hex form can.
      w.Key("name_hex");
      w.String(HexEncode(AsBytes(e.name)));
    }
    w.Key("kind");
    w.String(EntryKindName(e.kind));
    w.Key("mode");
    w.Uint(e.mode);
    w.Key("mtime_sec");
    w.Int(e.mtime_sec);
    w.Key("mtime_nsec");
    w.Uint(e.mtime_nsec);
    if (e.kind == EntryKind::kFile) {
      w.Key("size");
      w.Uint(e.size);
    }
    if (e.kind == EntryKind::kSymlink) {
      w.Key("target");
      w.String(e.target);
    }
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
  std::printf("%s\n", w.Take().c_str());
}

int Run(const ParsedArgs& args) {
  auto repo = OpenRepository(args.positional[0], "dds ls");
  if (!repo.ok()) return ReportError(repo.status());
  auto id = (*repo)->ResolveSnapshot(args.positional[1]);
  if (!id.ok()) return ReportError(id.status());
  const std::string path = args.positional.size() > 2 ? args.positional[2] : "";
  auto entries = (*repo)->ListPath(*id, path);
  CloseRepository(**repo);
  if (!entries.ok()) return ReportError(entries.status());
  if (args.Has("json")) {
    PrintJson(*id, path, *entries);
    return kExitOk;
  }
  for (const TreeEntry& e : *entries) {
    const std::string size = e.kind == EntryKind::kFile ? std::to_string(e.size) : "-";
    std::fprintf(stderr, "%s  %12s  %s  %s%s\n", FormatMode(e.kind, e.mode).c_str(), size.c_str(),
                 FormatUnixSeconds(e.mtime_sec).c_str(), e.name.c_str(),
                 e.kind == EntryKind::kSymlink ? StrCat(" -> ", e.target).c_str() : "");
  }
  return kExitOk;
}

}  // namespace

Command MakeLsCommand() {
  Command cmd;
  cmd.name = "ls";
  cmd.usage = kUsage;
  cmd.summary = "list a directory inside a snapshot";
  cmd.flags = {{"json", false}};
  cmd.min_positional = 2;
  cmd.max_positional = 3;
  cmd.run = Run;
  return cmd;
}

}  // namespace dds::cli
