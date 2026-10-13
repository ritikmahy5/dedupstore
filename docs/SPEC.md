# dedupstore — specification

What dedupstore does, what it guarantees, and what it deliberately does not do. ARCHITECTURE.md
says how; ON_DISK_FORMAT.md and PROTOCOL.md define the bytes.

## 1. Concepts

| Term | Meaning |
|---|---|
| Repository | A directory holding one store: config, lock, containers, snapshots. Owned by one process at a time. |
| Blob | A unit of stored content with a type: `data` (a chunk of a file), `tree` (a directory listing), or `recipe` (a piece of a file's chunk list). |
| Fingerprint | `BLAKE3-256(type_tag ‖ payload)`, 32 bytes. A blob's identity. Equal fingerprints mean equal content (D7). |
| Chunk | A `data` blob produced by the chunker from file content. |
| Container | An immutable file holding many blobs plus a footer index of them. Written once, never modified, deleted only by GC. |
| Snapshot | A committed, immutable record of one directory tree at one time. Its manifest names the root tree. |
| Index | In-memory map from fingerprint to the container location of that blob. Derived: rebuilt on open from container footers, or from the index checkpoint for the containers it covers (D10, D104). |
| Ingest session | Any operation that adds blobs and may commit a snapshot: a backup, or an incoming replication. |
| Logical bytes | Sum of file sizes in a snapshot (what the user backed up). |
| Unique data bytes | Sum of the lengths of distinct `data` blobs in the index. |
| Physical bytes | Sum of container file sizes on disk. |
| Dedup ratio | Logical bytes of all live snapshots divided by unique data bytes. |

## 2. Guarantees

These are the properties the tests in TESTING.md exist to prove. Each has a stable id so tests and
documents can cite it.

- **G1 Round trip.** Restoring a committed snapshot reproduces every regular file's bytes exactly,
  the directory structure, symlink targets, the permission bits of files and directories, and the
  modification times of all entries including symlinks (to the nanosecond where the platform
  supports it). Symlink permission bits are recorded but not restored (D54).
- **G2 Atomic commit.** A snapshot is either fully committed (restorable) or absent. There is no
  partially visible snapshot after any crash.
- **G3 Crash safety.** After a crash at any point (process kill or power loss, under the file
  system assumptions in ARCHITECTURE.md "Durability protocol"), the repository opens, `verify`
  passes, and every snapshot committed before the crash still satisfies G1. Only space may leak,
  and the next GC reclaims it.
- **G4 Integrity.** Any corruption of stored bytes is detected: by CRC32C when a structure is read,
  and by BLAKE3 when a blob is restored or deep-verified. Corrupt input yields an error naming the
  file and offset. It never crashes the process or restores wrong bytes silently.
- **G5 Dedup.** Within one session, a blob with a given fingerprint is stored at most once. Across
  concurrent sessions, the same new blob may be stored more than once (D11, D32). `stats` counts
  these duplicate copies exactly. GC drops a duplicate when it compacts the container holding it,
  so a GC at threshold 1.0 leaves none.
- **G6 Safe concurrent GC.** GC running concurrently with backups, restores, and replication never
  removes a blob that a committed snapshot, or a session that later commits, references (D20), and
  never deletes a container a reader is still using (D21).
- **G7 Space reclamation.** After snapshots are removed, GC returns the space of every blob no
  remaining snapshot references, subject to the compaction threshold (default: containers less
  than 50% live are rewritten).
- **G8 Replication correctness.** A replicated snapshot restores on the replica exactly as on the
  source (G1). An interrupted replication leaves the replica consistent (G3). Resuming it transfers
  only blobs the replica still lacks.
- **G9 Replication efficiency.** Replicating snapshot B after snapshot A was replicated transfers
  only the blobs of B that the replica lacks. The overhead is 32 bytes per blob of B (the
  fingerprint in the query), one bit per blob (the reply), a 37-byte header per blob sent (PUT),
  and message framing.
- **G10 Restore containment.** Restore never creates, modifies, or follows anything outside the
  destination directory, whatever the repository contains (names such as `..` or names with `/`
  are rejected as corruption; existing symlinks are never followed).
- **G11 Determinism.** Backing up the same tree with the same repository parameters produces the
  same root tree fingerprint, whatever the thread count or scheduling.

## 3. Command-line interface

Binary: `dds`. Every command takes the repository path as its first argument. Commands that report
results accept `--json` and then print exactly one JSON object to stdout. Human-readable output and
logs go to stderr. JSON strings are UTF-8; bytes of a name that are not valid UTF-8 appear as U+FFFD,
and `ls --json` then adds the name's exact bytes as `name_hex` (D71).

| Command | Effect |
|---|---|
| `dds init <repo> [--chunker fastcdc\|fixed] [--min-size B] [--avg-size B] [--max-size B] [--container-size B] [--compression none\|zstd[:LEVEL]]` | Create an empty repository. `--compression` stores each new blob as a zstd frame when that is smaller (D107): `zstd` means level 3, levels run from 1 to 19, and the default is `none`. Sizes accept `K`/`M` suffixes (powers of 1024). Defaults: FastCDC, 2K / 8K / 64K, 4M containers (D8, D9). With `--chunker fixed`, every chunk is `--avg-size` bytes (default 8K), and `--min-size`/`--max-size` are rejected. `<repo>` must not exist, or must be empty, or must be left over from an init that crashed (ARCHITECTURE.md 7.7); otherwise `AlreadyExists`. |
| `dds backup <repo> <path> [--threads N] [--split-size B] [--tag TEXT] [--parent auto\|none\|ID]` | Snapshot the tree at `<path>`. Prints the snapshot id alone on stdout (its summary goes to stderr, D70). `N` defaults to the hardware thread count. A file of at least `B` bytes (default 64M; 0 never) is chunked by one worker and stored by all of them (D109); the snapshot is the same either way. `TEXT` is at most 1024 bytes. Files unchanged since the parent snapshot are not read (section 4); `auto`, the default, is the newest snapshot this repository made of the same path on the same host, and `none` reads every file. |
| `dds restore <repo> <snapshot> <dest> [--no-verify]` | Restore into `<dest>`, which must not exist or must be an empty directory. Verifies BLAKE3 of every chunk unless `--no-verify`. Never replaces anything: a name the destination file system cannot hold separately (for example names differing only in case on a case-insensitive volume, or bytes the volume rejects) fails with `AlreadyExists` or `IoError` naming the entry. |
| `dds snapshots <repo>` | List snapshots: id, time, source path, tag, logical bytes, file count. |
| `dds ls <repo> <snapshot> [<path-in-snapshot>]` | List a directory inside a snapshot. |
| `dds rm <repo> <snapshot>...` | Remove snapshots. Space is reclaimed by the next `gc`. |
| `dds gc <repo> [--threshold F] [--dry-run]` | Run garbage collection. `F` in (0, 1], default 0.5: containers with live fraction below `F` are compacted. `--dry-run` reports what would happen without changing anything. |
| `dds verify <repo> [--deep]` | Check repository consistency (see section 5). `--deep` also recomputes BLAKE3 of every reachable blob. |
| `dds stats <repo>` | Logical, unique, and physical bytes; dedup ratio; container count; live-fraction histogram; duplicate copies; index memory. |
| `dds serve <repo> --listen HOST:PORT [--gc-interval DURATION]` | Accept replication connections. Optionally run GC periodically in the same process. `DURATION` is an integer followed by `s`, `m`, or `h` (for example `30s`, `10m`). |
| `dds replicate <repo> <snapshot> --to HOST:PORT [--window W] [--no-prune] [--json]` | Send a snapshot to a serving repository. `W` = outstanding batches, default 8. Subtrees the receiver holds whole are skipped (protocol version 2, D108); `--no-prune` queries every blob over version 1, which receivers built before version 2 also speak. A connection that cannot be made is an I/O error (exit 5, D91). |
| `dds chunk-stats <file> [--chunker ...] [--min-size ...] [--avg-size ...] [--max-size ...]` | Development tool: chunk a file and print the chunk count, size histogram, and throughput. Touches no repository. |
| `dds --version`, `dds --help`, `dds <command> --help` | |

Snapshots are named by full id (32 hex characters) or by any unique prefix of at least 6
characters.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success (`kOk`). |
| 1 | Usage error: bad or conflicting command-line arguments (detected by the CLI parser). |
| 2 | Not found (`kNotFound`): repository, snapshot, or path. |
| 3 | Corruption detected (`kCorruption`). |
| 4 | Repository locked by another live process (`kBusy`). |
| 5 | I/O error (`kIoError`). |
| 6 | Aborted (`kAborted`): the replication peer disconnected or ended the session with an error. |
| 7 | Incompatible (`kIncompatible`): repository format version, or replication peers with different chunker parameters. |
| 8 | Invalid input (`kInvalidArgument` from the library): for example a file or directory exceeding a limit in section 6. |
| 9 | Already exists (`kAlreadyExists`): `init` into a non-empty directory, or a restore name collision. |

Inside one process, `Aborted` also covers a restore whose snapshot is removed while it runs
(ARCHITECTURE.md 4.2). The CLI cannot produce that case, because one repository is open in one
process at a time (D16), but tests and `serve` can.

## 4. What a backup stores

- Regular files: content (chunked), permission bits, modification time, size.
- Directories: entries sorted by name (bytewise), permission bits, modification time.
- Symlinks: the target string, stored as is and never followed during backup.
- Skipped with a counted warning: devices, FIFOs, sockets. Hard links are stored as independent
  files (their data deduplicates anyway). Ownership (uid/gid), extended attributes, ACLs, and
  sparseness are not preserved (section 7).
- A file that changes size while it is being read is stored as read and counted as a warning,
  never as an error.
- With a parent snapshot (D106), a regular file is not read when the parent has a file of the same
  name, size, and modification time to the nanosecond, and that time is more than 2 s older than
  the start of the parent's backup: the parent's content for it is stored again instead. A change
  that keeps a file's size and sets its modification time back to the old value is therefore not
  noticed; `--parent none` reads every file. A parent written before manifest version 2 records no
  start time, so no file is skipped against it.

## 5. What `verify` checks

1. The config and every snapshot manifest parse and pass their checksums.
2. Every container's header, footer, and trailer parse and pass their checksums, and every index
   entry that points into a valid container equals one of its footer's entries (D104).
3. Every blob reachable from every snapshot is present in the index, and its type matches how it is
   referenced.
4. Every tree and recipe blob reachable from a snapshot parses (G10's name rules included).
5. With `--deep`: every reachable blob's bytes hash to its fingerprint.

`verify` reports unreachable blobs and duplicate copies as space, not as errors.

## 6. Limits (default parameters)

| Item | Limit | Why |
|---|---|---|
| File name | 1 to 255 bytes, no `/`, no NUL, not `.` or `..` | POSIX `NAME_MAX`; G10. |
| Symlink target | ≤ 4095 bytes | POSIX `PATH_MAX` − 1. |
| Data blob | `max-size` (default 64 KiB, at most 16 MiB) | Chunker bound. |
| Tree or recipe blob | ≤ 64 MiB | Keeps any single blob bounded in memory. A directory needing a larger tree is rejected with a clear error. |
| Chunk references per recipe piece | ≤ 4096 | D14. |
| Recipe pieces per file | ≤ 65,536, so about 2 TiB per file at the 8 KiB average | D14. |
| Container | < 4 GiB (offsets are 32-bit) | Containers seal at about the target size. |

Exceeding a limit fails the backup with `InvalidArgument` naming the path. Nothing is committed.

## 7. Non-goals (v1)

Compression by default (it is an init option, D107); encryption (D40); preserving ownership,
xattrs, ACLs, hard-link identity, or sparseness (D41); more than one process opening a repository
at a time (D16); a mounted file-system view (D47); authentication or TLS for replication, so use an
SSH tunnel (D48); Windows (D5).
