#include "dds/repo/backup_queue.h"

#include <utility>

#include "dds/common/check.h"

namespace dds {

BackupQueue::BackupQueue(size_t batch_capacity) : batch_capacity_(batch_capacity) {
  DDS_CHECK(batch_capacity >= 1);
}

bool BackupQueue::PushBatch(FileBatch batch) {
  MutexLock lock(mu_);
  while (batches_.size() >= batch_capacity_ && !closed_) room_.wait(lock);
  if (closed_) return false;
  batches_.push_back(std::move(batch));
  work_.notify_one();
  return true;
}

bool BackupQueue::TryPushSegment(Segment* s) {
  MutexLock lock(mu_);
  if (segments_.size() >= waiting_) return false;
  segments_.push_back(s);
  // Either an idle worker or a waiting splitter may take it; the splitters are few.
  work_.notify_one();
  splitters_.notify_all();
  return true;
}

std::optional<BackupQueue::Task> BackupQueue::Pop() {
  MutexLock lock(mu_);
  ++waiting_;
  // After Close, stay while a batch is being stored: one of its files may still be split.
  while (segments_.empty() && batches_.empty() && (!closed_ || active_ > 0)) work_.wait(lock);
  --waiting_;
  if (!segments_.empty()) {
    Segment* s = segments_.front();
    segments_.pop_front();
    return Task(s);
  }
  if (batches_.empty()) return std::nullopt;  // closed, empty, and every batch done
  FileBatch batch = std::move(batches_.front());
  batches_.pop_front();
  ++active_;
  room_.notify_one();
  return Task(std::move(batch));
}

Segment* BackupQueue::NextWhileWaiting(const Segment* mine) {
  MutexLock lock(mu_);
  ++waiting_;
  while (done_.count(mine) == 0 && segments_.empty()) splitters_.wait(lock);
  --waiting_;
  if (done_.erase(mine) != 0) return nullptr;
  Segment* s = segments_.front();
  segments_.pop_front();
  return s;
}

void BackupQueue::MarkDone(const Segment* s) {
  MutexLock lock(mu_);
  done_.insert(s);
  splitters_.notify_all();  // the one waiting for `s` among the few splitters
}

void BackupQueue::BatchDone() {
  MutexLock lock(mu_);
  DDS_CHECK(active_ > 0);
  --active_;
  if (active_ == 0) work_.notify_all();  // after Close, idle workers may now exit
}

bool BackupQueue::HasIdleThread() {
  MutexLock lock(mu_);
  return segments_.size() < waiting_;
}

void BackupQueue::Close() {
  MutexLock lock(mu_);
  closed_ = true;
  work_.notify_all();
  room_.notify_all();
}

}  // namespace dds
