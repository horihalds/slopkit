# AGENTS.md

This file is the AI agent's operating guide for this repository. It records the
rules and conventions the project owner has mandated, and the agent must follow
them for every task in this repository.

## Project & toolchain

- This project is a C++23 project.
- Build it with CMake and Ninja.

## Build & test

- Configure and build:

  ```sh
  cmake -G Ninja -B build
  cmake --build build
  ```

- Two convenience scripts at the repository root wrap those commands and work
  from any directory: `./configure.sh` runs the configure step, and `./build.sh`
  builds, configuring first when the `build/` directory does not exist yet.
  
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

## Reference tree

- Everything under `reference/` is read-only.
- Never modify anything under `reference/`.

## Temporary files

- If possible, when you need to create temporary files, do so under the main project directory in a tmp folder instead of the system tmp folder.
- This project tmp directory should be gitignored.

## Source layout

- Put all source files under `./src`.
- Place each `.hpp` and `.cpp` file next to each other.
- Use sub-folders under `./src` to group modules.
- Put tests under `./src/tests`; CMake picks them up automatically.

## Project index

- `INDEX.md` at the repository root indexes every file in the project: what it
  is and what it does.
- Whenever a task adds, removes, renames or repurposes a file, update `INDEX.md`
  in the same change so every file stays indexed exactly once.
- Be terse when writing the index

## Maintaining this file

- Treat this file as a living document: update it whenever future work changes the project's rules, conventions, or toolchain.

## Version control

- When any plan is completed, automatically create a git commit for the changes.
- If the current branch has an upstream, also push the commit.
