// dds_bench interference (E6): ingest with and without a concurrent GC loop, and 4 concurrent
// sessions over overlapping versions (duplicate copies, D32).

#include <atomic>
#include <cstdio>
#include <thread>

#include "dds/gc/gc_runner.h"
#include "dds/snapshot/source_tree.h"
#include "driver/driver.h"
#include "driver/report.h"

namespace dds::bench {

namespace {

// Backs up dirs[first, last) in order. Returns the logical bytes, or -1 on error.
double BackupRange(Repository& repo, const std::vector<std::string>& dirs, size_t first,
                   size_t last, size_t threads) {
  double logical = 0;
  for (size_t v = first; v < last; ++v) {
    PosixSourceTree source(dirs[v]);
    BackupOptions options;
    options.threads = threads;
    auto b = repo.Backup(source, options);
    if (!b.ok()) {
      std::fprintf(stderr, "dds_bench: %s\n", b.status().ToString().c_str());
      return -1;
    }
    logical += static_cast<double>(b->manifest.logical_bytes);
  }
  return logical;
}

}  // namespace

int RunInterference(const std::vector<std::string>& words) {
  auto a = ParseFlags(words, {{"versions", true},
                              {"mode", true},
                              {"storage", true},
                              {"repo", true},
                              {"threads", true},
                              {"dataset", true},
                              {"sessions", true}});
  if (!a || !a->Get("out") || !a->Get("versions") || !a->Get("mode")) {
    std::fprintf(stderr,
                 "usage: dds_bench interference --experiment E --out FILE --versions DIR,... "
                 "--mode sequential|with-gc|concurrent [--storage mem|disk] [--threads N]\n");
    return 1;
  }
  const auto dirs = SplitComma(*a->Get("versions"));
  const std::string mode = *a->Get("mode");
  const std::string storage = a->Get("storage").value_or("mem");
  const size_t threads = static_cast<size_t>(FlagU64(*a, "threads", 4));
  const size_t sessions = static_cast<size_t>(FlagU64(*a, "sessions", 4));
  const uint64_t runs = FlagU64(*a, "runs", 5), warmup = FlagU64(*a, "warmup", 1);
  Report report(*a->Get("experiment"));
  report.Param("mode", mode);
  report.Param("storage", storage);
  report.Param("threads", static_cast<double>(threads));
  report.Dataset("name", a->Get("dataset").value_or("synth"));
  report.Dataset("versions", static_cast<double>(dirs.size()));
  for (uint64_t run = 0; run < warmup + runs; ++run) {
    auto r = FreshRepo(storage, a->Get("repo").value_or("/tmp/dds-bench-repo"), RepoParams{});
    if (!r.ok()) return 1;
    Repository& repo = *r->repo;
    std::map<std::string, double> c;
    const auto start = std::chrono::steady_clock::now();
    if (mode == "concurrent") {
      // Session s backs up versions s .. s + n - sessions, all at once: overlapping new data.
      std::vector<std::thread> workers;
      std::atomic<bool> failed{false};
      const size_t span = dirs.size() - sessions + 1;
      for (size_t s = 0; s < sessions; ++s) {
        workers.emplace_back([&, s] {
          if (BackupRange(repo, dirs, s, s + span, threads) < 0) failed = true;
        });
      }
      for (auto& w : workers) w.join();
      if (failed) return 1;
      const ContainerSet::Totals t = repo.Internals().containers.PublishedTotals();
      const IndexTotals idx = CountIndex(repo);
      const double canonical = static_cast<double>(idx.data + idx.tree + idx.recipe);
      c["physical_bytes"] = static_cast<double>(t.file_bytes);
      c["duplicate_copies"] = static_cast<double>(t.entries - repo.Internals().index.Size());
      c["duplicate_bytes"] = static_cast<double>(t.payload_bytes) - canonical;
      c["duplicate_share"] =
          (static_cast<double>(t.payload_bytes) - canonical) / static_cast<double>(t.file_bytes);
    } else {
      std::atomic<bool> done{false};
      std::atomic<uint64_t> gc_runs{0};
      // A GC that stopped early would make the ingest look unhindered, so a failure fails the run.
      std::atomic<bool> gc_failed{false};
      // How much work the GC did, not only how often it ran: runs before the first snapshot exists
      // mark nothing and take microseconds. Written by the GC thread, read after it is joined.
      uint64_t gc_blobs_marked = 0, gc_runs_marking = 0;
      double gc_busy_s = 0;
      std::thread gc_thread;
      if (mode == "with-gc") {
        gc_thread = std::thread([&] {
          GcRunner gc(repo);
          GcOptions options;
          while (!done) {
            const auto gc_start = std::chrono::steady_clock::now();
            auto g = gc.Run(options);
            if (!g.ok()) {
              std::fprintf(stderr, "dds_bench: gc: %s\n", g.status().ToString().c_str());
              gc_failed = true;
              return;
            }
            gc_busy_s += SecondsSince(gc_start);
            gc_blobs_marked += g->mark.blobs;
            if (g->mark.snapshots > 0) ++gc_runs_marking;
            ++gc_runs;
          }
        });
      }
      const double logical = BackupRange(repo, dirs, 0, dirs.size(), threads);
      const double seconds = SecondsSince(start);
      done = true;
      if (gc_thread.joinable()) gc_thread.join();
      if (logical < 0 || gc_failed) return 1;
      c["logical_bytes"] = logical;
      c["ingest_mb_per_s"] = logical / 1e6 / seconds;
      c["gc_runs"] = static_cast<double>(gc_runs.load());
      c["gc_runs_per_s"] = static_cast<double>(gc_runs.load()) / seconds;
      c["gc_runs_marking"] = static_cast<double>(gc_runs_marking);
      c["gc_blobs_marked_per_s"] = static_cast<double>(gc_blobs_marked) / seconds;
      c["gc_busy_share"] = gc_busy_s / seconds;
    }
    const double wall = SecondsSince(start);
    c["seconds"] = wall;
    if (run >= warmup) report.AddSample(wall, std::move(c));
  }
  return report.Write(*a->Get("out")) ? 0 : 1;
}

}  // namespace dds::bench
