# Anti-detection

Undetectability is a core design principle, not an afterthought. A target process
should not be able to tell that slopkit is inspecting it, so the default
read/write path never attaches to the target. The technique categories:

- **No `ptrace` attach on the default path** — scanning never stops the target,
  so nothing like `TracerPid` appears in `/proc/<pid>/status`. `ptrace` exists
  only in the opt-in debugger: it is started explicitly for the already-attached
  target — from `Start Debugging` or, since the watch commands attach on demand,
  only after the user confirms the attach prompt — the session is reported in the
  log at start and stop, and `Stop Debugging` detaches every thread and resumes
  the target, leaving `TracerPid` empty again. The listing command's register
  capture stops the target just long enough to read the register file and resume,
  and the target sees that stop like any other debugger interruption.
- **`process_vm_readv` / `process_vm_writev` first** — the preferred primitive,
  with a `/proc/<pid>/mem` pread/pwrite fallback when the syscall is unavailable
  or refused.
- **No artifacts in the target** — slopkit does not inject code, load libraries
  into or leave handles behind in the target process.
- **Kernel read permission** — with the Linux Yama LSM at its default
  `ptrace_scope=1`, `process_vm_readv` / `/proc/<pid>/mem` are permitted only for
  a descendant of the target. The practice target is started as slopkit's own
  child (in its own session, so it still outlives slopkit) for exactly that
  reason; a target slopkit did not start shows as `Memory: not readable` in the
  Process List details, and a table auto-attach says so on the status line.
  Relaxing `kernel.yama.ptrace_scope` or granting `CAP_SYS_PTRACE` is the
  user-side alternative.
- **Per-plugin access strategies** — each plugin advertises the methods it can
  use, and the method actually used is reported per session, so the least
  detectable path can be preferred.
- **Optional out-of-process transport** — plugins are designed to run embedded
  today and behind IPC later, so the inspection agent itself can be separated
  from the UI.

The concrete method is left to each plugin; a plugin may legitimately use a more
detectable primitive while it is being brought up, as long as it reports that.
The debugger is the sanctioned case: both `linux-proc` and `wine-proton` use
`ptrace` only while an explicitly started debug session is open, and each reports
that session in the log.
