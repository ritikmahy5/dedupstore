#include "dds/chunk/chunk_reader.h"

#include <algorithm>

namespace dds {

namespace {

// Small chunk sizes still read in large pieces, so the read syscall count stays low.
constexpr size_t kMinBufferBytes = size_t{256} * 1024;

}  // namespace

Result<size_t> SpanByteSource::Read(std::span<std::byte> out) {
  size_t n = std::min(out.size(), data_.size() - pos_);
  if (max_read_ != 0) n = std::min(n, max_read_);
  std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(pos_), n, out.begin());
  pos_ += n;
  return n;
}

// own_buf_ is declared before buf_, so it is constructed by the time buf_ refers to it.
ChunkReader::ChunkReader(const Chunker& chunker, ByteSource& source)
    : chunker_(chunker), source_(source), buf_(own_buf_) {
  GrowBuffer();
}

ChunkReader::ChunkReader(const Chunker& chunker, ByteSource& source, std::vector<std::byte>& buffer)
    : chunker_(chunker), source_(source), buf_(buffer) {
  GrowBuffer();
}

void ChunkReader::GrowBuffer() {
  // Twice the largest chunk: after compaction there is always room to read more. A larger
  // borrowed buffer is fine: Fill only needs MaxSize() <= buf_.size() / 2.
  const size_t needed = std::max(2 * chunker_.MaxSize(), kMinBufferBytes);
  if (buf_.size() < needed) buf_.resize(needed);
}

Result<std::optional<std::span<const std::byte>>> ChunkReader::Next() {
  while (true) {
    const auto available = std::span<const std::byte>(buf_).subspan(start_, end_ - start_);
    const size_t len = chunker_.NextChunkLength(available, eof_);
    if (len > 0) {
      start_ += len;
      return std::optional(available.first(len));
    }
    // The chunker declines only with fewer than MaxSize() bytes before end of stream, or with
    // nothing left at the end.
    if (eof_) return std::optional<std::span<const std::byte>>();
    DDS_TRY(Fill());
  }
}

Status ChunkReader::Fill() {
  if (start_ > 0) {
    std::copy(buf_.begin() + static_cast<std::ptrdiff_t>(start_),
              buf_.begin() + static_cast<std::ptrdiff_t>(end_), buf_.begin());
    end_ -= start_;
    start_ = 0;
  }
  while (!eof_ && end_ < chunker_.MaxSize()) {
    // Room to read is guaranteed: end_ < MaxSize() <= buf_.size() / 2. Without room, Read would
    // return 0, which would be mistaken for end of stream and silently truncate the file.
    DDS_CHECK(end_ < buf_.size());
    auto n = source_.Read(std::span(buf_).subspan(end_));
    DDS_TRY(n.status());
    if (*n == 0) {
      eof_ = true;
    } else {
      end_ += *n;
      bytes_read_ += *n;
    }
  }
  return Status::Ok();
}

}  // namespace dds
