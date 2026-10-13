// dds stats <repo> [--json]

#include <cstdio>

#include "dds/cli/command.h"
#include "dds/cli/format.h"
#include "dds/cli/json.h"
#include "dds/cli/repo_access.h"

namespace dds::cli {

namespace {

constexpr char kUsage[] = "dds stats <repo> [--json]";

void PrintJson(const RepoStats& s) {
  JsonWriter w;
  w.BeginObject();
  w.Key("snapshots");
  w.Uint(s.snapshots);
  w.Key("logical_bytes");
  w.Uint(s.logical_bytes);
  w.Key("unique_data_bytes");
  w.Uint(s.unique_data_bytes);
  w.Key("stored_data_bytes");
  w.Uint(s.stored_data_bytes);
  w.Key("physical_bytes");
  w.Uint(s.physical_bytes);
  w.Key("dedup_ratio");
  w.Double(s.dedup_ratio);
  w.Key("compression_ratio");
  w.Double(s.compression_ratio);
  w.Key("containers");
  w.Uint(s.containers);
  w.Key("blobs");
  w.Uint(s.blobs);
  w.Key("duplicate_copies");
  w.Uint(s.duplicate_copies);
  w.Key("index_memory_bytes");
  w.Uint(s.index_memory_bytes);
  w.Key("live_fraction_histogram");
  w.BeginArray();
  for (uint64_t n : s.live_fraction_histogram) w.Uint(n);
  w.EndArray();
  w.EndObject();
  std::printf("%s\n", w.Take().c_str());
}

void PrintHuman(const RepoStats& s) {
  std::fprintf(stderr, "snapshots          %llu\n", static_cast<unsigned long long>(s.snapshots));
  std::fprintf(stderr, "logical bytes      %s\n", FormatBytes(s.logical_bytes).c_str());
  std::fprintf(stderr, "unique data bytes  %s\n", FormatBytes(s.unique_data_bytes).c_str());
  std::fprintf(stderr, "physical bytes     %s\n", FormatBytes(s.physical_bytes).c_str());
  std::fprintf(stderr, "dedup ratio        %.2f\n", s.dedup_ratio);
  std::fprintf(stderr, "compression ratio  %.2f (%s of data as stored)\n", s.compression_ratio,
               FormatBytes(s.stored_data_bytes).c_str());
  std::fprintf(stderr, "containers         %llu\n", static_cast<unsigned long long>(s.containers));
  std::fprintf(stderr, "blobs              %llu\n", static_cast<unsigned long long>(s.blobs));
  std::fprintf(stderr, "duplicate copies   %llu\n",
               static_cast<unsigned long long>(s.duplicate_copies));
  std::fprintf(stderr, "index memory       %s\n", FormatBytes(s.index_memory_bytes).c_str());
  std::fprintf(stderr, "containers by live fraction:\n");
  for (size_t i = 0; i < s.live_fraction_histogram.size(); ++i) {
    std::fprintf(stderr, "  %3zu%%-%3zu%%  %llu\n", i * 10, i == 9 ? size_t{100} : i * 10 + 9,
                 static_cast<unsigned long long>(s.live_fraction_histogram[i]));
  }
}

int Run(const ParsedArgs& args) {
  auto repo = OpenRepository(args.positional[0], "dds stats");
  if (!repo.ok()) return ReportError(repo.status());
  auto stats = (*repo)->Stats();
  CloseRepository(**repo);
  if (!stats.ok()) return ReportError(stats.status());
  if (args.Has("json")) {
    PrintJson(*stats);
  } else {
    PrintHuman(*stats);
  }
  return kExitOk;
}

}  // namespace

Command MakeStatsCommand() {
  Command cmd;
  cmd.name = "stats";
  cmd.usage = kUsage;
  cmd.summary = "space usage, dedup ratio, and container live fractions";
  cmd.flags = {{"json", false}};
  cmd.min_positional = 1;
  cmd.max_positional = 1;
  cmd.run = Run;
  return cmd;
}

}  // namespace dds::cli
