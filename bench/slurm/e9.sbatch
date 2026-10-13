#!/bin/bash
#SBATCH --job-name=dds-e9
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:00:00
#
# E9 (BENCHMARKS.md 5): open time and index memory. Repositories of 10, 50, and 100 GiB of unique
# synthetic data (streamed, no input files), and the kernel repositories of E1 (8 KiB) and E2
# (2 KiB average), on node-local disk; each opened 5 times after a warm-up.
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
for gib in 10 50 100; do
  measure E9 "build-${gib}G" ingest --synthetic-files $gib --synthetic-file-size 1024M --storage disk \
    --repo "$WORK/e9-${gib}G" --threads 16 --keep 1 --runs 1 --warmup 0 --dataset "synthetic-random-${gib}GiB"
  measure E9 "open-${gib}G" open --repo "$WORK/e9-${gib}G" --label "synthetic-random-${gib}GiB"
  rm -rf "$WORK/e9-${gib}G"
done
KERNEL="6.0 6.1 6.2 6.3 6.4 6.5 6.6 6.7 6.8 6.9 6.10"
extract_kernel $KERNEL
for avg in 8K 2K; do
  measure E9 "build-kernel-$avg" ingest --versions "$(kernel_dirs $KERNEL)" --labels "${KERNEL// /,}" \
    --dataset kernel --storage disk --repo "$WORK/e9-kernel-$avg" --avg $avg --threads 16 --keep 1 --runs 1 --warmup 0
  measure E9 "open-kernel-$avg" open --repo "$WORK/e9-kernel-$avg" --label "kernel-all-avg-$avg"
  rm -rf "$WORK/e9-kernel-$avg"
done
echo "overall: $overall"
exit $overall
