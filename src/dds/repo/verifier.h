#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dds/common/result.h"

namespace dds {

struct RepoInternals;

struct VerifyOptions {
  bool deep = false;  // also hash every reachable blob (SPEC.md section 5, check 5)
};

struct VerifyReport {
  std::vector<std::string> errors;    // each names a file, or a snapshot and a path
  std::vector<std::string> warnings;  // stray files in containers/ or snapshots/
  uint64_t snapshots = 0;
  uint64_t containers = 0;         // container files whose header, footer, and trailer passed
  uint64_t reachable_blobs = 0;    // distinct
  uint64_t unreachable_blobs = 0;  // space for the next gc, not an error
  uint64_t unreachable_bytes = 0;
  uint64_t duplicate_copies = 0;  // footer entries beyond each fingerprint's first (D32)
  uint64_t bytes_hashed = 0;      // with deep

  bool ok() const { return errors.empty(); }
};

// The checks of SPEC.md section 5. Problems found in the repository go into the report; the
// returned status is an error only if verification itself could not run (for example, snapshots/
// cannot be listed).
Result<VerifyReport> RunVerify(RepoInternals& repo, const VerifyOptions& options);

}  // namespace dds
