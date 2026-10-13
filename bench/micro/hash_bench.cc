// BLAKE3 and CRC32C throughput by buffer size (BENCHMARKS.md section 6).

#include <benchmark/benchmark.h>

#include <cstddef>
#include <random>
#include <string>
#include <vector>

#include "dds/common/crc32c.h"
#include "dds/hash/blake3_hasher.h"
#include "dds/hash/fingerprint.h"

namespace dds {
namespace {

std::vector<std::byte> RandomBuffer(size_t size) {
  std::vector<std::byte> v(size);
  std::mt19937_64 rng(7);
  for (auto& b : v) b = static_cast<std::byte>(rng());
  return v;
}

void BM_Blake3(benchmark::State& state) {
  const auto data = RandomBuffer(static_cast<size_t>(state.range(0)));
  for (auto _ : state) {
    Blake3Hasher h;
    h.Update(data);
    benchmark::DoNotOptimize(h.Finalize());
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(data.size()));
}
BENCHMARK(BM_Blake3)->RangeMultiplier(4)->Range(1024, 1 << 20);

void BM_FingerprintOf(benchmark::State& state) {
  const auto data = RandomBuffer(static_cast<size_t>(state.range(0)));
  for (auto _ : state) benchmark::DoNotOptimize(FingerprintOf(BlobType::kData, data));
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(data.size()));
}
BENCHMARK(BM_FingerprintOf)->Arg(8192);

// Crc32c uses the CPU's CRC32C instructions where it has them (D103); the label says which.
void BM_Crc32c(benchmark::State& state) {
  const auto data = RandomBuffer(static_cast<size_t>(state.range(0)));
  for (auto _ : state) benchmark::DoNotOptimize(Crc32c(data));
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(data.size()));
  state.SetLabel(std::string(Crc32cImplementation()));
}
BENCHMARK(BM_Crc32c)->RangeMultiplier(4)->Range(1024, 1 << 20);

// Slicing-by-8 on the same machine, for the comparison S2ab reports.
void BM_Crc32cSoftware(benchmark::State& state) {
  const auto data = RandomBuffer(static_cast<size_t>(state.range(0)));
  for (auto _ : state) benchmark::DoNotOptimize(Crc32cExtendSoftware(0, data));
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(data.size()));
}
BENCHMARK(BM_Crc32cSoftware)->RangeMultiplier(4)->Range(1024, 1 << 20);

}  // namespace
}  // namespace dds
