#!/bin/bash
#SBATCH --job-name=dds-s3-zstd
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=02:00:00
#
# Compression (D107), experiment S3zstd: levels none, zstd:1, zstd:3, and zstd:9 on three kinds of
# data, ingested in mem mode with 16 threads at an 8 KiB average (E1's settings): the 11 kernel
# releases (text), synth (E1's random-content generator), and the partial fine-tune's 6 checkpoints
# from E8 (model weights). Dedup is measured on content, so it should not move; compression is
# reported as its own ratio. Then restore of linux-6.10 from a disk repository with and without
# zstd:3 (E4's method, warm cache, no hash check).
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
KERNEL="6.0 6.1 6.2 6.3 6.4 6.5 6.6 6.7 6.8 6.9 6.10"
extract_kernel $KERNEL
"$GEN_SYNTH" --out "$WORK/synth" --seed 1 > "$DDS_OUT/gen_synth.log" 2>&1 || { echo "gen_synth failed"; exit 1; }
SYNTH_DIRS=""; SYNTH_LABELS=""
for v in 0 1 2 3 4 5 6 7 8 9 10; do SYNTH_DIRS="$SYNTH_DIRS${SYNTH_DIRS:+,}$WORK/synth/v$v"; SYNTH_LABELS="$SYNTH_LABELS${SYNTH_LABELS:+,}v$v"; done
# The partial fine-tune's checkpoints in step order (e8.sbatch's `steps`).
CKPT="$DATA/ckpt/partial-seed0"
CKPT_DIRS=$(for d in "$CKPT"/checkpoint-*; do echo "${d##*-} $d"; done | sort -n | cut -d' ' -f2- | paste -sd, -)
CKPT_LABELS=$(echo "$CKPT_DIRS" | tr , '\n' | xargs -n1 basename | sed 's/checkpoint-/step-/' | paste -sd, -)
for level in none zstd:1 zstd:3 zstd:9; do
  tag="${level/:/}"
  measure S3zstd "kernel-$tag" ingest --versions "$(kernel_dirs $KERNEL)" --labels "${KERNEL// /,}" \
    --dataset kernel --storage mem --avg 8K --threads 16 --compression "$level"
  measure S3zstd "synth-$tag" ingest --versions "$SYNTH_DIRS" --labels "$SYNTH_LABELS" \
    --dataset synth --storage mem --avg 8K --threads 16 --compression "$level"
  measure S3zstd "ckpt-$tag" ingest --versions "$CKPT_DIRS" --labels "$CKPT_LABELS" \
    --dataset ckpt-partial-seed0 --storage mem --avg 8K --threads 16 --compression "$level"
done
declare -A ID
for level in none zstd:3; do
  tag="${level/:/}"
  "$DDS" init "$WORK/r-$tag" --compression "$level" > /dev/null 2>> "$DDS_OUT/build-repos.log" || exit 1
  ID[$tag]=$("$DDS" backup "$WORK/r-$tag" "$WORK/kernel/linux-6.10" 2>> "$DDS_OUT/build-repos.log") ||
    exit 1
done
rm -rf "$WORK/kernel"   # restores must read the repository, not a cached source tree (E4)
for tag in none zstd3; do
  measure S3zstd "restore-6.10-$tag" restore --repo "$WORK/r-$tag" --snapshot "${ID[$tag]}" \
    --dest "$WORK/dest" --verify 0 --label kernel-6.10
done
echo "overall: $overall"
exit $overall
