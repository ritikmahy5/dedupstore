#!/bin/bash
#SBATCH --job-name=dds-s2-ab
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:30:00
#
# Hardware CRC32C (D103) measured on one node against the commit before it (old), in
# ab_common.sh's configurations (experiment S2ab), plus the CRC32C microbenchmarks of the job's own
# build, which has both the hardware and the slicing-by-8 version.
# Needs commit 33df8db's source uploaded next to the job's (src-33df8db) before submission.
AB_EXPERIMENT=S2ab
OLD_COMMIT=33df8db
AB_MICRO_FILTER=Crc32c
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/ab_common.sh"
