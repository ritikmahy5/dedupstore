// dds_bench gc (E5): copy a prepared disk repository, then run one GC on the copy, per sample.

#include <cstdio>
#include <filesystem>

#include "dds/gc/gc_runner.h"
#include "driver/driver.h"
#include "driver/report.h"

namespace dds::bench {

int RunGc(const std::vector<std::string>& words) {
  auto a =
      ParseFlags(words, {{"repo", true}, {"work", true}, {"threshold", true}, {"label", true}});
  if (!a || !a->Get("out") || !a->Get("repo") || !a->Get("work")) {
    std::fprintf(stderr,
                 "usage: dds_bench gc --experiment E --out FILE --repo DIR --work DIR "
                 "--threshold F [--label L]\n");
    return 1;
  }
  const double threshold = FlagDouble(*a, "threshold", 0.5);
  const uint64_t runs = FlagU64(*a, "runs", 5), warmup = FlagU64(*a, "warmup", 1);
  Report report(*a->Get("experiment"));
  report.Param("threshold", threshold);
  report.Param("storage", "disk");
  report.Dataset("name", a->Get("label").value_or("unnamed"));
  for (uint64_t run = 0; run < warmup + runs; ++run) {
    // Every sample starts from the same repository (BENCHMARKS.md E5): copy it first, untimed.
    std::error_code ec;
    std::filesystem::remove_all(*a->Get("work"), ec);
    std::filesystem::copy(*a->Get("repo"), *a->Get("work"),
                          std::filesystem::copy_options::recursive, ec);
    if (ec) {
      std::fprintf(stderr, "dds_bench: copy: %s\n", ec.message().c_str());
      return 1;
    }
    auto r = OpenDiskRepo(*a->Get("work"));
    if (!r.ok()) {
      std::fprintf(stderr, "dds_bench: %s\n", r.status().ToString().c_str());
      return 1;
    }
    const auto physical_before = r->repo->Internals().containers.PublishedTotals().file_bytes;
    GcRunner gc(*r->repo);
    GcOptions options;
    options.threshold = threshold;
    const auto start = std::chrono::steady_clock::now();
    auto g = gc.Run(options);
    const double seconds = SecondsSince(start);
    if (!g.ok()) {
      std::fprintf(stderr, "dds_bench: %s\n", g.status().ToString().c_str());
      return 1;
    }
    const auto physical_after = r->repo->Internals().containers.PublishedTotals().file_bytes;
    std::map<std::string, double> c;
    c["physical_before"] = static_cast<double>(physical_before);
    c["physical_after"] = static_cast<double>(physical_after);
    c["bytes_reclaimed"] = static_cast<double>(g->bytes_reclaimed);
    c["bytes_copied"] = static_cast<double>(g->compact.bytes_copied);
    // BENCHMARKS.md E5: bytes copied / bytes reclaimed, where reclaimed is the space freed
    // (physical bytes before minus after). GcReport's bytes_reclaimed counts every unlinked
    // container file, including the live blobs just copied out of it, so dividing by it
    // understated write amplification (0.61 instead of 1.58 at threshold 0.9, job 10930720).
    const double freed = static_cast<double>(physical_before) - static_cast<double>(physical_after);
    c["bytes_freed"] = freed;
    c["write_amplification"] =
        freed <= 0 ? 0 : static_cast<double>(g->compact.bytes_copied) / freed;
    c["copied_per_unlinked"] = g->bytes_reclaimed == 0
                                   ? 0
                                   : static_cast<double>(g->compact.bytes_copied) /
                                         static_cast<double>(g->bytes_reclaimed);
    c["containers_deleted"] = static_cast<double>(g->compact.containers_deleted);
    c["containers_compacted"] = static_cast<double>(g->compact.containers_compacted);
    c["mark_s"] = g->mark_seconds;
    c["plan_s"] = g->plan_seconds;
    c["compact_s"] = g->compact_seconds;
    c["seconds"] = seconds;
    if (run >= warmup) report.AddSample(seconds, std::move(c));
    r->repo.reset();
    std::filesystem::remove_all(*a->Get("work"), ec);
  }
  return report.Write(*a->Get("out")) ? 0 : 1;
}

}  // namespace dds::bench
