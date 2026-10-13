#include "dds/store/compression.h"

#include <zstd.h>

#include "dds/common/check.h"
#include "dds/common/strings.h"

namespace dds {

namespace {

struct DctxDeleter {
  void operator()(ZSTD_DCtx* ctx) const { ZSTD_freeDCtx(ctx); }
};

// One decompression context per thread, reused for every read: restore reads one blob at a time
// per thread, and a context per read would allocate on every chunk.
ZSTD_DCtx* ThreadDecompressor() {
  thread_local std::unique_ptr<ZSTD_DCtx, DctxDeleter> ctx(ZSTD_createDCtx());
  DDS_CHECK(ctx != nullptr);  // allocation failed: out of memory
  return ctx.get();
}

}  // namespace

std::optional<Codec> CodecFromByte(uint8_t value) {
  if (value == 0) return Codec::kNone;
  if (value == 1) return Codec::kZstd;
  return std::nullopt;
}

const char* CodecName(Codec codec) { return codec == Codec::kZstd ? "zstd" : "none"; }

void ZstdCompressor::Deleter::operator()(ZSTD_CCtx_s* ctx) const { ZSTD_freeCCtx(ctx); }

ZstdCompressor::ZstdCompressor(int level) : ctx_(ZSTD_createCCtx()), level_(level) {
  DDS_CHECK(ctx_ != nullptr);  // allocation failed: out of memory
  DDS_CHECK(level >= kMinZstdLevel && level <= kMaxZstdLevel);
}

bool ZstdCompressor::CompressIfSmaller(std::span<const std::byte> payload,
                                       std::vector<std::byte>& out) {
  out.resize(ZSTD_compressBound(payload.size()));
  // ZSTD_compressCCtx knows the input size up front, so the frame header records the content size,
  // which DecompressZstd requires (zstd.h, ZSTD_getFrameContentSize).
  const size_t n =
      ZSTD_compressCCtx(ctx_.get(), out.data(), out.size(), payload.data(), payload.size(), level_);
  if (ZSTD_isError(n) != 0 || n >= payload.size()) return false;
  out.resize(n);
  return true;
}

Result<uint32_t> ZstdContentSize(std::span<const std::byte> stored, uint32_t max_size) {
  const uint64_t size = ZSTD_getFrameContentSize(stored.data(), stored.size());
  if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN) {
    return Status::Corruption("not a zstd frame with a recorded content size");
  }
  if (size == 0 || size > max_size) {
    return Status::Corruption(
        StrCat("zstd frame records ", size, " content bytes, outside 1..", max_size));
  }
  return static_cast<uint32_t>(size);
}

Result<std::vector<std::byte>> DecompressZstd(std::span<const std::byte> stored,
                                              uint32_t max_size) {
  auto content_size = ZstdContentSize(stored, max_size);
  DDS_TRY(content_size.status());
  const uint32_t size = *content_size;
  // Exactly one frame: nothing may follow it in a stored blob.
  const size_t frame = ZSTD_findFrameCompressedSize(stored.data(), stored.size());
  if (ZSTD_isError(frame) != 0 || frame != stored.size()) {
    return Status::Corruption("stored bytes are not exactly one zstd frame");
  }
  std::vector<std::byte> content(size);
  const size_t n = ZSTD_decompressDCtx(ThreadDecompressor(), content.data(), content.size(),
                                       stored.data(), stored.size());
  if (ZSTD_isError(n) != 0) {
    return Status::Corruption(StrCat("zstd frame does not decode: ", ZSTD_getErrorName(n)));
  }
  if (n != content.size()) {
    return Status::Corruption(StrCat("zstd frame decodes to ", n, " bytes, not ", size));
  }
  return content;
}

}  // namespace dds
