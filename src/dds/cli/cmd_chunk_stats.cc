// dds chunk-stats <file> [--chunker fastcdc|fixed] [--min-size B] [--avg-size B] [--max-size B]
//                        [--json]
// A development tool: chunk one file and report the chunk count, size histogram, and throughput.
// It touches no repository (SPEC.md section 3).

#include <algorithm>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "dds/chunk/chunk_reader.h"
#include "dds/chunk/chunker.h"
#include "dds/chunk/fd_byte_source.h"
#include "dds/cli/chunker_flags.h"
#include "dds/cli/command.h"
#include "dds/cli/json.h"
#include "dds/common/clock.h"
#include "dds/common/strings.h"

namespace dds::cli {

namespace {

constexpr char kUsage[] =
    "dds chunk-stats <file> [--chunker fastcdc|fixed] [--min-size B] [--avg-size B] "
    "[--max-size B] [--json]";

struct ChunkStats {
  uint64_t bytes = 0;
  uint64_t chunks = 0;
  uint64_t smallest = 0;
  uint64_t largest = 0;
  uint64_t bucket_bytes = 0;     // histogram bucket width: avg_size / 4
  std::vector<uint64_t> counts;  // counts[i]: chunks with length in [i, i + 1) * bucket_bytes
  double seconds = 0;
};

Result<ChunkStats> Measure(const std::string& path, const ChunkerParams& params) {
  auto chunker = MakeChunker(params);
  DDS_TRY(chunker.status());
  auto source = FdByteSource::Open(path);
  DDS_TRY(source.status());
  ChunkStats stats;
  stats.bucket_bytes = std::max<uint64_t>(1, params.avg_size / 4);
  stats.counts.assign(params.max_size / stats.bucket_bytes + 1, 0);
  const int64_t start = SystemClock::Instance().MonotonicNanos();
  ChunkReader reader(**chunker, *source);
  while (true) {
    auto next = reader.Next();
    DDS_TRY(next.status());
    const std::optional<std::span<const std::byte>>& chunk = *next;
    if (!chunk.has_value()) break;
    const uint64_t len = chunk->size();
    stats.smallest = stats.chunks == 0 ? len : std::min(stats.smallest, len);
    stats.largest = std::max(stats.largest, len);
    ++stats.chunks;
    ++stats.counts[len / stats.bucket_bytes];
  }
  stats.seconds = static_cast<double>(SystemClock::Instance().MonotonicNanos() - start) / 1e9;
  stats.bytes = reader.bytes_read();
  return stats;
}

double MeanChunk(const ChunkStats& s) {
  return s.chunks == 0 ? 0.0 : static_cast<double>(s.bytes) / static_cast<double>(s.chunks);
}

double MegabytesPerSecond(const ChunkStats& s) {  // decimal MB: 10^6 bytes
  return s.seconds > 0 ? static_cast<double>(s.bytes) / 1e6 / s.seconds : 0.0;
}

void PrintJson(const std::string& path, const ChunkerParams& p, const ChunkStats& s) {
  JsonWriter w;
  w.BeginObject();
  w.Key("file");
  w.String(path);
  w.Key("bytes");
  w.Uint(s.bytes);
  w.Key("chunker");
  w.String(ChunkerKindName(p.kind));
  w.Key("min_size");
  w.Uint(p.min_size);
  w.Key("avg_size");
  w.Uint(p.avg_size);
  w.Key("max_size");
  w.Uint(p.max_size);
  w.Key("normalization");
  w.Int(p.normalization);
  w.Key("chunks");
  w.Uint(s.chunks);
  w.Key("mean_chunk_bytes");
  w.Double(MeanChunk(s));
  w.Key("smallest_chunk");
  w.Uint(s.smallest);
  w.Key("largest_chunk");
  w.Uint(s.largest);
  w.Key("seconds");
  w.Double(s.seconds);
  w.Key("mb_per_s");
  w.Double(MegabytesPerSecond(s));
  w.Key("histogram");
  w.BeginObject();
  w.Key("bucket_bytes");
  w.Uint(s.bucket_bytes);
  w.Key("counts");
  w.BeginArray();
  for (uint64_t c : s.counts) w.Uint(c);
  w.EndArray();
  w.EndObject();
  w.EndObject();
  std::printf("%s\n", w.Take().c_str());
}

void PrintHuman(const std::string& path, const ChunkerParams& p, const ChunkStats& s) {
  std::fprintf(stderr, "file: %s\nbytes: %llu\n", path.c_str(),
               static_cast<unsigned long long>(s.bytes));
  std::fprintf(stderr, "chunker: %s (min %u, avg %u, max %u, normalization %d)\n",
               ChunkerKindName(p.kind), p.min_size, p.avg_size, p.max_size, p.normalization);
  std::fprintf(stderr, "chunks: %llu\nmean chunk: %.1f bytes (smallest %llu, largest %llu)\n",
               static_cast<unsigned long long>(s.chunks), MeanChunk(s),
               static_cast<unsigned long long>(s.smallest),
               static_cast<unsigned long long>(s.largest));
  std::fprintf(stderr, "time: %.3f s (%.1f MB/s, read + chunk)\n", s.seconds,
               MegabytesPerSecond(s));
  if (s.chunks == 0) return;
  // Print the buckets from the first to the last non-empty one, with a bar scaled to the largest.
  size_t first = 0;
  size_t last = s.counts.size() - 1;
  while (s.counts[first] == 0) ++first;
  while (s.counts[last] == 0) --last;
  const uint64_t peak = *std::max_element(s.counts.begin(), s.counts.end());
  std::fprintf(stderr, "histogram (bucket width %llu bytes):\n",
               static_cast<unsigned long long>(s.bucket_bytes));
  for (size_t i = first; i <= last; ++i) {
    const auto bar =
        static_cast<size_t>(50.0 * static_cast<double>(s.counts[i]) / static_cast<double>(peak));
    std::fprintf(stderr, "  [%8llu, %8llu) %10llu %s\n",
                 static_cast<unsigned long long>(i) * s.bucket_bytes,
                 static_cast<unsigned long long>(i + 1) * s.bucket_bytes,
                 static_cast<unsigned long long>(s.counts[i]), std::string(bar, '#').c_str());
  }
}

int Run(const ParsedArgs& args) {
  ChunkerParams params;
  if (auto usage_error = ChunkerParamsFromFlags(args, &params))
    return UsageError(kUsage, *usage_error);
  const std::string& path = args.positional[0];
  auto stats = Measure(path, params);
  if (!stats.ok()) return ReportError(stats.status());
  if (args.Has("json")) {
    PrintJson(path, params, *stats);
  } else {
    PrintHuman(path, params, *stats);
  }
  return kExitOk;
}

}  // namespace

Command MakeChunkStatsCommand() {
  Command cmd;
  cmd.name = "chunk-stats";
  cmd.usage = kUsage;
  cmd.summary = "development tool: chunk a file and print chunk count, size histogram, speed";
  cmd.flags = kChunkerFlags;
  cmd.flags.push_back({"json", false});
  cmd.min_positional = 1;
  cmd.max_positional = 1;
  cmd.run = Run;
  return cmd;
}

}  // namespace dds::cli
