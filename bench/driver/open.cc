// dds_bench open (E9, S1open): open an existing disk repository, per sample, with or without its
// index checkpoint. dds_bench checkpoint (S1open): write the index checkpoint, per sample.

#include <cstdio>

#include "driver/driver.h"
#include "driver/report.h"

namespace dds::bench {

namespace {

// Neither command may change the repository between samples, so no open writes a checkpoint.
OpenOptions BenchOpenOptions(bool use_checkpoint) {
  OpenOptions options;
  options.use_index_checkpoint = use_checkpoint;
  options.write_index_checkpoint = false;
  return options;
}

double CheckpointBytes(BenchRepo& r) {
  auto file = r.fs->OpenRead(r.repo->Internals().layout.index_checkpoint());
  if (!file.ok()) return 0;
  auto size = (*file)->Size();
  return size.ok() ? static_cast<double>(*size) : 0;
}

}  // namespace

int RunOpen(const std::vector<std::string>& words) {
  auto a = ParseFlags(words, {{"repo", true}, {"label", true}, {"checkpoint", true}});
  const std::string checkpoint = a ? a->Get("checkpoint").value_or("on") : "";
  if (!a || !a->Get("out") || !a->Get("repo") || (checkpoint != "on" && checkpoint != "off")) {
    std::fprintf(stderr,
                 "usage: dds_bench open --experiment E --out FILE --repo DIR [--label L] "
                 "[--checkpoint on|off]\n");
    return 1;
  }
  const uint64_t runs = FlagU64(*a, "runs", 5), warmup = FlagU64(*a, "warmup", 1);
  Report report(*a->Get("experiment"));
  report.Param("storage", "disk");
  report.Param("checkpoint", checkpoint);
  report.Dataset("name", a->Get("label").value_or("unnamed"));
  for (uint64_t run = 0; run < warmup + runs; ++run) {
    const auto start = std::chrono::steady_clock::now();
    auto r = OpenDiskRepo(*a->Get("repo"), BenchOpenOptions(checkpoint == "on"));
    const double seconds = SecondsSince(start);
    if (!r.ok()) {
      std::fprintf(stderr, "dds_bench: %s\n", r.status().ToString().c_str());
      return 1;
    }
    const IndexTotals t = CountIndex(*r->repo);
    RepoInternals& in = r->repo->Internals();
    const OpenReport& opened = r->repo->open_report();
    std::map<std::string, double> c;
    c["open_s"] = seconds;
    c["containers"] = static_cast<double>(in.containers.PublishedTotals().containers);
    c["checkpoint_containers"] = static_cast<double>(opened.checkpoint_containers);
    c["footers_read"] = static_cast<double>(opened.footers_read);
    c["checkpoint_bytes"] = CheckpointBytes(*r);
    c["blobs"] = static_cast<double>(t.blobs);
    c["unique_data_bytes"] = static_cast<double>(t.data);
    c["index_bytes"] = static_cast<double>(in.index.MemoryBytes());
    c["index_bytes_per_blob"] =
        t.blobs == 0 ? 0
                     : static_cast<double>(in.index.MemoryBytes()) / static_cast<double>(t.blobs);
    if (run >= warmup) report.AddSample(seconds, std::move(c));
  }
  return report.Write(*a->Get("out")) ? 0 : 1;
}

int RunCheckpoint(const std::vector<std::string>& words) {
  auto a = ParseFlags(words, {{"repo", true}, {"label", true}});
  if (!a || !a->Get("out") || !a->Get("repo")) {
    std::fprintf(stderr,
                 "usage: dds_bench checkpoint --experiment E --out FILE --repo DIR [--label L]\n");
    return 1;
  }
  const uint64_t runs = FlagU64(*a, "runs", 5), warmup = FlagU64(*a, "warmup", 1);
  auto r = OpenDiskRepo(*a->Get("repo"), BenchOpenOptions(true));
  if (!r.ok()) {
    std::fprintf(stderr, "dds_bench: %s\n", r.status().ToString().c_str());
    return 1;
  }
  Report report(*a->Get("experiment"));
  report.Param("storage", "disk");
  report.Dataset("name", a->Get("label").value_or("unnamed"));
  for (uint64_t run = 0; run < warmup + runs; ++run) {
    const auto start = std::chrono::steady_clock::now();
    Status written = r->repo->WriteIndexCheckpoint();
    const double seconds = SecondsSince(start);
    if (!written.ok()) {
      std::fprintf(stderr, "dds_bench: %s\n", written.ToString().c_str());
      return 1;
    }
    std::map<std::string, double> c;
    c["write_s"] = seconds;
    c["checkpoint_bytes"] = CheckpointBytes(*r);
    c["blobs"] = static_cast<double>(r->repo->Internals().index.Size());
    if (run >= warmup) report.AddSample(seconds, std::move(c));
  }
  return report.Write(*a->Get("out")) ? 0 : 1;
}

}  // namespace dds::bench
