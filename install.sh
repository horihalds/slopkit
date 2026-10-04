#!/usr/bin/env bash
set -euo pipefail

# Configures, builds and installs slopkit together with its desktop entry and
# hicolor icons. ImageMagick must be installed: the icon is generated from
# assets/icons/icon.svg at configure time.
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

# Finish the `.skt` registration: refresh the per-user MIME and desktop
# databases and make slopkit the default handler. Every tool is optional. A
# system prefix is left to the administrator, so another user's configuration
# is never written here.
case "$prefix" in
    "$HOME"/*)
        if command -v update-mime-database >/dev/null 2>&1; then
            update-mime-database "$prefix/share/mime"
        fi
        if command -v update-desktop-database >/dev/null 2>&1; then
            update-desktop-database "$prefix/share/applications"
        fi
        if command -v xdg-mime >/dev/null 2>&1; then
            xdg-mime default slopkit.desktop application/x-slopkit-table
        else
            echo "Install xdg-mime to make slopkit the default .skt handler." >&2
        fi
        ;;
    *)
        echo "To register .skt files with this system-wide prefix, run:"
        echo "  update-mime-database $prefix/share/mime"
        echo "  update-desktop-database $prefix/share/applications"
        ;;
esac

echo "Installed slopkit into: $prefix"
