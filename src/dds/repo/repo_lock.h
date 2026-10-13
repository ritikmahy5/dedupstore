#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "dds/common/clock.h"
#include "dds/common/process_identity.h"
#include "dds/common/result.h"
#include "dds/fs/fs.h"
#include "dds/repo/repo_layout.h"

namespace dds {

// The contents of `lock` (ON_DISK_FORMAT.md section 4).
struct LockRecord {
  std::string host;
  uint64_t pid = 0;
  uint64_t pid_start = 0;
  int64_t acquired_at_ns = 0;
  std::string command;  // at most 512 bytes, no newline
};

std::string EncodeLockRecord(const LockRecord& record);
// Strict parse: the five keys in order, canonical decimals, a non-empty host. Corruption otherwise.
Result<LockRecord> DecodeLockRecord(std::string_view text);
// The command line as stored: newlines and carriage returns become spaces, truncated to 512 bytes.
std::string SanitizeLockCommand(std::string_view command);

// Holds a repository's lock (D16). One process owns a repository at a time.
//
// Acquire writes this process's record to a unique file in tmp/, syncs it, and links it to `lock`.
// link(2) fails if `lock` exists, and `lock` never exists without its full contents. If the lock is
// held: a holder on another host, an unparsable record, or a live holder gives Busy naming the
// holder; a dead holder on this host is taken over by renaming its `lock` away, checking that the
// renamed file is the record judged dead, and linking a fresh record. Release removes `lock` if it
// still holds this process's record. The destructor releases a lock not yet released.
class RepoLock {
 public:
  static Result<std::unique_ptr<RepoLock>> Acquire(Fs& fs, const RepoLayout& layout,
                                                   const ProcessIdentity& self, const Clock& clock,
                                                   std::string_view command);
  ~RepoLock();
  RepoLock(const RepoLock&) = delete;
  RepoLock& operator=(const RepoLock&) = delete;

  Status Release();
  const LockRecord& record() const { return record_; }

  // Only Acquire can make a Key, so only Acquire can construct a RepoLock (the constructor is
  // public so std::make_unique can call it).
  class Key {
    friend class RepoLock;
    Key() = default;
  };
  RepoLock(Key /*key*/, Fs& fs, RepoLayout layout, LockRecord record, std::string text)
      : fs_(fs), layout_(std::move(layout)), record_(std::move(record)), text_(std::move(text)) {}

 private:
  Fs& fs_;
  RepoLayout layout_;
  LockRecord record_;
  std::string text_;  // the encoded record, compared before removing `lock`
  bool held_ = true;
};

}  // namespace dds
