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

## Run the tests

The suite is Catch2, wired into CTest:

```sh
./tools/test.sh
ctest --test-dir build --output-on-failure
```

For a targeted run, ctest sets `QT_QPA_PLATFORM=offscreen` and the 60 s timeout
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

## Diagnose a build or a run

- `tools/verify.sh` prints only the `warning:`/`error:`/`FAILED` lines of its
  build and keeps the full transcripts in `build/verify-format.log`,
  `build/verify-build.log` and `build/verify-tests.log`. By hand:
  `cmake --build build --target slopkit_tests 2>&1 | grep -E "error:|FAILED|warning:"`.
- Extra arguments are forwarded to ctest, so a targeted run reuses the entry
  point: `./tools/verify.sh -R "<test name>"`.

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

## Definition of done

`./tools/verify.sh` is the one-shot check; its three stages are:

- **format** — `clang-format --dry-run --Werror` over every `src/**` and
  `tests/**` `.hpp`/`.cpp` file (`skipped` when `clang-format` is missing).
- **build** — a warning-free build; a warning alone fails the stage.
- **tests** — the whole CTest suite, including `clang-format-check` and
  `docs-links-check` (the link/anchor/hygiene check in `tools/docs-check.sh`).

All three green is the definition of done; the exit status is non-zero when any
stage failed.
