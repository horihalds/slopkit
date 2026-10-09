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

Both plugins link the shared ABI plumbing in `src/plugins/support/` — the session
registry, string arena, error mapping, the vtable and all 23 entry functions — and
supply only a `PluginProfile` (identity, precedence, access methods, claim policy,
module shaping and foreign-signal policy), so a fix there lands in both. Each
plugin still keeps its own state because the host `dlopen`s plugins with
`RTLD_LOCAL`.

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
- `suspend_target` / `resume_target` — the ABI 1.5 suspend operations, left null
  by a plugin that cannot stop its target. A session that suspended its target
  resumes it when the session closes or is destroyed, so an orderly teardown never
  leaves a target frozen.

`src/plugin/plugin_api.h` is the authoritative contract, and the bundled plugins
under `src/plugins/` are the worked example. They do not hand-write the vtable:
each defines a `plugins::support::PluginProfile` and returns
`plugins::support::entry<kProfile>(host)`, which fills the vtable from the shared
`slopkit_plugin_support` library.


## The debugger

The debugger is opt-in and is the only `ptrace` user in the project. It sits
beside the normal access path rather than in it, so the read/write path stays
`process_vm_*` / procfs and the target sees no `TracerPid` while it is read:

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
- **Step Out** reuses the same table: from a stop it takes the caller's return
  address from the cached frame-pointer backtrace (`frames[1].pc`), arms one
  hidden software breakpoint there and submits the continue only from the
  insert's completion, so a failed arm can never leave the target running
  untrapped. The stop that hits that trap erases the transient entry before it is
  reported, without counting a hit; any other stop — `Break`, another breakpoint,
  a watch hit, a signal, an exit or a failed continue — drops the pending step-out
  and is reported normally. A call stack with no caller frame refuses and warns
  instead of running away.
- The session is not started only from the Debugger pane: the watch commands
  (`Find out what writes/accesses this address` in the address list and the scan
  results, and `Find out what addresses this instruction accesses` in the
  listing) run through `ui::DebugSessionGate`, which asks the user (Yes/No, `No`
  default), calls `Controller::start()` when no session exists and waits for
  `running` before the command continues. The listing command then reads the
  register file through `Controller::capture_registers()`: a one-shot invisible
  stop built on the same machinery as the maintenance stop, which reads the
  registers, emits only `registersChanged()` and resumes, without ever reporting
  a stop or moving the listing position.
- The Debugger pane, the Breakpoints window and the Access Watch window only
  submit and render; they never touch a session. `ptrace` runs only inside a
  session, which starts from `Start Debugging` or from a watch command after the
  user confirms the attach prompt, and which the controller reports under the
  `debug` log category at start and stop. The Access Watch window's instruction
  reads are the one debugger-adjacent UI access that does not go through the debug
  worker: they are ordinary memory reads batched on `process::AccessWorker`.

## Lua scripting

The address table can hold **script entries** beside its value entries: a script
is Lua source that travels inside the `.skt` file (`entries/<stem>.lua`, whose
body is the source and nothing else) and runs on demand against the attached
target.

- The engine lives in `slopkit::script` (`src/script`) and is built on the system
  Lua through `sol2`. `script::Engine` is a pimpl whose Lua state is created once
  per attached session by `process::AccessWorker` — worker thread only, exactly
  like its `Session` — and dropped on detach, so a script's globals survive
  between runs on one target and never outlive it. `sol2` and the Lua headers are
  included only by `script/engine.cpp`, so no public header depends on them.
- `script::MemoryApi` is the seam to the target: three `std::function`s the worker
  fills from its session. The engine never sees `Session` or `ProcessAccess`, so
  it is unit-testable against an in-memory buffer
  (`tests/support/script_helpers.hpp`). A failed access raises a Lua error that
  carries the target's error text, which then surfaces as `RunResult::error`.
- A chunk sees `mem.pointer_size()`, `mem.read(addr, "u32")`-style typed reads
  (`u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 ptr`, decoded by `script::codec`),
  `mem.read_bytes`, `mem.read_string`, typed writes and `mem.write_bytes`; `print`
  is captured into `RunResult::output` — bounded by `kMaxOutputLines` and
  `kMaxOutputLine` — instead of writing to stdout, and a scalar `return` is
  rendered into `RunResult::returned`.
- Besides `run(chunk)`, the engine exposes `run_lifecycle(chunk, function)`, which
  runs the chunk and then calls its named global (`activate` / `deactivate`),
  returning a `script::LifecycleResult`. The chunk runs first, so a chunk error
  never reaches the hook; a missing or non-function global reports
  `<name>() is not defined`. The hook's first return value decides the verdict —
  nothing or a truthy value succeeds, an explicit `false` refuses — and a second
  return value is the message either way. Both entry points share the instruction
  budget, the wall-clock deadline and the `print` capture, and the globals persist
  between calls exactly as for `run()`.
- Running a script is an `AccessWorker` job (`JobKind::script` → `ScriptResult`):
  `submit_script` takes an optional hook name, so an empty name runs the whole
  chunk (the address list's `Run Script`) and a name runs `run_lifecycle()`. Either
  way the chunk touches the target on the worker thread and the UI only submits and
  handles the completion. The address list logs the printed lines under the
  `script` category, logs the first error exactly once and reports a short outcome
  in the status line; a script row's `Active` checkbox drives the lifecycle jobs
  (a tick submits the `activate` job, an untick the `deactivate` one), the flag is
  written only when the verdict accepts, and a refusal snaps the box back and
  reaches the status line as `Activate failed: <message>` / `Deactivate failed:
  <message>`. A run or toggle without an attached target is refused.
- A runaway chunk is aborted by the instruction budget and the wall-clock deadline
  in `script::EngineConfig`, checked from a `lua_sethook` installed for the run; the
  abort is an ordinary Lua error that leaves both the state and the session usable.

A script entry has no address and no value, never takes part in the freeze pass
(its `Active` flag drives the lifecycle hooks instead of a freeze writer) and
renders read-only in the grid apart from that leading `Active` checkbox: the Type
column says `script`, the Address and Value cells stay blank and no other cell is
editable. `File > Add Script…`, the row menu's `Run Script` and `Edit Script…` and
the `Active` checkbox are the only new UI; `Add Script…` seeds the two hooks as a
commented skeleton.
