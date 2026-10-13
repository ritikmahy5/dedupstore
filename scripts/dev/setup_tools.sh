#!/usr/bin/env bash
# Installs the pinned clang-format and clang-tidy (LLVM 19, the version Explorer's toolchain
# ships; D61) into .tools/venv. The tidy preset finds clang-tidy there.
#
#   scripts/dev/setup_tools.sh
#   .tools/venv/bin/clang-format -i src/dds/**/*.{h,cc}
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 -m venv .tools/venv
.tools/venv/bin/pip install --quiet clang-format==19.1.7 clang-tidy==19.1.0.1
.tools/venv/bin/clang-format --version
.tools/venv/bin/clang-tidy --version | head -2
