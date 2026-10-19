# Wine/Proton resident agent

`wine-proton` allocates target memory *inside* the target, through a small
resident agent DLL, instead of running a raw `mmap` in the ptrace injection
window. Wine's own virtual-memory manager owns the mapping, so `VirtualQuery`
reports it as committed and the prefix never places its own allocation on top of
it. The agent is the only allocation path: a failure is an error, never a silent
raw `mmap`.

The same agent supplies the target's *introspection*: the region list is the
agent's `VirtualQuery` walk and the module list is the agent's walk of Wine's own
loader list. Reads and writes never go through the agent: the host's
`process_vm_readv`/`process_vm_writev`, then `/proc/<pid>/mem`, move the bytes,
exactly as they do for `linux-proc`, so a scan costs one host syscall per chunk
and the DLL stays idle while memory moves. Address validation uses the same
host-side chunked probe. `/proc/<pid>/maps` is still read, but only to merge host
paths onto the agent's regions and modules, and to answer the region list while
the target cannot run (see "Agent-served versus host-served"). `docs/ARCHITECTURE.md`
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
| Protocol header | `src/plugins/wine_proton/agent/slopkit_agent_protocol.h` | The one contract (version 7): the fixed header, the ring of `SLOPKIT_AGENT_SLOT_COUNT` request slots — each with its own atomic state word, request/response record and 64 KiB payload — the trailing in-process debug block (whose software-trap table holds `SLOPKIT_AGENT_DEBUG_MAX_TRAPS` entries) and the op/status/page constants. Compiled by mingw-w64 and by the Linux plugin. |
| Agent DLL | `src/plugins/wine_proton/agent/slopkit_agent.c` | C11, `-nostdlib`, kernel32 only. `DllMain` starts one dispatcher thread; the dispatcher maps the region file next to the DLL, claims every slot in the request state and hands it to a worker created lazily (up to the worker cap, never under Wine's loader lock), and the workers serve `HELLO`, `ALLOC`, `ALLOC_NEAR`, `FREE`, `PROTECT`, `QUERY`, `ENUM`, `MODULES`. Each worker answers its own slot and publishes the response with a release store. `ENUM` walks `VirtualQuery` into that slot's payload; `MODULES` walks the target's own `InLoadOrderModuleList`. `SHUTDOWN` is answered by the dispatcher, which drains in-flight slots, frees everything, joins every worker and unmaps. |
| Injector | `src/plugins/wine_proton/agent/slopkit_injector.c` | One-shot exe run under the target's own wine, in two calls around the host write (see "Wine's write into a Proton game" below): `--allocate <bytes>` finds the target by image path, is idempotent when the agent is already loaded and returns the block `VirtualAllocEx` reserved in it; `--load <address>` runs `CreateRemoteThread(LoadLibraryW)` on that block, writing the DLL path itself with `WriteProcessMemory` only when the plugin hands it over instead. |
| Test probe | `src/plugins/wine_proton/agent/slopkit_probe.c` | A trivial console target for the wine-gated end-to-end test; never installed. |
| Cross-compile | `cmake/Agent.cmake` | One pair of targets per bitness (`x86_64`, `i686`), a clear error when mingw-w64 and no prebuilt exist, and the install rule. |
| Target derivation | `src/plugins/wine_proton/wine_target.{hpp,cpp}` | From `/proc/<pid>`: the guest's main PE (for the bitness and the `Z:` image path), the loader to host the injector, the target's own environment (without the process-local handoff entries below), a `WINEPREFIX` fallback and the staging directory the target's own namespace can reach. The same one-module selection also names the target in the process list: `guest_image_name` returns that PE image's file name with its extension, so a claimed pid reads `game.exe` rather than the loader's 15-character comm or its exe basename, and a `NameCache` remembers the name per pid for a five-second TTL so the picker's two-second refresh does not re-walk every claimed target — a name is therefore up to the TTL stale, including after a pid is reused. A claimed pid with no identifiable executable image (a `proton`, `pressure-vessel` or `wine64-preloader` wrapper) keeps the comm-then-exe-basename fallback instead of an empty name. |
| Transport | `src/plugins/wine_proton/agent_transport.{hpp,cpp}` | Creates and mmaps the region and exposes a pool of request slots. `acquire()` hands out a free slot and `Slot::transact()` publishes on it, waits with a bounded timeout and bumps the heartbeat; the request payload is copied into the slot before publishing and the response payload out before the state returns to idle, so one slot never races another. Capacity is the count `HELLO` advertised (one when it reported none). A slot whose exchange times out is retired and never reused, so a late answer can never be read as the next request's. |
| Client | `src/plugins/wine_proton/agent_client.{hpp,cpp}` | Locates the binaries, stages them where the target can see them, spawns the injector under the target's wine once to reserve the DLL path's block and once — after placing the UTF-16 `Z:` path there through the host's own view of the target, or letting the injector write it when even the host cannot — to run `LoadLibraryW` on it, runs the `HELLO` handshake (refusing a version, record-size or payload-size mismatch, and sizing the slot pool from the advertised capacity), and serves `allocate`, `allocate_near`, `free`, `protect`, `query`, `enumerate` and `modules`; tracks the addresses it handed out under a mutex and reports whether every slot is retired. |
| Service | `src/plugins/wine_proton/agent_service.{hpp,cpp}` | The per-pid owner and the profile hooks that still need the target's own manager: `allocate`/`free` (agent-only), `modules` and `regions`. Parallel calls run without a global lock, lazy bring-up, the bounded revive policy, the stopped-target routing, the module-list cache, the in-flight counter `close` waits on, and `close`. |
| Region merge | `src/plugins/wine_proton/agent_regions.{hpp,cpp}` | The pure merge of the agent's walk with `/proc/<pid>/maps`: boundaries, commit state and protection come from the agent, `path`, `offset`, `shared` and `deleted` from the overlapping procfs line. |
| Module merge | `src/plugins/wine_proton/agent_modules.{hpp,cpp}` | The pure merge of the agent's loader walk with `/proc/<pid>/maps`: `base`, image size, entry point, Wine's base name and the main flag come from the loader, `path` and `offset` from the overlapping procfs line; also the UTF-16 to UTF-8 name decoder. |

## Flow

1. The first call that needs the agent for a Wine target — an allocation, or an
   introspection call while the target is runnable — derives the target, stages
   the bitness-matched DLL and injector into a per-session directory the target
   can reach (see "Proton's private runtime directory" below), creates the region
   file there and runs the injector under the loader the target itself runs — once
   to reserve the block that is to carry the DLL path, then again to load it (see
   "Wine's write into a Proton game" below) — with the target's own environment,
   minus the process-local handoffs described below. One launch serves the whole
   process: a call that arrives while a bring-up is already in flight waits for it
   (bounded by the launch timeout) instead of being told the agent is unavailable.
   A failure here records the reason and is retried by a later call,
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
   `Sleep()` ceiling wakes quickly. `ENUM` copies its region records out of the
   slot's payload before the state returns to idle.
4. Introspection runs on this same slot pool: `list_regions` loops `ENUM` batches
   (a 64 KiB payload holds 2048 region records) and `list_modules` loops `MODULES`
   batches (a 64 KiB payload holds 1638 module records plus the names). Both list
   operations merge the walk with `/proc/<pid>/maps` for the host paths. Reads,
   writes and `validate_memory` never reach the slot pool: they run entirely on
   the host.
5. On session close the plugin waits for in-flight requests, then sends
   `SHUTDOWN`, which drains the remaining slots, frees everything the agent
   allocated and joins every worker; the region is unmapped and the per-session
   directory removed.

## Agent-served versus host-served

Allocation and module enumeration are agent-only. Wine's virtual-memory manager
must own an allocation, and only the target's own loader list names the images
the way Wine and the game do — a `/proc` module list is the host's view, so a
failure is returned, never patched over with a silently different list.

Reads, writes and address validation are always host-served. They run on
`session.mem` (`process_vm_readv`/`process_vm_writev`, then `/proc/<pid>/mem`)
and on the shared chunked readability probe, in every target state, so a scan
never waits on the agent's polling loop, a slot timeout or a revive:

| Call | Served by |
| --- | --- |
| `allocate`, `free` | The agent's `VirtualAlloc`/`VirtualFree`; a failed exchange returns the error, never a raw `mmap`. |
| `list_regions` | The agent's `ENUM` walk merged with `/proc/<pid>/maps`; a plain `/proc/<pid>/maps` while the agent cannot serve. |
| `list_modules` | The agent's `MODULES` walk, or the last recorded list while the agent cannot serve. |
| `validate_memory`, `read_memory`, `write_memory` | The host: the chunked readability probe and `session.mem`. |

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
rest of the pool serving; the host-served calls never touch the pool at all.

## Debugging the target

A debug attach seizes and stops every thread of the group, which would stop the
agent's dispatcher and workers with it. The attach therefore accepts an
exclusion list (`DebugSession::attach(excluded)`), and the wine-proton profile
fills it through the optional `debug_excluded_threads` hook: the excluded tids
are never seized, never programmed with debug registers and never waited on by
`resume`, `step` or `detach`, so the agent keeps serving allocation, the region
walk and the module walk while the rest of the target sits at a breakpoint.

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
the injector printed anything. The shared host line for a failed module list
(`could not list modules: I/O error`) only ever carries the error code, so the
plugin logs the recorded reason next to it as well — and only for the first
occurrence of that reason, because a caller that retries the listing would
otherwise repeat the same warning on every attempt.

## Wine's write into a Proton game

The agent's memory work crosses two namespaces, and only one of them can write
into the game. For the reporter's Proton game, Wine's own cross-process write
failed:

```
warning [process] wine-proton agent for pid 312355 could not start (the injector
exited with 3: slopkit-injector: WriteProcessMemory failed)
warning [process] could not list modules: I/O error
```

The injector's `OpenProcess`, `VirtualAllocEx` and its enumeration of the game
all still worked, so the block was reserved in the game's address space — but the
write into it came back as `ERROR_ACCESS_DENIED`, the same game whose memory
slopkit's own `process_vm_writev` and `/proc/<pid>/mem` reach without trouble.
The agent never started, the module list stayed empty, and every script that
followed then searched the whole address space instead of one module until its
time budget expired.

The block therefore belongs to the plugin: `--allocate` reserves it and prints
its address, the plugin writes the UTF-16 `Z:` DLL path into it through the path
it already uses for every read and write, and `--load` starts `LoadLibraryW` on
it. A target the *host* cannot reach either — the wine-gated "inherited a
wineserver socket" test stands in for one — is still injected, because the plugin
then hands the path to the `--load` call and the injector's own
`WriteProcessMemory` places it. Each direction names which side refused, so a
failure is never silent.

## What was measured

- The wine-gated end-to-end test (`tests/plugin/wine_proton_agent_e2e_test.cpp`)
  passes against Wine 11.0-staging on a warm prefix: probe launch, injection,
  `HELLO`, `alloc`, `alloc_near`, `query`, `protect`, a host write and read-back
  of the block, a full region walk (`ENUM`), a full module walk (`MODULES`) that names
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
- A scan's read cost is one `process_vm_readv` syscall per chunk, falling back to
  `/proc/<pid>/mem`, with no per-64 KiB agent round trip. The removed agent
  `READ` averaged **1.05 ms** per 64 bytes over 2000 reads on a warm agent, where
  the agent's 1 ms `Sleep()` poll dominated and the copy inside the target was
  negligible — the price the host path no longer pays.
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
- The Proton write failure was reproduced against the reporter's own game:
  `VirtualAllocEx` reserved a block in the game, the host wrote the DLL path into
  it and the injector's `--load` mapped the staged DLL into the game
  (`slopkit-injector: loaded into pid <windows pid>`, with the DLL then present in
  the game's `/proc/<pid>/maps`), while Wine's own `WriteProcessMemory` into the
  same block answered `ERROR_ACCESS_DENIED`.
- The agent and injector build warning-free for both bitnesses; `tools/verify.sh`
  covers them.

## Deviations from the original request

- **No ptrace fallback (owner request).** The first cut served a failed agent
  call through the shared ptrace allocator. The owner asked for the agent to be
  the only path, so injection, handshake or request failures now log the reason
  and return the error; a mapping Wine's virtual-memory manager cannot see is
  never produced. A later call may retry the injection, bounded by the revive
  policy below.
- **Introspection moved onto the agent too (owner request, later partly
  reversed).** The first cut used the agent only to allocate. The region list,
  address validation and memory transfer then moved onto the agent, because its
  `VirtualQuery` is Wine's own view. The region list and the module walk stayed
  there; reads, writes and validation left again (next entry).
- **Reads, writes and validation moved back to the host (owner request).** The
  agent used to copy bytes and answer `QUERY` for validation, so a scan spent one
  agent poll per 64 KiB chunk and could be delayed or rerouted by a slot timeout,
  a retired slot or a revive. The owner asked for the transfer to leave the agent
  end to end: the DLL, the protocol (bumped to version 5) and the client no
  longer declare `READ`/`WRITE`, and reads, writes and validation are served by
  the host `process_vm`/procfs path and the shared chunked probe.
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
  game may refuse to run with it loaded. The threads poll only while an allocation
  or an introspection walk is in flight, so a scan reads through the host and
  leaves the DLL idle.
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
  the agent path stays unavailable for that session (allocation, `free`,
  `modules` and the region walk report the reason; reads, writes and validation
  never needed the agent).
- **An exempt agent thread is not traced.** A software breakpoint the user arms
  in code that thread executes (the agent's own module, or a system DLL it calls)
  would kill that thread, since the thread is not under ptrace. The agent runs
  only its own module and system calls, the loss is detected as an exhausted pool
  and handled by the revive policy, and everything else in the target is traced
  as before.
- **Agent-thread identification is a heuristic.** If neither the `comm` name nor
  the stack marker matches (a Wine layer difference, a stripped agent), the debug
  attach proceeds without exemptions and logs a warning; the agent path then
  behaves exactly as it did before the attach, and a stopped target is served by
  the host path as always.
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
