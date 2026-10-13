#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "dds/chunk/chunker.h"
#include "dds/common/epoch.h"
#include "dds/common/ordered_mutex.h"
#include "dds/common/result.h"
#include "dds/common/thread_annotations.h"
#include "dds/fs/fs.h"
#include "dds/store/container_writer.h"
#include "dds/store/types.h"

namespace dds {

// The repository's containers: id allocation, the set of published containers with their sizes,
// and blob reads through a bounded cache of open file handles. Thread-safe.
//
// A container is "published" once every entry it holds is in the index (ARCHITECTURE.md 4.1):
// after open rebuilt the index from it, or after a session's Publish. Only published containers
// are GC candidates (D49). GC retires a container once it has moved or removed every entry that
// pointed at it; a retired container is never a candidate again, and ReclaimRetired unlinks it
// once no reader that could still read it remains (6.3, D21).
class ContainerSet final : public ContainerIdSource {
 public:
  // At most this many container files stay open for reads. macOS's default descriptor limit is
  // 256 per process, so the cache leaves room for everything else.
  static constexpr size_t kDefaultHandleCacheCapacity = 128;

  // `max_data_size` bounds a data blob's decompressed content (the repository's chunker max_size;
  // the default is the largest any repository allows).
  ContainerSet(Fs& fs, std::string containers_dir,
               size_t handle_cache_capacity = kDefaultHandleCacheCapacity,
               uint32_t max_data_size = kMaxChunkSizeLimit);

  uint32_t AllocateId() override;
  // Open sets this to 1 + the highest id among the container files present (ARCHITECTURE.md 7.6
  // step 6), so a new container never reuses a name.
  void SetNextId(uint32_t next_id);
  uint32_t NextId() const;

  void MarkPublished(uint32_t id, const ContainerInfo& info) DDS_EXCLUDES(mu_);
  std::vector<uint32_t> PublishedNotRetired() const DDS_EXCLUDES(mu_);
  std::optional<ContainerInfo> Info(uint32_t id) const DDS_EXCLUDES(mu_);

  // 4.3 step 6. `retire_epoch` comes from EpochManager::Advance, after the relocations.
  void Retire(uint32_t id, Epoch retire_epoch) DDS_EXCLUDES(mu_);
  size_t RetiredCount() const DDS_EXCLUDES(mu_);
  struct Reclaimed {
    uint64_t containers = 0;
    uint64_t file_bytes = 0;
  };
  // Unlinks every retired container whose retire epoch is <= `oldest_reader` (every one when
  // nobody is reading), drops its cached read handle, then syncs containers/. The unlinks happen
  // without the lock held. A container whose unlink fails is no longer tracked; the next open finds
  // it again, and its entries are duplicates for the next GC.
  Result<Reclaimed> ReclaimRetired(std::optional<Epoch> oldest_reader) DDS_EXCLUDES(mu_);

  struct Totals {
    uint64_t containers = 0;
    uint64_t file_bytes = 0;
    uint64_t payload_bytes = 0;
    uint64_t entries = 0;
  };
  Totals PublishedTotals() const DDS_EXCLUDES(mu_);

  // Reads the blob at `loc`, checks its CRC32C, and returns its content, decompressed if it is
  // stored as a zstd frame (D107). The file is opened outside the lock, and reads use a shared
  // handle, so readers of different containers never wait for each other's I/O.
  Result<std::vector<std::byte>> Read(const Location& loc) DDS_EXCLUDES(mu_);
  // The content length of the blob at `loc` (ReadContentLength): no read for a blob stored as
  // content.
  Result<uint32_t> ContentLength(const Location& loc) DDS_EXCLUDES(mu_);

  // containers/<8 hex>.ctr
  std::string PathFor(uint32_t id) const;

 private:
  Result<std::shared_ptr<ReadableFile>> Handle(uint32_t id) DDS_EXCLUDES(mu_);

  Fs& fs_;
  const std::string dir_;
  const size_t capacity_;
  const uint32_t max_data_size_;

  // Relaxed ordering is enough: all modifications of one atomic form a single total order, so an
  // id allocated after another (in happens-before order) is larger, which is all D12 needs
  // (ARCHITECTURE.md 5.2).
  std::atomic<uint32_t> next_id_{1};

  struct CachedHandle {
    std::shared_ptr<ReadableFile> file;
    std::list<uint32_t>::iterator lru_position;
  };
  mutable OrderedMutex mu_{lock_level::kContainers, "containers"};
  // Published containers, retired ones included until they are unlinked (they are still on disk).
  std::map<uint32_t, ContainerInfo> published_ DDS_GUARDED_BY(mu_);
  std::map<uint32_t, Epoch> retired_ DDS_GUARDED_BY(mu_);  // id -> retire epoch
  std::list<uint32_t> lru_ DDS_GUARDED_BY(mu_);  // container ids, most recently used first
  std::unordered_map<uint32_t, CachedHandle> handles_ DDS_GUARDED_BY(mu_);
};

}  // namespace dds
