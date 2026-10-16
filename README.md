# slopkit

slopkit is a reverse-engineering toolset.
It targets Linux and, so that a remembered window position can be restored, runs
its GUI on X11/XWayland when a `DISPLAY` is available; native Wayland is the
fallback, and `QT_QPA_PLATFORM=wayland slopkit` forces it. It is built around a
single rule: every interaction with another process goes through a plugin.

The project is young: the plugin-first process-access ABI and host, two bundled
plugins and the three-zone scanner shell (scan, address list, memory browser),
including the Memory Viewer's live disassembler and its opt-in debugger, exist
today. Address tables are `.skt` files — ZIP archives holding one readable
text file per entry, the row order and the ReClass classes the table carries —
and double-clicking one opens slopkit with that table, while a running instance
offers to cancel, overwrite the open table or merge the file.

## Tools

| Tool | Purpose | Status |
| --- | --- | --- |
| Process list | Browse running processes, see which plugin claims each one, inspect it and attach/detach. | Implemented |
| Memory scanner | Search a target's memory for values and refine the result set with first/next/undo scans, keeping the shown hits up to date with live memory. | Implemented |
| Address list | Track found addresses, edit and freeze their values, watch them refresh live, and save or reopen the table. | Implemented |
| ReClass | Rebuild a target's C/C++ structs and classes: pin a class to an address expression, describe its fields as a typed node list, watch the values refresh live, type a corrected value back, dissect a hex field into the kind its bytes fit and follow pointers into other classes. The classes ride inside the same `.skt` table. | Implemented (ReClass window) |
| Lua scripts | Keep one-off Lua scripts beside the table as script entries, edit them in a Lua-highlighting, line-numbered editor and verify the source before running it, run them against the attached target through a small memory API (`mem.read`, `mem.write`, byte and string reads), publish named 64-bit values with `rsymbol` / `ssymbol` / `usymbol` that any address field then resolves, keep script-local `rlabel` / `slabel` / `ulabel` labels and locate a wildcard byte pattern with `aobscan`, rename one inline from its `Description` cell, and see what they printed and what failed. | Implemented (Add Script / Run Script) |
| Memory browser | Hex dump of live memory around a chosen address, refreshed automatically, beside a live disassembly listing and the Debugger pane. | Implemented |
| Practice target | A separate `slopkit-sandbox` process exposing values of every scan type; launch it from `Help > Launch Practice Target` and attach to practise the scanner, or open a table whose target is `slopkit-sandbox` to have a fresh one launched and attached without a prompt — that table's sandbox is stopped again when slopkit exits while still attached, while a Help-menu or hand-started sandbox keeps running. | Implemented |
| Disassembler | Instruction decoding (Zydis) of live memory. | Implemented (Memory Viewer pane) |
| Debugger | Opt-in instruction-level debugging of the attached target: break, resume, step, editable registers, a call stack, and software and hardware breakpoints with hit counters. | Implemented (Debugger pane plus the Breakpoints window) |
| Access Watch | Record every instruction that writes or accesses a watched address, and resolve an instruction's memory operands from live registers. The "find out what writes/accesses" commands attach the debug session on demand after a confirmation, so the Memory Viewer does not have to be opened first. | Implemented (Access Watch window) |
| Referenced strings | Analyze a loaded module for the ASCII and UTF-16LE strings its code directly references and show each as a comment on the referencing disassembly line, with a filterable, per-module list in the Referenced strings window. | Implemented (Referenced strings window) |
| Module / thread views | Inspect loaded modules and threads of the attached target. | Implemented (Process list detail) |
| Standalone trainer | Turn the open table into self-contained console trainers (`File > Generate Trainer…`) that reproduce its live values, frozen writes and Lua scripts on a machine with no slopkit, Qt or plugin installed; a Linux executable and a 64-bit Windows one are written, each with the table's value entries, scripts and ReClass classes appended to a runtime template rather than a sidecar file. | Implemented (Generate Trainer…) |

## Quick start

Dependencies (from the system repositories): CMake and Ninja, a C++23 compiler,
the libzip development package (e.g. Fedora's `libzip-devel`) with `pkg-config`,
Catch2, Qt 6 Widgets (`Qt6Core`, `Qt6Gui`, `Qt6Widgets`) and ImageMagick
(build-time only, for the icon set). Lua, sol2 and Zydis are not system packages:
CMake fetches them with `FetchContent` — sol2 and Zydis from their repositories
at pinned revisions, Lua from the Lua team's official 5.4.7 release tarball,
pinned by its SHA-256 — so the first configure needs network access and downloads
each once into `build/_deps`, which the Windows trainer template's cross-build
reuses. The
Wine/Proton resident agent is cross-compiled and
needs the mingw-w64 compilers for both bitnesses (`x86_64-w64-mingw32-gcc` and
`i686-w64-mingw32-gcc`); without them the build uses a prebuilt agent under
`src/plugins/wine_proton/agent/prebuilt/<bitness>` and otherwise fails the
`slopkit-agent` target — see [`docs/WINE_AGENT.md`](docs/WINE_AGENT.md). The
Windows trainer template needs the matching C++ cross-compiler
(`x86_64-w64-mingw32-g++` from the same package); without it the build still
succeeds and only the Windows trainer target becomes unavailable.

```sh
./tools/configure.sh
./tools/build.sh
```

The whole definition of done — the format check, a warning-only build and the
summarized test run — is one command:

```sh
./tools/verify.sh
```

Every command and recipe — a targeted test run, diagnostics, formatting, the
known flakes — lives in [`docs/TESTING.md`](docs/TESTING.md).

Run:

```sh
./build/slopkit                    # launches the GUI
./build/slopkit --version          # prints the version
./build/slopkit --list-plugins     # lists discovered plugins and diagnostics
./build/slopkit --list-processes   # lists processes claimed by the loaded plugins
./build/slopkit --scan <pid> 1234  # runs one exact-value scan and prints the hits
```

These flags work headlessly, with no display or GPU required.

### Standalone trainers

`File > Generate Trainer…` turns the open table into executables that carry its
value entries and Lua scripts inside themselves and reproduce their behaviour —
live values, frozen writes and scripted effects — on a machine that has neither
slopkit, Qt nor any plugin installed. The same generation is reachable
headlessly:

```sh
./build/slopkit generate-trainer --table game.skt --target both --out ~/trainer
```

| Flag | Meaning |
| --- | --- |
| `--table <file.skt>` | the table to embed (required) |
| `--target linux\|windows\|both` | which executables to write (default `both`) |
| `--out <directory>` | where they are written (default the current directory) |
| `--name <base>` | the file name stem (default the table file's own name) |

`<base>.trainer` is the Linux executable and `<base>.trainer.exe` the 64-bit
Windows one; each is a copy of a runtime template with the payload appended, so
it is one self-contained file with no sidecars and no plugin. Every produced path
is printed on stdout, a target whose template is missing is reported on stderr
while the other target is still written, and the command exits non-zero only when
no file was produced at all. Script (Lua) entries and the table's ReClass classes
travel in that payload too, so the generated trainer runs the table's scripts
against the target through slopkit's own scripting engine, and the number of
script entries is reported beside the value entries.

A generated trainer attaches to `--pid`, else `--name`, else the process name
recorded when it was generated:

```sh
./game.trainer                            # interactive console, four refreshes a second
./game.trainer --pid 4711 --list          # one-shot: print the rows and exit
./game.trainer --pid 4711 --set 1=250 --freeze 1 --wait 900 --list
./game.trainer --pid 4711 --activate 1 --wait 500 --list
```

The console lists the value entries as `1..N` — index, description, resolved
address, type, value and a frozen marker — and then the scripts as `S1..Sm`, with
each script's state (`idle`, `active` or `error`), tick interval and last
message. `f<N>` toggles freeze, `s <N> <value>` writes a value (hex honouring the
entry's hex flag), `a<N>` activates script `S<N>` and `d<N>` deactivates it (each
running the script's `activate` / `deactivate` hook), `r` re-resolves, `l`
re-lists, `q` quits. The two numberings are independent, so `f1` never touches
`S1`. Frozen entries are re-written on every tick, and an active script's
`update` hook runs on the interval it asks for, else `--interval` (250 ms by
default), both inside `--wait` and in the interactive console. `--activate N` is
repeatable and ticks a script before the requested actions run, so a headless run
starts only the scripts it trusts — scripts always start idle otherwise. An entry
whose expression cannot resolve shows the address recorded at generation time
with an `unresolved` marker and is never written to; a symbol a script published
resolves such expressions until that script is deactivated. A script's own
failure marks that one script `error`, is reported on stderr and leaves the
other scripts and the exit code alone. Exit codes: `0` success, `1` usage error,
`2` no embedded data, `3` target process not found, `4` memory access failure.

`./tools/install.sh` configures, builds and installs slopkit with its desktop entry and
hicolor icons into `~/.local` by default, with no root needed; override the
prefix with `PREFIX` (`PREFIX=/usr/local ./tools/install.sh`). The prefix is applied at
configure time and the docdir carries the `README.md`, `docs/` files and font licence.

## Documentation

- [`docs/ARCHITECTURE_MAP.md`](docs/ARCHITECTURE_MAP.md) — fast lookup of where components live and the project conventions.
- [`docs/TESTING.md`](docs/TESTING.md) — configure, build, install, test and diagnose commands and recipes.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — host/plugin split, the `ProcessAccess` seam, plugin discovery and writing a plugin.
- [`docs/SCRIPTING.md`](docs/SCRIPTING.md) — the end-user guide to address-table Lua scripts, the memory API and worked examples.
- [`docs/UI_DESIGN.md`](docs/UI_DESIGN.md) — UI and design rules for the project.
- [`docs/LOGGING.md`](docs/LOGGING.md) — logging conventions.
- [`docs/WINE_AGENT.md`](docs/WINE_AGENT.md) — the Wine/Proton resident agent: design, measurements, deviations and limitations.
- [`src/plugin/plugin_api.h`](src/plugin/plugin_api.h) — the C plugin ABI.
