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
- `allocate_memory` / `free_memory` — the ABI 1.6 target-allocation operations,
  left null by a plugin that cannot map memory. `allocate_memory` maps a
  page-rounded read/write/execute block in the target, as close to a best-effort
  `near_address` hint as it can (0 = anywhere), and `free_memory` unmaps a block
  an earlier call of the same session returned; anything else is
  `SLOPKIT_ERR_NOT_FOUND`. A session whose plugin leaves them null reports
  `unsupported`, which a script sees as `alloc: the target's plugin cannot
  allocate memory`.

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
- `script::check_syntax(source)` is a target-free compile check: it loads the
  source into a throwaway `lua_State` (opened and closed within the call, with no
  library bound and no `mem` table) under the same `@script` chunk name a run
  uses, so it rejects exactly what the Lua compiler rejects and reports the same
  `script:<line>: <message>` text a run would — nothing is ever executed, so the
  check needs no session and is safe on the UI thread.
- `script::MemoryApi` is the seam to the target: the `pointer_size`/`read`/`write`
  `std::function`s plus a `regions` supplier returning every mapped region
  (`MemoryRegion {base, size, readable, module}`), which the worker builds from its
  session's regions joined with its modules. It also carries the ABI 1.6
  `allocate`/`deallocate` operations and a `modules` supplier (`name` + base, the
  snapshot `expression` resolves module names against). The engine never sees
  `Session` or `ProcessAccess`, so it is unit-testable against an in-memory buffer
  (`tests/support/script_helpers.hpp`). A failed access raises a Lua error that
  carries the target's error text, which then surfaces as `RunResult::error`.
- A chunk sees `mem.pointer_size()`, `mem.read(addr, "u32")`-style typed reads
  (`u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 ptr`, decoded by `script::codec`),
  `mem.read_bytes`, `mem.read_string`, typed writes and `mem.write_bytes`; `print`
  is captured into `RunResult::output` — bounded by `kMaxOutputLines` and
  `kMaxOutputLine` — instead of writing to stdout, and a scalar `return` is
  rendered into `RunResult::returned`.
- Beside `mem.*`, a chunk reaches the target with the typed one-liners
  `read_u8`/`read_i8`/…/`read_f64` (`read_<type>(address)`, the same tokens and
  the same integer-Lua-integer/float-Lua-float split `mem.read` uses) and
  `write(address, value[, type])`. With no token the width is inferred from the
  Lua value — a float writes `f32`, an integer the **narrowest** of
  `u8`/`u16`/`u32`/`u64` (or `i8`/`i16`/`i32`/`i64` when negative) — and an
  explicit token from the `mem.write` vocabulary overrides that, so a 64-bit
  integer or an `f64` write is always expressible. Every failure names the
  function (`read_u32: <target's error text>`, `write: unknown value type 'x'`).
- A chunk also publishes **symbols** through `rsymbol(name[, value])` (registers
  `name`, with `value` or `0`), `ssymbol(name, value)` (sets the value, registering
  the name when it is new) and `usymbol(name)` (removes it). Names are
  case-insensitive — `HP` and `hp` are one entry, whose first spelling is kept —
  must be a non-blank string without `+` and not starting with `#`, and the value
  must be an integral number in `0 .. 2^64-1`. A bad argument raises a Lua error
  naming the function, which surfaces as `RunResult::error` exactly like a failed
  `mem` access. The registry (`script::SymbolTable`) is process-wide and
  mutex-guarded: the worker writes it while the UI reads a snapshot, so a symbol
  survives a detach and is lost only when slopkit exits, and it is never written to
  a `.skt` file.
- The same triple exists as **labels** — `rlabel(name[, value])`, `slabel(name,
  value)` and `ulabel(name)` — with identical arguments, naming and error wording,
  but the registry is the engine's own and a label only exists inside the script
  source that registered it: the engine drops the labels whenever a run is handed a
  *different* chunk, so a script's own `activate`/`deactivate` hooks share its
  labels while two scripts never do. Labels never reach `script::SymbolTable` or
  the address table's own resolution; a value that must outlive another script's
  run belongs in a global symbol (`ssymbol`).
- A chunk reads the names it knows back through `label(name)` (its own labels
  only) and `symbol(name)` (labels first, then the process-wide table), both
  returning `0` for an unknown name; and it resolves a full address expression —
  module/symbol/label base plus the usual `+offset` dereference chain — through
  `expression(text)`, which returns the absolute address. `expression` resolves
  the base **label → process-wide symbol → module name → literal**, so a script's
  own name beats a module of the same spelling; this is the one place a script
  label reaches an address expression, while the address table's own resolution
  (module → symbol → literal) is unchanged. A blank name, a name with `+` or a
  leading `#` and an unresolvable expression each raise a Lua error naming the
  function.
- A script function that resolves a name — `aobscan`, `symbol`, `expression`,
  `alloc` and `dealloc` — looks at the labels first (case-insensitively) and only
  then at the process-wide symbols, so a label shadows a global symbol of the same
  name.
- `aobscan(name, pattern[, module])` walks the target's readable regions in
  ascending address order for the byte pattern `pattern` — hex bytes with `?`
  wildcards, matched by `scan::BytePattern` — and stops at the first match. With
  `module` given only that module's regions are walked, matched case-insensitively
  against the target's module names. On a hit the address is stored under `name` —
  into a label the script owns, else an existing global symbol (so the address
  table can resolve it), else a new label — and the call returns `true, address`;
  on a miss it returns `false` and leaves the entry alone. Argument, name, pattern,
  missing-region and no-readable-memory failures all raise a Lua error naming
  `aobscan`, and the scan is bounded by the run's wall-clock budget and read in
  64 KiB chunks.
- `alloc(name, size_bytes[, near_address])` maps `size_bytes` (rounded up to the
  target's page size) of read/write/execute memory **inside the target** and
  returns the address, publishing it under `name` with `aobscan`'s exact rule — an
  existing label the script owns, else an existing global symbol, else a new label
  — so `local p = alloc("buf", 64)` works straight away with `write`/`read_u8`.
  `near_address` is a best-effort hint (omitted or `0` means anywhere): a hint the
  plugin cannot honour never fails the call while the mapping succeeds. The mapping
  is a plugin capability (ABI 1.6 `allocate_memory`/`free_memory`), so a target
  whose plugin cannot map memory reports `alloc: the target's plugin cannot
  allocate memory` rather than pretending.
- `dealloc(name)` resolves `name` through the same label-then-symbol chain and
  unmaps exactly the mapping an `alloc` of the **same session** created there; a
  foreign address is never unmapped (`dealloc: <name> is not this session's
  allocation`), so a typo cannot destroy one of the target's own mappings. The
  allocation belongs to the target and disappears with a detach.
- Allocation runs a remote `mmap`/`munmap` syscall inside the target through the
  plugin's existing ptrace `DebugSession`: the target's thread group is stopped,
  the syscall is executed with the whole register file saved and restored, and the
  group is resumed, so the target is left exactly as it was found and the only
  intended change is the new mapping. A debug session and an allocation never
  overlap — each refuses while the other holds the target. Each `alloc`/`dealloc`
  writes one `script`-category debug record (address, size, outcome).
- A registered symbol resolves like a module name in any address expression, in
  both the worker's resolve job and the deref-free UI parsers. `expr::evaluate`
  takes the snapshot as an argument and looks a base up as **module name → symbol
  name → literal**, so a loaded image is never shadowed while a symbol literally
  named `deadbeef` still beats the bare-hex reading; an unknown name keeps the
  existing base failure. The script-level `expression` reorders the front of that
  chain (it looks the base up as a label, then a symbol, and only passes the name
  that answered), so its own order is described above. `expr` stays a leaf module —
  `script` includes `expr/resolver.hpp`, not the other way round.
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
- The worker remembers every script whose `activate` verdict was accepted — its
  description and the exact chunk that ran — beside the engine, and forgets it when
  a `deactivate` verdict is accepted. As the worker stops (quitting slopkit), it
  runs each remembered chunk's `deactivate` hook once, in activation order, while
  the session and the engine's globals are still alive. The verdict is ignored — no
  retry and no box to write on the way out — and each result is logged once under
  `script`, so an activated script is undone before the target is released.

A script entry has no address and no value, never takes part in the freeze pass
(its `Active` flag drives the lifecycle hooks instead of a freeze writer) and
renders read-only in the grid apart from its leading `Active` checkbox and its
`Description` cell: the Type column says `script`, the Address and Value cells
stay blank and no other cell is editable, so a `Description` edit renames the
entry exactly as it renames a value row and can never submit a write job.
`File > Add Script…`, the row menu's `Run Script` and `Edit Script…`, the
`Active` checkbox and the Add/Edit Script dialog's compile-only `Verify` button
are the only new UI; `Add Script…` seeds the two hooks as a commented skeleton.
