#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include "dds/common/result.h"
#include "dds/fs/fs.h"
#include "dds/store/compression.h"
#include "dds/store/types.h"

namespace dds {

// Hands out container ids. ContainerSet implements it; ids increase in allocation order, which
// D12 relies on (a GC copy always gets a higher id than the container it copies from).
class ContainerIdSource {
 public:
  virtual ~ContainerIdSource() = default;
  virtual uint32_t AllocateId() = 0;
};

// Builds one container at a time and seals it durably (ARCHITECTURE.md 7.2). Owned by exactly one
// thread (each backup worker has its own, D31), so it has no lock.
//
//   ContainerWriter w(fs, containers_dir, tmp_dir, repo_id, ids, target_bytes);
//   DDS_TRY(w.Add(fp, type, payload));
//   if (w.Full()) { auto sealed = w.Seal(); ... publish sealed->entries ... }
//
// Blobs are buffered in memory and the file is written once, at Seal: a container is at most
// target_bytes plus one blob, and nothing reads a blob before it is sealed and published (D11).
// After a successful Seal the writer is empty and can build the next container.
class ContainerWriter {
 public:
  // With `compression` zstd, each blob is stored as a zstd frame when that is smaller (D107).
  ContainerWriter(Fs& fs, std::string containers_dir, std::string tmp_dir, const RepoId& repo_id,
                  ContainerIdSource& ids, uint64_t target_bytes,
                  const CompressionParams& compression = {});

  // Appends one blob, compressed if the writer compresses and that makes it smaller. The first Add
  // of each container allocates its id. Adding the same fingerprint twice to one container is a bug
  // (callers deduplicate first): it aborts.
  void Add(const Fingerprint& fp, BlobType type, std::span<const std::byte> payload);

  // True once the container file would be at least target_bytes (stored bytes, after compression).
  bool Full() const;
  bool Empty() const { return entries_.empty(); }
  size_t entry_count() const { return entries_.size(); }
  // The size the container file would have if sealed now.
  uint64_t FileBytes() const;

  // Writes tmp/<id>.ctr.tmp, syncs it, renames it to containers/<id>.ctr (failing with
  // Corruption rather than replacing an existing file), and syncs containers/. When OK returns the
  // container is durable; the caller then publishes the entries. Must not be called when Empty().
  Result<SealedContainer> Seal();

 private:
  Fs& fs_;
  std::string containers_dir_;
  std::string tmp_dir_;
  RepoId repo_id_;
  ContainerIdSource& ids_;
  uint64_t target_bytes_;
  std::optional<ZstdCompressor> compressor_;  // set when the repository compresses
  std::vector<std::byte> scratch_;            // the last blob's compressed bytes

  uint32_t id_ = 0;              // 0 until the first Add
  std::vector<std::byte> file_;  // header placeholder + payloads
  std::vector<SealedEntry> entries_;
  std::unordered_set<Fingerprint, FingerprintHash> fingerprints_;
};

}  // namespace dds
