#!/bin/bash
#SBATCH --job-name=dds-e2-ab-lock
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:00:00
#
# Why the ChunkReader fix (D98) slowed E2's 64 KiB ingest (E2ab, job 10953149; BENCHMARKS.md
# section 5, "E2ab-lock"). Both builds, as in e2_ab.sbatch, at 64 KiB with 4 and 16 threads
# (experiment E2ab, tags <build>-avg-64K-t<threads>), then for each build at 16 threads one pass
# under `perf stat` (context switches, migrations) and one under `perf record` (profile).
# Needs commit 8dd74df's source uploaded next to the job's (src-8dd74df) before submission.
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
OLD_COMMIT=8dd74df
OLD_SRC="$WORK/src-$OLD_COMMIT"
OLD_BUILD="$WORK/build-$OLD_COMMIT"
cp -a "/scratch/$USER/dedupstore/src-$OLD_COMMIT" "$OLD_SRC" ||
  { echo "no src-$OLD_COMMIT"; exit 1; }
grep -q "^commit=$OLD_COMMIT" "$OLD_SRC/dds-build-info.txt" ||
  { echo "src-$OLD_COMMIT holds another commit"; exit 1; }
(cd "$OLD_SRC" && cmake --preset release -B "$OLD_BUILD" -DDDS_DEPS_MIRROR="$DDS_DEPS_MIRROR" \
                 -DDDS_BUILD_TESTS=OFF) > "$DDS_OUT/configure-old.log" 2>&1 &&
  cmake --build "$OLD_BUILD" -j "$SLURM_CPUS_ON_NODE" --target dds_bench \
    > "$DDS_OUT/build-old.log" 2>&1 || { echo "old build failed"; exit 1; }
NEW_BENCH="$BENCH"
OLD_BENCH="$OLD_BUILD/bench/dds_bench"
old_full="$(sed -n 's/^commit=//p' "$OLD_SRC/dds-build-info.txt")"
python3 - "$DDS_OUT/meta.json" "$DDS_OUT/meta-old.json" "$old_full" <<'PY'
import json, sys
meta = json.load(open(sys.argv[1]))
meta["git_commit"] = sys.argv[3]
json.dump(meta, open(sys.argv[2], "w"), indent=1)
PY
KERNEL="6.0 6.1 6.2 6.3 6.4 6.5 6.6 6.7 6.8 6.9 6.10"
extract_kernel $KERNEL
warm "$WORK/kernel"
INGEST=(ingest --versions "$(kernel_dirs $KERNEL)" --labels "${KERNEL// /,}" --dataset kernel
        --storage mem --chunker fastcdc --avg 64K)
pick() {  # pick BUILD: point BENCH and META at that build
  if [ "$1" = old ]; then
    BENCH="$OLD_BENCH" META="$DDS_OUT/meta-old.json"
  else
    BENCH="$NEW_BENCH" META=""
  fi
}
run() {  # run BUILD THREADS PASS
  pick "$1"
  measure E2ab "$1-avg-64K-t$2-$3" "${INGEST[@]}" --threads "$2"
}
for threads in 4 16; do
  run old $threads a; run new $threads a; run new $threads b; run old $threads b
done
# Not result files: perf output goes to the job's evidence directory.
echo "perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid)" > "$DDS_OUT/perf-lock.txt"
for build in old new; do
  pick $build
  echo "=== perf stat, $build, 16 threads" >> "$DDS_OUT/perf-lock.txt"
  perf stat -e task-clock,context-switches,cpu-migrations -- "$BENCH" "${INGEST[@]}" \
    --threads 16 --runs 1 --warmup 1 --experiment E2ab --out "$WORK/stat-$build.json" \
    >> "$DDS_OUT/perf-lock.txt" 2>&1 || echo "perf stat failed for $build" >> "$DDS_OUT/perf-lock.txt"
  echo "=== perf record, $build, 16 threads" >> "$DDS_OUT/perf-lock.txt"
  if perf record -F 499 -g -o "$WORK/perf-$build.data" -- "$BENCH" "${INGEST[@]}" \
       --threads 16 --runs 1 --warmup 0 --experiment E2ab --out "$WORK/rec-$build.json" \
       >> "$DDS_OUT/perf-lock.txt" 2>&1; then
    perf report -i "$WORK/perf-$build.data" --stdio --no-children --percent-limit 0.5 --sort symbol \
      >> "$DDS_OUT/perf-lock.txt" 2>&1
  else
    echo "perf record failed for $build" >> "$DDS_OUT/perf-lock.txt"
  fi
done
echo "overall: $overall"
exit $overall
