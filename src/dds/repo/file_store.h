#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dds/common/status.h"
#include "dds/hash/fingerprint.h"
#include "dds/repo/backup_queue.h"
#include "dds/snapshot/recipe.h"
#include "dds/store/types.h"

namespace dds {

struct RepoInternals;
struct BackupTimings;
class ChunkReader;
class IngestSession;
class SourceTree;

// A split file is handled in runs of whole chunks of at least this many bytes (the last run may be
// shorter), each stored by the splitting worker or copied into a segment for another (D109).
inline constexpr size_t kSegmentBytes = size_t{1} << 20;

// Stores regular files' content for one backup (ARCHITECTURE.md 4.1): chunks each file, stores the
// chunks the repository lacks, and writes its recipe pieces. A file of at least `split_bytes` is
// split (D109): the worker that took it reads and chunks it alone, so the boundaries are those
// of chunking it whole, and hands runs of its chunks to idle workers to fingerprint and store,
// storing the rest itself. Its recipe is the same either way, so a snapshot's root does not depend
// on splitting. Shared by the workers: every
// method may be called from any of them, each with its own writer.
class FileStorer {
 public:
  FileStorer(RepoInternals& repo, SourceTree& source, IngestSession& session, BackupQueue& queue,
             uint64_t split_bytes, size_t workers, const std::atomic<bool>& stop);

  // Stores `task`'s file through writer `writer`, filling its entry's size and pieces. `buffer` is
  // the worker's read buffer, lent to every file's ChunkReader so it is allocated once per worker.
  Status StoreFile(FileTask& task, size_t writer, std::vector<std::byte>& buffer);
  // Stores a segment another worker queued, through writer `writer`, and marks it done.
  void StoreQueuedSegment(Segment& s, size_t writer);
  // Looks the blob up (reserving on a miss) and stores it if it is new.
  Status PutIfNew(const Fingerprint& fp, BlobType type, std::span<const std::byte> bytes,
                  size_t writer);

  uint64_t files_split() const { return split_.load(std::memory_order_relaxed); }
  uint64_t segments_queued() const { return queued_.load(std::memory_order_relaxed); }
  uint64_t changed_while_reading() const { return changed_.load(std::memory_order_relaxed); }
  void AddTimings(BackupTimings& t) const;

 private:
  // The recipe pieces of one file, built in chunk order: a piece is written every
  // kMaxRefsPerRecipe references (D14), whichever path stored the chunks.
  struct Pieces {
    Recipe piece;
    std::vector<Fingerprint> list;
    uint64_t size = 0;
  };
  // One run of a split file's chunks, in file order: stored by the splitting worker as it read
  // them (`queued` false, `segment` holding only refs), or copied into `segment` and queued for
  // another worker, which must be waited for before its refs are read.
  struct Run {
    std::unique_ptr<Segment> segment;
    bool queued = false;
  };
  Status StoreWhole(FileTask& task, size_t writer, std::vector<std::byte>& buffer);
  Status StoreSplit(FileTask& task, size_t writer, std::vector<std::byte>& buffer);
  // Reads the next run of chunks, at least kSegmentBytes unless the file ends (`end`): copied into
  // `s` if `copy`, or else fingerprinted and stored now, their refs added to `s`.
  Status ReadRun(ChunkReader& reader, Segment& s, bool copy, const std::string& path, size_t writer,
                 bool& end);
  // Takes the oldest run, waiting for it if it was queued (storing queued segments meanwhile), and
  // adds its refs to `pieces` (only while `status` is OK: after an error it just waits).
  Status CollectOldest(std::deque<Run>& in_flight, Pieces& pieces, const std::string& path,
                       size_t writer, const Status& status);
  Status StoreSegment(Segment& s, size_t writer);
  Status AddRef(Pieces& pieces, const ChunkRef& ref, const std::string& path, size_t writer);
  Status FlushPiece(Pieces& pieces, const std::string& path, size_t writer);
  Status Finish(FileTask& task, Pieces& pieces, size_t writer);

  RepoInternals& repo_;
  SourceTree& source_;
  IngestSession& session_;
  BackupQueue& queue_;
  const uint64_t split_bytes_;
  const size_t max_in_flight_;  // runs of one file not yet collected
  const std::atomic<bool>& stop_;
  // Relaxed: statistics, read only after every worker has been joined.
  std::atomic<uint64_t> split_{0};
  std::atomic<uint64_t> queued_{0};
  std::atomic<uint64_t> changed_{0};
  std::atomic<uint64_t> read_ns_{0};
  std::atomic<uint64_t> hash_ns_{0};
  std::atomic<uint64_t> store_ns_{0};
};

}  // namespace dds
