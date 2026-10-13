#!/bin/bash
#SBATCH --job-name=dds-s8-alone
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:00:00
#
# Splits S8ab's gain between the sharded pending map (D101) and the batched file queue (D102): the
# commit before both (old), the batched queue alone on that commit (s8: c853572, a cherry-pick of
# afa1f9f, not merged), and the job's commit with both (new), on one node in ab_common.sh's
# configurations (experiment S8alone).
# Needs the source of commits 64d51e6 and c853572 uploaded next to the job's before submission.
AB_EXPERIMENT=S8alone
OLD_COMMIT=64d51e6
MID_COMMIT=c853572
MID_TAG=s8
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/ab_common.sh"
