#!/usr/bin/env bash
# Prepares E8's checkpoint generator (D97) on Explorer's login node: downloads distilgpt2 with the
# huggingface_hub that Explorer's anaconda3/2024.06 module provides, and the numpy 1.26.4 wheel,
# both into /scratch. Training uses that module's torch and transformers inside a SLURM job,
# offline, but the module's numpy (2.4.4) is newer than its transformers (4.42.4) accepts, so the
# job unzips this wheel into its node-local directory and puts it first on PYTHONPATH.
#
#   bash bench/datasets/ckpt/setup.sh        # on the login node, from the synced source tree
set -euo pipefail
model="/scratch/$USER/dedupstore-data/models/distilgpt2"
wheels="/scratch/$USER/dedupstore-tools/wheels"
export HF_HOME="/scratch/$USER/dedupstore-tools/hf"  # the home directory has a small quota
source /usr/share/Modules/init/bash
module use /shared/EL9/explorer/modulefiles
module load anaconda3/2024.06
if [ ! -f "$model/model.safetensors" ]; then
  mkdir -p "$model"
  python -c "from huggingface_hub import snapshot_download; snapshot_download('distilbert/distilgpt2', local_dir='$model', allow_patterns=['*.json', '*.txt', 'model.safetensors'])"
fi

# The login node killed both `pip install --target` and `unzip` of this wheel part-way, so only the
# download happens here, checked against PyPI's published SHA-256 and renamed into place when whole.
wheel=numpy-1.26.4-cp312-cp312-manylinux_2_17_x86_64.manylinux2014_x86_64.whl
url=https://files.pythonhosted.org/packages/0f/50/de23fde84e45f5c4fda2488c759b69990fd4512387a8632860f3ac9cd225/$wheel
sha256=675d61ffbfa78604709862923189bad94014bef562cc35cf61d3a07bba02a7ed
if [ ! -f "$wheels/$wheel" ]; then
  mkdir -p "$wheels"
  curl -fsSL -o "$wheels/$wheel.tmp" "$url"
  echo "$sha256  $wheels/$wheel.tmp" | sha256sum -c -
  mv "$wheels/$wheel.tmp" "$wheels/$wheel"
fi
ls -la "$wheels"
ls -la "$model"
