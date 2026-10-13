#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "dds/common/result.h"

struct ZSTD_CCtx_s;  // zstd's compression context (zstd.h); only the .cc file includes zstd.h

namespace dds {

// How a blob's payload is stored in its container (D107). Fingerprints and CRCs work as
// before: the fingerprint names the content, and the CRC32C covers the bytes as stored.
enum class Codec : uint8_t {
  kNone = 0,  // the content itself
  kZstd = 1,  // one zstd frame that records its content size
};

// Validates a codec byte read from disk.
std::optional<Codec> CodecFromByte(uint8_t value);
const char* CodecName(Codec codec);  // "none", "zstd"

inline constexpr int kMinZstdLevel = 1;
inline constexpr int kMaxZstdLevel = 19;  // above 19 zstd needs far more memory per context

// A repository's compression setting, chosen at init (D107).
struct CompressionParams {
  Codec codec = Codec::kNone;
  int level = 0;  // kMinZstdLevel..kMaxZstdLevel for zstd; 0 for none

  friend bool operator==(const CompressionParams&, const CompressionParams&) = default;
};

// Compresses blob payloads with one reusable zstd context: a fresh context per blob would allocate
// hundreds of KB each time. Owned by one ContainerWriter and, like it, used by one thread.
class ZstdCompressor {
 public:
  explicit ZstdCompressor(int level);
  // Writes `payload` compressed into `out` and returns true if the frame is smaller than the
  // payload; otherwise returns false and `out` holds nothing useful. Already-compressed data
  // (archives, media, model weights) is stored as it is, so compression never makes a blob bigger.
  bool CompressIfSmaller(std::span<const std::byte> payload, std::vector<std::byte>& out);

 private:
  struct Deleter {
    void operator()(ZSTD_CCtx_s* ctx) const;
  };
  std::unique_ptr<ZSTD_CCtx_s, Deleter> ctx_;
  int level_;
};

// The content size a stored zstd frame's header records, which must be 1 to `max_size`; Corruption
// otherwise. Reads only the header: a hostile replication peer's recipe is checked against it
// without decompressing (D107).
Result<uint32_t> ZstdContentSize(std::span<const std::byte> stored, uint32_t max_size);

// Decompresses a stored zstd frame whose content must be 1 to `max_size` bytes. Corruption if the
// bytes are not exactly one frame that records its content size, if the recorded size is out of
// range, or if decoding fails. Never reads or writes out of bounds, whatever the bytes are.
Result<std::vector<std::byte>> DecompressZstd(std::span<const std::byte> stored, uint32_t max_size);

}  // namespace dds
