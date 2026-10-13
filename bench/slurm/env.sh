#!/usr/bin/env bash
# Sourced by every Explorer job: a clean, recorded toolchain environment (D57).
#
# A login profile may activate a conda environment that puts its own cmake and python first on
# PATH and an anaconda include directory on CPATH. None of that may leak into a build, so jobs
# start from a minimal PATH and load modules explicitly.

export PATH=/usr/local/bin:/usr/bin:/bin
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH LD_LIBRARY_PATH PKG_CONFIG_PATH
unset CONDA_PREFIX CONDA_DEFAULT_ENV PYTHONPATH
# shellcheck disable=SC1091
source /usr/share/Modules/init/bash
module purge
# What the site profile (/etc/profile.d/zz_modules.sh) does; the clean environment skips it.
module use /shared/EL9/explorer/modulefiles

DDS_ROOT="/scratch/$USER/dedupstore"
# The source this job builds: its own commit's directory (src-<commit>, set at submission), so a
# later upload cannot change what a queued job builds (D105).
DDS_SRC="${DDS_SRC_DIR:-$DDS_ROOT/src}"
DDS_OUT="$DDS_ROOT/out/${SLURM_JOB_ID:-manual}"
DDS_TOOLS="/scratch/$USER/dedupstore-tools"
mkdir -p "$DDS_OUT"

module load cmake/3.30.2

# There is no ninja module, and compute nodes cannot always reach PyPI or GitHub, so ninja is
# installed into $DDS_TOOLS/venv and the dependency tarballs mirrored into $DDS_TOOLS/deps on the
# login node beforehand.
export PATH="$DDS_TOOLS/venv/bin:$PATH"
DDS_DEPS_MIRROR="$DDS_TOOLS/deps"
command -v ninja >/dev/null || { echo "ninja missing from $DDS_TOOLS/venv" >&2; exit 2; }

# Compilers: the system GCC (Rocky Linux 9) and the LLVM clang that ships inside the Intel
# oneAPI 2025.0 module (there is no plain clang/llvm module on Explorer).
DDS_GCC_CC=/usr/bin/gcc
DDS_GCC_CXX=/usr/bin/g++
DDS_INTEL_ROOT=/shared/EL9/explorer/intel-oneapi/2025.0.1/compiler/2025.0
DDS_CLANG_CC="$DDS_INTEL_ROOT/bin/compiler/clang"
DDS_CLANG_CXX="$DDS_INTEL_ROOT/bin/compiler/clang++"

dds_record_machine() {
  {
    echo "job: ${SLURM_JOB_ID:-manual} node: $(hostname)"
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "kernel: $(uname -r)"
    lscpu | grep -E '^(Model name|CPU\(s\)|Thread|Core|Socket)'
    free -g | head -2
    # The source is uploaded as `git archive` of one commit, so it is exactly that (never dirty).
    echo "git: $(sed -n 's/^commit=//p' "$DDS_SRC/dds-build-info.txt") dirty=false"
    echo "cmake: $(cmake --version | head -1)"
    echo "ninja: $(ninja --version)"
    echo "gcc: $($DDS_GCC_CXX --version | head -1)"
    echo "clang: $($DDS_CLANG_CXX --version | head -1) ($("$DDS_CLANG_CXX" -print-resource-dir))"
    echo "modules:"; module list 2>&1
  } > "$DDS_OUT/machine.txt" 2>&1
}
