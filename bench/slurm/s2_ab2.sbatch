#!/bin/bash
#SBATCH --job-name=dds-s2-ab2
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:30:00
#
# Hardware CRC32C (D103) again after the load fix (experiment S2ab2): three builds on one node, in
# ab_common.sh's configurations, in the order old, s2fix, new, new, s2fix, old. old is the commit
# before hardware CRC32C (33df8db); s2fix is the build S2ab measured plus the load fix and nothing
# else (6366820); new is the job's commit, which also has the index checkpoint (D104) and parent
# snapshots (D106). Then the CRC32C microbenchmarks of the job's build.
# Needs the source of commits 33df8db and 6366820 uploaded next to the job's before submission.
AB_EXPERIMENT=S2ab2
OLD_COMMIT=33df8db
MID_COMMIT=6366820
MID_TAG=s2fix
AB_MICRO_FILTER=Crc32c
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/ab_common.sh"
