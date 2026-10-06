# Architecture Map

Fast-lookup map of the repo: where components live, how to build and test, and
the rules a newcomer would otherwise get wrong. Read this before a task and only
search the codebase broadly when it does not answer where something is. Deeper
prose lives in `docs/ARCHITECTURE.md` (plugin model, debugger), `docs/UI_DESIGN.md`
and `docs/LOGGING.md`.

## 1. Project summary

slopkit is a plugin-first memory scanner, address-table editor and opt-in
debugger for Linux (and Wine/Proton) processes, with a Qt 6 Widgets GUI. The
host never touches another process directly: normal access goes through the
`process::ProcessAccess` plugin seam on an `AccessWorker`, and only debugging
uses `ptrace` through the debug worker. The bundled plugins are `linux-proc` and
`wine-proton`.

## 2. Build and test commands

| Task | Command |
| --- | --- |
| Configure | `cmake -G Ninja -B build` (or `./configure.sh`) |
| Incremental build | `cmake --build build` (or `./build.sh`) |
| Single target | `cmake --build build --target slopkit_tests` — targets: `slopkit`, `slopkit-sandbox`, `slopkit_tests`, `slopkit-linux_proc`, `slopkit-wine_proton` |
| Single test | `ctest --test-dir build -R "<Catch2 name>" --output-on-failure` |
| Single test (direct) | `QT_QPA_PLATFORM=offscreen ./build/slopkit_tests "<name>"` |
| Full suite | `ctest --test-dir build --output-on-failure` |
| Full suite, no crash popups | `./test.sh` — builds, then runs the suite with KDE's DrKonqi crash launcher parked |
| Format check only | `ctest --test-dir build -R clang-format-check --output-on-failure` |
| Install build | `./install.sh` (needed when `src/**`, `CMakeLists.txt`, `cmake/**`, `assets/**` or `data/**` change) |

For small edits prefer `cmake --build build --target slopkit_tests` followed by
one filtered `ctest -R`. `-R` matches Catch2 `TEST_CASE` names, not file names.

## 3. Directory map

| Directory | Responsibility |
| --- | --- |
| `src/app` | Process entry, CLI/headless modes, single-instance handoff, sandbox launch |
| `src/core` | Shared logging (`log.hpp`, `log_categories.hpp`) and version string |
| `src/debug` | Debugger core: session controller (split over `controller*.cpp`), worker, backends, breakpoints, step-over, access watch |
| `src/disasm` | Zydis-backed instruction decoding and assembling |
| `src/expr` | Address expression parsing (`base + offset...`) and module/literal resolution |
| `src/platform/linux` | Linux primitives (ptrace, procfs, memory, modules, desktop entries, Wine) shared with plugins |
| `src/plugin` | Plugin C ABI, host-side facade and loader/discovery |
| `src/plugins/{support,linux_proc,wine_proton}` | Shared plugin ABI plumbing and the two bundled plugins |
| `src/process` | Access seam, `AccessWorker`, plugin access, attachment metadata, shared types |
| `src/sandbox` | Standalone practice-target binary (`slopkit-sandbox`) |
| `src/scan` | Multithreaded value-scan engine, matcher, sources, value types |
| `src/table` | Address table model, `.skt` serializer, per-table settings |
| `src/ui` | Qt main window and shared UI infrastructure |
| `src/ui/components` | Reusable widgets/views (memory view, disassembly view, input boxes, message box) |
| `src/ui/dialogs` | Modal/top-level dialogs |
| `src/ui/models` | Qt item models for tables and lists |
| `src/ui/panels` | Docked panes (address list, found list, scanner, debugger) |
| `tests/app` | Tests for `src/app` |
| `tests/core` | Tests for `src/core` |
| `tests/debug` | Tests for `src/debug` (incl. the practice target) |
| `tests/disasm` | Tests for `src/disasm` |
| `tests/expr` | Tests for `src/expr` |
| `tests/fixtures` | Plugin fixture libraries used by loader tests (never part of the app) |
| `tests/platform/linux` | Tests for `src/platform/linux` |
| `tests/plugin` | Plugin host + bundled plugin tests |
| `tests/process` | Tests for `src/process` |
| `tests/sandbox` | Tests for `src/sandbox` |
| `tests/scan` | Tests for `src/scan` |
| `tests/support` | Shared test helpers (`fake_process`, `fake_debug`, UI/scan helpers, `test_main.cpp`) |
| `tests/table` | Tests for `src/table` |
| `tests/ui` | Window/flow tests (table open, attach, auto-attach, live values, settings) |
| `tests/ui/components` | Tests for `src/ui/components` |
| `tests/ui/dialogs` | Tests for `src/ui/dialogs` |
| `tests/ui/models` | Tests for `src/ui/models` |
| `tests/ui/panels` | Tests for `src/ui/panels` |

## 4. Module / component table

| Component | Purpose | Key headers / classes | Tests | Depends on |
| --- | --- | --- | --- | --- |
| app | Entry point, CLI modes, instance handoff, sandbox launch | `app/cli.hpp` (`run`), `instance.hpp`, `instance_server.hpp`, `sandbox.hpp` | `tests/app/*` | core, plugin, process, ui, platform |
| core | Logging sinks/categories and version | `core/log.hpp`, `log_categories.hpp`, `version.hpp` | `tests/core/*` | — |
| plugin | Plugin C ABI, host facade, loader | `plugin/plugin_api.h`, `plugin.hpp` (`Plugin`), `plugin_host.hpp` (`PluginHost`) | `tests/plugin/*` | process, core |
| process | Access seam, worker, attachment metadata | `process/access.hpp` (`ProcessAccess`), `access_worker.hpp` (`AccessWorker`), `attachment.hpp` (`AttachedTarget`), `plugin_access.hpp`, `types.hpp` | `tests/process/*` | plugin |
| scan | Multithreaded value scanner | `scan/engine.hpp` (`ScanEngine`), `matcher.hpp`, `source.hpp`, `value.hpp`, `types.hpp` | `tests/scan/*` | process, expr |
| table | Address table, `.skt` serialization, table settings | `table/address_table.hpp`, `serializer.hpp`, `table_settings.hpp` | `tests/table/*` | expr, scan |
| expr | Address expression parsing/resolution | `expr/expression.hpp`, `expr/resolver.hpp` | `tests/expr/*` | process |
| disasm | Zydis decode/assemble | `disasm/decoder.hpp`, `disasm/assembler.hpp` | `tests/disasm/*` | Zydis |
| debug | Debugger core: controller, worker, backends, breakpoints | `debug/controller.hpp` (`Controller`; `controller*.cpp` holds the per-concern definitions), `worker.hpp`, `backend.hpp`, `plugin_backend.hpp`, `breakpoints.hpp`, `step_over.hpp`, `access_watch.hpp` | `tests/debug/*` | process, plugin, platform |
| platform/linux | Linux primitives shared with plugins | `platform/linux/debug_session.hpp` (`DebugSession`), `ptrace.hpp`, `procfs.hpp`, `memory.hpp`, `module_entry.hpp`, `proc_text.hpp`, `desktop_entry.hpp`, `wine.hpp` | `tests/platform/linux/*` | core |
| sandbox | Practice-target window and values | `sandbox/sandbox_window.hpp`, `sandbox_values.hpp` | `tests/sandbox/*` | ui/components, core |
| ui | Main window, settings, live values, table file, theme/fonts | `ui/main_window.hpp` (`MainWindow`), `app.hpp`, `settings.hpp`, `text.hpp` (`to_qstring`), `live_values.hpp`, `debug_session.hpp`, `access_watch.hpp`, `completion_notifier.hpp`, `log_notifier.hpp`, `theme.hpp`, `fonts.hpp`, `table_file.hpp`, `address_format.hpp` | `tests/ui/*` | process, plugin, table, scan, debug |
| ui/components | Reusable widgets/views/delegates | `ui/components/memory_view*.hpp`, `disassembly_*.hpp`, `navigation_history.hpp`, `neutral_scroller.hpp`, `input_box.hpp`, `message_box.hpp`, `widgets.hpp` (`ActionIcon`), `code_patch.hpp`, `elided_tooltip_delegate.hpp`, `row_menu.hpp` | `tests/ui/components/*` | ui, disasm |
| ui/dialogs | Dialog windows | `ui/dialogs/*` (`ProcessListDialog`, `AddAddressDialog`, `BreakpointsDialog`, `MemoryViewerDialog`, `SettingsDialog`, `TableSettingsDialog`, `TableConflict`, `LogDialog`, `AccessWatchDialog`) | `tests/ui/dialogs/*` | ui, process, debug |
| ui/models | Qt item models | `ui/models/{address_table,found_results,process_list,breakpoint,register,call_stack}_model.hpp`, `watch_hits_model.hpp`, `instruction_access_model.hpp`, `live_cells.hpp` | `tests/ui/models/*` | table, debug |
| ui/panels | Docked panes | `ui/panels/{address_list,found_list,scanner,debugger}_panel.hpp` | `tests/ui/panels/*` | ui/models, ui/components |
| plugins/support | Shared bundled-plugin ABI plumbing | `plugins/support/plugin_support.hpp` (`PluginProfile`, `entry<Profile>`), `session.hpp` | via the bundled plugins | plugin, platform |
| plugins/linux_proc | Bundled Linux process plugin (profile + entry) | `plugins/linux_proc/linux_proc_plugin.cpp` | `tests/plugin/linux_proc*` | plugin, support, platform |
| plugins/wine_proton | Bundled Wine/Proton plugin (profile + entry) | `plugins/wine_proton/wine_proton_plugin.cpp` | `tests/plugin/wine_proton*` | plugin, support, platform |

## 5. Entry points and data flow

- `src/main.cpp` → `slopkit::app::run()` (`src/app/cli.cpp`) handles argv: headless commands (`--version`, `--list-plugins`, `--list-processes`, scan), otherwise it hands a table path to a running instance (`src/app/instance*`) or builds `ui::App` / `MainWindow`.
- Plugin discovery: `PluginHost::discover()` over `app::plugin_search_directories()`.
- Reads/writes: UI → `process::AccessWorker` job → `ProcessAccess` plugin session → `AccessWorker::drain()` applies results on the UI thread.
- Scans: scanner panel → `scan::ScanEngine` worker thread → `MemorySource`; the UI polls `snapshot()`.
- Live values: `ui::LiveValues` batches every registered `LiveSurface` request into one worker job per interval.
- Debug: `debug::Controller` → `debug::Worker` job thread → `debug::PluginBackend` → plugin `debug_*` → `platform::DebugSession`/`ptrace`; `drain()` applies results; start is gated by `ui::DebugSessionGate`.
- Tables: `table::AddressTable` backs `ui::models::AddressTableModel`; `.skt` I/O in `table/serializer.cpp`, opened via `MainWindow::open_table_request()`.

## 6. Conventions

- C++23, namespaces `slopkit::<module>` mirroring `src/<module>`; `#pragma once`.
- Each `.hpp` sits next to its same-named `.cpp`; CMake globs sources with
  `CONFIGURE_DEPENDS`, so new files are picked up on the next configure.
- Errors return `std::expected`/`std::optional` (e.g. `expr::parse`,
  `app::launch_sandbox`); worker results use `std::variant`.
- Ownership is value semantics plus `std::unique_ptr`/`std::make_unique`; owner
  classes delete copy (and often move); no raw `new`/`delete`, no C-style casts.
- UI rule (see `AGENTS.md` and `docs/UI_DESIGN.md`): never touch a target on the
  UI thread. Access goes through `AccessWorker`; debug through the debug worker.
- `Q_OBJECT` classes live in headers (AUTOMOC); `[[nodiscard]]` on accessors.
- Formatting follows `.clang-format` and is enforced by `clang-format-check`.
- System dependencies only: Zydis (pkg-config), Qt 6 Widgets, ImageMagick
  (build-time), Catch2; never add a dependency without asking the owner.

## 7. Gotchas

- Generated/build-time files: icons and action glyphs (`cmake/EmbedIcon.cmake`),
  embedded fonts (`cmake/EmbedFont.cmake`), Qt resources, and
  `build/slopkit.desktop` from `assets/slopkit.desktop.in`. Never edit `build/`.
- Must stay in sync: `assets/application-x-slopkit-table.xml` MIME magic with the
  first line `src/table/serializer.cpp` writes; `SLOPKIT_PLUGIN_RELATIVE_DIR` in
  `CMakeLists.txt`; `SLOPKIT_ACTION_ICON_NAMES` with `widgets::ActionIcon`; the
  plugin ABI version in `src/plugin/plugin_api.h` with the host handshake.
- `reference/` is read-only — never modify anything under it.
- `slopkit_tests` compiles the whole app (minus `main.cpp`), the sandbox sources
  and the fixture plugins, so a change anywhere can trigger a large rebuild.
- Widget tests run headless via `QT_QPA_PLATFORM=offscreen`; set it when running
  `slopkit_tests` directly. CTest enforces a 60s timeout per test.
- Configure fails without ImageMagick, Zydis dev packages and `pkg-config`.
- `tmp/` is the project scratch dir; empty it when a plan completes.

## Maintaining this map
- When you add, remove, rename, or move a file or component, or change a
  component's responsibility, update this map in the same task.
- Update only the affected entries. Use `git diff --stat` or the files you
  just touched to decide what changed; do not re-explore the whole project.
- Keep it under 150 lines. If it grows past that, shorten older entries rather
  than adding detail.
