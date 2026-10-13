# dedupstore — testing

The guarantees in SPEC.md section 2 are only as good as the tests that try to break them. This
document says which tests exist, what each proves, and the rules every test follows.

## 1. Rules

- GoogleTest for C++ tests; Google Benchmark for microbenchmarks (D34).
- Every randomized test takes its seed from `DDS_TEST_SEED` if set, otherwise from a fixed list,
  and prints the seed in its failure message, so every failure can be replayed.
- No `sleep` to wait for other threads. Use latches, condition variables, or `join`.
- Tests that need wall-clock time (timeouts, the simulated link) take an injectable `Clock`, or
  bound their waits generously and assert on ordering, never on exact durations.
- A flaky test is a bug in the code or the test. Fix it. Never retry or disable it.
- ctest labels: `unit`, `conformance`, `codec`, `property`, `model`, `crash`, `stress`, `repl`,
  `cli`, `fuzz-replay`, `slow`. `ctest --preset <p>` runs everything except `slow`. Before a
  feature is declared done, `ctest --preset debug-slow` (only the `slow` label) also passes. Each
  test carries exactly one label, its family or `slow`; the plain presets exclude `slow` (D60).
- The default (non-`slow`) suite finishes in under 2 minutes in the `debug` preset on the
  development laptop. Longer runs get the `slow` label.

## 2. Build presets

| Preset | Compiler flags | Purpose |
|---|---|---|
| `debug` | `-O0 -g`, lock-order checker on, `DDS_ENABLE_TEST_HOOKS` | Default development loop. |
| `release` | `-O2 -g`, no checker, no test hooks | Benchmarks: the measured binary. `DDS_CHECK` stays on. |
| `asan` | `-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer`, checker on, test hooks | Memory errors and undefined behavior (fatal, D60). |
| `tsan` | `-O1 -g -fsanitize=thread`, checker on, test hooks | Data races. |
| `fuzz` | clang only, `-fsanitize=fuzzer,address,undefined`, test hooks | libFuzzer targets. Configured only if the compiler supports `-fsanitize=fuzzer` (Apple clang does not; use Homebrew LLVM or a Linux clang, D34). |
| `tidy` | clang with `-Wthread-safety -Werror` and clang-tidy (`--warnings-as-errors=*`) | Static checks. Every clang build also has `-Wthread-safety`. |
| `debug-slow`, `asan-slow`, `tsan-slow` | test presets only, on the matching build | Run only the `slow` label (D60). |

Tests that need test hooks (sync points, mutations) or the lock-order checker call `GTEST_SKIP()`
with a reason in presets that lack them, so every preset's suite still passes.

CI (`.github/workflows/ci.yml`) runs `debug`, `asan`, and `tsan` with GCC and clang on Ubuntu,
`debug` and `asan` with Apple clang on macOS, the `tidy` preset, and every fuzz target for 60
seconds on Ubuntu clang. CI runs on every push and pull request. Locally, the same presets are
the definition of "passes".

## 3. Test families

### 3.1 Unit tests (`tests/unit/`)

One test file per class. Each public behavior and each error path has a test. Examples: `Status`
and `Result` propagation; encoding round trips at integer boundaries; CRC32C check value
`0xE3069283` for `"123456789"`; `FlatTable` growth, probing across the wrap-around, and
backward-shift deletion; `ContainerWriter` sealing exactly at and just past the target size.
Large files split across workers (D109, `backup_split_test.cc`): the same root, logical bytes, and
blobs written as without splitting, for two large files beside small ones (one repeating itself,
so its chunks dedup across segments), twelve large files at once, and a file of one segment; one
worker never splits; a read error part way through a split file fails the backup and commits
nothing; a split file that changed size is stored as read. `BackupQueue` alone: a segment queues
only for an idle thread, a waiting splitter stores queued segments until its own is done, and
workers stay after Close until every batch taken is done. The GC stress test (3.7) splits files of 64
KiB and more.

### 3.2 Fs conformance (`tests/conformance/`)

One parameterized suite runs against `PosixFs` (in a temporary directory), `MemFs`, and
`CrashSimFs` with no crash injected. It pins the semantics the design relies on: `CreateNew` fails
if the file exists; `Link` fails with `AlreadyExists` if the target exists; `Rename` replaces
atomically; reads past the end fail; `ListDir` returns names only. A behavior difference between
implementations is a bug in `MemFs` or `CrashSimFs`, which must model `PosixFs`.

### 3.3 Codecs (`tests/codec/` and `fuzz/`)

For every decoder (config, lock, container header, footer and trailer, index checkpoint, tree,
recipe, manifest, protocol frame, each protocol message):

1. **Round trip:** encode → decode → equal, over seeded random valid values, including empty and
   maximum-size fields.
2. **Golden bytes:** a small hand-checked fixture in `tests/fixtures/` whose bytes are written out
   in the test with a comment per field. It catches accidental format changes.
3. **Every single-bit corruption:** for a small valid instance, flip every bit of every byte, one
   at a time, and decode.
   - Checksummed structures (config, container header + footer + trailer decoded as one unit,
     index checkpoint, manifest, protocol frame): the decoder must return `Corruption` (or `Incompatible` for a
     version field). It must never crash and never return success.
   - Structures without their own checksum (lock record, tree and recipe payloads, message payloads
     inside a valid frame): the decoder must not crash, and must return either `Corruption` or a
     valid value that re-encodes to the flipped bytes. Their integrity comes from the CRC and BLAKE3
     layer around them, which the checksummed cases and `verify --deep` tests cover.
4. **Canonical encoding (fuzz):** a libFuzzer target feeds arbitrary bytes to the decoder. If
   decoding succeeds, re-encoding must reproduce the input exactly. Seed corpora live in
   `fuzz/corpus/<target>/`. The `fuzz-replay` label replays each corpus through a plain driver in
   every preset, so compilers without libFuzzer still run the corpus. Decompressing a stored zstd
   frame (D107) has a fuzz target too (`fuzz_decompress`), without the canonical property, since
   many frames decode to one content: it must return content within the limit or `Corruption`.
   Container and config codecs keep golden tests for version 1, which existing repositories hold.

### 3.4 Chunker properties (`tests/property/`)

- Every chunk length is in `[min, max]`, except the last, which is in `[1, max]`.
- The concatenation of the chunks equals the input.
- **Feeding independence:** feeding the same stream through `ChunkReader` with buffer splits of 1
  byte, prime sizes, and random sizes yields identical boundaries.
- **Shift resistance:** insert one byte into the middle of 16 MiB of seeded random data. With
  FastCDC, at least 95% of the original bytes are still in unchanged chunks. With the fixed
  chunker, everything after the insertion point changes. Both assertions are kept, because the
  contrast is the point.
- **Golden boundaries:** a fixed 1 MiB seeded input has its boundary list in a fixture, generated
  once, with the chunker, and reviewed. Any change to the gear table, masks, or algorithm breaks
  it, and with it every existing repository's dedup. Changing the fixture requires a decision in DECISIONS.md.
- The gear table test regenerates the table from `gear_seed` and compares it with the committed
  constant.

### 3.5 Model-based tests (`tests/model/`)

- **FlatTable vs `std::unordered_map`:** one million seeded random operations (insert, lookup,
  erase, relocate) on both. Results must agree after every operation.
- **Repository vs reference model:** a seeded generator produces directory trees (sizes from 0 B
  to several MiB; shared content across files and across versions; empty directories; symlinks;
  non-ASCII names). Names may be arbitrary bytes only when the trees go through `MemSourceTree` and
  `MemRestoreSink`. Trees written to a real file system use valid UTF-8 names with no two names in
  one directory equal after case folding, because macOS volumes reject other bytes and are
  usually case-insensitive. A random sequence of backup, rm, gc, restore, verify, and
  close/reopen runs against a `MemFs` repository and against a model (a map from snapshot id to
  expected tree contents). After each step, every live snapshot restores to exactly its model
  tree, and `verify` passes.

### 3.6 Crash consistency (`tests/crash/`) — G2, G3

`CrashSimFs` wraps the `MemFs` model and records, per file, the synced content and the unsynced
appends, and per directory, the creates, renames, and removes since its last `SyncDir`.

- **Crash injection:** the harness runs a scenario (init; backup; backup; rm; gc; ...) once to
  count the mutating `Fs` calls, `K`. For each `i` in `0..K`, it reruns the scenario with the Fs
  failing every call after call `i` with `IoError`. That stops the process logic at that point, as
  a kill would.
- **Crash states:** for each crash point and each of 20 seeds, materialize the post-crash disk
  that A1–A3 allow (ARCHITECTURE.md 7.1). Each file keeps its synced content plus a seeded random
  prefix of its unsynced appends (possibly none, possibly all). Directory operations not yet made
  durable by a `SyncDir` survive or vanish independently (seeded), applied in their original order,
  where an operation whose source no longer exists is dropped:
  - a create, a `Link`, or a remove is pending in its own directory;
  - a `Rename` is one pending operation, made durable by `SyncDir` of the **target** directory. If
    it vanishes, the file is still under its old name (if that name was durable), so A3's "old name
    or new name, never both or neither" holds even across directories (`tmp/` to `containers/`).
- **Process identity:** the harness opens the post-crash repository with a fake `ProcessIdentity`
  that reports the crashed process as dead, so a surviving `lock` file is taken over, as it would
  be after a real crash.
- **Invariants, checked on every crash state:** the repository opens (for the init scenario:
  either it opens as an empty repository, or `Open` returns `NotFound` and running `init` again
  succeeds); `verify` reports no errors; `tmp/` is empty after open; every snapshot whose commit
  returned before the crash point restores exactly; the snapshot in flight at the crash is either
  absent or restores exactly.
- **Scenarios:** init alone; one backup; backup after backup (dedup hits); rm; GC with deletions
  and compactions; replication receiver commit; index checkpoints written by `Close`, loaded by the
  next open, then GC deleting and compacting containers they cover (D104). Each reports the number
  of crash states explored. The checkpoint scenario has no mutation: a checkpoint that did not
  survive a crash fails its CRCs and is removed, so no ordering of its write affects correctness.
  `IndexCheckpointTest.IgnoringTheCheckpointsNextIdLosesNewData` gives its one correctness rule,
  never reusing a covered container id, its teeth instead (mutation `checkpoint_next_id_ignored`).
- **Teeth (mutation checks):** presets with `DDS_ENABLE_TEST_HOOKS` compile `DDS_MUTATION` hooks
  (ARCHITECTURE.md section 10). Each named mutation breaks one ordering in the real code, selected
  at run time by the test. The harness must report a violation for each:
  - `manifest_before_seal`: write the manifest before sealing the session's containers.
  - `skip_container_dir_sync`: skip `SyncDir(containers/)` after renaming a container.
  - `gc_unlink_before_copy_sync`: unlink a compacted container before its copies are sealed.

  If a mutation goes undetected, the harness is too weak, and that is a failing test. The
  `release` preset does not define `DDS_ENABLE_TEST_HOOKS`, and the hooks compile to nothing.

### 3.7 Concurrency stress (`tests/stress/`) — G5, G6, G11

Run under the `tsan` preset (and `debug`), labeled `stress`, with a `slow` long version.

- **Deterministic interleavings:** each race that ARCHITECTURE.md section 6 prevents gets a test
  that forces it with `DDS_SYNC_POINT` latches, passes with the rule in place, and fails every time
  with the rule's mutation switched on:
  - *dedup during GC:* pause GC at `gc.after_plan`; a backup gets a lookup hit on a planned-dead
    entry and commits; resume GC. The snapshot must restore. Mutation `no_epoch_protection` (GC
    ignores `last_ref`) must make it fail.
  - *sealed but unpublished container:* pause a backup worker at `seal.before_publish`; run a full
    GC; resume; commit. The snapshot must restore. Mutation `candidate_before_publish` (candidates
    are all sealed containers) must make it fail.
  - *reader during retire:* pause a restore at `read.after_lookup`; let GC relocate and retire the
    container; resume. The restore must succeed. Mutation `retire_without_grace` (unlink without
    waiting for readers) must make it fail with a missing container.
  - *reuse from a parent during GC (D106):* pause a backup with a parent at `backup.after_reuse`,
    once it has looked up a reused file's recipe and chunks; remove the parent and run a full GC;
    resume. The snapshot must restore. Mutation `reuse_without_stamp` (the lookups check the index
    without stamping) must make it fail.
  - *subtree found whole, then GC (D108):* a scripted sender asks HAVE_TREES about a subtree the
    receiver holds only as garbage; a full GC runs before COMMIT. The snapshot must restore.
    Mutation `subtree_check_without_stamp` must make it fail (section 3.8).
  - *stale stamp:* force an older `now` to be applied after a newer one. `last_ref` must not
    decrease. Mutation `stamp_assign` (assign instead of max) must make the dedup-during-GC test
    above fail.
- **Ingest + GC + restore (random):** 4 backup threads (seeded trees sharing much content), 2
  threads restoring random committed snapshots and comparing with the model, 1 thread running `rm`
  on random old snapshots, 1 thread running GC in a loop with threshold 0.9 (maximum churn), and 1
  thread writing index checkpoints until GC stops (D104). Run for a fixed number of operations, not
  a duration. A restore may end in `Aborted` only if its snapshot was removed while it ran, and the
  test checks that. At the end, every committed and not removed snapshot restores exactly, and
  `verify` passes, both in the running repository and after reopening it from the last checkpoint. This test complements the deterministic
  ones: it looks for races nobody thought of.
- **Duplicate accounting (G5):** 4 sessions back up the same new tree in lockstep, so duplicates
  are certain. `stats` must report a duplicate count equal to the number of (fingerprint,
  container) footer entries minus the number of distinct fingerprints, computed independently by
  the test. After a GC at threshold 1.0 the count is 0 and every snapshot still restores.
- **Determinism (G11):** back up the same generated tree with 1, 2, 4, 8, and 16 threads. The root
  tree fingerprints are identical.
- **Lock order:** every mutex is an `OrderedMutex` with a level from ARCHITECTURE.md 5.3. In
  `debug`, `asan`, and `tsan`, acquiring out of order trips `DDS_CHECK`, and the stress and
  interleaving tests run with the checker on.

### 3.8 Replication (`tests/repl/`) — G8, G9

Over `SimulatedLink` unless stated otherwise:
- Replicate to an empty repository, restore from the replica, compare. Then replicate a second
  similar snapshot, with no GC running on the receiver, and assert that the sum of the PUT blob
  `length` fields equals the total length of the blobs the replica lacked (computed independently
  from both indexes before the run).
- **Disconnects:** 50 seeded runs disconnect at a random byte offset. Resuming completes. The
  replica commits the snapshot exactly once, and `verify` passes on it.
- **Hostile peer:** a scripted sender sends a blob whose bytes do not match its fingerprint, a PUT
  with an oversized length, a COMMIT whose snapshot references a blob it skipped after a HAVE
  miss, and frames with bad CRCs. Each yields `ERROR` and nothing committed. Mutation
  `have_miss_reserves` (a HAVE miss reserves the fingerprint) must make the skipped-blob case
  commit wrongly, which the test detects.
- **Incompatible params:** HELLO from a repository with a different chunker → `Incompatible`.
- **Subtree pruning (protocol version 2, D108, `pruning_test.cc`):** after one directory of ten
  changes, nine subtrees are pruned, the sender queries under a fifth as many fingerprints as
  without pruning, and both send the same PUT bytes; an unchanged snapshot sends no HAVE at all;
  the same directory content in two places is asked about once; a
  tree held without its descendants is not whole, and the replica still restores; HAVE_TREES in a
  version 1 session is `Corruption`; a chain of 50,000 nested trees is checked without recursion; a
  chunk received but not yet sealed makes its subtree not whole, so a wrong length or a tree
  referenced as a chunk behind it is still refused at COMMIT; 12,288 made-up fingerprints asked
  about leave no remembered answer; and trees go out in batches as they are found (batches of 2,
  window 1).
  A scripted sender asks HAVE_TREES about a subtree the receiver holds only as garbage, a GC runs,
  and COMMIT succeeds and restores; mutation `subtree_check_without_stamp` (the check's lookups do
  not stamp) must make that COMMIT succeed for a snapshot that no longer restores.
- **GC on the receiver** runs in a loop during the disconnect and hostile-peer runs above (not
  during the exact byte-accounting run). Correctness results are unchanged.
- **TCP:** one loopback smoke test with a real socket pair.

### 3.9 CLI (`tests/cli/`)

ctest runs the built `dds` binary on temporary directories: init → backup → snapshots → restore →
diff (a C++ or Python helper compares trees, including mode, mtime, and symlink targets) → rm → gc
→ verify. Each documented exit code has a test that provokes it.

## 4. What "done" means for a feature

Every test the feature's acceptance criteria name exists, passes in `debug`, `asan`, and `tsan`,
and passes in `debug-slow` once.
