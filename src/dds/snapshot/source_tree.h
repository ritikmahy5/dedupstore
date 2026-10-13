#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dds/chunk/chunk_reader.h"
#include "dds/common/result.h"
#include "dds/snapshot/mem_node.h"

namespace dds {

enum class SourceKind { kFile, kDirectory, kSymlink, kOther };

// What a backup needs to know about one directory entry.
struct SourceEntry {
  std::string name;
  SourceKind kind = SourceKind::kFile;
  uint32_t mode = 0;  // permission bits only
  int64_t mtime_sec = 0;
  uint32_t mtime_nsec = 0;
  uint64_t size = 0;  // files: the size when listed (the file may change while it is read)
};

// A directory tree to back up (D24): user data, read without going through Fs because its crash
// behavior is not part of the guarantees. Paths are relative to the tree's root, '/'-separated, and
// "" is the root. Implementations must be safe to call from several threads at once (backup
// workers read files in parallel).
class SourceTree {
 public:
  virtual ~SourceTree() = default;
  // The entries of directory `path`, sorted bytewise by name. Devices, FIFOs, and sockets appear
  // with kind kOther, so the backup can count them as skipped (SPEC.md section 4).
  virtual Result<std::vector<SourceEntry>> ListDir(const std::string& path) = 0;
  // The content of regular file `path`. Never follows a symlink.
  virtual Result<std::unique_ptr<ByteSource>> OpenFile(const std::string& path) = 0;
  // The target of symlink `path`, as stored, never resolved.
  virtual Result<std::string> ReadLink(const std::string& path) = 0;
};

// The file system under `root`.
class PosixSourceTree final : public SourceTree {
 public:
  explicit PosixSourceTree(std::string root) : root_(std::move(root)) {}
  Result<std::vector<SourceEntry>> ListDir(const std::string& path) override;
  Result<std::unique_ptr<ByteSource>> OpenFile(const std::string& path) override;
  Result<std::string> ReadLink(const std::string& path) override;

 private:
  std::string Full(const std::string& path) const;
  std::string root_;
};

// A tree held in memory (tests and generators). Names may be any bytes except '/' and NUL, so
// trees that a real file system would reject can be backed up and compared.
class MemSourceTree final : public SourceTree {
 public:
  MemSourceTree() = default;
  explicit MemSourceTree(MemTree nodes) : nodes_(std::move(nodes)) {}

  // Adders require the parent directory to exist.
  void AddDir(const std::string& path, uint32_t mode = 0755, int64_t sec = 0, uint32_t nsec = 0);
  void AddFile(const std::string& path, std::vector<std::byte> content, uint32_t mode = 0644,
               int64_t sec = 0, uint32_t nsec = 0);
  void AddSymlink(const std::string& path, std::string target, int64_t sec = 0, uint32_t nsec = 0);
  void AddSpecial(const std::string& path);

  const MemTree& nodes() const { return nodes_; }

  Result<std::vector<SourceEntry>> ListDir(const std::string& path) override;
  Result<std::unique_ptr<ByteSource>> OpenFile(const std::string& path) override;
  Result<std::string> ReadLink(const std::string& path) override;

 private:
  void CheckParent(const std::string& path) const;
  MemTree nodes_;  // written only while building, before any reader runs
};

}  // namespace dds
