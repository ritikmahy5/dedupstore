#include "dds/chunk/chunker.h"

#include <bit>
#include <memory>

#include "dds/chunk/fastcdc_chunker.h"
#include "dds/chunk/fixed_chunker.h"
#include "dds/common/strings.h"

namespace dds {

namespace {

constexpr uint32_t kMinAvg = 256;
constexpr uint32_t kMaxAvg = 4194304;  // 4 MiB
constexpr uint32_t kMinMin = 64;

}  // namespace

const char* ChunkerKindName(ChunkerKind kind) {
  switch (kind) {
    case ChunkerKind::kFastCdc:
      return "fastcdc-2016";
    case ChunkerKind::kFixed:
      return "fixed";
  }
  return "unknown";
}

Status ValidateChunkerParams(const ChunkerParams& p) {
  if (p.avg_size < kMinAvg || p.avg_size > kMaxAvg || !std::has_single_bit(p.avg_size)) {
    return Status::InvalidArgument(
        StrCat("avg_size ", p.avg_size, " must be a power of two from 256 to 4194304"));
  }
  if (p.kind == ChunkerKind::kFixed) {
    if (p.min_size != p.avg_size || p.max_size != p.avg_size) {
      return Status::InvalidArgument("fixed chunker: min_size and max_size must equal avg_size");
    }
    if (p.normalization != 0) {
      return Status::InvalidArgument("fixed chunker: normalization must be 0");
    }
    return Status::Ok();
  }
  if (p.min_size < kMinMin || p.min_size >= p.avg_size) {
    return Status::InvalidArgument(StrCat(
        "min_size ", p.min_size, " must be at least 64 and less than avg_size ", p.avg_size));
  }
  if (p.max_size <= p.avg_size || p.max_size > kMaxChunkSizeLimit) {
    return Status::InvalidArgument(StrCat("max_size ", p.max_size,
                                          " must be greater than avg_size ", p.avg_size,
                                          " and at most 16777216"));
  }
  if (p.normalization < 0 || p.normalization > 3) {
    return Status::InvalidArgument(StrCat("normalization ", p.normalization, " must be 0 to 3"));
  }
  return Status::Ok();
}

Result<std::unique_ptr<Chunker>> MakeChunker(const ChunkerParams& params) {
  DDS_TRY(ValidateChunkerParams(params));
  if (params.kind == ChunkerKind::kFixed) {
    return std::unique_ptr<Chunker>(std::make_unique<FixedChunker>(params.avg_size));
  }
  return std::unique_ptr<Chunker>(std::make_unique<FastCdcChunker>(params));
}

}  // namespace dds
