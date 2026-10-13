// dds verify <repo> [--deep] [--json]

#include <cstdio>

#include "dds/cli/command.h"
#include "dds/cli/format.h"
#include "dds/cli/json.h"
#include "dds/cli/repo_access.h"
#include "dds/common/strings.h"

namespace dds::cli {

namespace {

constexpr char kUsage[] = "dds verify <repo> [--deep] [--json]";

void PrintJson(const VerifyReport& r, const OpenReport& open, bool deep) {
  JsonWriter w;
  w.BeginObject();
  w.Key("ok");
  w.Bool(r.ok());
  w.Key("deep");
  w.Bool(deep);
  w.Key("errors");
  w.BeginArray();
  for (const std::string& e : r.errors) w.String(e);
  w.EndArray();
  w.Key("warnings");
  w.BeginArray();
  for (const std::string& e : r.warnings) w.String(e);
  for (const std::string& e : open.ignored_files) w.String(e);
  w.EndArray();
  w.Key("snapshots");
  w.Uint(r.snapshots);
  w.Key("containers");
  w.Uint(r.containers);
  w.Key("reachable_blobs");
  w.Uint(r.reachable_blobs);
  w.Key("unreachable_blobs");
  w.Uint(r.unreachable_blobs);
  w.Key("unreachable_bytes");
  w.Uint(r.unreachable_bytes);
  w.Key("duplicate_copies");
  w.Uint(r.duplicate_copies);
  w.Key("bytes_hashed");
  w.Uint(r.bytes_hashed);
  w.EndObject();
  std::printf("%s\n", w.Take().c_str());
}

void PrintHuman(const VerifyReport& r, const OpenReport& open) {
  for (const std::string& e : r.errors) std::fprintf(stderr, "error: %s\n", e.c_str());
  for (const std::string& e : r.warnings) std::fprintf(stderr, "warning: %s\n", e.c_str());
  for (const std::string& e : open.ignored_files) std::fprintf(stderr, "warning: %s\n", e.c_str());
  std::fprintf(stderr,
               "%s: %llu snapshots, %llu containers, %llu reachable blobs; "
               "%llu unreachable blobs (%s) and %llu duplicate copies await gc\n",
               r.ok() ? "ok" : "CORRUPT", static_cast<unsigned long long>(r.snapshots),
               static_cast<unsigned long long>(r.containers),
               static_cast<unsigned long long>(r.reachable_blobs),
               static_cast<unsigned long long>(r.unreachable_blobs),
               FormatBytes(r.unreachable_bytes).c_str(),
               static_cast<unsigned long long>(r.duplicate_copies));
}

int Run(const ParsedArgs& args) {
  auto repo = OpenRepository(args.positional[0], "dds verify");
  if (!repo.ok()) return ReportError(repo.status());
  VerifyOptions options;
  options.deep = args.Has("deep");
  auto report = (*repo)->Verify(options);
  const OpenReport open = (*repo)->open_report();
  CloseRepository(**repo);
  if (!report.ok()) return ReportError(report.status());
  if (args.Has("json")) {
    PrintJson(*report, open, options.deep);
  } else {
    PrintHuman(*report, open);
  }
  // Found damage is Corruption (exit 3), the same code any command uses when it meets damage.
  return report->ok() ? kExitOk : ExitCodeFor(Status::Corruption(""));
}

}  // namespace

Command MakeVerifyCommand() {
  Command cmd;
  cmd.name = "verify";
  cmd.usage = kUsage;
  cmd.summary = "check repository consistency (--deep also rehashes every reachable blob)";
  cmd.flags = {{"deep", false}, {"json", false}};
  cmd.min_positional = 1;
  cmd.max_positional = 1;
  cmd.run = Run;
  return cmd;
}

}  // namespace dds::cli
