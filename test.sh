#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$root_dir/build"
launcher_socket="drkonqi-coredump-launcher.socket"

"$root_dir/build.sh"

# A failing case may crash the test binary, and KDE's crash reporter turns every
# crash of a process the user owns into a desktop notification: systemd-coredump
# hands the dump to `drkonqi-coredump-processor`, which starts
# `drkonqi-coredump-launcher` on this socket and that shows the popup. The
# launcher has no per-process opt-out (`KDE_DEBUG` only skips the DrKonqi
# dialog, `KDE_COREDUMP_NOTIFY=1` only switches to the developer notification,
# and `ulimit -c 0` does not stop the dump either), so park its socket for the
# length of the run and restore it afterwards. The crashes still land in the
# journal and in `coredumpctl`. Set SLOPKIT_TEST_NOTIFY=1 to keep them.
parked=0

restore_launcher()
{
    if [ "$parked" -eq 1 ]; then
        systemctl --user unmask --runtime "$launcher_socket" 2>/dev/null || true
        if systemctl --user start "$launcher_socket" 2>/dev/null; then
            echo "Crash notifications restored."
        else
            echo "Start $launcher_socket to get crash notifications back."
        fi
    fi
}

trap restore_launcher EXIT INT TERM

if [ "${SLOPKIT_TEST_NOTIFY:-0}" != "1" ] && systemctl --user cat "$launcher_socket" >/dev/null 2>&1; then
    if systemctl --user mask --runtime "$launcher_socket" >/dev/null 2>&1; then
        if systemctl --user stop "$launcher_socket" >/dev/null 2>&1; then
            parked=1
            echo "Crash notifications parked for this run."
        fi
    fi
fi

ctest --test-dir "$build_dir" --output-on-failure "$@"
