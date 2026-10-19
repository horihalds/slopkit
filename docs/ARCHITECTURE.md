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
| `wine-proton` | Wine/Proton game processes | 10 | resident agent (allocation and introspection), `process_vm_*`, procfs, ptrace (debugger only) |

Lower precedence wins, so `wine-proton` is the default target for a Wine process
that `linux-proc` also claims; the process picker still shows the alternatives.

Both plugins link the shared ABI plumbing in `src/plugins/support/` — the session
registry, string arena, error mapping, the vtable and all 23 entry functions — and
supply only a `PluginProfile` (identity, precedence, access methods, claim policy,
module shaping and foreign-signal policy), so a fix there lands in both. Each
plugin still keeps its own state because the host `dlopen`s plugins with
`RTLD_LOCAL`.

The profile also carries seven optional hooks — `allocate`, `free`,
`close_session`, `regions`, `range_is_readable`, `read` and `write`.
`linux-proc` leaves them all null, so the shared support layer serves every one
of them from `session.mem` and the chunked readability probe. `wine-proton` fills
only `allocate`, `free`, `regions` and `close_session`, so allocation and the
region walk go through the target's own virtual-memory manager (the resident
agent) instead of a ptrace-injected `mmap`; `range_is_readable`, `read` and
`write` stay null and are host-served even while the target runs, so a scan never
waits on the agent. That mechanism is described in
[`docs/WINE_AGENT.md`](docs/WINE_AGENT.md).

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
- `read_memory` / `write_memory` — `wine-proton` serves them through the host
  `process_vm_*`/procfs path, exactly as `linux-proc` does, in every target state:
  a Wine scan is one host syscall per chunk and the resident agent is never asked
  to copy bytes
- `list_modules` / `list_threads` / `list_regions` — `wine-proton`'s
  `list_regions` is the agent's `VirtualQuery` walk merged with the `/proc` paths,
  and its `list_modules` is the target's own loader list (`MODULES`) merged with
  the same paths; both fall back while the target cannot run the agent, the module
  list to the last agent-derived list recorded for that session. Each module also
  carries the target's `pointer_size` (ABI 1.8), its pointer width in bytes, 0 when
  the plugin cannot tell; the field sits in what used to be trailing padding of
  `slopkit_module_info`, so the struct layout is unchanged.
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
  page-rounded read/write/execute block in the target as close to a best-effort
  `near_address` hint as it can (0 = anywhere): the hinted page when it is free,
  else the closest free gap below it, then above it, and only then anywhere. A
  hint the plugin cannot honour is never an error while the mapping succeeds.
  `free_memory` unmaps a block an earlier call of the same session returned;
  anything else is `SLOPKIT_ERR_NOT_FOUND`. A refusal is always reported as an
  error and **never as address 0**: an allocation is either a real address or a
  failure, so no caller can mistake a call that did not happen for a mapping. A
  session whose plugin leaves them null reports `unsupported`, which a script
  sees as `alloc: the target's plugin cannot allocate memory`. `wine-proton`
  serves the pair through its resident agent when it can: the mapping is created
  by the target's own `VirtualAlloc`, so Wine's VM manager knows about it, and a
  failure is an error rather than a fallback to the raw `mmap` above — see
  [`docs/WINE_AGENT.md`](docs/WINE_AGENT.md).
- `validate_memory` — the ABI 1.7 validation operation, left null by a plugin
  that cannot answer directly. It reports whether `[address, address + size)` is
  mapped **and readable**: a non-zero out-value means yes, 0 means no, and an
  unmapped or unreadable range is a successful answer rather than an error. A
  null session, a null out pointer or a zero size is
  `SLOPKIT_ERR_INVALID_ARGUMENT`. Unlike `allocate_memory` it never stops the
  target: it probes the existing `process_vm_*` / procfs path, so there is no
  ptrace attach and no interlock with the debug session. When a plugin leaves the
  slot null the host answers by probing its own read path, so behaviour never
  depends on a plugin's ABI minor. `wine-proton` leaves the hook null, so
  validation runs on that same host probe in every target state — the answer its
  own fallback already gave whenever the resident agent was unusable.

`src/plugin/plugin_api.h` is the authoritative contract, and the bundled plugins
under `src/plugins/` are the worked example. They do not hand-write the vtable:
each defines a `plugins::support::PluginProfile` and returns
`plugins::support::entry<kProfile>(host)`, which fills the vtable from the shared
`slopkit_plugin_support` library.

## Scanning

`scan::ScanEngine` (`src/scan/engine.cpp`) runs value scans on its own worker
thread, never on the UI thread, over a `scan::MemorySource` built from the
attached session. A **first scan** splits the filtered address space into shards
and scans them in parallel: each shard reads the target in 4 MiB chunks with a
small overlap window straddling the chunk boundary and matches every aligned
candidate with `scan::Matcher`. A short or failed read no longer skips the rest of
a chunk — the cursor resumes at the first candidate whose window is not fully
inside the readable prefix (back-tracking by the value width, and never advancing
by less than the alignment), so an unreadable hole inside an otherwise readable
region is bridged and the hits past it are still found. When a whole read fails
there is no prefix to resume after, so the cursor probes forward one page at a
time to find where reading resumes; a wholly unreadable stretch therefore costs
one read per page instead of one read per candidate.

A **next scan** refines the previous, address-sorted result set with
`matches_refinement` (compared on the stored bytes) or a value comparison, and
never touches the target one candidate at a time: candidates are grouped into
address-sorted runs whose read window stays within 4 KiB, each run is read once per
worker with `MemorySource::read_into` — or the legacy `read()` when the source
exposes no `read_into` — and every candidate is matched out of that one buffer. A
wide single hit forms its own run, so a sparse result set degrades to the old
per-candidate cost. A short or failed run read is resolved by matching the
candidates fully inside the readable prefix and retrying the remainder in halved
sub-runs down to one candidate; a candidate that still cannot be read alone is
counted unreadable and dropped. Runs are claimed from one atomic cursor over the
worker pool (`max_threads_`), and their hits are collected per run index and
concatenated in run order, so the result set — same addresses, same order — is
identical for any thread count.

A stored hit is one fixed 32-byte `scan::ScanHit` (`src/scan/results.hpp`) that
owns no heap block: values up to 8 bytes live inside the record, and only a wider
value (a `string` or `byte_array` needle) goes into the set's shared `blob`,
addressed by the record's `blob_offset` with a refinement's before-bytes right
after the value. `scan::ScanResults` owns that one address-sorted set, so
recording a hit is one `memcpy` and a whole set costs
`hits.size() * sizeof(ScanHit)` plus the blob.

Nothing caps the stored set: every match is kept, so `ScanSnapshot::hit_count` is
both the number of matches and `hits.size()`. `ScanSnapshot::result_hits` hands
the set to the UI, which pages through it (the snapshot's `page` is a
self-contained `ScanResults` copy of the first `kDisplayPage` rows, wide bytes
re-packed, as a cheap preview). A `Next Scan` therefore refines exactly the stored
set and `undo()` restores the previous one; the undo history is bounded by
`kHistoryDepth` (8) sets alone, independent of their size.

## Referenced strings

`analysis::find_string_references` (`src/analysis/string_references.cpp`) is a
target-free sweep of one module: it decodes the target's executable regions,
clamped to `[module.base, module.end)`, in `chunk_size` reads in address order and
resolves each instruction's **direct** memory operand (RIP-relative or absolute,
never a base/index register) to an absolute target. A region is swept on its
readable/executable flags and its overlap with the module alone: a missing mapped
file path — the Wine host-map overlay case — and a region that reaches past the
module end do not void it, and a module of unknown or zero size is reported as
empty instead of swept silently. A target is accepted only when it is a
**readable** address of the same module and the bytes there form an ASCII string
(at least `min_length` printable characters, NUL-terminated) or a UTF-16LE string
(same rule over 16-bit code units); the containing region need not be
non-executable, so an image that merged `.rdata` into `.text` still resolves, and
the string read is bounded by the containing region's end
(`min(max_string_bytes, region.end - address)`) so a string that ends at the
region boundary is still classified. Validated addresses — hits and misses — are
cached, so a trampoline or vtable that many instructions reference costs one read.
A read spanning a chunk boundary carries up to the longest x86 instruction (15
bytes) into the next chunk so an instruction is never lost at the seam, a failed
decode resynchronises one byte forward, and an unreadable chunk is skipped and
counted instead of ending the region. The sweep runs in the target's bitness
(`disasm::machine_mode_for(pointer_size)`), and returns its address-sorted
`StringReference` list beside a `StringScanReport` — the counters of every stage
plus the rejected regions' reasons, rendered by `analysis::describe` for the
Memory Viewer's status line and the `scan` log — so an empty result always states
which stage gave up. The Memory Viewer paints the references as per-instruction
comments and lists them in its Referenced strings window; the sweep itself
performs no reads on the UI thread.

## The debugger

The debugger is opt-in and is the only `ptrace` user in the project. It sits
beside the normal access path rather than in it, so the read/write path stays
`process_vm_*` / procfs for both bundled plugins and the target sees no
`TracerPid` while it is read:

- `platform::ptrace` is the only place that calls `ptrace`. Both bundled plugins
  and the test binary link it through `slopkit_platform`.
- `platform::DebugSession` is the shared debugger core: it seizes the thread
  group, owns the 4096 software / 4 hardware breakpoint slots and the wait loop,
  and both `linux-proc` and `wine-proton` implement the ABI 1.4 `debug_*`
  operations as thin wrappers over it. Its group `attach()` takes an optional
  exclusion list — the tids a plugin names through
  `PluginProfile::debug_excluded_threads`, i.e. the Wine resident agent's
  dispatcher and workers — and leaves those threads running: they are never
  seized, never programmed with debug registers and never waited on, so the agent
  keeps serving allocation, the region walk and the module walk while the rest
  of the target is stopped at a breakpoint. It also offers a **single-thread window**
  (`attach_thread` / `detach_thread`) beside the group one: the ptrace allocation
  path borrows one donor through it and never seizes the group, the two windows refuse
  while the other is open, and only the group window owns the debug registers.
  Its `ForeignSignalPolicy` decides what happens to a stop the debugger did not
  ask for: `suppress` resumes with signal
  0 (what `linux-proc` uses), while `forward` re-delivers the signal to the
  target's own handler and keeps waiting (`wine-proton`, because Wine's runtime
  signals the target constantly). A target *killed* by a signal is still reported
  so the controller can end the session.
- `debug::PluginBackend` wraps its own plugin session, `debug::Worker` owns every
  call on one job thread — a tracee may only be ptraced by the thread that
  attached it — and `debug::Controller` is the UI-facing session owner: the
  breakpoint table, trap-to-breakpoint resolution, register/backtrace caching, the
  access watch, the code filter and the signals the panes render.
- Starting a session attaches and leaves the target **running**; only a
  breakpoint hit, a watch hit or `Break` stops it. Because a debug register can
  only be written while its thread is in a ptrace stop, the controller arms and
  disarms breakpoints through an invisible *maintenance stop*: it queues the
  requested arm/disarm, interrupts the group, applies the whole queue while it is
  stopped and continues. The stop is never reported — no `Stopped at` line, no
  `stateChanged` — and a burst of ops costs one stop. Every memory read and write
  of that burst goes through the thread the stop reported rather than the leader:
  ptrace only reaches a target's memory through a stopped thread, and only one
  thread of the group is stopped at a time.
- The **access watch** is one hidden hardware data breakpoint in the same
  `BreakpointTable` (its `hidden` flag keeps it out of the Breakpoints window
  while the slot accounting stays in one place). The controller consumes its
  stops inside `apply_stop()`: it records the hit against the accessing
  instruction (`debug::AccessWatch`, coalesced per instruction with a 1024-row
  cap), marks the `watchChanged()` signal dirty and resumes immediately, so the
  target keeps running. A new watch replaces the running one; `Stop` disarms it
  without dropping the recorded rows.
- The **code filter** reuses the table too. Collection is a target-free
  `analysis::find_code_targets` sweep of the module's executable regions on the
  access worker: only directly encoded `call`/`jmp`/`jcc` targets are kept,
  deduplicated by target and limited to the module range.
  `Controller::start_code_filter` accepts a stopped or a running target. On a
  stopped one it adds one hidden software breakpoint per candidate and arms them
  one at a time; on a running one it arms the whole burst inside a single
  invisible maintenance stop and resumes, so collecting never needs a manual
  pause. Arming stops at the first backend refusal so the surplus waits for a
  slot. A trap whose address `debug::CodeFilter::covers()`
  is consumed inside `apply_stop()` before the stop is reported: the controller
  restores the original byte through the worker, drops the entry, arms the next
  waiting candidate in the freed slot, and continues past the trap address — no
  `stopped()` and no `stateChanged()`, while `codeFilterChanged()` is coalesced
  like `watchChanged()`. A trap parks every thread that ran into the same patched
  byte while only one of them is reported, so the rewind of `RIP` covers all of
  them and a thread that was not stopped refuses the register write and is left
  alone. `Keep executed` / `Keep not executed` drop
  the losing half and disarm it; a detach, an ended session or `Clear breakpoints`
  resets the filter together with the table.
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
  `allocate`/`deallocate` operations, the ABI 1.7 `validate` operation and a
  `modules` supplier (`name` + base, the snapshot `expression` resolves module names
  against). The engine never sees
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
- Beside them, `structs` (`script/structs.{hpp,cpp}` over the plain, sol2-free
  `script/structs_view.{hpp,cpp}`) exposes the address table's ReClass classes as
  typed views: `structs.get(name)` resolves a class by name and `Class:at(addr)`
  binds an instance, whose `__index`/`__newindex` read and write one field per
  access through `script::codec` (`read_field`/`write_field`). An instance stores
  the class's **name**, and a `script::ClassIndex` is immutable once built, so no
  pointer into the UI-owned model is ever kept: the UI thread snapshots the project
  (`script::class_index_of`) and hands the snapshot to the job, which the engine
  publishes with `Engine::set_classes` before each call. A renamed or deleted class
  or field, a detach or a class that never existed therefore fails with a clear Lua
  error on the next access rather than reading stale state. A `snapshot()` instance
  holds the bytes of one `MemoryApi::read` (at most `kMaxViewSnapshot`), while its
  pointer fields still follow live memory; arrays and text stop at
  `kMaxViewArray`/`kMaxViewText`.
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
  *different* chunk, so a script's own `activate`/`deactivate`/`update` hooks share
  its labels while two scripts never do. Labels never reach `script::SymbolTable` or
  the address table's own resolution; a value that must outlive another script's
  run belongs in a global symbol (`ssymbol`).
- A chunk reads the names it knows back through `label(name)` (its own labels
  only) and `symbol(name)` (labels first, then the process-wide table), both
  returning `0` for an unknown name; and it resolves a full address expression —
  module/symbol/label base plus a `+offset` chain that may use square-bracket
  pointer groups — through `expression(text)`, which returns the absolute address.
  Each dereference is validated through the same capability as the table's resolve
  job, so a broken level names the level and the address. `expression` resolves
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
  `near_address` is a best-effort hint (omitted or `0` means anywhere): the plugin
  places the mapping as close to it as it can — the hinted page, else the closest
  free gap below it, then above it, then anywhere — and a hint the plugin cannot
  honour never fails the call while the mapping succeeds. The mapping is a plugin
  capability (ABI 1.6 `allocate_memory`/`free_memory`), so a target whose plugin
  cannot map memory reports `alloc: the target's plugin cannot allocate memory`
  rather than pretending.
- `dealloc(name)` resolves `name` through the same label-then-symbol chain and
  unmaps exactly the mapping an `alloc` of the **same session** created there; a
  foreign address is never unmapped (`dealloc: <name> is not this session's
  allocation`), so a typo cannot destroy one of the target's own mappings. Because
  the mapping belongs to the target and its lifetime is the target's, the address
  is re-checked immediately before the remote `munmap`: `/proc/<pid>/maps` must
  still show an anonymous read-write-execute region covering the whole recorded
  size there, and a page the target has taken back with a mapping of its own is
  refused with the reason (`the target no longer holds an anonymous writable
  executable mapping of <size> bytes at <address>`) instead of unmapped. Coverage
  rather than an exact region is what is checked, because the kernel merges
  neighbouring mappings with equal flags. The allocation belongs to the target and
  disappears with a detach.
- Allocation runs a remote `mmap`/`munmap` syscall inside the target through the
  plugin's ptrace `DebugSession`: the window borrows **exactly one** target thread,
  never the thread group, so the target's own waiters keep running and a wait the
  window does not own is neither resumed, nor entered, nor even stopped. The donor
  is the first thread of the group that is **provably outside a system call**:
  every non-leader thread is a candidate, running ones first and the rest in tid
  order, and a thread procfs already reports inside a call is passed over without
  being attached at all. The thread-group leader is borrowed only by a target that
  has no other thread, because taking a live group's leader over is what crashed a
  Proton game with the `Exception 0x80000003` signature in the manual recipe of
  [`docs/TESTING.md`](docs/TESTING.md): an idle Wine target parks its workers in
  `ioctl`/ntsync and leaves the leader in user mode, so the shape where the leader
  looks like the only borrowable thread is exactly the one the window refuses
  rather than robs of the one thread its runtime still owns. The field
  `/proc/<pid>/task/<tid>/syscall` decides, because an interrupt stop reports
  `none` from `PTRACE_GET_SYSCALL_INFO` whether or not a call is in flight; a
  thread that is inside one is never borrowed, stopped, re-entered or drained.
  When a whole scan finds no thread outside a system call the window does not
  refuse at once: it re-reads the thread states after a short pause and tries again
  until a fixed ~200 ms window elapses (nothing stays attached while it waits), so
  a target that is only momentarily busy is still served. Only a target that never
  exposes a thread outside a call through that window is refused with the reason —
  the refusal names how many scans were made — rather than taking over the return
  path of a call the target started (Wine parks its waiters in `ioctl`/ntsync,
  which is exactly a thread the window must never borrow).
  The syscall is executed with the donor's whole raw `NT_PRSTATUS` register file saved
  and written back wholesale, so fields the per-name setter does not name survive
  the hand-back, and a planted call still sitting at its syscall-entry stop is
  disarmed first (the number is rewritten to `getpid`) so no give-up path can leave
  a `mmap`/`munmap` armed. **No instruction of the target's own code ever runs for
  it**: the call is planted at a `syscall` instruction that the Zydis-backed
  decoder confirms starts at that exact address inside a *file-backed* executable
  region — never in the target's own anonymous generated code — and the thread is
  resumed with `PTRACE_SYSCALL`, so the kernel stops it at the syscall's entry and
  exit and the instruction behind the gadget is never reached. The result is read
  from RAX only at the verified exit stop, and only when the entry stop of the same
  call was seen first; a stop that is neither (a signal, a trap, an interrupt, a
  syscall of the target's own) is handled explicitly — a signal is handed to the
  target's own handler — under a bounded budget that ends in a refusal instead of a
  fabricated address. The near search runs its few `MAP_FIXED_NOREPLACE`
  candidates inside that one borrow window, and a candidate the kernel refuses only
  moves the search on. Handing the target back is part of the call: the exact
  register file is written back, a thread that stopped only to hand its own signal
  over is detached **with that signal** instead of losing it, and
  `/proc/<pid>/task/<tid>/stat` confirms the donor is not left in a traced stop —
  otherwise the failure is reported with the tid. A debug session and an allocation
  never overlap — each refuses while the other holds the target. Each `alloc`/
  `dealloc` writes `script`-category debug records naming the borrowed thread, the
  scan that found it, why it was chosen (or why the call was refused) and the
  outcome (address, size).
- `validate(address[, size_bytes])` answers whether `[address, address + size)`
  is mapped **and readable** in the target, returning a Lua boolean; `size_bytes`
  defaults to `1` and is probed in bounded chunks, stopping at the first chunk
  that fails or comes back short. An unmapped, unreadable or partially mapped
  range is `false` — a value, not an error, which is the point of the function.
  It is backed by the ABI 1.7 `validate_memory` operation (or the host's
  probe-read fallback) through `script::MemoryApi::validate`, needs no ptrace
  attach and never stops the target, and it writes no log record. A zero size
  (`validate: the size must be larger than 0`), a bad argument and a missing
  target (`validate: no target is attached`) raise a Lua error naming the
  function, as does a target whose plugin refuses with its own text. It composes
  with the other globals (`validate(label("buf"), 64)`,
  `validate(expression("game.exe+10"))`).
- `assemble(address, text[, ...])` turns a newline-separated block of the listing's
  own instructions into bytes with `disasm::assemble_block` and writes them into
  the target in one `MemoryApi::write`. Each instruction is encoded for the
  address it will occupy, and the CPU mode follows `api.pointer_size()` (`4` →
  `legacy_32`, else `long_64`). Extra arguments expand `text` through Lua's
  `string.format`; a wrong argument type raises, while an instruction the encoder
  rejects (naming its 1-based line), an empty block or a refused write returns
  `false, reason` and writes nothing, and a success returns `true, size`. A
  relative-branch target (`jmp`/`call`/`j<cc>`) the encoder cannot reach is
  rejected with the added cause that a relative branch reaches at most ±2 GB.
- `hook.install(spec)` and `hook.remove(spec)` install and remove a
  jump-between-code hook in two target writes — the runtime behind the generated
  hook script. The argument is the facts table the generator emits: the pattern and
  its optional module, the window's offset inside the match, the original bytes,
  the user's assembler block, the replaced instructions and their `site`-relative
  `%X` argument deltas. `install` scans for the pattern with a **match limit of
  two** — a pattern that is not unique is refused with a reason rather than hooking
  an arbitrary copy — verifies the originals at `match + offset`, maps a cave near
  the site, then writes the cave payload (code, trampoline, jump back) and the
  site's near jump plus NOP padding in two `MemoryApi::write`s; a failure after the
  mapping frees the cave, so the target is never left half-hooked. `remove` writes
  the originals back and frees the cave. Both return `true` alone on success or
  `false, reason` on a refusal, and both publish and drop the `site_name`/`cave_name`
  script-local labels the generated table names, so a live hook's state stays in the
  script and nothing is remembered engine-side. The mechanics live in the pure,
  Qt-free `script::hook` (`src/script/hook.{hpp,cpp}`) over `MemoryApi`;
  `engine.cpp` only parses the table, scans and publishes labels. The shared region
  walk (`Impl::scan_memory`) now takes a match limit, so `aobscan` keeps its
  first-hit behaviour and the hook path can collect up to two.
- `script::hook_script` (`src/script/hook_script.{hpp,cpp}`) is the pure, Qt-free
  generator behind the listing's `Hook Instruction...` command. It turns a decoded
  neighbourhood into a `HookTarget` — the AoB pattern and its match offset, the hook
  window's original bytes, the re-encoded trampoline and its numeric `site`-relative
  argument deltas — or a refusal reason, and renders the prefilled Lua source. The
  generated script is a facts table plus
  `function activate() return hook.install(kHook) end` and
  `function deactivate() return hook.remove(kHook) end`: the target work lives in
  the `hook` global, not in a self-installing template. The pattern wildcards the
  register bytes — the ModRM of any instruction that has one and the SIB when
  present — every memory displacement and every absolute-address or 32/64-bit
  immediate value, which the decoder reports as
  `disasm::EncodingBytes {offset, length, relative}` (the encoded-position twin of
  `AddressRef`, which slices the printed text): a relative branch or rip-relative
  displacement is module-relative and stays literal, so a rebased module still
  matches, while the opcode, a `PUSH`/`POP` reg-encoded opcode, a REX/operand-size
  prefix and a smaller immediate (`SUB RSP, 0x28`) stay literal too. The window is
  whole instructions from the selected row until a 5-byte near
  jump fits; an address operand is re-emitted as `0x%X` with its runtime `site ± delta`
  argument, so the trampoline stays position-independent after ASLR; a memory
  operand the decoder printed without a size keyword (`INC [RBX+1C]`) gains the
  explicit width its `MemoryRef` records (`INC dword ptr [RBX+1C]`), so the re-encode
  cannot guess the wrong operand size; and the generator proves the rewritten
  trampoline assembles at the original address with `disasm::assemble_block`,
  refusing when it does not so a broken hook never reaches the editor. Generation
  reads only the listing's cached bytes — the UI thread never touches the target.
- A registered symbol resolves like a module name in any address expression, in
  both the worker's resolve job and the deref-free UI parsers. `expr::evaluate`
  takes the snapshot as an argument and looks a base up as **module name → symbol
  name → literal**, so a loaded image is never shadowed while a symbol literally
  named `deadbeef` still beats the bare-hex reading; an unknown name keeps the
  existing base failure. An expression may spell a multi-level pointer chain with
  square brackets: a `[` … `]` group closes with one pointer read, nesting is the
  chain (`[[game.exe+10]+18]+24` reads twice), while a bracket-free `+offset`
  expression keeps the legacy rule of one read between consecutive offsets, so
  saved `.skt` rows, the UI's deref-free parsers (they accept only
  `pointer_levels() == 0`) and the existing error texts are unchanged. `evaluate`
  validates **every** read before performing it through an injected
  `expr::AddressValidator` the caller supplies (the worker builds one from
  `Session::validate`, the script engine from the same `MemoryApi` seam); a `false`
  answer fails that level with `the address <HEX> at level N is not readable` and
  sets `ResolveError::failed_level`, a validator that itself errors is ignored so
  the reader's own text stands, and an empty validator reproduces the old
  behaviour exactly. `expr` stays a leaf module —
  `script` includes `expr/resolver.hpp`, not the other way round.
- Besides `run(chunk)`, the engine exposes `run_lifecycle(chunk, function)`, which
  runs the chunk and then calls its named global (`activate` / `deactivate`),
  returning a `script::LifecycleResult {ok, error, message, output, interval_ms}`.
  The chunk runs first, so a chunk error
  never reaches the hook; a missing or non-function global reports
  `<name>() is not defined`. The hook's first return value decides the verdict —
  nothing or a truthy value succeeds, an explicit `false` refuses — and a second
   return value is the message either way. Both entry points share the instruction
   budget, the wall-clock deadline and the `print` capture, and the globals persist
   between calls exactly as for `run()`; `interval_ms` is what the run last passed
   to the `update_interval(milliseconds)` global, unset when it declared none.
- `run_update(chunk)` is the third entry point: it runs the chunk and then calls the
  global `update` only when *this* chunk defined one — it compares the global's
  `lua_topointer` identity before and after the chunk run, so an `update` left
  behind by another script is never called and a hook-less chunk is a silent skip
  rather than the `update() is not defined` error the lifecycle path would report.
  It returns a `script::UpdateResult {ran, ok, error, message, output, interval_ms}`;
  the verdict maps exactly like `run_lifecycle`, a chunk error leaves `ran == false`
  with the error set, and a refusal or error sets `ok == false`. `interval_ms` is what
  the run last passed to `update_interval`, so a tick can re-tune the cadence.
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
  <message>`. A run or toggle without an attached target is refused, and
  `Edit Script…` is refused while the row is active, so an active script can
  only be changed after deactivating it.
- The ticked scripts' per-interval hooks are one more worker job
  (`JobKind::script_update` → `ScriptUpdatesResult`): `submit_script_updates` runs
  `Engine::run_update` for every remembered script in activation order inside one
  job. Each remembered script also carries the interval its accepted `activate`
  declared through `update_interval` and the time of its last tick
  (`ActiveScript::interval_ms`, `last_tick`); a script whose declared interval has
  not elapsed since its last tick is skipped in place, while the others keep
  ticking, so a slow script never holds up a fast one. Because the pass still
  arrives at the Settings `Live update` interval, a declared value below it only
  means "due on every pass" and the global cadence stays the ceiling. A script whose
  chunk defines no `update` of its own is skipped silently; a
  failed tick runs that script's `deactivate` hook in the same job, forgets it and
  reports one `{description, reason}` in the result — the reason is the hook's
  message, or the error text when it did not refuse with one. The tick's captured
  `print` lines are dropped on purpose, because a hook that runs at the user's
  interval cannot feed the Log window without flooding it.
- A runaway chunk is aborted by the instruction budget and the wall-clock deadline
  in `script::EngineConfig`, checked from a `lua_sethook` installed for the run; the
  abort is an ordinary Lua error that leaves both the state and the session usable.
- The worker remembers every script whose `activate` verdict was accepted — its
  description and the exact chunk that ran — beside the engine, and forgets it when
  a `deactivate` verdict is accepted; an atomic count mirror (`active_scripts()`)
  is refreshed at every change and cleared wherever the engine is dropped, so the UI
  can skip an update pass while nothing would be ticked. As the worker stops
  (quitting slopkit), it
  runs each remembered chunk's `deactivate` hook once, in activation order, while
  the session and the engine's globals are still alive. The verdict is ignored — no
  retry and no box to write on the way out — and each result is logged once under
  `script`, so an activated script is undone before the target is released.
- An attached target is checked once a second through `JobKind::target_liveness`
  (`submit_target_liveness` → `TargetLivenessResult`), which lists processes on the
  worker thread; only a successful listing that no longer carries the pid proves an
  exit, so a listing failure never detaches a live target. On that proof the window
  submits `detach` (whose completion clears the remembered scripts), clears
  `AttachedTarget`, refreshes the address list and reports the exit in the status
  line, which turns every active script row inactive without running its
  `deactivate` hook against a dead process. A table with an auto-attach identity
  also keeps that identity after the exit and, once a process matching it appears
  again, submits the same listing-driven attach about once a second, so a
  trainer resumes on the reappearing target.

A script entry has no address and no value, never takes part in the freeze pass
(its `Active` flag drives the lifecycle hooks and the per-interval `update`
instead of a freeze writer) and
renders read-only in the grid apart from its leading `Active` checkbox and its
`Description` cell: the Type column says `script`, the Address and Value cells
stay blank and no other cell is editable, so a `Description` edit renames the
entry exactly as it renames a value row and can never submit a write job.
`File > Add Script…`, the row menu's `Run Script` and `Edit Script…`, the
`Active` checkbox and the Add/Edit Script dialog's compile-only `Verify` button
are the only new UI; `Add Script…` seeds the three hooks as a commented skeleton
(the `update` stub commented out, since the hook is optional).

## Structures (ReClass)

`slopkit::reclass` (`src/reclass/`) is a target-free model of the C/C++ structs a
user rebuilds from live memory: `Project` holds the `Class`es, each pinned to an
address expression, and each class holds an ordered list of `NodePtr` nodes. A
node is a polymorphic `Node` (`src/reclass/nodes/node.hpp`) with one class per
kind (`make_node(kind)` builds one), and the kinds ported so far are `i8..i64`,
`u8..u64`, `f32`, `f64`, `bool`, `char`, `hex8..hex64`, `pointer`,
`class_instance` (a class's fields inlined where it sits), `class_pointer`
(eight bytes that inline their target) and the container kinds in
`src/reclass/nodes/container.hpp` — an `array` sums its elements, a `union` is as
large as its widest member and shares one offset across its members, and
`class_instance_array`/`class_wrapper_array` repeat their class `repeat()` times.
Offsets are never stored: `layout_class`
accumulates them in node order exactly as the reference's
`BaseContainerNode.UpdateOffsets` does (`offset += node_size(node)`), with no
alignment and no pointer padding. A class that contains itself by value is
refused with an error
naming the cycle instead of recursing, and an unknown reference takes no space.
A class project also carries its `EnumDescription` entries
(`src/reclass/enum.hpp`): each has a name, a width of one, two, four or eight
bytes and named values that an enum node and a bitfield node pick from, while the
enum editor in `src/ui/dialogs/enums.hpp` creates and edits them without a
prompt.

The kinds beyond those are added on top: `nint`/`nuint`, `bitfield` (a bit width
and a sign), `enum` (a value named by the project enum it points at), the
`utf8`/`utf16`/`utf32` text kinds and their pointers, `vector2/3/4` and
`matrix3x3/3x4/4x4`, `virtual_method_table`, `virtual_method`, `function` and
`function_ptr` (`src/reclass/nodes/{number,text,vector,function}.hpp`). The
reference's node-type table, in the reference's group order, lives in
`src/reclass/node_menu.hpp` and drives both the add-node and the change-type
menus; the grid renders each kind's value through `format_node_value_text`
(`src/reclass/value.cpp`), so a text node decodes its characters, a bitfield its
bits, an enum the name of the value it reads, and a vector or matrix its floats.

The class project is written into the `.skt` archive as the `reclass/` member
group. `reclass/index.txt` opens with the payload version `v2` and then lists the
class members in order, `reclass/classes/<stem>.txt` holds one class each, and
`reclass/enums.txt` holds the project's enums. A class member opens with `v2` too,
then `name="…" address="…"`, then one `node depth=… kind=… flags=… name="…"
comment="…"` line per node carrying the per-kind extras that kind needs: an array
or union nests its children one `depth` deeper, a class node its `class`, a
bitfield its `bits` and `signed`, an enum node its `enum` and `size`, a text node
its `length`, a class array its `count`; `flags` carries the hex and hidden bits.
Every kind and its parameters therefore survive a round trip.

The payload is not read-compatible with the one the flat node struct wrote: a
class member whose first line is not `v2` is still named but comes back as a page
of default padding, and an index written the same way lists its classes the same
way. An archive saved by an earlier build therefore loses its classes' contents,
which is deliberate — the alternative was a second codec for a grammar that could
not express the new kinds. A malformed or unknown-token `v2` member fails the load
naming the member.

The `View > ReClass` window splits the model over three pieces. The structure
model turns the *selected* class into rows and **blocks** — one block for the class
itself and one for every expanded reference node — and inlines the target class's
nodes beneath an expanded `class_instance`/`class_pointer`, with offsets
continuing from the node's own offset. An array node is one row that shows a
single element — the one the index the structure model stores for that node names,
wrapping at both ends — so a long buffer never draws a row per element, and the
index rides on the node like its expansion: it is clamped when the count shrinks,
dropped when the node stops being an array and cleared wherever the rows are
rebuilt for another class. An element is identified by its place in the array
rather than by a name of its own, so its row draws no name, takes no rename and
says why on the status line, and an element a grown array appends is minted
without one; the fields a class array inlines are the class's own nodes and keep
their names. The two class arrays build that one element behind
their class's stride: a wrapper array's block starts at `node offset + index ×
class size` inside the parent's bytes, so the value model can keep slicing its
rows from the block's own base, and an instance array keeps its base at the node's
own offset — the pointer's own bytes stay where the pointer is — so the stride is
carried past the dereference instead. Inlining stops at a
`kMaxReclassBlockDepth` of 8 levels and at a class that is already open on the
row's own ancestor chain, which renders as a muted `cycle` marker, so a
self-referential chain cannot explode the grid or the read batch. The value model
resolves each class's expression — one batched `submit_resolve_expressions` job,
keyed by class id, about once a second and immediately when an expression or the
module map changes — and submits **one read per visible block** through
`process::AccessWorker`, then slices every node's value out of its block at the
node's own offset. A reading is kept across a class edit and used only at the
address it was taken at, so re-laying the rows leaves every block whose address did
not move on screen and paints `?` for a block that moved until the next pass reads
it. A block that cannot be read paints `?` for all of its nodes, an
unresolved class address `not resolved yet` and a detached target
`Attach to a target first.`; the proxy model fuses the two models into the
nine fields the painted grid flows.

A `class_instance` shares its parent's bytes, so it costs no read of its own, and
a `class_pointer`'s target is read out of the pointer's own bytes in the parent
block — which means a pointer that changes value shows its children one interval
later, the same lag the reference has. A class array carries that lag too and adds
its own stride to the target after the dereference, so the pointer's own bytes are
still read where the pointer sits. Walking an array's index is a structure change
and nothing else: it submits no read of its own, and the existing
one-read-per-visible-block pass picks the moved base up, dropping an answer that
was asked for before the jump. The window adds no timer: it is a
registered `LiveSurface` that refreshes on the shared cadence, stays quiet while
hidden, while live update is off, while no target is attached or while a job is in
flight, and matches every completion against the job it was submitted for. A
`Value` cell of an editable kind, one byte of a `hexN` node's dump or `Ascii`
column, and the `[N]` a `Type` cell brackets — a text node's length in characters
or an array's element count — are edited in place: `ReClassView` keeps one editor
plus an edit target of node, column, byte index and the run, a whole cell writes
through the proxy's `setData` (the text is parsed for the node's kind and one
`submit_write` carries the node's address and bytes) and one byte writes through
`ReclassValueModel::write_byte`, which parses that single byte itself and submits
the same job with a one-byte payload at the node's address plus the byte's index,
so the row's other bytes are never written. The run is the node's own number and
writes through `ReclassStructureModel::set_node_extent`, which parses a whole
number of at least one and hands it to the node's own `set_extent`, so it touches
no target, and because it sizes the node the class re-flattens the way a kind
change makes it. An array's index has its own writer on the same model —
`set_element_index`, which parses a whole number and wraps it modulo the count, so
an index past an end is the element at the other end — and the row menu's
`Previous Element`/`Next Element` step it with `step_element`, which wraps the same
way and refuses an array of a single element. A write that is refused — a byte the
node does not cover or a text the field cannot read (`Byte: expected one hex byte
such as 1A.`), a length or count that is not a whole number of at least one
(`Length:` / `Count: expected a whole number of at least 1.`) or an index that is
not a whole number (`Index: expected a whole number.`) — says why on the status
line instead of writing a guessed address, and the editor stays open with its text
selected so the correction is one edit away. The grid paints the
last reading, so a completion patches the bytes it wrote into the cached block and
repaints that cell instead of leaving them invisible until the next pass, which
still corrects the cell if the target took the write and said nothing.
A node's `Name`, `Comment` and the bracketed length or count of its `Type` cell are
the structure model's own editable fields — a note is trimmed free text and an
empty one clears it, while the run takes a whole number of at least one the node's
own storage holds — so an inline rename, the row menu's `Edit Comment…` and a
length or count edit are validated in one place and reported on that same status
line, touch no target and need none. The `Name` and `Comment` cells take the
structure model's `setData` path and the run its own writer; an array's element
row is the one `Name` cell that is read-only, so a rename there reports itself
instead of opening an editor. `F2`,
`F3` and `Enter` open the cursor row's name, comment and value, and the `Return` an
open editor would hand up to its parent is taken in the view's own filter, where a
dialog would otherwise press its default button.
The cell's text and the text a write parses are deliberately two functions:
`format_node_value_display` adds the readings the reference draws beside a value
— an integer's hex form, a pointer's target, and the address a hex node's bytes
would point at when they land in a module — while the editor and the parser keep
`format_node_value`'s single value, so what the user types is what the write
reads back. A 32-bit float is the one value the two spell alike: `marked_float`
appends the `f` C marks a single-precision value with to a `float32` node's value
and editor text and to a `hex32` cell's float reading, so the width a value sits
at is written down, while a `float64` stays bare and a value that is no usable
number keeps its own `NaN`/`Inf`/`-Inf` spelling; `drop_float_marker` takes that
`f` off a typed float again — after the drawn spellings were matched, so `inf` is
not cut to `in` — which is what lets a drawn float write back as the same bytes.
A hex node's own readings are the one part of that text a write does read back:
`node_readings` names the readings a hex cell draws — the three a `hex32`/`hex64`
cell spells, the two a `hex8`/`hex16` cell does (the sign-extended integer and
the raw bits, since its width has no float kind to spell: `(127) (0x7F)`) —
`format_node_reading` spells one, the float at three decimals with the `f` a
`hex32` cell's single-precision width is spelled with (`(0.000f)`) or the
`NaN`/`Inf`/`-Inf` its bytes hold, the sign-extended integer, and the raw bits
behind a `0x`, and `format_node_readings` wraps each in its own parentheses one
space apart, always every reading the kind draws, so a zero cell reads
`(0.000f) (0) (0x0)` and the reference's `#####` is gone.
`parse_node_reading` reads one typed reading back in the base that reading is
drawn in, never in the node's own `Hex` toggle's, and refuses a reading the kind
draws none of — a `hex8`/`hex16` cell's float. The value model hands the grid one
`HintRun` per reading whose editor commits through `write_reading()`, which
parses that one reading and writes the node's whole bytes. `reading_datatype`
names a reading after the kind that spells it, which the view shows on its hover
over that run.
`Dissect` guesses a better kind for a `hexN` node from its live bytes — a
plausible pointer first, then a small `f32`, a small `i32` and an aligned `f64` —
and re-types the node with the padding rule that keeps the following offsets
where they were, while `Follow Pointer` sets a pointer node's referenced class and
inlines it. A class project rides in the `.skt` archive as its own member group
(see the map's `.skt` gotcha).
`Project > Export C++ Header…` reads no target and writes no file by itself: it
renders the whole project — every class and every enum — as C++ source text with
`reclass::cpp_header`, which walks the project and its derived layout only, and
shows the text in a modal preview (`ui::dialogs::CppExportDialog`) whose `Copy`
takes it to the clipboard and whose `Save As…` writes it. The text is the project
as a translation unit a target's own source tree can include: the platform type of
each member's kind (`uint8_t`, `int32_t`, `size_t`, `ptrdiff_t`, `bool`, `char`,
…) under one whole-file include guard, the helper types a target may already own
(`Vector2/3/4`, `Matrix3x3/3x4/4x4`) each inside its own `#ifndef
<TYPE>_DEFINED` block so the target's own definitions win, a `utf16`/`utf32` text
node as the width-correct `char16_t`/`char32_t` rather than the four-byte
`wchar_t`, the project's enums before the classes, and the classes
forward-declared and then defined in the order that leaves an embedded class
complete before the class that holds it. Because the offsets are the unaligned
ones `layout_class` derives, the definitions sit inside `#pragma pack(push, 1)`
and each class carries a `static_assert(sizeof(X) == 0x…)`; every member is
value-initialized and, on its line, followed by the node's comment and its own
bracketed `[0x…]` offset, while a member keeps its
user name only when that name reads as an identifier (otherwise it is sanitized,
and a keyword gains a trailing `_`), an unnamed member is named
`<kind>_0x<offset>`, collisions take a `_1` suffix, and a run of bytes no member
names collapses into one `uint8_t pad_0x…[N]` member so the class stays
byte-exact. Nothing the project holds can fail the export: a pointer into a class
it does not define is a `void*`, a class that contains itself by value is written
without the recursive member and without a size assertion, and a member the header
cannot declare leaves a comment where it stood.

The window itself is shaped after the reference's `MainForm`: a menu strip, a
toolbar, a horizontal splitter and a status strip, with
`ui::components::ProjectView` (a `QTreeWidget` of the `Classes` and `Enums`
groups) on the left and the node pane on the right. Selecting a class in the tree
drives the grid; the tree's `Class Hierarchy View` nests the classes a class
refers to behind the reference's cycle guard, and its context menus add, rename,
delete and clean up classes and edit the enums. The `Node` menu and the grid's row
menu carry the reference's node commands — `Change Type`, `Dissect`,
`Follow Pointer`, `Previous Element`/`Next Element` (which show the element before
or after the one an array row shows, and are dead with their reason on a row that
is not an array and on an array of one element), `Create Class from Nodes`,
`Shrink Class`,
`Copy Nodes`/`Paste Nodes`, the five hide/unhide commands and `Copy Address` — and
the row menu carries the reference's two byte submenus, `Add Bytes` and
`Insert Bytes`, at the 4/8/64/256/1024/2048/4096 presets or an `X bytes…` prompt,
the same two the toolbar's drop-down buttons offer, all on the prompt-free
`ReClassDialog` methods the tests drive. A paste clones each
copied sub-tree through `Node::clone()` and re-ids every node, so a container's
children come with it, and a `Create Class from Nodes` moves the selection into a
new class and leaves a same-sized class instance in its place. A hidden node
collapses to one `...` row the grid paints at the head of the row's own content
slot; the row inlines nothing and a click unhides the node, while the bytes a
hidden node covers stay where they are. Two commands hand off rather than read
the target: `Search for Equal Values` gives the node's shown value to the scanner
panel (filling its value field and picking the value type that fits) and the
`Find Out What Writes This
Address` / `Find Out What Accesses This Address` pair gives the node's address and
width to the Access Watch, whose dialog the main window opens.

The proxy model answers the live fields from the value model and leaves the rest
to the structure model, and a pass repaints a cell only when its text moved: it
remembers what each row last showed, emits `dataChanged` for that one cell, and
skips the first pass after a rebuild because the reset has already repainted the
grid. The grid itself is `ui::components::ReClassView`, a
custom-painted scroll area: it flattens the model's rows in pre-order, keeps the
cursor and the selection as node ids so a rebuild never drops them, paints no
header band, and caps each field at the width the class draws - so nothing can
resize or drag anything - while a pass that moves a value re-measures only the
span the bar covers, never a column. The field widths and the grid's horizontal
scrollbar are the layout rule of [`docs/UI_DESIGN.md`](docs/UI_DESIGN.md).
## Generated trainers

`Generate Trainer…` in the File menu and the `slopkit generate-trainer` command
turn the open table into a standalone trainer: one file that carries the table's
value entries, scripts and ReClass classes inside itself and reproduces their
live values, frozen writes and scripted effects on a machine with no slopkit and
no plugin. The Linux binary is a Qt 6 window and needs the same Qt 6 runtime
slopkit itself uses; the Windows one draws its own Win32 window, links no Qt and
stays one file.

`trainer::generate` (`src/trainer/generate.cpp`) does that by copying an installed
runtime template and appending a version-3 payload: a `SLPKTRN1` header carrying the
process name and the item and class counts, then one item per table row **in the
order the `.skt` stores them**, each opening with its kind, nesting depth and
per-kind CLI number followed by its record — a value record (description, value
type, frozen and hex flags, the recorded bytes, the expression and the address it
last resolved to) or a script record (description and source) — then one record per
ReClass member (`reclass::to_members`' `name`/`text`, the same text seam the `.skt`
archive uses), then a `SLPKTRN2` finger holding the payload's size. The saved
table's row order and nesting therefore drive the trainer's layout; the payload's
`values()` and `script_records()` keep the two kinds addressable without losing
that order, and the runtime rejects a record whose kind or number disagrees with its
position. Both the
ELF and the PE loader ignore trailing bytes, so the copy is a valid executable and
a data file at once, and the runtime finds its own data by reading the last sixteen
bytes of its own file. A payload whose version is not the runtime's own is refused
with a message saying it was generated by a different slopkit build and must be
regenerated (exit code 2), because the two sides only ever move together. Each
target is written to `<name>.trainer[.exe].tmp` and renamed over the destination, so
a failed run leaves no half-written trainer behind, and a target whose template is
missing fails on its own without stopping the other.

The runtime (`src/trainer/runtime/`) is the same source set on both platforms, with
the platform specifics in `target_linux.cpp` and `target_windows.cpp`. It attaches
to `--pid`, else to `--name`, else to the process name recorded at generation time,
and refuses to guess: no match is a message and exit code 3 without touching
anything. Its module list is what expressions are resolved against — the basenames
of every mapped file on Linux and of `EnumProcessModulesEx(LIST_MODULES_ALL)` on
Windows — so a trainer resolves `module+offset` and `[[module+offset]+n]` exactly
as slopkit does, through the same `expr::evaluate` and the same `scan::*` value
codec, both linked from `src/` rather than copied. A level the target cannot read
fails the expression, and the entry then keeps the address recorded at generation
time, is marked `unresolved` and is never written to; pointer reads and the final
value both go through a readability check first.

A game running under Proton/Wine is a Linux target with nothing where a native one
keeps its identity: `/proc/<pid>/exe` is the wine loader, `comm` is truncated to
fifteen characters, and the loader hosting a 32-bit guest is a 64-bit host binary.
`target_linux.cpp` therefore builds its `Target` from the same Qt-free
`platform/linux` units the `wine-proton` plugin uses (`procfs`, `module_entry`,
`wine`), and the two facts a Wine target cannot be matched or measured without come
from the guest's own images: `platform::guest_image_name` for the name a table
recorded, and `platform::guest_pointer_size`, which reads the width from the guest
PE's machine field rather than from the loader's ELF class, so `Target::pointer_size()`
is the target's own — 4 for a 32-bit game whose wine loader is 64-bit. Name lookup
keeps the comm/executable match for everything else and only walks guest images for
a name ending in `.exe`, so no native target changes. Modules and regions come from
`platform::read_maps` alone, and a region is named after the module whose address
span owns it rather than after the path it was mapped with
(`process::owning_module_name`, the rule the app's own memory map already used):
Wine maps a PE image's executable page anonymously, so the path alone would leave
the very bytes a module-scoped `aobscan` has to sweep labelled `[anon]` and the
trainer would report the pattern missing where slopkit finds it. `target_windows.cpp`
takes the name from the other end of the same question: every page of an image
reports that image's base — the module handle — as its allocation base, so
`GetModuleBaseNameW` names the module a region belongs to however late the image was
loaded. Both backends have to fill `Region::name` for this reason: an unnamed region
is in no module, and a module-scoped `aobscan` then finds nothing and the script
reports `no region belongs to module <name>`. Memory access is
the same `process_vm_*`/`/proc/<pid>/mem` path as for a native target, subject to
the same kernel ptrace policy.

The table's scripts run through the application's own `script::Engine`
(`src/trainer/runtime/scripts.cpp`), linked verbatim rather than re-bound, so
`mem.*`, `aobscan`, published symbols, the `activate`/`deactivate`/`update` hooks
and `structs` views behave in a trainer exactly as they do in slopkit.
`trainer::ScriptHost` owns that engine, one `script::SymbolTable` and one
`ScriptState` per payload script, and builds the engine's `MemoryApi` over `Target`
(`read`, `write`, `validate` and the module names) and its `allocate`/`deallocate`
over `trainer::CaveAllocator` (`src/trainer/runtime/caves.cpp`). An allocation is a
**mapping** of the target's own wherever the backend can make one: on Windows
`Target::allocate` and `Target::allocate_executable` call `VirtualAllocEx` with
`PAGE_READWRITE` or `PAGE_EXECUTE_READWRITE`. The mapping is placed within a near
jump's reach of the `near` the script passes — for `hook.install` its site, for `alloc`
the site whose instructions name the slot. Windows' allocator otherwise picks any
address it likes — a trainer mapped a slot at `0x1E029DF0000` for a site at
`0x6FFFF646FCF9` — and the assembler then rejects `MOV qword ptr [0x1E029DF0000],
RBX`, because an x86-64 instruction can only name an absolute address within a signed
32-bit displacement. `map_near` asks `VirtualAllocEx` for the hint itself, then walks
`VirtualQueryEx`'s regions to the nearest free base below and above it, so the result
stays inside that reach. A slot borrowed from a module's zero-initialized data is only
*probably* free: it is zero at attach time and the program fills it as it runs, and a
hook stores into it from the game's thread, so the published symbol turns into
whatever the game put there. Read back as a Lua integer, a value with the high bit set
is negative, and `ssymbol` then reports `the value must be a 64-bit unsigned integer`,
which is how a trainer that had been running a while failed while the same script was
fine in slopkit.

Where the backend cannot map memory — the Linux backend has no in-target allocator, and
a Windows address space with no free base within reach of the site is the same
situation — an allocation is a **cave**: a run of zero bytes in a mapping the target
already has, handed out at a 16-byte boundary, preferring the run nearest the `near` a
hook passes so its jump stays short and only within a near jump's reach.

The two uses are not interchangeable, because the trainer writes through
`process_vm_writev`/`WriteProcessMemory`, which force a read-only page, while the
target's own CPU cannot: `alloc` returns a **writable** mapping or cave
(`Region::writable`) that the target's own instructions may store into, and
`hook.install` an **executable** one (`Region::executable`) it may run, off the map
permissions on Linux and the page protection on Windows. A mapping the trainer has a
name for — a module's own region or a mapped file — is preferred to a heap, stack or
JIT mapping the target is still handing out and could reuse under a borrowed cave. The
cave is zeroed through the trainer's write path before it is returned, which is also
how an address it cannot reach is ruled out, and `dealloc` zeroes it again so the run
is a cave for the next allocation, while a mapping is unmapped through the backend that
made it; only an allocation of this session can be freed. `alloc` and `hook.install`
therefore work in a trainer, in a mapping of its own or in space the target already
owns, and a request that neither can satisfy reports `no free code cave of <size>
byte(s) in its executable memory` or `no free data cave of <size> byte(s) in its
writable memory` rather than corrupting anything. The payload's class records are
rebuilt with
`reclass::from_members` and handed to the engine as its class fields, which is what
makes `structs.get(name)` work; a project that cannot be parsed is reported once
and leaves only `structs` unavailable. Scripts start idle — the `.skt` archive
stores no Active column, so nothing runs behind the user's back — and the check
column (or `--activate N`) runs the corresponding lifecycle
hook. An active script's
`update` hook runs on the interval it declares with `update_interval`, else on the
trainer's own `--interval`, inside both the `--wait` pump and the interactive loop.
The symbols a script publishes are handed to the resolver as its symbol snapshot one
refresh ahead, so an entry whose expression names one resolves in the same pass, and
deactivating the script takes them away again. Every hook's output is printed, and a
failing hook marks that one script `error` and stops ticking it: no script can abort
the trainer or change its exit code.

However a run ends — the window closing, the last `--wait` pump finishing, or the
requested actions completing — `run()` deactivates every script still active
(`ScriptHost::deactivate_all`) in table order before the session, target and engine
globals go away, which is the same shutdown the application's worker performs on
quit. The outcome is logged under the `script` category and otherwise ignored: no
retry, no status line and no change to the exit code. A target that has already
exited is the exception, because the session cleared every flag when it dropped the
dead handle, so no hook runs against it.

A trainer follows its process instead of ending with it, and it may start before
the process does. The attach failure that says no such process is running is not
fatal for the interactive run or a `--wait` run: `Resolver` and `ScriptHost` are
bound to a `DetachedTarget` stand-in whose accesses fail cleanly, so every row
shows unresolved, and the session retries the attach once a second. Startup
actions that need a target (`--activate` and `--set`) are replayed by the first
successful attach. A headless run without `--wait` still exits `3`, and a `--wait`
run whose deadline passes with no target does too.

`Target::alive()` is the platform's cheap liveness probe (`/proc/<pid>` on Linux, a
zero-time wait on the process handle on Windows), checked before every tick,
refresh and action. When it reports the process gone, the session marks every
script inactive without running a `deactivate` hook against the dead handle, drops
the target, rebinds the resolver and script host to the detached stand-in and
retries the attach once a second; the payload's recorded process name is used when
the trainer was started with `--pid`. A successful attach rebinds them to the new
target — fresh modules, symbol table, engine and caves — re-resolves and re-freezes
the entries, and runs `activate` again for the scripts that were active, so a
trainer outlives a game restart. The interactive header reads the current pid for
every draw and says the trainer is waiting while there is none, and actions
submitted while the target is absent answer that the trainer is waiting for it to
appear.

Both front-ends render one row core. `rows.{hpp,cpp}` walks the payload's `items`
and turns each row into a `RowView` — kind, label, description, nesting depth,
resolved address, type, value, frozen marker, active flag, unresolved flag,
failed flag and the note that explains an unresolved value — in that same saved
order, with a script row reading `script` in its type and leaving address and
value empty. `console::render_value_row` / `render_script_row` compose their
printed line from those fields and indent the description by two spaces per
depth level, so the window and the printed list cannot drift apart. The printed
line takes the script's `ScriptState` as a second argument, because the row
itself no longer carries the state, interval and message that `--list` prints;
the window reads them from the check box and the log pane instead.
Values are `1..N` and scripts `S1..Sm`; the two numberings are independent, the
help text says so and the number a row answers to is its tooltip. Frozen entries
are re-written on every tick, which is what lets the trainer hold a value a game
keeps overwriting. `parse_value_text` is the one parse of a value text:
`apply_action` writes through it and the window's editor validates with it.

With a display, a run without any action flag opens the platform window. On
Linux it is a `QMainWindow` over one `QTableView` fed by `gui::RowsModel`
(`src/trainer/runtime/gui/window_qt.hpp`, implemented in `gui/window.cpp`); on
Windows `gui/window_win32.cpp` mirrors it with a `ListView`, a custom-drawn check
column in the first column, an `EDIT` control over a double-clicked value cell and
an info-tip naming the row's CLI number; a report-mode `ListView` cannot indent a
sub-item, so its description is padded with two spaces per depth level. Both
implementations are declared by the Qt-free `gui/window.hpp`, which
holds `Options`, `Perform`, `Step`, `Pid`, `window_available()` and `run()`. Each
has `File` (`Refresh now`, `Close`) and `Help` (`Help`, `About`) menus and a
status bar carrying `console::render_header` on the left and the last action or
script message on the right.

Below the table both windows carry a read-only log pane. `gui::LogFeed` is the
Qt-free half: it registers a `log::Logger` sink, copies every `info`-and-above
record into a mutex-guarded queue bounded to 500 entries and calls a wake
callback. The sink runs on whatever thread logged, so it only queues and wakes —
never touches a widget. The Qt window's wake is a queued `QMetaObject::invokeMethod`
that drains `take_new()` into `gui::LogPane`; the Win32 window posts a `WM_APP`
message and appends through `EM_SETSEL`/`EM_REPLACESEL` to a read-only mono `EDIT`
control. `gui::log_line()` is the one formatter both use, so a record reads
`HH:MM:SS  <level>  <message>` on either target. Debug records stay in the log
file, which is what keeps a `--verbose` run's pane quiet.

`RowsModel` holds the worker's immutable row snapshot and maps it onto the five
columns: an untitled check column, `Description`, `Address`, `Type` and `Value`.
Its `description` cell reports `RowView::depth` through `ui::widgets::kDepthRole`
so the address list's `ui::widgets::IndentDelegate` indents nested rows, its
tooltip names the CLI number (`entry 3` / `script S2`), and the check column stays
column 0. A failed script paints its description in the theme's error colour, and
an unresolved value row keeps a muted address with its note as the address cell's
tooltip. A `Value` cell edits in place through
`ui::components::ValidatingDelegate`, and the check column freezes a value entry
or activates a script. A cell never mutates a snapshot: it validates, emits the
`Action` it wants (the same `slopkit::trainer::Action` the worker applies) and
repaints from the current snapshot, so a submitted change appears on the worker's
next pass. The Win32 window submits the same `Action`s from its check-column
click and its edited value, sizes its description column to the remaining client
width so the five columns do not scroll sideways, and both front-ends carry the
same five columns in the same order.

Every target call stays off the GUI thread. `gui::SessionWorker` is a `QThread`
that owns the trainer's existing `Session`, `ScriptHost`, `Resolver` and
`LiveEntry` state through the same `perform(Action)` and `step()` callbacks
`main.cpp` builds, runs its own `QTimer` on the session interval, applies the
submitted actions, then calls `step()` and publishes a
`std::shared_ptr<const std::vector<RowView>>` snapshot with the current header and
message. The window hands that snapshot to `RowsModel` and never sees a `Target`.
Because a `QThread` object lives in the thread that created it, `submit()` queues
the action under a mutex and wakes the worker with a queued call on an object the
worker thread owns, so no action can ever run on the GUI thread; closing the
window stops the worker and waits for it before the session is released. The
Win32 window's `SessionThread` is the same design without Qt: a condition
variable waits on the interval, submitted actions queue under a mutex, and the
thread is joined before the session is released. The pid the About box shows is
the one the worker last published, so no window ever calls `Target::pid()` itself.

The non-interactive flags (`--list`, `--set N=VALUE`, `--freeze N`,
`--activate N`, `--interval`, `--wait`) drive the same actions without a display,
and any action flag forces the printed path even when one exists, so the trainer
can be scripted or run by a test. Without them and with a display
(`gui::window_available()`, which on Linux accepts `DISPLAY`, `WAYLAND_DISPLAY`
or an explicit `QT_QPA_PLATFORM`, and on Windows always answers true) it runs the
window; without a display it prints the rows and exits. The exit codes are the
ones `README.md` documents.

The Windows template is cross-compiled with mingw-w64 against the same source
lists, with the Windows target backend and `gui/window_win32.cpp` swapped in for
the Linux backend and the Qt window. It links no Qt at all, so it keeps the
one-file promise with no sidecar DLLs. `cmake/Trainer.cmake` only needs
`x86_64-w64-mingw32-g++`; when it is missing the rule warns and builds no
template, so `trainer::generate` reports the windows target unavailable while the
Linux one still works. The template stays a console-subsystem binary, so
`--list` output and the exit code keep working from a terminal, and a window run
launched that way keeps its console beside the window.

The handle `attach` opens for the target carries `SYNCHRONIZE` on top of the
query and VM rights, because `WindowsTarget::alive` asks `WaitForSingleObject`
and Wine refuses that call for a handle opened without it. A failed wait must not
read as an exit either: only a signalled handle ends the session, so an
inconclusive probe leaves a live target attached instead of cycling between the
waiting state and the same pid.

A headless trainer installs no log sink of its own, so an ordinary run writes
nothing; an interactive run installs only `gui::LogFeed`, which mirrors
`info`-and-above records into the window and never writes a file. `--verbose`
(`src/trainer/runtime/logging.cpp`) raises slopkit's logger to `debug`,
adds the application's rolling file sink with a 1 MiB cap, and points it at the
executable's own path with `.log` appended — `game.trainer.log` beside
`game.trainer` — printing that path on stderr. The records are the trainer's
lifecycle rather than its data: the payload's counts, the attach request and its
outcome (or failure), a class project the engine could not parse, each script's
activation, deactivation and every failed `update`, the lines a hook printed,
each interactive action's result under `app` (an `info` record for a completed
action, a `warning` for a refusal), and each `alloc`/`free` together with the
region a write probe ruled out. Values are
not logged per tick, so a long session stays a small file; a script that behaves
differently in a trainer than in slopkit can be diagnosed from the file without a
debugger.

The templates live in the build tree (`SLOPKIT_TRAINER_DIR`) and, once installed,
next to the plugins (`SLOPKIT_TRAINER_RELATIVE_DIR`), which is where the generator
looks. The Linux one is the `slopkit-trainer` target itself. The Windows one is a
nested CMake cross-build (`cmake/TrainerTemplate.cmake` over
`cmake/toolchain-mingw64.cmake`, driven by `cmake/Trainer.cmake`) of the same
dependency declarations, so it links the same fetched Lua, sol2 and Zydis as the
host build, plus the Win32 window's `comctl32`, `gdi32`, `user32` and `psapi`. It
links no Qt and no `slopkit_ui_common`, which is what keeps the template one
file with the payload appended. The template also statically links the C++ and
mingw runtimes, so it needs no DLL on the target machine.
