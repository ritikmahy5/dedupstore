#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

#include "dds/common/check.h"
#include "dds/common/ordered_mutex.h"
#include "dds/common/thread_annotations.h"

namespace dds {

// A bounded multi-producer, multi-consumer FIFO (ARCHITECTURE.md 4.1). Push blocks while the queue
// is full, which slows a producer to the consumers' pace so memory stays flat; Pop blocks while it
// is empty. Close() wakes everyone: later Pushes return false, and Pops return what is left, then
// nullopt. Header-only because it is a template; the mutex is a leaf (5.3), and nothing else is
// locked while it is held.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(size_t capacity) : capacity_(capacity) { DDS_CHECK(capacity >= 1); }
  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  // False, and `item` dropped, if the queue was closed before there was room.
  bool Push(T item) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    while (items_.size() >= capacity_ && !closed_) not_full_.wait(lock);
    if (closed_) return false;
    items_.push_back(std::move(item));
    not_empty_.notify_one();
    return true;
  }

  // The oldest item, or nullopt once the queue is closed and empty.
  std::optional<T> Pop() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    while (items_.empty() && !closed_) not_empty_.wait(lock);
    if (items_.empty()) return std::nullopt;
    T item = std::move(items_.front());
    items_.pop_front();
    not_full_.notify_one();
    return item;
  }

  void Close() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    closed_ = true;
    not_full_.notify_all();
    not_empty_.notify_all();
  }

 private:
  const size_t capacity_;
  OrderedMutex mu_{lock_level::kLeaf, "queue"};
  std::condition_variable_any not_full_;   // waited on with mu_
  std::condition_variable_any not_empty_;  // waited on with mu_
  std::deque<T> items_ DDS_GUARDED_BY(mu_);
  bool closed_ DDS_GUARDED_BY(mu_) = false;
};

}  // namespace dds
