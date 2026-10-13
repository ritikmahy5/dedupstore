#include "dds/repo/index_checkpoint.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <thread>

#include "dds/common/check.h"
#include "dds/common/crc32c.h"
#include "dds/common/ordered_mutex.h"
#include "dds/common/strings.h"
#include "dds/common/thread_annotations.h"
#include "dds/repo/index_checkpoint_format.h"
#include "dds/repo/repository.h"

namespace dds {

namespace {

constexpr size_t kMaxLoadThreads = 16;
// Encoded shards are appended in chunks of about this size: one Append per shard would be 256
// small writes even for an empty repository.
constexpr size_t kAppendChunk = 1 << 20;

bool Contains(const std::vector<uint32_t>& sorted, uint32_t id) {
  return std::binary_search(sorted.begin(), sorted.end(), id);
}

// Shard `shard`'s entries that point into a covered container, encoded in fingerprint order. The
// shard lock is held only while copying entries out, never during I/O.
std::vector<std::byte> EncodeShard(const ChunkIndex& index, size_t shard,
                                   const std::vector<uint32_t>& covered, uint32_t* count) {
  std::vector<SealedEntry> entries;
  index.ForEachInShard(shard, [&](const IndexEntry& e) {
    if (Contains(covered, e.container_id)) entries.push_back({e.fp, e.location()});
  });
  std::sort(entries.begin(), entries.end(),
            [](const SealedEntry& a, const SealedEntry& b) { return a.fp < b.fp; });
  std::vector<std::byte> bytes;
  bytes.reserve(entries.size() * kCheckpointEntrySize);
  for (const SealedEntry& e : entries) AppendCheckpointEntry(e, bytes);
  *count = static_cast<uint32_t>(entries.size());
  return bytes;
}

// The entries and tables, appended to `file`; returns the covered ids.
Result<std::vector<uint32_t>> WriteContents(RepoInternals& in, WritableFile& file)
    DDS_REQUIRES(in.gc.mu) {
  CheckpointTables tables;
  std::vector<uint32_t> covered = in.containers.PublishedNotRetired();  // increasing ids
  for (uint32_t id : covered) {
    std::optional<ContainerInfo> info = in.containers.Info(id);
    // Only GC retires and unlinks containers, and the caller holds the GC mutex.
    DDS_CHECK(info.has_value());
    tables.containers.push_back({id, *info});
  }
  std::vector<std::byte> pending;
  for (size_t shard = 0; shard < kCheckpointShards; ++shard) {
    CheckpointShard& s = tables.shards[shard];
    const std::vector<std::byte> bytes = EncodeShard(in.index, shard, covered, &s.entry_count);
    s.crc32c = Crc32c(bytes);
    tables.entry_count += s.entry_count;
    pending.insert(pending.end(), bytes.begin(), bytes.end());
    if (pending.size() >= kAppendChunk) {
      DDS_TRY(file.Append(pending));
      pending.clear();
    }
  }
  // Read after the scan: the counter only increases, so it is above every covered id.
  tables.next_container_id = in.containers.NextId();
  const std::vector<std::byte> tail = EncodeCheckpointTail(tables, in.config.repo_id);
  pending.insert(pending.end(), tail.begin(), tail.end());
  DDS_TRY(file.Append(pending));
  return covered;
}

// Decodes shard `shard` and inserts its entries for containers in `used`.
Status LoadShard(RepoInternals& in, const ReadableFile& file, const CheckpointTables& tables,
                 size_t shard, const std::vector<uint32_t>& used, const std::string& path) {
  const CheckpointShard& s = tables.shards[shard];
  std::vector<std::byte> bytes(uint64_t{kCheckpointEntrySize} * s.entry_count);
  DDS_TRY(file.ReadAt(s.offset, bytes));
  std::vector<SealedEntry> entries;
  entries.reserve(s.entry_count);
  DDS_TRY(DecodeCheckpointShard(bytes, shard, tables, path, in.config.chunker.max_size, &entries));
  std::erase_if(entries, [&](const SealedEntry& e) { return !Contains(used, e.loc.container_id); });
  in.index.Reserve(shard, entries.size());
  for (const SealedEntry& e : entries) {
    // The index starts empty and the shard's entries are distinct (DecodeCheckpointShard), so a
    // fingerprint already present means the file and the index disagree.
    if (in.index.RebuildInsert(e)) {
      return Status::Corruption(
          StrCat(path, ": shard ", shard, ": ", e.fp.ToHex(), " is already in the index"));
    }
  }
  return Status::Ok();
}

// The first error of the loading threads.
struct FirstError {
  OrderedMutex mu{lock_level::kLeaf, "checkpoint.load"};
  Status status DDS_GUARDED_BY(mu);
};

// Every shard, on up to `threads` threads; the first error wins.
Status LoadShards(RepoInternals& in, const ReadableFile& file, const CheckpointTables& tables,
                  const std::vector<uint32_t>& used, size_t threads, const std::string& path) {
  if (threads == 0) threads = std::max<size_t>(1, std::thread::hardware_concurrency());
  threads = std::min(threads, kMaxLoadThreads);
  // Relaxed: each fetch_add only hands out a distinct shard number; join orders everything else.
  std::atomic<size_t> next_shard{0};
  FirstError first;
  auto work = [&] {
    for (size_t shard = next_shard.fetch_add(1, std::memory_order_relaxed);
         shard < kCheckpointShards; shard = next_shard.fetch_add(1, std::memory_order_relaxed)) {
      Status s = LoadShard(in, file, tables, shard, used, path);
      if (s.ok()) continue;
      MutexLock lock(first.mu);
      if (first.status.ok()) first.status = std::move(s);
      return;
    }
  };
  std::vector<std::thread> pool;
  for (size_t t = 1; t < threads; ++t) pool.emplace_back(work);
  work();
  for (std::thread& t : pool) t.join();
  MutexLock lock(first.mu);
  return first.status;
}

Result<CheckpointTables> ReadTables(const ReadableFile& file, const std::string& path,
                                    const RepoId& repo_id) {
  auto size = file.Size();
  DDS_TRY(size.status());
  if (*size < kCheckpointTrailerSize) {
    return Status::Corruption(StrCat(path, ": file is ", *size, " bytes, too small"));
  }
  std::vector<std::byte> trailer_bytes(kCheckpointTrailerSize);
  DDS_TRY(file.ReadAt(*size - kCheckpointTrailerSize, trailer_bytes));
  auto trailer = ParseCheckpointTrailer(trailer_bytes, *size, path, repo_id);
  DDS_TRY(trailer.status());
  std::vector<std::byte> table_bytes(trailer->tables_size);
  DDS_TRY(file.ReadAt(trailer->tables_offset, table_bytes));
  return DecodeCheckpointTables(table_bytes, *trailer, path);
}

}  // namespace

Result<std::vector<uint32_t>> WriteIndexCheckpoint(RepoInternals& in) {
  MutexLock gc_lock(in.gc.mu);
  const std::string tmp_path = JoinPath(in.layout.tmp(), "index.ckpt.tmp");
  // A failed earlier write in this process may have left it; open empties tmp/ anyway.
  Status removed = in.fs.Remove(tmp_path);
  if (!removed.ok() && removed.code() != StatusCode::kNotFound) return removed;
  auto file = in.fs.CreateNew(tmp_path);
  DDS_TRY(file.status());
  auto covered = WriteContents(in, **file);
  DDS_TRY(covered.status());
  DDS_TRY((*file)->Sync());
  DDS_TRY((*file)->Close());
  DDS_TRY(in.fs.Rename(tmp_path, in.layout.index_checkpoint()));
  DDS_TRY(in.fs.SyncDir(in.layout.root));
  return covered;
}

Result<LoadedCheckpoint> LoadIndexCheckpoint(RepoInternals& in, const std::vector<uint32_t>& listed,
                                             size_t threads) {
  const std::string path = in.layout.index_checkpoint();
  auto file = in.fs.OpenRead(path);
  DDS_TRY(file.status());
  auto tables = ReadTables(**file, path, in.config.repo_id);
  DDS_TRY(tables.status());
  LoadedCheckpoint loaded;
  loaded.next_container_id = tables->next_container_id;
  std::vector<const CheckpointContainer*> used;
  for (const CheckpointContainer& c : tables->containers) {
    if (Contains(listed, c.id)) {
      loaded.used.push_back(c.id);
      used.push_back(&c);
    }
  }
  DDS_TRY(LoadShards(in, **file, *tables, loaded.used, threads, path));
  // After the entries, as for a session's container (D49): a container is a GC candidate only once
  // every entry it holds is findable.
  for (const CheckpointContainer* c : used) in.containers.MarkPublished(c->id, c->info);
  return loaded;
}

Result<uint32_t> ReadCheckpointNextId(Fs& fs, const RepoLayout& layout, const RepoId& repo_id) {
  const std::string path = layout.index_checkpoint();
  auto file = fs.OpenRead(path);
  DDS_TRY(file.status());
  auto tables = ReadTables(**file, path, repo_id);
  DDS_TRY(tables.status());
  return tables->next_container_id;
}

Status RemoveIndexCheckpoint(Fs& fs, const RepoLayout& layout) {
  Status removed = fs.Remove(layout.index_checkpoint());
  if (removed.code() == StatusCode::kNotFound) return Status::Ok();
  DDS_TRY(removed);
  return fs.SyncDir(layout.root);
}

}  // namespace dds
