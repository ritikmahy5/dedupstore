# dedupstore — replication protocol (version 2)

How a snapshot moves from a source repository (sender) to a destination repository (receiver).
The receiver runs `dds serve`; the sender runs `dds replicate`. The same code runs over TCP and
over the in-process `SimulatedLink` used by tests and benchmarks (D28).

## 1. Goals

- **Correct** (G8): the replica restores exactly as the source. Nothing on the wire is trusted.
- **Efficient** (G9): only blobs the receiver lacks cross the link. Queries cost 32 bytes per blob
  plus one reply bit. From version 2, a subtree the receiver already holds whole costs one query
  for its root tree, however many blobs lie below it (D108).
- **Latency tolerant:** up to `W` batches in flight, so throughput is bounded by bandwidth, not by
  round trips, once `W × batch size ≥ bandwidth × RTT` (BENCHMARKS.md E7 tests this).
- **Resumable:** after a disconnect, rerunning the same command transfers only what is missing.

## 2. Framing

Every message is one frame:

| Type | Field |
|---|---|
| u32 | length: bytes of `type` + `payload`, from 1 to 65 MiB (68,157,440). The whole frame is 8 bytes longer. |
| u8 | message type |
| bytes[length − 1] | payload |
| u32 | CRC32C of `type` + `payload` |

A frame whose length is out of range, whose CRC fails, or whose payload does not parse for its type
is a protocol error. The reader sends `ERROR(Corruption)` if it can and closes the connection.
Integers and strings follow ON_DISK_FORMAT.md section 1.

## 3. Messages

| Type | Name | Direction | Payload |
|---|---|---|---|
| 0x01 | HELLO | S→R | u16 protocol version (1 or 2); bytes[16] sender repo_id; bytes[16] params digest; str16 params text |
| 0x02 | HELLO_OK | R→S | u16 protocol version of the session; bytes[16] receiver repo_id |
| 0x10 | HAVE | S→R | u32 batch_id; u16 count (1–4096); count × fp |
| 0x11 | HAVE_REPLY | R→S | u32 batch_id; u16 count; ceil(count / 8) bytes bitmap, bit `i` (LSB first) = 1 if fp `i` is present; unused high bits of the last byte must be 0 |
| 0x12 | HAVE_TREES | S→R | version 2 only. As HAVE; every fp names a tree |
| 0x13 | HAVE_TREES_REPLY | R→S | as HAVE_REPLY; bit `i` = 1 if tree `i` and every blob below it are present (4.2 step 3) |
| 0x20 | PUT | S→R | u32 batch_id; u16 count (1–65,535); count × (u8 type, fp, u32 length, bytes[length]) — a 37-byte header per blob |
| 0x21 | PUT_ACK | R→S | u32 batch_id |
| 0x30 | COMMIT | S→R | the manifest file's bytes (ON_DISK_FORMAT.md section 8) |
| 0x31 | COMMIT_OK | R→S | bytes[16] snapshot id |
| 0x7F | ERROR | either | u16 status code (the explicit `StatusCode` values in ARCHITECTURE.md section 2, never `kOk`); str16 message (≤ 1024 bytes) |

- **Params text** is the `config` lines `hash` through `normalization` plus `gear_seed`, joined with
  `\n`. The **params digest** is the first 16 bytes of its BLAKE3. Different digests mean the two
  repositories chunk differently, so dedup between them is meaningless: the receiver replies
  `ERROR(Incompatible)` with both params texts in the message.
- A PUT carries at most 1 MiB of payload and at most 65,535 blobs; the sender starts a new PUT at
  whichever limit comes first. A single blob larger than 1 MiB (a large tree or recipe) is sent
  alone in its own PUT.
- A HAVE_REPLY whose count or batch_id does not match an outstanding HAVE, or whose padding bits are
  not 0, is a protocol error (`Corruption`). The same holds for a HAVE_TREES_REPLY and an
  outstanding HAVE_TREES, and for a HAVE_REPLY answering a HAVE_TREES or the other way round.
- **Versions.** The sender proposes a version in HELLO: 2 when it will prune subtrees, otherwise 1
  (`dds replicate --no-prune`). A receiver accepts 1 to 2 and answers with the proposed version,
  which the session then speaks; the sender accepts any version from 1 to the one it proposed. A
  receiver built before version 2 answers HELLO(2) with `ERROR(Incompatible)`; rerun with
  `--no-prune` (D108). HAVE_TREES in a version 1 session is a protocol error (`Corruption`).

## 4. Session

```
S: HELLO                     R: HELLO_OK
S: HAVE_TREES(1) ...         R: HAVE_TREES_REPLY(1) ...              (version 2: every tree, one pass)
S: HAVE(k) HAVE(k+1) ...     R: HAVE_REPLY(k) HAVE_REPLY(k+1) ...    (≤ W unanswered batches)
S: PUT(k) ...                R: PUT_ACK(k) ...
S: COMMIT(manifest)          R: COMMIT_OK(sid)
S closes.
```

### 4.1 Sender

1. Connect, send HELLO, wait for HELLO_OK or ERROR.
2. **Version 2: find the trees to walk.** Read every distinct tree of the snapshot, parents
   before children (a directory content that appears twice is one tree), and ask HAVE_TREES about
   all of them, in batches of up to 4096 within the window, each sent as soon as its trees are
   found, so the receiver never waits longer than one batch's reading. Then, from the root down: a
   tree
   answered whole is pruned, and nothing below it is read or queried; every other tree is kept for
   step 3, and its subdirectories are considered in turn. The step reads only tree blobs and costs
   32 bytes per tree and one round trip, whatever the depth of the changes.
3. Walk the snapshot's DAG from the root tree (version 2: from each tree kept in step 2, without
   descending into subdirectories, since step 2 kept or pruned each of them), depth-first,
   streaming: trees, then recipe pieces,
   then data chunks. Never hold the whole snapshot's fingerprint list in memory. Hold a
   `ReadGuard` per batch (ARCHITECTURE.md 6.3). Skip fingerprints already queried in this session,
   using a bounded in-memory set. A fingerprint evicted from that set may be queried again. If the
   second query reaches the receiver before the first PUT of that blob, the blob is sent twice,
   which the receiver tolerates (4.2 step 4).
4. Fill HAVE batches of up to 4096 fingerprints. For each reply, read the missing blobs from the
   local `ContainerSet` and send them in PUT batches.
5. Keep at most `W` batches unanswered (HAVE or HAVE_TREES without reply plus PUT without ack).
   Block when the window is full.
6. When the walk is done and every batch is answered, send COMMIT and wait for COMMIT_OK.
7. One thread writes, one thread reads replies. They share only the window: the outstanding-batch
   table, the HAVE_TREES answers, and the queue of missing fingerprints.

### 4.2 Receiver

Single-threaded per connection. Processes messages in order and replies in order.

1. HELLO: check the version (1 or 2) and params digest. Begin an `IngestSession`.
2. HAVE: for each fingerprint, present = an index hit through `session.Lookup` with reservation
   off (the hit stamps the entry, so a concurrent GC keeps it, ARCHITECTURE.md 6.2), or a blob
   this session has already received and holds in an unsealed writer. A miss reserves nothing,
   because the sender may never send that blob.
3. HAVE_TREES (version 2): for each tree, whole = the tree and every blob below it pass the checks
   of the COMMIT walk (step 5): present by step 2's rule, of the referenced type, trees and recipes
   parsable, chunk lengths as their recipes state. The receiver walks the subtree itself, with an
   explicit stack, and remembers for the rest of the session the answer for each tree it holds or
   that a tree it holds names, and each recipe that passed; a fingerprint that is neither leaves
   nothing behind, so a peer cannot grow the receiver's memory by asking. Every lookup goes through `session.Lookup`, so everything found is stamped and survives
   any GC until the session ends (ARCHITECTURE.md 6.2 item 3). A blob received in this session but
   not yet sealed has no index entry, so its type and length (and a tree's or recipe's content)
   cannot be checked: a subtree that needs one is answered "not whole", and the sender then
   queries its contents, which is only slower. The answer is checked, not assumed: no
   invariant ties a tree's presence to its descendants' (D29, D108).
4. PUT: for each blob, check type and length limits, compute `FingerprintOf(type, payload)`, and
   reject the whole connection with `ERROR(Corruption)` if it differs from the claimed fingerprint.
   A blob that is already present or already received in this session is then dropped. Otherwise
   `session.Put`. Reply PUT_ACK once the batch is processed. PUT_ACK means "received and verified",
   not "durable". Durability is promised only by COMMIT_OK.
5. COMMIT: parse the manifest and compute its id. If `snapshots/<sid>.snap` already exists, reply
   COMMIT_OK (idempotent). Otherwise seal and publish all writers. Then walk the snapshot from its
   root, finding every reachable blob with `session.Lookup` against the index only, so each one is
   stamped and protected from GC until the session ends (ARCHITECTURE.md 6.2 item 3). Check each
   blob's type and parse every tree and recipe (read under guards; the same checks as `verify`).
   A tree that HAVE_TREES found whole in this session, and a recipe it checked, are skipped: they
   were checked and stamped then, and the stamps hold until the session ends. Any missing or bad
   blob → `ERROR(Corruption)`, nothing committed. Then write the manifest byte
   for byte (ARCHITECTURE.md 7.3) and reply COMMIT_OK.
6. On disconnect or error before COMMIT: seal whatever the session's writers hold (those blobs are
   verified and durable after sealing), publish them, and end the session without a manifest.

## 5. Resumption

Rerunning `dds replicate` starts a new session from step 1. HAVE replies report the blobs that the
interrupted session sealed, so they are not sent again. Exception: if a GC ran on the receiver in
between, it may have reclaimed those blobs, because no committed snapshot referenced them and no
session protected them any more. They are then sent again. This is correct, only slower, and it is
recorded as D29's known cost.

## 6. Timeouts and limits

- **Idle timeout:** a side that is waiting for the peer with no request of its own outstanding
  closes the connection after 60 seconds.
- **Reply timeout:** a side waiting for the reply to its own outstanding HAVE, PUT, or COMMIT waits
  up to 10 minutes, because a large COMMIT walk can take minutes. While the receiver is processing,
  it sends nothing, and the sender is then waiting on a reply, so only the reply timeout applies.
- Both timeouts are configurable for tests.
- The receiver accepts up to 16 concurrent connections. Each is its own `IngestSession`.
- Frame size limit: 65 MiB (a 64 MiB metadata blob plus framing).

## 7. Not in version 2

- Falling back to version 1 by itself when a receiver built before version 2 refuses HELLO(2): the
  user reruns with `--no-prune`.
- Compression on the wire, authentication, encryption (tunnel over SSH), and multiple snapshots per
  session.
