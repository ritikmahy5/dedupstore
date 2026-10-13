#!/bin/bash
#SBATCH --job-name=dds-e2
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:30:00
#
# E2 (BENCHMARKS.md 5): chunk size trade-off on kernel, FastCDC with average 2 KiB to 1 MiB
# (min = avg / 4, max = avg x 8), mem mode.
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
KERNEL="6.0 6.1 6.2 6.3 6.4 6.5 6.6 6.7 6.8 6.9 6.10"
extract_kernel $KERNEL
warm "$WORK/kernel"
for avg in 2K 4K 8K 16K 64K 1M; do
  measure E2 "avg-$avg" ingest --versions "$(kernel_dirs $KERNEL)" --labels "${KERNEL// /,}" \
    --dataset kernel --storage mem --chunker fastcdc --avg $avg --threads 16
done
echo "overall: $overall"
exit $overall
