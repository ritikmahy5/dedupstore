#pragma once

#include <cstddef>
#include <vector>

#include "dds/common/result.h"
#include "dds/hash/fingerprint.h"
#include "dds/snapshot/recipe.h"
#include "dds/snapshot/tree.h"
#include "dds/store/types.h"

namespace dds {

struct RepoInternals;

// Reads blobs by fingerprint for restore, verify, and `ls`: index lookup (without stamping: readers
// do not deduplicate), type check, the container read's CRC32C check, and, if `verify_hash`, a
// BLAKE3 check that the bytes are the content the fingerprint names (D33). A fingerprint missing
// from the index is NotFound; the caller decides whether that is corruption or a concurrent
// removal (ARCHITECTURE.md 4.2). Callers hold a ReadGuard from before Locate (or Read) until the
// read returns (6.3).
class BlobReader {
 public:
  BlobReader(RepoInternals& repo, bool verify_hash) : repo_(repo), verify_hash_(verify_hash) {}

  // Index lookup and type check, without reading.
  Result<Location> Locate(const Fingerprint& fp, BlobType type) const;
  // Reads the blob at `loc` (from Locate) and checks it.
  Result<std::vector<std::byte>> ReadAt(const Fingerprint& fp, const Location& loc);
  Result<std::vector<std::byte>> Read(const Fingerprint& fp, BlobType type);
  Result<Tree> ReadTree(const Fingerprint& fp);
  Result<Recipe> ReadRecipe(const Fingerprint& fp);

  uint64_t bytes_read() const { return bytes_read_; }

 private:
  RepoInternals& repo_;
  bool verify_hash_;
  uint64_t bytes_read_ = 0;
};

}  // namespace dds
