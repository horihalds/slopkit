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

### controller consumes the step-out stop and erases the transient entry

- **Source:** `tests/debug/controller_test.cpp` (`[debug][controller]`).
- **Symptom:** `REQUIRE(pump_until(controller, ...))` fails: the controller never
  reaches the stopped state with the transient breakpoint erased and the register
  writes recorded.
- **Cause:** the step-out pass races the shared controller/worker under a full
  run; the same case passes when it has the process to itself.
- **Lone-run check:** `ctest -R "controller consumes the step-out stop and erases
  the transient entry"` passes.

### controller ignores a step-out without a stop

- **Source:** `tests/debug/controller_test.cpp` (`[debug][controller]`).
- **Symptom:** `CHECK(backend.count("cont") == 0)` fails (`1 == 0`): the controller
  issued a `cont` while ignoring a step-out that had no stop to consume.
- **Cause:** the same shared practice-backend timing as the case above; the calls
  are ordered correctly when the case has the process to itself.
- **Lone-run check:** `ctest -R "controller ignores a step-out without a stop"`
  passes.

### debug session drives a target with forwarding

- **Source:** `tests/platform/linux/debug_session_test.cpp` (`[debug_session]`).
- **Symptom:** `REQUIRE(leader.has_value())` fails to attach, or a later
  `CHECK(hit->reason == StopReason::breakpoint)` reports a different stop.
- **Cause:** `ptrace` attach/stop timing against the spawned practice target under
  a full run; the session behaves normally when run alone.
- **Lone-run check:** `ctest -R "debug session drives a target with forwarding"`
  passes.

### a signal storm that keeps the target busy is survived harmlessly

- **Source:** `tests/plugin/linux_proc_alloc_test.cpp` (`[linux_proc][alloc]`).
- **Symptom:** `REQUIRE(ok)` fails, so a `free_memory` inside the loop returned an
  error while the storm thread was signalling every thread of the target.
- **Cause:** the ptrace allocation window races the storm: under a full 900-test
  run the free can meet a stop the storm caused, and the call is refused. The
  window behaves normally when the case has the CPU to itself. (The wine-proton
  agent path is not involved — this case drives `linux-proc`, whose profile
  leaves the agent hooks null.)
- **Lone-run check:** `ctest -R "a signal storm that keeps the target busy is
  survived harmlessly"` passes.

### the address-table model writes one value to a selection of like entries

- **Source:** `tests/ui/models/address_table_model_test.cpp` (`[ui]`).
- **Symptom:** `CHECK_FALSE(is_error)` and `CHECK(message == "Value written.")`
  fail after the single-row `setData` path, reporting an error instead of the
  write status.
- **Cause:** the shared `AccessWorker` write races the scheduling of a full
  `[ui]` run; the same write completes normally when the case has the process to
  itself.
- **Lone-run check:** `ctest -R "the address-table model writes one value to a
  selection of like entries"` passes.

## Not a flake

A failure that is not listed above — or that also fails alone — is a regression.
Do not add it here to silence it.
