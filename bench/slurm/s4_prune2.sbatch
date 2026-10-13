#!/bin/bash
#SBATCH --job-name=dds-s4-prune2
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:30:00
#
# S4prune2 (BENCHMARKS.md 5): S4prune's pruned transfers with the build it measured (old, 61d9cac)
# and with this commit (new), which has the review's three fixes to D108, on one node, in the order
# old, new, new, old: S4prune's "small" and "big" workloads and links, pruning on, base sent over
# an unlimited link.
# Needs commit 61d9cac's source uploaded next to the job's (src-61d9cac) before submission.
OLD_COMMIT=61d9cac
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
build_side "$OLD_COMMIT" old
NEW_BENCH="$BENCH"
extract_kernel 6.0 6.1 6.10
K="$WORK/kernel/linux-6.10"
cp -a "$K" "$K-small" || { echo "cannot copy linux-6.10"; exit 1; }
: > "$WORK/modified.txt"
for d in fs/ext4 Documentation/admin-guide drivers/net/ethernet/intel/e1000e; do
  [ -d "$K-small/$d" ] || { echo "linux-6.10 has no $d"; exit 1; }
  find "$K-small/$d" -maxdepth 1 -type f | LC_ALL=C sort | head -5 >> "$WORK/modified.txt"
done
while IFS= read -r f; do printf 'x' >> "$f"; done < "$WORK/modified.txt"
sed "s|^$K-small/||" "$WORK/modified.txt" > "$DDS_OUT/S4prune2-modified.txt"
warm "$K" "$K-small" "$WORK/kernel/linux-6.0" "$WORK/kernel/linux-6.1"
pick() {  # pick old|new: point BENCH and META at that build
  if [ "$1" = old ]; then BENCH="$WORK/build-$OLD_COMMIT/bench/dds_bench" META="$DDS_OUT/meta-old.json"
  else BENCH="$NEW_BENCH" META=""; fi
}
for cell in "0.5 1000" "40 1000" "40 100" "40 10"; do
  read -r lat mbit <<< "$cell"
  for run in old-a new-a new-b old-b; do
    pick "${run%-*}"
    measure S4prune2 "small-l$lat-b$mbit-$run" replicate --base "$K" --next "$K-small" \
      --latency-ms "$lat" --mbit "$mbit" --window 8 --prune 1 --base-free 1 \
      --dataset kernel-6.10-small
  done
done
for cell in "0.5 1000" "40 100"; do
  read -r lat mbit <<< "$cell"
  for run in old-a new-a new-b old-b; do
    pick "${run%-*}"
    measure S4prune2 "big-l$lat-b$mbit-$run" replicate --base "$WORK/kernel/linux-6.0" \
      --next "$WORK/kernel/linux-6.1" --latency-ms "$lat" --mbit "$mbit" --window 8 --prune 1 \
      --base-free 1 --dataset kernel-6.0-6.1
  done
done
echo "overall: $overall"
exit $overall
