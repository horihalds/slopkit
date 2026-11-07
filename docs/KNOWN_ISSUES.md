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

Two facts decide most of these cases before any debugging:

- Every Catch2 case runs as its own CTest process (`catch_discover_tests`, see
  `CMakeLists.txt`), so no case can be disturbed by another case's state. A cause
  that blames a shared, long-lived object such as the process-wide controller is
  wrong here.
- A job's side effect and its completion are two steps, and only the drained
  completion produces what a test asserts; see the drain gotcha in
  [`docs/ARCHITECTURE_MAP.md`](docs/ARCHITECTURE_MAP.md).

`tools/flake-check.sh` repeats one case under CPU contention, which is how a
scheduling-dependent cause is proven rather than guessed; the recipe is in
[`docs/TESTING.md`](docs/TESTING.md).

## Known flaky tests

None. The entries that used to live here were each fixed at its root cause:

| Case | What it really was |
| --- | --- |
| `breakpoints window adds, removes and clears breakpoints` | No defect at all: the case is fully synchronous (no worker, no timer), and hundreds of single-CPU repeats under load never failed. The recorded cause — another `[ui]` case leaking breakpoints into a shared controller — cannot happen, because every case has its own process. |
| `controller consumes the step-out stop and erases the transient entry` | The test raced the controller: the step-out's `cont` was returned immediately, so the stop arrived and the controller was back to stopped inside one drain, and the wait for `running` could miss it. The wait now parks that `cont` in the backend, which makes the running state stable. |
| `controller ignores a step-out without a stop` | The test cleared the backend's call log while the start's parked `cont` was still unrecorded, so the `cont` it then saw and rejected was that one, not a new one. The wait now includes the recorded `cont`. |
| `controller drops a pending step-out when the session ends` | The test stopped a session whose `cont` was not parked, which left the release flag for the next step-out's `cont` to eat: that `cont` returned an empty stop instead of the queued one. The parked `cont` is now released by the stop itself. |
| `debug session drives a target with forwarding` | The parent seized a freshly forked child with no readiness handshake, so the seize could be refused (`ESRCH` on a task that was not established yet). The child runs first, and the attach retries a bounded number of times. |
| `a signal storm that keeps the target busy is survived harmlessly` | The allocation window forwarded the storm's signals at about one stop per millisecond and livelocked before the planted call could run, so it refused the call. It now polls tightly, and a window that never saw the call's entry is opened again. |
| `the address-table model writes one value to a selection of like entries` | The test waited for the job's memory side effect and then read the status, which the worker's *undelivered* completion sets — so it read the previous job's status. The shared wait helpers now drain to quiescence first. |
| `the address list changes the value of every selected entry` | The same completion/effect race, in the batched `change_value()` path. |

## Not a flake

A failure that is not listed above — or that also fails alone — is a regression.
Do not add it here to silence it.
