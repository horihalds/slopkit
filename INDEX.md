# Index

- `AGENTS.md` — AI agent operating guide: project rules, toolchain and conventions.
- `build.sh` — builds the CMake/Ninja project, configuring first when `build/` is missing.
- `configure.sh` — configures the CMake/Ninja build in `build/`.
- `CMakeLists.txt` — CMake build: the `slopkit` app and the Catch2/CTest `slopkit_tests` target.
- `.gitignore` — ignores build output, CMake/Ninja artifacts, editor files and `tmp/`.
- `.clang-format` — C++ formatting rules for the project.
- `INDEX.md` — this file: one line per project file.
- `reference/README.md` — placeholder documenting the read-only `reference/` tree.
- `src/main.cpp` — application entry point; prints the banner and `slopkit::version()`.
- `src/core/version.hpp` — declares `slopkit::version()`.
- `src/core/version.cpp` — implements `slopkit::version()`.
- `src/tests/test_main.cpp` — Catch2 test runner (`CATCH_CONFIG_MAIN`).
- `src/tests/version_test.cpp` — Catch2 tests for `slopkit::version()`.
