#!/bin/bash
#SBATCH --job-name=dds-micro
#SBATCH --partition=short
#SBATCH --constraint=cascadelake
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=01:00:00
#
# Microbenchmarks (BENCHMARKS.md section 6), Google Benchmark, 5 repetitions after its own warm-up;
# tools/micro_to_results.py converts the JSON into one result file per benchmark family.
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
"$BUILD/bench/dds_micro_bench" --benchmark_repetitions=5 --benchmark_min_warmup_time=1 \
  --benchmark_format=json --benchmark_out="$DDS_OUT/micro.json" > "$DDS_OUT/micro.log" 2>&1 ||
  { echo "micro: FAILED"; overall=1; }
python3 "$DDS_SRC/tools/micro_to_results.py" --micro "$DDS_OUT/micro.json" --meta "$DDS_OUT/meta.json" \
  --results "$RESULTS" >> "$DDS_OUT/micro.log" 2>&1 || { echo "micro conversion: FAILED"; overall=1; }
echo "overall: $overall"
exit $overall
