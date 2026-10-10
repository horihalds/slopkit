# Logging

`slopkit::log` is the project's observability path. Every layer records what it
decided and why, so the `View > Log` window and the rolling log file explain a
session without a debugger. This document is the convention every call site
follows; the category names live in `src/core/log_categories.hpp`.

## Levels

- `debug` — high-frequency or diagnostic detail: per-job submit/execute/complete,
  per-read and per-write outcomes, page loads, matcher chunk progress.
- `info` — a lifecycle step or state change completed: startup and teardown,
  plugin discovery summary, attach/detach, scan phase start and completion,
  address-table mutations and load/save summaries.
- `warning` — recoverable degradation: a rejected plugin, an unreadable region,
  a refused attach, a malformed table line, a settings file that could not be
  read.
- `error` — the requested operation failed outright.

The default view is `info`, so record at `debug` anything that would flood it;
hot paths (the 50 ms poll tick, per table row, per 4 KiB read, per matcher chunk)
must not log at `info`. Where the volume would otherwise be unbounded, log an
aggregated summary instead of one record per item.

Every failure is logged exactly once, at the layer that decides what to do about
it. For example, `PluginHost` logs a rejected library, not `Plugin::load`.

## Categories

The category vocabulary is `src/core/log_categories.hpp`; use the constants, never
string literals.

- `app` — CLI, startup/shutdown, settings.
- `plugin` — plugin discovery and loading, plus every message a plugin sends
  through the host services log hook.
- `process` — process inventory, probe, attach and the access-worker job queue.
- `memory` — target reads and writes, and memory-page loads.
- `scan` — the scan engine and matcher lifecycle.
- `table` — the address table and its files.
- `script` — a Lua script entry: run submit/execute/complete, the lines it printed,
  its first error, each `alloc`/`dealloc` of target memory, and the dialog's
  compile-only syntax check (an info line when it passes, a warning when it does
  not). A ticked script's `update` writes nothing on a successful tick and one
  `warning` when a tick fails and switches the script off; no record is written per
  tick.
- `debug` — the opt-in debug session: start/stop, stops, software and hardware
  breakpoint arming and every debug failure.
- `ui` — user actions in the Qt layer.

## Message style

- Compose the message at the call site with `std::format`.
- One record is one line: never embed a newline (the Log window's Save As writes
  one record per line).
- Never write raw target memory contents, whole environment dumps or other
  sensitive data; log addresses, sizes, ids and error text only.
- Never call the logger from inside a sink: a sink runs while the logger's lock
  is held, so logging from it would deadlock.
