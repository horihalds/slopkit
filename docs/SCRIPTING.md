# Lua scripting

A **script entry** is an address-table row that holds Lua source instead of a
value. It has no address, no value and no type: the row's Type column reads
`script`, and its Address and Value cells stay blank. A script travels inside the
`.skt` file with the rest of the table (`entries/<name>.lua` holds the source), so
a script you add and `Save Table` comes back when you reopen the table.

Scripts run against the attached target through a small memory API built on the
system Lua that slopkit links against. Everything else is ordinary Lua: the
globals this guide lists are the only additions to the standard library.

## Adding and editing a script

- `File > Add Script…`, or `Add Script…` in the address list's right-click menu,
  opens the **Add Script** dialog: a `Description` field and a Lua editor seeded
  with a commented `activate`/`deactivate` skeleton.
- `Edit Script…` on a script row opens the same dialog in **Edit Script** mode;
  `Save` replaces the entry's description and source.
- The description must not be blank, and an empty description is refused.
- Both modes work with **no target attached** — a script is text, not a live
  connection.

### The editor

The editor is built for Lua: syntax highlighting, a line-number gutter, the
caret's current line highlighted, and a matched-bracket highlight when the caret
touches a `)`/`]`/`}` or its opener. `Tab` indents to the next four-space stop,
`Shift+Tab` removes one stop, and `Enter` keeps the current line's indentation.

### Verify before you run

`Verify` compiles the editor's text **without running it and without touching the
target**:

- valid Lua → `Syntax OK.`;
- rejected source → the compiler's own message, for example
  `script:3: 'end' expected ...`, with the offending line marked in the gutter and
  the caret moved to it.

Editing the source clears the previous verdict. `Verify` never changes the entry,
never closes the dialog and needs no attached target, so it is safe to use before
there is anything to run against.

`Verify` reports only what the Lua compiler rejects. An undefined global, a
missing `activate()` hook or a bad `mem.read` type token still compile; they are
reported only when the script actually runs.

## Renaming a script

A script row's `Description` cell can be renamed in the grid exactly like a value
row's: double-click it or select the row and press `F2`, type the new name and
commit. A blank or whitespace-only name is refused and reported on the status
line, and `Save Table` writes whatever name the entry carries.

## Running a script

`Run Script` in a script row's right-click menu runs the whole chunk against the
attached target. It needs a live session, and only one script runs at a time;
while one is in flight the status line shows `Running script…`.

- The lines the script `print`s appear in the **Log** window (`View > Log`),
  under the `script` category.
- The status line shows a short outcome: `Script ok.`, `Script returned <value>.`
  when the chunk returns a scalar, or `Script failed: <message>` when it raises an
  error.
- A script's globals persist between runs on one target and are dropped when the
  target detaches.

## The Active checkbox and lifecycle hooks

Every script row's leading `Active` checkbox drives two hooks by name:

- ticking it runs the script's `activate()`;
- unticking it runs `deactivate()`.

The chunk runs first, and then the named global is called. The hook's first return
value decides the verdict — nothing or a truthy value accepts, an explicit `false`
refuses — and a second return value is the message either way. A refusal leaves
the flag as it was, snaps the box back and reports
`Activate failed: <message>` / `Deactivate failed: <message>` on the status line. A
missing or non-function hook reports `activate() is not defined` (or
`deactivate() is not defined`).

A ticked script is a promise that outlives a single click: when the target
detaches its flag is cleared, and **quitting slopkit runs `deactivate()` for every
script still ticked** before the target is released. The verdict of that final run
is not reported — there is no status line and no checkbox to write on the way out.

## Time and output limits

A runaway script cannot hang slopkit: a run is aborted after roughly 20 million
Lua VM instructions or five seconds, whichever comes first, and the abort is an
ordinary error (`script exceeded its instruction budget` or
`script exceeded its time budget`) that leaves the session usable.

`print` output is captured, not written to a terminal, and bounded: at most 1000
lines, each truncated to 4096 bytes with a trailing `…` when it was cut.

## The memory API

Every read and write goes through the plugin, fails as an ordinary Lua error when
the target refuses, and names the function that failed
(`read_u32: <target's error text>`).

### `mem`

- `mem.pointer_size()` — the target's pointer width in bytes.
- `mem.read(address, token)` — a typed read. The tokens are
  `u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 ptr`.
- `mem.write(address, token, value)` — a typed write, same tokens.
- `mem.read_bytes(address, size)` — `size` raw bytes as a Lua string.
- `mem.read_string(address[, limit])` — bytes up to the NUL terminator, at most
  `limit` bytes.
- `mem.write_bytes(address, data)` — writes a Lua string's bytes.

### Typed one-liners

Every read token has a one-liner: `read_u8(address)`, `read_i8`, `read_u16`,
`read_i16`, `read_u32`, `read_i32`, `read_u64`, `read_i64`, `read_f32` and
`read_f64`. Integer tokens return integers; float tokens return floats.

`write(address, value[, type])` writes a value. The type token is optional and,
without one, the width is inferred from the Lua value: a float writes `f32`, and
an integer the **narrowest** of `u8`/`u16`/`u32`/`u64` (or `i8`/`i16`/`i32`/`i64`
when negative). Pass an explicit token from the `mem.write` vocabulary when you
need a wider or different write.

## Symbols and labels

Two kinds of named values let a script publish addresses and numbers that other
rows and scripts can use.

**Global symbols** live in a process-wide registry for as long as slopkit runs —
they survive a detach, are shared across scripts and are never written to the
`.skt` file:

- `rsymbol(name[, value])` — registers `name` with `value` (or `0`).
- `ssymbol(name, value)` — sets the value, registering the name when it is new.
- `usymbol(name)` — removes it.

A registered symbol resolves like a module name in any address expression, so
after `ssymbol("hp", 0x1234)` a table row whose address is `hp` follows the value
as it changes.

**Labels** — `rlabel`, `slabel` and `ulabel` — take the same arguments and behave
the same way, but they belong to the script that registered them: the engine drops
a script's labels when a different script runs, so a script's own
`activate`/`deactivate` hooks share their labels while two scripts never do. A
value that must outlive another script's run belongs in a global symbol.

Names are case-insensitive, must be a non-blank string without `+` and not
starting with `#`, and the value must be a whole number in `0 .. 2^64-1`.

## Resolving addresses and expressions

- `symbol(name)` returns a global symbol's value, or `0` when the name is unknown.
- `label(name)` returns one of the script's own labels, or `0`.
- `expression(text)` resolves a full address expression — a module, symbol or
  label base plus the usual `+offset` dereference chain — and returns the absolute
  address.

Every function that resolves a name looks at the script's **labels first** and
then at the process-wide **symbols**, so a label shadows a global symbol of the
same spelling. `expression` additionally accepts a module name and a literal,
resolving the base as **label → symbol → module name → literal**.

## Scanning for a byte pattern

`aobscan(name, pattern[, module])` walks the target's readable memory for a byte
pattern and stops at the first match:

- `pattern` is hex bytes with `?` wildcards, for example `"48 8B ?? ?? 89"`.
- With `module`, only that module's regions are searched (the name is matched
  case-insensitively).
- On a hit it stores the address under `name`, returns `true, address` and lets
  the address table resolve the name; on a miss it returns `false` and stores
  nothing.

The scan is bounded by the same time budget as a run and reads memory in 64 KiB
chunks.

## Allocating memory in the target

- `alloc(name, size_bytes[, near_address])` maps `size_bytes` of read/write/execute
  memory **inside the target**, returns the address and publishes it under `name`,
  so `local p = alloc("buf", 64)` is immediately usable with `write`/`read_u8`.
  `near_address` is a best-effort hint; omitting it (or passing `0`) means
  anywhere.
- `dealloc(name)` unmaps exactly the mapping an `alloc` of the same session
  created under that name; a foreign address is never unmapped, so a typo cannot
  destroy one of the target's own mappings.

Allocating is a plugin capability: a target whose plugin cannot map memory reports
`alloc: the target's plugin cannot allocate memory` instead of pretending. A debug
session and an allocation never overlap, and the allocation disappears when the
target detaches.

## Assembling instructions into the target

`assemble(address, text[, ...])` turns a block of instructions into bytes and
writes them into the target at `address` in one go:

```lua
local ok, size = assemble(entry, [[
    push rbp
    mov rbp, rsp
    sub rsp, 0x%X
]], 1337)
```

- `text` holds one instruction per line, written the way the disassembly listing
  prints it (`MOV RBP, RSP`, `JZ 4010`, `MOV RAX, [22FE]`). Blank lines and
  everything from a `;` to the end of a line are ignored. This is the listing's
  own vocabulary, not NASM or GAS: a bare number is hex, so `MOV EAX, 10` writes
  `0x10`, while `#10` selects decimal.
- Each instruction is encoded for the address it will actually occupy, so a
  branch target and a bracket-less memory address written the way the listing
  shows them land on the right place.
- Extra arguments are passed to Lua's `string.format` to expand `text`; with none
  the text is used verbatim, so a literal `%` is never interpreted.
- The mode follows `mem.pointer_size()`: `4` assembles 32-bit code, anything else
  the native 64-bit mode. `push ebp` therefore only assembles against a 32-bit
  target and is rejected against a 64-bit one.
- It returns `true, size` (the byte count written) on success, or `false, reason`
  and writes nothing on any failure. A rejected instruction names its 1-based
  line: `"line 3: unknown mnemonic 'frobnicate'"`.

The whole block is a single write, but a target that fails part of the way
through a write can still leave the front of the block in memory; assemble into an
`alloc`ed scratch buffer first when a partial write would matter.

## Installing a hook in the target

The generated hook script leans on one runtime global, `hook`, which installs and
removes a jump-between-code hook in **two target writes**. The [Hooking an
instruction](#hooking-an-instruction) section shows the facts table it consumes.

- `hook.install(spec)` — finds `spec.pattern` (restricted to `spec.module` when it
  is not empty), requires it to match **exactly once** — more than one match refuses
  instead of hooking an arbitrary copy — verifies the bytes at `site = match +
  spec.offset` against `spec.original`, maps a code cave near the site, writes
  `spec.code`, then `spec.trampoline`, then a jump back, and patches the site with a
  near jump plus NOP padding. It returns `true` alone on success or `false, reason`
  on a refusal, and a refusal leaves the target as it was (a cave mapped before the
  failure is freed). On success the site and the cave are published under
  `spec.site_name` and `spec.cave_name` as script-local labels.
- `hook.remove(spec)` — writes `spec.original` back at the site and frees the cave.
  It resolves the site and the cave from the labels `install` published, so pass the
  **same table** back; when the site's label is gone it returns
  `false, "this hook's site is not known anymore"`. On success it drops both labels.

The fields a `spec` table must carry are `site_name` and `cave_name` (the labels to
publish), `pattern` (the AoB signature), `original` (the bytes the site must hold,
as a string built with `string.char`), `code` (the assembler block your hook runs)
and `trampoline` (the instructions the hook replaced, re-encoded so their addresses
keep working). `module` (optional; omitted or empty scans the whole address space),
`offset` (the window's offset inside the match, default `0`), `trampoline_args`
(one `site`-relative delta per `%X` in `trampoline`) and `cave_size` (an override of
the automatic cave size) round it out. A missing required field or a wrong type
raises `hook.install: …` / `hook.remove: …`.

## Hooking an instruction

The disassembly listing's `Hook Instruction...` command turns the selected row
into a ready-to-edit hook script and prefills the Add Script window with it (see
[`docs/UI_DESIGN.md`](docs/UI_DESIGN.md)); nothing is written to the target and no
row is created until you click **Add**. The generated script is a facts table plus
two one-line hooks, with the target work done by
[`hook.install`](#installing-a-hook-in-the-target):

```lua
local kHook = {
    site_name  = "hook_site_1a2b40",
    cave_name  = "hook_cave_1a2b40",
    module     = "game.exe",
    pattern    = "48 83 EC 28 48 89 D8 48 83 C4 28 C3 90 90 90 90",
    offset     = 0,   -- the window's offset inside the match
    original   = string.char(0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xD8),
    code       = [[
        nop    ; TODO: replace with your code
    ]],
    trampoline = [[
        SUB RSP, 28
        MOV RAX, RBX
    ]],
}

function activate()
    return hook.install(kHook)
end

function deactivate()
    return hook.remove(kHook)
end
```

Everything you edit is the `code` block — replace the `nop` with what the hook
should do. `activate()` is a single `hook.install(kHook)` call, so the `Active`
checkbox's verdict is exactly the helper's.

- The pattern spans the instruction plus its neighbours — up to two on each side,
  and at least 16 bytes — and wildcards only the bytes of *absolute* address
  fields, so a rebased module still matches while a relative branch or rip-relative
  displacement stays literal.
- The site is the window's first byte, `match + offset` bytes into the pattern; the
  bytes there are checked against `original`, so a site someone else already changed
  is refused rather than hooked twice.
- The cave is sized automatically from the assembled payload (the code, the
  trampoline and the jump back), so there is no budget to grow; add
  `cave_size = <bytes>` to `kHook` when you want extra room.
- Every address the replaced instructions print is re-emitted
  position-independently: a `%X` placeholder in `trampoline` is filled from the
  matching `trampoline_args` entry, a `site`-relative delta, so a rebased module
  still reads the same data. A memory operand the listing left without a size is
  re-emitted with its explicit width (`INC [RBX+1C]` becomes
  `INC dword ptr [RBX+1C]`), so a width the decoder could infer is not lost on the
  re-encode. The generator proves the rewritten trampoline assembles before the
  script reaches the editor and refuses with a reason when an instruction cannot be
  re-encoded.

The hook window is whole instructions: the selected instruction **plus as many
following instructions as needed** for a 5-byte near jump to fit, so an instruction
shorter than a jump pulls in its successor and the hook never cuts an instruction
in half. A row whose window would run past the listing's decoded bytes is refused
rather than hooked.

A live hook's state is the two script-local labels the helper publishes —
`hook_site_<rva>`, the hooked window's first byte, and `hook_cave_<rva>`, the stub —
so `deactivate()` reads them back and nothing is remembered engine-side. Being
labels they resolve only inside this script; the UI's address fields resolve only a
name a script registers as a symbol (see [`docs/UI_DESIGN.md`](docs/UI_DESIGN.md)).

One caveat: the hook's write is a direct script write, not a listing patch — it does
not show on the listing, is not part of the session patch list, and is undone by
`deactivate()` (and when slopkit closes, which deactivates every ticked script)
rather than by `Restore Original Instruction`. A hook script saved before `hook`
existed still activates and deactivates unchanged, because every global it used is
still there.

## Worked examples

### Find a pattern once and publish it

`activate` locates a byte pattern in a module and leaves the address under a
global symbol the rest of the table can use.

```lua
function activate()
    local found, address = aobscan("health_code", "48 8B ?? ?? ?? ?? 89", "game")
    if not found then
        return false, "the health pattern was not found"
    end
    print(string.format("health code at 0x%X", address))
    return true
end
```

### Map a scratch buffer and free it again

The buffer lives inside the target and is unmapped when the script is switched
off.

```lua
function activate()
    local p = alloc("scratch", 32)
    write_bytes(p, "slopkit")
    print("scratch at 0x%X holds", p, mem.read_string(p, 8))
    return true
end

function deactivate()
    dealloc("scratch")
    return true
end
```

### Assemble a stub into a scratch buffer

`alloc` a run of memory, `assemble` a small routine into it and publish its entry
address for the table or another script to use.

```lua
function activate()
    local entry = alloc("stub", 64)
    local ok, size = assemble(entry, [[
        push rbp
        mov rbp, rsp
        mov eax, #42
        pop rbp
        ret
    ]])
    if not ok then
        return false, "assemble failed: " .. size
    end
    ssymbol("stub_entry", entry)
    print(string.format("stub at 0x%X (%d bytes)", entry, size))
    return true
end

function deactivate()
    dealloc("stub")
    return true
end
```

### Follow a pointer chain in a one-off run

A `Run Script` that registers a base, follows a two-level chain and prints the
final integer.

```lua
ssymbol("base", 0x100000)
local first  = mem.read(expression("base + 0x10"), "ptr")
local second = mem.read(first + 8, "ptr")
print("value:", mem.read(second + 0x14, "i32"))
```

## Where to look next

The mechanism behind all of this — the engine's lifecycle, the type-token codec,
the symbol registry and the worker jobs — is documented in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). For a hands-on target,
`Help > Launch Practice Target` starts the sandbox process.
