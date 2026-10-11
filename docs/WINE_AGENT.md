# Wine/Proton resident agent

`wine-proton` allocates target memory *inside* the target, through a small
resident agent DLL, instead of running a raw `mmap` in the ptrace injection
window. Wine's own virtual-memory manager owns the mapping, so `VirtualQuery`
reports it as committed and the prefix never places its own allocation on top of
it. The agent is the only allocation path: a failure is an error, never a silent
raw `mmap`.

The same agent supplies the target's *introspection*: the region list is the
agent's `VirtualQuery` walk, the module list is the agent's walk of Wine's own
loader list, address validation is one `QUERY`, and reads and writes are copied
inside the target. `/proc/<pid>/maps` is still read, but only to merge host paths
onto the agent's regions and modules, and to answer the region list while the
target cannot run (see "Agent-served versus host-served"). `docs/ARCHITECTURE.md`
owns the ABI-level description of `allocate_memory`, `read_memory` and `list_regions`;
this file owns the agent's design, what was measured, the deviations from the
original request and the known limitations.

## Why the ptrace path is not enough

`support::allocate_memory` injects a `mmap(SYS_mmap)` through ptrace into the
target. On Wine that creates a mapping the Wine virtual-memory manager knows
nothing about: `VirtualQuery` still reports the range as free, and Wine can
commit its own allocation over it later. The resident agent avoids that by
asking the target's own `VirtualAlloc`/`VirtualFree`/`VirtualProtect`/
`VirtualQuery` to do the work.

## Pieces

| Piece | Where | Role |
| --- | --- | --- |
| Protocol header | `src/plugins/wine_proton/agent/slopkit_agent_protocol.h` | The one contract (version 4): the fixed header, the ring of `SLOPKIT_AGENT_SLOT_COUNT` request slots — each with its own atomic state word, request/response record and 64 KiB payload — and the op/status/page constants. Compiled by mingw-w64 and by the Linux plugin. |
| Agent DLL | `src/plugins/wine_proton/agent/slopkit_agent.c` | C11, `-nostdlib`, kernel32 only. `DllMain` starts one dispatcher thread; the dispatcher maps the region file next to the DLL, claims every slot in the request state and hands it to a worker created lazily (up to the worker cap, never under Wine's loader lock), and the workers serve `HELLO`, `ALLOC`, `ALLOC_NEAR`, `FREE`, `PROTECT`, `QUERY`, `ENUM`, `MODULES`, `READ`, `WRITE`. Each worker answers its own slot and publishes the response with a release store. `ENUM` walks `VirtualQuery` into that slot's payload; `MODULES` walks the target's own `InLoadOrderModuleList`; `READ`/`WRITE` copy bytes in the target, bounded by the payload and the containing region. `SHUTDOWN` is answered by the dispatcher, which drains in-flight slots, frees everything, joins every worker and unmaps. |
| Injector | `src/plugins/wine_proton/agent/slopkit_injector.c` | One-shot exe run under the target's own wine: finds the target by image path, is idempotent when the agent is already loaded, then `OpenProcess` / `VirtualAllocEx` / `WriteProcessMemory` / `CreateRemoteThread(LoadLibraryW)`. |
| Test probe | `src/plugins/wine_proton/agent/slopkit_probe.c` | A trivial console target for the wine-gated end-to-end test; never installed. |
| Cross-compile | `cmake/Agent.cmake` | One pair of targets per bitness (`x86_64`, `i686`), a clear error when mingw-w64 and no prebuilt exist, and the install rule. |
| Target derivation | `src/plugins/wine_proton/wine_target.{hpp,cpp}` | From `/proc/<pid>`: the guest's main PE (for the bitness and the `Z:` image path), the loader to host the injector, the target's own environment (without the process-local handoff entries below), a `WINEPREFIX` fallback and the staging directory the target's own namespace can reach. |
| Transport | `src/plugins/wine_proton/agent_transport.{hpp,cpp}` | Creates and mmaps the region and exposes a pool of request slots. `acquire()` hands out a free slot and `Slot::transact()` publishes on it, waits with a bounded timeout and bumps the heartbeat; the request payload is copied into the slot before publishing and the response payload out before the state returns to idle, so one slot never races another. Capacity is the count `HELLO` advertised (one when it reported none). A slot whose exchange times out is retired and never reused, so a late answer can never be read as the next request's. |
| Client | `src/plugins/wine_proton/agent_client.{hpp,cpp}` | Locates the binaries, stages them where the target can see them, spawns the injector under the target's wine, runs the `HELLO` handshake (refusing a version, record-size or payload-size mismatch, and sizing the slot pool from the advertised capacity), and serves `allocate`, `allocate_near`, `free`, `protect`, `query`, `enumerate`, `modules`, `read` and `write`; tracks the addresses it handed out under a mutex and reports whether every slot is retired. |
| Service | `src/plugins/wine_proton/agent_service.{hpp,cpp}` | The per-pid owner and the seven profile hooks: `allocate`/`free` and `modules` (agent-only) plus `regions`, `validate`, `read` and `write`. Parallel calls run without a global lock, lazy bring-up, the bounded revive policy, the stopped-target routing, the module-list cache, the in-flight counter `close` waits on, and `close`. |
| Region merge | `src/plugins/wine_proton/agent_regions.{hpp,cpp}` | The pure merge of the agent's walk with `/proc/<pid>/maps`: boundaries, commit state and protection come from the agent, `path`, `offset`, `shared` and `deleted` from the overlapping procfs line. |
| Module merge | `src/plugins/wine_proton/agent_modules.{hpp,cpp}` | The pure merge of the agent's loader walk with `/proc/<pid>/maps`: `base`, image size, entry point, Wine's base name and the main flag come from the loader, `path` and `offset` from the overlapping procfs line; also the UTF-16 to UTF-8 name decoder. |

## Flow

1. The first call that needs the agent for a Wine target — an allocation, or an
   introspection call while the target is runnable — derives the target, stages
   the bitness-matched DLL and injector into a per-session directory the target
   can reach (see "Proton's private runtime directory" below), creates the region
   file there and runs the injector under the loader the target itself runs, with
   the target's own environment, minus the process-local handoffs described
   below. A failure here records the reason and is retried by a later call,
   bounded by the revive policy (three revives with a five second cooldown); the
   failing call is answered with the error — there is no ptrace fallback for
   allocation.
2. The agent's dispatcher thread opens `slopkit-agent.bin` as the sibling of its
   own module through `Z:` and polls the slots' state words. `HELLO` reports the
   protocol version, the record size, the pointer size, the Windows pid, the
   allocation granularity, the agent's base address, the payload capacity and the
   slot/worker counts; a version, record-size or payload-size mismatch fails the
   handshake immediately instead of timing out.
3. Each later call claims a slot from the pool and publishes its request there,
   waits for the response with a bounded timeout, and returns the Win32 error
   code the agent reported. Parallel host threads take different slots, so they
   do not serialize behind one lock; a slot whose wait times out is retired and
   never reused, so a late answer can never be read as another request's. The
   heartbeat is bumped while waiting, so an agent that has backed off to its
   `Sleep()` ceiling wakes quickly. The payload-carrying ops (above) copy their
   bytes through the slot, in before the request and out before idle.
4. Introspection runs on this same slot pool: `list_regions` loops `ENUM` batches
   (a 64 KiB payload holds 2048 region records), `list_modules` loops `MODULES`
   batches (a 64 KiB payload holds 1638 module records plus the names),
   `validate_memory` issues one `QUERY`, and `read_memory`/`write_memory` chunk
   at the payload size. Both list operations merge the walk with
   `/proc/<pid>/maps` for the host paths.
5. On session close the plugin waits for in-flight requests, then sends
   `SHUTDOWN`, which drains the remaining slots, frees everything the agent
   allocated and joins every worker; the region is unmapped and the per-session
   directory removed.

## Agent-served versus host-served

Allocation and module enumeration are agent-only. Wine's virtual-memory manager
must own an allocation, and only the target's own loader list names the images
the way Wine and the game do — a `/proc` module list is the host's view, so a
failure is returned, never patched over with a silently different list.
`validate_memory`, `read_memory` and `write_memory`, and the region list, prefer
the agent, because the host's view is exactly what Wine does not share — a
`/proc` region list and a read probe miss Wine's commit state.

When the target cannot run the agent, the call is routed instead of failing:

| State | `list_regions`, `validate_memory`, `read_memory`, `write_memory` |
| --- | --- |
| Target runnable | The agent (`ENUM` walk, `QUERY`, `READ`/`WRITE`). |
| Our own pause (`session.suspended`, the scanner's "pause the game") | Host: `platform::read_maps` and `session.mem` (`process_vm` then `/proc/<pid>/mem`). |
| Debug session attached, agent's threads exempt | The agent keeps serving: the attach leaves the agent's dispatcher and workers running (see "Debugging the target" below). |
| Debug session attached, agent's threads not recognised | Host, as for our own pause. |
| A request failed | The host path for this call, with the agent's reason logged. |

Module enumeration is the exception: it needs the agent, so while our own pause
has the target stopped, or a debug session stopped it and the agent's threads
could not be exempted, `list_modules` returns the **last agent-derived list
recorded for that session** — the target cannot have changed its loader list
while it is stopped — and only reports the reason when no list has been recorded
yet. `allocate` and `free` answer with a retryable busy error in the paused
state, and never disable the agent. The debugger itself is unaffected, because
`debug::Controller` keeps its attach-time `ui::ModuleSpans` snapshot.

A failed exchange no longer disables the agent for the session: it records the
reason and, once **every** advertised slot has been retired (the pool is
exhausted), drops the client so a later call may bring a fresh agent up, bounded
by three revives with a five second cooldown. A single timed-out slot leaves the
rest of the pool serving, and the introspection calls fall back to the host path
while no agent is available.

## Debugging the target

A debug attach seizes and stops every thread of the group, which would stop the
agent's dispatcher and workers with it. The attach therefore accepts an
exclusion list (`DebugSession::attach(excluded)`), and the wine-proton profile
fills it through the optional `debug_excluded_threads` hook: the excluded tids
are never seized, never programmed with debug registers and never waited on by
`resume`, `step` or `detach`, so the agent keeps serving reads, regions, modules
and validation while the rest of the target sits at a breakpoint.

`AgentService::agent_threads` resolves those tids, in order:

1. every thread whose `comm` is `slopkit-agent` (the DLL names each of its
   threads through `SetThreadDescription`, which the DLL resolves at run time
   because the export is not on every Wine build), or
2. every thread whose stack holds `SLOPKIT_AGENT_THREAD_MAGIC` inside the
   `SLOPKIT_AGENT_STACK_SCAN_BYTES` window below its `startstack`, read through
   the host memory path (no ptrace window, so it cannot collide with an
   allocation).

If neither matches — a Wine build where the description did not take, a stripped
agent — the attach proceeds unexempted with a warning, and the agent path then
behaves exactly as it did before. The trade-off is documented under known
limitations.

## Proton's environment handoff

A game started by Proton is not started by the shell: Steam runs Proton, Proton
runs its own wine, and that wine starts the game. Wine gives every process it
starts an already-connected wineserver socket through the environment
(`WINESERVERSOCKET=<fd>`), which saves the child a round trip to the server.

The descriptor number only means anything inside the process that owns it. The
injector this plugin forks has its own descriptor table, so it inherited the
variable but not the socket, and wine's loader refused to start it:

```
injector exited with 1: wine: Bad server socket 64 : Bad file descriptor
```

That made `alloc` fail for every Proton game while a directly started wine
process worked, which is why the wine-gated tests did not catch it. The
environment handed to the injector is therefore the target's own with the
handoff entries removed: the injector connects to the prefix's wineserver
itself, exactly the way a plain `wine app.exe` does. `WINESERVERSOCKET` is the
one entry Wine hands down this way today, and the filter is a named list rather
than a blanket `WINE*` strip, so `WINEPREFIX`, `WINEDLLOVERRIDES` and the rest
survive.

## Proton's private runtime directory

Steam runs a Proton game inside a pressure-vessel container, and that container
replaces `/run/user/<uid>` with a private tmpfs: the tool's runtime directory is
not a path the game shares, even though both sides call it `/run/user/1000` in
their own namespace. The two directories hold different things — seen through the
target's own root there is no staging directory, while the tool's own view has
one:

```
ls /proc/<game pid>/root/run/user/1000     -> no slopkit-agent-* directory
ls /run/user/1000                          -> slopkit-agent-<pid>-<tool pid>
```

The first cut staged the agent under `$XDG_RUNTIME_DIR`, so the game could not
see the staging directory at all. Wine resolved the staged injector through `Z:`,
got `STATUS_NO_SUCH_FILE`, and fell back to its own launcher:

```
wine: created the configuration directory
Application could not be started, or no application associated with the specified file.
ShellExecuteEx failed: File not found.
```

The staging directory is now the first of `$XDG_RUNTIME_DIR`, the temporary
directory and `$HOME/.cache/slopkit` that the target can reach through
`/proc/<pid>/root`, so a containerised target is staged in a directory the two
namespaces really do share. The check runs before the injector does, so an
unreachable directory is reported as a staging failure instead of a loader
failure.

The reported reason for a failed injector was the wrong line as well: the game's
own `WINEDEBUG` prints traces, so the first line of the capture was a `seh`
trace and never the injector's message. It now reports the injector's last
`slopkit-injector:` line, or its last non-empty line when the loader died before
the injector printed anything.

## What was measured

- The wine-gated end-to-end test (`tests/plugin/wine_proton_agent_e2e_test.cpp`)
  passes against Wine 11.0-staging on a warm prefix: probe launch, injection,
  `HELLO`, `alloc`, `alloc_near`, `query`, `protect`, a `WRITE`/`READ` round
  trip, a full region walk (`ENUM`), a full module walk (`MODULES`) that names
  the probe itself as the main image plus `kernel32.dll` and `ntdll.dll`,
  another `query`, `free` and `shutdown` in
  **0.61 s** wall clock for the whole run. `query` on the allocated block reports
  `MEM_COMMIT` + `PAGE_EXECUTE_READWRITE`, which is the property the ptrace path
  could not provide, and the walk contains both the block and the agent's own
  module base.
- The region walk costs one `ENUM` batch for a process with fewer than 2048
  regions — **0.92 ms** for the whole walk on the same prefix — and one round
  trip per 2048 records beyond that, because the 64 KiB payload holds that many
  `MEMORY_BASIC_INFORMATION` records.
- A 64-byte `READ` round trip averages **1.05 ms** over 2000 reads on a warm
  agent; the agent's 1 ms `Sleep()` poll dominates and the copy inside the target
  is negligible. A scan's read cost is therefore one poll per 64 KiB chunk,
  where `process_vm_readv` was one syscall — the deliberate price of
  Wine-consistent data, and the reason the host path still serves a stopped
  target.
- A single request after the handshake is bounded by the agent's poll: the host
  bumps the heartbeat while waiting, so the round trip stays within the 1 ms
  `Sleep()` floor rather than the 64 ms idle ceiling.
- `objdump -p` on the produced DLL and injector shows `kernel32.dll` as the only
  import, so neither needs a CRT, `libgcc_s` or a UCRT DLL.
- The Proton handoff failure was reproduced and closed locally: with the filter
  removed, the handoff regression test fails with
  `the injector exited with 1: wine: Bad server socket 15 : Bad file descriptor`
  and the environment check reports the handoff still present; with it in place
  the same test injects, allocates and shuts down.
- The private-runtime-directory failure was reproduced and closed against the
  reporter's own game: `/proc/<pid>/root/run/user/1000` contains no
  `slopkit-agent-*` directory, so an injector staged there dies in the loader
  with `ShellExecuteEx failed: File not found.`, while the same injector run from
  a directory under `/tmp` with the target's environment loads the agent into the
  game (`slopkit-injector: loaded into pid <windows pid>`).
- The agent and injector build warning-free for both bitnesses; `tools/verify.sh`
  covers them.

## Deviations from the original request

- **No ptrace fallback (owner request).** The first cut served a failed agent
  call through the shared ptrace allocator. The owner asked for the agent to be
  the only path, so injection, handshake or request failures now log the reason
  and return the error; a mapping Wine's virtual-memory manager cannot see is
  never produced. A later call may retry the injection, bounded by the revive
  policy below.
- **Introspection moved onto the agent too (owner request).** The first cut used
  the agent only to allocate. The region list, address validation and memory
  transfer now prefer the agent as well, because its `VirtualQuery` is Wine's own
  view; the host path answers only while the target is stopped by a debug
  session or by our own in-flight call (see "Agent-served versus host-served").
- **Injection is lazy, on the first allocation, not eager at session open.** The
  confirmed design said "eager at session open", but `plugin_open_session` runs
  on the UI thread and `docs/UI_DESIGN.md` forbids touching a target there.
  Injecting eagerly would mean spawning wine and waiting for a remote
  `LoadLibraryW` on the UI thread; the agent therefore comes up on the first
  allocation, which runs on the access worker. Failures are still learned at the
  first allocation; a later call retries under the revive policy.
- **The per-session directory is chosen for the target, not for the tool.** The
  installed plugin directory may be read-only, and the DLL must find the region
  as its own sibling, so region, DLL and injector are staged together in a
  writable directory *the target's own namespace can see*, not simply in the
  tool's runtime directory.
- **No AF_UNIX/Winsock transport.** As agreed, the file-backed shared region was
  built first and proved sufficient; the socket transport stays the documented
  backup if a prefix without a `Z:` mapping to `/` ever shows up.
- **Failure is retryable, not permanent (owner request).** The first cut disabled
  the agent for the session on any failure. A transient failure now records the
  reason and, once the slot pool is exhausted, drops the client so a later call
  may inject a fresh agent, bounded by three revives with a five second cooldown;
  the failing call still returns the reason.
- **Several requests at once (owner request).** A single request slot and one
  global lock serialized every call. The region now carries four independent
  slots served by a lazily grown worker pool, and the service runs exchanges
  without a global lock, so parallel host reads are served in parallel.
- **A stopped target is not a failure, and a debug attach exempts the agent
  (owner request).** Our own SIGSTOP pause used to produce a 500 ms timeout that
  disabled the agent; it is now routed to the host path (with `modules` answered
  from the recorded list). A debug attach leaves the agent's own threads running
  so the agent keeps serving (see "Debugging the target").
- **The e2e test is hidden and environment-gated** (`SLOPKIT_WINE_E2E=1`) rather
  than part of the default suite, because it needs wine, a prefix and mingw-w64.

## Known limitations

- **Anti-cheat visibility.** The agent is a DLL plus a thread inside the target,
  so it appears in the target's module list. Nothing here hides it; a protected
  game may refuse to run with it loaded. The thread now polls during a scan as
  well as during an allocation, so an already visible DLL is also a visibly busy
  one while memory is being read.
- **A crashed host leaves the agent dormant, not gone.** The region is unmapped
  and its file unlinked, but the target keeps the module until it exits; the
  agent's threads drop to their `Sleep()` ceiling and cost almost no CPU. It can
  no longer be shut down from the tool.
- **`Z:` is assumed.** The region is opened through Wine's default `Z:` drive.
  A prefix with that mapping removed makes the agent unusable; the handshake
  times out and allocation fails for that session.
- **The staging directory must be one the target shares.** `$XDG_RUNTIME_DIR`,
  the temporary directory and `$HOME/.cache/slopkit` are tried in that order; a
  target whose container shares none of them fails with a staging error.
- **The i686 path is built, not exercised.** Both bitnesses cross-compile
  warning-free and the selector reads the guest's COFF machine, but no 32-bit
  game was available here to run the end-to-end test for i686.
- **A revive loses the previous client's ownership table.** `free` for a block
  handed out before a revive reports "not this session's allocation" and the
  block leaks inside the target until it exits. The alternative was losing every
  introspection path for the session, so this is the better trade; it is bounded
  by the revive cap.
- **The revive policy is bounded but not free.** A hardened target is injected at
  most three times per session, never twice within five seconds; after the cap
  the agent path stays unavailable for that session (allocation, `free` and
  `modules` report the reason, the introspection calls keep using the host path).
- **An exempt agent thread is not traced.** A software breakpoint the user arms
  in code that thread executes (the agent's own module, or a system DLL it calls)
  would kill that thread, since the thread is not under ptrace. The agent runs
  only its own module and system calls, the loss is detected as an exhausted pool
  and handled by the revive policy, and everything else in the target is traced
  as before.
- **Agent-thread identification is a heuristic.** If neither the `comm` name nor
  the stack marker matches (a Wine layer difference, a stripped agent), the debug
  attach proceeds without exemptions and logs a warning; the agent path then
  behaves exactly as it did before the attach, and a stopped target falls back to
  the host path.
- **The module-list cache is per pid and cleared on close.** While the target is
  paused, `list_modules` answers from it; a session that pauses before it has
  ever listed modules still reports the reason.
- **The module walk reads the loader list without the loader lock.** The agent
  cannot call `LdrLockLoaderLock` (it is not exported to a kernel32-only DLL), so
  a module unloaded concurrently could point at freed memory. The walk is bounded
  and skips entries with a null `DllBase`, and the game's own images — which are
  never unloaded — are read first, so the realistic failure is a short or empty
  list, never a fault or a spin.
- **Module enumeration is agent-only, with a recorded fallback.** While our own
  pause has the target stopped, a debug session stopped it and the agent's
  threads could not be exempted, `list_modules` answers from the last
  agent-derived list recorded for the session instead of a host-derived one; only
  a session that has never recorded a list reports the reason. The region list is
  not affected: it falls back to `/proc/<pid>/maps`, so the memory-map job keeps
  producing a region list. The debugger itself keeps its attach-time module
  snapshot for name resolution.
