#!/bin/bash
#SBATCH --job-name=dds-s1-open
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:30:00
#
# The index checkpoint (D104), experiment S1open: open time with and without it on E9's
# repositories (10, 50, and 100 GiB of unique synthetic data, and the kernel at 8 and 2 KiB average
# chunks), in the order off, on, on, off (passes a and b), then the time to write the checkpoint.
# The ingest that builds each repository closes it, which writes its checkpoint (each has more than
# 256 containers); no measured open writes one.
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
measure_repo() {  # measure_repo NAME LABEL: the opens and the checkpoint write, then remove it
  local name="$1" label="$2" repo="$WORK/s1-$1"
  for run in off-a on-a on-b off-b; do
    measure S1open "open-$name-$run" open --repo "$repo" --label "$label" --checkpoint "${run%-*}"
  done
  measure S1open "write-$name" checkpoint --repo "$repo" --label "$label"
  rm -rf "$repo"
}
for gib in 10 50 100; do
  measure S1open "build-${gib}G" ingest --synthetic-files $gib --synthetic-file-size 1024M --storage disk \
    --repo "$WORK/s1-${gib}G" --threads 16 --keep 1 --runs 1 --warmup 0 --dataset "synthetic-random-${gib}GiB"
  measure_repo "${gib}G" "synthetic-random-${gib}GiB"
done
KERNEL="6.0 6.1 6.2 6.3 6.4 6.5 6.6 6.7 6.8 6.9 6.10"
extract_kernel $KERNEL
for avg in 8K 2K; do
  measure S1open "build-kernel-$avg" ingest --versions "$(kernel_dirs $KERNEL)" --labels "${KERNEL// /,}" \
    --dataset kernel --storage disk --repo "$WORK/s1-kernel-$avg" --avg $avg --threads 16 --keep 1 --runs 1 --warmup 0
  measure_repo "kernel-$avg" "kernel-all-avg-$avg"
done
echo "overall: $overall"
exit $overall
