#pragma once

/* The agent's in-process debugger. It registers a vectored exception handler,
   runs the target's own threads up to a trap, freezes the rest of the process,
   and serves the host's debug commands through the region's debug block until
   it is told to resume. The engine is C11 and CRT-free, like the rest of the
   agent: kernel32 only, no libc. */

#include "slopkit_agent_protocol.h"

/* Clears any leftover debug state (called once the region is open) so a
   re-injected agent never mistakes a stale block for a live session. */
void slopkit_debug_reset(slopkit_agent_region* region);

/* Notes the calling thread as an agent thread, so the debugger never freezes
   it. Every agent thread calls this as it starts. */
void slopkit_debug_note_agent_thread(void);

/* Serves SLOPKIT_AGENT_OP_DEBUG_ATTACH: claims the block, registers the VEH,
   starts the watchdog, freezes every target thread and publishes the initial
   interrupt stop. It returns as soon as the stop is published; the watchdog
   serves the commands that follow. Returns 0 on success. */
int slopkit_debug_attach(slopkit_agent_region* region);

/* Serves SLOPKIT_AGENT_OP_DEBUG_DETACH: unpatches every trap, clears the debug
   registers, thaws the process, removes the VEH and stops the watchdog.
   Returns 0 on success. */
int slopkit_debug_detach(slopkit_agent_region* region);

/* True while a debug session holds the block. */
int slopkit_debug_active(void);
