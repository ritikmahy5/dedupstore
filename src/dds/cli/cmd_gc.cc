// dds gc <repo> [--threshold F] [--dry-run] [--json]

#include <cstdio>

#include "dds/cli/command.h"
#include "dds/cli/format.h"
#include "dds/cli/json.h"
#include "dds/cli/numbers.h"
#include "dds/cli/repo_access.h"
#include "dds/common/strings.h"
#include "dds/gc/gc_runner.h"

namespace dds::cli {

namespace {

constexpr char kUsage[] = "dds gc <repo> [--threshold F] [--dry-run] [--json]";

void PrintJson(const GcReport& r) {
  JsonWriter w;
  w.BeginObject();
  w.Key("run");
  w.Uint(r.run);
  w.Key("dry_run");
  w.Bool(r.dry_run);
  w.Key("snapshots");
  w.Uint(r.mark.snapshots);
  w.Key("blobs_marked");
  w.Uint(r.mark.blobs);
  w.Key("candidates");
  w.Uint(r.candidates);
  w.Key("candidate_payload_bytes");
  w.Uint(r.candidate_payload_bytes);
  w.Key("live_bytes");
  w.Uint(r.live_bytes);
  w.Key("planned_delete");
  w.Uint(r.planned_delete);
  w.Key("planned_compact");
  w.Uint(r.planned_compact);
  w.Key("planned_keep");
  w.Uint(r.planned_keep);
  w.Key("containers_deleted");
  w.Uint(r.compact.containers_deleted);
  w.Key("containers_compacted");
  w.Uint(r.compact.containers_compacted);
  w.Key("blobs_removed");
  w.Uint(r.compact.blobs_removed);
  w.Key("blobs_copied");
  w.Uint(r.compact.blobs_copied);
  w.Key("bytes_copied");
  w.Uint(r.compact.bytes_copied);
  w.Key("duplicates_dropped");
  w.Uint(r.compact.duplicates_dropped);
  w.Key("containers_reclaimed");
  w.Uint(r.containers_reclaimed);
  w.Key("bytes_reclaimed");
  w.Uint(r.bytes_reclaimed);
  w.Key("mark_seconds");
  w.Double(r.mark_seconds);
  w.Key("plan_seconds");
  w.Double(r.plan_seconds);
  w.Key("compact_seconds");
  w.Double(r.compact_seconds);
  w.EndObject();
  std::printf("%s\n", w.Take().c_str());
}

void PrintHuman(const GcReport& r) {
  std::fprintf(stderr, "gc run %llu%s: %llu snapshots, %llu live blobs marked\n",
               static_cast<unsigned long long>(r.run), r.dry_run ? " (dry run)" : "",
               static_cast<unsigned long long>(r.mark.snapshots),
               static_cast<unsigned long long>(r.mark.blobs));
  std::fprintf(stderr,
               "  %llu containers: %llu to delete, %llu to compact, %llu to keep (%s of %s live)\n",
               static_cast<unsigned long long>(r.candidates),
               static_cast<unsigned long long>(r.planned_delete),
               static_cast<unsigned long long>(r.planned_compact),
               static_cast<unsigned long long>(r.planned_keep), FormatBytes(r.live_bytes).c_str(),
               FormatBytes(r.candidate_payload_bytes).c_str());
  if (r.dry_run) return;
  std::fprintf(stderr, "  copied %llu blobs (%s), removed %llu dead blobs, reclaimed %s\n",
               static_cast<unsigned long long>(r.compact.blobs_copied),
               FormatBytes(r.compact.bytes_copied).c_str(),
               static_cast<unsigned long long>(r.compact.blobs_removed),
               FormatBytes(r.bytes_reclaimed).c_str());
}

int Run(const ParsedArgs& args) {
  GcOptions options;
  if (auto text = args.Get("threshold")) {
    auto threshold = ParseThreshold(*text);
    if (!threshold)
      return UsageError(kUsage, StrCat("--threshold must be in (0, 1], got '", *text, "'"));
    options.threshold = *threshold;
  }
  options.dry_run = args.Has("dry-run");
  auto repo = OpenRepository(args.positional[0], "dds gc");
  if (!repo.ok()) return ReportError(repo.status());
  GcRunner gc(**repo);
  auto report = gc.Run(options);
  CloseRepository(**repo);
  if (!report.ok()) return ReportError(report.status());
  if (args.Has("json")) {
    PrintJson(*report);
  } else {
    PrintHuman(*report);
  }
  return kExitOk;
}

}  // namespace

Command MakeGcCommand() {
  Command cmd;
  cmd.name = "gc";
  cmd.usage = kUsage;
  cmd.summary = "reclaim space from removed snapshots and duplicate copies";
  cmd.flags = {{"threshold", true}, {"dry-run", false}, {"json", false}};
  cmd.min_positional = 1;
  cmd.max_positional = 1;
  cmd.run = Run;
  return cmd;
}

}  // namespace dds::cli
