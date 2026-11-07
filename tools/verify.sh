#!/usr/bin/env bash
set -euo pipefail

# One-shot definition-of-done check: clang-format, a warning-only build and the
# CTest suite, run in a single pass. Every stage writes its full transcript to
# its own log under build/ and prints a short status line, so the terminal stays
# readable. A failing stage never aborts the run — the remaining stages still
# execute — and the exit status is non-zero if any stage failed.
#
# Run from any directory:
#   tools/verify.sh [ctest args...]
#
# Extra arguments are forwarded to ctest through test.sh, so a targeted run
# reuses the same entry point:
#
#   tools/verify.sh -R "widgets follow a theme switch"

# tools/ sits one level below the repository root; resolving it here keeps the
# script working from any working directory.
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "$script_dir/.." && pwd)"
build_dir="$root_dir/build"

# build/ is gitignored. Create the log directory up front: configure.sh/build.sh
# create the rest of it, but the format stage runs before any of them and must
# also work on a fresh tree.
mkdir -p "$build_dir"

format_log="$build_dir/verify-format.log"
build_log="$build_dir/verify-build.log"
tests_log="$build_dir/verify-tests.log"
# ctest writes this only when a test fails; the test stage reads it back.
failed_log="$build_dir/Testing/Temporary/LastTestsFailed.log"

# Per-stage status (ok / FAILED / skipped) and a one-line detail for the
# summary. Accumulated instead of raised so that every stage runs even when an
# earlier one failed.
status_format=ok
detail_format="no formatting drift"
status_build=ok
detail_build="no warnings"
status_tests=ok
detail_tests=""

# Runs one stage, capturing stdout+stderr into its log and returning the
# command's status. Used in an `if` so a failing stage is recorded instead of
# letting `set -e` abort the run.
run_logged()
{
    local log_file="$1"
    shift
    "$@" >"$log_file" 2>&1
}

# --- format ----------------------------------------------------------------
#
# Checks the same file set as `SLOPKIT_FORMAT_SOURCES` in CMakeLists.txt — every
# src/** and tests/** .hpp/.cpp file; keep the two in sync. Running clang-format
# directly, rather than the registered `clang-format-check` test, keeps this
# stage independent of a configured build directory.
if ! command -v clang-format >/dev/null 2>&1; then
    # Mirror the CMake guard: `clang-format-check` is only registered when
    # clang-format is on the PATH, so a missing formatter skips instead of fails.
    status_format=skipped
    detail_format="clang-format not on PATH"
    echo "format  skipped   clang-format not on PATH"
else
    mapfile -t format_files < <(find "$root_dir/src" "$root_dir/tests" \
        -type f \( -name '*.hpp' -o -name '*.cpp' \) | sort)

    if run_logged "$format_log" clang-format --dry-run --Werror "${format_files[@]}"; then
        echo "format  ok        no formatting drift"
    else
        status_format=FAILED
        # clang-format prints one `<file>:<line>:<col>: error:` line per
        # violation; the leading path points at each file that needs formatting.
        format_offenders="$(sed -n 's/^\([^:]*\):[0-9][0-9]*:[0-9][0-9]*:.*/\1/p' "$format_log" | sort -u)"
        format_offender_count="$(grep -c . <<<"$format_offenders" || true)"
        detail_format="$format_offender_count unformatted file(s)"
        echo "format  FAILED    $detail_format:"
        sed "s|^${root_dir}/||; s|^|                  |" <<<"$format_offenders"
        echo "                  format with: clang-format -i <files>"
    fi
fi

# --- build -----------------------------------------------------------------
#
# Goes through build.sh so an unconfigured tree configures itself first and the
# build command is defined in exactly one place. The full transcript stays in
# the log; only diagnostics reach the terminal.
if run_logged "$build_log" "$script_dir/build.sh"; then
    build_command_failed=0
else
    build_command_failed=1
fi

build_warning_count="$(grep -c 'warning:' "$build_log" || true)"
build_error_count="$(grep -c 'error:' "$build_log" || true)"
build_diagnostics="$(grep -E '(warning|error):|FAILED' "$build_log" || true)"

# Green means the build succeeded *and* was warning-free: the definition of done
# asks for zero warnings, so a warning alone fails this stage.
if [ "$build_command_failed" -ne 0 ] || [ "$build_warning_count" -ne 0 ] || [ "$build_error_count" -ne 0 ]; then
    status_build=FAILED
fi
detail_build="$build_warning_count warning(s), $build_error_count error(s)"
echo "build   $status_build  $detail_build"
if [ -n "$build_diagnostics" ]; then
    printf '%s\n' "$build_diagnostics"
elif [ "$status_build" = FAILED ]; then
    # A failure with no `warning:`/`error:`/`FAILED` line (a configure or link
    # error) would otherwise stay silent; show the tail of the log instead.
    tail -n 20 "$build_log"
fi

# --- tests -----------------------------------------------------------------
#
# Through test.sh, which parks KDE's crash notifications for the run and hands
# the extra arguments to ctest. Drop any stale failure log first so the summary
# only ever reflects this run.
rm -f "$failed_log"
test_command_ok=0
if run_logged "$tests_log" "$script_dir/test.sh" "$@"; then
    test_command_ok=1
fi

if grep -q 'No tests were found' "$tests_log"; then
    # ctest exits 0 when a filter selects nothing but still prints this line; a
    # filter that matches no test is a mistake, not a pass.
    status_tests=FAILED
    detail_tests="no tests matched"
    echo "tests   FAILED    $detail_tests"
elif [ "$test_command_ok" -eq 1 ]; then
    tests_total="$(sed -n 's/.*out of \([0-9][0-9]*\).*/\1/p' "$tests_log" | tail -n 1)"
    detail_tests="${tests_total:-all} passed"
    echo "tests   ok        $detail_tests"
else
    status_tests=FAILED
    if [ -s "$failed_log" ]; then
        # One `<test id>:<name>` line per failure; the name is what is needed.
        failed_tests="$(sed -n 's/^[0-9][0-9]*://p' "$failed_log" | sort -u)"
        failed_names="$(paste -sd, - <<<"$failed_tests" | sed 's/,/, /g')"
        failed_count="$(grep -c . <<<"$failed_tests" || true)"
        detail_tests="$failed_count failed: $failed_names"
    else
        # ctest wrote no failure list: it aborted before running a test. Fall
        # back to the log so the failure is never reported as an empty list.
        detail_tests="$(grep -v '^[[:space:]]*$' "$tests_log" | tail -n 1)"
    fi
    echo "tests   FAILED    $detail_tests"
fi

# --- summary ---------------------------------------------------------------
#
# One short block: per-stage status plus the log holding the full transcript.
summary()
{
    echo "============================== verify =============================="
    printf 'format  %-7s %-44s (%s)\n' "$status_format" "$detail_format" "${format_log#"$root_dir"/}"
    printf 'build   %-7s %-44s (%s)\n' "$status_build" "$detail_build" "${build_log#"$root_dir"/}"
    printf 'tests   %-7s %-44s (%s)\n' "$status_tests" "$detail_tests" "${tests_log#"$root_dir"/}"
    echo "==================================================================="
}

echo
summary

# Any failed stage (skipped is not a failure) makes the whole run fail.
exit_code=0
for stage_status in "$status_format" "$status_build" "$status_tests"; do
    if [ "$stage_status" = FAILED ]; then
        exit_code=1
    fi
done
exit "$exit_code"
