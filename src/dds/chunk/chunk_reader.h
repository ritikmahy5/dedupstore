#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "dds/chunk/chunker.h"
#include "dds/common/result.h"

namespace dds {

// A stream of bytes: a file being backed up, or bytes in memory.
class ByteSource {
 public:
  virtual ~ByteSource() = default;
  // Reads up to out.size() bytes into `out` and returns how many. Returns 0 only at end of stream.
  // May return fewer bytes than asked for at any time.
  virtual Result<size_t> Read(std::span<std::byte> out) = 0;
};

// A ByteSource over bytes in memory. With `max_read`, each Read returns at most that many bytes
// (0 means no limit), which tests use to feed input in small pieces.
class SpanByteSource final : public ByteSource {
 public:
  explicit SpanByteSource(std::span<const std::byte> data, size_t max_read = 0)
      : data_(data), max_read_(max_read) {}
  Result<size_t> Read(std::span<std::byte> out) override;

 private:
  std::span<const std::byte> data_;
  size_t max_read_;
  size_t pos_ = 0;
};

// Buffers a ByteSource and yields its chunks in order:
//
//   ChunkReader reader(chunker, source);
//   while (true) {
//     auto chunk = reader.Next();          // error, nullopt at end, or a view of the next chunk
//     ...
//   }
//
// The buffer holds at least MaxSize() bytes beyond the chunk start whenever the stream has them,
// so the chunker always decides on a full window (Chunker's contract) and boundaries do not
// depend on how the source splits its reads. A returned chunk is a view into the buffer, valid
// until the next call to Next.
//
// The buffer is 2 x MaxSize() (at least 256 KiB). A caller that reads many streams, as a backup
// worker reads one per file, lends the reader one buffer for all of them: allocating and
// zero-filling it per stream cost more than chunking small files (a 16 MiB buffer per kernel file
// at a 1 MiB average chunk size cut E2's ingest from about 3000 to 93 MB/s).
class ChunkReader {
 public:
  // Uses a buffer of its own.
  ChunkReader(const Chunker& chunker, ByteSource& source);
  // Uses `buffer`, growing it only if it is too small. Its contents are ignored. It must outlive
  // the reader, and nothing else may use it meanwhile.
  ChunkReader(const Chunker& chunker, ByteSource& source, std::vector<std::byte>& buffer);

  Result<std::optional<std::span<const std::byte>>> Next();

  // Total bytes read from the source so far.
  uint64_t bytes_read() const { return bytes_read_; }

 private:
  // Resizes buf_ to 2 x MaxSize() (at least 256 KiB) unless it is already that large.
  void GrowBuffer();
  // Moves the unconsumed bytes to the front and reads until MaxSize() bytes are buffered or the
  // source ends.
  Status Fill();

  const Chunker& chunker_;
  ByteSource& source_;
  std::vector<std::byte> own_buf_;  // empty when the caller lends a buffer
  std::vector<std::byte>& buf_;     // own_buf_ or the caller's (declared after own_buf_)
  size_t start_ = 0;                // first byte of the next chunk
  size_t end_ = 0;                  // one past the last buffered byte
  bool eof_ = false;
  uint64_t bytes_read_ = 0;
};

}  // namespace dds
