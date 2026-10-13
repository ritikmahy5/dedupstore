#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "dds/fs/fs.h"

namespace dds::testing {

// Forwards every call to another Fs and counts OpenRead calls, so tests can see whether a cache
// avoided reopening a file.
class CountingFs final : public Fs {
 public:
  explicit CountingFs(Fs& base) : base_(base) {}
  int open_reads() const { return open_reads_.load(); }

  Result<std::unique_ptr<WritableFile>> CreateNew(const std::string& path) override {
    return base_.CreateNew(path);
  }
  Result<std::unique_ptr<ReadableFile>> OpenRead(const std::string& path) override {
    open_reads_.fetch_add(1);
    return base_.OpenRead(path);
  }
  Status Rename(const std::string& from, const std::string& to) override {
    return base_.Rename(from, to);
  }
  Status Link(const std::string& existing, const std::string& link) override {
    return base_.Link(existing, link);
  }
  Status Remove(const std::string& path) override { return base_.Remove(path); }
  Status CreateDir(const std::string& path) override { return base_.CreateDir(path); }
  Status SyncDir(const std::string& dir) override { return base_.SyncDir(dir); }
  Result<std::vector<std::string>> ListDir(const std::string& dir) override {
    return base_.ListDir(dir);
  }
  Result<bool> Exists(const std::string& path) override { return base_.Exists(path); }

 private:
  Fs& base_;
  std::atomic<int> open_reads_{0};
};

}  // namespace dds::testing
