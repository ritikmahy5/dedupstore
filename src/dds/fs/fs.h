#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dds/common/result.h"
#include "dds/common/status.h"

namespace dds {

// The file-system interface for repository storage (D24). Every repository file operation goes
// through it, so MemFs can stand in for fast tests and CrashSimFs can see every operation
// that matters for durability. User files being backed up or restored do not use Fs: they go
// through SourceTree and RestoreSink.
//
// Implementations: PosixFs (the real one), MemFs (in memory). The conformance suite in
// tests/conformance/ pins the semantics below; a difference from PosixFs is a bug in the model.
//
// Paths are absolute, '/'-separated, without a trailing slash. All implementations are
// thread-safe; distinct handles may be used from distinct threads concurrently.

// A file opened for appending, created by Fs::CreateNew. Not thread-safe: one owner.
class WritableFile {
 public:
  virtual ~WritableFile() = default;
  // Appends all of `data`.
  virtual Status Append(std::span<const std::byte> data) = 0;
  // When OK returns, everything appended so far is durable (assumption A1; D23).
  virtual Status Sync() = 0;
  // Releases the file. Destroying an unclosed file also releases it, silently.
  virtual Status Close() = 0;
};

// A file opened for reading. ReadAt is const and thread-safe.
class ReadableFile {
 public:
  virtual ~ReadableFile() = default;
  // Fills `out` exactly with the bytes at [offset, offset + out.size()). A range that extends
  // past the end of the file is an IoError, never a short read.
  virtual Status ReadAt(uint64_t offset, std::span<std::byte> out) const = 0;
  virtual Result<uint64_t> Size() const = 0;
};

class Fs {
 public:
  virtual ~Fs() = default;

  // Creates a new, empty file. AlreadyExists if `path` exists (O_EXCL); NotFound if its directory
  // does not exist.
  virtual Result<std::unique_ptr<WritableFile>> CreateNew(const std::string& path) = 0;
  // NotFound if `path` does not exist. A handle stays readable after its name is removed.
  virtual Result<std::unique_ptr<ReadableFile>> OpenRead(const std::string& path) = 0;
  // Atomically gives the file at `from` the name `to`, replacing any file there (A3). NotFound
  // if `from` does not exist.
  virtual Status Rename(const std::string& from, const std::string& to) = 0;
  // Gives the file at `existing` a second name. AlreadyExists if `link` exists, which makes a
  // fully written file + Link an atomic lock acquisition (D16).
  virtual Status Link(const std::string& existing, const std::string& link) = 0;
  // Removes a file's name. NotFound if absent; an error for a directory.
  virtual Status Remove(const std::string& path) = 0;
  // AlreadyExists if `path` exists; NotFound if its parent does not.
  virtual Status CreateDir(const std::string& path) = 0;
  // Makes every create, rename into, link into, and remove in `dir` durable (A2).
  virtual Status SyncDir(const std::string& dir) = 0;
  // Names (not paths) of the entries in `dir`, sorted bytewise, without "." and "..".
  virtual Result<std::vector<std::string>> ListDir(const std::string& dir) = 0;
  virtual Result<bool> Exists(const std::string& path) = 0;
};

// "a" + "/" + "b".
std::string JoinPath(std::string_view dir, std::string_view name);

// The directory containing `path`: "/a/b" -> "/a", "/a" -> "/", "a/b" -> "a", "a" -> ".".
std::string ParentDir(std::string_view path);

// `path` without trailing slashes ("/r/" -> "/r"); "/" stays "/".
std::string StripTrailingSlashes(std::string path);

// Reads a whole file through `fs`.
Result<std::vector<std::byte>> ReadWholeFile(Fs& fs, const std::string& path);

}  // namespace dds
