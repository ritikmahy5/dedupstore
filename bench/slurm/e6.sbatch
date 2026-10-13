#!/bin/bash
#SBATCH --job-name=dds-e6
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:00:00
#
# E6 (BENCHMARKS.md 5): GC interference. Back up the synth versions in order without GC, then with a
# GC loop on the same repository; then 4 concurrent sessions over overlapping versions (duplicate
# copies, D32). mem mode.
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
"$GEN_SYNTH" --out "$WORK/synth" --seed 1 > "$DDS_OUT/gen_synth.log" 2>&1 || { echo "gen_synth failed"; exit 1; }
cp "$WORK/synth/synth.json" "$DDS_OUT/synth.json"
DIRS=""; for v in 0 1 2 3 4 5 6 7 8 9 10; do DIRS="$DIRS${DIRS:+,}$WORK/synth/v$v"; done
warm "$WORK/synth"
measure E6 sequential interference --versions "$DIRS" --mode sequential --threads 8
measure E6 with-gc interference --versions "$DIRS" --mode with-gc --threads 8
measure E6 concurrent-4 interference --versions "$DIRS" --mode concurrent --sessions 4 --threads 4
echo "overall: $overall"
exit $overall
