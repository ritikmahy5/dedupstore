#!/bin/bash
#SBATCH --job-name=dds-s7-ab
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:30:00
#
# The sharded pending map (D101) measured on one node, as E2ab measured D98 (D99): the commit
# before it (old) against the job's own commit (new), in ab_common.sh's configurations
# (experiment S7ab).
# Needs commit 64d51e6's source uploaded next to the job's (src-64d51e6) before submission.
AB_EXPERIMENT=S7ab
OLD_COMMIT=64d51e6
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/ab_common.sh"
