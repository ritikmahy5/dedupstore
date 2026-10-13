#pragma once

// Internal to sender.cc: the state its two threads share.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "dds/common/ordered_mutex.h"
#include "dds/common/result.h"
#include "dds/common/strings.h"
#include "dds/common/thread_annotations.h"
#include "dds/hash/fingerprint.h"
#include "dds/repl/messages.h"
#include "dds/snapshot/manifest.h"

namespace dds {

// What the sender's writer and reader threads share (PROTOCOL.md 4.1 step 7): the outstanding
// batches, which at most `capacity` may be, the missing blobs the replies found, the answers to
// HAVE_TREES, the first error, and the committed id. Internal to sender.cc.
class Window {
 public:
  explicit Window(size_t capacity) : capacity_(capacity) {}

  // Blocks until a batch may be sent. False once the session failed.
  bool WaitForFreeSlot() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    while (outstanding_.size() >= capacity_ && error_.ok()) cv_.wait(lock);
    return error_.ok();
  }
  enum class Wake { kSlot, kMissing, kError };
  // Blocks until there are missing blobs to send (checked first: answering the receiver's replies
  // keeps the pipeline moving), a free slot, or an error.
  Wake WaitForSlotOrWork() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    while (outstanding_.size() >= capacity_ && missing_.empty() && error_.ok()) cv_.wait(lock);
    if (!error_.ok()) return Wake::kError;
    return missing_.empty() ? Wake::kSlot : Wake::kMissing;
  }
  void Sent(uint32_t batch, std::vector<Fingerprint> have_fps) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    outstanding_.emplace(batch, Outstanding{Kind::kHave, std::move(have_fps)});
  }
  void SentPut(uint32_t batch) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    outstanding_.emplace(batch, Outstanding{Kind::kPut, {}});
  }
  void SentTrees(uint32_t batch, std::vector<Fingerprint> trees) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    outstanding_.emplace(batch, Outstanding{Kind::kTrees, std::move(trees)});
  }
  // Reader: a HAVE_REPLY. Frees the HAVE's slot and queues its missing fingerprints in one
  // critical section, so the writer never sees the batch answered without its missing blobs queued
  // (it would then commit without sending them).
  Status HaveAnswered(const HaveReply& reply) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    auto it = outstanding_.find(reply.batch_id);
    if (it == outstanding_.end() || it->second.kind != Kind::kHave) {
      return Status::Corruption(
          StrCat("HAVE_REPLY for batch ", reply.batch_id, ", which is not an outstanding HAVE"));
    }
    const std::vector<Fingerprint>& fps = it->second.fps;
    if (fps.size() != reply.present.size()) return Status::Corruption("HAVE_REPLY count");
    std::vector<Fingerprint> missing;
    for (size_t i = 0; i < reply.present.size(); ++i) {
      if (!reply.present[i]) missing.push_back(fps[i]);
    }
    outstanding_.erase(it);
    if (!missing.empty()) missing_.push_back(std::move(missing));
    cv_.notify_all();
    return Status::Ok();
  }
  // Reader: a HAVE_TREES_REPLY (protocol version 2, D108). Frees the slot and keeps the answers
  // for the writer, which waits for them.
  Status TreesAnswered(const HaveReply& reply) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    auto it = outstanding_.find(reply.batch_id);
    if (it == outstanding_.end() || it->second.kind != Kind::kTrees) {
      return Status::Corruption(StrCat("HAVE_TREES_REPLY for batch ", reply.batch_id,
                                       ", which is not an outstanding HAVE_TREES"));
    }
    if (it->second.fps.size() != reply.present.size()) {
      return Status::Corruption("HAVE_TREES_REPLY count");
    }
    tree_answers_.emplace(reply.batch_id, reply.present);
    outstanding_.erase(it);
    cv_.notify_all();
    return Status::Ok();
  }
  // Writer: blocks until HAVE_TREES batch `batch` is answered, then returns its answers.
  Result<std::vector<bool>> WaitForTrees(uint32_t batch) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    while (tree_answers_.count(batch) == 0 && error_.ok()) cv_.wait(lock);
    if (!error_.ok()) return error_;
    std::vector<bool> answers = std::move(tree_answers_.at(batch));
    tree_answers_.erase(batch);
    return answers;
  }
  // Reader: a PUT_ACK.
  Status PutAnswered(uint32_t batch) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    auto it = outstanding_.find(batch);
    if (it == outstanding_.end() || it->second.kind != Kind::kPut) {
      return Status::Corruption(
          StrCat("PUT_ACK for batch ", batch, ", which is not an outstanding PUT"));
    }
    outstanding_.erase(it);
    cv_.notify_all();
    return Status::Ok();
  }
  std::optional<std::vector<Fingerprint>> TakeMissing() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    if (missing_.empty()) return std::nullopt;
    auto fps = std::move(missing_.front());
    missing_.pop_front();
    return fps;
  }
  // Blocks while batches are outstanding and nothing waits to be sent. False on an error.
  bool WaitForQuietOrWork() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    while (!outstanding_.empty() && missing_.empty() && error_.ok()) cv_.wait(lock);
    return error_.ok();
  }
  bool Quiet() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    return outstanding_.empty() && missing_.empty();
  }
  void Fail(const Status& s) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    if (error_.ok()) error_ = s;
    cv_.notify_all();
  }
  Status error() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    return error_;
  }
  void CommitOk(const SnapshotId& id) DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    committed_ = id;
    cv_.notify_all();
  }
  // Blocks until COMMIT_OK or an error. Returns the committed id.
  Result<SnapshotId> WaitForCommit() DDS_EXCLUDES(mu_) {
    MutexLock lock(mu_);
    while (!committed_.has_value() && error_.ok()) cv_.wait(lock);
    if (!error_.ok()) return error_;
    if (!committed_.has_value()) return Status::Aborted("no COMMIT_OK");  // the loop ends on one
    return *committed_;
  }

 private:
  enum class Kind { kHave, kTrees, kPut };
  struct Outstanding {
    Kind kind;
    std::vector<Fingerprint> fps;  // what a HAVE or HAVE_TREES asked about; empty for a PUT
  };

  const size_t capacity_;
  OrderedMutex mu_{lock_level::kLeaf, "sender.window"};
  std::condition_variable_any cv_;                                          // waited on with mu_
  std::map<uint32_t, Outstanding> outstanding_ DDS_GUARDED_BY(mu_);         // by batch id
  std::map<uint32_t, std::vector<bool>> tree_answers_ DDS_GUARDED_BY(mu_);  // answered HAVE_TREES
  std::deque<std::vector<Fingerprint>> missing_ DDS_GUARDED_BY(mu_);        // to send as PUTs
  Status error_ DDS_GUARDED_BY(mu_);
  std::optional<SnapshotId> committed_ DDS_GUARDED_BY(mu_);
};

}  // namespace dds
