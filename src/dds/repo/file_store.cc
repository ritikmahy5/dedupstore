#include "dds/repo/file_store.h"

#include <optional>
#include <utility>

#include "dds/chunk/chunk_reader.h"
#include "dds/common/clock.h"
#include "dds/common/log.h"
#include "dds/common/strings.h"
#include "dds/repo/backup.h"
#include "dds/repo/ingest_session.h"
#include "dds/repo/repository.h"

namespace dds {

FileStorer::FileStorer(RepoInternals& repo, SourceTree& source, IngestSession& session,
                       BackupQueue& queue, uint64_t split_bytes, size_t workers,
                       const std::atomic<bool>& stop)
    : repo_(repo),
      source_(source),
      session_(session),
      queue_(queue),
      // Splitting needs another worker to share the file with.
      split_bytes_(workers > 1 ? split_bytes : UINT64_MAX),
      // Enough to keep every worker busy with this file's segments, and one to fill meanwhile.
      max_in_flight_(workers + 1),
      stop_(stop) {}

Status FileStorer::PutIfNew(const Fingerprint& fp, BlobType type, std::span<const std::byte> bytes,
                            size_t writer) {
  if (session_.Lookup(fp, Reserve::kOn) == LookupResult::kHit) return Status::Ok();
  return session_.Put(fp, type, bytes, writer);
}

Status FileStorer::FlushPiece(Pieces& pieces, const std::string& path, size_t writer) {
  if (pieces.list.size() == kMaxPiecesPerFile) {
    return Status::InvalidArgument(StrCat(path, ": file needs more than 65536 recipe pieces"));
  }
  const auto bytes = EncodeRecipe(pieces.piece);
  const Fingerprint fp = FingerprintOf(BlobType::kRecipe, bytes);
  DDS_TRY(PutIfNew(fp, BlobType::kRecipe, bytes, writer));
  pieces.list.push_back(fp);
  pieces.piece.refs.clear();
  return Status::Ok();
}

Status FileStorer::AddRef(Pieces& pieces, const ChunkRef& ref, const std::string& path,
                          size_t writer) {
  pieces.piece.refs.push_back(ref);
  pieces.size += ref.length;
  if (pieces.piece.refs.size() == kMaxRefsPerRecipe) return FlushPiece(pieces, path, writer);
  return Status::Ok();
}

Status FileStorer::Finish(FileTask& task, Pieces& pieces, size_t writer) {
  if (!pieces.piece.refs.empty()) DDS_TRY(FlushPiece(pieces, task.path, writer));
  if (pieces.size != task.listed_size) {
    // Stored as read, with a warning, never an error (SPEC.md section 4).
    LogWarn(StrCat(task.path, " changed size while it was read; storing what was read"));
    changed_.fetch_add(1, std::memory_order_relaxed);
  }
  task.entry->size = pieces.size;
  task.entry->pieces = std::move(pieces.list);
  return Status::Ok();
}

Status FileStorer::StoreFile(FileTask& task, size_t writer, std::vector<std::byte>& buffer) {
  if (task.listed_size >= split_bytes_) return StoreSplit(task, writer, buffer);
  return StoreWhole(task, writer, buffer);
}

Status FileStorer::StoreWhole(FileTask& task, size_t writer, std::vector<std::byte>& buffer) {
  auto source = source_.OpenFile(task.path);
  DDS_TRY(source.status());
  ChunkReader reader(*repo_.chunker, **source, buffer);
  Pieces pieces;
  // Three clock reads per chunk (E3's breakdown): about 1% of the time of an 8 KiB chunk.
  const Clock& clock = repo_.clock;
  int64_t read_ns = 0, hash_ns = 0, store_ns = 0;
  while (true) {
    const int64_t t0 = clock.MonotonicNanos();
    auto next = reader.Next();
    const int64_t t1 = clock.MonotonicNanos();
    read_ns += t1 - t0;
    if (!next.ok()) return next.status().WithContext(task.path);
    const std::optional<std::span<const std::byte>>& chunk = *next;
    if (!chunk.has_value()) break;
    const Fingerprint fp = FingerprintOf(BlobType::kData, *chunk);
    const int64_t t2 = clock.MonotonicNanos();
    hash_ns += t2 - t1;
    DDS_TRY(PutIfNew(fp, BlobType::kData, *chunk, writer));
    DDS_TRY(AddRef(pieces, {fp, static_cast<uint32_t>(chunk->size())}, task.path, writer));
    store_ns += clock.MonotonicNanos() - t2;
  }
  const int64_t t3 = clock.MonotonicNanos();
  DDS_TRY(Finish(task, pieces, writer));
  store_ns += clock.MonotonicNanos() - t3;
  read_ns_.fetch_add(static_cast<uint64_t>(read_ns), std::memory_order_relaxed);
  hash_ns_.fetch_add(static_cast<uint64_t>(hash_ns), std::memory_order_relaxed);
  store_ns_.fetch_add(static_cast<uint64_t>(store_ns), std::memory_order_relaxed);
  return Status::Ok();
}

Status FileStorer::StoreSplit(FileTask& task, size_t writer, std::vector<std::byte>& buffer) {
  auto source = source_.OpenFile(task.path);
  DDS_TRY(source.status());
  split_.fetch_add(1, std::memory_order_relaxed);
  ChunkReader reader(*repo_.chunker, **source, buffer);
  Pieces pieces;
  // In file order. Every queued run must be done before this returns, error or not: another worker
  // may be storing it.
  std::deque<Run> in_flight;
  Status status;
  bool end = false;
  while (status.ok() && !end) {
    // Copy the chunks for another worker only if one is idle: otherwise storing them here, straight
    // from the read buffer, costs no copy, as when every worker has a large file of its own.
    // Assigned, not brace-initialized: clang-tidy 19.1's analyzer, with libstdc++, loses the
    // segment's destructor on the `break` below and reports a leak that is not there.
    Run run;
    run.segment = std::make_unique<Segment>();
    run.queued = queue_.HasIdleThread();
    status = ReadRun(reader, *run.segment, run.queued, task.path, writer, end);
    if (!status.ok() || (run.segment->lengths.empty() && run.segment->refs.empty())) break;
    if (run.queued && !queue_.TryPushSegment(run.segment.get())) {
      run.queued = false;  // the idle thread found other work meanwhile
      status = StoreSegment(*run.segment, writer);
    }
    if (run.queued) queued_.fetch_add(1, std::memory_order_relaxed);
    in_flight.push_back(std::move(run));
    // Runs stored here are collected at once, in order; a queued one at the front is waited for
    // only when too many are in flight.
    while (status.ok() && !in_flight.empty() &&
           (!in_flight.front().queued || in_flight.size() > max_in_flight_)) {
      status = CollectOldest(in_flight, pieces, task.path, writer, status);
    }
  }
  while (!in_flight.empty()) {
    const Status collected = CollectOldest(in_flight, pieces, task.path, writer, status);
    if (status.ok()) status = collected;
  }
  DDS_TRY(status);
  const int64_t t0 = repo_.clock.MonotonicNanos();
  DDS_TRY(Finish(task, pieces, writer));
  store_ns_.fetch_add(static_cast<uint64_t>(repo_.clock.MonotonicNanos() - t0),
                      std::memory_order_relaxed);
  return Status::Ok();
}

Status FileStorer::ReadRun(ChunkReader& reader, Segment& s, bool copy, const std::string& path,
                           size_t writer, bool& end) {
  const Clock& clock = repo_.clock;
  int64_t read_ns = 0, hash_ns = 0, store_ns = 0;
  size_t bytes = 0;
  Status status;
  // A run ends at the first chunk that reaches kSegmentBytes, so it never holds more than this.
  // Growing the vector from empty instead copied each run about three times into a fresh buffer,
  // on the splitter's thread, which is the one that limits a split file (S6split, D109).
  if (copy) s.data.reserve(kSegmentBytes + repo_.config.chunker.max_size);
  while (bytes < kSegmentBytes) {
    const int64_t t0 = clock.MonotonicNanos();
    auto next = reader.Next();
    const int64_t t1 = clock.MonotonicNanos();
    read_ns += t1 - t0;
    if (!next.ok()) {
      status = next.status().WithContext(path);
      break;
    }
    const std::optional<std::span<const std::byte>>& chunk = *next;
    if (!chunk.has_value()) {
      end = true;
      break;
    }
    bytes += chunk->size();
    if (copy) {
      s.data.insert(s.data.end(), chunk->begin(), chunk->end());
      s.lengths.push_back(static_cast<uint32_t>(chunk->size()));
      continue;
    }
    const Fingerprint fp = FingerprintOf(BlobType::kData, *chunk);
    const int64_t t2 = clock.MonotonicNanos();
    hash_ns += t2 - t1;
    status = PutIfNew(fp, BlobType::kData, *chunk, writer);
    store_ns += clock.MonotonicNanos() - t2;
    if (!status.ok()) break;
    s.refs.push_back({fp, static_cast<uint32_t>(chunk->size())});
  }
  read_ns_.fetch_add(static_cast<uint64_t>(read_ns), std::memory_order_relaxed);
  hash_ns_.fetch_add(static_cast<uint64_t>(hash_ns), std::memory_order_relaxed);
  store_ns_.fetch_add(static_cast<uint64_t>(store_ns), std::memory_order_relaxed);
  return status;
}

Status FileStorer::CollectOldest(std::deque<Run>& in_flight, Pieces& pieces,
                                 const std::string& path, size_t writer, const Status& status) {
  const Run oldest = std::move(in_flight.front());
  in_flight.pop_front();
  if (oldest.queued) {
    while (Segment* other = queue_.NextWhileWaiting(oldest.segment.get())) {
      StoreQueuedSegment(*other, writer);
    }
  }
  if (!status.ok()) return status;
  DDS_TRY(oldest.segment->status);
  const int64_t t0 = repo_.clock.MonotonicNanos();
  Status added;
  for (const ChunkRef& ref : oldest.segment->refs) {
    added = AddRef(pieces, ref, path, writer);
    if (!added.ok()) break;
  }
  store_ns_.fetch_add(static_cast<uint64_t>(repo_.clock.MonotonicNanos() - t0),
                      std::memory_order_relaxed);
  return added;
}

void FileStorer::StoreQueuedSegment(Segment& s, size_t writer) {
  s.status = StoreSegment(s, writer);
  queue_.MarkDone(&s);
}

Status FileStorer::StoreSegment(Segment& s, size_t writer) {
  // After a failure elsewhere, finish quickly: the backup will not commit.
  if (stop_) return Status::Aborted("backup stopped after a worker failed");
  const Clock& clock = repo_.clock;
  int64_t hash_ns = 0, store_ns = 0;
  s.refs.reserve(s.lengths.size());
  size_t at = 0;
  Status status;
  for (const uint32_t length : s.lengths) {
    const std::span<const std::byte> chunk(s.data.data() + at, length);
    at += length;
    const int64_t t0 = clock.MonotonicNanos();
    const Fingerprint fp = FingerprintOf(BlobType::kData, chunk);
    const int64_t t1 = clock.MonotonicNanos();
    hash_ns += t1 - t0;
    status = PutIfNew(fp, BlobType::kData, chunk, writer);
    store_ns += clock.MonotonicNanos() - t1;
    if (!status.ok()) break;
    s.refs.push_back({fp, length});
  }
  // The bytes are stored or failed: free them now, not when the splitter collects the refs.
  s.data = std::vector<std::byte>();
  hash_ns_.fetch_add(static_cast<uint64_t>(hash_ns), std::memory_order_relaxed);
  store_ns_.fetch_add(static_cast<uint64_t>(store_ns), std::memory_order_relaxed);
  return status;
}

void FileStorer::AddTimings(BackupTimings& t) const {
  t.read_chunk_ns += read_ns_.load(std::memory_order_relaxed);
  t.hash_ns += hash_ns_.load(std::memory_order_relaxed);
  t.store_ns += store_ns_.load(std::memory_order_relaxed);
}

}  // namespace dds
