#!/usr/bin/env bash
set -euo pipefail

# Configures the CMake/Ninja build tree in build/.
#
# Run from any directory:
#   tools/configure.sh

# tools/ sits one level below the repository root; resolving it here keeps the
# script working from any working directory.
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "$script_dir/.." && pwd)"
build_dir="$root_dir/build"

echo "Configuring CMake/Ninja build in: $build_dir"
cmake -G Ninja -B "$build_dir" "$root_dir"
echo "Configuration complete."
echo "Run tools/build.sh to build." >&2
