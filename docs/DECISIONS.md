# dedupstore — decisions

Every design question is decided here, with the evidence it rests on, what it costs, and when to
revisit it. New decisions are added at the bottom in the same format, and every document a
decision touches is updated with it.

Sources marked **(checked 2026-10-07)** were opened while this file was written. The others are
standard references. The work that depends on a specific detail from them must open the source
first and correct this file if it disagrees. "Not verified" means no source was checked, so the
design either checks the fact at run time or the work that needs it verifies it first.

---

## Scope and process

**D1 — Purpose: a storage engine whose every line can be explained and defended.**
Context: the project covers data deduplication, reclamation, replication across wide-area networks,
concurrency, and automated testing, and is meant to be read as well as run.
Consequence: readability limits (D36), and no feature whose code cannot be explained line by line.
Revisit if: the project turns into a library meant for other users.

**D2 — Name `dedupstore`, binary `dds`, C++ namespace `dds`.**
Descriptive, so a reviewer knows what it is from the name. Not verified: no search for name
conflicts was done.
Revisit if: a conflicting project matters before the repository is made public.

**D3 — C++20, CMake ≥ 3.25, Ninja. Apple clang (macOS), GCC ≥ 11 and clang ≥ 15 (Linux).**
GCC 11 is the oldest compiler that plausibly exists on a RHEL 9 cluster. Not verified for Explorer:
the first Explorer job lists the GCC and clang modules (`module avail`), builds with them in a SLURM
job, and the working module names and versions are appended here.
Banned because GCC 11's libstdc++ or Apple's libc++ lacks or only partly supports them:
`std::format`, `std::print`, `std::expected`, `std::jthread`, C++20 modules.
Revisit if: Explorer offers only newer compilers, so the floor can rise.
Result (2026-10-07): Explorer has no gcc or clang modules. GCC is the system gcc 11.5.0; clang is
the LLVM 19 driver inside module `intel/compilers-2025.0.4`; CMake is module `cmake/3.30.2`. Details
and the job environment are in D57.

**D4 — Dependencies: BLAKE3 (C), GoogleTest, Google Benchmark. Nothing else.**
Each is fetched with CMake `FetchContent` from a release URL pinned with `URL_HASH SHA256`, so
builds are reproducible and need no system packages. The BLAKE3 repository's `c/` directory ships a
`CMakeLists.txt` and is documented as buildable with CMake **(checked 2026-10-07)**. CRC32C,
the hash table, encoding, argument parsing, and logging are written here. They are small and they
are part of what the project demonstrates.
Revisit if: a dependency's build fails on Explorer (fall back to compiling the needed BLAKE3
sources directly in this CMake project).
Versions and hashes: D58. Explorer's compute nodes cannot always reach GitHub, so builds there
read the same hash-checked tarballs from a mirror directory on `/scratch` (D57).
The one exception: zstd 1.5.7, added for compression (D107).

**D5 — POSIX only: macOS for development, Linux x86-64 for measurements.**
Every reported number comes from Linux (D37). macOS support stays because it is the development
machine.
Revisit if: never in v1.

**D6 — Library errors are `Status` / `Result<T>`. No exceptions. `DDS_CHECK` only for invariants.**
Storage engines use this convention (LevelDB and RocksDB return `Status`), and it keeps every error
path visible in review. Corrupt input from disk or the network is a normal outcome (G4), so it is a
returned error, never an abort.
Cost: more verbose call sites. A `DDS_TRY(expr)` macro returns early on error.
Revisit if: the boilerplate obscures logic in review. Prefer helper functions over exceptions.

## Data model

**D7 — Fingerprint = BLAKE3-256(type tag ‖ payload), 32 bytes.**
A cryptographic hash makes an accidental collision negligible (birthday bound about 2^128 blobs)
and a deliberate one infeasible, which matters because equal fingerprints are treated as equal
content. SHA-1 is excluded (practical collisions are public). Non-cryptographic hashes are excluded
(collisions would be easy to construct). BLAKE3 over SHA-256: its authors report it much faster,
and it parallelizes with SIMD. A microbenchmark measures it on Explorer. The one-byte type tag
keeps a tree and a data chunk with identical bytes from sharing an identity.
Revisit if: BLAKE3 throughput is the ingest bottleneck by a wide margin and a measured alternative
is safer (unlikely).

**D8 — Chunker: FastCDC (USENIX ATC 2016), normalized chunking level 2, gear table from a seed.**
FastCDC finds cut points with the Gear rolling hash (one shift and one add per byte), skips the
first `min_size` bytes of each chunk, and uses a stricter mask before the average size and a looser
one after it ("normalized chunking"), which narrows the chunk size distribution. Paper:
`https://www.usenix.org/conference/atc16/technical-sessions/presentation/xia`. Its exact masks and
loop structure are taken from the paper when the chunker is written and cross-checked against a
published implementation
(for example the Rust `fastcdc` crate's `v2016` module). The result is recorded as a new decision
with both sources.
The 256-entry gear table is generated by SplitMix64 from `gear_seed` (default
`0x6465647570737472`, ASCII "dedupstr") and committed as a constant array. A test regenerates it.
The seed is stored in `config`.
Parameters are fixed per repository at `init`, because changing them changes every boundary and
destroys dedup with existing data.
A fixed-size chunker uses the same interface, only as a measured baseline (E1).
Revisit if: E1 shows FastCDC's advantage is negligible on the target data (it then still stays,
because it costs little).
Result: the loop and masks, with their sources, are D62.

**D9 — Defaults: chunks 2 / 8 / 64 KiB (min / avg / max); containers 4 MiB.**
An 8 KiB average matches the FastCDC paper's main configuration, so results can be compared with
it. A smaller average dedups better but needs more index memory per GB (ARCHITECTURE.md 8). E2
measures the trade-off. 4 MiB containers make sequential container I/O efficient while keeping
compaction granular. E5 shows the GC side.
Revisit if: E2 or E5 show a clearly better default for the measured datasets.

**D10 — No write-ahead log. All durable state is immutable files plus atomic renames. The index is
derived.**
Containers and manifests are written once and made visible by rename (ARCHITECTURE.md 7). The index
is rebuilt from container footers on open. With nothing updated in place, there is nothing for a
log to protect, and the recovery code is very small (7.6). Commit = manifest rename, as with git
objects and refs, or restic snapshots.
Cost: open time grows with the number of containers (one footer read each, done in parallel). E9
measures it.
Revisit if: E9's fit, extrapolated to the supported scale (500 GB of unique data), predicts an open
time above 5 seconds. Then add an index checkpoint file, still derived and validated against the
container list, never the source of truth.
E9 put 500 GB at 4.5 s, under that line; the index checkpoint was built anyway (D104).

**D11 — Publish after seal: the index never points at data that is not durable.**
A session's new blobs become visible to lookups only after their container is sealed (7.2). Until
then a session-local pending map dedups within the session.
Consequence: two concurrent sessions may each store one copy of the same new chunk (D32).
Benefit: a lookup hit is always safe to reference. No session can commit a snapshot that depends on
another session's unsealed data.
Revisit if: duplicate copies measured in E6 exceed 1% of physical bytes.

**D12 — When rebuilding the index, the highest container id holding a fingerprint wins.**
All copies of a fingerprint hold identical bytes, so any copy is correct. Choosing the highest id
makes GC's copy-forward win after a crash (GC output ids are allocated after every candidate's id,
ARCHITECTURE.md 4.3), so the old container becomes garbage.
Cost: if the highest copy is corrupt, restore reports corruption even though an older good copy may
exist. Repairing from duplicates is out of scope for v1.

**D13 — Trees and recipes are content-addressed blobs (a Merkle DAG).**
Directory listings and file chunk lists are blobs like data, stored in containers, identified by
fingerprint. An unchanged subtree has the same fingerprint, so a second snapshot stores only the
trees along changed paths. git and restic use the same structure. It also gives GC a uniform mark
traversal and replication a uniform "send what is missing" rule.

**D14 — Two-level file recipes: pieces of at most 4096 chunk references, at most 65,536 pieces.**
A flat list for a 1 TB file would be about 4 GB of metadata in one blob. Fixed-count pieces bound
every blob (≤ 4096 × 36 bytes ≈ 144 KiB) while allowing files of about 2 TiB at the default
average.
Cost: an insertion near the start of a large file shifts piece boundaries, so recipe pieces after
it do not dedup (data chunks still do). Metadata is under 0.5% of data at default sizes.
Revisit if: E1 shows recipe blobs are a significant share of new bytes per version.
Checked with E1 (job 10949339): totals stay small (recipes are 36 bytes per 8 KiB chunk, 0.44% of
data), but per version after the first, recipes are 4.1% to 4.4% of synth's new bytes. Every synth
version edits every file, about 160 edits per 32 MiB piece, so every piece is rewritten whatever its
size; smaller pieces would not help this workload, and kernel versions spend 0.9% to 1.1% on
recipes (their 3.4% to 4.1% of tree blobs is the larger metadata cost). D14 stands.

**D15 — Snapshot id = first 16 bytes of BLAKE3 over the manifest file. Manifests are separate
files.**
Content-derived ids make replication idempotent (same bytes, same id, everywhere) and need no
counter. Manifests live outside containers so listing snapshots is a directory listing, and
removing one is an unlink.

## Concurrency

**D16 — One process owns a repository: a lock file created by hard-linking a fully written file.**
Concurrency is inside one process (D17). Cross-process coordination would need durable shared
state and is out of scope. Lock: write the holder record to a unique file in `tmp/`, `Sync`, then
`Link` it to `lock`. `link(2)` fails with `EEXIST` if `lock` exists, and `lock` never exists
without its contents. That is the portable recipe in Linux's `open(2)` man page for atomic lock
files (it also works on NFS).
Takeover: only when the holder is on the same host and its PID is not alive, or alive with a
different process start time (PID reuse). Never across hosts. The procedure:
rename `lock` to a unique name in `tmp/`; read the renamed file and compare it with the
record that was judged stale; if it differs, another process took over first, so rename it back
and fail with `Busy`; otherwise `Link` a fresh record to `lock` as in normal acquisition. Two
processes cannot both win, because only one rename of `lock` succeeds. The remaining race (a
process that read the stale record, then lost the rename) ends in `Busy`, never in two owners.
Process identity (host, PID, start time, liveness check) is behind the injectable
`ProcessIdentity` interface, so the crash harness can declare a crashed holder dead.
Revisit if: two processes need one repository (for example a separate GC process).
Residual race found later (three processes contending for one stale lock): D66.

**D17 — Thread-safe `Repository`; index in 256 shards each guarded by a `std::mutex`; clang
thread-safety annotations.**
A plain mutex per shard is the simplest correct choice. Each critical section is one probe
sequence in one table, so a lock is held for well under a microsecond, and 256 shards spread
threads out. Two of 32 threads will often touch the same shard at some point, but rarely at the
same instant. E3's profile checks this rather than this paragraph. Lookups also write (the stamp),
so a reader-writer lock would not help. Annotations (`DDS_GUARDED_BY` and friends) let clang's `-Wthread-safety` prove at compile
time that guarded fields are accessed under their lock.
Revisit if: E3's profile shows meaningful time in shard locks.

**D18 — `FlatTable`: open addressing, linear probing, backward-shift deletion, hash = fingerprint
bytes 8..15, 64-byte entries.**
Fingerprints are already uniformly random, so rehashing them adds nothing. Byte 0 selects the shard,
and bytes 8..15 index the slot. Linear probing over 64-byte entries is one cache line per probe.
Backward-shift deletion keeps probe sequences short without tombstones, and GC deletes many
entries. `std::unordered_map` would allocate a node per entry (more memory, pointer chasing).
Writing the table is also a core data-structures exercise.
Revisit if: E9's measured bytes per blob, or the `FlatTable` and `ChunkIndex` microbenchmarks'
lookup times, disagree badly with ARCHITECTURE.md 8.

**D19 — GC is mark-and-sweep from snapshot roots plus copy-forward compaction, not reference
counting.**
Reference counts would have to stay exact across crashes and concurrent sessions, which needs a
log (contradicting D10) and makes every crash a potential leak or premature free. Mark-and-sweep
recomputes liveness from the durable roots each run, so it corrects itself: garbage from crashed
sessions is found without special handling. Containers are immutable, so space is reclaimed by
deleting fully dead containers and copying the live blobs out of mostly dead ones, which is the
approach described for Data Domain in "The Logic of Physical Garbage Collection in Deduplicating
Storage" (Douglis et al., FAST 2017) **(checked 2026-10-07)**.
Cost: each run reads all metadata blobs of all snapshots. E5 measures it.
Revisit if: GC duration grows unacceptably with the number of snapshots.

**D20 — GC safety via epoch stamps: remove only if unmarked and `last_ref < E_min`.**
The race: a backup dedups against a chunk at the same moment GC decides that chunk is dead. The
protocol and its argument are in ARCHITECTURE.md 6.1–6.2. It costs one 8-byte stamp per entry and
a check under a lock GC already takes. The alternatives were worse: stopping all ingest during GC
(simple, but G6 would be trivially true and the project would lose its hardest problem), or
writers re-copying any chunk in a GC "deleting set" (needs extra shared state the epoch rule
avoids).
Revisit if: the stress test's mutation check (`no_epoch_protection`) fails to detect a race, which
would mean the stress test is too weak, not that the protocol is unnecessary.

**D21 — Readers are protected by epoch-based reclamation with short guards.**
GC relocates entries before it retires the old container. Unlinking waits until every reader that
entered before the retirement has left (ARCHITECTURE.md 6.3). Guards cover one file or one
replication batch, so a slow restore delays reclamation by at most one file. Relying only on POSIX
"an unlinked file stays readable while open" would not cover a reader that looked up a location but
has not yet opened the container.

**D22 — Nothing is deleted until a durable copy of everything still reachable exists elsewhere.**
GC order: copy → seal (data and directory synced) → relocate in memory → retire → unlink → sync
directory. A crash at any point leaves at least one durable copy of every live blob (7.5). The
crash test with mutation `gc_unlink_before_copy_sync` proves the order matters.

## Durability and integrity

**D23 — `Sync` = `fdatasync` on Linux and `fcntl(F_FULLFSYNC)` on macOS. Directories:
`fsync`.**
Apple's `fsync(2)` man page says `fsync` does not make the drive flush its write cache, and points
to `F_FULLFSYNC` for that guarantee. Linux `fdatasync` flushes the data plus the metadata needed to
read it back (including the file size), which is all that an append-only file written once needs.
Directory entries become durable only with `fsync` on the directory.
On macOS, `fsync` on a directory has the same drive-cache weakness as on a file. A first check tests
whether `fcntl(F_FULLFSYNC)` succeeds on a directory file descriptor. If it does, `SyncDir` uses it
on macOS. If not, `SyncDir` uses `fsync`, and this decision records that macOS directory durability
is weaker than A2 assumes. That is acceptable only because macOS is never the measured or deployed
platform (D5).
Cost: `F_FULLFSYNC` is slow, which is one more reason macOS numbers are never reported.
Result (checked 2026-10-07, macOS 26.6.2, APFS): `fcntl(fd, F_FULLFSYNC)` on a directory
descriptor returns 0, so `SyncDir` uses it on macOS. This checks only that the call succeeds; it
cannot observe the drive cache. `tests/unit/fs/posix_fs_test.cc`
(`FullFsyncSucceedsOnDirectoryDescriptor`) re-checks it on every macOS test run.

**D24 — All repository I/O goes through `Fs`. User files go through `SourceTree` / `RestoreSink`.**
One narrow interface for repository files lets `MemFs` make tests fast and lets `CrashSimFs` see
every operation that matters for durability. User data is separate because its crash behavior is not
part of the guarantees, and because the restore side needs directory-fd-relative calls for G10
(D42).

**D25 — The crash model is exactly assumptions A1–A3, explored by `CrashSimFs`.**
Real file systems differ in what survives a crash. "All File Systems Are Not Created Equal" (Pillai
et al., OSDI 2014) documents applications that broke by assuming stronger ordering. The design
relies only on: fsync'd file data is durable (A1), directory operations are durable after the
directory is fsync'd (A2), and rename is atomic (A3). The simulator allows any outcome those three
permit: arbitrary prefixes of unsynced appends, and any subset of unsynced directory operations.
Revisit if: a target file system is found to violate A3 (then the design needs a different commit
primitive).

**D26 — CRC32C for structures and stored payloads. BLAKE3 for identity.**
CRC32C detects random corruption and is cheap enough to check on every read. BLAKE3 proves content
identity on restore and in `verify --deep`. Software slicing-by-8 tables first. Hardware
instructions came later (D103) and must match the table version on random input.

**D27 — Explicit little-endian encoding through one helper module. No struct `memcpy`.**
Struct layout and padding are compiler- and platform-dependent. Explicit encoding makes the format
exactly what ON_DISK_FORMAT.md says on every platform, and gives the fuzzer one bounds-checked
`Reader` to attack.

**D39 — `config` is line-oriented text with a CRC.**
It is written once and read by humans debugging a repository, so readability wins. Strict parsing
(exact key order, ranges) keeps it as safe as the binary formats, and it is fuzzed like them.

## Replication

**D28 — Own binary protocol over TCP. WAN conditions come from an in-process `SimulatedLink`.**
The protocol is small (PROTOCOL.md) and is part of what the project demonstrates. Emulating latency
on Linux with `tc netem` needs `CAP_NET_ADMIN` (root), which cluster users lack. `SimulatedLink`
models one-way latency and bandwidth with a serialization queue (a message finishes transmitting at
`max(now, link_free) + size / bandwidth` and arrives one latency later), and injects disconnects.
It is deterministic in content and real-time in timing.
Revisit if: a machine with root becomes available for a `netem` cross-check of E7.

**D29 — No subtree pruning in replication v1. Resumption may resend blobs if a GC ran in between.**
Skipping a subtree whose root tree the receiver has would be safe only if "tree present implies
everything below it present" held. GC deletes garbage trees and their children in separate
containers at separate times, so it does not hold. Every blob is queried instead, at 32 bytes per
blob. Received but uncommitted blobs are unprotected once their session ends, so a GC on the
receiver between an interrupted transfer and its retry may reclaim them. Correct, only slower.
Revisit if: E7 shows HAVE traffic is a significant share of bytes for small incremental snapshots.
Revisited by D108: subtree pruning without the invariant, by checking each subtree
when asked.

**D30 — Flow control: window `W` = 8 batches by default; HAVE batches of up to 4096 fingerprints;
PUT batches of up to 1 MiB.**
At 100 Mbit/s and 80 ms RTT, the bandwidth-delay product is 1 MB, so 8 batches of PUTs up to 1 MiB
keep the link busy with margin. 4096 fingerprints = 128 KiB per query. E7 measures the smallest `W`
that saturates each link.

## Pipeline and behavior

**D31 — Backup parallelism is per file. Each worker owns a `ContainerWriter`.**
Chunking a file sequentially keeps its recipe ordered without coordination. Per-worker writers mean
no lock on the write path. They also place one file's chunks together, a simple form of the
"stream-informed segment layout" in "Avoiding the Disk Bottleneck in the Data Domain Deduplication
File System" (Zhu, Li, Patterson, FAST 2008), which helps restore locality.
Cost: one huge file uses one core. Addressed by D109: a large file is chunked by one
worker and stored by all.

**D32 — Concurrent sessions may store duplicate copies of a new chunk. Accepted, counted exactly,
removed by GC.**
Reserving a fingerprint across sessions on a lookup miss would make sessions wait on each other's
unsealed containers, contradicting D11. Duplicates are not bounded in general: two sessions backing
up the same new data in lockstep can duplicate all of it, because each is always ahead of the
other's last publish. What is guaranteed instead: `stats` counts them exactly ((fingerprint,
container) footer entries minus distinct fingerprints), compaction drops each one it meets, and a GC
at threshold 1.0 removes them all (G5). E6 measures how many arise under realistic overlap.
Revisit if: E6 shows duplicates above 1% of physical bytes in realistic workloads. Then consider
cross-session reservation with a short wait.

**D33 — Restore verifies BLAKE3 of every chunk by default.**
A restore is the moment wrong bytes would reach the user. BLAKE3 runs at several GB/s per core. E4
reports the cost, and `--no-verify` exists for measuring it.

**D41 — Backups preserve content, structure, permission bits, mtimes, and symlinks. Not ownership,
xattrs, ACLs, hard-link identity, or sparseness.**
Those are file-system metadata problems, not storage-engine problems, and each adds platform
differences (macOS vs Linux xattrs) without adding to the storage-engine story. Special files are
skipped and
counted.

**D42 — Restore uses directory file descriptors and `O_NOFOLLOW` everywhere (G10).**
Every restored path is created with `openat`/`mkdirat`/`symlinkat` relative to the parent directory
fd, with `O_NOFOLLOW` on opens, after validating names (no `/`, `.`, `..`). A corrupted or hostile
tree therefore cannot write outside the destination, even through symlinks it created itself
earlier in the same restore.

**D45 — Containers live in one flat directory.**
At 4 MiB per container, 400 GB is 100,000 files, which common local file systems list without
trouble. Sharded subdirectories add path logic for no measured benefit.
Revisit if: `ListDir` on `containers/` shows up in E9's open time.

## Testing, measurement, and readability

**D34 — GoogleTest, Google Benchmark, libFuzzer where available, and corpus replay everywhere.**
These are the standard tools for C++. Apple clang does not ship libFuzzer, so fuzz targets also
compile as a plain driver that replays the saved corpus, which keeps the corpus tested in every
preset. Not verified: which Explorer clang modules include libFuzzer (checked when the first fuzz
target lands).

**D35 — Test hooks: mutations ("teeth") and sync points, compiled only under
`DDS_ENABLE_TEST_HOOKS`.**
A crash or stress test that never fails proves nothing. Each named `DDS_MUTATION` breaks one
ordering or one protection in the real code, and a test asserts the harness catches it. Random
stress alone finds a race only when scheduling cooperates, so each known race also gets a
deterministic test that pauses threads at named `DDS_SYNC_POINT`s (the RocksDB "sync point"
technique) and fails every time under its mutation (TESTING.md 3.6–3.7). The hooks are defined in
the `debug`, `asan`, `tsan`, and `fuzz` presets, and compile to nothing in `release`, the measured
binary.

**D36 — Readability limits.**
Files about 400 lines or fewer, functions about 60 lines or fewer, comments explaining why, no
metaprogramming.

**D37 — Reported numbers come only from Explorer, in SLURM jobs, under BENCHMARKS.md's protocol,
with predictions committed first.**
Login nodes kill long processes (observed on Explorer's). Reporting `mem` and `disk`
separately keeps the CPU story and the storage story apart. Warm-cache only, stated, because
dropping caches needs root. Writing predictions first keeps the analysis honest.

**D38 — Datasets: Linux kernel releases, a seeded synthetic edit workload, and checkpoints from a
model fine-tuning run.**
The kernel series is public and the conventional dedup benchmark. `cdn.kernel.org` serves
`linux-6.N.tar.xz` for 6.0 to 6.10 **(checked 2026-10-07)**. The synthetic workload has a known edit
rate, so chunker behavior can be checked against expectation. Checkpoints are binary model files,
a different kind of data from source trees.

**D40 — No compression or encryption in v1.**
Compression would confound the dedup measurements (E1, E2, E8 measure dedup alone) and adds a
dependency. Encryption changes the dedup design itself (convergent encryption and its known
weaknesses) and is a different project. Compression is reported separately.
Since 2026-10-10 compression is a repository option, off by default, and dedup is still measured on
content (D107).

**D44 — Commits: `<area>: <what changed>`, small.**
Each commit is scoped to one concern. Areas: `build`, `common`, `fs`, `chunk`, `hash`, `store`,
`index`, `snapshot`, `repo`, `gc`, `repl`, `cli`, `test`, `bench`, `docs`.

**D46 — Python only for tooling (`tools/report.py`, dataset scripts).**
The product is C++. Report generation is easier to read in Python and is not part of what is being
demonstrated.

## Added after the design review (2026-10-07)

**D47 — No mounted file-system view (FUSE).**
Browsing is `dds ls` and restore. A FUSE layer is a large separate surface (caching, permissions,
platform differences) that adds nothing to the storage-engine story.

**D48 — No authentication or TLS on the replication port. Use an SSH tunnel.**
Writing a secure transport is a project of its own, and getting it subtly wrong is worse than not
having it. `ssh -L` gives authentication and encryption today. Documentation examples bind `--listen` to
`127.0.0.1`, and `dds serve` prints a warning when given a non-loopback address.

**D49 — GC candidates are containers whose entries are published, never merely sealed.**
Found in design review: a container sealed just before a GC starts and published just after it
looks 0% live to the planner (no index entry points at it), so it would be deleted while a session
is about to reference it. `ContainerSet::MarkPublished` runs after `ChunkIndex::Publish`, and the
candidate set is taken from published, not-retired containers (ARCHITECTURE.md 4.3, 6.2 item 5).
Proved by the `seal.before_publish` interleaving test and mutation `candidate_before_publish`.

**D50 — Exact epoch semantics.**
Found in design review: "`Advance()`" and "the epoch" admitted readings under which reader safety
fails. Fixed in ARCHITECTURE.md 6.1:
- `Advance()` returns the new value, under `epoch_mutex`;
- session start, reader entry, and GC's `E_gc`/`E_min` are each read and registered in a single
  `epoch_mutex` critical section;
- stamps take the max, so a delayed stamp cannot lower `last_ref`;
- `Publish` stamps an existing entry it skips as a duplicate;
- GC run numbers start at 1.

Mutation `stamp_assign` proves the max matters.

**D51 — Explorer work runs as SLURM jobs, CPU jobs on the `short` partition.**
Jobs are submitted with `sbatch` and followed with `squeue` and `sacct`; nothing heavier than
`--help` runs on a login node (D37).
Revisit if: the `short` partition rejects the job (then pick a partition from `sinfo` and record it
here).

**D52 — Init's commit point is the rename of `config`.**
Found in design review: init had no defined crash behavior, and "the repository opens" could not
hold for a crash during init. Now: directories first, then `config` via temp file + rename +
`SyncDir` (ARCHITECTURE.md 7.7). Without `config`, `Open` returns `NotFound`, and `init` accepts the
leftover directories, so a crashed init is simply run again.
Refined by D65 (a leftover `tmp/config.tmp` is accepted).

**D53 — Module layering keeps the one-way rule: backup and restore live in `repo`; GC and
replication drive the repository from outside.**
Found in design review: the first module map had `snapshot` using `repo` types and `repo` calling
`gc`, which would create build cycles. `Epoch` and `GcRun` move to `common`. `BackupPipeline` and
`Restorer` move to `repo`. `Repository` exposes `Internals()` to `gc` and `repl` and never calls
them (ARCHITECTURE.md section 1).

**D54 — Restoring symlinks and name collisions.**
Linux cannot change a symlink's permission bits, so they are recorded at backup but not restored.
Symlink times are restored with `utimensat(..., AT_SYMLINK_NOFOLLOW)`. Restore never replaces an
existing entry: an `EEXIST` (for example two names that differ only in case on a case-insensitive
macOS volume, which the Linux kernel tree contains) fails with `AlreadyExists` naming both entries.
Directories are created with mode 0700 and get their recorded mode after their children.

**D55 — A replication HAVE miss reserves nothing.**
In a backup, a lookup miss reserves the fingerprint, because the same worker always writes it next.
A replication sender may never send a blob it was told is missing, so a reservation there would let
the commit walk accept a blob that does not exist. Receiver lookups therefore run with reservation
off, and a blob sent twice is verified and dropped (PROTOCOL.md 4.2). Mutation `have_miss_reserves`
proves the hostile-peer test can see the difference.

## Added during implementation

**D56 — Locks are taken with `MutexLock`, an annotated RAII guard for `OrderedMutex`.**
The standard RAII lock types are `std::scoped_lock` and `std::unique_lock`, and D17 requires
clang's thread-safety analysis. The two conflict: libstdc++ (GCC's library, also used by clang on
Linux) has no annotations on its mutex or lock types (checked 2026-10-07: zero `capability`,
`scoped_lockable`, or `guarded_by` attributes in GCC 11's `<mutex>`, `bits/std_mutex.h`, and
`bits/unique_lock.h` on Explorer), and libc++ annotates `std::mutex` and `std::lock_guard` only when
`_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS` is defined (checked in the macOS SDK's libc++
`__config`). With the standard guards, `-Wthread-safety` could not see that a `DDS_GUARDED_BY` field
is accessed under its lock. `MutexLock` (in `common/ordered_mutex.h`) is the same RAII idea marked
`DDS_SCOPED_CAPABILITY`. It also has `lock()`/`unlock()`, so `std::condition_variable_any` can wait
on it. Condition waits are written as `while (!cond) cv.wait(lock);`, because the analysis does not
know that a predicate lambda runs with the lock held.
Revisit if: libstdc++ gains thread-safety annotations.

**D57 — The Explorer toolchain and job environment (completes D3).**
Found 2026-10-07 with `module avail`, `which`, and a probe job on node c0585:
- No `gcc`, `clang`, `llvm`, or `ninja` modules exist. GCC is the system compiler, gcc 11.5.0
  (Rocky Linux 9.7), so no module is needed.
- clang is the LLVM 19 driver inside module `intel/compilers-2025.0.4`:
  `/shared/EL9/explorer/intel-oneapi/2025.0.1/compiler/2025.0/bin/compiler/clang++`. It reports
  itself as "Intel(R) oneAPI DPC++/C++ Compiler 2025.0.4", uses resource directory `lib/clang/19`,
  selects GCC 11's libstdc++, and ships `clang-tidy`, `clang-format`, `ld.lld`, and the ASan, TSan,
  UBSan, and libFuzzer runtimes. The job script calls it directly; it does not load the module (the module
  only adds Intel's own compilers to `PATH`).
- CMake: module `cmake/3.30.2`. In a clean environment, Explorer's modules are visible only after
  `module use /shared/EL9/explorer/modulefiles`, which is what the site profile
  (`/etc/profile.d/zz_modules.sh`) does; jobs run it themselves.
- Compute nodes cannot always reach the internet: a probe job on c0585 fetched from GitHub, but on
  c0589 `pip` timed out connecting to pypi.org. So all downloads happen on the login node, before
  any job: ninja (there is no ninja module) as `ninja==1.13.2` from PyPI in a
  venv made with `/usr/bin/python3` at `/scratch/<user>/dedupstore-tools/venv`, and the three
  dependency tarballs, checked against the SHA-256 values in `cmake/DdsDependencies.cmake`, in
  `/scratch/<user>/dedupstore-tools/deps`. Builds pass `-DDDS_DEPS_MIRROR=<that directory>`.
- The login profile loads an anaconda module and activates a conda environment, which puts conda's
  `cmake` first on `PATH` and an anaconda `include/` directory on `CPATH` (observed in clang's `-v`
  include list). Jobs therefore start from a minimal `PATH`, unset `CPATH` and friends, and load
  modules explicitly (the jobs' `env.sh`). `sbatch` runs from an `env -i` shell, and
  `--export=NONE` is not used, because the sbatch man page says that with `NONE` "Slurm will then
  implicitly attempt to load the user's environment" on the compute node.
- Jobs always build an exact commit, uploaded as `git archive` output, never uncommitted work.
- Compute nodes have node-local `/tmp` (821 GB on c0585), where builds go.
- GCC's sanitizer runtimes are not installed (no `libasan`, `libubsan`, or `libtsan` packages; a
  GCC ASan link fails with "cannot find /usr/lib64/libasan.so.6.0.0", job 10893030), so sanitizer
  builds on Explorer use clang, which links its own static runtimes. CI covers GCC with sanitizers.
- CMake identifies Intel's clang as `IntelLLVM 2025.0.4`. `DDS_IS_CLANG` accepts it, and BLAKE3,
  whose CMakeLists knows only GNU, Clang, and AppleClang, is given its own Clang options
  (`cmake/DdsDependencies.cmake`).
- `sbatch` sometimes answers "Batch job submission failed: Unexpected message received"
  (2026-10-08, twice), and that answer can come after the job was queued: one such "failed"
  submission had queued job 10926208, and the retry queued a duplicate (10926226). Submission
  therefore accepts only a numeric job id, and after a failure looks in `sacct` for a job of the
  same name before retrying.
Evidence: the job logs, including the failed first attempt 10893030.
Revisit if: Explorer adds gcc or llvm modules, or the Intel module is retired.

**D58 — Dependency versions (completes D4).**
BLAKE3 1.8.7, GoogleTest 1.18.0, Google Benchmark 1.9.5: the latest release of each on
2026-10-07 (GitHub releases API). GoogleTest 1.18.0 requires C++17. The SHA-256 of each release
tarball is in `cmake/DdsDependencies.cmake` (computed from the downloaded files). BLAKE3's
`c/CMakeLists.txt` is used as is, with its TBB options off.
Revisit if: a release fails to build with GCC 11 or Apple clang.

**D59 — The lock-order checker is its own build flag, `DDS_ENABLE_LOCK_ORDER_CHECK`.**
TESTING.md section 2 lists "checker on" and "test hooks" as separate properties of each preset, so
they are separate compile definitions. It is on in `debug`, `asan`, `tsan`, and `tidy`, and off in
`release` (the measured binary) and `fuzz`. It is a `-D` compile definition used with `#ifdef`, not
a macro defined in code, so no macro is added. `LockOrderCheckEnabled()` lets
tests skip when it is off.

**D60 — Each test has exactly one label, and slow tests run through their own test presets.**
Measured 2026-10-07 with CMake 4.3.3: with a label filter in the `debug` test preset,
`ctest --preset debug -L slow` reported "No tests were found!!!" while `ctest -N -L slow` listed
the slow test. ctest combines a command-line `-L` with the preset's filter rather than replacing it,
so the command that TESTING.md named could never select anything. Now every test
carries one label, its family or `slow`. The `debug`, `release`, `asan`, `tsan`, and `tidy` test
presets exclude `slow`, and the test presets `debug-slow`, `asan-slow`, and `tsan-slow` run only
`slow`. TESTING.md now says `ctest --preset debug-slow`.
In the same change, UBSan builds add `-fno-sanitize-recover=all`, so undefined behavior fails the
test instead of printing a report and continuing.

**D61 — Development tools are pinned: clang-format 19.1.7 and clang-tidy 19.1.0.1.**
Neither tool is installed on the development laptop, and Explorer's toolchain is LLVM 19 (D57).
`scripts/dev/setup_tools.sh` installs both from PyPI wheels into `.tools/venv`, so formatting and
tidy results match across machines. They are tools, not code dependencies (D4 covers what the
build links). On macOS the `tidy` preset passes `--extra-arg=-isysroot<SDK>`, because CMake 4 no
longer adds `-isysroot` when none is configured, and a standalone clang-tidy then cannot find the
standard headers (observed: `'cstddef' file not found`).
Revisit if: the pinned wheels stop installing on a supported platform.

**D62 — FastCDC is the paper's Algorithm 1 with the fastcdc-rs `v2016` mask table (completes D8).**
Sources opened 2026-10-07: the paper (`atc16-paper-xia.pdf` from usenix.org), fastcdc-rs
`src/v2016/mod.rs` (github.com/nlfiedler/fastcdc-rs, master), and the first author's C code
(github.com/wxiacode/FastCDC-c, `fastcdc.h` and `fastcdc.c`).
- Loop: Algorithm 1 exactly. `fp = (fp << 1) + Gear[src[i]]`, starting at `i = MinSize` with
  `fp = 0`; `if n <= MinSize return n`; `n = min(n, MaxSize)`; `NormalSize = min(avg, n)`; `MaskS`
  before `NormalSize`, `MaskL` after; a match at byte `i` returns `i`, so the chunk ends before the
  matching byte; with no match, return `n`. fastcdc-rs `cut()` is the same loop (it writes `>` and
  `<` where the paper writes `>=` and `<=`, which selects the same lengths). A test transcribes the
  pseudocode line by line and compares boundaries on random data for eight parameter sets
  (`FastCdcChunkerTest.MatchesPaperAlgorithmOnRandomData`).
- Masks: the paper prints only the 8 KiB masks: `MaskS = 0x0003590703530000` (15 one bits),
  `MaskA = 0x0000d90303530000` (13), `MaskL = 0x0000d90003530000` (11). The other sizes come from
  fastcdc-rs `MASKS`, indexed by bit count: `mask_s = MASKS[log2(avg) + level]` and
  `mask_l = MASKS[log2(avg) - level]`, covering indices 5 to 25, which is every allowed average
  (256 B to 4 MiB) at every level (0 to 3). Its comment credits "the C reference implementation
  (destor repository)" for 64 B to 128 KB and restic-FastCDC for the rest. Its 13- and 11-bit entries
  equal the paper's `MaskA` and `MaskL`. Its 15-bit entry, `0x0000d90f03530000`, has `MaskS`'s bit
  count but not its bit positions. dedupstore uses the table for every size, 8 KiB included, so
  every (average, level) pair comes from one published table. The cut probability, `2^-bits` on
  random input, depends only on the bit count. A test checks that entry `k` has `k` one bits.
- Not used: FastCDC-c's masks (8 KiB `0x0000d93003530000`, and `0x0000d9f003530000` for its
  stricter mask) and its normalized loop, which starts hashing at 6 KiB. The upstream destor
  repository (github.com/fomy/destor) contains no FastCDC code, only Rabin and AE chunking, so the
  crate's attribution could not be traced further.
- Boundaries are comparable only within one implementation anyway, because the gear table comes
  from our own seed (D8).
Cost: default boundaries differ from those of an implementation using the paper's printed `MaskS`;
nothing depends on matching them.
Revisit if: E1 or E2 show dedup clearly worse than the paper reports for a comparable
configuration. Trying the paper's `MaskS` would need a new `chunker` name in `config`, because
existing repositories must keep their boundaries.

**D63 — A ContainerWriter buffers its whole container in memory and writes it once, at seal.**
Nothing reads a blob before its container is sealed and published (D11), so there is no reason to
write it earlier. One Append per container keeps the seal protocol (ARCHITECTURE.md 7.2) to one
file write, one sync, one rename, and one directory sync. Adding a blob only appends to a buffer, so
`ContainerWriter::Add` cannot fail and returns nothing; adding one fingerprint twice to one container
is a caller bug and aborts (the footer would fail validation). The id is allocated at the first Add,
not at construction, so GC's writer gets an id above every candidate (D12).
Cost: memory of one container per writer: 16 workers × 4 MiB = 64 MiB at the defaults; a 64 MiB
metadata blob makes one container that large.
Revisit if: large container targets times many workers make writer memory matter.

**D64 — ContainerSet keeps at most 128 container files open for reads, least recently used out.**
Restores and GC read many containers. Reopening a file per blob would cost a syscall pair each time,
and keeping every file open would exhaust descriptors: launchd's default soft limit on macOS is 256
per process (`launchctl limit maxfiles`, checked 2026-10-08), and Linux's common default is 1024.
Files are opened outside `containers_mutex`, and an evicted handle stays valid for a reader that
still holds it (shared ownership).
Revisit if: E4 shows reopen costs in restore profiles.

**D65 — Init accepts a leftover `tmp/config.tmp`, syncs the parent of a new root, and returns the
config.**
ARCHITECTURE.md 7.7 step 1 required `tmp/` to be empty, but a crash between writing
`tmp/config.tmp` and renaming it leaves exactly that file, so "a crashed init is simply run again"
(D52) would fail. Init now accepts that one file and replaces it. When init creates the repository
directory itself, it also syncs the parent directory; otherwise a power loss right after a
successful init could drop the whole repository (A2 covers only the synced directory). Init returns
the new `RepoConfig` so `dds init` can report the repo id. Separately, the config decoder checks the
`dedupstore-config` version before the line count and CRC, because a later version may change
everything after it; a bit flip in that digit can then read as Incompatible, which TESTING.md 3.3
allows.

**D66 — The lock takeover's put-back uses Link; a three-way race can still leave two owners.**
Found while implementing D16: if process A judges a lock stale, B takes it over first, and A then
renames B's fresh lock away, A must put it back. If a third process C links its own record into the
gap before A's put-back, no put-back can be right: replacing C's lock (rename) leaves B and C both
believing they own the repository, and failing to restore (link) leaves the same two owners. The
code uses Link, so it never destroys a lock file, and returns Busy. The window needs three
processes opening one repository within milliseconds of each other while its holder is dead, which
is already outside the one-process model (D16).
Revisit if: dds ever supports several processes on one repository (D16's revisit condition).

**D67 — Open loads container footers in parallel straight into the index.**
`ChunkIndex::RebuildInsert` keeps the higher container id whichever copy arrives first, so the
result does not depend on loading order. Threads (hardware concurrency, at most 16) therefore read
footers and insert entries directly, with no staging copy of all footers in memory (which at 500 GB
of unique data would be several GB). Any failure to load a container, corruption or I/O error, skips
and reports it (7.6 step 5); its file name still counts for the next id (step 6).
Revisit if: E9 shows open time dominated by shard-lock contention.

**D68 — A restore leaves the destination directory's own mode and times alone.**
A snapshot records metadata for entries, and the root directory is not an entry of any tree: the
manifest names only the root tree (ON_DISK_FORMAT.md section 8). There is nothing to apply. A
destination that restore creates is made with mode 0700, as every restored directory starts (4.2).
Revisit if: manifests start recording the backed-up root's own metadata.

**D69 — verify and stats walk snapshots with in-memory memos, not with the index's GC marks.**
GC's marks belong to GC runs (D21), and a `serve` process may run GC while verify or stats walks, so
they cannot share the field. The walker keeps a map from each reachable fingerprint to its location,
and per tree and recipe what it found beneath, with paths relative to that blob. Shared subtrees are
then checked once, yet a damaged blob is reported under every snapshot and path that reaches it, as
verify's report requires (a walk that simply skipped seen blobs named only the first snapshot; the
bit-flip test caught it). Cost: about 100 bytes per distinct
reachable blob, roughly 1 GB per 10 million blobs. Both the walker and the restorer also stop at a
tree that contains itself: honest content cannot (its fingerprint would be a BLAKE3 fixed point),
but without hash checks (non-deep verify, `restore --no-verify`) a damaged index can claim one, and
following it would recurse until the stack overflows, a crash on bad data (D6).
Revisit if: verify's memory matters at the sizes E9 measures; then key the memo by index slot.

**D70 — stdout carries only machine-readable results; backup also prints its bare id there.**
SPEC.md section 3 says human-readable output goes to stderr and that backup "prints the snapshot
id". Both hold: without `--json`, backup prints the 32-hex id alone on stdout (so
`id=$(dds backup repo dir)` works) and its summary on stderr; listings (`snapshots`, `ls`) are
human-readable, so they go to stderr, and scripts use `--json`. Argument checks the CLI can make
from the argument alone (a count, a size) are usage errors, exit 1; limits the format defines (the
1024-byte tag) are checked by the library before any work and are InvalidArgument, exit 8.
Revisit if: listings are wanted on stdout.

**D71 — JSON strings write invalid UTF-8 as U+FFFD, and `ls --json` adds `name_hex` for such names.**
JSON text must be UTF-8 (RFC 8259 section 8.1), and file names may be any bytes (D41); passing them
through produced invalid JSON. Validity follows RFC 3629 section 4 (no overlong forms, no
surrogates, nothing above U+10FFFF), and each invalid byte becomes one U+FFFD. Where the exact bytes
matter, the name of an `ls` entry, the hex form goes alongside.
Revisit if: another field (a symlink target, a path in a verify error) needs its exact bytes in JSON.

**D72 — Backup and Restore return result structs, and the clock lives in RepoInternals.**
ARCHITECTURE.md section 2 had `Result<SnapshotId> Backup` and `Status Restore`, but `--json`, the
benchmarks, and section 11 need the counters (blobs written, dedup hits, bytes restored), so they
return `BackupResult` and `RestoreResult`. `OpenOptions::clock` (tests pass a `ManualClock`) now
reaches backup timestamps through `RepoInternals::clock`, as it will reach GC. `BeginIngest` waits
for its only caller, the replication receiver.
Revisit if: never; this records an interface change.

**D73 — A damaged manifest fails `snapshots` and `stats` with Corruption; `verify` reports it and
continues.**
Listing a damaged snapshot as if it were fine, or computing a dedup ratio without it, would hide
damage; failing names it (exit 3) and points to `verify`, which collects every problem.
Revisit if: listing a repository with some damaged snapshots becomes a common need; then list them
with a "damaged" marker instead.

**D74 — A backup lists the whole tree before storing any file.**
The tree builder needs every directory's entries, which ARCHITECTURE.md 4.1's walker records in any
case, so listing first costs no more memory than a pipelined walker: about 100 bytes plus the name
plus 32 bytes per recipe piece for each entry, roughly 1 to 2 GB at 10 million files. Doing it in
three steps (list, store files, build trees) means parallel ingest replaces only the middle step
with workers.
Revisit if: a benchmark with many millions of files shows memory pressure; then build each
directory's tree as soon as its last descendant is stored.

**D75 — Test hooks are a run-time registry behind two macros, and unknown names abort.**
`DDS_MUTATION("name")` asks a process-wide registry whether a test switched `name` on (RAII
`ScopedMutation`); `DDS_SYNC_POINT("name")` runs a callback a test registered (`ScopedSyncPoint`),
outside the registry's lock so it may block. Without `DDS_ENABLE_TEST_HOOKS` they compile to `false`
and nothing (D35). Every name must appear in the list in `test_hooks.cc`, and an unknown one aborts:
a misspelled mutation would otherwise be silently off, and a test meant to prove the harness has
teeth would pass without breaking anything. `stamp_assign` sits on the per-chunk stamping
path, so `MutationOn` first reads an atomic count of active mutations and takes the registry's
mutex only when a test has switched one on; sync points are only on seal, commit, read, and GC
steps.
Revisit if: a sync point is ever needed on a per-chunk path; then give sync points the same check.

**D76 — How CrashSimFs reads TESTING.md 3.6.**
TESTING.md fixes the model's outline; these are the details it leaves open, each chosen to allow at
least what a real file system could do (fsync(2): "Calling fsync() does not necessarily ensure
that the entry in the directory containing the file has also reached disk. For that an explicit
fsync() on a file descriptor for the directory is also needed"):
- Names bind to inodes. A create or link binds a name; a remove unbinds it only if it still refers
  to the same file; a rename binds the new name and unbinds the old one if it refers to the file.
  So a rename made durable (by SyncDir of its target) needs no durable old name: 7.2 never syncs
  `tmp/`, and the container is still durable under `containers/`. Making such a rename durable also
  retires pending binds of its old name, so a later SyncDir of the source never brings it back.
- A directory's contents may become durable before its own name (SyncDir of the child first). On
  the materialized disk only what is reachable from `/` exists.
- Seed 0 keeps no pending operation and no unsynced byte; seed 1 keeps all of them; seeds 2-19 flip
  a coin per operation and pick a random prefix per file, from SplitMix64 seeded with the seed
  (library code avoids `<random>`, which the pinned clang-tidy cannot parse on macOS). Both extremes are explored at every
  crash point.
- After the crash point every Fs and WritableFile call fails, reads included; read handles opened
  earlier keep working, because reads cannot change the disk. Close is not a mutating call.
- Pending operations survive independently, which allows more states than ext4's ordered journal
  (where a later metadata operation implies the earlier ones), so passing here implies passing
  there.
Revisit if: a protocol step relies on an ordering between directories that real file systems
provide and this model does not (none does today).

**D77 — CrashSimFs's mutex has lock level 90.**
It is held while CrashSimFs calls into its MemFs (whose mutexes are leaves, level 100), so that the
recorded order of operations is the order they took effect even with several threads. Callers
may hold `snapshots_mutex` (20) or `containers_mutex` (50) during I/O, so it sits above them.
Revisit if: a caller needs to do I/O while holding a leaf mutex (forbidden today, 5.3).

**D78 — The backup queue is a mutex-and-condition-variable `BoundedQueue<T>`.**
ARCHITECTURE.md 4.1 asks for a bounded multi-producer, multi-consumer queue of capacity 4 x N. The
simplest correct form is a `std::deque` under one leaf mutex with two `condition_variable_any`
(waiting on `MutexLock`, so the thread-safety analysis and the lock-order checker see every
acquisition), and `Close()` to end it. A lock-free queue would save microseconds per file, while a
file costs at least one open and one read; the queue is not where ingest time goes (E3 measures
this). It is a class template, the one generic class in `common/`, because the same queue serves
backup tasks now and replication batches later.
Revisit if: E3's profile shows time in the queue's mutex.

**D79 — The first failure in a backup stops the whole pipeline, and its error is the one returned.**
A worker that fails records its status (the first one wins), sets a stop flag, and closes the queue.
The walker's next `Push` then fails, so it returns Aborted; the other workers drain the queue without
storing anything more. The caller joins every worker before looking at any result, then returns the
worker's error in preference to the walker's Aborted, so the message names the real cause. Nothing is
committed, and whatever was sealed is garbage for the next GC, as after a crash (G2).
Revisit if: users want a backup to skip unreadable files instead of failing (an option, not a
change to this rule).

**D80 — The first parallel-ingest throughput run on Explorer is exploratory, on one kernel
tarball, and the kernel dataset's checksum list is trusted without its PGP signature.**
The run is exploratory, not a reported one, so it skips BENCHMARKS.md section 2's protocol (no
prediction, three repetitions, no result files under `bench/results/`) and its output is kept only
as job logs. It uses `linux-6.10`, extracted into node-local `/tmp` with
the repository next to it, read once first ("warm source cache"). `bench/datasets/fetch_kernel.sh`
checks each tarball against kernel.org's `sha256sums.asc` for that directory but does not verify
that file's PGP signature (no keyring is set up on Explorer); a tarball the list does not name is
recorded on first download ("tofu"), as BENCHMARKS.md section 4 allows.
Revisit if: results are ever reported from this dataset; then import the kernel.org signing keys
and verify the signature, or mark every such result "tofu".

**D81 — GC counters in `stats` are this process's; `dds gc --json` reports each run.**
`stats` is to show GC counters. Every CLI command is its own process, and the
repository has no file for statistics (D10: derived state is rebuilt, never stored), so `RepoStats`
carries GC totals for the runs of the current process: useful in `serve` and in tests, zero in a
fresh `dds stats`. A single run's counters (containers deleted and compacted, blobs removed and
copied, bytes copied and reclaimed, phase times) are in `dds gc --json`, which is where E5 reads
them.
Revisit if: users want lifetime GC history; that would need a durable record, a format change.

**D82 — The stale-stamp test injects a stamp older than E_min; under 6.1 no correct caller makes one.**
TESTING.md 3.7 asks that forcing an older `now` after a newer one leaves `last_ref` unchanged and
that `stamp_assign` then breaks the dedup-during-GC test. Under ARCHITECTURE.md 6.1 every stamp
applied after `BeginGc` comes from a session registered at or after `E_min`, so even an assigned
stamp would be at least `E_min`: the max is defense in depth, not something the protocol needs
today. The test therefore applies a stamp of epoch 1 to every entry directly, as a delayed stamp
from a session that has since ended would. With the max it changes nothing; with `stamp_assign` it
lowers `last_ref` below `E_min`, GC removes the entries, and the snapshot no longer restores.
Revisit if: a caller ever stamps with an epoch read before it registered; then the max becomes
load-bearing, and this test already covers it.

**D83 — How the GC mutations break the rules, and how the tests sample them.**
`no_epoch_protection` makes `IsProtected` false, which the planner and `RemoveIfDead` share, so they
cannot disagree. `stamp_assign` assigns in `ChunkIndex::Stamp`. `candidate_before_publish` takes
every container file on disk as a candidate, sizes read from its footer. `retire_without_grace`
ignores readers in `ReclaimRetired`. `gc_unlink_before_copy_sync` retires and reclaims each
processed container at once, before its copies are sealed, so the real unlink-and-sync code runs
in the wrong order (no second unlink exists to clash with the normal one). The crash mutation tests
sample 6 seeds per crash point (the two extremes and 4 random): detection needs violating states,
not all of them, and each mutation produces hundreds; the scenarios themselves keep 20.
Revisit if: a mutation test ever passes with 6 seeds and fails with 20; then raise the sample.

**D84 — `Repository::Close` reclaims retired containers before releasing the lock.**
ARCHITECTURE.md 4.3 step 6 names close as one of the three reclamation points; once the caller is
done with the repository no reader can remain. The destructor calls `Close`; clang-tidy's
`bugprone-exception-escape` is suppressed there with the reason in the code: only `std::bad_alloc`
could escape, and running out of memory ends the process either way.
Revisit if: library code ever starts using exceptions for errors (it must not, D6).

**D85 — GC's marker walks iteratively and uses the index mark itself as its visited set.**
`MarkLive` reports whether it set the mark or found it already set in this run, so the marker
descends into a tree or recipe only on the first visit: shared subtrees are walked once, and a
tree that (through a damaged index) contains itself cannot loop. The walk keeps an explicit stack,
so a very deep directory tree cannot exhaust GC's thread stack. No visited set is allocated, unlike
verify's walker (D69), which cannot use the GC mark field (it may run while GC does).
Revisit if: never; this records an interface choice.

**D86 — A `Connection` receives exactly N bytes or fails with Aborted; Send and Receive may run on
two threads.**
PROTOCOL.md 4.1 has the sender write on one thread and read replies on another, so the interface
allows one concurrent Send and Receive and a Close from any thread that wakes both. Receive takes a
timeout (PROTOCOL.md 6: 60 s idle, 10 min for a reply, configurable) and returns Aborted on
timeout, on the peer closing, and on a stream that ends mid-message, so every caller handles one
outcome for "the peer is gone". `TcpConnection` closes by `shutdown`, which wakes a `poll` blocked
on the other thread, and closes the descriptor only in its destructor, so no thread ever polls a
descriptor number that was reused.
Revisit if: a transport needs partial reads (none does: frames have a length prefix).

**D87 — SimulatedLink's disconnect counts bytes in both directions and delivers the exact prefix.**
TESTING.md 3.8 asks for disconnects "at a random byte offset". The link counts every byte sent on
the connection, both ways; the send that crosses the offset delivers the bytes up to it (often
half a frame) and breaks both directions. The tests draw the offsets from a seeded generator over
the byte count of a complete transfer, which is the same every time, so a cut anywhere before the
end must fail the transfer, and the test asserts that.
Revisit if: E7 needs packet loss or reordering (TCP hides both; the link models a TCP stream).

**D88 — The receiver has one notion of "present", shared by HAVE and the commit walk.**
`Present(fp)` is `session.Lookup` with HAVE's reservation mode (off, D55): an index hit, which
stamps the entry so a concurrent GC keeps it, or a blob received in this session. The COMMIT walk
asks the same question for every reachable blob, then checks the type of every blob the index
holds and parses every tree and recipe (read under guards, the same checks as `verify`); it does
not rehash data chunks, because each received one was hashed on receipt and the rest were already
in the repository. PUT's "already have it" check uses received-only semantics. With
`have_miss_reserves`, a HAVE miss leaves a reservation that `Present` then counts, so a COMMIT
referencing a never-sent chunk is accepted: the hostile-peer test sees exactly that.
Revisit if: the commit walk should also rehash (a `--deep` replication option).

**D89 — The sender answers a HAVE and queues its missing blobs in one critical section.**
A first version freed the HAVE's window slot and then, in a second critical section, queued its
missing fingerprints. In between, the writer could see nothing outstanding and nothing queued, send
COMMIT, and never send those blobs; the receiver's commit walk then refused the snapshot. The
replication test caught it. The window table, the missing queue, the error, and COMMIT_OK now
live in one `Window` object under one leaf mutex, and a slot for a PUT is awaited separately from
"a slot or work", so the window is never exceeded. The queried-fingerprint set holds at most 2^20
entries and is cleared when full: forgetting can only re-query a blob (PROTOCOL.md 4.1 step 3).
Revisit if: E7 shows the set's clearing causes noticeable re-queries on large snapshots.

**D90 — `dds serve`: at most 16 sessions, ERROR(Busy) beyond, SIGINT or SIGTERM stops it cleanly.**
Each accepted connection gets a thread and its own ingest session; a seventeenth connection is
answered `ERROR(Busy)` and closed (PROTOCOL.md 6). The signal handler only stores to a lock-free
`std::atomic<bool>`, the one thing a handler may safely do; the accept loop and the GC loop check
it at least every 200 ms (a `poll` timeout and a bounded sleep, in production code only). On stop
the server closes every open connection, so each session seals what it received, then joins every
thread. With `--listen HOST:0` the kernel picks the port and `serve` prints it; the CLI tests read
that line through a pipe instead of sleeping.
Revisit if: serve needs reloading or graceful draining of active transfers.

**D91 — A refused or failed TCP connect is IoError (exit 5); an unresolvable host is NotFound.**
SPEC.md's exit 2 covers a missing repository, snapshot, or path, and exit 6 a peer that disconnects
or ends a session with an error. A `connect` that fails is neither: an OS call failed. A host name
that does not resolve is reported as NotFound, like any other name that does not exist.
Revisit if: users script around replication failures and need finer codes.

**D92 — Replication fuzzing: one target for frames, one selector-driven target for every message.**
Replication needs fuzz targets for the frame and every message. `fuzz_frame` decodes whole frames;
`fuzz_message` uses the first byte to choose one of the eight payload decoders, so every decoder is
fuzzed without eight near-identical binaries. COMMIT's payload is a manifest, already covered by
`fuzz_manifest`. Every target requires accepted input to re-encode byte for byte.
Revisit if: one decoder's coverage lags because the selector splits the fuzzing time.

**D93 — The synth dataset is generated by `bench/gen_synth` from one seed.**
BENCHMARKS.md section 4 fixes the shape (2 GiB random plus 1 GiB text-like, 10 versions at a set
edit rate); the details: files of 1 to 16 MiB; text-like bytes are words of 2 to 9 letters from a
fixed 4096-word pseudo-vocabulary; each version applies inserts, deletes, and overwrites of 1 to
4096 bytes at random offsets until about `edit_rate` of each file's bytes are touched. All
randomness is SplitMix64 keyed by (seed, version, file), written little-endian, so any version can
be regenerated alone, on any machine. E9's large repositories use a streaming random source instead
of files, so 100 GiB of input never touches a disk.
Revisit if: a result depends on a property of real data the generator lacks (say, repeated
blocks within a file).

**D94 — Reported runs: whole cascadelake nodes of `short`, GCC 11.5 release, node-local xfs.**
BENCHMARKS.md section 1 left the partition and disks open. `short` allows 2 days and 2 nodes; its
24 cascadelake nodes (Intel Xeon Platinum 8276, 56 CPUs, 186 GB) are the largest uniform group, so
every reported job asks for `--constraint=cascadelake --exclusive`: one CPU model for every result
and no other jobs on the node. Binaries: the system GCC 11.5, `release` preset (-O2 -g -DNDEBUG).
Datasets are downloaded to /scratch on the login node and extracted to node-local /tmp (xfs, about
290 GB free on the nodes seen); disk-mode repositories also live on /tmp. Every result records the
node, CPU, cores, memory, kernel, file systems, compiler, and flags.
Found in the final check (2026-10-10): node d0020's system compiler is GCC 11.4.1, not 11.5, with
the same kernel. Five jobs at commit 620f2a6 ran there (10949339 E1, 10949358 E2, 10949418 E3,
10949428 E6, 10949230 E8), and their result files record 11.4.1. RESULTS.md shows those runs for
E1, E2, E6, and E8 (E3's latest run is a later one). Dedup ratios do not depend on the compiler;
E2 and E6 compare configurations built by the one compiler in their job. The jobs do not check the
compiler version. No more reported runs are planned, so no check was added.
Revisit if: cascadelake nodes become hard to get; then pin another uniform group and rerun all.
Or if more reported runs are planned: make the jobs fail unless the compiler is GCC 11.5.

**D95 — Benchmarks run in a C++ driver; Python only finalizes and reports.**
`mem` mode needs a repository on MemFs and E7 needs SimulatedLink, both reachable only in-process,
so `dds_bench` (C++) runs the measurements and writes samples; `tools/finalize_result.py` adds the
machine and build record and the summary; `tools/report.py` writes RESULTS.md. Run ids are UTC
timestamp + short commit + node, as section 2 says, plus a tag naming the configuration, because
one job writes several result files in the same second. A file name that exists is an error, never
an overwrite.
Revisit if: never; this records the tool split.

**D96 — E3's component times come from counters in the backup; `perf` is tried in the job.**
`BackupTimings` sums, over workers, the time in reading and chunking, hashing, and storing (three
clock reads per chunk, about 1% at 8 KiB), plus walk, tree, and commit time on the calling thread.
`perf` exists on Explorer and `perf_event_paranoid` is 2, which allows user-space sampling of one's
own processes; the E3 job records the profile, or that it failed.
Revisit if: the counters' overhead shows up in E3 (compare with the profile).

**D97 — E8's checkpoints are produced on Explorer by fine-tuning.**
BENCHMARKS.md section 4 first expected existing checkpoint paths; instead the checkpoints are
produced on Explorer in /scratch and the paths reported (2026-10-08). The study fine-tunes
distilgpt2 (82M parameters) on text from the Linux 6.10 Documentation directory, already downloaded
for the kernel dataset: a full fine-tune with seeds 0, 1, and 2, a partial fine-tune of the top 2 of
6 blocks, and LoRA, each saving 6 checkpoints (300 steps, one every 50). It trains on one GPU of
`gpu-short`. The first attempt trained on CPU (`short`, 32 cores, not exclusive) and timed out after
8 hours with 150 of 1500 steps done (job 10930678): the node's other job drove it into swap. On a
quiet node the same step took 1.5 s (job 10930971), so CPU training was viable in principle, but a
GPU job is shorter and cannot share a node's CPUs with such a neighbour; `train.py --device cuda`
fails rather than falling back to the CPU. Explorer's `anaconda3/2024.06` module has torch 2.8.0,
transformers 4.42.4, and safetensors but not `accelerate` (which Hugging Face's Trainer needs) or
`peft`, and the login node killed an attempt to create a virtual environment for them. The module
also has numpy 2.4.4, which transformers 4.42.4 refuses at import ("numpy>=1.17,<2.0 is required",
job 10930387). So `setup.sh` downloads the numpy 1.26.4 wheel (what Anaconda 2024.06 shipped),
checked against PyPI's SHA-256, and the training job unzips it into its node-local directory and
puts it first on `PYTHONPATH`; the login node killed both `pip install` and `unzip` part-way.
Without the Trainer, `bench/datasets/ckpt/train.py` runs its own AdamW loop with linear warm-up and
decay, implements rank-8 LoRA in about 20 lines, and writes each checkpoint in the Trainer's layout
(model or adapter safetensors, optimizer, scheduler, RNG state, trainer state, configs, tokenizer).
Only the model download runs on the login node. This is dataset tooling outside the C++ build, like
`fetch_kernel.sh`; D4 governs the C++ dependencies only.
Revisit if: checkpoints from a real training project become available; then run E8 on them as
well.

**D98 — A backup worker lends one read buffer to every file's ChunkReader.**
`ChunkReader` needs 2 x the maximum chunk size of buffer (at least 256 KiB) so the chunker always
sees a full window. It allocated that per stream, and `std::vector` zero-fills it, so a backup paid
one allocation and memset per file. E2 found it: at a 1 MiB average (8 MiB maximum) the 16 MiB
per kernel file cut ingest to 93 MB/s from about 3000 at 64 KiB (job 10930691), and E3's profile
put `memset` at 14.5% of samples at the default 8 KiB (job 10930705). A second constructor borrows
a caller-owned vector and grows it only when it is too small; each worker owns one for the whole
backup. Considered and rejected: sizing the buffer to the file (still one allocation per file) and
uninitialized allocation (`make_unique_for_overwrite`, whose availability across the three standard
libraries D3 names was not verified). The test
`ChunkReaderTest.ABorrowedBufferYieldsTheSameChunksAndIsAllocatedOnce` fails if the buffer is
reallocated per stream.
Revisit if: a profile shows the first file's one-time zero-fill per worker, or memory per worker
(16 MiB at the largest allowed maximum) matters.
Measured on one node (E2ab, E2ab-lock): 26.5x at a 1 MiB average and 1.90x at 64 KiB with 4
threads, but 0.84x at 64 KiB with 16 threads, where removing the work exposed contention (D100).

**D99 — A bug found by a measurement is fixed, and the affected experiments are rerun.**
Results are append-only (BENCHMARKS.md section 2), so the first runs stay in `bench/results/` and
RESULTS.md shows the latest run of each configuration. The expected outcome of the rerun is
committed before it runs, like any prediction (for D98: E1, E2, E3, and E6, in the introduction to
BENCHMARKS.md section 5). Each outcome names the runs it rests on, and the first run's numbers stay
quotable as the "before" measurement. Experiments whose measured quantities the fix cannot change
(E4 restore, E5 GC, E7 transfer, E9 open, the microbenchmarks) are not rerun.
Revisit if: a fix changes what an experiment measures, not only how fast; then rerun everything
that depends on it and say so in each outcome.

**D100 — Ingest's measured scaling limit is lock contention in the store path, most likely the
session mutex; the fix is proposed here and made later (D101).**
Evidence: E3's mem-mode ingest peaks at 16 threads and falls at 32 while time summed in `store`
grows from 1.4 to 26.2 s; after D98 removed per-file work, 64 KiB ingest at 16 threads got 16%
slower, with threads blocked half the time and mutex functions at 5.7% of samples (E2ab-lock, job
10953982). `IngestSession::Lookup` and `Put` take the one `session_mutex` for every blob of every
worker, and `Lookup` holds it across an index probe (ARCHITECTURE.md 4.1, lookup protocol). The
profile cannot attribute the waiting to that mutex (user-space sampling, no frame pointers), so
this is the prime suspect, not a proven cause. Proposed fix: shard the pending map and its mutex
by fingerprint, like the index. The lookup protocol's guarantee ("a session never stores the same
new blob twice") is per fingerprint, so sharding keeps it, and the seal protocol removes entries
shard by shard. Not done with this decision: it changes the locking of the path GC's safety
argument depends on (ARCHITECTURE.md 6.2), needs its own tests (TSan, the GC interleaving tests, a
scaling run), and is an optimization.
Revisit if: a profile built with frame pointers names a different lock.
Done on 2026-10-10: D101 records the design, and BENCHMARKS.md section 5 its effect.

**D101 — The session's pending map is 256 shards by fingerprint byte 0, each under its own
mutex.**
The single session mutex guarded two guarantees, both about one fingerprint at a time: a lookup
miss reserves the fingerprint so that a session stores each new blob once (ARCHITECTURE.md 4.1,
lookup protocol), and a fingerprint is always in the pending map or the index (seal protocol).
Locking only the fingerprint's shard keeps both. The scheme copies `ChunkIndex` (D17): 256
`alignas(64)` shards chosen by `Fingerprint::ShardByte()`, every shard mutex at the session level
(40), so the index shard (60) still nests inside for the probe, and the lock-order checker forbids
holding two session shards. The seal step erases entries one shard lock at a time, after
publishing, as before. Cost: 256 empty maps and mutexes, about 32 KiB per session (16 replication
sessions on a server: 512 KiB).
Considered and rejected: probing the index before taking the session lock (a fingerprint sealed,
published, and erased between that probe and the pending check would be reserved and stored a
second time); a reader-writer lock (lookups write: a miss reserves); a concurrent hash map
library (D4 allows no dependency).
Proved by `IngestSessionTest.ConcurrentWorkersReserveEachFingerprintExactlyOnce` (8 workers, 4001
shared fingerprints, writers sealing throughout), which fails at its exactly-once assertion for a
shard chosen by thread, for erasing before publishing, and for unlocking between the pending check
and the reservation; and by the existing GC interleaving, stress, and crash tests under TSan.
Revisit if: a profile shows a session shard hot (one fingerprint byte value dominating), or a
measurement shows the next limit elsewhere (BENCHMARKS.md section 5).
Measured (BENCHMARKS.md section 5): alone, the sharded map sped up new-data ingest but slowed
dedup-heavy ingest at 16 and 32 threads (S7ab, 0.67 to 0.87). Together with the batched file queue
(D102), ingest ran at 1.19 to 2.60 times the build before the sharded map (S8ab), so it stays. On
top of the batched queue alone (S8alone, job 10974438), the sharded map adds 1.07x at 64 KiB with 4
threads, 1.33x to 1.43x at 64 KiB with 16 and 32 threads, and 1.83x
to 2.01x at 8 KiB: it matters most where per-chunk lookups are densest.

**D102 — The walker queues files to the workers in batches.**
The sharded pending map (D101) removed the session lock's waiting but made dedup-heavy ingest
slower at 16 and 32 threads
(S7ab, job 10957115; E3's second backup 2607 to 2226 MB/s at 16 threads). Workers with little to
store outran the one walker, every file passed through one queue lock (one push, one pop), and
fewer CPUs were busy. The choice (2026-10-10) was to keep D101 and remove that per-file round trip.
The walker fills a batch of up to 64 files and pushes it with one queue operation; a worker pops a
batch and stores its files in order. A batch also closes once its files' listed sizes reach
16 MiB, so a run of large files still spreads across workers, and at 64 small files a batch is at
most a few milliseconds of one worker's time. The queue keeps its bound of 4 x workers entries,
now batches (at most 4 x workers x 64 queued files, a few hundred KiB of task records). The
partial batch is pushed when the walk ends; a walker or worker failure behaves as before (D79).
The hypothesis behind this, that the queue's per-file round trip limits dedup-heavy ingest, was
not measured directly (user-space profiles only); S8ab tests it by its effect.
Considered: several walker threads (more change: directory order and the walker's tree recording
would need coordination), per-worker queues with stealing (more code than the measured problem
needs). Proved by `BackupBatchTest.*` (batch boundaries, final flush) and the D79 test, now with
2000 files so the walker is blocked on a full queue when the worker fails.
Revisit if: S8ab shows dedup-heavy ingest still slower than before D101, or a batch's size shows up
as tail latency (the last batches finishing on few workers).
Measured (S8ab, job 10971699; BENCHMARKS.md section 5): with D101, ingest ran at 1.19 to 2.60
times the build before D101 in every configuration, and E3's dedup-heavy backup at 16 threads ran
at 5448 MB/s against 2607 before D101. Every prediction hit, so D101 and D102 both stay. Without
D101 (S8alone, job 10974438), batching alone ran at 1.10 to 1.91 times the build before D101, most
with large chunks
(64 KiB), where a file has few chunks and the per-file round trip weighs most.

**D103 — CRC32C uses the CPU's CRC32C instructions when it has them.**
CRC32C was 13.5% of the user-space
samples of ingest at 64 KiB with 16 threads (S8alone's profile of the current build, job 10974438),
at 2.2 GB/s in slicing-by-8 (micro, job 10930767). Both target CPUs have an instruction for exactly
this CRC: SSE4.2's `crc32` on x86-64 (Intel SDM vol. 2A) and the ARMv8 CRC32 extension, through the
ACLE intrinsics `__crc32cd` and `__crc32cb`. Their signatures were read in clang's `crc32intrin.h`
and `arm_acle.h` **(checked 2026-10-10)**. On x86-64, one function is compiled with
`__attribute__((target("sse4.2")))` and chosen at run time with `__builtin_cpu_supports("sse4.2")`,
so the binary still runs on a CPU without SSE4.2. On ARM, the path is compiled only when the compiler
defines `__ARM_FEATURE_CRC32`, which it does only when every CPU it targets has the extension (Apple
clang on arm64 macOS does; checked with `clang -dM -E`), so there is no run-time check. One stream
of 8-byte instructions, not three interleaved streams merged with carry-less multiplies: on Cascade
Lake the instruction has a latency of 3 cycles and a throughput of one per cycle (uops.info,
CRC32 (R64, R64), **checked 2026-10-10**), so one stream does 8 bytes per 3 cycles, about 8 to 10
GB/s at this CPU's clock. That cuts CRC32C from about 5.5 of the 60 CPU-seconds in that profile to
about 1.4, and interleaving could save about 1 more at the cost of the hardest code in the
repository. Before its first use the hardware is checked against the check value 0xE3069283, and a
mismatch aborts (`DDS_CHECK`: an instruction computing the wrong CRC breaks an internal invariant,
it is not bad input), so a misdetected or miscompiled path cannot write wrong CRCs into containers.
Tests compare both the dispatched version and slicing-by-8 with the bytewise version on random input
(D26); every benchmark result records `params.crc32c`.
Revisit if: CRC32C is still above 5% of an ingest profile, or a target CPU lacks the instructions.
Measured: S2ab (job 10976818) found the SSE4.2 path at 1.77 GB/s, slower than slicing-by-8, because
GCC 11.5 compiled `LoadU64`'s loop as eight byte loads (a miss); the loads were rewritten as one
expression (commit 4721630). S2ab2 (job 10978928): 10.64 GB/s against 2.66 for slicing-by-8, ingest
1.02 to 1.16 times the build before D103, and CRC32C below 0.5% of the profile.

**D104 — The index checkpoint: one derived file, trusted per covered container id, never the
source of truth.**
Built on 2026-10-10 although E9 put 500 GB at 4.5 s, under D10's 5-second line. `<repo>/index.ckpt`
(ON_DISK_FORMAT.md section 9) holds every index entry of the containers it covers, grouped by index
shard, and each covered container's sizes. Open loads it with up to 16 threads, one shard at a time
per thread, each shard's table sized first with `FlatTable::Reserve` so it never rehashes, and reads
the footers only of containers it does not cover (ARCHITECTURE.md 7.6, 7.8).
- **Trusting a covered id without reading the container** is sound because container files are
  immutable (D10) and ids are never reused: open sets the next id to at least the checkpoint's
  next_container_id, even when it does not load the entries. Without that, GC deleting the newest
  containers and a reopen would hand their ids to new containers, and the next open would take the
  old entries for them and never read the new footers
  (`IndexCheckpointTest.IgnoringTheCheckpointsNextIdLosesNewData`, mutation
  `checkpoint_next_id_ignored`).
- **The writer covers** the containers published and not retired when its scan starts, and holds
  gc_mutex for the whole write: no entry pointing into a covered container moves or leaves the
  index meanwhile, sessions only add entries, and every entry of a covered container was in the
  index before that container was published (D49). GC's compactor relocates before it publishes a
  copy, which would also make a scan without the lock safe; the lock keeps the argument independent
  of that order.
- **Close writes it** when at least 256 containers (`checkpoint_stale_containers`, about 1 GiB of
  new data at the default container size) are published but not covered, or covered but deleted
  since. Small repositories never write one, and a large one is not rewritten after every small
  backup. `Repository::WriteIndexCheckpoint` writes one on demand. The write follows 7.1 (tmp/,
  sync, rename, sync the directory) in appends of about 1 MiB.
- **A checkpoint that fails any check is removed** and open rebuilds from every footer. That
  includes one whose bytes did not survive a crash, which fails its CRCs, so no ordering of its write
  affects correctness; the sync and rename only keep it usable. `CrashTest.IndexCheckpoint` has no
  mutation for that reason.
- **What changes for open.** It no longer re-reads the footers of covered containers, so damage to
  one is found by blob reads (payload CRCs) and by `verify`, which reads every footer and now also
  checks every index entry against its container's footer. Where GC deleted a covered container
  holding the indexed copy of a dead blob, a second copy in another covered container is not
  re-indexed, as a rebuild from footers would do; the running process does not index it either, and
  nothing reachable depends on it (`IndexCheckpointTest.ContainersGcDeletedAfterTheCheckpointAreDropped`).
- **Cost:** 49 bytes per blob on disk (about 560 MB at 100 GiB of unique data), written by one
  thread.
Revisit if: S1open measures open with the checkpoint less than 1.5 times as fast as without at 100
GiB, or the write costs more than the opens it saves before the next write.
Measured (S1open, job 10977387; BENCHMARKS.md section 5): open 3.86 times as fast at 100 GiB (1.10
to 0.29 s) and 2.04 to 3.43 times elsewhere, the same index either way; writing the checkpoint took
5.28 s at 100 GiB, paid back after about 6.5 opens.

**D105 — Each Explorer job builds its own commit's source directory.**
A job reads its source when it starts, not when it is submitted, and until now every upload replaced
the one shared `/scratch/<user>/dedupstore/src`. With the exclusive cascadelake nodes busy, a job
can wait for hours (S2ab, job 10976818, was given an estimated start about 10 hours after it was
submitted), and an upload for the next experiment in that time would change what the queued job
builds, so its result would no longer belong to the commit its prediction was written for. Now
the submission scripts upload the commit (HEAD, or `DDS_COMMIT`) to `src-<7 hex digits>`, the
directories the same-node comparisons already used for their old builds (D99), and submit with
`--export=ALL,DDS_SRC_DIR=<that directory>`; every job script sources `common.sh` and `env.sh`
from there, and `env.sh` sets `DDS_SRC` to it. Jobs submitted before this change (S2ab) read the
shared `src`, so nothing was uploaded there until they had started.
Revisit if: the `src-*` copies (a few MB each) crowd the /scratch quota; then remove those of
finished jobs.

**D106 — A backup may skip files unchanged since a parent snapshot: same size and mtime, mtime
older than the parent's start.**
A nightly backup of a tree that barely
changed reads and hashes every byte only to find it stored already (E3: the dedup-heavy second
backup to disk runs at 500 MB/s with one thread). With `BackupOptions::parent` (CLI `--parent`,
default `auto`: the newest snapshot this repository made of the same path on the same host), a file
whose parent entry has the same name, size, and mtime to the nanosecond is not read; its parent's
recipe pieces are reused (ARCHITECTURE.md 4.1).
- **The racy window.** File systems stamp mtimes from a coarse clock (Linux: the timer tick, a few
  milliseconds; FAT: 2 s), so a file rewritten at the same size within one tick of the parent
  reading it keeps the same mtime, the problem git calls "racy" index entries. So a file is skipped
  only if its mtime is more than 2 s older than the start of the parent's backup, which manifest
  version 2 records (`started_at_ns`, ON_DISK_FORMAT.md section 8): any change after the parent
  listed it then has a newer mtime. Version 1 manifests decode unchanged; a version 1 parent has no
  start time, so nothing is skipped against it. `created_at_ns` is taken at the end of a backup,
  too late to bound the window, and redefining it would have changed what old snapshots mean.
- **Why not the inode and ctime**, which restic compares: the tree
  format records neither, and recording them would change every tree's fingerprint whenever a file
  is restored, copied, chmod-ed, or hard-linked, though its content did not change, so trees would
  stop deduplicating across those operations. The cost of leaving them out is the documented
  limitation (SPEC.md section 4): a rewrite that keeps the size and sets the mtime back is not
  noticed (`BackupWithParentTest.AChangeThatKeepsSizeAndMtimeIsOnlySeenWithoutAParent`); a tool
  that does that on purpose needs `--parent none`. The test also assumes the file system's clock is
  within 2 s of this host's.
- **GC safety.** The requirement: every reused blob is stamped. The worker looks up
  each reused recipe and every chunk it lists through the session (`Reserve::kOff`), exactly as if
  it had read the file, and falls back to reading on any miss. So a reused file adds a fourth way
  for a session to reference a blob to ARCHITECTURE.md 6.2 item 3, with the same stamp and the same
  proof. `GcInterleavingTest.ReusedBlobsSurviveRemovingTheParentAndGc` pauses a backup after the
  lookups, removes the parent, runs a full GC, and resumes; mutation `reuse_without_stamp` makes it
  fail. The parent's trees are only read, never referenced by the new snapshot, so they need no
  stamp; one that GC removed meanwhile just means reading the files below it.
- **Same snapshot as a full read.** Reused pieces are what reading the unchanged file would produce
  with this repository's chunker, so the trees, and the root fingerprint, equal those of a backup
  without a parent (`BackupWithParentTest.AnUnchangedTreeIsNotReadAndGivesTheSameSnapshotRoot`).
  `auto` only picks snapshots this repository created, so their pieces come from its own chunker.
Revisit if: S5parent shows a backup with a parent no faster than one without at one thread, or a
platform where file mtimes come from a clock that can lag this host's by more than 2 s.
Measured (S5parent, job 10978179, and S5parent2, job 10982725): the second backup of linux-6.10 is
9.06 times as fast with the parent at 1 thread and 2.60 at 4 (7.08 and 2.31 with 1% of files
modified). At 16 threads, where both are bounded by the walker and reading from the page cache costs
almost nothing, it is 0.85 to 0.91 times as fast: the workers wait on shared mutexes, the epoch
mutex (now taken once per batch, 64c596a) and the container set's handle cache, which remains. The
loss there is about 40 ms on 84,905 files.

**D107 — zstd per blob, after dedup, chosen at init; containers and config at version 2.**
zstd is the one dependency added beyond D4's three (zstd 1.5.7 from its release tarball, SHA-256
checked against the published `zstd-1.5.7.tar.gz.sha256`, static library only, legacy formats and
multithreading
off).
- **After dedup, per blob.** Fingerprints still name the content, so dedup does not change, and
  each blob is compressed on its own, so a read decompresses one blob, not a container. A blob is
  stored as a zstd frame only if the frame is smaller than the content; already-compressed data
  (archives, media, most model weights) is stored as it is and costs one failed attempt.
- **What a reader sees.** `ContainerSet::Read` checks the CRC32C of the bytes as stored, then
  decompresses, so restore, verify, GC's copy-forward, and the replication sender all handle
  content. Replication therefore sends content and each repository compresses by its own setting:
  the protocol does not change (`ReplicationTest.CompressionIsEachRepositorysOwnChoice`).
- **Formats.** The repository chooses at init: `config` version 2 records `compression=none` or
  `zstd:<level>` (levels 1 to 19; above 19 zstd needs far more memory per context). Container
  footer entries (version 2, 50 bytes) record each blob's codec and content length. The index
  entry stores the codec in a byte that was padding, so it is still one cache line; there is no
  room for the content length, so the index holds stored lengths. Version 1 configs and containers
  are still read (golden tests); the index checkpoint, derived, moves to version 2 without reading
  version 1 (open removes it). Signatures of `ZSTD_compressCCtx`, `ZSTD_decompressDCtx`,
  `ZSTD_getFrameContentSize`, `ZSTD_findFrameCompressedSize`, and `ZSTD_compressBound`, all stable
  API, were read in zstd 1.5.7's `zstd.h` **(checked 2026-10-10)**.
- **Lengths.** Two checks compared an index length with a recipe's content length and would have
  called every compressed chunk corrupt: verify's DAG walk now compares only blobs stored as
  content (a deep walk hashes the others), and the replication receiver's COMMIT check reads a
  compressed chunk's frame header for its content size (`ContainerSet::ContentLength`), so a
  hostile peer still cannot commit a recipe with a wrong length
  (`WrongChunkLengthTest.CommitIsRefused/Zstd`). `stats` takes content bytes from the recipes and
  reports a compression ratio apart from the dedup ratio (D40).
- **Decoding untrusted frames.** A frame must be exactly the stored bytes, record a content size
  of 1 to the blob's limit, and decode to that size; anything else is Corruption, never a crash
  (`fuzz_decompress`, seeded with frames from the zstd command-line tool).
- **Contexts.** Each writer owns one compression context and each thread one decompression
  context, because zstd allocates hundreds of KB per context.
Revisit if: S3zstd shows zstd:1 slowing ingest by more than half, or a repository's data compresses
so little that the failed attempts cost more than they save.
Measured (S3zstd, job 10982306): dedup unchanged at every level; kernel source stored 3.43 to 3.77
times smaller, checkpoints 1.09; kernel ingest at 0.67 of uncompressed with zstd:1 and 0.63 with
zstd:3, restore at 0.63 with zstd:3.

**D108 — Subtree pruning by checking each subtree when asked, not by a GC invariant; protocol
version 2.**
The original plan worded subtree pruning as "with a 'complete subtree' invariant maintained by GC"
(D29). E7's kernel 6.0 to 6.1 transfer met D29's
revisit condition in kind: the second snapshot's HAVE queries were 8.1 MB, 4.4% of its wire bytes,
and they stay 8.1 MB however small the change, so for a few edited files they are nearly all of
it.
- **Why not the invariant.** "A tree is present only if everything below it is" fails in three
  places, not one. (1) GC: a garbage tree and its garbage children usually sit in different
  containers, and compaction drops a container's garbage only when its live fraction is under the
  threshold, so a child can go while its parent stays. Keeping the invariant would mean either
  never collecting anything below a present tree, or dropping every garbage tree above each
  dropped blob, rewriting containers that are otherwise above the threshold, in a crash-safe order.
  (2) Replication itself: the sender walks depth-first, a tree before its contents, and a session
  that ends early keeps what it received (PROTOCOL.md 4.2 step 6), so a tree arrives before its
  children whatever GC does. (3) A receiver could not trust the invariant for blobs a peer sent
  anyway (G8).
- **Chosen: check when asked.** New message HAVE_TREES asks, for each tree, whether it and every
  blob below it are present. The receiver answers by walking the subtree with the COMMIT walk's
  checks (presence, types, parsable trees and recipes, chunk lengths), each lookup through its
  session, so everything it found is stamped and kept until the session ends (ARCHITECTURE.md 6.2
  item 3, the argument backups and HAVE already rely on). COMMIT then skips the trees it found
  whole. No invariant, no GC change. Mutation `subtree_check_without_stamp` (lookups that do not
  stamp) makes `ReplicationPruningTest.WithoutTheCheckStampingGcLosesThem` commit a snapshot GC has
  already destroyed.
- **What it costs and saves.** The receiver reads every tree and recipe of a subtree it finds whole,
  as the COMMIT walk would, and only once per session, so its work moves earlier rather than
  growing; it no longer answers a HAVE for each of those blobs. The sender reads every tree, as a
  walk without pruning does, but no recipe below a pruned tree, and sends 32 bytes per tree instead
  of per blob: for linux-6.10, about 6,000 trees instead of about 290,000 blobs.
- **One pass, not level by level.** The first version asked one level of the directory tree per
  round trip, asking only about trees below a tree that was not whole. Fewer questions, but a change
  d directories deep cost d + 1 round trips before any HAVE: the laptop smoke test (SDK framework
  headers, 2,277 trees, 15 files changed, 40 ms one way, unlimited bandwidth) took 0.81 s against
  0.46 s without pruning. Asking about every tree at once costs one round trip, so the same run
  took 0.455 s against 0.461, and twice as fast as without pruning at 10 Mbit/s. The sender holds
  every tree's fingerprint and subdirectory list for the pass, a few dozen bytes per directory.
  Each batch of up to 4096 trees goes out as soon as its trees are found: the list only grows by
  appending, so every tree in it is final. The first version read every tree before sending any,
  a silence that grows with the snapshot and, for hundreds of thousands of directories on a cold
  cache, would outlast the receiver's 60-second idle timeout every time (found by review after
  S4prune; streaming also overlaps the round trips with the reading). S4prune2 (job 11002706)
  measured the reviewed code against S4prune's on one node: identical bytes in every cell, and
  0.944 to 1.000 of the time.
- **Versions.** HELLO proposes 2 when pruning, 1 otherwise; the receiver accepts 1 or 2 and speaks
  the proposed version. A receiver built before version 2 refuses HELLO(2) as Incompatible; `dds
  replicate --no-prune` reaches it. An automatic retry at version 1 was left out: it needs a second
  connection and would hide that the receiver is out of date.
- **Edges.** The walk uses an explicit stack (a peer may send a chain of 50,000 nested trees,
  `AVeryDeepChainOfTreesIsCheckedWithoutRecursion`). A subtree that needs a blob received in this
  session but not yet sealed is answered "not whole", which costs only queries: such a blob has no
  index entry, so its type and length cannot be checked until COMMIT seals it. The first version
  got this wrong for chunks: it skipped an unsealed chunk, as the COMMIT walk does (safely there:
  COMMIT seals everything first, and a HAVE miss reserves nothing, D55), so a peer could send a chunk the recipe gives the wrong length (or a tree referenced as a chunk), have
  HAVE_TREES find the subtree whole, and COMMIT skip it, committing a snapshot that cannot be
  restored. A review found it after S4prune; a HAVE_TREES check now fails on an unsealed chunk
  (`AWrongLengthBehindAnUnsealedChunkIsRefused`, `ATreeReferencedAsAChunkBehindAnUnsealedBlobIsRefused`,
  `AnUnsealedChunkIsNotFoundWhole`, all three failing without the fix). S4prune is unaffected: its
  sender asks HAVE_TREES before any PUT. The memo of answers and recipes checked lasts one session
  and holds only trees held here or named by trees held here, and recipes that passed: a
  fingerprint a peer made up is answered not whole and leaves nothing behind. The first version
  remembered every answer, so a peer looping HAVE_TREES with made-up fingerprints grew the
  receiver's memory without bound, up to taking down `dds serve`; the same review found it
  (`MadeUpFingerprintsAreNotRemembered`, with `ReceiveReport::trees_remembered`).
Revisit if: S4prune shows pruning slower than not pruning on any link it measures, or a snapshot
whose directories outnumber its other blobs (where 32 bytes per tree saves nothing).
Measured (S4prune, job 10999082): with 15 files changed in linux-6.10, 39.6 times fewer query bytes
and 1.75 to 9.78 times as fast, the PUT payload unchanged. Linux 6.0 to 6.1, where release tarballs
give every file a new mtime so no subtree is whole, pruned nothing and ran at 0.98 and 0.99 of the
speed without it: the condition is met. The time went to the pass itself, one round trip and 32
bytes per tree (0.135 s of 15.32 at 40 ms and 100 Mbit/s). Pruning stays on: what it can lose is
bounded by that pass, about 2% of a transfer in which every directory changed, while what it gains
on small changes is several-fold. Overlapping the pass with the start of the walk would remove the
round trip; not done.

**D109 — Large files: chunked by one worker, stored by all, in segments.**
D31 parallelizes across files, so one
large file uses one core: E3 measured 432 MB/s for one worker in memory (per 1.43 GB: 0.85 s reading
and chunking, 0.75 s fingerprinting, 1.40 s storing), against 3,827 MB/s for sixteen.
- **Why not chunk ranges of the file in parallel** (the original plan's framing). A FastCDC cut
  depends on the previous cut (the minimum-size skip, the mask switch at the normal size, the
  maximum size), so a range chunked from its own start disagrees with chunking the whole file until
  the two sequences of cuts happen to meet. Stitching needs a serial re-chunk from the last true cut
  to a common cut, and on content where they never meet (all zeros: a cut every maximum size from
  the previous one) whole ranges are re-chunked. Correct with that fallback, but intricate to show
  correct.
- **Chosen: serial chunking, parallel storing.** The worker that takes the file reads and chunks it
  alone: FastCDC runs at 3.3 GB/s on one core (BM_FastCdc), 7.6 times one worker's ingest rate. It
  takes the chunks in runs of at least 1 MiB; any worker may fingerprint (2.2 GB/s per core,
  BM_FingerprintOf) and store a run through its own writer. Boundaries are exactly those of chunking
  the whole file, and the splitter adds each run's references to the recipe in file order, writing
  a piece every 4096 as before (D14), so recipes and roots are byte-identical
  (`BackupSplitTest.SplittingDoesNotChangeTheSnapshot`).
- **A run is copied only for an idle thread.** Before each run the splitter asks whether more
  threads wait for work than segments are queued. If so, it copies the run's chunks into a segment
  and queues it; if not (or the idle thread found other work meanwhile), it fingerprints and stores
  them itself, straight from its read buffer, as for an unsplit file. Pushing never blocks, so a
  splitter never waits for room that only workers make; when every worker is busy, as with many
  large files at once, splitting costs neither a copy nor memory (laptop smoke test, eight 128 MiB
  files on 8 threads: 5,379 and 5,443 MB/s split against 5,421 and 5,392 unsplit). A splitter waits
  for its oldest queued run storing other queued segments meanwhile, and keeps at most workers + 1
  runs of one file in flight. Queued segments are taken before file batches, because a splitter is
  waiting for them.
- **Workers stay until every batch taken is done.** The first version let idle workers exit once the
  walk had ended and the queue was empty. With a tree of one large file the walk ends before its
  worker even opens it, so no run was ever queued: the laptop smoke test queued 0 segments and ran
  at 599 MB/s on 8 threads against 738 unsplit, the copies costing and nothing gained. A count of
  files being split did not fix it (the walk can still end before the count is raised); counting
  batches taken and not yet done does, since a file can only start splitting inside one. With it,
  the same test queued 510 of 512 runs and ran at 2,026 MB/s against 795.
- **Threshold.** 64 MiB by default (`BackupOptions::split_file_bytes`): about 0.15 s of one core,
  long enough for the segments to spread. One worker never splits. The benchmark driver defaults to
  never splitting, so experiments from before D109 rerun as they ran.
- **Costs.** One more copy of every byte of a run handed to another worker. A split file's chunks
  land in several workers' containers in runs of about 1 MiB, instead of one writer's stream, so the
  restore locality D31 gives a file is weaker. GC safety is unchanged: segment chunks go through the
  same session lookups with reservation (ARCHITECTURE.md 6.2 item 3), and the GC stress test splits
  files.
Revisit if: S6split shows a split file's ingest held well below the many-files rate by the serial
reading and chunking (then chunking ranges, with stitching, would pay), or restores of split files
markedly slower than of the same files unsplit.
Measured (S6split, job 10999396): the same roots with and without splitting; one 8 GiB file 1.53
times as fast at 4 threads and 1.88 at 16, checkpoints 1.31 and 1.85, below the predicted 1.8 to 2.0;
restores of a split file 0.956 of unsplit. The first condition is met: the splitter's reading and
chunking alone (1.59 GB/s) holds a split file to about 1.14 GB/s, against 4.2 GB/s for many files.
Splitting stays: it was faster in every configuration measured and roots do not change. The D109
code costs something even with nothing split: linux-6.10 at 16 threads ran at 0.977 of the commit
before, lower in both passes, within the predicted 0.97 to 1.03 (not profiled). Chunking ranges in
parallel is what would lift the reader's limit, and is not done.
Measured again (S6split2, job 11002248): reserving each segment's buffer once (df41430) made split
ingest 1.123 times as fast for the 8 GiB file and 1.136 for the checkpoints, every prediction hit;
the splitter's time outside reading and chunking fell from about 2.1 to 2.4 s to about 1.2 to
1.4 s, and the split file ran at 2.41 times the unsplit one.

## License

**D43 — License: MIT (`LICENSE`).**
Decided on 2026-10-10.
Revisit if: the license is ever changed.
