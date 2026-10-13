#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "dds/common/result.h"
#include "dds/repl/connection.h"
#include "dds/snapshot/manifest.h"

namespace dds {

class Repository;

struct ReceiverOptions {
  // PROTOCOL.md 6: with nothing of its own outstanding, the receiver waits this long for the
  // sender's next message, then closes.
  std::chrono::milliseconds idle_timeout{60000};
};

struct ReceiveReport {
  uint64_t fingerprints_queried = 0;
  uint64_t fingerprints_present = 0;
  uint64_t blobs_received = 0;
  uint64_t blobs_dropped = 0;  // received but already present (sent twice, PROTOCOL.md 4.2)
  uint64_t payload_bytes_received = 0;
  uint64_t trees_checked = 0;   // HAVE_TREES questions (D108)
  uint64_t subtrees_whole = 0;  // answered yes
  // Answers kept for the rest of the session: trees held here, or named by trees held here, never
  // a fingerprint a peer made up (D108).
  uint64_t trees_remembered = 0;
  std::optional<SnapshotId> committed;
};

// The receiving side of one replication connection (PROTOCOL.md 4.2): single-threaded, replies in
// order, one IngestSession for the connection. Nothing from the peer is trusted (G8): every blob
// must hash to its claimed fingerprint, and COMMIT writes the manifest only after walking the whole
// snapshot and finding every blob present with the right type and every tree and recipe parsable.
// A protocol error sends ERROR, closes the connection, and is returned. On any exit before COMMIT,
// whatever the session received is sealed and published, so a resumed transfer finds it.
// Returns when the sender closes the connection, or on an error.
Result<ReceiveReport> ServeReplication(Repository& repo, Connection& connection,
                                       const ReceiverOptions& options);

}  // namespace dds
