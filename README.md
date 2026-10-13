# dedupstore

A deduplicating, crash-safe snapshot store in C++20, with a concurrent garbage collector and
WAN-efficient replication.

- **Dedup:** content-defined chunking (FastCDC) and BLAKE3 fingerprints. Each unique chunk is
  stored once, in immutable append-only containers.
- **Crash safety:** no write-ahead log. Every durable change is an immutable file made visible by
  an atomic rename. A crash simulator explores every crash point to prove it.
- **Reclamation:** a mark-and-sweep GC with copy-forward compaction runs concurrently with backups,
  restores, and replication. Epoch stamps keep it from deleting a chunk a running backup has just
  deduplicated against. Epoch-based reclamation keeps it from deleting a container a reader is
  still using.
- **Replication:** sends only the chunks the remote lacks, over a pipelined protocol measured on a
  simulated high-latency, low-bandwidth link.

## Status

Complete and measured: storage, snapshots, crash consistency, parallel ingest, garbage collection,
replication, and measurement on Explorer, each with its acceptance tests. On top of these, each
measured against a prediction written down before its run: an index checkpoint for fast open
(D104), hardware CRC32C (D103), zstd compression (D107), replication that skips unchanged subtrees
(D108), backups that skip files unchanged since a parent snapshot (D106), large files split across
workers (D109), and ingest scaling (D101, D102).

## Quick start

Requires CMake 3.25 or later, Ninja, and Apple clang, GCC 11 or later, or clang 15 or later. The
first configure downloads BLAKE3, zstd, GoogleTest, and Google Benchmark at pinned versions.

```bash
cmake --preset release && cmake --build --preset release
./build/release/dds init repo
./build/release/dds backup repo ~/project          # prints the snapshot id
./build/release/dds snapshots repo
./build/release/dds restore repo <id-prefix> restored
./build/release/dds verify repo --deep
./build/release/dds init replica
./build/release/dds serve replica --listen 127.0.0.1:7433 &       # or in another terminal
./build/release/dds replicate repo <id-prefix> --to 127.0.0.1:7433
./build/release/dds rm repo <id-prefix> && ./build/release/dds gc repo --threshold 0.5
```

Tests: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug` (and the
`asan`, `tsan`, and `debug-slow` presets; `docs/TESTING.md`).

## Documents

| File | What it is |
|---|---|
| `docs/SPEC.md` | What the system does and guarantees (G1–G11), the CLI, limits, non-goals. |
| `docs/ARCHITECTURE.md` | Modules, interfaces, data flow, concurrency model, the GC safety argument, the durability protocol. |
| `docs/ON_DISK_FORMAT.md` | Every byte of every file in a repository. |
| `docs/PROTOCOL.md` | The replication wire protocol. |
| `docs/DECISIONS.md` | Every design decision, with evidence and when to revisit it. |
| `docs/TESTING.md` | The test families, including crash simulation, stress tests, fuzzing, and mutation checks. |
| `docs/BENCHMARKS.md` | Measurement protocol, datasets, experiments E1–E9 and the later ones, with every prediction and outcome. |
| `docs/RESULTS.md` | Every measured number, generated from `bench/results/`. |

## Results

Quoted from `docs/RESULTS.md`, which `tools/report.py` generates from the raw result files in
`bench/results/`; each number there links to its file (run id, commit, machine, compiler, flags).
All runs: Linux, Intel Xeon Platinum 8276 (56 cores), exclusive node, GCC 11.5 `-O2` (11.4.1 for the
E1, E2, E6, and E8 runs shown, from a node with an older system compiler; D94), inside SLURM jobs
on Northeastern's Explorer cluster. Each prediction was written down before its run, and
`docs/BENCHMARKS.md` section 5 records every outcome, misses included.

- **Dedup (E1):** 11 Linux releases (6.0 to 6.10) store in 2697 MiB of unique data: ratio 5.25
  with FastCDC against 3.82 with fixed-size chunks. On data with inserts and deletes (`synth`),
  FastCDC reaches 5.94 and fixed-size chunks 1.03.
- **Ingest (E3):** 3.8 GB/s of kernel source with 16 threads in memory (4.6 with 32), 350 MB/s
  to disk, where the store step dominates; to disk, the dedup-heavy second backup runs 3.4x to 6.1x
  faster.
- **Restore (E4):** 346 to 491 MB/s; a release scattered over 4x more containers restores 8%
  slower (warm cache).
- **Garbage collection (E5, E6):** compaction at threshold 0.9 copies 1.58 bytes per byte freed. A
  GC looping beside a backup slows it by 1%.
- **Replication (E7):** over a simulated 100 Mbit/s link with 80 ms round trips, 2 outstanding
  batches reach 92% of the link; an edited version sends 0.365 of its bytes.
- **Open (E9):** a 100 GiB repository (25,801 containers) opens in 1.0 s, rebuilding its index.
- **Checkpoints (E8):** successive checkpoints of a full fine-tune share almost nothing (ratio
  1.008); a partial fine-tune shares its frozen 82.7% of weights (ratio 2.06).

Measuring found bugs, recorded with before-and-after runs. A per-file buffer zero-fill held ingest
at a 1 MiB chunk size to 94 MB/s; without it, the same node does 2488 (D98, E2ab). Removing it
nearly doubled 64 KiB ingest with 4 threads but slowed it with 16, exposing lock contention in the
store path as ingest's scaling limit (D100). Sharding the backup session's lock (D101) sped up new
data but slowed dedup-heavy backups; handing files to workers in batches (D102) fixed that, and
together they run 1.19x to 2.60x as fast as the build before both on the same node (S8ab). Neither
gets there alone: batching alone reaches 1.10x to 1.91x, and sharding adds 1.07x to 2.01x on top of
it (S8alone). Also found: a write-amplification metric with the wrong denominator, and two flawed
microbenchmarks.

Later additions, each against the build without it on the same node unless stated:

- **Index checkpoint (S1open):** a 100 GiB repository opens in 0.286 s instead of 1.10.
- **Hardware CRC32C (S2ab2):** 10.64 GB/s against 2.66 for the table version; ingest 1.02x to
  1.16x as fast. The first measurement (S2ab) found GCC compiling the 8-byte loads as byte loops,
  which the second fixed.
- **Compression (S3zstd):** zstd at level 3 stores kernel source 3.55x smaller with dedup
  unchanged, at 0.63x the ingest and restore speed. Off by default.
- **Subtree pruning in replication (S4prune):** with 15 files changed in linux-6.10, 39.6x fewer
  query bytes and 1.75x (fast link) to 9.78x (10 Mbit/s) as fast; 0.98x when nothing can be
  pruned. The receiver checks each subtree when asked instead of relying on an invariant GC would
  have to maintain (D108). A review then found three problems, fixed with tests; rerun against the
  measured build (S4prune2), the fixed code sends identical bytes and is no slower.
- **Backups with a parent (S5parent2):** the second backup of an unchanged kernel tree runs 9.06x
  as fast with 1 thread and 2.60x with 4, but 0.85x with 16, where a backup reading every file
  from the page cache is already faster than looking up and stamping the reused blobs; recorded as
  a miss.
- **Large files split across workers (S6split, S6split2):** with 16 threads, one 8 GiB file ingests
  1.88x as fast and full fine-tune checkpoints 1.85x, with byte-identical snapshots; reserving
  each segment's buffer once then added 1.12x to 1.14x. One reader per file is the limit, about
  1.3 GB/s against 4.2 GB/s for many files.

## License

MIT; see `LICENSE`.
