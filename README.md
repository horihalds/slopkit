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
text file per entry plus the row order — and double-clicking one opens slopkit
with that table, while a running instance offers to cancel, overwrite the open
table or merge the file.

## Tools

| Tool | Purpose | Status |
| --- | --- | --- |
| Process list | Browse running processes, see which plugin claims each one, inspect it and attach/detach. | Implemented |
| Memory scanner | Search a target's memory for values and refine the result set with first/next/undo scans, keeping the shown hits up to date with live memory. | Implemented |
| Address list | Track found addresses, edit and freeze their values, watch them refresh live, and save or reopen the table. | Implemented |
| Lua scripts | Keep one-off Lua scripts beside the table as script entries, edit them in a Lua-highlighting, line-numbered editor and verify the source before running it, run them against the attached target through a small memory API (`mem.read`, `mem.write`, byte and string reads), publish named 64-bit values with `rsymbol` / `ssymbol` / `usymbol` that any address field then resolves, keep script-local `rlabel` / `slabel` / `ulabel` labels and locate a wildcard byte pattern with `aobscan`, rename one inline from its `Description` cell, and see what they printed and what failed. | Implemented (Add Script / Run Script) |
| Memory browser | Hex dump of live memory around a chosen address, refreshed automatically, beside a live disassembly listing and the Debugger pane. | Implemented |
| Practice target | A separate `slopkit-sandbox` process exposing values of every scan type; launch it from `Help > Launch Practice Target` and attach to practise the scanner. | Implemented |
| Disassembler | Instruction decoding (Zydis) of live memory. | Implemented (Memory Viewer pane) |
| Debugger | Opt-in instruction-level debugging of the attached target: break, resume, step, editable registers, a call stack, and software and hardware breakpoints with hit counters. | Implemented (Debugger pane plus the Breakpoints window) |
| Access Watch | Record every instruction that writes or accesses a watched address, and resolve an instruction's memory operands from live registers. The "find out what writes/accesses" commands attach the debug session on demand after a confirmation, so the Memory Viewer does not have to be opened first. | Implemented (Access Watch window) |
| Module / thread views | Inspect loaded modules and threads of the attached target. | Implemented (Process list detail) |

## Quick start

Dependencies (from the system repositories): CMake and Ninja, a C++23 compiler,
the Zydis development package (e.g. Fedora's `zydis-devel`) and the libzip
development package (e.g. Fedora's `libzip-devel`) with `pkg-config`, the Lua
development package and sol2 (e.g. Fedora's `lua-devel` and `sol2-devel`), Catch2,
Qt 6 Widgets (`Qt6Core`, `Qt6Gui`, `Qt6Widgets`) and ImageMagick (build-time
only, for the icon set).

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
- [`src/plugin/plugin_api.h`](src/plugin/plugin_api.h) — the C plugin ABI.
