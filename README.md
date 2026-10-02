# slopkit

slopkit is a Cheat Engine-like reverse-engineering toolset for games and software.
It targets Linux with **Wayland** as the primary display server (X11/XWayland is
supported as a fallback), and it is being built around a single rule: every
interaction with another process goes through a plugin.

The project is young. What exists today is the application shell, the process-access
plugin ABI and host, two bundled plugins, and one working tool — the process picker.
The rest of the suite is planned but not implemented.

## Tools

| Tool | Purpose | Status |
| --- | --- | --- |
| Process picker | Browse running processes, see which plugin claims each one, inspect it and attach/detach. | Implemented |
| Memory scanner | Search a target's memory for values and refine the result set. | Planned |
| Memory browser + disassembler | Hex view and instruction decoding (Zydis) of live memory. | Planned |
| Debugger | Breakpoints, stepping and register inspection. | Planned |
| Module / thread views | Inspect loaded modules and threads of the attached target. | Planned |

## Architecture

slopkit follows a **plugin-first** design. The host application never touches another
process directly: all external-process access is provided by a plugin implementing the
C ABI declared in [`src/plugin/plugin_api.h`](src/plugin/plugin_api.h).

- **Host** — owns plugin discovery, ABI/versioning checks, the merged process listing
  and the entire UI. It knows nothing about Linux internals, Wine or any access
  primitive.
- **Plugins** — own all target interaction: enumerating processes, listing modules and
  threads, and reading/writing memory. Each plugin advertises the access methods it can
  use.

The UI talks to the plugin layer only through the `ProcessAccess` seam
(`src/process/access.hpp`), a small C++ interface with `list_processes()` and an
`attach()` that opens a `Session`. This keeps the door open for an out-of-process
transport later: because the UI depends on the seam and not on `dlopen`ed code, plugins
can eventually run in their own agent processes without any UI change.

### Bundled plugins

| Plugin id | Claims | Precedence | Access methods |
| --- | --- | --- | --- |
| `linux-proc` | Any normal Linux process | 100 | `process_vm_*`, procfs |
| `wine-proton` | Wine/Proton game processes | 10 | `process_vm_*`, procfs |

Lower precedence wins, so `wine-proton` is the default target for a Wine process that
`linux-proc` also claims; the process picker still shows the alternatives.

Plugins are discovered at startup in:

- `${exe_dir}/plugins` — the directory next to the `slopkit` executable.
- `${exe_dir}/../<libdir>/slopkit/plugins` — the directory the CMake install
  rules use, so an installed binary finds its plugins with no environment
  variable.
- Every directory listed in the colon-separated `SLOPKIT_PLUGIN_PATH` environment
  variable.

Incompatible or broken libraries are reported as diagnostics and skipped; they never
abort startup.

### Writing a plugin

A plugin is a shared library that exports exactly one symbol:

```c
const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host);
```

At load time the host calls the entry point and performs a handshake: the returned
vtable's `abi_version` must match the host's ABI major version, and its `struct_size`
must be large enough for the host to read the fields it expects. A missing entry symbol,
a major-version mismatch or a too-small vtable is diagnosed and the library is rejected.

The host passes a `slopkit_host_services` struct so plugins do not need to allocate
through a different allocator than the one the host frees with; it exposes `alloc`,
`dealloc` and a `log` sink. A plugin must not let C++ exceptions escape across the ABI
boundary and owns any string it returns in a `slopkit_result`.

A plugin's vtable provides its identity and precedence plus the operations the host
calls:

- `info` / `precedence`
- `list_processes` — the processes the plugin claims
- `open_session` / `close_session`
- `read_memory` / `write_memory`
- `list_modules` / `list_threads`
- `access_methods` — the access primitives the plugin can use

## Anti-detection

Undetectability is a core design principle, not an afterthought. A target process
should not be able to tell that slopkit is inspecting it, so the default read/write path
never attaches to the target. The technique categories:

- **No `ptrace` attach** — the default path does not stop the target, so nothing like
  `TracerPid` appears in `/proc/<pid>/status`.
- **`process_vm_readv` / `process_vm_writev` first** — the preferred primitive, with a
  `/proc/<pid>/mem` pread/pwrite fallback when the syscall is unavailable or refused.
- **No artifacts in the target** — slopkit does not inject code, load libraries into or
  leave handles behind in the target process.
- **Per-plugin access strategies** — each plugin advertises the methods it can use, and
  the method actually used is reported per session, so the least detectable path can be
  preferred.
- **Optional out-of-process transport** — plugins are designed to run embedded today and
  behind IPC later, so the inspection agent itself can be separated from the UI.

The concrete method is left to each plugin; a plugin may legitimately use a more
detectable primitive while it is being brought up, as long as it reports that.

## Building and running

Dependencies (from the system repositories):

- CMake and Ninja
- A C++23 compiler
- Zydis development package (e.g. Fedora's `zydis-devel`) and `pkg-config`
- Catch2
- GLFW 3.4 or newer, built with Wayland support
- OpenGL development files (e.g. Fedora's `mesa-libGL-devel`)
- ImageMagick — build-time only: generates the icon set and the embedded window
  icon from `data/icon.jpg`

Configure and build:

```sh
./configure.sh
./build.sh
```

Run the tests (Catch2 suites wired into CTest, including a `clang-format-check` test):

```sh
ctest --test-dir build --output-on-failure
```

Run:

```sh
./build/slopkit              # launches the GUI
./build/slopkit --version    # prints the version
./build/slopkit --list-plugins   # lists discovered plugins and diagnostics
./build/slopkit --list-processes # lists processes claimed by the loaded plugins
```

The three flags work headlessly, with no display or GPU required.

## Installing

`./install.sh` configures, builds and installs slopkit together with its desktop
entry and hicolor icons. It installs into `~/.local` by default and needs no
root; override the prefix with `PREFIX`:

```sh
./install.sh                    # installs into ~/.local
PREFIX=/usr/local ./install.sh  # installs system-wide
```

The prefix is applied at configure time, because the desktop entry bakes the
absolute path of the installed binary. The resulting layout is:

- `<prefix>/bin/slopkit`
- `<prefix>/<libdir>/slopkit/plugins/libslopkit-linux_proc.so` and
  `libslopkit-wine_proton.so`
- `<prefix>/share/applications/slopkit.desktop`
- `<prefix>/share/icons/hicolor/<N>x<N>/apps/slopkit.png`
- `<prefix>/share/doc/slopkit/README.md`

`<libdir>` comes from CMake's `GNUInstallDirs`, so it is `lib64` on RPM-based
distros and `lib` elsewhere. ImageMagick is required to generate the icons at
configure time (see the dependency list above).

## Repository layout

- `src/` — application sources.
- `src/plugin/` — the plugin ABI, the loader and the host.
- `src/plugins/` — the bundled plugins (`linux_proc`, `wine_proton`).
- `src/platform/` — host-side platform libraries (procfs, memory, Wine detection).
- `src/ui/` — the ImGui/GLFW shell, theme, scaling, fonts and panels.
- `docs/` — project documentation, including `docs/INDEX.md` and `docs/UI_DESIGN.md`.
- `assets/` — embedded assets such as the UI font and its license.
- `reference/` — read-only reference material; never modified.

## Roadmap

1. Process picker — done.
2. Memory scanner.
3. Memory browser + disassembler.
4. Debugger.
