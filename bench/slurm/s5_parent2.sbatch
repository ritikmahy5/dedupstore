#!/bin/bash
#SBATCH --job-name=dds-s5-parent2
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:30:00
#
# Parent snapshots (D106) again after one read guard per batch replaced one per reused file
# (experiment S5parent2): the same runs as S5parent (s5_common.sh).
S5_EXPERIMENT=S5parent2
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/s5_common.sh"
