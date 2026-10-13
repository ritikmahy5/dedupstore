// dds_bench restore (E4): restore one snapshot of an existing disk repository, per sample.

#include <cstdio>
#include <filesystem>
#include <set>

#include "dds/repo/dag_walker.h"
#include "dds/snapshot/restore_sink.h"
#include "driver/driver.h"
#include "driver/report.h"

namespace dds::bench {

int RunRestore(const std::vector<std::string>& words) {
  auto a = ParseFlags(
      words,
      {{"repo", true}, {"snapshot", true}, {"dest", true}, {"verify", true}, {"label", true}});
  if (!a || !a->Get("out") || !a->Get("repo") || !a->Get("snapshot") || !a->Get("dest")) {
    std::fprintf(stderr,
                 "usage: dds_bench restore --experiment E --out FILE --repo DIR "
                 "--snapshot ID --dest DIR [--verify 0|1] [--label L]\n");
    return 1;
  }
  const bool verify = FlagU64(*a, "verify", 1) != 0;
  const uint64_t runs = FlagU64(*a, "runs", 5), warmup = FlagU64(*a, "warmup", 1);
  Report report(*a->Get("experiment"));
  report.Param("verify", verify ? "yes" : "no");
  report.Param("storage", "disk");
  report.Dataset("name", a->Get("label").value_or("unnamed"));
  report.Dataset("repository", *a->Get("repo"));
  for (uint64_t run = 0; run < warmup + runs; ++run) {
    auto r = OpenDiskRepo(*a->Get("repo"));
    if (!r.ok()) {
      std::fprintf(stderr, "dds_bench: %s\n", r.status().ToString().c_str());
      return 1;
    }
    auto id = r->repo->ResolveSnapshot(*a->Get("snapshot"));
    if (!id.ok()) {
      std::fprintf(stderr, "dds_bench: %s\n", id.status().ToString().c_str());
      return 1;
    }
    std::error_code ec;
    std::filesystem::remove_all(*a->Get("dest"), ec);
    PosixRestoreSink sink(*a->Get("dest"));
    RestoreOptions options;
    options.verify = verify;
    const auto start = std::chrono::steady_clock::now();
    auto restored = r->repo->Restore(*id, sink, options);
    const double seconds = SecondsSince(start);
    if (!restored.ok()) {
      std::fprintf(stderr, "dds_bench: %s\n", restored.status().ToString().c_str());
      return 1;
    }
    // Containers touched: the distinct containers holding the snapshot's blobs (fragmentation).
    DagWalker walker(r->repo->Internals(), /*deep=*/false);
    std::vector<std::string> problems;
    walker.Walk(r->repo->Internals().snapshots.Read(*id)->root, problems);
    std::set<uint32_t> containers;
    for (const auto& [fp, loc] : walker.reachable()) containers.insert(loc.container_id);
    std::map<std::string, double> c;
    const double gb = static_cast<double>(restored->bytes) / 1e9;
    c["bytes"] = static_cast<double>(restored->bytes);
    c["files"] = static_cast<double>(restored->files);
    c["seconds"] = seconds;
    c["mb_per_s"] = static_cast<double>(restored->bytes) / 1e6 / seconds;
    c["containers_touched"] = static_cast<double>(containers.size());
    c["containers_per_gb"] = gb == 0 ? 0 : static_cast<double>(containers.size()) / gb;
    if (run >= warmup) report.AddSample(seconds, std::move(c));
  }
  std::error_code ec;
  std::filesystem::remove_all(*a->Get("dest"), ec);
  return report.Write(*a->Get("out")) ? 0 : 1;
}

}  // namespace dds::bench
