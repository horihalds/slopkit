#!/usr/bin/env bash
set -euo pipefail

# Repeats one CTest case many times under CPU contention, so a scheduling-
# dependent failure has a chance to surface instead of hiding behind a lucky
# single run. Not registered with CTest: it repeats a case, it does not verify
# it once.
#
# Run from any directory:
#   tools/flake-check.sh <test-name> [runs] [--load]
#
#   <test-name>  the CTest name (or a unique substring), passed to `ctest -R`
#   [runs]       how many repeats (default 200)
#   --load       also start a busy loop on the same CPU, so the case is never
#                the only runnable task
#
# Exits non-zero as soon as any repeat fails, and keeps the first failing
# transcript under tmp/ so the reproduction is not lost.

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "$script_dir/.." && pwd)"
build_dir="$root_dir/build"
tmp_dir="$root_dir/tmp"

name="${1:-}"
if [ -z "$name" ]; then
    echo "usage: tools/flake-check.sh <test-name> [runs] [--load]" >&2
    exit 2
fi
shift

runs=200
load=0
for arg in "$@"; do
    case "$arg" in
    --load) load=1 ;;
    '' | *[!0-9]*) echo "flake-check: bad argument: $arg" >&2; exit 2 ;;
    *) runs="$arg" ;;
    esac
done

"$script_dir/build.sh"
mkdir -p "$tmp_dir"

# Pin the run to one CPU, so every repeat is subject to the same scheduling
# pressure; with --load a busy loop shares that CPU for the whole run.
run_case()
{
    taskset -c 0 ctest --test-dir "$build_dir" --output-on-failure -R "$name"
}

load_pid=""
cleanup()
{
    if [ -n "$load_pid" ]; then
        kill "$load_pid" 2>/dev/null || true
        wait "$load_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

if [ "$load" -eq 1 ]; then
    taskset -c 0 yes >/dev/null 2>&1 &
    load_pid="$!"
fi

fails=0
slug="$(printf '%s' "$name" | tr -c 'a-zA-Z0-9' '-')"
log="$tmp_dir/flake-check.log"
failure_log="$tmp_dir/flake-check-$slug.log"
for ((i = 1; i <= runs; ++i)); do
    if ! run_case >"$log" 2>&1; then
        fails=$((fails + 1))
        cp "$log" "$failure_log"
        echo "flake-check: '$name' failed on repeat $i/$runs" >&2
        break
    fi
done

if [ "$fails" -eq 0 ]; then
    echo "flake-check: '$name' passed $runs/$runs"
    exit 0
fi
echo "flake-check: '$name' failed; transcript: $failure_log" >&2
exit 1
