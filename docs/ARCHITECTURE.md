# dedupstore — architecture

How the system is built. SPEC.md says what it guarantees; this document says why the design keeps
those guarantees. Decision numbers (Dn) refer to DECISIONS.md.

## 1. Module map

```
src/dds/
  common/    Status, Result<T>, DDS_CHECK, DDS_TRY, little-endian encoding + bounds-checked Reader,
             CRC32C, logging, thread-safety annotation macros, OrderedMutex, UniqueFd, Clock,
             epoch.h (the Epoch and GcRun types), ProcessIdentity, test hooks
             (DDS_MUTATION, DDS_SYNC_POINT)
  fs/        Fs interface; PosixFs; MemFs; CrashSimFs (test support, but lives here so the
             conformance suite covers it)
  hash/      Fingerprint, BlobType, Blake3Hasher
  chunk/     Chunker interface, FastCdcChunker, FixedChunker, GearTable, ChunkReader
  store/     ContainerWriter, ContainerReader, ContainerSet (ids, published set, read-handle cache,
             retirement)
  index/     FlatTable (open addressing), ChunkIndex (sharded, epoch stamps, GC marks)
  snapshot/  Tree / Recipe / Manifest codecs; SourceTree + PosixSourceTree + MemSourceTree;
             RestoreSink + PosixRestoreSink + MemRestoreSink
  repo/      Repository (init/open/close and the snapshot operations), RepoConfig, RepoLock,
             EpochManager, ReadGuard, SnapshotStore, IngestSession, backup (RunBackup),
             BlobReader, Restorer, DagWalker (shared by Verifier and Stats), Verifier, Stats
  gc/        Marker, Planner, Compactor, GcRunner
  repl/      Message codecs, Connection interface, TcpConnection, SimulatedLink, Sender, Receiver,
             Server
  cli/       argument parser, one file per command, main.cc
```

Dependencies point one way:
`common` ← `fs` ← `hash` ← `chunk` ← `store` ← `index` ← `snapshot` ← `repo` ← `gc` ← `repl` ← `cli`.
A module may use anything to its left and nothing to its right (D53). Consequences:
- Types that lower modules need from higher-level concepts (`Epoch`, `GcRun`) live in `common`.
- Backup and restore live in `repo`, because they need `IngestSession` and `ReadGuard`.
  `snapshot` holds only formats and the user-file interfaces.
- `Repository` never calls GC or replication. `gc::GcRunner` and the `repl` classes take a
  `Repository&` and reach its parts through `Repository::Internals()` (the index, the container
  set, the epoch manager, the snapshot directory), which only `gc` and `repl` use. The CLI and the
  server construct `GcRunner`. `repl::Server` may run GC, so `repl` depends on `gc`.

Tests live in `tests/<kind>/` (TESTING.md). Benchmarks live in `bench/` (BENCHMARKS.md).

## 2. Key interfaces

Signatures are the intended shape. Exact parameter lists may change during implementation; record
any material change in DECISIONS.md.

```cpp
namespace dds {

// ---- common ----
enum class StatusCode : uint16_t {     // explicit values: they cross the wire in PROTOCOL ERROR
  kOk = 0, kNotFound = 1, kCorruption = 2, kIoError = 3, kInvalidArgument = 4,
  kAlreadyExists = 5, kBusy = 6, kAborted = 7, kIncompatible = 8 };
class [[nodiscard]] Status { /* code + message; static factories Status::Corruption(...) ... */ };
template <class T> class [[nodiscard]] Result { /* holds T or a non-OK Status */ };
using Epoch = uint64_t;                // section 6
using GcRun = uint32_t;                // GC run number; starts at 1, a fresh entry's mark is 0
class ProcessIdentity {                // injectable so the crash harness can simulate a dead holder
 public:
  virtual ~ProcessIdentity() = default;
  virtual std::string Host() const = 0;
  virtual uint64_t Pid() const = 0;
  virtual uint64_t PidStart() const = 0;              // ON_DISK_FORMAT.md section 4
  virtual bool IsAlive(uint64_t pid, uint64_t pid_start) const = 0;
};

// ---- fs: repository storage only (D24) ----
class WritableFile {                          // append-only
 public:
  virtual ~WritableFile() = default;
  virtual Status Append(std::span<const std::byte> data) = 0;
  virtual Status Sync() = 0;                  // data durable when OK returns (D23)
  virtual Status Close() = 0;
};
class ReadableFile {
 public:
  virtual ~ReadableFile() = default;
  virtual Status ReadAt(uint64_t offset, std::span<std::byte> out) const = 0;  // exact or error
  virtual Result<uint64_t> Size() const = 0;
};
class Fs {
 public:
  virtual ~Fs() = default;
  virtual Result<std::unique_ptr<WritableFile>> CreateNew(const std::string& path) = 0;  // O_EXCL
  virtual Result<std::unique_ptr<ReadableFile>> OpenRead(const std::string& path) = 0;
  virtual Status Rename(const std::string& from, const std::string& to) = 0;  // atomic replace
  virtual Status Link(const std::string& existing, const std::string& link) = 0;  // EEXIST if taken
  virtual Status Remove(const std::string& path) = 0;
  virtual Status CreateDir(const std::string& path) = 0;
  virtual Status SyncDir(const std::string& dir) = 0;  // makes create/rename/link/remove durable
  virtual Result<std::vector<std::string>> ListDir(const std::string& dir) = 0;
  virtual Result<bool> Exists(const std::string& path) = 0;
};

// ---- hash ----
enum class BlobType : uint8_t { kData = 1, kTree = 2, kRecipe = 3 };
struct Fingerprint { std::array<uint8_t, 32> bytes; /* ==, <, ToHex, FromHex, Hash64() */ };
Fingerprint FingerprintOf(BlobType type, std::span<const std::byte> payload);  // BLAKE3(tag‖payload)

// ---- chunk ----
class Chunker {                               // stateless, thread-safe
 public:
  virtual ~Chunker() = default;
  // `data` starts at the current chunk start. Returns the next chunk's length, or 0 exactly when
  // !eof and data.size() < MaxSize() (more input needed), or when data is empty at eof.
  virtual size_t NextChunkLength(std::span<const std::byte> data, bool eof) const = 0;
  virtual size_t MinSize() const = 0;
  virtual size_t MaxSize() const = 0;
};
Result<std::unique_ptr<Chunker>> MakeChunker(const ChunkerParams& params);  // validates (D62)
class ByteSource {                            // a file being chunked, or bytes in memory
 public:
  virtual Result<size_t> Read(std::span<std::byte> out) = 0;  // 0 only at end of stream
};
class ChunkReader {                           // buffers a byte stream and yields chunks
 public:
  ChunkReader(const Chunker& chunker, ByteSource& source);  // owns a 2 x max-chunk buffer
  ChunkReader(const Chunker& chunker, ByteSource& source,
              std::vector<std::byte>& buffer);              // borrows one, grown if small (D98)
  Result<std::optional<std::span<const std::byte>>> Next();  // nullopt at end of stream
};

// ---- store ----
struct Location { uint32_t container_id; uint32_t offset; uint32_t length; uint32_t crc32c;
                  BlobType type; };
struct SealedEntry { Fingerprint fp; Location loc; };
class ContainerWriter {                       // single-threaded; owned by one thread
 public:
  void Add(const Fingerprint& fp, BlobType type, std::span<const std::byte> payload);  // D63
  bool Full() const;                          // file size >= target
  bool Empty() const;
  Result<SealedContainer> Seal();             // durable when OK (7.2); not yet published
};
class ContainerSet {                          // thread-safe
 public:
  uint32_t AllocateId();
  void MarkPublished(uint32_t container_id, const ContainerInfo& sizes);  // after Publish/relocation
  std::vector<uint32_t> PublishedNotRetired() const;  // GC candidates (4.3 step 1)
  Result<std::vector<std::byte>> Read(const Location& loc);  // verifies loc.crc32c
  void Retire(uint32_t container_id, Epoch retire_epoch);
  // Unlinks retired containers whose retire epoch <= the oldest reader's entry (all if none),
  // drops their cached handles, then SyncDir.
  Result<Reclaimed> ReclaimRetired(std::optional<Epoch> oldest_reader);
};

// ---- index ----
enum class RemoveResult { kRemoved, kKept, kNotCanonical };
class ChunkIndex {                            // thread-safe, 256 shards (D17, D18)
 public:
  // `now` must be read after the caller's session or guard registered (6.1). Stamps take the max.
  std::optional<Location> LookupAndStamp(const Fingerprint& fp, Epoch now);
  std::optional<Location> Lookup(const Fingerprint& fp) const;               // no stamp
  bool RebuildInsert(const SealedEntry& e);   // open: highest container id wins, any order (D67)
  size_t Publish(std::span<const SealedEntry> entries, Epoch now);  // insert-if-absent; an existing
                                                                    // entry is stamped instead;
                                                                    // returns duplicates skipped
  // GC mark phase: location, and whether this call marked it (false: already marked this run).
  std::optional<MarkResult> MarkLive(const Fingerprint& fp, GcRun run);
  RemoveResult RemoveIfDead(const Fingerprint& fp, const Location& here, GcRun run, Epoch e_min);
  bool Relocate(const Fingerprint& fp, const Location& from, const Location& to);  // CAS
  void ForEachInShard(size_t shard, const std::function<void(const IndexEntry&)>& fn);
};

// ---- repo ----
class EpochManager {                          // section 6.1
 public:
  Epoch Current() const;
  Epoch Advance();                            // returns the NEW value
  Epoch BeginSession();                       // records and returns the start epoch
  void EndSession(Epoch start);               // IngestSession calls both (ctor, dtor)
  Epoch EnterRead();                          // records and returns the entry epoch
  void ExitRead(Epoch entry);                 // ReadGuard calls both (RAII)
  GcEpochs BeginGc();                         // {E_gc, E_min} computed in one critical section
  std::optional<Epoch> OldestReader() const;  // nullopt when nobody is reading
};
class ReadGuard {                             // one lookup-then-read unit (6.3, D21)
 public:
  explicit ReadGuard(EpochManager& epochs);
};
class IngestSession {                         // used by backup and by the replication receiver
 public:
  // Hit: the fingerprint is pending in this session (only with reserve == kOn: reserved or
  // received; with kOff: received) or published (stamped). Miss with kOn: the caller must Put it,
  // and it is now reserved as pending (4.1). Miss with kOff: nothing is reserved (4.4).
  LookupResult Lookup(const Fingerprint& fp, Reserve reserve);
  Status Put(const Fingerprint& fp, BlobType type, std::span<const std::byte> payload,
             size_t writer);                                     // writer index = owning thread
  Result<SnapshotId> Commit(std::span<const std::byte> manifest_bytes);  // 7.3
};
class Repository {
 public:
  static Result<RepoConfig> Init(Fs& fs, const std::string& path, const RepoParams& params);  // 7.7
  static Result<std::unique_ptr<Repository>> Open(Fs& fs, const std::string& path,
                                                  const ProcessIdentity& self);    // 7.6
  Result<BackupResult> Backup(SourceTree& source, const BackupOptions& options);   // D72
  Result<RestoreResult> Restore(const SnapshotId& id, RestoreSink& sink,
                                const RestoreOptions& options);
  Result<std::vector<SnapshotInfo>> ListSnapshots();
  Result<SnapshotId> ResolveSnapshot(std::string_view id_or_prefix);
  Result<std::vector<TreeEntry>> ListPath(const SnapshotId& id, std::string_view path);
  Status RemoveSnapshot(const SnapshotId& id);
  Result<VerifyReport> Verify(const VerifyOptions& options);
  Result<RepoStats> Stats();
  Result<std::unique_ptr<IngestSession>> BeginIngest();
  RepoInternals& Internals();                 // for gc/ and repl/ only (section 1)
};

}  // namespace dds
```

`SourceTree` (walk a directory, read files, read links) and `RestoreSink` (create directories,
files, symlinks; set mode and times) are separate from `Fs` because they touch user data, not
repository state, and crash simulation of user files is out of scope.

## 3. Repository layout (summary)

```
<repo>/config                  parameters; its rename is init's commit point (7.7)
<repo>/lock                    process lock (D16)
<repo>/containers/<8 hex>.ctr  immutable containers, id in hex
<repo>/snapshots/<32 hex>.snap snapshot manifests (commit records)
<repo>/index.ckpt              index checkpoint: derived, optional, removed by open if bad (7.8)
<repo>/tmp/                    files being written; emptied on open
```

ON_DISK_FORMAT.md defines every byte.

## 4. Data flow

### 4.1 Backup

```
Walker ──(file batches, bounded queue)─▶ worker 1..N ──▶ per-worker ContainerWriter ──seal──▶ publish
   │                                       │ read file → ChunkReader → fingerprint
   │                                       │ session.Lookup(fp): hit  → reference it
   │                                       │                     miss → writer.Add (reserved)
   │                                       ▼
   │                               file result: recipe pieces (written as recipe blobs) + metadata
   ▼
caller thread, after the workers drain: tree blobs bottom-up with its own writer → root tree
   ▼
IngestSession::Commit: seal + publish every writer → write manifest (commit point)
```

- The walker lists directories in name order and emits one task per regular file, collected into
  batches of up to 64 files (closed early at 16 MiB of listed size) that it pushes into a bounded
  multi-producer, multi-consumer queue (capacity 4 × N batches); a worker pops a batch and stores
  its files in order (D102). One queue operation per batch instead of per file keeps the walker,
  which every file passes through, from waiting on the queue's lock. The bound applies
  backpressure so memory stays flat on huge trees. The walker records directory and symlink
  entries for the tree builder.
- A worker processes a whole file, so chunk order within a file is sequential, and the recipe is
  built in order without coordination. Parallelism is across files (D31), except for a file of at
  least `split_file_bytes` (64 MiB by default) with more than one worker (D109): the worker that
  took it reads and chunks it alone, so its boundaries are those of chunking it whole, in runs of at
  least 1 MiB of whole chunks. When more threads are idle than segments wait, a run is copied into
  a segment and queued (`BackupQueue`, ahead of file batches) for any worker to fingerprint and
  store through its own writer; otherwise the splitter stores the run itself, without a copy. The
  splitter collects the runs in file order, storing queued segments while it waits, and writes the
  recipe pieces exactly as for a whole file, so the snapshot does not change. After the walk ends,
  idle workers stay until every batch taken is done, since a file in one may still be split. Each
  worker owns a
  `ContainerWriter`; the caller thread owns one more for trees (writer index N). Each worker also
  owns one read buffer, lent to every file's `ChunkReader`, so a backup of many small files does
  not allocate and zero-fill 2 x the maximum chunk per file (D98).
- **Lookup protocol** (backup uses reservation on; all inside the session shard mutex of the
  fingerprint, one of 256 chosen by fingerprint byte 0, D101): if the fingerprint is in the pending
  map, it is a hit. Otherwise call `index.LookupAndStamp` (a shard lock nested inside),
  and a found entry is a hit. Otherwise insert the fingerprint into the pending map and return a
  miss, so the calling worker must `Put` it and no other worker of this session will. A session
  therefore never stores the same new blob twice.
- **Seal protocol:** when a writer is full (or at commit), seal it (7.2), then `index.Publish` its
  entries, then `containers.MarkPublished(id)`, then remove the entries from the pending map, each
  under its own shard's mutex, one at a time. In that order, a fingerprint is always findable in the
  pending map or the index, never in neither. Both protocols' guarantees concern one fingerprint at
  a time, which is why sharding the pending map by fingerprint keeps them (D101).
- **With a parent snapshot (D106):** the walker also descends the parent's trees, reading each
  directory's parent tree as it lists the directory, and attaches the parent's entry to a file's
  task when the file is unchanged since the parent (same name, size, and mtime to the nanosecond,
  that mtime more than 2 s older than the parent's start, SPEC.md section 4). The worker then
  looks up the parent's recipe pieces and every chunk they list through the session, which stamps
  each (6.2 item 3), and reuses the pieces without opening the file. If any lookup misses or a
  recipe cannot be read (GC removed it after the parent was removed), it reads the file as usual.
  A reused file's pieces are the ones reading it would give, so the snapshot, root fingerprint
  included, is the same as without a parent.
- After the workers drain, the caller thread builds tree blobs bottom-up from the recorded entries.
  Entries are sorted by name, so the root fingerprint is independent of scheduling (G11).
- **Failure:** the first worker error is recorded, sets a stop flag, and closes the queue. The
  walker's next push fails, the other workers drain the queue without storing anything, and after
  every worker is joined the backup returns that first error and commits nothing (D79).
- `seal.before_publish` (section 10) sits between sealing a container and publishing it: the window
  in which another session misses the new blobs and may store copies (D32).

### 4.2 Restore

The restorer reads the manifest, then walks trees depth-first.
- **Guards:** every lookup-then-read runs inside a `ReadGuard` (6.3). Reading one tree blob is one
  guarded unit. Restoring one file (its recipe pieces and chunks) is one guarded unit.
- Chunks are read with `ContainerSet::Read` (CRC32C of the stored bytes always checked, then
  decompressed if the blob is a zstd frame, D107). BLAKE3 is verified unless `--no-verify`.
- **Creation:** directories are created with mode 0700, files with
  `O_CREAT | O_EXCL | O_NOFOLLOW` and mode 0600, and symlinks with `symlinkat`, all relative to
  the parent directory's file descriptor (D42). A file's mode and times are applied with
  `fchmod`/`futimens` after its content. A directory's mode and times are applied after all its
  children, so a read-only directory (0555) does not block its own restore. A symlink's times are
  set with `utimensat(..., AT_SYMLINK_NOFOLLOW)`. Its mode is recorded at backup but not restored,
  because Linux cannot change a symlink's mode (D54).
- **Collisions:** creation never replaces anything. `EEXIST` (for example two names that differ
  only in case on a case-insensitive destination) fails the restore with `AlreadyExists` naming
  both entries.
- **Missing blob:** if a lookup misses, the restorer checks whether the snapshot's manifest still
  exists. If it is gone (removed concurrently in a serving process), the result is `Aborted`;
  otherwise `Corruption`.
- **Sizes and cycles:** each chunk's length must match its recipe reference and a file's chunks
  must add up to its recorded size, or the restore fails with `Corruption`. A tree that contains
  itself, which only a damaged index can claim when hashes are not checked, is `Corruption` too
  (D69). The destination directory's own mode and times are left as they are (D68).

### 4.3 Garbage collection

Section 6 holds the safety argument. In order:

1. **Start.** Take `gc_mutex` (one GC at a time). Run `ReclaimRetired` for any containers left
   retired by the previous run. Increment the run number (the first run is 1).
   `{E_gc, E_min} = epochs.BeginGc()`. Then take the candidate set
   `containers.PublishedNotRetired()`: containers whose entries are all in the index and that are
   not already retired. A container that is sealed but not yet published is not a candidate
   (6.2 item 5). Then list the snapshot manifests.
2. **Mark.** For each listed snapshot, walk its DAG with `index.MarkLive(fp, run)`, which sets the
   entry's mark to this run and returns its location so the marker can read tree and recipe
   blobs. A reachable fingerprint missing from the index is corruption, and **GC aborts without
   deleting anything** (fail closed). A manifest that disappeared after listing (concurrent `rm`)
   is skipped. Any other read error aborts. (GC's own reads need no guard: only GC unlinks
   containers, and it does not unlink while marking.)
3. **Plan.** Scan all index shards and sum, per candidate container, the bytes of entries that are
   marked or protected (`last_ref ≥ E_min`). Compare with each container's payload bytes (known
   from its footer since open or seal). Live fraction 0 → delete. Below `--threshold` → compact.
   Otherwise keep.
4. **Decide per entry.** For each container to delete or compact, walk its footer. For each entry,
   call `index.RemoveIfDead(fp, here, run, E_min)` under the shard lock:
   - `kRemoved`: the entry was canonical here and dead, and it has left the index;
   - `kKept`: canonical here and marked or protected, so it goes on the copy list;
   - `kNotCanonical`: the index points elsewhere (a duplicate copy), so there is nothing to do.

   A container planned for deletion that yields any `kKept` (a writer stamped an entry after the
   plan) becomes a compaction.
5. **Copy forward.** Append every blob on the copy list to the GC's own `ContainerWriter`, and seal.
   For each moved entry, `index.Relocate(fp, old, new)`. Relocation cannot fail, because only GC
   changes or removes existing entries and only one GC runs; a failure is a bug (`DDS_CHECK`).
   Then `containers.MarkPublished` for the GC's new containers. They were published after the
   start, so they are not candidates in this run. Their ids were allocated after the start, so
   they are higher than every candidate's id (D12).
6. **Retire and reclaim.** `ContainerSet::Retire(id, epochs.Advance())` for every deleted or
   compacted container (one `Advance` after every relocation of the run). Retired containers are never candidates again. At the end of the run
   (and at the start of the next one, and on close), `ReclaimRetired` unlinks each retired container
   whose retire epoch is ≤ every active reader's epoch, drops its cached read handles, and runs
   `SyncDir(containers/)`.

### 4.4 Replication

PROTOCOL.md defines the messages. The sender walks the snapshot DAG in a streaming fashion and
batches fingerprints into `HAVE` queries (up to `W` outstanding), reading local containers under
guards. For each reply it sends the missing blobs in `PUT` batches. The receiver is an
`IngestSession`:
- `HAVE` answers come from `session.Lookup` with reservation off, so hits are stamped, and the
  receiver's GC keeps them. A miss reserves nothing, unlike a backup, because the sender may never
  send the blob. A reservation without data would let the commit walk accept a missing blob.
- Each received blob is verified (BLAKE3 must equal the claimed fingerprint) and then `Put`. A blob
  that is already present or pending is verified and then dropped.
- On `COMMIT`, the receiver seals and publishes everything. It then walks the snapshot from its
  root, finding every blob in the index with `session.Lookup` (so every blob it validated is
  stamped and protected, 6.2) and reading trees and recipes under guards. Only then does it write
  the manifest.
- In protocol version 2 the sender first asks `HAVE_TREES` about every tree of the snapshot, in one
  pipelined pass, and walks only the trees the receiver does not hold whole, without descending
  into the ones it does (D108). The receiver answers by walking each subtree
  with the COMMIT walk's checks, its lookups going through the session so everything found is
  stamped as above, and remembers what it found: the COMMIT walk skips those trees. No invariant
  links a tree's presence to its descendants'; GC is unchanged.

A dropped connection loses nothing that was sealed, and a resumed replication finds those blobs
present (PROTOCOL.md section 5 has the one exception).

The sender's writer and reader threads share one `Window` (`send_window.h`): the outstanding-batch
table (each batch a HAVE, HAVE_TREES, or PUT), the HAVE_TREES answers, the queue of missing
fingerprints, the first error, and COMMIT_OK, under one leaf mutex. A HAVE_REPLY frees
its slot and queues its missing blobs in one critical section (D89). `dds serve` runs a `Server`:
one thread and one ingest session per connection (at most 16, D90), and optionally a GC loop.

### 4.5 Verify and stats

Both walk every snapshot with one `DagWalker` (D69), which checks each blob once: present in the
index with the referenced type, trees and recipes parse, chunk lengths match their references and
add up to each file's size, and with `--deep` every blob hashes to its fingerprint. What it finds
under each tree and recipe is kept with paths relative to that blob, so a damaged blob in a shared
subtree is reported under every snapshot and path that reaches it. `verify` also re-reads every
container's header, footer, and trailer, reports stray file names as warnings, and counts
unreachable blobs and duplicate copies as space for GC. `stats` turns the reachable set into each
container's live fraction, counting only the canonical (indexed) copy of a blob as live, exactly as
GC's planner does (4.3 step 3).

## 5. Concurrency model

### 5.1 Threads

| Thread | Count | Does |
|---|---|---|
| Caller (CLI or test) | 1 | Drives a command; builds trees at the end of a backup. |
| Backup walker | 1 per backup | Lists directories, enqueues file tasks. |
| Backup workers | N per backup | Read, chunk, hash, look up, write. Each owns one `ContainerWriter`. |
| GC | 1 at a time | Mark, plan, compact, retire, reclaim. |
| Replication sender | 2 per connection | One sends queries and blobs, one reads replies. |
| Replication receiver | 1 per connection | Reads messages, writes replies. |
| Server | 1 | Accepts connections; optionally runs GC every `--gc-interval`. |

Within one process any number of backups, restores, and replication receivers may run at once,
with at most one GC. One repository is open in at most one process (D16).

### 5.2 Shared state

| State | Guarded by |
|---|---|
| `ChunkIndex` shard `i` (entries, stamps, marks) | `shard_mutex[i]` |
| An `IngestSession`'s pending map, shard `i` (fingerprints with byte 0 = `i`) | that session's `session_shard_mutex[i]` (D101) |
| `ContainerSet` published set, retired list, read-handle cache, payload sizes | `containers_mutex` |
| `ContainerSet` id counter | `std::atomic<uint32_t>`, `fetch_add` with relaxed ordering. Relaxed is enough: all modifications of one atomic have a single total order, so an id allocated after another (in happens-before) is larger, which is all D12 needs. |
| `EpochManager` counter | `std::atomic<uint64_t>`: written only under `epoch_mutex`, read with acquire anywhere |
| `EpochManager` active sessions and readers | `epoch_mutex` |
| GC exclusivity, GC run number | `gc_mutex` |
| Snapshot directory listing, commit rename, removal | `snapshots_mutex` |
| A `ContainerWriter` | owned by exactly one thread, not shared |
| Bounded file queue, sender window, simulated-link pipes, `MemFs` namespace and inodes | leaf mutexes (5.3) |
| Logger | no dds mutex: each line is one `fwrite`, and stdio locks the stream for every call (POSIX `flockfile`) |

### 5.3 Lock order

Every mutex is an `OrderedMutex` with a level, taken through the annotated RAII guard `MutexLock`
(D56). A thread may acquire a mutex only if its level is higher than every level it already holds:

| Level | Mutex |
|---|---|
| 10 | `gc_mutex` |
| 20 | `snapshots_mutex` |
| 30 | `epoch_mutex` |
| 40 | `session_shard_mutex[i]` (any one session's; never two at once, D101) |
| 50 | `containers_mutex` |
| 60 | `shard_mutex[i]` (never two at once) |
| 90 | `crash_sim` (test-only `CrashSimFs`, held while it calls its `MemFs`, D77) |
| 100 | leaf mutexes: queue, sender window, link pipes, `MemFs` internals. Nothing is acquired while one is held. |

Never hold a shard mutex, a session shard mutex, or `epoch_mutex` across I/O. `gc_mutex` is held
across I/O by a GC run and by the index checkpoint writer (7.8), so the two never overlap. The checker
(`DDS_ENABLE_LOCK_ORDER_CHECK`, D59) is on in the `debug`, `asan`, `tsan`, and `tidy` presets and off
in `release` (TESTING.md section 2). It checks before blocking, so a wrong order aborts with both
mutex names instead of deadlocking.

## 6. Epochs: why concurrent GC is safe

### 6.1 Definitions (exact)

- `counter`: one process-wide 64-bit value, 1 on open, only increasing. It is modified only under
  `epoch_mutex`.
- `Advance()`: under `epoch_mutex`, `counter = counter + 1`. Returns the **new** value.
- `BeginSession()`: under `epoch_mutex`, `start = counter` and the session is added to the active
  set, in one critical section. A session performs no lookup or publish before `BeginSession`
  returns, and deregisters (under `epoch_mutex`) only after its commit or abort is complete.
- `EnterRead()`: under `epoch_mutex`, `entry = counter` and the reader is added to the reader
  multiset, in one critical section.
- `BeginGc()`: in one `epoch_mutex` critical section, `E_gc = ++counter` and
  `E_min = min(E_gc, start of every active session)`.
- **Stamping:** `LookupAndStamp` (on a hit) and `Publish` (for a new entry and for an existing entry
  it skips as a duplicate) set `last_ref = max(last_ref, now)` under the shard lock. `now` is a
  value of `counter` read after the caller's `BeginSession` returned, so `now ≥ start`. Taking the
  max means a delayed stamp with an older `now` can never lower `last_ref`.
- Entries rebuilt from footers on open have `last_ref = 0` and mark 0. GC run numbers start at 1,
  so a fresh entry is never "marked in this run".
- **Removal rule:** GC removes an entry only if its mark is not this run **and** `last_ref < E_min`,
  evaluated under the entry's shard lock.

### 6.2 Claim and argument (G6, blob safety)

*Claim:* no blob that a snapshot committed before GC started, or a snapshot committed later, can
reach is removed.

1. Snapshots committed before GC started are listed and marked (step 2), so their blobs are marked.
   GC lists manifests *after* `BeginGc`. A session renames its manifest into place before it
   deregisters, so a session that was no longer active at `BeginGc` has a manifest the listing
   sees.
2. A snapshot committed later comes from a session S that either was active at `BeginGc` or began
   afterwards. In both cases `start(S) ≥ E_min`.
3. S references a blob in only four ways: it published the blob, it got a lookup hit, (as a
   replication receiver) it validated the blob at commit through `Lookup`, or (as a backup with a
   parent, D106) it reused a parent's file, after looking up through S that file's recipe pieces
   and every chunk they list. All four stamp `last_ref` with some `now ≥ start(S) ≥ E_min`, and a
   reused file whose lookups miss is read instead. Fingerprints pending in S are not in the index
   until S publishes them, and publishing stamps them.
4. The stamp and the removal check happen under the same shard lock, so they are ordered. If the
   stamp comes first, the removal check sees `last_ref ≥ E_min` and keeps the entry. If the removal
   comes first, S's lookup misses and S writes a fresh copy. Either way S never references an entry
   that GC removes.
5. GC only touches candidates, and a container becomes a candidate only after `MarkPublished`,
   which follows `Publish`. So a candidate's live entries are all in the index, where steps 3 and 4
   protect them. A container sealed but not yet published, or published after `BeginGc`, is not a
   candidate. Without this rule, a container sealed just before GC and published just after it
   would look 0% live and be deleted (mutation `candidate_before_publish`, TESTING.md).

So every blob a committed snapshot reaches is either marked, protected by its stamp, or not in a
candidate. The deterministic interleaving tests and the stress test check this. Mutations that
remove each rule must make them fail (TESTING.md 3.7).

### 6.3 Reader safety (G6, container lifetime)

Anything that reads containers by location (restore, verify, the replication sender, the
receiver's commit walk) may race with GC relocating an entry and deleting its old container.
Epoch-based reclamation prevents the reader from finding the file gone:

- Every lookup-then-read runs inside a `ReadGuard`, taken before the lookup and released after the
  read. Guards cover one tree blob, one file, or one replication batch, never a whole snapshot, so
  a long restore delays reclamation by at most one unit.
- GC relocates entries **before** retiring their old container with `retire_epoch = Advance()`.
- A retired container is unlinked only when every active reader's `entry ≥ retire_epoch`. A reader
  that entered before the retire has `entry ≤ retire_epoch − 1` and blocks the unlink. A reader
  that entered after it acquired `epoch_mutex` after GC's `Advance`, which itself followed the
  relocation's shard-lock release, so its lookup sees the new location.

## 7. Durability protocol

### 7.1 File system assumptions (the only ones the design relies on)

- **A1** After `Sync()` on a file returns OK, the file's data is durable.
- **A2** After `SyncDir(d)` returns OK, every create, rename into `d`, link into `d`, and remove in
  `d` is durable.
- **A3** `Rename` is atomic: after a crash the file is under its old name or its new name, never
  both and never neither, and the target name never refers to a mix of two files.

`PosixFs` implements `Sync` with `fdatasync` on Linux and `fcntl(F_FULLFSYNC)` on macOS, because
plain `fsync` on macOS does not ask the drive to flush its write cache (D23). `CrashSimFs`
(TESTING.md 3.6) explores crash states allowed by A1–A3 and nothing stronger.

### 7.2 Sealing a container

1. Write header, blobs, footer, and trailer to `tmp/<id>.ctr.tmp`.
2. `Sync()` the file.
3. Check that `containers/<id>.ctr` does not exist (it cannot, given 7.6 step 6; if it does, fail
   with `Corruption` rather than replace it). `Rename` the file there.
4. `SyncDir(containers/)`.
5. Return the entries. The caller then publishes them and calls `MarkPublished` (4.1). D11: the
   index never points at non-durable data.

### 7.3 Committing a snapshot (G2)

1. Seal and publish every open writer of the session (7.2, 4.1).
2. Build tree blobs bottom-up with the caller thread's writer, seal, and publish. Trees are written
   bottom-up, so by the time the root tree is published everything below it is durable.
3. Encode the manifest. Under `snapshots_mutex`: write `tmp/<id>.snap.tmp`, `Sync()`, `Rename` to
   `snapshots/<id>.snap`, `SyncDir(snapshots/)`. **The rename is the commit point.** A crash before
   it leaves orphan containers (garbage), never a partial snapshot.
4. Deregister the session.

### 7.4 Removing a snapshot

Under `snapshots_mutex`: `Remove(snapshots/<id>.snap)`, then `SyncDir(snapshots/)`. The blobs
become garbage for the next GC.

### 7.5 GC

Copy, seal (7.2), relocate in memory, retire, then unlink after the grace period, then `SyncDir`.
At every point a durable copy of every live blob exists. After a crash, the index rebuild may see
two copies of a blob. The higher container id wins (D12). A GC copy always has a higher id than the
candidate it was copied from. A later session's duplicate may have an even higher id, and that copy
is equally valid. Whatever loses is garbage for the next run.

### 7.6 Opening a repository (recovery)

1. If `config` does not exist, return `NotFound` ("not a dedupstore repository, or an unfinished
   init"; 7.7).
2. Acquire the lock with the caller's `ProcessIdentity` (D16).
3. Parse `config`. Refuse an unknown format version with `Incompatible`.
4. Remove everything in `tmp/` (unfinished writes from a crash), then `SyncDir(tmp/)`.
5. List `containers/`. If `index.ckpt` exists and passes its checks (ON_DISK_FORMAT.md 9.4),
   load its entries for every covered container that is listed, its 256 shards in parallel, and
   publish those containers with the sizes it records; covered containers no longer listed
   contribute nothing. A checkpoint that fails any check is removed, and the load starts over
   without it (7.8, D104).
6. Read the trailer and footer of every listed container the checkpoint did not cover (all of them
   without a checkpoint), in parallel, and verify them. A container that fails is reported and
   skipped. `verify` then reports any snapshot that needed it.
7. Set the container id counter to the larger of 1 + the maximum id over **every** `*.ctr` name
   listed, including skipped ones, so a bad container's file is never replaced, and the
   checkpoint's next_container_id, so an id it covers is never reused.
8. Insert the footer entries. Where a fingerprint appears in more than one container, keep the
   highest container id (D12) and count the others as duplicate copies. Every valid container is
   published.
9. Set `counter = 1`.

No write-ahead log exists, because no durable state is ever updated in place (D10).

### 7.7 Initializing a repository

1. `<repo>` must not exist, or must be a directory containing nothing except possibly empty
   `tmp/`, `containers/`, and `snapshots/` directories and no `config` (a crashed earlier init).
   `tmp/` may hold a partial `config.tmp`, which init replaces (D65). Anything else is
   `AlreadyExists`.
2. Create the missing directories, then `SyncDir(<repo>)`. If init created `<repo>` itself, it also
   syncs `<repo>`'s parent directory (D65).
3. Write the config to `tmp/config.tmp`, `Sync()`, `Rename` to `config`, `SyncDir(<repo>)`. **The
   config rename is init's commit point.** Before it, `Open` returns `NotFound` and `init` can be
   run again (D52).

### 7.8 The index checkpoint (D104)

`index.ckpt` (ON_DISK_FORMAT.md section 9) copies the index entries of every container it covers,
so open reads one file instead of one footer per container. It is written by `Close` when at least
256 containers are published but not covered, or covered but deleted, and by
`Repository::WriteIndexCheckpoint`: in `tmp/`, synced, renamed, and the repository directory
synced, like every file (7.1).

- **What it covers.** The containers published and not retired when the writer's scan starts. The
  writer holds `gc_mutex` for the whole write, so no entry pointing into a covered container moves
  or leaves the index during the scan. Sessions only add entries, and every entry of a covered
  container was in the index before the container was published (D49), so the scan sees all of
  them. Entries pointing elsewhere are left out; those containers' footers are read at the next
  open.
- **Why open may trust a covered id without reading it.** Container files are immutable, and open
  never hands out an id below the checkpoint's next_container_id, so a listed id the checkpoint
  covers names the same file it did when the checkpoint was written.
- **Why no write ordering matters for correctness.** A checkpoint whose bytes did not survive a
  crash fails its CRCs and is removed, so open falls back to the footers. The sync and rename only
  keep it usable.
- **What open no longer does.** It does not re-read the footers of covered containers, so damage
  to one is found by blob reads (payload CRCs) and by `verify`, which reads every footer and checks
  every index entry against its container's footer.

## 8. Index and memory

- `ChunkIndex` has 256 shards selected by fingerprint byte 0. Each shard is a `FlatTable`: open
  addressing with linear probing, a power-of-two capacity, growth at load factor 0.75, and
  backward-shift deletion (no tombstones). Fingerprints are already uniformly distributed, so the
  table hashes with the fingerprint's bytes 8..15 directly. Byte 0 already chose the shard (D18).
- Entry: fingerprint 32 B, container id 4 B, offset 4 B, length 4 B, payload CRC32C 4 B,
  `last_ref` 8 B, mark 4 B, type 1 B, padding to 64 B: one cache line per probe. With the load
  factor between 0.375 and 0.75 across the growth cycle, that is 85 to 171 B per blob, or about 11
  to 22 MB of RAM per GB of unique data at the 8 KiB average chunk size. The supported scale is
  about 500 GB of unique data per repository on a 16 GB machine. `stats` reports the measured bytes
  per blob.

## 9. Error model

- Library functions return `Status` or `Result<T>`. Errors carry the path and offset where
  relevant. SPEC.md section 3 maps each `StatusCode` to a CLI exit code.
- `Corruption` means stored or received bytes are wrong. `IoError` means the OS call failed.
  `InvalidArgument` means the caller passed something unusable. `AlreadyExists` means a target
  that must be new is not. `Aborted` means a concurrent operation made the request impossible.
  `Incompatible` means a version or parameter mismatch.
- Background work (GC thread in `serve`) logs failures and keeps the server running. A failed GC
  run deletes nothing that its successful steps did not already make safe to delete (sections 4.3
  and 7.5).

## 10. Test hooks

Builds with `DDS_ENABLE_TEST_HOOKS` (TESTING.md section 2) compile two kinds of hook. Without the
define, both compile to nothing.
- `DDS_SYNC_POINT("name")`: a named point where a test can pause a thread on a latch, to force an
  interleaving deterministically. Required points: `seal.before_publish` (between `Seal` and
  `Publish`), `gc.after_plan` (between plan and decide), `gc.after_relocate` (before retire),
  `read.after_lookup` (between a guarded lookup and its read), `commit.before_manifest`.
- `DDS_MUTATION("name")`: a boolean that a test can switch on to break one rule on purpose (the
  list is in TESTING.md 3.6–3.7).

## 11. Observability

`stats` and every `--json` report come from counters kept per component: blobs and bytes written,
dedup hits, duplicate copies, chunker and hasher bytes and time, lookup counts, seal count and
time, GC phase durations, bytes copied, bytes reclaimed, replication bytes on the wire by message
type, and round trips. BENCHMARKS.md defines which counters each experiment records.
