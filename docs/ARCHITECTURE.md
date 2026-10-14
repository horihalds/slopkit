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
  group, owns the 64 software / 4 hardware breakpoint slots and the wait loop,
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
  <message>`. A run or toggle without an attached target is refused.
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
address expression, and each class holds an ordered `Node` list of the kinds
`i8..i64`, `u8..u64`, `f32`, `f64`, `bool`, `char`, `hex8..hex64`, `pointer`,
`class_instance` and `class_pointer`. Offsets are never stored: `layout_class`
accumulates them in node order exactly as the reference's
`BaseContainerNode.UpdateOffsets` does (`offset += node_size(node)`), with no
alignment, `class_instance` sized by the class it names and both pointer kinds
taking a pointer. A class that contains itself by value is refused with an error
naming the cycle instead of recursing, and an unknown reference takes no space.

The `View > ReClass` window splits the model over three pieces. The structure
model turns the *selected* class into rows and **blocks** — one block for the class
itself and one for every expanded reference node — and inlines the target class's
nodes beneath an expanded `class_instance`/`class_pointer`, with offsets
continuing from the node's own offset. Inlining stops at a
`kMaxReclassBlockDepth` of 8 levels and at a class that is already open on the
row's own ancestor chain, which renders as a muted `cycle` marker, so a
self-referential chain cannot explode the grid or the read batch. The value model
resolves each class's expression — one batched `submit_resolve_expressions` job,
keyed by class id, about once a second and immediately when an expression or the
module map changes — and submits **one read per visible block** through
`process::AccessWorker`, then slices every node's value out of its block at the
node's own offset. A block that cannot be read paints `?` for all of its nodes, an
unresolved class address `not resolved yet` and a detached target
`Attach to a target first.`; the proxy model fuses the two models into the
six-column tree the view shows.

A `class_instance` shares its parent's bytes, so it costs no read of its own, and
a `class_pointer`'s target is read out of the pointer's own bytes in the parent
block — which means a pointer that changes value shows its children one interval
later, the same lag the reference has. The window adds no timer: it is a
registered `LiveSurface` that refreshes on the shared cadence, stays quiet while
hidden, while live update is off, while no target is attached or while a job is in
flight, and matches every completion against the job it was submitted for. A
`Value` cell of an editable kind is edited in place: the text is parsed for the
node's kind, one `submit_write` carries the node's address and bytes, and a
refused write says why on the status line instead of writing a guessed address.
The cell's text and the text a write parses are deliberately two functions:
`format_node_value_display` adds the readings the reference draws beside a value
— an integer's hex form, a float at three decimals, a hex node's float and
integer readings, a pointer's target, and the address a hex node's bytes would
point at when they land in a module — while the editor and the parser keep
`format_node_value`'s single value, so what the user types is what the write
reads back.
`Dissect` guesses a better kind for a `hexN` node from its live bytes — a
plausible pointer first, then a small `f32`, a small `i32` and an aligned `f64` —
and re-types the node with the padding rule that keeps the following offsets
where they were, while `Follow Pointer` sets a pointer node's referenced class and
inlines it. A class project rides in the `.skt` archive as its own member group
(see the map's `.skt` gotcha).

The proxy model answers the two live columns from the value model and leaves the
other four to the structure model, and a pass repaints a cell only when its text
moved: it remembers what each row last showed, emits `dataChanged` for that one
cell, and skips the first pass after a rebuild because the reset has already
repainted the grid. The header keeps the static columns at a width measured once
from the font instead of measuring the model, so a pass that moves a value never
walks the rows to re-lay-out the view.
