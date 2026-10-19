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
| Standalone trainer | Turn the open table into trainers (`File > Generate Trainer…`) that reproduce its live values, frozen writes and Lua scripts on a machine with no slopkit or plugin installed; a Linux executable and a 64-bit Windows one are written, each with the table's value entries, scripts and ReClass classes appended to a runtime template rather than a sidecar file. The trainer is a window on both targets: the Linux one is a Qt 6 window and needs the same Qt 6 runtime slopkit uses, while the Windows one draws its own Win32 window and stays a single file with no Qt. | Implemented (Generate Trainer…) |

## Quick start

Dependencies (from the system repositories): CMake and Ninja, a C++23 compiler,
the libzip development package (e.g. Fedora's `libzip-devel`) with `pkg-config`,
Catch2, Qt 6 Widgets (`Qt6Core`, `Qt6Gui`, `Qt6Widgets`) and ImageMagick
(build-time only, for the icon set). Lua, sol2 and Zydis are not system packages:
CMake fetches them with `FetchContent` — sol2 and Zydis from their repositories
at pinned revisions, Lua from the Lua team's official 5.4.7 release tarball,
pinned by its SHA-256 — so the first configure needs network access and
downloads each once into `build/_deps`, which the Windows trainer template's
cross-build reuses. The Wine/Proton resident agent is cross-compiled and
needs the mingw-w64 compilers for both bitnesses (`x86_64-w64-mingw32-gcc` and
`i686-w64-mingw32-gcc`); without them the build uses a prebuilt agent under
`src/plugins/wine_proton/agent/prebuilt/<bitness>` and otherwise fails the
`slopkit-agent` target — see [`docs/WINE_AGENT.md`](docs/WINE_AGENT.md). The
Windows trainer template needs the matching C++ cross-compiler
(`x86_64-w64-mingw32-g++` from the same package); without it the build still
succeeds and only the Windows trainer target becomes unavailable. The template
needs no Qt kit, because its window is drawn with the Win32 API.

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
slopkit nor any plugin installed. The generated Linux trainer is a Qt 6 window
and therefore needs the same Qt 6 libraries slopkit itself needs; the Windows
trainer draws its own Win32 window, needs no Qt and stays one file. The same
generation is reachable headlessly:

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
recorded when it was generated. If that process is not running yet, an
interactive run still opens the window with its rows unresolved and attaches when
the process appears; a `--wait` run waits the same way and exits `3` only if the
wait expires without a target:

```sh
./game.trainer                            # the interactive window, refreshed four times a second
./game.trainer --pid 4711 --list          # one-shot: print the rows and exit
./game.trainer --pid 4711 --set 1=250 --freeze 1 --wait 900 --list
./game.trainer --pid 4711 --activate 1 --wait 500 --list
./game.trainer --pid 4711 --verbose --activate 1 --wait 500 --list
./game.trainer --wait 10000 --list        # wait for the recorded game to start, then print
```

A game running under Proton/Wine is a target like any other on Linux: the name a
table records for it is the guest executable's (`game.exe`, not the wine loader
running it), and the trainer finds it through the guest's own images. The guest's
image also decides the pointer width, so a 32-bit game's pointer chains resolve
with 4-byte pointers even though the loader hosting it is 64-bit. Reading and
writing that memory follows the same kernel ptrace policy as any other target.

When a display is available, a run without any action flag opens the trainer
window: a table whose rows are the table's own rows in the order they were
saved, with a child indented under its parent, refreshed by the live pass. Its
columns are an untitled check column, then `Description`, `Address`, `Type`,
`Value`, `State`, `Interval` and `Message` — the first five as in slopkit's
address list, the last three carrying the script detail. A script row shows
`script` in `Type` and leaves `Address` and `Value` empty. The check column
freezes or unfreezes a value entry and activates or deactivates a script (each
running the script's `activate` / `deactivate` hook). The CLI numbers stay in
use underneath: `--set N` and `--freeze N` address value entry `N`,
`--activate N` addresses script `S<N>`, and each row names its own number in its
tooltip. Double-click a `Value` cell, type the new value and press Enter; `Esc`
or a click elsewhere discards the edit, and a text the entry's type or hex flag
refuses keeps the editor open with the reason on the status bar.
`File > Refresh now` re-resolves every expression, `File > Close` (`Ctrl+W`)
ends the session, `Help > Help` (`F1`) shows the window's help and
`Help > About` names the trainer, its entry and script counts and its target.
The status bar carries the header line on the left and the last action or script
message on the right, both refreshed by the live pass.

The Windows trainer keeps a console beside its window, because the template is a
console-subsystem binary and that is where `--list` output and the exit code go;
launched from the command line under Wine it therefore shows both the console
and the window. Its window uses the system UI font for prose and a fixed-pitch
face for addresses, values and intervals, so it follows the Windows theme.

The two numberings are independent, so entry `1` never touches `S1`. Frozen
entries are re-written on every tick, and an active script's `update` hook runs on
the interval it asks for, else `--interval` (250 ms by default), both inside
`--wait` and in the window. `--activate N` is repeatable and ticks a
script before the requested actions run, so a headless run starts only the scripts
it trusts — scripts always start idle otherwise. An entry whose expression cannot
resolve shows the address recorded at generation time with an `unresolved` marker
and is never written to; a symbol a script published resolves such expressions
until that script is deactivated. A script's own failure marks that one script
`error`, is reported in the list and leaves the other scripts and the exit code
alone. Without a display, or with any of the flags above, the trainer prints the
same rows as text and exits. Exit codes: `0` success, `1` usage error, `2` no
embedded data, `3` target process not found, `4` memory access failure.

`--verbose` writes slopkit's debug-level log to `<trainer file>.log` beside the
executable — `game.trainer.log` next to `game.trainer` — and prints that path on
stderr. A run without the flag records nothing. The file carries the payload the
trainer was generated from, the process it attached to, each script's activation
and deactivation, each hook's output and failure, every class-project error and
every `alloc`/`free`, which is where to look when a script behaves differently in
a trainer than it does in slopkit.

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
