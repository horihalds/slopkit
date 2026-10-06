# Known issues

Accepted problems that can turn a full `tools/verify.sh` run red for a reason
other than a regression. The definition of done (`AGENTS.md`) tolerates a flaky
failure only when it is listed here **and** the test passes alone under
`ctest -R`; a newly accepted flake is added here in the same commit.

## How to use this file

When a test fails during a full run:

1. Rerun just that test under the same 60 s CTest timeout, using its exact name:

   ```sh
   ctest --test-dir build -R "<test name>" --output-on-failure
   ```

   (`tools/verify.sh -R "<test name>"` does the same after building.)
2. If it passes alone and appears below, the full-run failure is the known flake —
   note it and move on; there is nothing to debug.
3. If it fails alone, or is not listed below, it is a regression: debug it. Do
   not add it here just to make a run green.

## Known flaky tests

### `the memory viewer resolves an instruction's memory operands`

- **Source:** `tests/ui/dialogs/memory_viewer_test.cpp`, CTest tag `[ui]`.
- **Symptom:** intermittent failure in a full or otherwise load-heavy run; the
  same test passes alone, well inside the 60 s timeout (~0.06 s observed).
- **Cause:** the test waits for the asynchronous access worker to deliver the
  listing's live pass by pumping the Qt event loop a fixed number of times
  (`QCoreApplication::processEvents()` in a loop of 8) instead of waiting on the
  reading. Under CPU load the worker has not delivered when the test reads the
  request, so the seeded listing has no code base and the operand `CHECK`s fail.
- **Lone-run check:** `ctest --test-dir build -R "the memory viewer resolves an instruction's memory operands" --output-on-failure`
- **Status:** accepted flake, not yet fixed. The real fix is to wait on the live
  reading instead of pumping a fixed number of times.

## Not a flake

A failure that is not listed above — or that also fails alone — is a regression.
Do not add it here to silence it.
