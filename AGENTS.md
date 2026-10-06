# AGENTS.md

This file is the AI agent's operating guide for this repository. It records the
rules and conventions the project owner has mandated, and the agent must follow
them for every task in this repository.

## Architecture map

- Read [`docs/ARCHITECTURE_MAP.md`](docs/ARCHITECTURE_MAP.md) before starting any
  task. It lists where components live and the project conventions, so you can 
  find the right files without searching the codebase broadly; 
  only search widely when the map does not answer where something is.
- Keep the map up to date in the same task whenever you add, remove, rename or
  move files or components, or change a component's responsibility (see
  "Maintaining this map" inside it).

## General Guidelines
- Rely on docs/ARCHITECTURE_MAP.md Do not rediscover files unnecessarily.
- Limit exploration to ~25 tool calls, then ask at most 3 questions or propose a plan.
- Do not re-read a file or re-run `git log`/`git status` you already have in context.
- Pipe wide searches and scripts through `| head -40`.
  
## Project & toolchain

- This project is a C++23 project.
- Build it with CMake and Ninja.

## C++ Standard

- Target C++23 (`-std=c++23`). Prefer modern C++ idioms over legacy ones.
- Use standard library features first: `std::expected`, `std::optional`, `std::variant`, `std::span`, `std::string_view`, `std::format` / `std::print`, `std::ranges` and views, `std::flat_map`, `std::mdspan`.
- Use language features where they simplify code: concepts, `constexpr`/`consteval`, structured bindings, designated initializers, `if consteval`, deducing `this`, `[[nodiscard]]`, `std::unreachable`.
- Prefer RAII and value semantics; use `std::unique_ptr` / `std::make_unique` and avoid raw `new`/`delete`.
- Avoid C-style casts, raw arrays, `NULL`, and C string/IO functions when a standard C++ alternative exists.
- If a C++23 feature isn't available in the project's compiler or standard library version, fall back to the closest C++20/17 equivalent rather than hand-rolling a workaround, and mention it.

## UI & Design

- Follow the guidelines in `docs/UI_DESIGN.md`
- Never perform target access on the UI thread. UI code must not touch a `process::Session`, call a plugin or make a syscall; process listing, attach, probing, the desktop-entry index, memory reads/writes and the freeze pass all go through `process::AccessWorker` and are applied by its queued `drain()` on the UI thread. Debug operations are the one exception in mechanism but not in rule: they go through the debug worker and its `drain()`, because `ptrace` cannot run on the shared access worker.

## Build & test

- Configure and build:

  ```sh
  cmake -G Ninja -B build
  cmake --build build
  ```

- Targets are compiled for the host CPU's full instruction set by default
  (`-march=native -mtune=native`); pass `-DSLOPKIT_NATIVE=OFF` to CMake for a
  portable build targeting the compiler's default baseline.

- Five convenience scripts under `tools/` work from any directory:
  `./tools/configure.sh` runs the configure step, `./tools/build.sh` builds
  (configuring first when `build/` does not exist yet), `./tools/install.sh`
  configures, builds and installs into `PREFIX` (default `~/.local`), including
  the desktop entry and hicolor icons, `./tools/test.sh` builds and runs the test
  suite with KDE's crash notifications parked (see "Run the tests" below), and
  `./tools/verify.sh` is the one-shot definition-of-done check: format check,
  warning-only build and the summarized test run, with the full logs in
  `build/verify-*.log` and the extra arguments forwarded to ctest.

- When a completed plan changes anything that ships in the installed build —
  `src/**`, `CMakeLists.txt`, `cmake/**`, `assets/**` or `data/**` — run
  `./tools/install.sh` so the owner can try the installed build. Plans that only touch
  `tests/**`, `README.md`, `AGENTS.md`, `docs/**`, formatting or comments skip it.
  If `./tools/install.sh` fails, report the failing command and its output and still
  finish the plan.
  
- Run the tests (Catch2 suites, wired into CTest):

  ```sh
  ./tools/test.sh
  ctest --test-dir build --output-on-failure
  ```

- KDE notifies about every crash of a process the user owns: `systemd-coredump`
  hands the dump to `drkonqi-coredump-processor`, which starts
  `drkonqi-coredump-launcher` on its user socket and that shows the popup. The
  launcher has no per-process opt-out (`KDE_DEBUG` only skips the DrKonqi dialog,
  `KDE_COREDUMP_NOTIFY=1` only switches to the developer notification, and
  `ulimit -c 0` does not stop the dump), so run the suite through `./tools/test.sh`,
  which parks that socket for the run and restores it afterwards; the crashes
  still land in the journal and in `coredumpctl`. Set `SLOPKIT_TEST_NOTIFY=1` to
  keep the notifications while debugging a crash. A bare `ctest` parks nothing.

- One CTest test, `clang-format-check`, verifies that every `src/**/*.hpp`,
  `src/**/*.cpp`, `tests/**/*.hpp` and `tests/**/*.cpp` file is formatted with
  `clang-format` (targeting the installed 23.x rules in `.clang-format`). It is
  registered only when `clang-format` is on the `PATH`, so a missing formatter
  skips the check instead of failing the build.

## Running tests (verified recipes)
- Preferred: `ctest --test-dir build -R "<exact or partial TEST_CASE name>" --output-on-failure`
  (ctest sets QT_QPA_PLATFORM=offscreen and the 60 s timeout for you).
- Direct binary: ALWAYS `QT_QPA_PLATFORM=offscreen ./build/slopkit_tests "name1,name2,*glob*"`.
  Multiple names are comma-separated, never space-separated.
- Never run broad tags like `[ui]` directly: it exceeds the tool timeout. Use `./tools/test.sh`.
- Build warnings/errors only: `tools/verify.sh` runs the build and prints just
  the `warning:`/`error:`/`FAILED` lines (or by hand:
  `cmake --build build --target slopkit_tests 2>&1 | grep -E "error:|FAILED|warning:"`).
- Format touched files before building: `clang-format -i <files>`.
- A failing test: rerun it alone with `ctest -R`. If it passes alone, check KNOWN_ISSUES
  before debugging.
  
## Dependencies

- Never add any other dependency without consulting the owner first.
- When a dependency is required, prefer packages that are available in the system repositories.
- Tying into that, the build links the system Zydis through `pkg-config`; it
  needs the distro Zydis development package (e.g. Fedora's `zydis-devel`) and
  `pkg-config` installed, and does not vendor Zydis.
- Qt 6 Widgets (`Qt6::Core`, `Qt6::Gui`, `Qt6::Widgets`) is the UI toolkit; the
  build requires the Qt 6 development packages and does not vendor Qt.
- ImageMagick is a build-time dependency: configure rasterises
  `assets/icons/icon.svg` into the desktop/window icon set and every other
  `assets/icons/*.svg` action glyph into embedded Qt resources, and fails with an
  actionable message when neither `magick` nor `convert` is on the `PATH`.

## Reference tree

- Everything under `reference/` is read-only.
- Never modify anything under `reference/`.

## Temporary files

- If possible, when you need to create temporary files, do so under the main project directory in a tmp folder instead of the system tmp folder.
- This project tmp directory should be gitignored.
- When any plan is completed, empty the project `tmp/` directory of any remaining files so no scratch files are left behind.

## Source layout

- Put all source files under `./src`.
- Place each `.hpp` and `.cpp` file next to each other.
- Use sub-folders under `./src` to group modules.
- Put tests under `./tests`, mirroring the `./src` module tree; CMake picks
  them up automatically.

## Address tables and `.skt` files

- An address table is an `.skt` file registered as `application/x-slopkit-table`
  (`assets/application-x-slopkit-table.xml`), so a double-click opens slopkit and
  the file carries the slopkit icon. Keep the MIME `slopkit-table` magic token in
  sync with the first line `src/table/serializer.cpp` writes.
- `tools/install.sh` finishes the registration (MIME/desktop database refresh and the
  per-user default handler) for a `$HOME` prefix; a system prefix only prints the
  commands.
- A second launch hands its `<table.skt>` path to the running instance over the
  per-user abstract socket in `src/app/instance*`; the running window then runs
  the same open flow as File > Open Table.

## Version control

- When any plan is completed, automatically create a git commit for the changes.
- If the current branch has an upstream, also push the commit.

## Definition of done
1. `tools/verify.sh` all green.
2. A flaky failure is acceptable only if it is listed in docs/KNOWN_ISSUES.md AND the
   test passes alone under `ctest -R`. Add new flakes to that file in the same commit.
3. Docs/map updated; `tmp/` emptied; then commit (and push if upstream exists).

## Maintaining this file

- Treat this file as a living document: update it whenever future work changes the project's rules, conventions, or toolchain.
