#!/usr/bin/env bash
set -euo pipefail

# Configures, builds and installs slopkit together with its desktop entry and
# hicolor icons. ImageMagick must be installed: the icon is generated from
# data/icon.svg at configure time.
#
# The install prefix defaults to ~/.local and can be overridden:
#   PREFIX=/usr/local ./install.sh
# It is applied at configure time because the desktop entry bakes the absolute
# path of the installed binary.

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$root_dir/build"
prefix="${PREFIX:-$HOME/.local}"

echo "Configuring CMake/Ninja build in: $build_dir (install prefix: $prefix)"
cmake -G Ninja -B "$build_dir" -DCMAKE_INSTALL_PREFIX="$prefix" "$root_dir"
cmake --build "$build_dir"
cmake --install "$build_dir"
echo "Installed slopkit into: $prefix"
