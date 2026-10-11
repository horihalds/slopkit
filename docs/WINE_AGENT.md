# Wine/Proton resident agent

`wine-proton` allocates target memory *inside* the target, through a small
resident agent DLL, instead of running a raw `mmap` in the ptrace injection
window. Wine's own virtual-memory manager owns the mapping, so `VirtualQuery`
reports it as committed and the prefix never places its own allocation on top of
it. The ptrace allocator stays as the automatic fallback behind the same
interface. `docs/ARCHITECTURE.md` owns the ABI-level description of
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
| Target derivation | `src/plugins/wine_proton/wine_target.{hpp,cpp}` | From `/proc/<pid>`: the guest's main PE (for the bitness and the `Z:` image path), the loader to host the injector, the target's own environment and a `WINEPREFIX` fallback. |
| Transport | `src/plugins/wine_proton/agent_transport.{hpp,cpp}` | Creates and mmaps the region, publishes one request, waits with a bounded timeout, returns the response. |
| Client | `src/plugins/wine_proton/agent_client.{hpp,cpp}` | Locates the binaries, spawns the injector under the target's wine, runs the `HELLO` handshake, and serves the operations; tracks the addresses it handed out. |
| Allocator | `src/plugins/wine_proton/agent_allocator.{hpp,cpp}` | Agent-first `allocate`/`free` for the profile hooks, permanent ptrace fallback per session, logging of which path served each call, teardown on session close. |

## Flow

1. The first `allocate_memory` for a Wine target derives the target, stages the
   bitness-matched DLL and injector into a per-session directory under
   `$XDG_RUNTIME_DIR` (or `/tmp`), creates the region file there and runs the
   injector under the loader the target itself runs, with the target's own
   environment. Any failure here disables the agent for that session, logs why,
   and the call is served by ptrace.
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
- The agent and injector build warning-free for both bitnesses; `tools/verify.sh`
  covers them.

## Deviations from the original request

- **Injection is lazy, on the first allocation, not eager at session open.** The
  confirmed design said "eager at session open", but `plugin_open_session` runs
  on the UI thread and `docs/UI_DESIGN.md` forbids touching a target there.
  Injecting eagerly would mean spawning wine and waiting for a remote
  `LoadLibraryW` on the UI thread; the agent therefore comes up on the first
  allocation, which runs on the access worker. Failures are still learned at the
  first allocation and the session falls back to ptrace for good.
- **The per-session directory is under `$XDG_RUNTIME_DIR` (else `/tmp`), not next
  to the plugin.** The installed plugin directory may be read-only, and the DLL
  must find the region as its own sibling, so region, DLL and injector are staged
  together in a writable directory the target can reach through `Z:`.
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
  times out and the plugin falls back to ptrace.
- **The i686 path is built, not exercised.** Both bitnesses cross-compile
  warning-free and the selector reads the guest's COFF machine, but no 32-bit
  game was available here to run the end-to-end test for i686.
- **The fallback does not undo Wine's view.** If the agent fails after handing
  out an address that the session then frees, the ptrace fallback unmaps it with
  `munmap`, which works because Wine implements `VirtualAlloc` with `mmap`, but
  Wine's own bookkeeping is only as accurate as that assumption.
