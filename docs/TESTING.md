# Testing and toolchain

How to configure, build, install, test and diagnose this project. This file owns
every command an agent runs; [`docs/ARCHITECTURE_MAP.md`](docs/ARCHITECTURE_MAP.md)
owns where the code lives and the conventions, [`README.md`](README.md) is the
human landing page, and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) explains
the plugin model and the debugger. Read this file before build, test or
verification work.

## Configure and build

```sh
cmake -G Ninja -B build
cmake --build build
```

The convenience scripts under `tools/` work from any directory:

| Script | Purpose |
| --- | --- |
| `tools/configure.sh` | Configure the CMake/Ninja build tree in `build/`. |
| `tools/build.sh` | Build, configuring `build/` first when it is not configured yet. |
| `tools/install.sh` | Configure, build and install into `PREFIX` (default `~/.local`), including the desktop entry, hicolor icons and the `.skt` registration. |
| `tools/test.sh` | Build and run the CTest suite with KDE crash notifications parked. |
| `tools/verify.sh` | One-shot definition-of-done check: format, warning-only build and the summarized test run. |

Targets compile for the host CPU's full instruction set by default
(`-march=native -mtune=native`); pass `-DSLOPKIT_NATIVE=OFF` to CMake for a
portable build targeting the compiler's default baseline.

Debug info is compressed so the artifacts stay small while remaining debuggable:
`-gz=zstd` is preferred and falls back to `-gz=zlib` when the host debugger
cannot read zstd. Pass `-DSLOPKIT_COMPRESS_DEBUG=OFF` for uncompressed DWARF.
`tools/install.sh` additionally strips the installed executables and plugins,
moving their debug info into sidecars (a sibling `<name>.debug` for the
executables, a `.debug/` sub-directory for the plugins), so `gdb` on an installed
binary still resolves source lines. Pass `-DSLOPKIT_STRIP_INSTALL=OFF` to install
them verbatim.

Lua, sol2 and Zydis are not system packages: the first configure fetches them
through `FetchContent` (see `cmake/Dependencies.cmake`) — sol2 and Zydis from
their repositories at pinned revisions, Lua from the official 5.4.7 release
tarball pinned by its SHA-256 — so it needs network access and the downloads land
in `build/_deps`, which the Windows trainer cross-build reuses. Qt 6 comes from
the system packages and is linked by every Linux UI binary, trainer included.
The Windows trainer template is a nested mingw cross-build
(`cmake/TrainerTemplate.cmake`) that builds the same runtime and the script
engine, with the Windows target backend and the native Win32 window
(`gui/window_win32.cpp`) in place of the Linux backend and the Qt window, into
`slopkit-trainer.exe`. It needs `x86_64-w64-mingw32-gcc`/`-g++` and no Qt kit at
all; without the compilers, configure warns and only the Windows trainer target
becomes unavailable.

## Run the tests

The suite is Catch2, wired into CTest:

```sh
./tools/test.sh
ctest --test-dir build --output-on-failure
```

For a targeted run, ctest sets `QT_QPA_PLATFORM=offscreen` and the 15 s timeout
for you:

```sh
ctest --test-dir build -R "<exact or partial TEST_CASE name>" --output-on-failure
```

The binary can also be run directly, with the offscreen platform set yourself:

```sh
QT_QPA_PLATFORM=offscreen ./build/slopkit_tests "name1,name2,*glob*"
```

- Multiple names are comma-separated, never space-separated; `*` globs are
  allowed.
- Never run broad tags like `[ui]` through the binary directly: the full set
  exceeds the tool timeout. Use `./tools/test.sh`.

## What earns a test

A case earns its place only when a plausible defect — a change to slopkit's own
logic — would make it fail. Judge a case by that question, not by its size. A
case is removed when every assertion in it falls under one of four criteria:

- **Framework behavior** — it asserts Qt, Zydis, Lua or sol2 behavior rather than
  slopkit's; only that library changing would fail it.
- **Duplicated coverage** — another case already fails if the behavior breaks.
- **Trivial accessors/constants** — it restates a literal, an enum name, or a
  one-line accessor.
- **Over-specified internals** — a harmless refactor would fail it while the
  behavior is unchanged.

A case that mixes valuable and low-value assertions is **trimmed** — drop the
low-value sections/checks — not deleted wholesale; delete only when every
assertion falls under a criterion. Tests that model a documented failure mode
stay: the regressions in [`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md), the
hazards [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) describes, and the hidden
wine-agent cases below.

A forked test child that must be seen making progress — a spinning counter, a
request/answer flag — publishes it through a `volatile` (or atomic) access, not
a plain field. At `-O2` an endless loop whose only side effect is a plain store
may lose that store, so the parent never sees movement and times out. The
symptom is `the target stopped making progress` or an `await_counter` that never
answers, reproducibly and with no product code involved;
`tests/plugin/linux_proc_alloc_test.cpp` carries the idiom.

## The Wine resident agent

The agent is a cross-compiled PE, so it is part of the normal build: the
`slopkit-agent` target cross-compiles the DLL and the injector for both
bitnesses into `build/plugins` — the same directory the `wine-proton` plugin is
loaded from — with `x86_64-w64-mingw32-gcc` and `i686-w64-mingw32-gcc`. Without
those compilers the build copies a prebuilt pair from
`src/plugins/wine_proton/agent/prebuilt/<bitness>`; with neither it fails with an
actionable message. [`docs/WINE_AGENT.md`](docs/WINE_AGENT.md) owns the design,
the measurements and the known limits.

Two of the three agent tests run in the default suite:

```sh
ctest --test-dir build -R "agent" --output-on-failure
```

- the protocol layout test, which pins `sizeof`/`offsetof` for the shared header
  and the little-endian field encoding both sides compile;
- the transport test, which serves the real wire protocol from an in-process
  mock agent and covers the handshake, every operation, a refused request and a
  timeout.

The last two are hidden and environment-gated, because they need a working wine
prefix as well as the cross-compiled probe. `catch_discover_tests` leaves hidden
tests out of ctest, so they are selected by tag on the binary:

```sh
SLOPKIT_WINE_E2E=1 ./build/slopkit_tests "[wine-proton][agent]"
```

The first starts `build/agent_probe/slopkit-probe-x86_64.exe` under wine, injects
the agent and asserts `alloc`, `alloc_near`, `query` (the block must report
`MEM_COMMIT` + `PAGE_EXECUTE_READWRITE`, which the ptrace mapping does not),
`protect`, a full region walk (`ENUM`), a full module walk (`MODULES`) that names
the probe itself as the main image plus `kernel32.dll` and `ntdll.dll`, `free` and
`shutdown`. On a warm prefix the whole run takes about 0.65 s.

The second reproduces the Proton case, where the game is started *by another Wine
process*: it runs the probe as `wine cmd /c <probe>`, finds the process wine
handed its wineserver socket to, and asserts both that the environment the
injector runs with has no `WINESERVERSOCKET` and that injection, `alloc` and
`shutdown` still work. It is the regression test for the reported
`wine: Bad server socket <fd> : Bad file descriptor` failure: without the filter
that same check fails with that exact message.

The generated trainer's Wine target handling is gated the same way, and is not an
agent case:

```sh
SLOPKIT_WINE_E2E=1 ./build/slopkit_tests "the trainer attaches to a wine target by its guest name"
```

It runs the probe under wine and points a generated trainer at it by the probe's
own executable name, which is longer than the kernel's fifteen-character `comm`, so
that name can only come from the guest's images. The trainer must find that pid and
report the guest's width (`8-byte pointers`); whether it may then read the guest is
the kernel's ptrace policy, so the row has to show an address inside the guest image
either way.

A second case runs a generated *Windows* trainer under wine against the same probe,
because a Windows target is the only kind the trainer can map memory in:

```sh
SLOPKIT_WINE_E2E=1 ./build/slopkit_tests "a windows trainer maps a script's slot within reach of its site"
```

Its script `alloc`s a slot near the site it found, `alloc`s a second mapping beside
it and assembles `MOV qword ptr [<slot>], RBX` there, then writes a value through the
slot, reads it back and frees both — the encoder accepts the instruction only while
the two mappings are within a signed 32-bit displacement of each other.
The trainer's log must show `alloc: mapped 8 byte(s)` and the slot must give the
mapping up on `dealloc`, which is the rule a slot borrowed from the guest's own
zero-initialized data broke.

## Manually verify allocation against a Proton game

The allocation window's blast radius against a real Proton target cannot be
reproduced in a unit test, so it has a manual recipe; the suite models the hazard
with a multi-threaded child instead (see
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)).

Use the script the reporter used and drive it from the script panel:

```lua
function activate()
    alloc("test_12223", 8)
    return true
end

function deactivate()
    dealloc("test_12223")
    return true
end
```

With Dying Light The Beast running under GE-Proton11:

1. Point slopkit at the `wine-proton` process, tick the script's row and wait for
   `activated: ok`.
2. Wait a few seconds with the game running.
3. Untick the row and wait for `deactivated: ok`.
4. Repeat the tick/untick cycle at least five times; the game must keep running.

Check three logs after the run:

- `~/.local/state/slopkit/slopkit.log` — every cycle shows `activated: ok` and
  `deactivated: ok`, and the window's `script`-category debug records name the
  borrowed thread id.
- The Proton log (`steam-<appid>.log`; the reporter's was
  `/home/user/steam-3008130.log`) — it must not end with
  `warn:seh:dispatch_exception backtrace: --- Exception 0x80000003`. That
  `EXCEPTION_BREAKPOINT` in `engine_core_x64_rwdi.dll` was the crash signature the
  single-thread window removes.
- The per-run game log under the prefix's
  `drive_c/users/steamuser/Documents/Dying Light The Beast/out/logs/crash_<stamp>.log`
  — a new `crash_*` file means the run still died.

A `wine-proton` target no longer borrows a target thread to allocate: the agent
does the work, so the refusal above cannot come from this recipe. A failure now
comes from the agent (injection, the handshake or a request) and is reported as
the `alloc`/`dealloc` error, with the game left running. Opening the same game
through `linux-proc` still uses the ptrace path and its refusal, because a live
group's leader is never borrowed and a thread procfs already reports inside a
system call is not even stopped, so neither can produce the crash signature above.

### With the resident agent

`alloc`/`dealloc` on a `wine-proton` target are served by the resident agent
alone, so the log says whether the agent came up and a failure is a visible
error rather than a silent fallback. Watch the `process`-category records:

1. The first `alloc` for the game logs
   `the resident agent is serving pid <linux pid>: wine pid <windows pid>, ...`;
   if injection or the handshake fails it logs
   `... agent for pid <linux pid> is off for this session (<reason>); allocation is unavailable`
   and the `alloc` call fails.
2. Every later call is served by the agent and logs `... through the resident agent`;
   after a failure the agent is not retried and the calls fail with the reason
   from step 1 instead of falling back to ptrace.
3. After the tick/untick cycles, detach (closing the session or switching away)
   must log no timeout: `SHUTDOWN` stops the agent and frees what it allocated.
4. No `slopkit-agent-*` staging directory is left behind: the one holding the
   region, the DLL and the injector is removed on detach. It is created in the
   first of `$XDG_RUNTIME_DIR`, the temporary directory and
   `$HOME/.cache/slopkit` that the target's own namespace can see (see
   [`docs/WINE_AGENT.md`](docs/WINE_AGENT.md)), so check whichever of the three a
   containerised game actually shares.

`protect` and `query` have no plugin-ABI entry, so this recipe cannot drive them;
the wine-gated test above does.

For a **32-bit game** the same steps apply: the plugin reads the guest's COFF
machine and stages `slopkit-agent-i686.dll` with `slopkit-injector-i686.exe`
instead. No second host architecture is involved — the i686 agent is a PE for a
32-bit Windows process.

## Manually verify a trainer against a Proton game

The gated case above covers an ordinary wine process; a Steam-launched Proton game
is the real target, and its check is manual because whether one process may read
another's memory is the kernel's decision (`/proc/sys/kernel/yama/ptrace_scope`)
rather than the trainer's:

1. `./build/slopkit generate-trainer --table <table>.skt --target linux --out tmp/trainer`.
2. With the game running, start it by the name the table recorded, which is the
   guest's own executable: `tmp/trainer/<table>.trainer --name <game>.exe --list`.
3. The header must name the game's pid and the guest's width (`8-byte pointers` for
   a 64-bit game, `4-byte` for a 32-bit one), and a value entry naming one of the
   guest's modules must resolve to an address inside that image.
4. A hook script must get past its scan and its allocation:
   `tmp/trainer/<table>.trainer --name <game>.exe --activate <n> --wait 500 --list`
   must leave the script `active` rather than `error`. A pattern that is not found
   and a request no mapping or cave can satisfy are the two ways it stops, and both
   are reported with the reason the script printed — the second as `alloc: the target
   has no free data cave of <size> byte(s) in its writable memory` when even a
   borrowed cave is refused.

   Trigger the hook in game and the game must keep running: a slot the trainer
   borrowed from the image's code page accepts the trainer's own write but not the
   game's, so that is what a crash on trigger used to be. `--verbose` records which
   mapping or cave was handed out, and the two `alloc` lines must name writable
   memory for the script's slot and executable memory for the hook's payload.
5. Rerun any of the above with `--verbose` when a script behaves differently in the
   trainer than in slopkit: the trainer prints the log path on stderr and writes it
   to `<trainer file>.log` beside itself (for step 4,
   `tmp/trainer/<table>.trainer.log`). It must contain the payload's counts, the
   attach line with the target's pid and pointer width, the script's activation and
   each hook's output or failure. The end-to-end `verbose logging writes a log file
   beside the trainer` case covers the same path in the suite.

Verified against Dying Light The Beast under GE-Proton11 through the trainer's own
attachment path (`src/trainer/runtime/target_linux.cpp`): the name lookup found the
guest by `DyingLightGame_TheBeast_x64_rwdi.exe`, its 304 modules were listed with
the guest's bases, and the two bytes at the image base read `MZ` — where the
process's `comm` (truncated to `DyingLightGame_`) and `/proc/<pid>/exe` (a wine
preloader) both would have missed it.

The cave side was checked the same way, against the same game: 234 of its regions are
readable and executable, and asking for a 64-byte cave near a site inside
`d3d12core.dll` returned one in that same module — its section padding, 4.6 MB from
the site and so inside a near jump's reach — whose write probed clean, which is what
shows a Wine PE's code pages accept the trainer's write path at all. That is also why
the probe cannot decide where a script's slot may live: the trainer writes with a
forced read-only page, so only the region's own writable bit matches what the game's
instructions can do, which is what `tests/trainer/caves_test.cpp` pins down.

## Diagnose a build or a run

- An allocation failure names the reason. A failed injector is reported as its
  last `slopkit-injector:` line when it printed one, otherwise as the last
  non-empty line of its output, because a game's own `WINEDEBUG` buries the
  useful line under wine traces.
- `tools/verify.sh` prints only the `warning:`/`error:`/`FAILED` lines of its
  build and keeps the full transcripts in `build/verify-format.log`,
  `build/verify-build.log` and `build/verify-tests.log`. By hand:
  `cmake --build build --target slopkit_tests 2>&1 | grep -E "error:|FAILED|warning:"`.
- Extra arguments are forwarded to ctest, so a targeted run reuses the entry
  point: `./tools/verify.sh -R "<test name>"`.
- The Linux trainer window runs in the suite without a display: `slopkit_tests`
  sets `QT_QPA_PLATFORM=offscreen`, `tests/trainer/gui_test.cpp` checks the
  model's roles/flags and `setData` submissions and opens the window with stub
  session callbacks (asserting a snapshot arrives and the window closes with
  `0`), and `tests/trainer/rows_test.cpp` keeps the printed list equal to the row
  fields. The real session against a live process is still checked by hand:
  `./build/trainer/slopkit-trainer --pid $$` against the shell, in both themes.
- The Windows window has no headless harness of its own. The cross-build proves
  it compiles and links; check its imports after a build with
  `x86_64-w64-mingw32-objdump -p build/trainer/slopkit-trainer.exe | grep 'DLL Name'`
  — it must list only system DLLs (`COMCTL32`, `GDI32`, `KERNEL32`, `msvcrt`,
  `USER32`), never a `Qt6*.dll` — and exercise the window under Wine by hand.

## Formatting

- Format the files you touched before building: `clang-format -i <files>`
  (installed 23.x rules from `.clang-format`).
- The `clang-format-check` CTest test covers every `src/**/*.hpp`,
  `src/**/*.cpp`, `tests/**/*.hpp` and `tests/**/*.cpp` file. It is registered
  only when `clang-format` is on the `PATH`, so a missing formatter skips the
  check instead of failing the build.

## Crash notifications

KDE notifies about every crash of a process the user owns: `systemd-coredump`
hands the dump to `drkonqi-coredump-processor`, which starts
`drkonqi-coredump-launcher` on its user socket and that shows the popup. The
launcher has no per-process opt-out (`KDE_DEBUG` only skips the DrKonqi dialog,
`KDE_COREDUMP_NOTIFY=1` only switches to the developer notification, and
`ulimit -c 0` does not stop the dump), so run the suite through `./tools/test.sh`,
which parks that socket for the run and restores it afterwards; the crashes still
land in the journal and in `coredumpctl`. Set `SLOPKIT_TEST_NOTIFY=1` to keep the
notifications while debugging a crash. A bare `ctest` parks nothing.

## Flaky failures

A test that fails in a full run but passes alone may be a known flake. Rerun it
alone with its exact name:

```sh
ctest --test-dir build -R "<test name>" --output-on-failure
```

If it passes alone, check [`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md) — it owns
the accepted flakes and their lone-run commands. Add a newly accepted flake there
in the same commit; a failure that also happens alone is a regression, not a flake.

When the failure looks scheduling-dependent, prove it instead of guessing.
`./tools/flake-check.sh "<test name>" [runs] [--load]` builds, then repeats the
case with the run pinned to one CPU (`taskset -c 0`), optionally sharing that CPU
with a busy loop. It stops at the first failure, keeps that transcript in
`tmp/flake-check-<name>.log` and exits non-zero, so a few hundred repeats are the
normal way to show a fix holds; it is a manual tool and is not registered with
CTest, which runs each case once.

A case whose reported `Test time` equals the timeout did not hang: its process
crashed and left a forked child holding CTest's output pipe open, so CTest kept
draining the pipe until the timeout expired. Check
`coredumpctl list | grep slopkit_tests` for the core that names the case, then
rerun it alone as above.

## Definition of done

`./tools/verify.sh` is the one-shot check; its three stages are:

- **format** — `clang-format --dry-run --Werror` over every `src/**` and
  `tests/**` `.hpp`/`.cpp` file (`skipped` when `clang-format` is missing).
- **build** — a warning-free build; a warning alone fails the stage.
- **tests** — the whole CTest suite, including `clang-format-check` and
  `docs-links-check` (the link/anchor/hygiene check in `tools/docs-check.sh`).

All three green is the definition of done; the exit status is non-zero when any
stage failed.
