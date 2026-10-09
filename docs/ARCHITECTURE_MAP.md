# Architecture Map

Fast-lookup map of the repo: where components live and the conventions a
newcomer would otherwise get wrong. `AGENTS.md` routes each kind of task to the
document that owns it; read this map second, and only search the codebase broadly
when it does not answer where something is. `AGENTS.md` also routes to the sibling
docs that hold the detail behind this map.

## 1. Directory map

| Directory | Responsibility |
| --- | --- |
| `(repo root)` | Top-level build entry point: `CMakeLists.txt` |
| `src/app` | Process entry, CLI/headless modes, single-instance handoff, sandbox launch |
| `src/core` | Shared logging (`log.hpp`, `log_categories.hpp`) and version string |
| `src/debug` | Debugger core: session controller (split over `controller*.cpp`), worker, backends, breakpoints, step-over, access watch |
| `src/disasm` | Zydis-backed instruction decoding and assembling |
| `src/expr` | Address expression parsing (`base + offset...`) and module/symbol/literal resolution |
| `src/platform/linux` | Linux primitives (ptrace, procfs, memory, modules, desktop entries, Wine) shared with plugins |
| `src/plugin` | Plugin C ABI, host-side facade and loader/discovery |
| `src/plugins/{support,linux_proc,wine_proton}` | Shared plugin ABI plumbing and the two bundled plugins |
| `src/process` | Access seam, `AccessWorker`, plugin access, attachment metadata, shared types |
| `src/sandbox` | Standalone practice-target binary (`slopkit-sandbox`) |
| `src/scan` | Multithreaded value-scan engine, matcher, sources, value types |
| `src/script` | Lua scripting engine: a sol2 state, the `mem` API, the type-token codec and the process-wide symbol registry |
| `src/table` | Address table model, `.skt` ZIP archive serializer, per-table settings |
| `src/ui` | Qt main window and shared UI infrastructure |
| `src/ui/components` | Reusable widgets/views (memory view, disassembly view, script editor, input boxes, message box, window geometry keeper, window centerer) |
| `src/ui/dialogs` | Modal/top-level dialogs |
| `src/ui/models` | Qt item models for tables and lists |
| `src/ui/panels` | Docked panes (address list, found list, scanner, debugger), the one-line debug control bar, the Memory Viewer's menu bar and the shared debugger command enablement |
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
| `tests/script` | Tests for `src/script` |
| `tests/support` | Shared test helpers (`fake_process`, `fake_debug`, `access_worker_helpers.hpp`, `sandbox_helpers.hpp`, `scan_helpers.hpp`, `script_helpers.hpp`, `memory_view_helpers.hpp`, `ui_helpers.hpp`, `test_main.cpp`) |
| `tests/table` | Tests for `src/table` |
| `tests/ui` | Window/flow tests (the four subdirectories have their own rows) |
| `tests/ui/components` | Tests for `src/ui/components` |
| `tests/ui/dialogs` | Tests for `src/ui/dialogs` |
| `tests/ui/models` | Tests for `src/ui/models` |
| `tests/ui/panels` | Tests for `src/ui/panels` |
| `assets/` | Desktop/MIME files (`slopkit.desktop.in`, `application-x-slopkit-table.xml`), `icons/*.svg` (app icon + action glyphs), `fonts/*.ttf` with `OFL.txt` |
| `cmake/` | Build helpers `EmbedIcon.cmake` (rasterises icons/glyphs), `EmbedFont.cmake` (embeds fonts) and `StripDebugInfo.cmake` (strips installed binaries and installs `.debug` sidecars) |
| `docs/` | Prose: `ARCHITECTURE.md`, this map, `TESTING.md`, `UI_DESIGN.md`, `LOGGING.md`, `SCRIPTING.md`, `KNOWN_ISSUES.md` |
| `reference/` | Read-only reference material — never modified |
| `tools` | Repo dev tooling: `configure.sh`, `build.sh`, `install.sh`, `test.sh`, `verify.sh`, `docs-check.sh` — see `docs/TESTING.md` |

## 2. Module / component table

| Component | Purpose | Key headers / classes | Tests | Depends on |
| --- | --- | --- | --- | --- |
| app | Entry point, CLI modes, instance handoff, sandbox launch | `app/cli.hpp` (`run`), `instance.hpp`, `instance_server.hpp`, `sandbox.hpp` | `tests/app/*` | core, plugin, process, ui, platform |
| core | Logging sinks/categories and version | `core/log.hpp`, `log_categories.hpp`, `version.hpp` | `tests/core/*` | — |
| plugin | Plugin C ABI, host facade, loader | `plugin/plugin_api.h`, `plugin.hpp` (`Plugin`), `plugin_host.hpp` (`PluginHost`) | `tests/plugin/*` | process, core |
| process | Access seam, worker, attachment metadata | `process/access.hpp` (`ProcessAccess`), `access_worker.hpp` (`AccessWorker`), `attachment.hpp` (`AttachedTarget`), `plugin_access.hpp`, `types.hpp` | `tests/process/*` | plugin |
| scan | Multithreaded value scanner and byte-pattern matcher | `scan/engine.hpp` (`ScanEngine`), `matcher.hpp`, `pattern.hpp` (`BytePattern`), `source.hpp`, `value.hpp`, `types.hpp` | `tests/scan/*` | process, expr |
| table | Address table, `.skt` archive serialization, table settings | `table/address_table.hpp`, `serializer.hpp`, `table_zip.hpp`, `entry_name.hpp`, `table_settings.hpp` | `tests/table/*` | expr, scan, libzip |
| expr | Address expression parsing/resolution; a dependency-free leaf (it includes nothing outside `expr/`) | `expr/expression.hpp`, `expr/resolver.hpp` | `tests/expr/*` | — |
| disasm | Zydis decode/assemble | `disasm/decoder.hpp`, `disasm/assembler.hpp` | `tests/disasm/*` | Zydis |
| script | Lua scripting engine over the system Lua and sol2 | `script/engine.hpp` (`Engine`, `run_lifecycle`, `check_syntax`), `script/types.hpp` (`RunResult`, `LifecycleResult`, `SymbolApi`), `script/symbols.hpp` (`SymbolTable`), `script/codec.hpp` | `tests/script/*` | Lua, sol2, scan, expr |
| debug | Debugger core: controller, worker, backends, breakpoints | `debug/controller.hpp` (`Controller`; `controller*.cpp` holds the per-concern definitions), `worker.hpp`, `backend.hpp`, `plugin_backend.hpp`, `breakpoints.hpp`, `step_over.hpp`, `access_watch.hpp` | `tests/debug/*` | process, plugin, platform |
| platform/linux | Linux primitives shared with plugins | `platform/linux/debug_session.hpp` (`DebugSession`), `ptrace.hpp`, `procfs.hpp`, `memory.hpp`, `process_control.hpp`, `module_entry.hpp`, `proc_text.hpp`, `desktop_entry.hpp`, `wine.hpp` | `tests/platform/linux/*` | core |
| sandbox | Practice-target window, values and the `slopkit-sandbox` entry point | `sandbox/main.cpp`, `sandbox/sandbox_window.hpp`, `sandbox_values.hpp` | `tests/sandbox/*` | ui/components, core |
| ui | Main window, settings, live values, table file, theme/fonts | `ui/main_window.hpp` (`MainWindow`), `app.hpp`, `settings.hpp` (per-window geometries), `platform.hpp` (`preferred_platform_spec`), `text.hpp` (`to_qstring`), `live_values.hpp`, `debug_session.hpp`, `access_watch.hpp`, `completion_notifier.hpp`, `log_notifier.hpp`, `theme.hpp`, `fonts.hpp`, `table_file.hpp`, `address_format.hpp` | `tests/ui/*` | process, plugin, table, scan, debug |
| ui/components | Reusable widgets/views/delegates | `ui/components/memory_view*.hpp` (view + document), `disassembly_*.hpp` (view + document), `script_editor.hpp` (`ScriptEditor`), `script_highlighter.hpp` (`ScriptHighlighter`), `navigation_history.hpp`, `neutral_scroller.hpp`, `input_box.hpp`, `message_box.hpp`, `window_geometry.hpp` (`WindowGeometryKeeper`), `window_centerer.hpp` (`WindowCenterer`), `widgets.hpp` (`ActionIcon`), `code_patch.hpp`, `elided_tooltip_delegate.hpp`, `row_menu.hpp` | `tests/ui/components/*` | ui, disasm |
| ui/dialogs | Dialog windows | `ui/dialogs/*` (`ProcessListDialog`, `AddAddressDialog`, `AddScriptDialog`, `BreakpointsDialog`, `MemoryViewerDialog`, `SettingsDialog`, `TableSettingsDialog`, `TableConflict`, `LogDialog`, `AccessWatchDialog`) | `tests/ui/dialogs/*` | ui, process, debug, script |
| ui/models | Qt item models | `ui/models/{address_table,found_results,process_list,breakpoint,register,call_stack}_model.hpp`, `watch_hits_model.hpp`, `instruction_access_model.hpp`, `live_cells.hpp` | `tests/ui/models/*` | table, debug |
| ui/panels | Docked panes | `ui/panels/{address_list,found_list,scanner,debugger}_panel.hpp`, `ui/panels/debug_controls.hpp`, `ui/panels/debug_enablement.hpp`, `ui/panels/viewer_menu.hpp` | `tests/ui/panels/*` | ui/models, ui/components, debug |
| plugins/support | Shared bundled-plugin ABI plumbing | `plugins/support/plugin_support.hpp` (`PluginProfile`, `entry<Profile>`), `session.hpp`, `allocate.hpp` (ABI 1.6 remote `mmap`/`munmap`) | via the bundled plugins (shared helpers in `tests/support`) | plugin, platform |
| plugins/linux_proc | Bundled Linux process plugin (profile + entry) | `plugins/linux_proc/linux_proc_plugin.cpp` | `tests/plugin/linux_proc*` | plugin, support, platform |
| plugins/wine_proton | Bundled Wine/Proton plugin (profile + entry) | `plugins/wine_proton/wine_proton_plugin.cpp` | `tests/plugin/wine_proton*` | plugin, support, platform |

## 3. Entry points and data flow

- `src/main.cpp` → `slopkit::app::run()` (`src/app/cli.cpp`) handles argv: headless commands (`--version`, `--list-plugins`, `--list-processes`, scan), otherwise it hands a table path to a running instance (`src/app/instance*`) or builds `ui::App` / `MainWindow`.
- Plugin discovery: `PluginHost::discover()` over `app::plugin_search_directories()`.
- Reads/writes: UI → `process::AccessWorker` job → `ProcessAccess` plugin session → `AccessWorker::drain()` applies results on the UI thread.
- Scans: scanner panel → `scan::ScanEngine` worker thread → `MemorySource`; the UI polls `snapshot()`.
  With `Pause the game while scanning`, the panel suspends the target through the access worker before the engine starts and resumes it once the engine stops.
- Live values: `ui::LiveValues` batches every registered `LiveSurface` request into one worker job per interval.
- Debug: `debug::Controller` → `debug::Worker` job thread → `debug::PluginBackend` → plugin `debug_*` → `platform::DebugSession`/`ptrace`; `drain()` applies results; start is gated by `ui::DebugSessionGate`.
- Tables: `table::AddressTable` backs `ui::models::AddressTableModel`; a drag reorders rows through `AddressTable::move`; `.skt` is a ZIP archive (`table/serializer.cpp` over `table/table_zip.cpp`), opened via `MainWindow::open_table_request()`.
- Scripts: the address list's `Run Script` and a script row's `Active` checkbox submit `AccessWorker::submit_script`; the checkbox passes the `activate`/`deactivate` hook name and the worker calls `script::Engine::run_lifecycle`. The worker's `script::Engine` (one per attached session) runs the chunk with `print` captured, logs the printed lines under the `script` category and reports the outcome in the status line. It also deactivates a still-active script — running its `deactivate` hook and ignoring the verdict — when it stops, before the session is released. The `rsymbol`/`ssymbol`/`usymbol` globals write a process-wide `script::SymbolTable` (owned by `ui::App`); the worker takes a snapshot in its resolve job and every typed-address parser resolves a base as module → symbol → literal, so a symbol survives a detach and is lost only when slopkit exits. The same engine also exposes the typed `read_*`/`write` one-liners, the `label`/`symbol`/`expression` resolvers and the target-allocation `alloc`/`dealloc` pair (backed by the plugin ABI 1.6 `allocate_memory`/`free_memory`). The Add/Edit Script dialog's `Verify` button instead calls `script::check_syntax` on the UI thread — a target-free compile of the same `@script` chunk — so it needs no worker job and no session.

## 4. Conventions

- C++23 (`-std=c++23`), namespaces `slopkit::<module>` mirroring `src/<module>`;
  `#pragma once`, `[[nodiscard]]` on accessors, `Q_OBJECT` classes in headers
  (AUTOMOC).
- Reach for the standard library first: `std::expected`, `std::optional`,
  `std::variant`, `std::span`, `std::string_view`, `std::format` / `std::print`,
  `std::ranges` and views, `std::flat_map`, `std::mdspan`. Use the language
  features that simplify code (concepts, `constexpr`/`consteval`, structured
  bindings, designated initializers, `if consteval`, deducing `this`,
  `std::unreachable`); if a C++23 feature is unavailable in the project's
  compiler or standard library, fall back to the closest C++20/17 equivalent and
  mention it.
- Value semantics and RAII: `std::unique_ptr`/`std::make_unique` over raw
  `new`/`delete`; no C-style casts, raw arrays, `NULL` or C string/IO functions
  when a standard C++ alternative exists. Owner classes delete copy, and often
  move.
- Errors return `std::expected`/`std::optional` (e.g. `expr::parse`,
  `app::launch_sandbox`); worker results use `std::variant`.
- Source layout: all sources under `src/`, each `.hpp` next to its same-named
  `.cpp`, sub-folders group modules; tests under `tests/` mirror the `src` module
  tree and CMake picks them up automatically. CMake globs sources with
  `CONFIGURE_DEPENDS`, so new files are picked up on the next configure.
- UI rule (owned by `docs/UI_DESIGN.md` and mandated by `AGENTS.md`): never touch
  a target on the UI thread. Access goes through `AccessWorker`; debug through the
  debug worker.
- Formatting follows `.clang-format` and is enforced by `clang-format-check`.
- Never add a dependency without asking the owner; the system dependency list is
  owned by `README.md`.

## 5. Gotchas

- Generated/build-time files: icons and action glyphs (`cmake/EmbedIcon.cmake`),
  embedded fonts (`cmake/EmbedFont.cmake`), Qt resources, and
  `build/slopkit.desktop` from `assets/slopkit.desktop.in`. Never edit `build/`.
- Must stay in sync: `assets/application-x-slopkit-table.xml` MIME magic (the ZIP
  signature `PK\003\004`) with the bytes `src/table/table_zip.cpp` writes;
  `SLOPKIT_PLUGIN_RELATIVE_DIR` in
  `CMakeLists.txt`; `SLOPKIT_ACTION_ICON_NAMES` with `widgets::ActionIcon`; the
  plugin ABI version in `src/plugin/plugin_api.h` with the host handshake.
- A `.skt` archive is versioned by `version.txt`: the writer emits
  `slopkit-table 4` (value and script entries) and the reader also accepts
  `slopkit-table 3` (value entries only). An entry's kind is its member
  extension: `entries/<stem>.txt` is a value entry and a `entries/<stem>.lua`
  member's whole body is the script verbatim. `index.txt` lists both kinds in
  row order.
- `.skt` address tables are registered as `application/x-slopkit-table`, so a
  double-click opens slopkit with the slopkit icon; `tools/install.sh` finishes
  the MIME/desktop registration and the per-user default handler for a `$HOME`
  prefix (a system prefix only prints the commands). A second launch hands its
  `<table.skt>` path to the running instance over the per-user abstract socket in
  `src/app/instance*`, which then runs the same open flow as File > Open Table.
- `slopkit_tests` compiles every app source except the `main.cpp` entry points
  (`src/main.cpp`, `src/sandbox/main.cpp`), plus the sandbox sources minus
  `src/sandbox/main.cpp`; the fixture plugins are separate libraries, so a change
  anywhere can trigger a large rebuild.
- Widget tests run headless via `QT_QPA_PLATFORM=offscreen`; set it when running
  `slopkit_tests` directly. CTest enforces a 60s timeout per test.
- A test fake driven from a worker thread keeps its state behind a lock and
  exposes snapshot accessors instead of public fields, so the test thread never
  reads a record mid-write — see `tests/support/fake_debug.hpp`.
- Configure fails without ImageMagick, the Zydis, libzip, Lua and sol2 dev
  packages and `pkg-config`.

## 6. Maintaining this map

- When you add, remove, rename, or move a file or component, or change a
  component's responsibility, update this map in the same task.
- Update only the affected entries. Use `git diff --stat` or the files you
  just touched to decide what changed; do not re-explore the whole project.
- Re-derive the rows you touch from `git ls-files <dir>` rather than memory, so
  the listing stays checkable.
- Keep prose to a minimum. Relative links and anchor citations are guarded by
  the `docs-links-check` CTest test (`tools/docs-check.sh`).
