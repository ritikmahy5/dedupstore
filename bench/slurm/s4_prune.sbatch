#!/bin/bash
#SBATCH --job-name=dds-s4-prune
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:30:00
#
# S4prune (BENCHMARKS.md 5): replication with and without subtree pruning (D108). The base goes
# over an unlimited link; the next version's transfer is measured. "small": linux-6.10, then a copy
# with one byte appended to the first 5 files (in name order) of three directories, 2 and 5 levels
# deep. "big": linux-6.0, then 6.1 (E7's realism pair).
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
extract_kernel 6.0 6.1 6.10
K="$WORK/kernel/linux-6.10"
cp -a "$K" "$K-small" || { echo "cannot copy linux-6.10"; exit 1; }
: > "$WORK/modified.txt"
for d in fs/ext4 Documentation/admin-guide drivers/net/ethernet/intel/e1000e; do
  [ -d "$K-small/$d" ] || { echo "linux-6.10 has no $d"; exit 1; }
  find "$K-small/$d" -maxdepth 1 -type f | LC_ALL=C sort | head -5 >> "$WORK/modified.txt"
done
while IFS= read -r f; do printf 'x' >> "$f"; done < "$WORK/modified.txt"
sed "s|^$K-small/||" "$WORK/modified.txt" | tee "$DDS_OUT/S4prune-modified.txt"
warm "$K" "$K-small" "$WORK/kernel/linux-6.0" "$WORK/kernel/linux-6.1"
for cell in "0.5 1000" "40 1000" "40 100" "40 10"; do
  read -r lat mbit <<< "$cell"
  for prune in 0 1; do
    measure S4prune "small-l$lat-b$mbit-p$prune" replicate --base "$K" --next "$K-small" \
      --latency-ms "$lat" --mbit "$mbit" --window 8 --prune "$prune" --base-free 1 \
      --dataset kernel-6.10-small
  done
done
for cell in "0.5 1000" "40 100"; do
  read -r lat mbit <<< "$cell"
  for prune in 0 1; do
    measure S4prune "big-l$lat-b$mbit-p$prune" replicate --base "$WORK/kernel/linux-6.0" \
      --next "$WORK/kernel/linux-6.1" --latency-ms "$lat" --mbit "$mbit" --window 8 \
      --prune "$prune" --base-free 1 --dataset kernel-6.0-6.1
  done
done
echo "overall: $overall"
exit $overall
