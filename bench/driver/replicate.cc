// dds_bench replicate (E7, S4prune): replicate a base snapshot, then the next version, over a
// SimulatedLink with a given latency, bandwidth, and window, per sample. --prune 0 turns subtree
// pruning off (D108); --base-free 1 sends the base over an unlimited link, as setup for measuring
// the next version alone.

#include <cstdio>
#include <thread>

#include "dds/repl/receiver.h"
#include "dds/repl/sender.h"
#include "dds/repl/simulated_link.h"
#include "dds/snapshot/source_tree.h"
#include "driver/driver.h"
#include "driver/report.h"

namespace dds::bench {

namespace {

// One transfer into `dst`; adds "<label>.*" counters. False on error.
bool Transfer(Repository& src, const SnapshotId& id, Repository& dst, const LinkOptions& link,
              const SenderOptions& options, double logical, const std::string& label, double mbit,
              std::map<std::string, double>& c) {
  auto [to_receiver, to_sender] = MakeSimulatedLink(link);
  Result<ReceiveReport> received = Status::Aborted("not run");
  std::thread serve([&, end = to_sender.get()] { received = ServeReplication(dst, *end, {}); });
  const auto start = std::chrono::steady_clock::now();
  auto sent = Replicate(src, id, *to_receiver, options);
  const double seconds = SecondsSince(start);
  to_receiver->Close();
  serve.join();
  if (!sent.ok()) {
    std::fprintf(stderr, "dds_bench: %s: %s\n", label.c_str(), sent.status().ToString().c_str());
    return false;
  }
  if (!received.ok()) {
    std::fprintf(stderr, "dds_bench: %s: receiver: %s\n", label.c_str(),
                 received.status().ToString().c_str());
    return false;
  }
  c[label + ".fingerprints_queried"] = static_cast<double>(sent->fingerprints_queried);
  c[label + ".trees_checked"] = static_cast<double>(sent->trees_checked);
  c[label + ".subtrees_pruned"] = static_cast<double>(sent->subtrees_pruned);
  c[label + ".receiver_subtrees_whole"] = static_cast<double>(received->subtrees_whole);
  double wire = static_cast<double>(sent->wire_bytes_received);
  for (const auto& [type, bytes] : sent->wire_bytes_sent) {
    c[label + ".wire." + MessageTypeName(type)] = static_cast<double>(bytes);
    wire += static_cast<double>(bytes);
  }
  c[label + ".wire.received"] = static_cast<double>(sent->wire_bytes_received);
  c[label + ".wire_bytes"] = wire;
  c[label + ".seconds"] = seconds;
  c[label + ".logical_bytes"] = logical;
  c[label + ".payload_bytes"] = static_cast<double>(sent->payload_bytes_sent);
  c[label + ".wire_per_logical"] = wire / logical;
  c[label + ".achieved_mbit"] = wire * 8 / 1e6 / seconds;
  if (mbit > 0) c[label + ".utilization"] = wire * 8 / 1e6 / seconds / mbit;
  return true;
}

}  // namespace

int RunReplicate(const std::vector<std::string>& words) {
  auto a = ParseFlags(words, {{"base", true},
                              {"next", true},
                              {"latency-ms", true},
                              {"mbit", true},
                              {"window", true},
                              {"prune", true},
                              {"base-free", true},
                              {"dataset", true}});
  if (!a || !a->Get("out") || !a->Get("base") || !a->Get("next")) {
    std::fprintf(stderr,
                 "usage: dds_bench replicate --experiment E --out FILE --base DIR --next DIR "
                 "--latency-ms L --mbit B --window W [--prune 0|1] [--base-free 0|1]\n");
    return 1;
  }
  const double latency_ms = FlagDouble(*a, "latency-ms", 0);
  const double mbit = FlagDouble(*a, "mbit", 0);
  SenderOptions options;
  options.window = static_cast<size_t>(FlagU64(*a, "window", 8));
  options.prune_subtrees = FlagU64(*a, "prune", 1) != 0;
  const bool base_free = FlagU64(*a, "base-free", 0) != 0;
  const uint64_t runs = FlagU64(*a, "runs", 5), warmup = FlagU64(*a, "warmup", 1);
  LinkOptions link;
  link.one_way_latency = std::chrono::microseconds(static_cast<int64_t>(latency_ms * 1000));
  link.bytes_per_second = static_cast<uint64_t>(mbit * 1e6 / 8);
  Report report(*a->Get("experiment"));
  report.Param("one_way_latency_ms", latency_ms);
  report.Param("mbit", mbit);
  report.Param("window", static_cast<double>(options.window));
  report.Param("prune", options.prune_subtrees ? 1.0 : 0.0);
  report.Param("base_free", base_free ? 1.0 : 0.0);
  report.Dataset("name", a->Get("dataset").value_or("synth"));
  // The source is built once, untimed.
  auto src = FreshRepo("mem", "", RepoParams{});
  if (!src.ok()) return 1;
  PosixSourceTree base_tree(*a->Get("base"));
  PosixSourceTree next_tree(*a->Get("next"));
  auto base = src->repo->Backup(base_tree, BackupOptions{});
  auto next = src->repo->Backup(next_tree, BackupOptions{});
  if (!base.ok() || !next.ok()) return 1;
  report.Dataset("base_logical_bytes", static_cast<double>(base->manifest.logical_bytes));
  report.Dataset("next_logical_bytes", static_cast<double>(next->manifest.logical_bytes));
  for (uint64_t run = 0; run < warmup + runs; ++run) {
    auto dst = FreshRepo("mem", "", RepoParams{});
    if (!dst.ok()) return 1;
    std::map<std::string, double> c;
    const auto start = std::chrono::steady_clock::now();
    if (!Transfer(*src->repo, base->id, *dst->repo, base_free ? LinkOptions{} : link, options,
                  static_cast<double>(base->manifest.logical_bytes), "base", base_free ? 0 : mbit,
                  c) ||
        !Transfer(*src->repo, next->id, *dst->repo, link, options,
                  static_cast<double>(next->manifest.logical_bytes), "next", mbit, c)) {
      return 1;
    }
    if (run >= warmup) report.AddSample(SecondsSince(start), std::move(c));
  }
  return report.Write(*a->Get("out")) ? 0 : 1;
}

}  // namespace dds::bench
