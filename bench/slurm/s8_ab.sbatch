#!/bin/bash
#SBATCH --job-name=dds-s8-ab
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:30:00
#
# The sharded pending map and the batched file queue together (D101, D102) measured on one node
# against the commit before both (old), in ab_common.sh's configurations (experiment S8ab). The
# criterion: if dedup-heavy ingest is still slower than before them, both are reverted.
# Needs commit 64d51e6's source uploaded next to the job's (src-64d51e6) before submission.
AB_EXPERIMENT=S8ab
OLD_COMMIT=64d51e6
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/ab_common.sh"
