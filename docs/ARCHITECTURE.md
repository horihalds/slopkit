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
| `linux-proc` | Any normal Linux process | 100 | `process_vm_*`, procfs, ptrace (debugger only) |
| `wine-proton` | Wine/Proton game processes | 10 | `process_vm_*`, procfs, ptrace (debugger only) |

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
- `debug_*` — the ABI 1.4 opt-in debugger operations (`debug_attach` /
  `debug_detach` / `debug_continue` / `debug_step` / `debug_interrupt` /
  `debug_get_registers` / `debug_set_register` / `debug_set_software_breakpoint` /
  `debug_set_hardware_breakpoint` / `debug_backtrace`), left null by a plugin that
  cannot debug

`src/plugin/plugin_api.h` is the authoritative contract, and the bundled plugins
under `src/plugins/` are the worked example.


## The debugger

The debugger is opt-in and is the only `ptrace` user in the project. It sits
beside the normal access path rather than in it, so the read/write path stays
`process_vm_*` / procfs exactly as `docs/ANTI_DETECTION.md` describes:

- `platform::ptrace` is the only place that calls `ptrace`. Both bundled plugins
  and the test binary link it through `slopkit_platform`.
- `platform::DebugSession` is the shared debugger core: it seizes the thread
  group, owns the 64 software / 4 hardware breakpoint slots and the wait loop,
  and both `linux-proc` and `wine-proton` implement the ABI 1.4 `debug_*`
  operations as thin wrappers over it. Its `ForeignSignalPolicy` decides what
  happens to a stop the debugger did not ask for: `suppress` resumes with signal
  0 (what `linux-proc` uses), while `forward` re-delivers the signal to the
  target's own handler and keeps waiting (`wine-proton`, because Wine's runtime
  signals the target constantly). A target *killed* by a signal is still reported
  so the controller can end the session.
- `debug::PluginBackend` wraps its own plugin session, `debug::Worker` owns every
  call on one job thread — a tracee may only be ptraced by the thread that
  attached it — and `debug::Controller` is the UI-facing session owner: the
  breakpoint table, trap-to-breakpoint resolution, register/backtrace caching, the
  access watch and the signals the panes render.
- Starting a session attaches and leaves the target **running**; only a
  breakpoint hit, a watch hit or `Break` stops it. Because a debug register can
  only be written while its thread is in a ptrace stop, the controller arms and
  disarms breakpoints through an invisible *maintenance stop*: it queues the
  requested arm/disarm, interrupts the group, applies the whole queue while it is
  stopped and continues. The stop is never reported — no `Stopped at` line, no
  `stateChanged` — and a burst of ops costs one stop.
- The **access watch** is one hidden hardware data breakpoint in the same
  `BreakpointTable` (its `hidden` flag keeps it out of the Breakpoints window
  while the slot accounting stays in one place). The controller consumes its
  stops inside `apply_stop()`: it records the hit against the accessing
  instruction (`debug::AccessWatch`, coalesced per instruction with a 1024-row
  cap), marks the `watchChanged()` signal dirty and resumes immediately, so the
  target keeps running. A new watch replaces the running one; `Stop` disarms it
  without dropping the recorded rows.
- The Debugger pane, the Breakpoints window and the Access Watch window only
  submit and render; they never touch a session. `ptrace` runs only inside an
  explicitly started session, which the controller reports under the `debug` log
  category at start and stop. The Access Watch window's instruction reads are the
  one debugger-adjacent UI access that does not go through the debug worker: they
  are ordinary memory reads batched on `process::AccessWorker`.
