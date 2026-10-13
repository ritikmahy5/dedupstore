#!/bin/bash
#SBATCH --job-name=dds-e8-train
#SBATCH --partition=gpu-short
#SBATCH --gres=gpu:1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=02:00:00
#
# E8's dataset (D97): fine-tune distilgpt2 five ways on text from the Linux 6.10 Documentation
# directory and keep every checkpoint in /scratch/$USER/dedupstore-data/ckpt/<run>/checkpoint-<step>.
# On one GPU: the CPU version (job 10930678) shared a node whose other job drove it into swap, and
# timed out after 8 hours with 150 of 1500 steps done.
# Not a measurement: no result files. Needs bench/datasets/ckpt/setup.sh (the model and numpy
# downloads) run on the login node first.
set -uo pipefail
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/env.sh"
dds_record_machine
DATA="/scratch/$USER/dedupstore-data"
module load anaconda3/2024.06  # torch 2.8.0, transformers 4.42.4, safetensors (D97)
WORK="${TMPDIR:-/tmp}/dds-e8-$SLURM_JOB_ID"
mkdir -p "$WORK" "$DATA/ckpt"
trap 'rm -rf "$WORK"' EXIT
# numpy 1.26.4 from setup.sh, since the module's numpy 2.4.4 fails transformers' import check.
unzip -q "/scratch/$USER/dedupstore-tools/wheels/numpy-1.26.4-cp312-cp312-manylinux_2_17_x86_64.manylinux2014_x86_64.whl" \
  -d "$WORK/pylib" || exit 1
export PYTHONPATH="$WORK/pylib" PYTHONNOUSERSITE=1
# A node-local copy, since a later sync replaces /scratch/.../src while this job runs.
cp "$DDS_SRC/bench/datasets/ckpt/train.py" "$WORK/train.py" || exit 1
export HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 OMP_NUM_THREADS=$SLURM_CPUS_PER_TASK
export HF_HOME="/scratch/$USER/dedupstore-tools/hf"  # the home directory has a small quota
python -c "import numpy, torch, transformers; print('numpy', numpy.__version__, 'torch', torch.__version__, 'transformers', transformers.__version__, 'cuda', torch.cuda.is_available() and torch.cuda.get_device_name(0))" | tee "$DDS_OUT/versions.txt"
nvidia-smi >> "$DDS_OUT/versions.txt" 2>&1
tar -xJf "$DATA/kernel/linux-6.10.tar.xz" -C "$WORK" linux-6.10/Documentation || exit 1
find "$WORK/linux-6.10/Documentation" -name '*.rst' -print0 | sort -z | xargs -0 cat > "$WORK/corpus.txt"
echo "corpus: $(wc -c < "$WORK/corpus.txt") bytes" | tee "$DDS_OUT/corpus.txt"
overall=0
for run in full:0 full:1 full:2 partial:0 lora:0; do
  method="${run%%:*}"; seed="${run##*:}"
  out="$DATA/ckpt/$method-seed$seed"
  rm -rf "$out"
  echo "=== $method seed $seed"
  if python "$WORK/train.py" --method "$method" --seed "$seed" --device cuda \
       --model "$DATA/models/distilgpt2" --corpus "$WORK/corpus.txt" --out "$out" \
       > "$DDS_OUT/train-$method-seed$seed.log" 2>&1; then
    du -sb "$out"/checkpoint-* | tee -a "$DDS_OUT/checkpoints.txt"
  else
    echo "$method seed $seed: FAILED"; overall=1
  fi
done
echo "overall: $overall"
exit $overall
