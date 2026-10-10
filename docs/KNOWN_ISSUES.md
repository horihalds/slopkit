# Known issues

Accepted problems that can turn a full `tools/verify.sh` run red for a reason
other than a regression. The definition of done (`AGENTS.md`) tolerates a flaky
failure only when it is listed here **and** the test passes alone under
`ctest -R`; a newly accepted flake is added here in the same commit.

## How to use this file

When a test fails during a full run, rerun it alone with its exact name and
follow the procedure in [`docs/TESTING.md`](docs/TESTING.md). If it passes alone
and is listed below, the failure is the known flake — note it and move on; there
is nothing to debug. If it fails alone, or is not listed here, it is a
regression: debug it, do not add it just to make a run green.

## Known flaky tests

### breakpoints window adds, removes and clears breakpoints

- **Source:** `tests/ui/dialogs/breakpoints_test.cpp`
  (`[ui][dialogs][breakpoints]`).
- **Symptom:** `CHECK(dialog.model()->rowCount() == 1)` and
  `CHECK(controller.breakpoints().size() == 1)` fail after
  `remove_button()->click()`, reporting two rows and two controller entries.
- **Cause:** an earlier case in the shared `[ui]` run leaves breakpoints in the
  process-wide `DebugController`, so removing the selected row drops a different
  entry than the test expects. It reproduces on a clean checkout.
- **Lone-run check:** `ctest -R "breakpoints window adds, removes and clears
  breakpoints"` passes.

## Not a flake

A failure that is not listed above — or that also fails alone — is a regression.
Do not add it here to silence it.
