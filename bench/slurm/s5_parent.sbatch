#!/bin/bash
#SBATCH --job-name=dds-s5-parent
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:30:00
#
# Parent snapshots (D106), experiment S5parent: two backups to disk in a fresh repository, the
# second with the first as its parent (previous) or without (none), at 1, 4, and 16 threads, in the
# order none, previous, previous, none (passes a and b). Workloads: linux-6.10 twice ("same"), and
# linux-6.10 then a copy with its mtimes kept and every 100th file appended to ("mod1pct"), the
# nightly backup parent snapshots are for. The kernel tarball's mtimes are the release date, far
# older than any backup's start.
S5_EXPERIMENT=S5parent
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/s5_common.sh"
