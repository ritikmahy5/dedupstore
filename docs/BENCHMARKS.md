# dedupstore — benchmarks and measurement protocol

How every reported number is produced. If a number is not produced this way, it is not reported.

## 1. Environment

- **Machine:** Northeastern's Explorer cluster, Linux x86-64, inside a SLURM job, never on a login
  node. Each result records the node name, CPU model, core count, memory, kernel version, compiler
  and version, preset, and `git rev-parse HEAD` plus a dirty flag. A run from a dirty tree is
  allowed for exploration but is excluded from `docs/RESULTS.md`.
- **Partition and node-local disk (D94):** whole cascadelake nodes of `short`
  (`--constraint=cascadelake --exclusive`; Intel Xeon Platinum 8276, 56 CPUs, 186 GB). Datasets and
  disk-mode repositories on node-local /tmp (xfs). Throughput depends on the file system, so every
  result records the file system type and mount of the repository and of the dataset.
- **Two storage modes,** reported separately and never mixed in one table:
  - `mem`: the repository on `MemFs`. Measures CPU cost (chunking, hashing, indexing, copying)
    with no storage I/O.
  - `disk`: the repository on `PosixFs` on the recorded file system.
- **Page cache:** dropping caches needs root, which a cluster user does not have. Before each
  measured run, the dataset is read once (warm cache), and results say "warm source cache". Cold
  numbers are not claimed.
- **Developer laptop (macOS):** smoke tests only. Never reported.

## 2. Protocol

- One warm-up run, discarded. Then 5 measured runs. Report the median, with min and max.
- Every run writes one JSON file to `bench/results/<experiment>/<run_id>.json`, append-only.
  `run_id` = UTC timestamp + short commit + node. Existing files are never edited or deleted. A
  bad run is superseded by a newer one, and the report generator takes the latest complete set.
- `tools/report.py` (Python 3, standard library, plus matplotlib if available) reads
  `bench/results/` and writes `docs/RESULTS.md` and `docs/figures/`. Every number in RESULTS.md
  links to the run ids it came from. README quotes only RESULTS.md.
- **Predict first:** before an experiment's first measured run, write its prediction in section 5
  under "Prediction", with a tolerance, and commit it. After the run, add "Outcome" next to it,
  including misses.

## 3. Result file schema

```json
{
  "schema": 1,
  "run_id": "...", "experiment": "E3", "git_commit": "...", "git_dirty": false,
  "started_at": "2026-...Z",
  "machine": {"node": "...", "cpu": "...", "cores": 32, "mem_gb": 256, "kernel": "...",
              "fs_repo": "...", "fs_dataset": "..."},
  "build": {"compiler": "gcc 13.2.0", "preset": "release", "flags": "..."},
  "dataset": {"name": "kernel", "versions": ["6.0", "..."], "logical_bytes": 0, "files": 0,
              "verification": "sha256sums.asc | tofu"},
  "params": {"chunker": "fastcdc-2016", "min": 2048, "avg": 8192, "max": 65536,
             "threads": 8, "storage_mode": "disk"},
  "samples": [{"wall_s": 0.0, "counters": {}}],
  "summary": {"median": {}, "min": {}, "max": {}}
}
```

`counters` are the component counters from ARCHITECTURE.md section 11, exported by `--json`.

## 4. Datasets

| Id | What | How obtained | Why |
|---|---|---|---|
| `kernel` | Linux source releases 6.0 through 6.10, each extracted to its own directory | `bench/datasets/fetch_kernel.sh` downloads `linux-6.N.tar.xz` from `https://cdn.kernel.org/pub/linux/kernel/v6.x/`, verifies against that directory's `sha256sums.asc` if present, otherwise records the SHA-256 on first download and marks results "tofu" | Public, reproducible, and the classic dedup benchmark: successive versions share most content. |
| `synth` | A seeded base of 2 GiB of random bytes plus 1 GiB of text-like bytes, then 10 versions, each applying random inserts, deletes, and overwrites at a set edit rate | `bench/gen_synth` (C++, part of this repository), seed recorded | Controlled edits. The expected dedup can be estimated from the edit rate, so the chunker can be judged against it. |
| `ckpt` | Model checkpoints on Explorer: successive steps of a full fine-tune, successive steps of LoRA runs, and runs across seeds | Paths supplied to the job. Never copied into the repository. Results record model, method, step, and file sizes only. | The angle no other dedup project has: how much do training checkpoints actually share? |

Datasets live under `/scratch/<user>/dedupstore-data/`, never in the git repository.

## 5. Experiments

Each experiment lists its question, setup, and metrics, and has a "Prediction" slot to fill before
running (section 2). Experiments E1 to E9 were planned first; the other names label later
experiments, and each name is the directory of its result files under `bench/results/`.

**Rerun of E1, E2, E3, and E6 (prediction committed 2026-10-09, before the rerun).** The first runs
(commit 8dd74df) found that every file's `ChunkReader` allocated and zero-filled 2 x the maximum
chunk size; commit 2107407 lends each backup worker one buffer instead. Those four experiments are
rerun on the fixed code, and both runs stay in `bench/results/` (RESULTS.md shows the latest).
Expected: every dedup ratio, blob count, and byte count identical to the first run, since chunk
boundaries do not depend on the buffer; E2's 1 MiB ingest at least 1500 MB/s (it was 93, against
2979 at 64 KiB); throughput at 8 KiB unchanged or higher by at most 25% (256 KiB zero-filled per
17 KB file was a small cost next to reading, hashing, and storing it).

**Same-node comparison E2ab (prediction committed 2026-10-09, before it ran).** The reruns landed on
another node (d0020, against d0030), and some throughputs moved by up to 17% in either direction,
more than the spread within a job, so a comparison across jobs cannot isolate the fix. E2ab
(`bench/slurm/e2_ab.sbatch`) builds both commits in one job on one node and alternates them.
Expected, as new / old ingest MB/s: at least 10 at 1 MiB; between 1.00 and 1.25 at 8 KiB and at
64 KiB; and the two passes of each build within 5% of each other.
Outcome (job 10953149, node d0029; RESULTS.md E2ab): 1 MiB 26.5x (hit). 8 KiB 0.97 (miss, by less
than the 3% between passes). 64 KiB 0.83: the fix made it slower (miss), with both new passes below
both old ones, so not drift. Passes within 5% except the new build at 64 KiB (7%). Every dedup count
is identical between the builds. Each build's medians here are within 5.3% of the same build's on
nodes d0030 and d0020, so the reruns' changes at 8 and 64 KiB came from the fix, not from the node.

**E2ab-lock, why the fix slowed 64 KiB (prediction committed 2026-10-10, before it ran).** E2ab
(job 10953149) measured new / old 0.83 at 64 KiB, with summed `store` time up from 16 to 23 s and
nothing else in the library changed. Hypothesis: every blob lookup and put of a backup takes the
one `IngestSession` mutex, holding it across an index probe; the per-file zero-fill had spaced the
workers' arrivals at that lock, and without it the lock is more contended. `bench/slurm/e2_ab_lock.sbatch`
runs both builds at 64 KiB with 4 and 16 threads and counts context switches with `perf stat`.
Expected: at 4 threads new / old at least 1.00 (little contention, so less work is simply
faster); at 16 threads below 0.90 again; at 16 threads the new build makes at least 1.5x the old
one's context switches per second (threads blocking on the mutex), and its profile shows more
time in mutex functions (`pthread_mutex_lock`, `__lll_lock_wait`) than the old one's.
Outcome (job 10953982, node d0029; RESULTS.md E2ab and the job's `perf-lock.txt`): 4 threads
1.90 (hit: 1087 to 2068 MB/s; the zero-fill was 57.6% of the old build's samples at 64 KiB). 16
threads 0.84 (hit). Context switches: not measured, since `perf stat` counts user space only at
`perf_event_paranoid` 2 and reported 0 for both builds. Mutex functions: 5.7% of the new build's
samples against 1.1% of the old one's (hit). And `perf stat` shows the new build doing 30% less
work (91 against 131 CPU-seconds) in more time (12.1 against 10.4 s), with 7.5 CPUs busy against
12.6: its threads are blocked half the time. The new build gains only 1.22x from 4 to 16 threads.
Consistent with the hypothesis, not proof of it: the session mutex is the one lock every lookup and
put of every worker takes, and its pending map (`std::_Hashtable<dds::Fingerprint, ..., bool>`)
appears in the profile, but user-space sampling without frame pointers cannot attribute the
waiting to a particular mutex. D100 records the limit and the proposed fix.

**S7ab and the E3 rerun, sharding the session mutex (prediction committed 2026-10-10, before
either ran).** Sharding (D101) splits the session's pending map and mutex into 256 shards by
fingerprint. `bench/slurm/s7_ab.sbatch` builds the commit before sharding (64d51e6, whose library
is E2ab-lock's new build) next to the sharded build on one node and alternates them; E3 is rerun on
the sharded build as it is.
Expected, as new / old ingest MB/s in S7ab: 64 KiB with 4 threads between 0.95 and 1.05 (little
contention at 4); with 16 threads at least 1.15, recovering at least the 2960 MB/s that the build
with the zero-fill reached (E2ab); with 32 threads at least 1.25; 8 KiB with 16 threads at least
1.05 and with 32 at least 1.15. At 64 KiB with 16 threads, `perf stat` shows the new build with at
least 10 CPUs busy (the old one: 7.5 in E2ab-lock). Every dedup count is identical. E3 rerun: dedup
counts identical; mem mode at 16 threads at least 1840 MB/s for 6.10 (1.15x the 1601 before
sharding); mem mode at 32 threads no slower than at 16 (before: 1378 against 1601); time summed in
`store` at 32 threads at most half the 26.2 s before; disk mode no slower than before at any thread
count.
Outcome (S7ab job 10957115 and E3 job 10957122, node d0029; RESULTS.md S7ab and E3): sharding
removed the session-lock waiting (summed `store` time at 64 KiB with 16 threads 22.1 to 9.3 s; at 8
KiB with 32 threads 109.0 to 13.0 s; E3 mem mode at 32 threads 26.2 to 3.6 s, hit) and changed no
dedup count (hit), but throughput split by workload. E3's first backup (6.10, mostly new data)
gained: 1532 to 2129 MB/s at 8 threads and 1601 to 2321 at 16 (hit), though 32 threads (1586)
stayed slower than 16 (miss). Dedup-heavy ingest lost at high thread counts: E3's second backup
(6.9) 2607 to 2226 MB/s at 16 threads and 1999 to 1407 at 32, and S7ab's 11-version workload,
mostly dedup-heavy: 64 KiB 1.04 at 4 threads (hit), 0.79 at 16 and 0.68 at 32, 8 KiB 0.87 at 16
and 0.67 at 32 (all misses). Disk mode: unchanged within 6% except 6.9 at 32 threads, 1305 to 1079
(miss). `perf stat` at 64 KiB with 16 threads: 6.0 CPUs busy against 7.05 (miss: predicted at least
10); both builds spend about 45% of their CPU time in the kernel (43 and 39 of 92 s), which the
user-space-only profile cannot see, and their user-space profiles are nearly identical. In every
losing configuration the time per byte of hashing and reading rose (64 KiB, 32 threads: hashing
11.3 to 20.3 s summed), and wall time tracks the single walker. Hypothesis, not tested: without
the session lock pacing them, workers that have little to store wait on the file queue instead,
and the one walker thread, which every file passes through, is slowed by that queue's contention
and by lower clock speeds with more cores busy. The prediction assumed the session lock was the
only serial point; the walker and its queue are another.

**S8ab and a second E3 rerun, sharding plus a batched file queue (prediction committed
2026-10-10, before either ran).** Batching (D102) has the walker push batches of up to 64 files.
`s8_ab.sbatch` compares sharding plus batching against the build before sharding (64d51e6) on one
node, in S7ab's configurations; E3 is rerun on sharding plus batching. The criterion: if
dedup-heavy ingest is still slower than before sharding, both changes are reverted. Expected, as new
/ old in S8ab: at least 1.00 in every configuration (the dedup-heavy losses gone), and at least 1.10
at 64 KiB with 16 threads; at 64 KiB with 16 threads, `perf stat` shows at least as many CPUs busy
as the old build. E3: dedup counts identical; the dedup-heavy second backup (6.9) in mem mode at
least as fast as before sharding at 16 and 32 threads (2607 and 1999 MB/s); the first backup (6.10)
keeps sharding's gains (at least 2129 MB/s at 8 threads and 2321 at 16).
Outcome (S8ab job 10971699 and E3 job 10971713, node d0036; RESULTS.md S8ab and E3): every
prediction hit, so by the criterion both sharding and batching stay. S8ab, new / old: 64 KiB 1.19 at
4 threads, 2.14 at 16 (2553 to 5472 MB/s, the mean of each build's two medians), and 2.60 at 32; 8
KiB 2.11 at 16 and 2.49 at 32. Every dedup count is identical across the two builds and all runs;
only the number of containers varies (at 64 KiB, 1035 to 1046 with 4 threads and up to 1238 with
32), since every writer that stored data seals its last, partial container when each backup ends,
and with it the physical bytes, by 68 per container (header, trailer, and footer count;
ON_DISK_FORMAT.md). `perf stat` at 64 KiB with 16 threads: 10.3 CPUs busy against 8.2 (hit), and the
same work in 59.8 CPU-seconds against 92.6, mostly less kernel time (18.6 against 45.6 s). Mutex
functions fell from 5.7% of the user-space samples to 1.9% (counting the symbols the report lists,
those above 0.5%). In summed times at 64 KiB with 32 threads, `store` went from 54.4 to 10.7 s, and
hashing from 12.0 to 8.3 s, so the rise in hashing time that sharding alone caused (11.3 to 20.3 s
in S7ab) is gone. E3: dedup counts identical to the two earlier E3 runs (hit). The second backup
(6.9) in mem mode ran at 5448 MB/s at 16 threads and 5363 at 32 (hit); the first (6.10) at 2348 at 8
threads and 3827 at 16 (hit), and at 32 threads 4556, now faster than at 16 (the sharded build's
miss at 32 threads is gone). Disk mode was at least as fast as the run before sharding and the
sharded run at every thread count; 6.9 at 32 threads went from 1305 (before sharding) and 1079
(sharding) to 2170. E3's runs come from three nodes
(d0020, d0029, d0036), so its comparisons across runs include any difference between nodes; S8ab is
the same-node comparison. The old build ran at nearly the same speed on d0036 as in S7ab on d0029
(64 KiB, 16 threads: 2558 and 2549 against 2361 and 2601). What this does not show: batching
without sharding was not measured, so how much of the gain needs the sharded lock is unknown. The
result is consistent with D102's hypothesis that the per-file queue round trip was the serial point
that sharding exposed. The drop in kernel time fits less blocking on contended locks, but the
user-space-only profile cannot show which locks.

**S8alone, batching with and without sharding (prediction committed 2026-10-10, before it ran).**
S8ab measured sharding and batching together, so it cannot say whether sharding is needed.
`bench/slurm/s8_alone.sbatch` builds three versions on one node: the commit before sharding (old,
64d51e6), batching alone on that commit (s8, c853572, a cherry-pick of afa1f9f that is kept but not
merged; 433 tests pass on it under debug and TSan), and sharding with batching (new, the job's
commit). It runs them in the order old, s8, new, new, s8, old in S8ab's configurations. Hypothesis:
both serial points matter. Without sharding, batching moves the waiting back to the session lock,
which costs most where lookups are densest: more threads and smaller chunks. Expected:
- s8 / old at least 1.10 in every configuration (batching helps even with one session lock).
- new / s8 between 0.95 and 1.10 at 64 KiB with 4 threads, where there is little contention
  (sharding alone gave 1.04 there).
- new / s8 at least 1.10 in each 16- and 32-thread configuration, largest at 8 KiB with 32 threads.
- Summed `store` time at 8 KiB with 32 threads at least twice as long for s8 as for new.
- At 64 KiB with 16 threads, `perf stat` shows at least as many CPUs busy for new as for s8.
- Every dedup count identical across the three builds.

What follows from the outcome: if new / s8 is below 1.05 in every configuration, sharding adds
no measured speed on top of batching, and reverting it is proposed. Otherwise sharding stays, and
D101 records what it adds.
Outcome (job 10974438, node d0036; RESULTS.md S8alone): five predictions hit and one missed;
sharding stays. s8 / old: 64 KiB 1.10 at 4 threads (1.102, a hit by a hair), 1.54 at 16, and 1.91 at
32; 8 KiB 1.17 at 16 and 1.22 at 32 (hit). new / s8: 1.07 at 64 KiB with 4 threads (hit); 64 KiB
1.43 at 16 and 1.33 at 32, 8 KiB 1.83 at 16 and 2.01 at 32 (hit, largest at 8 KiB with 32 threads).
Summed `store` time at 8 KiB with 32 threads, a and b passes: 118.5 and 128.0 s for s8 against 8.9
and 9.1 for new (hit), and longer for s8 than for old (104.6 and 112.5): with batching, workers
reach the session lock sooner and wait there. `perf stat` at 64 KiB with 16 threads: 11.0 CPUs busy
for s8 against 10.1 for new (miss). Batching alone keeps more CPUs busy but spends 87.1 CPU-seconds
on the work against 60.2, 41.5 s of it in the kernel against 19.0 (old: 90.5 CPU-seconds, 43.3 in
the kernel, 7.7 CPUs busy). Busy CPUs were the wrong stand-in for useful work when threads contend,
and kernel time fell substantially only with both changes (sharding alone in S7ab: 39 s against 43,
on another node). Every dedup count is identical across the three builds (hit). Reading: each change
removes a different serial point, and neither alone reaches the combination (64 KiB, 32 threads:
batching alone 1.91, sharding alone 0.68 in S7ab on another node, both 2.55). Batching helps most
with large chunks (1.91 at 64 KiB against 1.22 at 8 KiB, 32 threads), where a file has few chunks
and the per-file queue round trip is a large share of each file's work. Sharding adds most with
small chunks (2.01 at 8 KiB against 1.33 at 64 KiB, 32 threads), where lookups through the one
session lock are densest. Every ratio here is consistent with that reading, but no profile shows it
directly. By the rule committed with the prediction, sharding adds more than 5% in every
configuration, so it stays.

**S2ab, hardware CRC32C (prediction committed 2026-10-10, before it ran).** Hardware CRC32C (D103)
computes CRC32C with SSE4.2's `crc32` instruction on Explorer. `bench/slurm/s2_ab.sbatch` builds the
commit before it (33df8db) next to it on one node and alternates them in S8ab's configurations,
then runs the CRC32C microbenchmarks of the new build, which has both versions. CRC32C runs only
over stored data, and in the current build's profile at 64 KiB with 16 threads it was 13.5% of the
user-space samples, about 5.5 of 60 CPU-seconds (S8alone). At 3 cycles per 8 bytes the instruction
should be about 4 times as fast as slicing-by-8, saving about 4 CPU-seconds, 7% of the total.
Expected:
- Microbenchmarks: BM_Crc32c (labelled `sse4.2`) between 6 and 11 GB/s at 64 KiB, and at least 3
  times BM_Crc32cSoftware at every size from 4 KiB up; BM_Crc32cSoftware within 10% of BM_Crc32c's
  2.2 GB/s before the change (job 10930767), since it is the same code.
- new / old ingest between 1.02 and 1.12 in every configuration.
- At 64 KiB with 16 threads, `perf stat` shows the new build using at least 3 CPU-seconds less than
  the old, and in its profile the CRC32C functions (symbols containing `Crc32c` or
  `UpdateHardware`) are below 5% of the samples, against about 13% for the old build.
- Every dedup count identical across the two builds; every result of the new build records
  `params.crc32c` = `sse4.2`.

What follows from the outcome: if new / old is below 1.00 in any configuration, the hardware path
costs something I do not understand, and I will find out what before keeping it.
Outcome (job 10976818, node d0031; RESULTS.md S2ab and the CRC32C microbenchmarks): every
prediction about speed missed, and the cause is in the code, not the measurement. BM_Crc32c,
labelled `sse4.2`, ran at 1.77 GB/s at 64 KiB, slower than BM_Crc32cSoftware at 2.23 GB/s (miss;
the software version within 10% of the old 2.2, hit). new / old: 64 KiB 1.01 at 4 threads, 0.99 at
16, and 0.93 at 32; 8 KiB 1.00 at 16 and 1.03 at 32 (miss: predicted 1.02 to 1.12). `perf stat` at
64 KiB with 16 threads: 64.1 CPU-seconds against 63.8 (miss: predicted at least 3 fewer), and the
CRC symbol 14.5% of the new build's samples against 12.5% of the old one's (miss: predicted below
5%). Every dedup count is identical across the builds and passes (hit), and every new result
records `params.crc32c` = `sse4.2` (hit). The rule committed with the prediction applies (0.93 and
0.99 are below 1.00), so I looked for the cost. GCC 11.5's assembly of `crc32c.cc`, compiled on a
compute node, shows it: `LoadU64` (encoding.h), a loop of eight byte loads, variable shifts, and
ORs, is not merged into one 8-byte load at `-O2`, so each `crc32q` waits on about 56 instructions,
and the slicing-by-8 version's `LoadU32` likely the same. Clang merges the loop, which is why the
laptop's ARM build ran at 9.6 GiB/s. Next: the load helpers are rewritten so GCC merges them,
checked in its assembly, and hardware CRC32C is measured again (S2ab2) with a prediction of its own.

**S2ab2, hardware CRC32C after the load fix (prediction committed 2026-10-10, before it ran).** S2ab
found GCC compiling the load helpers as byte loops (commit 6366820 rewrote them, checked in GCC
11.5's assembly). `bench/slurm/s2_ab2.sbatch` builds three versions on one node: the commit before
hardware CRC32C (old, 33df8db), hardware CRC32C as S2ab measured it plus the load fix and nothing
else (s2fix, 6366820, kept but not merged), and the job's commit, which also has the index
checkpoint and backups with a parent snapshot (new). It
runs them in the order old, s2fix, new, new, s2fix, old in S8ab's configurations, then the CRC32C
microbenchmarks of the job's build, where the load fix speeds up the slicing-by-8 version too.
Expected:
- BM_Crc32c (labelled `sse4.2`) between 6 and 11 GB/s at 64 KiB, and at least 2 times
  BM_Crc32cSoftware at every size from 4 KiB up.
- s2fix / old between 1.02 and 1.12 in every configuration (S2ab's reasoning: CRC32C was about 5 of
  64 CPU-seconds).
- new / s2fix between 0.97 and 1.03 in every configuration: the index checkpoint is written
  when a repository closes, after the timed backups, and the parent-snapshot code does nothing
  without a parent.
- At 64 KiB with 16 threads, `perf stat` shows s2fix using at least 3 CPU-seconds less than old,
  with the CRC32C symbols below 5% of its samples.
- Every dedup count identical across the three builds; every s2fix and new result records
  `params.crc32c` = `sse4.2`.

What follows from the outcome: if s2fix / old is below 1.00 in any configuration, the hardware path
still costs something, and removing it is proposed.
Outcome (job 10978928, node d0031; RESULTS.md S2ab2 and the CRC32C microbenchmarks): eight
predictions hit and two missed, both at 64 KiB with 4 threads; hardware CRC32C stays. BM_Crc32c
(`sse4.2`) ran at 10.64 GB/s at 64 KiB, 4.0 to 5.0 times BM_Crc32cSoftware from 4 KiB up, which the
load fix raised from 2.23 to 2.66 GB/s (hit). s2fix / old: 64 KiB 1.04 at 16 threads and 1.05 at 32,
8 KiB 1.06 at 16 and 1.02 at 32 (hit), and 1.16 at 64 KiB with 4 threads (miss: above the predicted
1.12). With fewer threads the session lock and the walker wait less, so CRC32C was a larger share of
the time than at 16 threads, where the estimate came from. new / s2fix: 0.99, 0.99, 0.98, and 1.01
at 16 and 32 threads (hit), and 0.94 at 64 KiB with 4 threads (miss: predicted 0.97 to 1.03),
consistent across both passes of each build (new 2566 and 2587 MB/s, s2fix 2732 and 2739). Nothing
the index checkpoint or the parent-snapshot code adds runs in a backup without a parent; a guess,
not tested, is that the extra code moved the hot FastCDC loop to a less favourable place in memory,
which matters most when 4 threads do not wait on each other. `perf stat` at 64 KiB with 16 threads:
s2fix 54.4 CPU-seconds against 60.5 for old (hit), and no CRC32C symbol among s2fix's samples above
the report's 0.5% floor, against 13.3% for old (hit). Every dedup count is identical across the
three builds (hit), and every s2fix and new result records `sse4.2` (hit).

**S1open, open with and without the index checkpoint (prediction committed 2026-10-10, before it
ran).** The index checkpoint (D104) lets open load the index from `index.ckpt` instead of reading
every container's footer. `bench/slurm/s1_open.sbatch` builds E9's repositories (10, 50, and 100 GiB
of unique synthetic data, the kernel at 8 and 2 KiB average chunks) on one node; each build's Close
writes the checkpoint. Then it opens each repository with the checkpoint off and on, in the order
off, on, on, off, and times writing the checkpoint. Both paths read about the same bytes (45 per
blob in footers, 49 in the checkpoint); the checkpoint saves one file open and read per container
and the hash tables' growth, so the gain should grow with the number of containers. E9
(job 10930747) opened 10 GiB in 0.079 s, 50 GiB in 0.466 s, 100 GiB in 1.014 s, and the kernel
repositories in 0.064 s (8 KiB) and 0.075 s (2 KiB); a laptop smoke test gave 1.4x at 1 GiB and 2.2x
at 4 GiB. Expected:
- Off / on: at least 2.0 at 50 and 100 GiB, at least 1.5 at 10 GiB, at least 1.2 for both kernel
  repositories (more blobs per container, so per-blob work, the same on both paths, weighs more);
  at most 10 anywhere.
- With the checkpoint on, no footer is read in any repository; blobs and index bytes are identical
  with it on and off.
- With it off, open times within 25% of E9's for the same repositories.
- The checkpoint is exactly 49 x blobs + 24 x containers + 2100 bytes (about 537 MiB at 100 GiB),
  and writing it at 100 GiB takes between 1 and 10 s.

What follows from the outcome: if off / on is below 1.5 at 100 GiB, D104's revisit condition is met,
and removing the checkpoint is proposed. Otherwise the outcome states how many opens one write must
be followed by to pay for itself (write time / (off - on)).
Outcome (job 10977387, node d0031, commit 8da2534; RESULTS.md S1open): every prediction hit, so the
checkpoint stays. Off / on: 3.86 at 100 GiB (1.10 s to 0.286 s, the means of the two passes'
medians), 3.43 at 50 GiB, and 2.51 at 10 GiB; 2.04 for the kernel at 8 KiB and 2.49 at 2 KiB. With
the checkpoint on, no footer was read in any repository, and blobs and index bytes were identical on
and off. Off, the opens took 0.082, 0.471, and 1.10 s (E9: 0.079, 0.466, 1.014) and 0.058 and 0.076
s for the kernel (E9: 0.064, 0.075), all within 9% of E9. Each checkpoint was exactly 49 x blobs +
24 x containers + 2100 bytes (537.6 MiB at 100 GiB; this commit predates compression (D107), whose
checkpoint version 2 adds one codec byte per entry), and writing it took 5.28 s at 100 GiB, 2.60 at
50, and 0.50 at 10. One write pays for itself after write / (off - on) opens: 6.5 at 100 GiB, 7.8 at
50, 10 at 10, 14 for the kernel at 8 KiB and 11 at 2 KiB; Close writes one only after 256 new or
deleted containers, so a repository is opened many times per write. With the checkpoint, open grows
by about 11 us per container from 10 to 100 GiB (against 39 us at 100 GiB without it, E9), which
extrapolated to 500 GB (about 119,000 containers) is about 1.3 s, an extrapolation from at most 100
GiB.

**S5parent, backups with and without a parent snapshot (prediction committed 2026-10-10, before it
ran).** A backup with a parent snapshot (D106) skips reading files unchanged since a parent
snapshot. `bench/slurm/s5_parent.sbatch` backs up linux-6.10 into a fresh disk repository and then
backs up a second tree with the first snapshot as its parent (previous) or without one (none), at 1,
4, and 16 threads, in the order none, previous, previous, none. The second tree is linux-6.10 again
("same"), or a copy that kept its mtimes with every 100th regular file appended to ("mod1pct").
Without a parent the second backup reads and hashes every byte to find it stored already: E3's
dedup-heavy backup to disk ran at 500, 1267, and 1988 MB/s at 1, 4, and 16 threads (job 10971713),
about 2.9, 1.1, and 0.7 s for 1.4 GB. With one, what is left is listing about 88,000 files on the
walker's one thread, reading each reused file's recipe, and looking up its chunks. Expected, as none
/ previous for the second backup's seconds:
- same: at least 2.0 at 1 thread, at least 1.5 at 4, at least 1.2 at 16; mod1pct: at least 1.8,
  1.4, and 1.1; at most 20 anywhere.
- With the parent, files reused = files for same, and files minus the modified count the job
  prints for mod1pct; without it, 0.
- In each workload, the second snapshot's root (its first 48 bits) is the same in every run, with
  or without the parent.

What follows from the outcome: if a backup with the parent is slower than one without at 1 thread
in either workload, D106's revisit condition is met, and removing parent snapshots is proposed.
Outcome (job 10978179, node d0031; RESULTS.md S5parent): six predictions hit and two missed, both
at 16 threads; by the rule above parent snapshots stay. none / previous: same 8.76 at 1 thread and
2.45 at 4, mod1pct 6.64 and 2.19 (hit), but at 16 threads 0.78 and 0.82 (miss: predicted at least
1.2 and 1.1). Files reused: all 84,905 for same, and 84,056 = 84,905 - 849 modified for mod1pct
(hit), none without the parent; every second snapshot's root is the same with and without it (hit).
The baseline was wrong: reading the identical tree back from the page cache took only 0.21 s at 16
threads (6.7 GB/s), not the 0.7 s E3's dedup-heavy backup of a different version suggested, and at
that speed both ways are bounded by the one walker listing 84,905 files (walk 0.18 s without a
parent). Why the parent is slower there, from the backup's own timings: the reuse work, summed over
the workers, was 0.15 s at 1 thread, 0.32 at 4, and 1.27 at 16, 8.6 times more time for the same
files, so the workers wait on each other; and the walker took 0.23 s with the parent at 16 threads
against 0.18 at 1. Every reused file takes two locks every worker and the walker share: the epoch
mutex, twice, for its read guard, and the container set's mutex for the cached file handle of each
recipe it reads. That is consistent with the numbers, not shown by a profile. With 1 to 4 threads
the backup with a parent takes about the walker's time (0.21 s), against 0.52 to 1.85 s without.

**S5parent2, backups with a parent after one read guard per batch (prediction committed
2026-10-10, before it ran).** S5parent found the backup with a parent slower at 16 threads, with
the reuse work summed over the workers 8.6 times its 1-thread total. Commit 64c596a holds one read
guard per batch of up to 64 files instead of one per reused file, so the epoch mutex is taken
64 times less often; on the laptop that halved the summed reuse time at 12 threads (exploratory).
`bench/slurm/s5_parent2.sbatch` repeats S5parent's runs (s5_common.sh). At 16 threads both kinds of
backup are bounded by the walker listing 84,905 files (0.18 s), so the best this change can do there
is draw level. Expected:
- At 16 threads, none / previous at least 0.95 in both workloads, and the second backup's summed
  reuse time at most half of S5parent's (1.27 s for same).
- At 1 and 4 threads, none / previous at least 6 and 2 for same, and at least 5 and 1.8 for mod1pct.
- Files reused and the second snapshots' roots exactly as in S5parent.

What follows from the outcome: if the 16-thread ratio stays below 0.95, the container set's
handle-cache mutex, taken for every recipe read, is the next suspect, and the outcome says so.
Outcome (job 10982725, node d0031; RESULTS.md S5parent2): two predictions hit and two missed. At 1
and 4 threads, none / previous: same 9.06 and 2.60, mod1pct 7.08 and 2.31 (hit); files reused and
the second snapshots' roots exactly as in S5parent (hit). At 16 threads the ratio rose from 0.78 to
0.85 for same and from 0.82 to 0.91 for mod1pct, below the predicted 0.95 (miss), and the summed
reuse time fell from 1.27 s to 0.90 s, 29% and not the predicted half (miss); the walker took 0.215
s with the parent against 0.184 without. The guard's mutex was part of the waiting, not all of it.
By the rule above, the next suspect is the container set's mutex, which every recipe read takes to
find its cached file handle; it is left as a known limitation (D106): the remaining loss is about 40
ms on 84,905 files, in the one case where reading every file back from the page cache costs almost
nothing.

**S3zstd, compression apart from dedup (prediction committed 2026-10-10, before it ran).**
Compression (D107) stores a blob as a zstd frame when that is smaller. `bench/slurm/s3_zstd.sbatch`
ingests three datasets at E1's settings (mem mode, 16 threads, 8 KiB average) with no compression
and with zstd levels 1, 3, and 9: the 11 kernel releases (source text), E1's synth (random content
with inserts and deletes), and the partial fine-tune's 6 checkpoints from E8 (model weights and
optimizer state). Then it restores linux-6.10 from a disk repository without compression and with
zstd:3 (E4's method). Each blob is compressed alone, 8 KiB at a time, so ratios fall below those of
compressing whole files; the laptop smoke test gave 3.8 to 3.9 on C headers. Expected:
- In each dataset the dedup ratio and unique content bytes are identical at every level, and
  without compression they equal E1's (kernel 5.25, synth 5.94) and E8's (partial fine-tune 2.06).
- Compression ratio (unique content / stored): kernel between 2.5 and 4.5 at zstd:3, with zstd:1 at
  most 10% below zstd:3 and zstd:9 at most 15% above it; synth exactly 1.000 at every level (zstd
  never shrinks random bytes, so every blob is stored as content); checkpoints between 1.0 and 1.3.
- Ingest MB/s relative to no compression: kernel at least 0.75 at zstd:1, at least 0.65 at zstd:3,
  and between 0.25 and 0.8 at zstd:9; synth and checkpoints at least 0.6 at zstd:1 (every failed
  attempt still costs a compression).
- Restore of linux-6.10 with zstd:3 between 0.6 and 1.1 times the speed without compression.

What follows from the outcome: a dedup ratio that moves with the compression level is a bug, to be
found before compression is kept. D107's revisit condition is zstd:1 slowing kernel ingest by more
than half.
Outcome (job 10982306, node d0031; RESULTS.md S3zstd): eight predictions hit and four missed;
compression stays, off by default. Dedup did not move: kernel 5.254, synth 5.941, and checkpoints
2.059 at every level, with identical unique content, and those equal E1's 5.25 and 5.94 and E8's
2.06 (hit). Compression ratio: kernel 3.43 at zstd:1, 3.55 at zstd:3, and 3.77 at zstd:9 (hit:
physical bytes 2818 MiB to 855 at zstd:3); checkpoints 1.09 (hit); synth 1.15 at every level (miss:
predicted exactly 1.000). The prediction misremembered synth: a third of its base version is
text-like words by design (D93; BENCHMARKS.md section 4), and that third compresses. Kernel ingest
fell further than predicted: 0.67 of no compression at zstd:1, 0.63 at zstd:3, and 0.22 at zstd:9
(miss: predicted at least 0.75, at least 0.65, and 0.25 to 0.8); synth (0.79 at zstd:1) and
checkpoints (0.69) met their floor of 0.6 (hit). A likely reason, not profiled: each 8 KiB blob is
compressed alone, and zstd's per-call setup is a large share of so small an input, so it costs more
per byte than its speed on whole files suggested. Restore of linux-6.10: 368 MB/s with zstd:3
against 588 without, 0.63 (hit, near the low end of 0.6 to 1.1): decompression is on restore's one
thread. D107's revisit condition (zstd:1 costing more than half of kernel ingest) is not met.

**S4prune, replication with and without subtree pruning (prediction committed 2026-10-11, before
it ran).** Subtree pruning (D108) lets the sender skip subtrees the receiver holds whole, asking
HAVE_TREES about every tree in one pass. `bench/slurm/s4_prune.sbatch` replicates a base over an
unlimited link and then measures the next version's transfer with pruning off and on, at W = 8, over
four links (one way 0.5 ms at 1000 Mbit/s, and 40 ms at 1000, 100, and 10 Mbit/s). "small":
linux-6.10, then a copy with one byte appended to the first 5 files of `fs/ext4`,
`Documentation/admin-guide`, and `drivers/net/ethernet/intel/e1000e`. "big": linux-6.0, then 6.1
(E7's pair), at 0.5 ms and 1000 Mbit/s and at 40 ms and 100 Mbit/s. The estimates: E7's 6.1 transfer
sent 8.15 MB of HAVE (about 254,000 fingerprints), and 6.10 has 84,905 files; pruning asks about
each tree (32 bytes, a few thousand trees) plus the blobs of the ten directories on the changed
paths. Without pruning, the HAVE traffic at 100 Mbit/s takes about 0.7 s, and at 1000 Mbit/s and 40
ms the window (8 batches of 128 KiB per 80 ms round trip) holds it to about the same; with it, the
transfer is about five round trips plus the receiver's subtree checks. The laptop smoke test (SDK
framework headers, not a reported number) gave 2.0 times as fast at 10 Mbit/s and 1.0 at 40 ms with
unlimited bandwidth. Expected:
- PUT payload bytes identical with and without pruning in every cell of both workloads.
- small: query bytes (HAVE plus HAVE_TREES) without pruning between 7 and 12 MB; with pruning at
  most a tenth of that. Trees asked between 4,000 and 8,000; subtrees pruned between 200 and 1,000.
- small, next-version seconds without / with pruning: at least 3 at 10 Mbit/s, at least 1.2 at 100
  Mbit/s and at 1000 Mbit/s with 40 ms, and at least 1.0 at 0.5 ms (pruning does less work: the
  sender reads no recipe below a pruned tree and the receiver answers no HAVE for those blobs).
- big: query bytes with pruning below those without, and without / with pruning at least 0.95 in
  both cells (most of 6.1's directories change, so most of the walk remains).

What follows from the outcome: pruning slower than not pruning on any link meets D108's revisit
condition, and I will look at where the time goes before keeping it on by default.
Outcome (job 10999082, node d0031, commit 61d9cac; RESULTS.md S4prune): ten predictions hit and one
missed. The PUT payload was identical with and without pruning in every cell (hit). small: 8,626
KiB of queries without pruning (8.83 MB, hit), 218 KiB with it, a 39.6th (hit); 5,532 trees asked
and 468 subtrees pruned (hit). Without / with pruning: 9.78 at 10 Mbit/s (7.585 s against 0.776),
2.33 at 100 Mbit/s, 2.24 at 1000 Mbit/s with 40 ms, and 1.75 at 0.5 ms (0.270 s against 0.154)
(all hit). big: 0.98 at 0.5 ms and 0.99 at 40 ms and 100 Mbit/s (hit), but pruning sent more query
bytes, not fewer: 8,114 KiB against 7,955 (miss). It pruned nothing: no subtree of 6.1 was whole on
the receiver, because a kernel release tarball gives every file the release's time (every entry
listed in linux-6.0.tar.xz reads 2022-10-02 21:09, in 6.1 2022-12-11 22:15), so every tree differs
between releases even where no content does. Pruning then only adds its pass: 162,750 bytes of
HAVE_TREES for 5,085 trees and one round trip, with the same 254,517 fingerprints queried; at 40 ms
and 100 Mbit/s that is 0.135 s of 15.32 (13 ms of bytes, an 80 ms round trip, and reading every
tree), and 0.036 s of 1.66 at 0.5 ms. The small workload copied 6.10 with `cp -a`, which keeps
mtimes, as a real tree's unchanged directories do between backups. D108's revisit condition is met
by the big cells; D108 records why pruning stays on by default.

**S4prune2, pruned replication before and after the review's fixes (prediction committed
2026-10-12, before it ran).** After S4prune, a review found three problems in D108's first version,
fixed in 1c47cb1 (a HAVE_TREES check fails on an unsealed chunk), 62cb16d (only trees held here,
or named by them, are remembered), and 4df3ab9 (each HAVE_TREES batch goes out as soon as its trees
are found). `bench/slurm/s4_prune2.sbatch` repeats S4prune's pruned transfers, both workloads and
all of its links, with S4prune's build (old, 61d9cac) and the reviewed one (new) on one node, in the
order old, new, new, old. None of the fixes changes a message: the batches are the same 4096 trees
in the same order, only sent earlier. The memo fix adds one index lookup per tree answered not
whole (5,085 for big, a few for small), and streaming can only overlap the first batch's round trip
with reading the rest of the trees (about 1,400 more for small and 1,000 for big, some milliseconds).
Time estimate: 24 measurements, about 30 minutes with the builds. Expected:
- Query bytes (HAVE plus HAVE_TREES), PUT payload bytes, trees asked, and subtrees pruned identical
  for old and new in every cell: small 218 KiB of queries, big 8,114 KiB, as S4prune measured.
- next-version seconds, new / old: between 0.95 and 1.05 in every small cell, and between 0.97 and
  1.03 in both big cells.

What follows from the outcome: if every cell holds, S4prune's numbers stand for the reviewed code
and the documents say so; a cell outside its range is reported as a miss and explained before
anything quotes S4prune for the current code.
Outcome (job 11002706, node d0031, commits 61d9cac and ac167ff; RESULTS.md S4prune2): the byte
counts hit and five of six timing cells hit. Query bytes, PUT payload bytes, trees asked, and
subtrees pruned were identical in all 24 runs and equal to S4prune's: small 223,085 bytes of queries
(218 KiB), 5,532 trees, 468 pruned; big 8,308,239 bytes (8,114 KiB), 5,085 trees, none pruned (hit).
New / old: small 0.987, 0.988, and 0.989 at 40 ms one way (hit), big 0.987 and 1.000 (hit), and
small 0.944 at 0.5 ms (0.145 s against 0.1535; miss, faster than the predicted 0.95). A likely
reason, not profiled: streaming lets the receiver start its subtree checks on the first batch, the
expensive part of a pruned transfer on a fast link, while the sender still reads the remaining
1,436 trees; the prediction counted only the overlap with the round trip. S4prune's numbers
therefore stand for the reviewed code, which is no slower anywhere measured.

**S6split, large files split across workers (prediction committed 2026-10-11, before it ran).**
Splitting (D109) has one worker read and chunk a large file while idle workers fingerprint and store
runs of its chunks. `bench/slurm/s6_split.sbatch` ingests in memory at 8 KiB: one 8 GiB random file,
and the full fine-tune's 6 checkpoints from E8 (a 655 MB optimizer file and a 328 MB model file in
each), with splitting off and at the default 64 MiB, at 4 and 16 threads in the order off, on, on,
off; and the 8 GiB file at 1 thread. Then linux-6.10 at 16 threads with the commit before splitting
and this one, and restores of the 8 GiB file from disk repositories backed up with and without
splitting. The estimates: one worker stores a large file at about 0.45 to 0.55 GB/s (E8 measured
these checkpoints at 679 MB/s with 16 threads, each version gated by its optimizer file on one
worker); a split file is held to what its one reader can read and chunk, about 1.6 GB/s (FastCDC
3.3 GB/s, plus the read and the copy into segments), while 3 or 15 other workers can fingerprint
and store more than that. The laptop smoke test (512 MiB file, not a reported number) gave 2.1
times as fast at 4 threads and 2.5 at 8, with every second snapshot's root unchanged, and no
difference with eight 128 MiB files on 8 threads. Expected:
- Every snapshot's root the same with and without splitting; files split: 1 for the 8 GiB file,
  2 per checkpoint, none for linux-6.10.
- 8 GiB file: 1 thread between 400 and 700 MB/s; off at 4 and 16 threads within 15% of 1 thread;
  on / off at least 1.8 at 4 threads and at least 2.0 at 16; on at 16 / on at 4 between 1.0 and
  1.6 (the reader limits both).
- Checkpoints: off at 16 threads between 580 and 780 MB/s (E8's 679); on / off at least 1.6 at 4
  threads and at least 2.0 at 16.
- linux-6.10 at 16 threads: new / old between 0.97 and 1.03 (no file reaches 64 MiB).
- Restore of the 8 GiB file: split / unsplit between 0.85 and 1.15.

What follows from the outcome: a split file held well below the many-files rate by its reader meets
D109's revisit condition; restores of split files more than 15% slower meet the other.
Outcome (job 10999396, node d0031, commit d78a548; RESULTS.md S6split): seven predictions hit and
five missed. Every root was the same with and without splitting, and 1 file was split for the
8 GiB file, 12 for the checkpoints, none for linux-6.10 (hit). The 8 GiB file: 635 MB/s at 1
thread (hit); off at 4 threads 631 and 629, at 16 629 and 587, within 15% of 1 thread (hit); on /
off 1.53 at 4 threads (967 and 957 MB/s; miss, predicted at least 1.8) and 1.88 at 16 (1,138 and
1,149; miss, predicted at least 2.0); on at 16 / on at 4 1.19 (hit). Checkpoints: off at 16 threads
905 and 888 MB/s (miss: predicted 580 to 780 from E8's 679, which was measured at 620f2a6 on node
d0020, before hardware CRC32C); on / off 1.31 at 4 threads and 1.85 at 16 (miss, predicted at least
1.6 and 2.0). linux-6.10 at 16 threads: new / old 0.977 (old 4,295 and 4,292 MB/s, new 4,205 and
4,183; hit, near the low end, and lower in both passes; not profiled). Restore of the 8 GiB file:
split / unsplit 0.956 (1,471 against 1,538 MB/s; hit). Where the speedup went: at 16 threads the
splitter's reading and chunking alone took 5.49 s of the 7.55 s run (5.39 s at 1 thread, 1.59 GB/s),
so the prediction's reader at about 1.6 GB/s left no room for anything else on that thread. The
estimate took FastCDC's 3.3 GB/s and a cheap copy; E3 had already measured reading and chunking at
1.68 GB/s. The rest of the splitter's time is copying runs into segments: each segment's vector
grows by doubling from empty, so a run of about 1 MiB is copied about three times into a fresh
2 MiB buffer. Fingerprinting and storing, summed over the workers, rose from 8.1 s at 1 thread to
29.7 s at 16 (pass a) for the same bytes, likely because each chunk is read cold from another
core's copy and 16 threads share memory bandwidth (not profiled), but they were off the critical
path. D109's first revisit condition is met: a
split file runs at about 1.14 GB/s, against 4.2 GB/s for linux-6.10 on the same node. S6split2
measures removing the reallocation.

**S6split2, segment buffers reserved once (prediction committed 2026-10-12, before it ran).**
S6split found the splitter's thread the limit: 5.49 s of reading and chunking in a 7.55 s run, the
rest mostly copying runs into segments whose vectors grew from empty. df41430 reserves each
segment's buffer once. `bench/slurm/s6_split2.sbatch` runs splitting as S6split measured it (old,
d78a548) and the new build on one node, 16 threads in memory, in the order old, new, new, old: the 8
GiB file and the full fine-tune's checkpoints with splitting on, and the 8 GiB file with it off as a
control. If the growth cost about 1 s of the splitter's 2.06 s outside reading and chunking, a split
file gains about 15%; the laptop smoke test showed no gain (1,964 and 1,967 MB/s against 2,026
before, on macOS's allocator, not a reported number), so the estimate is uncertain. Expected:
- The same roots in every run of each configuration.
- 8 GiB file, splitting on: new / old between 1.03 and 1.25.
- Checkpoints, splitting on: new / old between 1.02 and 1.25.
- 8 GiB file, splitting off: new / old between 0.97 and 1.03 (the change is only in the split path).

What follows from the outcome: a new / old below 1.03 for the 8 GiB file means the reallocation was
not where the splitter's other 2 s went, and D109 records that it stays unexplained.
Outcome (job 11002248, node d0029, commits d78a548 and 8806bcf; RESULTS.md S6split2): every
prediction hit. The same roots in every run (hit). 8 GiB file, splitting on: new / old 1.123 (old
1,144 and 1,169 MB/s, new 1,308 and 1,290; hit). Checkpoints, splitting on: 1.136 (old 1,575 and
1,601, new 1,839 and 1,769; hit). 8 GiB file, splitting off: 0.974 (old 555 and 550, new 577 and
500; hit, at the edge, with a spread of 485 to 598 MB/s within runs of a path the change does not
touch). Where it came from: the 8 GiB runs took 7.51 and 7.35 s before and 6.57 and 6.66 s after,
while reading and chunking stayed between 4.97 and 5.49 s, so the time the splitter's thread spent
outside them fell from about 2.1 to 2.4 s to about 1.2 to 1.4 s. The laptop smoke test had shown no
gain; macOS's allocator is not glibc's. With the reservation, the split 8 GiB file runs at 2.41
times the unsplit one in this job (1,299 against 539 MB/s), and still at well under the
many-files rate: D109's first revisit condition stays met.

### E1 — Content-defined vs fixed chunking
- Question: how much of FastCDC's dedup advantage over fixed-size chunking survives on real data?
- Setup: four repositories, {`kernel`, `synth`} × {fastcdc, fixed}, same average size (8 KiB).
  Back up the dataset's versions in order into each.
- Metrics: dedup ratio after each version; chunk size histogram; new unique bytes per version,
  split by blob type (data, tree, recipe), which also checks D14's metadata-share assumption.
- Prediction (committed 2026-10-09, before any measured run): kernel, FastCDC: dedup ratio after
  all 11 versions 3 to 6 (most files are unchanged from one release to the next). kernel, fixed:
  lower, but by only 10% to 50% relative to FastCDC, because kernel files are small (about 17 KB on
  average) and fixed-size chunks restart at every file. synth, FastCDC: 4 to 7 (each version adds
  roughly 8% to 10% new bytes at a 1% edit rate; a laptop smoke run of the generator saw 7.7% and
  9.7%). synth, fixed: 1.0 to 1.3, since every insert or delete shifts the rest of its file. Tree
  and recipe bytes are at most 3% of the new bytes in every configuration (D14's assumption).
- Outcome (rerun job 10949339, commit 620f2a6; every count identical in the first run, job
  10930690; RESULTS.md E1): kernel, FastCDC 5.25 (hit). kernel, fixed 3.82, 27% below FastCDC
  (hit). synth, FastCDC 5.94 (hit), each version adding 8.3% to 8.9% new data. synth, fixed 1.03
  (hit). Tree and recipe bytes over all versions: 3.23%, 2.44%, 2.24%, and 0.45% of the new bytes
  (miss for kernel with FastCDC, by 0.23 points). Per version after the first they are larger:
  kernel with FastCDC 4.3% to 5.2%, mostly tree blobs (3.4% to 4.1%) rewritten along the paths of
  changed files; synth with FastCDC 4.1% to 4.5%, almost all recipes, because every synth version
  edits every file and so rewrites every recipe piece. D14's revisit condition was checked (see
  D14).

### E2 — Chunk size trade-off
- Question: how do dedup ratio, index memory, and ingest throughput trade off as the average chunk
  size changes?
- Setup: `kernel`, FastCDC, average 2, 4, 8, 16, 64 KiB and 1 MiB (min = avg / 4, max = avg × 8).
- Metrics: dedup ratio, index bytes per blob and in total, ingest throughput (`mem` mode).
- Prediction (committed 2026-10-09): dedup ratio non-increasing as the average grows; 2 KiB within
  10% of 8 KiB, and 1 MiB at least 15% below 8 KiB (sub-file dedup is lost for files above 256 KiB,
  file-level dedup remains). Blobs roughly inverse to the average for large averages. Index bytes
  per blob 85 to 175 (64-byte entries in a table between 0.375 and 0.75 full). Ingest MB/s rises by
  at least 1.5x from 2 KiB to 64 KiB (fewer per-chunk lookups, recipes, and index inserts).
- Outcome (rerun job 10949358, commit 620f2a6; dedup counts identical in the first run, job
  10930691; RESULTS.md E2): dedup ratio falls from 7.20 at 2 KiB to 3.25 at 1 MiB (hit:
  non-increasing). 2 KiB is 37% above 8 KiB (miss: predicted within 10%; small chunks find far
  more sub-file sharing between releases than predicted). 1 MiB is 38% below 8 KiB (hit). Blobs do
  not fall inversely at large averages: 479,646 at 64 KiB and 465,215 at 1 MiB (miss: the average
  kernel file, 17 KB, is below even 64 KiB's 16 KiB minimum, so most files are one chunk at every
  large average). Index bytes per blob 103 to 160 (hit). Ingest rises 1.81x from 2 KiB to 64 KiB
  (hit; 2.09x in the first run). The first run's 1 MiB ingest, 93 MB/s, exposed the per-file
  buffer zero-fill (D98); the rerun gives 2469.

### E3 — Ingest throughput and scaling
- Question: where does ingest time go, and how does it scale with threads?
- Setup: `kernel` 6.10 then 6.9 (the second backup is dedup-heavy). Threads 1, 2, 4, 8, 16, 32.
  Both storage modes.
- Metrics: MB/s of logical input; per-component time from counters; a `perf record` profile of the
  8-thread run (top functions, share of time in locks). Not
  verified: whether `perf` is usable by users on Explorer compute nodes. If it is not, report the
  counters only and say so.
- Prediction (committed 2026-10-09; informed by the exploratory parallel-ingest run (D80), which
  used disk mode on a different node type): disk mode, first backup (6.10): 1 thread 100 to 200
  MB/s, 16 threads 250 to 450 MB/s, flattening beyond about 4 threads; `store` (which includes
  sealing, so fsync) is the largest component. mem mode: at least 3x disk mode at 8 threads, and at
  least 4x its own 1-thread rate at 8 threads. The dedup-heavy 6.9 backup is at least 2x faster than
  6.10 at the same thread count in disk mode (almost nothing to store). In the 8-thread profile,
  BLAKE3 compression and FastCDC's boundary loop are the top two user-space symbols; lock functions
  are under 10%.
- Outcome (rerun job 10949418, commit 620f2a6; RESULTS.md E3): disk mode, 6.10: 144 MB/s at 1
  thread and 313 at 16 (hit on both), flattening (243 at 4, 282 at 8, 304 at 32) (hit); `store`
  is the largest component (6.2 s against 1.6 for reading and chunking and 1.5 for hashing at 1
  thread) (hit). mem mode at 8 threads is 5.4x disk mode (hit) but only 3.7x its own 1-thread rate
  (miss); it peaks at 16 threads (1601 MB/s) and falls at 32 (1378), while time summed in `store`
  grows from 1.4 s at 1 thread to 26.2 s at 32: workers wait inside the store path, which was not
  profiled further. 6.9 is 3.3x to 5.4x faster than 6.10 in disk mode at every thread count (hit).
  Profile (8 threads, mem mode, job 10949418's `perf.txt`): FastCDC's
  boundary loop 23.3% and BLAKE3 compression 17.2% are the top two symbols, then CRC32C 15.1%;
  `pthread_mutex_lock` and unlock 4.2% (hit on both). The first run's profile (job 10930705) had
  `memset` second at 14.5%: the per-file zero-fill (D98), absent after the fix.

### E4 — Restore throughput and fragmentation
- Question: does dedup slow restores of later versions, whose chunks are scattered across older
  containers?
- Setup: restore 6.0 and 6.10 from the repository holding all 11 versions; also restore 6.10 from a
  repository holding only 6.10. With verification (the default) and with `--no-verify`.
- Metrics: restore MB/s; containers touched per restored GB.
- Prediction (committed 2026-10-09): containers per GB: 6.10 from the all-versions repository is at
  least 1.5x 6.10 alone, and 6.0 from the all-versions repository is the lowest of the three (it
  was written first, contiguously). Because the repository is in the page cache (warm), restore
  throughput differs by at most 25% between the fragmented and unfragmented cases. Verification
  (BLAKE3) costs at most 25% of throughput; restores are dominated by creating about 85,000 files.
- Outcome (job 10930713, commit 8dd74df; RESULTS.md E4): containers per GB: 6.10 from the
  all-versions repository 1028, 4.0x the 255 of 6.10 alone (hit). 6.0 from the all-versions
  repository is 272, not the lowest of the three: 6.10 alone is lower (miss, by 6%; both were
  written contiguously, so the prediction's reasoning did not separate them). Fragmentation costs
  8% of restore throughput with verification (346 against 376 MB/s) and 8% without (454 against
  491), within the 25% predicted (hit). Verification costs 22% to 24% of throughput in all three
  cases, just inside the 25% predicted (hit). Not measured: the share of time spent creating files.

### E5 — Garbage collection: space vs write amplification
- Question: how does the compaction threshold trade reclaimed space against bytes copied?
- Setup: back up all `kernel` versions, `rm` every other one, run GC at thresholds 0.1, 0.3, 0.5,
  0.7, 0.9 (each from a copy of the same starting repository). Repeat the whole procedure at
  threshold 0.5 with container sizes 1, 4, and 16 MiB (checks D9).
- Metrics: physical bytes before and after; bytes copied; write amplification = bytes copied /
  bytes reclaimed; GC duration by phase.
- Prediction (committed 2026-10-09): write amplification (bytes copied / bytes reclaimed) is
  non-decreasing in the threshold; at 0.1 it is below 0.2 (almost only whole-container deletions),
  at 0.9 between 0.5 and 4. Physical bytes after GC are non-increasing in the threshold. At 0.5,
  smaller containers copy less: write amplification 1 MiB <= 4 MiB <= 16 MiB.
- Outcome (job 10930720, commit 8dd74df; RESULTS.md E5): the job's own write-amplification
  field divided by every byte unlinked, including the live blobs copied out of those containers;
  RESULTS.md recomputes it from the raw counters as copied / freed, as defined above (fixed in the
  driver by commit 59ce8b5). At 4 MiB it rises with the threshold: 0, 0, 0.17, 1.42, 1.58 at 0.1 to
  0.9 (hit: non-decreasing, below 0.2 at 0.1, between 0.5 and 4 at 0.9), and physical bytes after
  GC fall from 2783 to 2496 MiB (hit). At 0.5, container size: 1 MiB 0.37, 4 MiB 0.17, 16 MiB 0.23
  (miss: 1 MiB copies the most). At 1 MiB, 174 containers fell below half live against 10 at
  4 MiB: small containers vary more in their live fraction, so more qualify for compaction, and
  1 MiB also freed the most (65 MiB against 40). The surprise is how little removing 5 of 11
  releases frees: 32 MiB (1.1%) by whole-container deletion, at most 319 MiB (11%) at threshold
  0.9, because almost every chunk of a removed release is also in a kept neighbor. Mark takes about
  0.5 s in every run; compaction dominates at 0.7 and 0.9 (2.9 and 3.8 s).

### E6 — GC interference
- Question: how much does a concurrent GC slow ingest, and how many duplicate copies do concurrent
  sessions create?
- Setup: back up `synth` versions while GC runs in a loop on the same repository, vs without GC.
  Then run 4 concurrent sessions backing up overlapping `synth` versions.
- Metrics: ingest MB/s with and without GC; GC throughput while ingest runs; duplicate copies as a
  share of physical bytes (checks D11 and D32).
- Prediction (committed 2026-10-09): a GC loop on the same repository lowers ingest MB/s by 10%
  to 40% (it competes for cores and shard locks). The 4 concurrent sessions start on versions 0 to
  3 at once, which share about 90% of their bytes, so all four store most of the base: duplicate
  copies are 20% to 60% of physical bytes. That is a deliberately adversarial overlap, far above
  D32's 1% "realistic workload" threshold, and says nothing about realistic workloads by itself.
- Outcome (rerun job 10949428, commit 620f2a6; RESULTS.md E6): the GC loop lowers ingest by 1.1%
  (4567 against 4620 MB/s; 3.2% in the first run, job 10930728) (miss: predicted 10% to 40%). It
  was busy throughout: once the first snapshot existed, GC marked 1.6 million blobs per second of
  ingest (29 to 32 marking runs per sample; the other 1450 to 2370 runs found no snapshot yet, so
  all fell within the first version's backup and took under a millisecond each). One GC thread on a 56-core node beside 8 ingest threads competes for
  neither cores nor, measurably, shard locks; a node with fewer cores would show more. Four
  concurrent sessions: duplicate copies 51.6% of physical bytes (55.9% in the first run) (hit);
  the share depends on how the sessions interleave, varying from 50.9% to 56.4% between samples.

### E7 — Replication over a WAN
- Question: does pipelining hide latency, and how close do transfers come to the bandwidth limit?
- Dataset: a 256 MiB `synth` base and one version with 5% edits, so one transfer at 10 Mbit/s
  takes minutes, not hours. The `kernel` pair 6.0 → 6.1 is run only at 1000 Mbit/s as a realism
  check.
- Setup: replicate the base, then the edited version, over `SimulatedLink`. Two grids:
  - window sweep: `W` = 1, 2, 4, 8, 16, 32 at 100 Mbit/s and 80 ms RTT;
  - link grid: one-way latency 0.5, 10, 40, and 100 ms (RTT 1, 20, 80, 200 ms) × bandwidth 10, 100,
    and 1000 Mbit/s, at `W` = 8 and `W` = 32.
- Before running, estimate the total link time of all cells and runs (bytes / bandwidth + round
  trips × RTT, × 6 runs) and record it. If it exceeds 12 hours, drop the 10 Mbit/s cells at
  `W` = 32 first, and record that.
- Metrics: bytes on the wire by message type; bytes on the wire / logical bytes; wall time;
  achieved bandwidth vs link bandwidth; the smallest `W` that reaches 90% of bandwidth, compared
  with the bandwidth-delay product estimate.
- Time estimate (recorded 2026-10-09, before running): per run, the base is about 257 MiB on the
  wire (data plus 32 bytes of HAVE per chunk) and the 5%-edit version about 90 MiB, about 2.9 Gbit
  together. 6 runs per cell (one warm-up). Window sweep (6 cells at 100 Mbit/s): about 30 minutes.
  Link grid: the 8 cells at 10 Mbit/s about 4 hours (about 290 s per run), the 8 at 100 Mbit/s about
  25 minutes, the 8 at 1000 Mbit/s a few minutes; the kernel realism check a few minutes. Total
  about 5 hours, under the 12-hour limit, so no cells are dropped.
- Prediction (committed 2026-10-09): window sweep at 100 Mbit/s and 80 ms RTT (bandwidth-delay
  product about 1 MB, about one PUT): W = 1 reaches 40% to 60% of the link, and the smallest W
  reaching 90% is 2 (the estimate from the bandwidth-delay product). Link grid at W = 8: at least
  85% utilization in every cell whose bandwidth-delay product is under 8 MiB; at 1000 Mbit/s and
  100 ms one-way (25 MB in flight needed) at most 40% at W = 8 and at least 70% at W = 32. The
  edited version's wire bytes are 0.3 to 0.5 of its logical bytes.
- Outcome (job 10930737, commit 8dd74df; RESULTS.md E7): window sweep: W = 1 reaches 47% of the
  link and W = 2 reaches 92%, so the smallest W reaching 90% is 2 (hit on both). Link grid at
  W = 8: every cell whose bandwidth-delay product is under 8 MiB reaches at least 95% (hit); at
  1000 Mbit/s and 100 ms one-way, 27% at W = 8 and 72% at W = 32 (hit on both, the second by 2
  points); at 1000 Mbit/s and 40 ms (10 MB in flight needed) W = 8 reaches 60%. The edited
  version's wire bytes are 0.365 of its logical bytes in every cell (hit). The kernel 6.0 to 6.1
  check sends 0.143 of 6.1's logical bytes at 97% of 1000 Mbit/s. The job took 4 h 49 min, against
  the 5 h estimated.

### E8 — Checkpoint dedup study
- Question: what fraction of successive training checkpoints is shared, and how does that depend
  on the training method?
- Setup: back up `ckpt` checkpoint directories in step order, one repository per run, plus one
  repository holding several seeds of the same recipe.
- Metrics: dedup ratio per method; new unique bytes per checkpoint; dedup by file (weights,
  optimizer state, configs, tokenizer).
- Dataset (D97): checkpoints are produced for this study by fine-tuning
  distilgpt2 on Explorer (one GPU; checkpoints in /scratch): full fine-tune with 3 seeds, a partial fine-tune of
  the top 2 of 6 transformer blocks, and LoRA, each saving 6 checkpoints in the file layout of
  Hugging Face's Trainer.
- Prediction (committed 2026-10-09): full fine-tune: successive checkpoints share almost nothing,
  since AdamW changes every weight and every optimizer moment at every step, so every 8 KiB chunk of
  the weights and optimizer files differs: dedup ratio over the 6 checkpoints 1.0 to 1.1, the
  shared bytes being configs and tokenizer files. Partial fine-tune: the frozen embeddings and
  blocks are identical in every checkpoint, so the weights file dedups by about the frozen fraction
  (about 80%) while the optimizer file (trainable parameters only) does not: ratio 1.6 to 2.6.
  LoRA: adapter weights change every step: 1.0 to 1.2 (the checkpoints are tiny anyway). Across 3
  seeds of the full fine-tune: 1.0 to 1.1.
- Outcome (job 10949230 on checkpoints from job 10949197, commit 620f2a6; RESULTS.md E8): full
  fine-tune 1.008 for each seed (hit): in checkpoints 2 to 6, model.safetensors is 100% new and
  optimizer.pt 99.3% new, and the configs and tokenizer files are shared. Partial fine-tune 2.06
  (hit): model.safetensors is 17.3% new, so 82.7% is shared, about the frozen fraction (hit), and
  optimizer.pt is 100% new (hit). LoRA 2.19 (miss: predicted 1.0 to 1.2): each 4.9 MiB checkpoint
  carries 3.2 MiB of tokenizer files that never change, and only the 1.7 MiB of adapter, optimizer,
  and training state is new; the prediction treated the adapter as the whole checkpoint. Three
  seeds of the full fine-tune 1.010 (hit). Not investigated: which 0.7% of optimizer.pt repeats.
  Checkpoints: /scratch/$USER/dedupstore-data/ckpt/<run>/checkpoint-<50..300>.

### E9 — Open time and index memory
- Question: how long does opening a repository take as it grows, and is the measured index memory
  what ARCHITECTURE.md section 8 predicts?
- Setup: open repositories from E1 and E2, plus `synth`-generated repositories of 10, 50, and
  100 GiB of unique data (or the largest that fits the `/scratch` quota, recorded), 5 times each.
  Fit open time against container count and extrapolate to 500 GB of unique data, stating the fit.
  This is the measurement D10's revisit condition refers to.
- Metrics: open time (footer read + index build), containers, blobs, index bytes per blob. Index
  lookup cost comes from the `FlatTable` and `ChunkIndex` microbenchmarks (section 6), which D18's
  revisit condition refers to.
- Prediction (committed 2026-10-09): open time grows linearly with the number of containers
  (footers read with up to 16 threads, then index inserts): 10 GiB (about 2,560 containers) opens in
  0.05 to 1 s, 100 GiB in 0.5 to 10 s; index bytes per blob 85 to 175, matching ARCHITECTURE.md
  section 8's 64-byte entries; the linear fit puts 500 GB of unique data under 60 s.
- Outcome (job 10930747, commit 8dd74df; RESULTS.md E9): 10 GiB (2581 containers) opens in
  0.079 s and 100 GiB (25801) in 1.01 s (hit on both); index bytes per blob 93 to 117 (hit); the
  linear fit puts 500 GB at 4.5 s (hit). Growth is close to linear but not exactly: 31 us per
  container at 10 GiB, 39 us at 100 GiB. And blobs matter, not only containers: kernel at 2 KiB
  (649 containers, 1.18M blobs) opens slower than kernel at 8 KiB (793 containers, 0.65M blobs),
  0.075 against 0.064 s, so a fit over containers alone is a simplification.

## 6. Microbenchmarks (`bench/micro/`, Google Benchmark)

Chunker GB/s (FastCDC and fixed) per average size; BLAKE3 GB/s by buffer size (1 KiB to 1 MiB);
CRC32C GB/s; `FlatTable` lookup and insert ns/op at load factors 0.4, 0.6, and 0.75; `ChunkIndex`
`LookupAndStamp` throughput with 1 to 32 threads. Reported in RESULTS.md under the same rules, with
the Google Benchmark JSON output converted to the result schema.

Prediction (committed 2026-10-09): BLAKE3 at least 1 GB/s on buffers of 64 KiB and more (the
cascadelake nodes have AVX-512); FastCDC at 8 KiB 0.7 to 2 GB/s; CRC32C (slicing-by-8, no hardware
instruction) 1 to 3 GB/s; `FlatTable` hits 40 to 150 ns at every load factor (a 64 MiB table misses
the caches), misses slower at 0.75 than at 0.4; `ChunkIndex::LookupAndStamp` at least 8x faster
with 16 threads than with 1 (256 shards).

Outcome (job 10930767, commit 8dd74df; RESULTS.md Microbenchmarks): BLAKE3 7.8 to 8.0 GB/s on
64 KiB and more (hit). FastCDC at 8 KiB 3.2 GB/s (miss: faster than the 0.7 to 2 predicted).
CRC32C 2.2 GB/s (hit). `FlatTable` hits 35, 55, and 66 ns at load 0.4, 0.6, and 0.75 (miss at
0.4, under the 40 predicted: successive lookups are independent, so the CPU overlaps their cache
misses; this is time per lookup in a stream, not the latency of one); misses 53 ns at 0.4 and
109 ns at 0.75 (hit). `ChunkIndex::LookupAndStamp` 9.0x faster with 16 threads than with 1
(hit). The first version of the index benchmarks measured cached misses and a load of 0.375 for
"0.75"; both were fixed before this run (commit 7ef0edf).
