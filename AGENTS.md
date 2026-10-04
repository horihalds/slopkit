# AGENTS.md

This file is the AI agent's operating guide for this repository. It records the
rules and conventions the project owner has mandated, and the agent must follow
them for every task in this repository.

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
- Never perform target access on the UI thread. UI code must not touch a `process::Session`, call a plugin or make a syscall; process listing, attach, probing, the desktop-entry index, memory reads/writes and the freeze pass all go through `process::AccessWorker` and are applied by its queued `drain()` on the UI thread.

## Build & test

- Configure and build:

  ```sh
  cmake -G Ninja -B build
  cmake --build build
  ```

- Targets are compiled for the host CPU's full instruction set by default
  (`-march=native -mtune=native`); pass `-DSLOPKIT_NATIVE=OFF` to CMake for a
  portable build targeting the compiler's default baseline.

- Three convenience scripts at the repository root work from any directory:
  `./configure.sh` runs the configure step, `./build.sh` builds (configuring
  first when `build/` does not exist yet), and `./install.sh` configures, builds
  and installs into `PREFIX` (default `~/.local`), including the desktop entry
  and hicolor icons.

- When a completed plan changes anything that ships in the installed build —
  `src/**` other than `src/tests/**`, `CMakeLists.txt`, `cmake/**`, `assets/**`
  or `data/**` — run `./install.sh` so the owner can try the installed build.
  Plans that only touch `src/tests/**`, `README.md`, `AGENTS.md`, `docs/**`,
  formatting or comments skip it. If `./install.sh` fails, report the failing
  command and its output and still finish the plan.
  
- Run the tests (Catch2 suites, wired into CTest):

  ```sh
  ctest --test-dir build --output-on-failure
  ```

- One CTest test, `clang-format-check`, verifies that every `src/**/*.hpp` and
  `src/**/*.cpp` file is formatted with `clang-format` (targeting the installed
  23.x rules in `.clang-format`). It is registered only when `clang-format` is on
  the `PATH`, so a missing formatter skips the check instead of failing the build.

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
- Put tests under `./src/tests`; CMake picks them up automatically.

## Project index

- Prefer `docs/INDEX.md` for file discovery over shell searches such as `find`
  (or tree-wide `ls`/`grep`): it lists every project file with a terse
  description. Fall back to `find` only when the file is not indexed yet or the
  index looks stale.
- `docs/INDEX.md` at the repository root indexes every file in the project: what
  it is and what it does.
- Whenever a task adds, removes, renames or repurposes a file, update `docs/INDEX.md`
  in the same change so every file stays indexed exactly once.
- Be terse when writing the index

## Address tables and `.skt` files

- An address table is an `.skt` file registered as `application/x-slopkit-table`
  (`assets/application-x-slopkit-table.xml`), so a double-click opens slopkit and
  the file carries the slopkit icon. Keep the MIME `slopkit-table` magic token in
  sync with the first line `src/table/serializer.cpp` writes.
- `install.sh` finishes the registration (MIME/desktop database refresh and the
  per-user default handler) for a `$HOME` prefix; a system prefix only prints the
  commands.
- A second launch hands its `<table.skt>` path to the running instance over the
  per-user abstract socket in `src/app/instance*`; the running window then runs
  the same open flow as File > Open Table.

## Maintaining this file

- Treat this file as a living document: update it whenever future work changes the project's rules, conventions, or toolchain.

## Version control

- When any plan is completed, automatically create a git commit for the changes.
- If the current branch has an upstream, also push the commit.
