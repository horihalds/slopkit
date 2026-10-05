# Architecture

slopkit follows a **plugin-first** design: the host application never touches
another process directly. Every external-process interaction is provided by a
plugin implementing the C ABI declared in `src/plugin/plugin_api.h`; the
host-side facade and RAII wrappers live in `src/plugin/plugin.hpp`.

## Host and plugins

- **Host** — owns plugin discovery, ABI/versioning checks, the merged process
  listing and the entire UI. It knows nothing about Linux internals, Wine or any
  access primitive.
- **Plugins** — own all target interaction: enumerating processes, listing
  modules and threads, and reading/writing memory. Each plugin advertises the
  access methods it can use.

The UI talks to the plugin layer only through the `ProcessAccess` seam
(`src/process/access.hpp`), a small C++ interface with `list_processes()` and an
`attach()` that opens a `Session`. Because the UI depends on the seam and not on
`dlopen`ed code, plugins can eventually run in their own agent processes through
an out-of-process transport, with no UI change.

## Bundled plugins

| Plugin id | Claims | Precedence | Access methods |
| --- | --- | --- | --- |
| `linux-proc` | Any normal Linux process | 100 | `process_vm_*`, procfs |
| `wine-proton` | Wine/Proton game processes | 10 | `process_vm_*`, procfs |

Lower precedence wins, so `wine-proton` is the default target for a Wine process
that `linux-proc` also claims; the process picker still shows the alternatives.

## Discovery

Plugins are discovered at startup in:

- `${exe_dir}/plugins` — the directory next to the `slopkit` executable.
- `${exe_dir}/../<libdir>/slopkit/plugins` — the directory the CMake install
  rules use, so an installed binary finds its plugins with no environment
  variable.
- Every directory listed in the colon-separated `SLOPKIT_PLUGIN_PATH` environment
  variable.

Incompatible or broken libraries are reported as diagnostics and skipped; they
never abort startup.

## Writing a plugin

A plugin is a shared library that exports exactly one symbol:

```c
const slopkit_plugin_vtable* slopkit_plugin_entry(const slopkit_host_services* host);
```

At load time the host calls the entry point and performs a handshake: the
returned vtable's `abi_version` must match the host's ABI major version, and its
`struct_size` must be large enough for the host to read the fields it expects. A
missing entry symbol, a major-version mismatch or a too-small vtable is diagnosed
and the library is rejected.

The host passes a `slopkit_host_services` struct so plugins do not need to
allocate through a different allocator than the one the host frees with; it
exposes `alloc`, `dealloc` and a `log` sink. A plugin must not let C++ exceptions
escape across the ABI boundary and owns any string it returns in a
`slopkit_result`.

A plugin's vtable provides its identity and precedence plus the operations the
host calls:

- `info` / `precedence`
- `list_processes` — the processes the plugin claims
- `open_session` / `close_session`
- `read_memory` / `write_memory`
- `list_modules` / `list_threads` / `list_regions`
- `access_methods` — the access primitives the plugin can use

`src/plugin/plugin_api.h` is the authoritative contract, and the bundled plugins
under `src/plugins/` are the worked example.
