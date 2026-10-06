#!/usr/bin/env bash
set -euo pipefail

# Builds the project, configuring build/ first when it is not configured yet.
#
# Run from any directory:
#   tools/build.sh

# tools/ sits one level below the repository root; resolving it here keeps the
# script working from any working directory.
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "$script_dir/.." && pwd)"
build_dir="$root_dir/build"

if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    echo "No configured build directory found; running tools/configure.sh first." >&2
    "$script_dir/configure.sh"
fi

cmake --build "$build_dir"
echo "Build complete: $build_dir/slopkit"
