#!/usr/bin/env bash
# Sourced by the same-node comparison jobs (s7_ab.sbatch, s8_ab.sbatch, s8_alone.sbatch; D99), which
# set AB_EXPERIMENT and OLD_COMMIT first, and optionally MID_COMMIT and MID_TAG for a third build.
# Builds OLD_COMMIT (and MID_COMMIT), each commit's source uploaded before submission, next to
# the job's own commit and runs E2's kernel ingest with each, in the order old, new, new, old (with a
# third build: old, mid, new, new, mid, old), at 64 KiB with 4, 16, and 32 threads and at 8 KiB with
# 16 and 32 (tags name the build, the average, the threads, and the pass). Then, at 64 KiB with 16
# threads, one pass of each build under `perf stat` and one under `perf record` ($AB_EXPERIMENT.txt
# in the job's evidence). With AB_MICRO_FILTER set, also the job's own build's microbenchmarks that
# match it (micro result files, as micro.sbatch writes them).
: "${AB_EXPERIMENT:?set AB_EXPERIMENT}" "${OLD_COMMIT:?set OLD_COMMIT}"
[ -z "${MID_COMMIT:-}" ] || : "${MID_TAG:?set MID_TAG with MID_COMMIT}"
source "${DDS_SRC_DIR:-/scratch/$USER/dedupstore/src}/bench/slurm/common.sh"
build_side "$OLD_COMMIT" old
BUILDS="old new"
[ -z "${MID_COMMIT:-}" ] || { build_side "$MID_COMMIT" "$MID_TAG"; BUILDS="old $MID_TAG new"; }
REVERSED=""
for build in $BUILDS; do REVERSED="$build $REVERSED"; done
NEW_BENCH="$BENCH"
KERNEL="6.0 6.1 6.2 6.3 6.4 6.5 6.6 6.7 6.8 6.9 6.10"
extract_kernel $KERNEL
warm "$WORK/kernel"
pick() {  # pick BUILD: point BENCH and META at that build (old, MID_TAG, or new)
  case "$1" in
    old) BENCH="$WORK/build-$OLD_COMMIT/bench/dds_bench" META="$DDS_OUT/meta-old.json" ;;
    new) BENCH="$NEW_BENCH" META="" ;;
    *) BENCH="$WORK/build-$MID_COMMIT/bench/dds_bench" META="$DDS_OUT/meta-$1.json" ;;
  esac
}
ingest() {  # ingest AVG THREADS [dds_bench args...]: E2's kernel configuration
  local avg="$1" threads="$2"
  shift 2
  echo ingest --versions "$(kernel_dirs $KERNEL)" --labels "${KERNEL// /,}" --dataset kernel \
    --storage mem --chunker fastcdc --avg "$avg" --threads "$threads" "$@"
}
run() {  # run BUILD AVG THREADS PASS
  pick "$1"
  # shellcheck disable=SC2046 # word splitting of ingest's output is intended (no spaces in paths)
  measure "$AB_EXPERIMENT" "$1-avg-$2-t$3-$4" $(ingest "$2" "$3")
}
for config in 64K:4 64K:16 64K:32 8K:16 8K:32; do
  avg="${config%%:*}" threads="${config##*:}"
  for build in $BUILDS; do run $build $avg $threads a; done
  for build in $REVERSED; do run $build $avg $threads b; done
done
if [ -n "${AB_MICRO_FILTER:-}" ]; then
  "$BUILD/bench/dds_micro_bench" --benchmark_filter="$AB_MICRO_FILTER" --benchmark_repetitions=5 \
    --benchmark_min_warmup_time=1 --benchmark_format=json --benchmark_out="$DDS_OUT/micro.json" \
    > "$DDS_OUT/micro.log" 2>&1 &&
    python3 "$DDS_SRC/tools/micro_to_results.py" --micro "$DDS_OUT/micro.json" \
      --meta "$DDS_OUT/meta.json" --results "$RESULTS" >> "$DDS_OUT/micro.log" 2>&1 ||
    { echo "micro: FAILED"; overall=1; }
fi
# Not result files: perf output goes to the job's evidence directory.
echo "perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid)" > "$DDS_OUT/$AB_EXPERIMENT.txt"
for build in $BUILDS; do
  pick $build
  echo "=== perf stat, $build, 64 KiB, 16 threads" >> "$DDS_OUT/$AB_EXPERIMENT.txt"
  # shellcheck disable=SC2046
  perf stat -e task-clock -- "$BENCH" $(ingest 64K 16 --runs 1 --warmup 1) --experiment "$AB_EXPERIMENT" \
    --out "$WORK/stat-$build.json" >> "$DDS_OUT/$AB_EXPERIMENT.txt" 2>&1 ||
    echo "perf stat failed for $build" >> "$DDS_OUT/$AB_EXPERIMENT.txt"
  echo "=== perf record, $build, 64 KiB, 16 threads" >> "$DDS_OUT/$AB_EXPERIMENT.txt"
  # shellcheck disable=SC2046
  if perf record -F 499 -g -o "$WORK/perf-$build.data" -- "$BENCH" $(ingest 64K 16 --runs 1 \
       --warmup 0) --experiment "$AB_EXPERIMENT" --out "$WORK/rec-$build.json" >> "$DDS_OUT/$AB_EXPERIMENT.txt" 2>&1; then
    perf report -i "$WORK/perf-$build.data" --stdio --no-children --percent-limit 0.5 \
      --sort symbol >> "$DDS_OUT/$AB_EXPERIMENT.txt" 2>&1
  else
    echo "perf record failed for $build" >> "$DDS_OUT/$AB_EXPERIMENT.txt"
  fi
done
echo "overall: $overall"
exit $overall
