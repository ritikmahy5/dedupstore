#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "dds/common/result.h"
#include "dds/common/unique_fd.h"
#include "dds/snapshot/mem_node.h"

namespace dds {

// Permission bits and modification time to apply to a restored entry.
struct EntryMeta {
  uint32_t mode = 0;
  int64_t mtime_sec = 0;
  uint32_t mtime_nsec = 0;
};

// Receives one restored file's content.
class FileSink {
 public:
  virtual ~FileSink() = default;
  virtual Status Write(std::span<const std::byte> data) = 0;
  // Sets the mode and times after the content is written (so a read-only mode cannot block the
  // writes), then closes the file.
  virtual Status Finish(const EntryMeta& meta) = 0;
};

// Where a restore creates its output (D24, D42). Creation never replaces anything: an existing
// name is AlreadyExists. Directories are created with mode 0700 and receive their recorded mode
// and times in FinishDir, after all their children, so a 0555 directory can still be filled
// (ARCHITECTURE.md 4.2). Names must be valid (IsValidName); anything else is InvalidArgument.
// Not thread-safe: one restore uses one sink.
class RestoreSink {
 public:
  using DirHandle = uint64_t;

  virtual ~RestoreSink() = default;
  // The destination directory: created if absent; if present it must be an empty directory,
  // otherwise AlreadyExists (SPEC.md section 3).
  virtual Result<DirHandle> OpenRoot() = 0;
  virtual Result<DirHandle> MakeDir(DirHandle parent, std::string_view name) = 0;
  virtual Result<std::unique_ptr<FileSink>> CreateFile(DirHandle parent, std::string_view name) = 0;
  // A symlink's times are set without following it; its mode is not restored (D54).
  virtual Status MakeSymlink(DirHandle parent, std::string_view name, std::string_view target,
                             const EntryMeta& meta) = 0;
  // Applies `meta` (if any; the root has none) and releases the handle.
  virtual Status FinishDir(DirHandle dir, const std::optional<EntryMeta>& meta) = 0;
};

// Restores into a real directory, every operation relative to its parent directory's descriptor
// (openat, mkdirat, symlinkat) and with O_NOFOLLOW on every open (D42). Nothing outside the
// destination can be created, modified, or followed, even through symlinks the same restore made
// earlier (G10).
class PosixRestoreSink final : public RestoreSink {
 public:
  explicit PosixRestoreSink(std::string dest) : dest_(std::move(dest)) {}
  Result<DirHandle> OpenRoot() override;
  Result<DirHandle> MakeDir(DirHandle parent, std::string_view name) override;
  Result<std::unique_ptr<FileSink>> CreateFile(DirHandle parent, std::string_view name) override;
  Status MakeSymlink(DirHandle parent, std::string_view name, std::string_view target,
                     const EntryMeta& meta) override;
  Status FinishDir(DirHandle dir, const std::optional<EntryMeta>& meta) override;

 private:
  struct OpenDir {
    UniqueFd fd;
    std::string path;  // for messages
  };
  Result<OpenDir*> Lookup(DirHandle dir);

  std::string dest_;
  std::unordered_map<DirHandle, OpenDir> dirs_;
  DirHandle next_handle_ = 1;
};

// Restores into memory (tests). Produces the same MemTree shape as MemSourceTree holds.
class MemRestoreSink final : public RestoreSink {
 public:
  Result<DirHandle> OpenRoot() override;
  Result<DirHandle> MakeDir(DirHandle parent, std::string_view name) override;
  Result<std::unique_ptr<FileSink>> CreateFile(DirHandle parent, std::string_view name) override;
  Status MakeSymlink(DirHandle parent, std::string_view name, std::string_view target,
                     const EntryMeta& meta) override;
  Status FinishDir(DirHandle dir, const std::optional<EntryMeta>& meta) override;

  const MemTree& nodes() const { return nodes_; }

 private:
  Result<std::string> Create(DirHandle parent, std::string_view name, MemNode node);

  MemTree nodes_;
  std::unordered_map<DirHandle, std::string> dirs_;  // handle -> path
  DirHandle next_handle_ = 1;
};

}  // namespace dds
