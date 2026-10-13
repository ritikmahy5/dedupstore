#include "dds/store/container_set.h"

#include <limits>

#include "dds/common/test_hooks.h"
#include "dds/store/container_format.h"
#include "dds/store/container_reader.h"

namespace dds {

ContainerSet::ContainerSet(Fs& fs, std::string containers_dir, size_t handle_cache_capacity,
                           uint32_t max_data_size)
    : fs_(fs),
      dir_(std::move(containers_dir)),
      capacity_(handle_cache_capacity),
      max_data_size_(max_data_size) {
  DDS_CHECK(capacity_ >= 1);
}

uint32_t ContainerSet::AllocateId() {
  // Relaxed: see the comment on next_id_ (only the total order of modifications matters).
  const uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
  DDS_CHECK(id != 0 && id != std::numeric_limits<uint32_t>::max());  // 2^32 ids would wrap
  return id;
}

void ContainerSet::SetNextId(uint32_t next_id) {
  // Relaxed: called while opening, before the repository is shared with any other thread; starting
  // those threads publishes the value.
  next_id_.store(next_id, std::memory_order_relaxed);
}

uint32_t ContainerSet::NextId() const { return next_id_.load(std::memory_order_relaxed); }

void ContainerSet::MarkPublished(uint32_t id, const ContainerInfo& info) {
  MutexLock lock(mu_);
  published_[id] = info;
}

std::vector<uint32_t> ContainerSet::PublishedNotRetired() const {
  MutexLock lock(mu_);
  std::vector<uint32_t> ids;
  ids.reserve(published_.size());
  for (const auto& [id, info] : published_) {
    if (retired_.count(id) == 0) ids.push_back(id);
  }
  return ids;
}

void ContainerSet::Retire(uint32_t id, Epoch retire_epoch) {
  MutexLock lock(mu_);
  retired_[id] = retire_epoch;
}

size_t ContainerSet::RetiredCount() const {
  MutexLock lock(mu_);
  return retired_.size();
}

Result<ContainerSet::Reclaimed> ContainerSet::ReclaimRetired(std::optional<Epoch> oldest_reader) {
  std::vector<uint32_t> doomed;
  Reclaimed reclaimed;
  {
    MutexLock lock(mu_);
    for (auto it = retired_.begin(); it != retired_.end();) {
      // 6.3: a reader that entered before the retirement (entry < retire epoch) may still read
      // this container through a location it looked up earlier, so it blocks the unlink.
      const bool safe = !oldest_reader.has_value() || *oldest_reader >= it->second ||
                        DDS_MUTATION("retire_without_grace");
      if (!safe) {
        ++it;
        continue;
      }
      const uint32_t id = it->first;
      doomed.push_back(id);
      if (auto info = published_.find(id); info != published_.end()) {
        reclaimed.file_bytes += info->second.file_bytes;
        published_.erase(info);
      }
      if (auto handle = handles_.find(id); handle != handles_.end()) {
        lru_.erase(handle->second.lru_position);
        handles_.erase(handle);
      }
      it = retired_.erase(it);
    }
  }
  for (uint32_t id : doomed) {
    DDS_TRY(fs_.Remove(PathFor(id)));
    ++reclaimed.containers;
  }
  if (!doomed.empty()) DDS_TRY(fs_.SyncDir(dir_));
  return reclaimed;
}

std::optional<ContainerInfo> ContainerSet::Info(uint32_t id) const {
  MutexLock lock(mu_);
  auto it = published_.find(id);
  if (it == published_.end()) return std::nullopt;
  return it->second;
}

ContainerSet::Totals ContainerSet::PublishedTotals() const {
  MutexLock lock(mu_);
  Totals t;
  for (const auto& [id, info] : published_) {
    ++t.containers;
    t.file_bytes += info.file_bytes;
    t.payload_bytes += info.payload_bytes;
    t.entries += info.entry_count;
  }
  return t;
}

std::string ContainerSet::PathFor(uint32_t id) const {
  return JoinPath(dir_, ContainerFileName(id));
}

Result<std::vector<std::byte>> ContainerSet::Read(const Location& loc) {
  auto file = Handle(loc.container_id);
  DDS_TRY(file.status());
  return ReadBlob(**file, PathFor(loc.container_id), loc, max_data_size_);
}

Result<uint32_t> ContainerSet::ContentLength(const Location& loc) {
  if (loc.codec == Codec::kNone) return loc.length;
  auto file = Handle(loc.container_id);
  DDS_TRY(file.status());
  return ReadContentLength(**file, PathFor(loc.container_id), loc, max_data_size_);
}

Result<std::shared_ptr<ReadableFile>> ContainerSet::Handle(uint32_t id) {
  {
    MutexLock lock(mu_);
    auto it = handles_.find(id);
    if (it != handles_.end()) {
      lru_.splice(lru_.begin(), lru_, it->second.lru_position);
      return it->second.file;
    }
  }
  // Open without the lock: containers_mutex is not held across I/O, so a slow open does not
  // block readers of cached containers.
  auto opened = fs_.OpenRead(PathFor(id));
  DDS_TRY(opened.status());
  std::shared_ptr<ReadableFile> file = std::move(opened).value();
  MutexLock lock(mu_);
  auto it = handles_.find(id);
  if (it != handles_.end()) return it->second.file;  // another reader opened it meanwhile
  lru_.push_front(id);
  handles_.emplace(id, CachedHandle{file, lru_.begin()});
  // Evicting only drops the cache's reference: a reader holding the shared_ptr keeps reading.
  while (handles_.size() > capacity_) {
    handles_.erase(lru_.back());
    lru_.pop_back();
  }
  return file;
}

}  // namespace dds
