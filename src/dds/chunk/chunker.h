#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "dds/chunk/gear_table.h"
#include "dds/common/result.h"

namespace dds {

// The largest max_size any repository may use: the data blob limit (SPEC.md section 6).
inline constexpr uint32_t kMaxChunkSizeLimit = 16777216;

// Splits a byte stream into chunks. Stateless and thread-safe: one instance serves every worker.
//
// NextChunkLength looks at `data`, which starts at the current chunk start, and returns the length
// of the next chunk. It returns 0 exactly when it cannot decide yet: when more input may follow
// (`!eof`) and fewer than MaxSize() bytes are available, or when `data` is empty at end of input.
// Deciding earlier would let a boundary depend on where the input happened to be split, so feeding
// the same stream in different pieces could give different chunks (TESTING.md 3.4).
class Chunker {
 public:
  virtual ~Chunker() = default;
  virtual size_t NextChunkLength(std::span<const std::byte> data, bool eof) const = 0;
  virtual size_t MinSize() const = 0;
  virtual size_t MaxSize() const = 0;
};

enum class ChunkerKind { kFastCdc, kFixed };

// "fastcdc-2016" or "fixed": the `chunker` value in a repository's config.
const char* ChunkerKindName(ChunkerKind kind);

// Chunker parameters, fixed per repository at init (D8, D9), with the ranges of
// ON_DISK_FORMAT.md section 3:
// - fastcdc: 64 <= min < avg; avg a power of two in [256, 4 MiB]; avg < max <= 16 MiB;
//   normalization 0..3.
// - fixed: min = avg = max, avg a power of two in [256, 4 MiB]; normalization 0.
struct ChunkerParams {
  ChunkerKind kind = ChunkerKind::kFastCdc;
  uint32_t min_size = 2048;
  uint32_t avg_size = 8192;
  uint32_t max_size = 65536;
  int normalization = 2;
  uint64_t gear_seed = kDefaultGearSeed;
};

// InvalidArgument naming the first parameter out of range.
Status ValidateChunkerParams(const ChunkerParams& params);

// Validates `params` and builds the chunker they describe.
Result<std::unique_ptr<Chunker>> MakeChunker(const ChunkerParams& params);

}  // namespace dds
