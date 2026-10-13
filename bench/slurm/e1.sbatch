#!/bin/bash
#SBATCH --job-name=dds-e1
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:00:00
#
# E1 (BENCHMARKS.md 5): content-defined vs fixed chunking, {kernel, synth} x {fastcdc, fixed}, 8 KiB
# average, every version backed up in order, mem mode (dedup metrics do not depend on storage).
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
KERNEL="6.0 6.1 6.2 6.3 6.4 6.5 6.6 6.7 6.8 6.9 6.10"
extract_kernel $KERNEL
"$GEN_SYNTH" --out "$WORK/synth" --seed 1 > "$DDS_OUT/gen_synth.log" 2>&1 || { echo "gen_synth failed"; exit 1; }
SYNTH_DIRS=""; SYNTH_LABELS=""
for v in 0 1 2 3 4 5 6 7 8 9 10; do SYNTH_DIRS="$SYNTH_DIRS${SYNTH_DIRS:+,}$WORK/synth/v$v"; SYNTH_LABELS="$SYNTH_LABELS${SYNTH_LABELS:+,}v$v"; done
cp "$WORK/synth/synth.json" "$DDS_OUT/synth.json"
for chunker in fastcdc fixed; do
  measure E1 "kernel-$chunker" ingest --versions "$(kernel_dirs $KERNEL)" --labels "${KERNEL// /,}" \
    --dataset kernel --storage mem --chunker $chunker --avg 8K --threads 16
  measure E1 "synth-$chunker" ingest --versions "$SYNTH_DIRS" --labels "$SYNTH_LABELS" \
    --dataset synth --storage mem --chunker $chunker --avg 8K --threads 16
done
echo "overall: $overall"
exit $overall
