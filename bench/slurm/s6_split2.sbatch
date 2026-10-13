#!/bin/bash
#SBATCH --job-name=dds-s6-split2
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:30:00
#
# S6split2 (BENCHMARKS.md 5): the build S6split measured (old, d78a548) against this commit, which
# reserves each segment's buffer once (new), on one node: ingest in memory at 8 KiB with 16 threads
# of S6split's 8 GiB file and of the full fine-tune's checkpoints with splitting on (64 MiB), and of
# the 8 GiB file with splitting off as a control, each in the order old, new, new, old.
# Needs commit d78a548's source uploaded next to the job's (src-d78a548) before submission.
OLD_COMMIT=d78a548
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
build_side "$OLD_COMMIT" old
NEW_BENCH="$BENCH"
"$GEN_SYNTH" --out "$WORK/one8g" --seed 6 --random-bytes 8192M --text-bytes 0 --min-file 8192M \
  --max-file 8192M --versions 0 > "$DDS_OUT/gen_synth.log" 2>&1 || { echo "gen_synth failed"; exit 1; }
ONE="$WORK/one8g/v0"
CKPT="$DATA/ckpt/full-seed0"
CKPT_DIRS=$(for d in "$CKPT"/checkpoint-*; do echo "${d##*-} $d"; done | sort -n | cut -d' ' -f2- | paste -sd, -)
CKPT_LABELS=$(echo "$CKPT_DIRS" | tr , '\n' | xargs -n1 basename | sed 's/checkpoint-/step-/' | paste -sd, -)
[ -n "$CKPT_DIRS" ] || { echo "no checkpoints under $CKPT"; exit 1; }
warm "$ONE" "$CKPT"
for run in old-a new-a new-b old-b; do
  if [ "${run%-*}" = old ]; then BENCH="$WORK/build-$OLD_COMMIT/bench/dds_bench" META="$DDS_OUT/meta-old.json"
  else BENCH="$NEW_BENCH" META=""; fi
  measure S6split2 "one8g-on-$run" ingest --versions "$ONE" --labels one8g --dataset one8g \
    --storage mem --avg 8K --threads 16 --split-mib 64
  measure S6split2 "ckpt-on-$run" ingest --versions "$CKPT_DIRS" --labels "$CKPT_LABELS" \
    --dataset ckpt-full-seed0 --storage mem --avg 8K --threads 16 --split-mib 64
  measure S6split2 "one8g-off-$run" ingest --versions "$ONE" --labels one8g --dataset one8g \
    --storage mem --avg 8K --threads 16 --split-mib 0
done
echo "overall: $overall"
exit $overall
