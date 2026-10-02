#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$root_dir/build"

if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    echo "No configured build directory found; running configure.sh first."
    "$root_dir/configure.sh"
fi

cmake --build "$build_dir"
echo "Build complete: $build_dir/slopkit"
