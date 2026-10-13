#!/usr/bin/env bash
# Formats the C++ sources (tracked and new *.h / *.cc files) with the pinned clang-format (D61).
# Only C++ files: clang-format would happily reflow a CMakeLists.txt as if it were C++.
#
#   scripts/dev/format.sh           # format in place
#   scripts/dev/format.sh --check   # list files that need formatting; exit 1 if any
set -euo pipefail
cd "$(dirname "$0")/../.."
files=$(git ls-files -co --exclude-standard '*.h' '*.cc')
if [ "${1:-}" = "--check" ]; then
  # Capture the output first: with pipefail, `| grep -q` would end clang-format with SIGPIPE on a
  # file with many violations, fail the pipeline, and leave the file unlisted.
  bad=$(for f in $files; do
          out=$(.tools/venv/bin/clang-format --dry-run "$f" 2>&1)
          [[ "$out" == *warning* ]] && echo "$f"
        done || true)
  [ -z "$bad" ] || { echo "$bad"; exit 1; }
else
  .tools/venv/bin/clang-format -i $files
fi
