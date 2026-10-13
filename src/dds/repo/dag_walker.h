#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "dds/hash/fingerprint.h"
#include "dds/repo/blob_reader.h"
#include "dds/store/types.h"

namespace dds {

struct RepoInternals;

// Walks snapshot DAGs for `verify` and `stats` (SPEC.md section 5): every reachable blob must be in
// the index with the type it is referenced as, every tree and recipe must parse, every file's chunk
// lengths must add up to its size, and with `deep` every blob must hash to its fingerprint.
//
// Snapshots share most of their blobs, so each blob is checked once and what was found under it is
// remembered with paths relative to it (D69). A shared subtree is not walked again, but its
// problems are reported again under every path and snapshot that reaches it.
class DagWalker {
 public:
  DagWalker(RepoInternals& repo, bool deep) : repo_(repo), deep_(deep), reader_(repo, deep) {}

  // Appends "<path>: <problem>" for each problem under `root` ("/" is the snapshot root).
  void Walk(const Fingerprint& root, std::vector<std::string>& problems);

  // Every reachable blob found in the index, with its canonical location.
  const std::unordered_map<Fingerprint, Location, FingerprintHash>& reachable() const {
    return reachable_;
  }
  uint64_t bytes_hashed() const { return deep_ ? reader_.bytes_read() : 0; }
  // Over the distinct data chunks the walked recipes reference: their content bytes (the recipes'
  // lengths) and the bytes they take as stored, which compression makes smaller (D107).
  uint64_t data_content_bytes() const { return data_content_bytes_; }
  uint64_t data_stored_bytes() const { return data_stored_bytes_; }

 private:
  struct Problem {
    std::string path;  // relative to the tree it was found under; "" is that tree itself
    std::string message;
  };
  struct PieceResult {
    std::vector<std::string> problems;
    std::optional<uint64_t> bytes;  // total chunk bytes, if the piece itself could be read
  };

  const std::vector<Problem>& CheckTree(const Fingerprint& fp);
  std::vector<Problem> CheckEntries(const Fingerprint& fp);
  std::vector<std::string> CheckFile(const TreeEntry& e);
  const PieceResult& CheckPiece(const Fingerprint& fp);
  PieceResult ReadPiece(const Fingerprint& fp);
  // A data blob's problem, or nullopt if it is good.
  const std::optional<std::string>& CheckData(const Fingerprint& fp);

  // Locates `fp` with the expected type and records it as reachable.
  Result<Location> Reach(const Fingerprint& fp, BlobType type);
  // Reach, then read, under one ReadGuard.
  Result<std::vector<std::byte>> GuardedRead(const Fingerprint& fp, BlobType type);

  RepoInternals& repo_;
  const bool deep_;
  BlobReader reader_;
  std::unordered_map<Fingerprint, Location, FingerprintHash> reachable_;
  std::unordered_map<Fingerprint, uint32_t, FingerprintHash> data_content_;  // recipe lengths
  uint64_t data_content_bytes_ = 0;
  uint64_t data_stored_bytes_ = 0;
  // What each blob was found to hold. unordered_map never moves its elements, so references into
  // these stay valid while the recursion inserts more.
  std::unordered_map<Fingerprint, std::vector<Problem>, FingerprintHash> trees_;
  std::unordered_map<Fingerprint, PieceResult, FingerprintHash> pieces_;
  std::unordered_map<Fingerprint, std::optional<std::string>, FingerprintHash> data_;
  // Trees on the current path. A tree that contains itself is impossible for honest content (its
  // fingerprint would be a BLAKE3 fixed point), but a damaged index can claim one when hashes are
  // not checked, and following it would recurse forever.
  std::vector<Fingerprint> ancestors_;
};

}  // namespace dds
