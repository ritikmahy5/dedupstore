#!/bin/bash
#SBATCH --job-name=dds-s6-split
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:30:00
#
# S6split (BENCHMARKS.md 5): large files split across workers (D109). Ingest in memory at 8 KiB of
# one 8 GiB random file and of the full fine-tune's 6 checkpoints (E8: two large files each), with
# splitting off (--split-mib 0) and at the default 64 MiB, at 4 and 16 threads, in the order off,
# on, on, off (passes a and b); and the 8 GiB file at 1 thread. linux-6.10, which has no file of
# 64 MiB, at 16 threads with the commit before splitting (old) and this one (new), in the order
# old, new, new, old. Then restores of the 8 GiB file from disk repositories backed up with and
# without splitting.
# Needs commit 61d9cac's source uploaded next to the job's (src-61d9cac) before submission.
OLD_COMMIT=61d9cac
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
build_side "$OLD_COMMIT" old
NEW_BENCH="$BENCH"
"$GEN_SYNTH" --out "$WORK/one8g" --seed 6 --random-bytes 8192M --text-bytes 0 --min-file 8192M \
  --max-file 8192M --versions 0 > "$DDS_OUT/gen_synth.log" 2>&1 || { echo "gen_synth failed"; exit 1; }
ONE="$WORK/one8g/v0"
# The full fine-tune's checkpoints in step order (e8.sbatch's `steps`).
CKPT="$DATA/ckpt/full-seed0"
CKPT_DIRS=$(for d in "$CKPT"/checkpoint-*; do echo "${d##*-} $d"; done | sort -n | cut -d' ' -f2- | paste -sd, -)
CKPT_LABELS=$(echo "$CKPT_DIRS" | tr , '\n' | xargs -n1 basename | sed 's/checkpoint-/step-/' | paste -sd, -)
[ -n "$CKPT_DIRS" ] || { echo "no checkpoints under $CKPT"; exit 1; }
extract_kernel 6.10
K="$WORK/kernel/linux-6.10"
warm "$ONE" "$CKPT" "$K"
measure S6split one8g-t1 ingest --versions "$ONE" --labels one8g --dataset one8g --storage mem \
  --avg 8K --threads 1
for threads in 4 16; do
  for run in off-a on-a on-b off-b; do
    split=$([ "${run%-*}" = on ] && echo 64 || echo 0)
    measure S6split "one8g-t$threads-$run" ingest --versions "$ONE" --labels one8g --dataset one8g \
      --storage mem --avg 8K --threads "$threads" --split-mib "$split"
    measure S6split "ckpt-t$threads-$run" ingest --versions "$CKPT_DIRS" --labels "$CKPT_LABELS" \
      --dataset ckpt-full-seed0 --storage mem --avg 8K --threads "$threads" --split-mib "$split"
  done
done
for run in old-a new-a new-b old-b; do
  if [ "${run%-*}" = old ]; then BENCH="$WORK/build-$OLD_COMMIT/bench/dds_bench" META="$DDS_OUT/meta-old.json"
  else BENCH="$NEW_BENCH" META=""; fi
  measure S6split "kernel-t16-$run" ingest --versions "$K" --labels 6.10 --dataset kernel-6.10 \
    --storage mem --avg 8K --threads 16
done
BENCH="$NEW_BENCH" META=""
declare -A ID
for mode in on off; do
  split=$([ "$mode" = on ] && echo 64M || echo 0)
  "$DDS" init "$WORK/r-$mode" > /dev/null 2>> "$DDS_OUT/build-repos.log" || exit 1
  ID[$mode]=$("$DDS" backup "$WORK/r-$mode" "$ONE" --threads 16 --split-size "$split" \
    2>> "$DDS_OUT/build-repos.log") || exit 1
done
rm -rf "$WORK/one8g"   # restores must read the repository, not a cached source (E4)
for mode in on off; do
  measure S6split "restore-one8g-$mode" restore --repo "$WORK/r-$mode" --snapshot "${ID[$mode]}" \
    --dest "$WORK/dest" --verify 0 --label one8g
done
echo "overall: $overall"
exit $overall
