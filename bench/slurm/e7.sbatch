#!/bin/bash
#SBATCH --job-name=dds-e7
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=08:00:00
#
# E7 (BENCHMARKS.md 5): replication over a simulated WAN. synth base of 256 MiB (192 MiB random,
# 64 MiB text) and one version with 5% edits; replicate the base, then the version, per cell.
# Window sweep at 100 Mbit/s and 80 ms RTT; link grid of one-way latency x bandwidth at W = 8 and
# 32; and kernel 6.0 -> 6.1 at 1000 Mbit/s as a realism check. Time estimate: BENCHMARKS.md E7.
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
"$GEN_SYNTH" --out "$WORK/synth7" --seed 7 --random-bytes 192M --text-bytes 64M --versions 1 \
  --edit-rate 0.05 > "$DDS_OUT/gen_synth.log" 2>&1 || { echo "gen_synth failed"; exit 1; }
cp "$WORK/synth7/synth.json" "$DDS_OUT/synth7.json"
BASE="$WORK/synth7/v0"; NEXT="$WORK/synth7/v1"
warm "$WORK/synth7"
for w in 1 2 4 8 16 32; do
  measure E7 "sweep-w$w" replicate --base "$BASE" --next "$NEXT" --latency-ms 40 --mbit 100 --window $w --dataset synth7
done
for mbit in 1000 100 10; do
  for lat in 0.5 10 40 100; do
    for w in 8 32; do
      measure E7 "grid-l$lat-b$mbit-w$w" replicate --base "$BASE" --next "$NEXT" --latency-ms $lat \
        --mbit $mbit --window $w --dataset synth7
    done
  done
done
extract_kernel 6.0 6.1
measure E7 kernel-6.0-6.1-b1000 replicate --base "$WORK/kernel/linux-6.0" --next "$WORK/kernel/linux-6.1" \
  --latency-ms 10 --mbit 1000 --window 32 --dataset kernel-6.0-6.1
echo "overall: $overall"
exit $overall
