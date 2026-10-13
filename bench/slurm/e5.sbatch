#!/bin/bash
#SBATCH --job-name=dds-e5
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:00:00
#
# E5 (BENCHMARKS.md 5): GC space vs write amplification. Back up every kernel version, rm every other
# one (6.1, 6.3, 6.5, 6.7, 6.9), then GC at thresholds 0.1 to 0.9, each from a copy of the same
# starting repository; then threshold 0.5 with 1, 4, and 16 MiB containers. Node-local disk.
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
KERNEL="6.0 6.1 6.2 6.3 6.4 6.5 6.6 6.7 6.8 6.9 6.10"
extract_kernel $KERNEL
build_repo() {  # build_repo DIR CONTAINER_SIZE
  "$DDS" init "$1" --container-size "$2" > /dev/null 2>&1 || exit 1
  local ids=()
  for v in $KERNEL; do ids+=("$("$DDS" backup "$1" "$WORK/kernel/linux-$v" 2>> "$DDS_OUT/build-repos.log")"); done
  for i in 1 3 5 7 9; do "$DDS" rm "$1" "${ids[$i]}" >> "$DDS_OUT/build-repos.log" 2>&1 || exit 1; done
}
build_repo "$WORK/repo-4M" 4M
for t in 0.1 0.3 0.5 0.7 0.9; do
  measure E5 "t$t-c4M" gc --repo "$WORK/repo-4M" --work "$WORK/gc-work" --threshold $t --label kernel-alternate-rm
done
rm -rf "$WORK/repo-4M"
for c in 1M 16M; do
  build_repo "$WORK/repo-$c" $c
  measure E5 "t0.5-c$c" gc --repo "$WORK/repo-$c" --work "$WORK/gc-work" --threshold 0.5 --label kernel-alternate-rm
  rm -rf "$WORK/repo-$c"
done
echo "overall: $overall"
exit $overall
