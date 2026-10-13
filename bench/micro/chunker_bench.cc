// Chunker throughput (BENCHMARKS.md section 6): FastCDC and fixed-size, per average chunk size.
// The sizes follow E2 (min = avg / 4, max = avg * 8). Input is random, so FastCDC sees no
// pathological runs.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <random>
#include <vector>

#include "dds/chunk/chunker.h"

namespace dds {
namespace {

const std::vector<std::byte>& Input() {
  static const std::vector<std::byte> data = [] {
    std::vector<std::byte> v(64 << 20);
    std::mt19937_64 rng(42);
    for (auto& b : v) b = static_cast<std::byte>(rng());
    return v;
  }();
  return data;
}

void ChunkAll(benchmark::State& state, const ChunkerParams& params) {
  auto chunker = MakeChunker(params);
  if (!chunker.ok()) {
    state.SkipWithError(chunker.status().ToString());
    return;
  }
  const auto& data = Input();
  size_t chunks = 0;
  for (auto _ : state) {
    size_t pos = 0;
    while (pos < data.size()) {
      pos += (*chunker)->NextChunkLength(std::span(data).subspan(pos), true);
      ++chunks;
    }
    benchmark::DoNotOptimize(pos);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(data.size()));
  state.counters["mean_chunk"] =
      benchmark::Counter(static_cast<double>(state.iterations()) *
                         static_cast<double>(data.size()) / static_cast<double>(chunks));
}

void BM_FastCdc(benchmark::State& state) {
  const auto avg = static_cast<uint32_t>(state.range(0));
  ChunkerParams p;
  p.avg_size = avg;
  p.min_size = avg / 4;
  p.max_size = avg * 8;
  ChunkAll(state, p);
}
BENCHMARK(BM_FastCdc)->Arg(2048)->Arg(4096)->Arg(8192)->Arg(16384)->Arg(65536)->Arg(1 << 20);

void BM_Fixed(benchmark::State& state) {
  const auto avg = static_cast<uint32_t>(state.range(0));
  ChunkerParams p;
  p.kind = ChunkerKind::kFixed;
  p.min_size = p.avg_size = p.max_size = avg;
  p.normalization = 0;
  ChunkAll(state, p);
}
// The fixed chunker does no per-byte work (it returns its size), so its "throughput" only shows
// that chunking cost is entirely FastCDC's hashing loop.
BENCHMARK(BM_Fixed)->Arg(8192);

}  // namespace
}  // namespace dds
