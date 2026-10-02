#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$root_dir/build"

echo "Configuring CMake/Ninja build in: $build_dir"
cmake -G Ninja -B "$build_dir" "$root_dir"
echo "Configuration complete. Run ./build.sh to build."
