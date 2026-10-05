# Anti-detection

Undetectability is a core design principle, not an afterthought. A target process
should not be able to tell that slopkit is inspecting it, so the default
read/write path never attaches to the target. The technique categories:

- **No `ptrace` attach** — the default path does not stop the target, so nothing
  like `TracerPid` appears in `/proc/<pid>/status`.
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
