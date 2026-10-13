#!/usr/bin/env bash
# Sourced by s5_parent.sbatch and s5_parent2.sbatch, which set S5_EXPERIMENT first: two backups to
# disk in a fresh repository, the second with the first as its parent (previous) or without (none),
# at 1, 4, and 16 threads, in the order none, previous, previous, none (passes a and b), for
# linux-6.10 twice ("same") and linux-6.10 then a copy that kept its mtimes with every 100th file
# appended to ("mod1pct").
: "${S5_EXPERIMENT:?set S5_EXPERIMENT}"
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
extract_kernel 6.10
K="$WORK/kernel/linux-6.10"
cp -a "$K" "$K-mod" || { echo "cannot copy linux-6.10"; exit 1; }
# Every 100th regular file in name order gets one more byte, so its size and mtime change; cp -a
# kept every other file's mtime.
find "$K-mod" -type f | sort | awk 'NR % 100 == 0' > "$WORK/modified.txt"
while IFS= read -r f; do printf 'x' >> "$f"; done < "$WORK/modified.txt"
echo "modified $(wc -l < "$WORK/modified.txt") of $(find "$K-mod" -type f | wc -l) regular files" |
  tee "$DDS_OUT/$S5_EXPERIMENT-modified.txt"
warm "$K" "$K-mod"
for workload in "same:$K" "mod1pct:$K-mod"; do
  name="${workload%%:*}" second="${workload#*:}"
  for threads in 1 4 16; do
    for run in none-a previous-a previous-b none-b; do
      measure "$S5_EXPERIMENT" "$name-t$threads-$run" ingest --versions "$K,$second" \
        --labels first,second \
        --dataset "kernel-6.10-$name" --storage disk --repo "$WORK/s5-repo" --threads "$threads" \
        --parent "${run%-*}"
    done
  done
done
echo "overall: $overall"
exit $overall
