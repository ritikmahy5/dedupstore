#!/bin/bash
#SBATCH --job-name=dds-e8
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=02:00:00
#
# E8 (BENCHMARKS.md 5): back up each run's checkpoints in step order into its own repository, with
# per-file new bytes; then one repository holding the three seeds of the full fine-tune. mem mode.
# The checkpoints come from e8_train.sbatch (D97).
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
CKPT="$DATA/ckpt"
# steps RUN_DIR: its checkpoint directories in step order, sorted by the number after the last '-'.
# `sort -t- -k2 -n` would also split at the '-' in "dedupstore-data" and put checkpoint-50 last.
steps() { for d in "$1"/checkpoint-*; do echo "${d##*-} $d"; done | sort -n | cut -d' ' -f2-; }
for run in full-seed0 full-seed1 full-seed2 partial-seed0 lora-seed0; do
  dirs=$(steps "$CKPT/$run" | paste -sd, -)
  labels=$(steps "$CKPT/$run" | xargs -n1 basename | sed 's/checkpoint-/step-/' | paste -sd, -)
  measure E8 "$run" ingest --versions "$dirs" --labels "$labels" --dataset "ckpt-$run" \
    --storage mem --threads 16 --per-file 1
done
dirs=""; labels=""
for s in 0 1 2; do
  dirs="$dirs${dirs:+,}$(steps "$CKPT/full-seed$s" | paste -sd, -)"
  labels="$labels${labels:+,}$(steps "$CKPT/full-seed$s" | xargs -n1 basename | sed "s/checkpoint-/s$s-step-/" | paste -sd, -)"
done
measure E8 full-3-seeds ingest --versions "$dirs" --labels "$labels" --dataset ckpt-full-3-seeds \
  --storage mem --threads 16
echo "overall: $overall"
exit $overall
