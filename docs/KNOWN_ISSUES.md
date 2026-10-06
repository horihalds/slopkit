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

### `the debug session gate attaches after consent and waits for running`

- **Source:** `tests/ui/debug_session_test.cpp`, CTest tag `[ui][debug_session]`.
- **Symptom:** intermittent `CHECK(backend.count("attach") == 1)` failure
  (`0 == 1`) in a load-heavy full run, even though the preceding
  `REQUIRE(backend.attached_pid.has_value())` and pid `CHECK` already passed; the
  same test passes alone (~0.02 s observed) and the suite went green on re-run.
- **Cause:** `FakeDebugBackend` records its `calls` and attachment state on the
  debug worker thread while the test reads them on its own thread without
  synchronization, so under load the read can miss the recorded `"attach"` call.
- **Lone-run check:** `ctest --test-dir build -R "the debug session gate attaches after consent and waits for running" --output-on-failure`
- **Status:** accepted flake, not yet fixed. The real fix is to synchronize the
  fake backend's recorded state (or wait on the recorded call).

### `controller captures the registers of a running target invisibly`

- **Source:** `tests/debug/controller_test.cpp`, CTest tag `[debug][controller]`.
- **Symptom:** one-off `SIGSEGV` in a load-heavy full run (`test #44` in a 534-test
  run); the same test passes alone (~0.01 s observed) and the next full run went
  green.
- **Cause:** the test clears and reads `FakeDebugBackend::calls` on its own thread
  while the debug worker thread records into the same container without
  synchronization — the same unsynchronized-fake root cause as the `debug session
  gate` flake above.
- **Lone-run check:** `ctest --test-dir build -R "controller captures the registers of a running target invisibly" --output-on-failure`
- **Status:** accepted flake, not yet fixed. The real fix is to synchronize
  `FakeDebugBackend`'s recorded calls (or stop clearing them while a session runs).

## Not a flake

A failure that is not listed above — or that also fails alone — is a regression.
Do not add it here to silence it.
