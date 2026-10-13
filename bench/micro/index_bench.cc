// FlatTable lookup and insert cost at load factors 0.4, 0.6, and 0.75, and ChunkIndex
// LookupAndStamp throughput with 1 to 32 threads (BENCHMARKS.md section 6; D18's and E9's
// revisit conditions refer to these).

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "dds/common/splitmix64.h"
#include "dds/index/chunk_index.h"
#include "dds/index/flat_table.h"

namespace dds {
namespace {

Fingerprint RandomFingerprint(uint64_t& state) {
  Fingerprint fp;
  for (size_t i = 0; i < fp.bytes.size(); i += 8) {
    uint64_t v = SplitMix64Next(state);
    for (size_t j = 0; j < 8; ++j, v >>= 8) fp.bytes[i + j] = static_cast<uint8_t>(v);
  }
  return fp;
}

constexpr size_t kCapacity = size_t{1} << 20;  // 64 MiB of entries: larger than the CPU caches

// A table of kCapacity slots filled to `load` (in percent), the fingerprints it holds, and as many
// it does not. The benchmarks walk these arrays in order: the slots they probe are random (the
// fingerprints are), so each probe misses the caches as in a real index, while the next key
// arrives through the hardware prefetcher, as a just-computed fingerprint would sit in a register.
// (A first version cycled through 4096 absent keys, whose slots stayed cached, and strided through
// the present keys, so every hit also paid a cache miss to load its key.) Successive lookups are
// independent, so the CPU overlaps their misses: the result is time per lookup in a stream of
// lookups, as in ingest, not the latency of one isolated lookup.
struct Filled {
  explicit Filled(int load) : table(kCapacity) {
    uint64_t state = 42;
    // One below 3/4 at load 75, so that InsertErase's insert does not grow the table to 2^21 slots
    // and measure load 0.375 instead.
    const size_t n = std::min(kCapacity * static_cast<size_t>(load) / 100, kCapacity * 3 / 4 - 1);
    for (size_t i = 0; i < n; ++i) {
      IndexEntry e;
      e.fp = RandomFingerprint(state);
      e.length = 1;
      table.Insert(e);
      present.push_back(e.fp);
    }
    for (size_t i = 0; i < n; ++i) absent.push_back(RandomFingerprint(state));
  }
  FlatTable table;
  std::vector<Fingerprint> present;
  std::vector<Fingerprint> absent;
};

void CheckCapacity(benchmark::State& state, const Filled& f) {
  if (f.table.capacity() != kCapacity) state.SkipWithError("the table grew; the load is wrong");
}

void BM_FlatTableLookupHit(benchmark::State& state) {
  Filled f(static_cast<int>(state.range(0)));
  size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(f.table.Find(f.present[i]));
    if (++i == f.present.size()) i = 0;
  }
  state.SetItemsProcessed(state.iterations());
  CheckCapacity(state, f);
}
BENCHMARK(BM_FlatTableLookupHit)->Arg(40)->Arg(60)->Arg(75);

void BM_FlatTableLookupMiss(benchmark::State& state) {
  Filled f(static_cast<int>(state.range(0)));
  size_t i = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(f.table.Find(f.absent[i]));
    if (++i == f.absent.size()) i = 0;
  }
  state.SetItemsProcessed(state.iterations());
  CheckCapacity(state, f);
}
BENCHMARK(BM_FlatTableLookupMiss)->Arg(40)->Arg(60)->Arg(75);

void BM_FlatTableInsertErase(benchmark::State& state) {
  Filled f(static_cast<int>(state.range(0)));
  size_t i = 0;
  for (auto _ : state) {
    IndexEntry e;
    e.fp = f.absent[i];
    e.length = 1;
    f.table.Insert(e);
    f.table.Erase(e.fp);  // keeps the load factor where it is
    if (++i == f.absent.size()) i = 0;
  }
  state.SetItemsProcessed(state.iterations());
  CheckCapacity(state, f);
}
BENCHMARK(BM_FlatTableInsertErase)->Arg(40)->Arg(60)->Arg(75);

// Shared by the benchmark's threads. Thread 0 builds it before the timed loop and frees it after;
// Google Benchmark starts and ends the loop with a barrier across threads, which orders both.
struct SharedIndex {
  ChunkIndex index;
  std::vector<Fingerprint> fps;
};
std::unique_ptr<SharedIndex> g_shared;
constexpr size_t kIndexKeys = size_t{1} << 20;

void BM_ChunkIndexLookupAndStamp(benchmark::State& state) {
  if (state.thread_index() == 0) {
    g_shared = std::make_unique<SharedIndex>();
    uint64_t seed = 99;
    std::vector<SealedEntry> entries;
    for (size_t i = 0; i < kIndexKeys; ++i) {
      SealedEntry e;
      e.fp = RandomFingerprint(seed);
      e.loc.length = 1;
      entries.push_back(e);
      g_shared->fps.push_back(e.fp);
    }
    g_shared->index.Publish(entries, 1);
  }
  // Each thread walks the keys in order from its own starting point (see Filled for why in order).
  // Only the timed loop may touch g_shared: before its barrier, thread 0 may still be building it.
  size_t i = static_cast<size_t>(state.thread_index()) * (kIndexKeys / 32);
  for (auto _ : state) {
    benchmark::DoNotOptimize(g_shared->index.LookupAndStamp(g_shared->fps[i], 2));
    if (++i == kIndexKeys) i = 0;
  }
  state.SetItemsProcessed(state.iterations());
  if (state.thread_index() == 0) g_shared.reset();
}
BENCHMARK(BM_ChunkIndexLookupAndStamp)->ThreadRange(1, 32)->UseRealTime();

}  // namespace
}  // namespace dds
