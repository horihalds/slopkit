# Wine/Proton resident agent

`wine-proton` allocates target memory *inside* the target, through a small
resident agent DLL, instead of running a raw `mmap` in the ptrace injection
window. Wine's own virtual-memory manager owns the mapping, so `VirtualQuery`
reports it as committed and the prefix never places its own allocation on top of
it. The agent is the only allocation path: a failure is an error, never a silent
raw `mmap`. `docs/ARCHITECTURE.md` owns the ABI-level description of
`allocate_memory`; this file owns the agent's design, what was measured, the
deviations from the original request and the known limitations.

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
| Protocol header | `src/plugins/wine_proton/agent/slopkit_agent_protocol.h` | The one contract: a packed little-endian request/response record, the header with the atomic state word, and the op/status/page constants. Compiled by mingw-w64 and by the Linux plugin. |
| Agent DLL | `src/plugins/wine_proton/agent/slopkit_agent.c` | C11, `-nostdlib`, kernel32 only. `DllMain` starts one service thread; the thread maps the region file next to the DLL and serves `HELLO`, `ALLOC`, `ALLOC_NEAR`, `FREE`, `PROTECT`, `QUERY`, `SHUTDOWN`. |
| Injector | `src/plugins/wine_proton/agent/slopkit_injector.c` | One-shot exe run under the target's own wine: finds the target by image path, is idempotent when the agent is already loaded, then `OpenProcess` / `VirtualAllocEx` / `WriteProcessMemory` / `CreateRemoteThread(LoadLibraryW)`. |
| Test probe | `src/plugins/wine_proton/agent/slopkit_probe.c` | A trivial console target for the wine-gated end-to-end test; never installed. |
| Cross-compile | `cmake/Agent.cmake` | One pair of targets per bitness (`x86_64`, `i686`), a clear error when mingw-w64 and no prebuilt exist, and the install rule. |
| Target derivation | `src/plugins/wine_proton/wine_target.{hpp,cpp}` | From `/proc/<pid>`: the guest's main PE (for the bitness and the `Z:` image path), the loader to host the injector, the target's own environment (without the process-local handoff entries below), a `WINEPREFIX` fallback and the staging directory the target's own namespace can reach. |
| Transport | `src/plugins/wine_proton/agent_transport.{hpp,cpp}` | Creates and mmaps the region, publishes one request, waits with a bounded timeout, returns the response. |
| Client | `src/plugins/wine_proton/agent_client.{hpp,cpp}` | Locates the binaries, stages them where the target can see them, spawns the injector under the target's wine, runs the `HELLO` handshake, and serves the operations; tracks the addresses it handed out. |
| Allocator | `src/plugins/wine_proton/agent_allocator.{hpp,cpp}` | The `allocate`/`free` profile hooks: the agent is the only path, so a failure disables the agent for the session and returns the error, and `close` tears the agent down. |

## Flow

1. The first `allocate_memory` for a Wine target derives the target, stages the
   bitness-matched DLL and injector into a per-session directory the target can
   reach (see "Proton's private runtime directory" below), creates the region
   file there and runs the injector under the loader the target itself runs, with
   the target's own environment, minus the process-local handoffs described
   below. Any failure here disables the agent for that session, logs why, and the
   call is answered with the error — there is no ptrace fallback.
2. The agent's service thread opens `slopkit-agent.bin` as the sibling of its own
   module through `Z:` and polls the header's state word. `HELLO` reports the
   protocol version, the record size, the pointer size, the Windows pid, the
   allocation granularity and the agent's base address; a version, record-size or
   pointer-size mismatch fails the handshake immediately instead of timing out.
3. Each later call publishes one request, waits for the response with a bounded
   timeout, and returns the Win32 error code the agent reported. The heartbeat
   is bumped while waiting, so an agent that has backed off to its `Sleep()`
   ceiling wakes quickly.
4. On session close the plugin sends `SHUTDOWN`, which frees everything the
   agent allocated and stops the service thread; the region is unmapped and the
   per-session directory removed.

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
  `HELLO`, `alloc`, `alloc_near`, `query`, `protect`, `free` and `shutdown` in
  **0.65 s** wall clock for the whole run. `query` on the allocated block reports
  `MEM_COMMIT` + `PAGE_EXECUTE_READWRITE`, which is the property the ptrace path
  could not provide.
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
  the only path, so injection, handshake or request failures now log the reason,
  disable the agent for the session and return the error; a mapping Wine's
  virtual-memory manager cannot see is never produced.
- **Injection is lazy, on the first allocation, not eager at session open.** The
  confirmed design said "eager at session open", but `plugin_open_session` runs
  on the UI thread and `docs/UI_DESIGN.md` forbids touching a target there.
  Injecting eagerly would mean spawning wine and waiting for a remote
  `LoadLibraryW` on the UI thread; the agent therefore comes up on the first
  allocation, which runs on the access worker. Failures are still learned at the
  first allocation, and from then on allocation fails for the session.
- **The per-session directory is chosen for the target, not for the tool.** The
  installed plugin directory may be read-only, and the DLL must find the region
  as its own sibling, so region, DLL and injector are staged together in a
  writable directory *the target's own namespace can see*, not simply in the
  tool's runtime directory.
- **No AF_UNIX/Winsock transport.** As agreed, the file-backed shared region was
  built first and proved sufficient; the socket transport stays the documented
  backup if a prefix without a `Z:` mapping to `/` ever shows up.
- **The e2e test is hidden and environment-gated** (`SLOPKIT_WINE_E2E=1`) rather
  than part of the default suite, because it needs wine, a prefix and mingw-w64.

## Known limitations

- **Anti-cheat visibility.** The agent is a DLL plus a thread inside the target,
  so it appears in the target's module list. Nothing here hides it; a protected
  game may refuse to run with it loaded.
- **A crashed host leaves the agent dormant, not gone.** The region is unmapped
  and its file unlinked, but the target keeps the module until it exits; the
  service thread drops to its `Sleep()` ceiling and costs almost no CPU. It can
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
- **A failed agent is permanent for the session.** Once injection, the handshake
  or any request fails, the agent is not retried and every later allocation for
  that target fails until the session (or the process) is reopened; there is no
  ptrace path to fall back to.
