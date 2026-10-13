#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>

#include "dds/common/result.h"
#include "dds/repl/connection.h"
#include "dds/repl/frame.h"
#include "dds/snapshot/manifest.h"

namespace dds {

class Repository;

struct SenderOptions {
  size_t window = 8;                                // unanswered HAVE + PUT batches (D30)
  size_t have_batch = 4096;                         // fingerprints per HAVE (PROTOCOL.md 3)
  size_t put_payload_limit = 1 << 20;               // payload bytes per PUT (PROTOCOL.md 3)
  size_t queried_set_limit = 1 << 20;               // fingerprints remembered as already queried
  std::chrono::milliseconds reply_timeout{600000};  // PROTOCOL.md 6
  // Skip subtrees the receiver holds whole (protocol version 2, D108). Off, every blob is queried
  // and the session speaks version 1, which a receiver built before version 2 also accepts.
  bool prune_subtrees = true;
};

struct SendReport {
  SnapshotId id{};
  uint64_t fingerprints_queried = 0;
  uint64_t blobs_sent = 0;
  uint64_t payload_bytes_sent = 0;  // sum of PUT blob lengths
  uint64_t have_batches = 0;
  uint64_t put_batches = 0;
  uint64_t trees_checked = 0;    // trees asked about in HAVE_TREES (D108)
  uint64_t subtrees_pruned = 0;  // whole trees reached from the root: nothing below was queried
  std::map<MessageType, uint64_t> wire_bytes_sent;  // frames, by message type (E7)
  uint64_t wire_bytes_received = 0;
};

// Sends snapshot `id` of `repo` over `connection` (PROTOCOL.md 4.1): HELLO; with pruning, one
// HAVE_TREES pass over every tree of the snapshot (D108); then a streaming depth-first walk of the
// snapshot, or only of the trees not held whole (trees, recipe pieces, chunks), queried in HAVE
// batches, the missing blobs sent in PUT batches with at most `window` batches unanswered, then
// COMMIT. The calling thread walks and writes; a second thread reads replies; they share only the
// window. Returns when COMMIT_OK arrives, or the first error (an ERROR from the receiver arrives as
// its status code). Closes the connection either way.
Result<SendReport> Replicate(Repository& repo, const SnapshotId& id, Connection& connection,
                             const SenderOptions& options);

}  // namespace dds
