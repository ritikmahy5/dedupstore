#!/bin/bash
#SBATCH --job-name=dds-e3
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:00:00
#
# E3 (BENCHMARKS.md 5): ingest throughput and scaling. kernel 6.10, then 6.9 (dedup-heavy), threads
# 1 to 32, mem and disk (node-local) storage; and a perf profile of the 8-thread mem run.
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
extract_kernel 6.10 6.9
warm "$WORK/kernel"
DIRS="$(kernel_dirs 6.10 6.9)"
for storage in mem disk; do
  for threads in 1 2 4 8 16 32; do
    measure E3 "$storage-t$threads" ingest --versions "$DIRS" --labels 6.10,6.9 --dataset kernel \
      --storage $storage --repo "$WORK/e3-repo" --threads $threads
  done
done
# perf: usable by users if perf_event_paranoid <= 2 (user-space samples only). Not a result file.
echo "perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid)" > "$DDS_OUT/perf.txt"
if perf record -F 499 -g -o "$WORK/perf.data" -- "$BENCH" ingest --experiment E3 --out "$WORK/perf-run.json" \
     --versions "$DIRS" --labels 6.10,6.9 --dataset kernel --storage mem --threads 8 --runs 1 --warmup 0 \
     >> "$DDS_OUT/perf.txt" 2>&1; then
  perf report -i "$WORK/perf.data" --stdio --no-children --percent-limit 0.5 --sort symbol \
    >> "$DDS_OUT/perf.txt" 2>&1
else
  echo "perf record failed: profile not available" >> "$DDS_OUT/perf.txt"
fi
echo "overall: $overall"
exit $overall
