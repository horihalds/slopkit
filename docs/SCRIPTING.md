# Lua scripting

A **script entry** is an address-table row that holds Lua source instead of a
value: its Type column reads `script`, its Address and Value cells stay blank, and
the source travels inside the `.skt` file (`entries/<name>.lua`). Scripts run
against the attached target through a small memory API over the Lua VM slopkit
links against; everything else is ordinary Lua.

## Hooks

A script row's leading `Active` checkbox drives three optional global functions.
The chunk runs first, then the named global is called with no arguments.

| Hook | Runs when | Missing hook |
| --- | --- | --- |
| `activate()` | the box is ticked | refused: `activate() is not defined` |
| `deactivate()` | the box is unticked, on detach, and on quit | refused: `deactivate() is not defined` |
| `update()` | once per `Live update` interval, or the script's own `update_interval()`, while the box stays ticked | ignored silently |

The verdict is the same vocabulary for all three: nothing or a truthy first value
accepts, an explicit `false` refuses, and a second return value is the message
either way. A refusal of `activate`/`deactivate` leaves the flag as it was, snaps
the box back and reports `Activate failed: <message>` / `Deactivate failed:
<message>`.

`update` runs in activation order while the box stays ticked; the Settings
`Live update` toggle and interval stay the ceiling, and there is no per-row
toggle. A script can ask for a slower cadence with the global
`update_interval(milliseconds)`, callable from the chunk, `activate` or `update`:

```lua
function activate()
    update_interval(1000) -- poll the target at most once a second
end
```

Only a whole, non-negative number is accepted; anything else raises
`update_interval: the value must be a whole number of milliseconds` and leaves the
script untouched. The last call in a run wins, so `update` can re-tune the cadence
on the fly; `0` — and declaring nothing at all — means "due on every pass", and a
value smaller than the Settings interval cannot tick faster than it. The first
tick follows activation, then one tick per declared interval. `update` runs even
when the address list displays nothing to read, because a ticked script drives the
target rather than reporting a value. A successful tick writes nothing at all —
not the hook's `print`ed lines, not the status line, not the log — while a failed
tick (a chunk error, a hook error, a budget abort or a `false` verdict) switches
the script off: its `deactivate()` runs, the `Active` box clears, and one `warning`
`script '<description>' update failed: <reason>` plus `Update failed: <reason>` on
the status line says why. Other ticked scripts keep running. The chunk is re-run
every tick, so top-level code runs at the interval: put per-tick work in `update`,
defined as `function update() … end` so a global left behind by another script is
never mistaken for yours.

A ticked script is a promise that outlives a click: detaching clears its flag, and
quitting slopkit runs `deactivate()` for every script still ticked before the
target is released (that final verdict is not reported).

## Editing, verifying and running

`File > Add Script…` (or `Add Script…` in the address list) opens the **Add
Script** dialog: a `Description` field and a Lua editor seeded with the
`activate`/`deactivate` skeleton and a commented `update` stub. `Edit Script…`
opens it in edit mode and `Save` replaces the description and source; the
description must not be blank, and both modes work with no target attached. The
editor, its completion and argument hints and the dialog's keyboard rules are
owned by [`docs/UI_DESIGN.md`](docs/UI_DESIGN.md).

`Verify` compiles the editor's text **without running it and without touching the
target**: valid source reports `Syntax OK.`, rejected source the compiler's own
message (`script:3: 'end' expected …`) with the offending line marked. It reports
only what the compiler rejects, so a missing hook or a bad `mem.read` token still
compiles and is reported only at run time.

`Run Script` runs the whole chunk against the attached target, one at a time: its
`print` lines go to the **Log** window under the `script` category, and the status
line shows `Script ok.`, `Script returned <value>.` or `Script failed: <message>`.
A script's globals persist between runs on one target and drop when it detaches,
and a script row is renamed from its `Description` cell like any row (blank names
are refused).

## Time and output limits

A runaway script cannot hang slopkit: a run (including one `update` tick) is
aborted after roughly 20 million Lua VM instructions or five seconds, whichever
comes first, reported as `script exceeded its instruction budget` or `script
exceeded its time budget` and leaving the session usable.

`print` output is captured, not written to a terminal, and bounded: at most 1000
lines, each truncated to 4096 bytes with a trailing `…` when it was cut.

## The memory API

Every read and write goes through the plugin and fails as an ordinary Lua error
naming the function (`read_u32: <target's error text>`).

| Call | Meaning |
| --- | --- |
| `mem.pointer_size()` | the target's pointer width in bytes |
| `mem.read(address, token)` | typed read; tokens `u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 ptr` |
| `mem.write(address, token, value)` | typed write, same tokens |
| `mem.read_bytes(address, size)` | `size` raw bytes as a Lua string |
| `mem.read_string(address[, limit])` | bytes up to the NUL, at most `limit` |
| `mem.write_bytes(address, data)` | writes a Lua string's bytes |
| `read_u8(address)` … `read_f64` | one typed read per token; integers return integers, floats return floats |
| `write(address, value[, type])` | writes a value; without `type` a float writes `f32` and an integer the narrowest token that fits |

## ReClass classes as typed views

The `structs` global exposes the classes of the ReClass view as named, typed
views over the target's memory. Field names, offsets and types come from the
live definition, never from a copy, so an edit in the ReClass view takes effect
on the next access. The classes live in the address table and travel with the
`.skt` file, and a lookup answers with or without an attached target while a
read or a write still fails with the usual no-target error.

| Call | Meaning |
| --- | --- |
| `structs.get(name)` | the class handle; `structs.get: no class named '<name>'` when there is none |
| `structs.list()` | an array of the table's class names |
| `Class:at(address)` | an instance bound to an absolute address, which need not be readable yet — reads and writes fail individually |
| `Class:fields()` | an array of `{name=, offset=, type=, size=}` |
| `inst.<field>`, `inst["<field>"]` | the field's value; an unknown field raises `<Class>.<field>: no such field` |
| `inst.<field> = value` | writes the field; an unknown field raises the same way |
| `inst:base()` | the instance's address |
| `inst:address_of(field)` | the address of a field |
| `inst:field(name[, value])` | reads or writes a field by name, helpers included |
| `inst:raw(field)` | the field's own address: a pointer without following it, an inlined class or an array where it sits |
| `inst:snapshot()` | the whole class read once into a buffer |
| `tostring(inst)` | `Player@0x1A2B40` |
| `pairs(inst)` | field names and current values; a field the view cannot read reports `nil` |

```lua
local Player = structs.get("Player")
local p = Player:at(expression("game.exe+1A2B40"))

print(p.health)         -- the f32 field named "health", at base + its offset
p.health = 100          -- written back as f32
p.ammo = 30             -- the i32 field
print(p.weapon.damage)  -- a pointer to the "Weapon" class, followed once
```

Field types map onto the `mem` tokens — `u8 i8 u16 i16 u32 i32 u64 i64 f32 f64`,
plus `ptr` for a plain pointer — and read and write through the same codec, so
`mem.pointer_size()` decides a pointer's width and a hex node reads as the
unsigned integer of its width. Beyond the numbers: `bool` reads as a Lua
boolean, fixed-length UTF-8 and UTF-16 text as a Lua string, a raw blob (vector,
matrix, function body) as `mem.read_bytes` would, a pointer that names a class as
an instance of it (`nil` when it is null, an integer address when it names none),
and an inlined class as an instance bound to `base + offset` without a
dereference. Writing a text field that does not fit raises instead of truncating,
as does a fractional value given to an integer field, a write to an inlined class
or to an array, and a kind with no script value at all (a union, a bit field, an
enum, UTF-32 text): `<Class>.<field>: the '<kind>' kind has no script value`.

An array field reads as an indexable proxy, indexed **from 1** like every other
Lua sequence: `p.items[1]` is its first element and `#p.items` its length. An
index outside the array raises. A proxy stops at 4096 elements and a text field
at 4096 characters; a longer one raises rather than allocating.

The helpers win over a field of the same name: `base`, `raw`, `field`,
`snapshot` and `address_of` are the helpers, and a field carrying one of those
names is reached with `inst:field("<name>")` — writing it as `inst.<name>`
raises and says so.

`inst:snapshot()` reads the class's whole extent — at most 1 MiB — in one read
and serves its fields, nested inlined classes and arrays from that buffer with no
further memory I/O. Writes through a snapshot raise. A pointer field still
follows live memory, so `p:snapshot().weapon.damage` is what the target holds now
while `p:snapshot().health` is what the snapshot took.

An instance holds the class's name rather than a copy of its definition, so an
edit in the ReClass view shows up on the next access, a class or field that is
gone raises `<Class>: no class named '<Class>'` or `<Class>.<field>: no such
field`, and an instance a script kept in a global fails the same way after a
detach instead of reading stale memory. Following a pointer chain costs one
dereference per field access, so the time and instruction limits above bound a
runaway script.

## Symbols and labels

**Global symbols** live in a process-wide registry for as long as slopkit runs:
they survive a detach, are shared across scripts and are never saved.

| Call | Meaning |
| --- | --- |
| `rsymbol(name[, value])` | registers `name` with `value` (or `0`) |
| `ssymbol(name, value)` | sets the value, registering the name when new |
| `usymbol(name)` | removes the name |

A registered symbol resolves like a module name in any address expression, so
after `ssymbol("hp", 0x1234)` a row whose address is `hp` follows it.

**Labels** — `rlabel`, `slabel` and `ulabel` — take the same arguments but belong
to the script that registered them: the engine drops a script's labels when a
different script runs, so its `activate`/`deactivate` share them while two scripts
never do. A value that must outlive another script's run belongs in a global
symbol. Names are case-insensitive, must be a non-blank string without `+` and not
starting with `#`, and the value must be a whole number in `0 .. 2^64-1`.

## Resolving addresses and expressions

| Call | Meaning |
| --- | --- |
| `symbol(name)` | a global symbol's value, or `0` when unknown |
| `label(name)` | one of the script's own labels, or `0` |
| `expression(text)` | resolves a full address expression to an absolute address |
| `validate(address[, size_bytes])` | `true` when `[address, address + size)` is mapped and readable |

In `expression`, a `[` … `]` group closes with **one dereference** and nesting
chains them — `[[game.exe+10]+18]+24` dereferences twice — while a bracket-free
expression keeps the usual `+offset` chain. Each level is validated before it is
dereferenced, so a broken step names the level and address. `validate` defaults
`size_bytes` to `1`, probes in bounded chunks and stops at the first failure; an
unmapped range is `false`, not an error, while a zero size or a missing target
raises. Name lookups check the script's **labels first**, then the global
**symbols**; `expression` resolves its base as **label → symbol → module name →
literal**.

## Scanning, allocating and assembling

| Call | Meaning |
| --- | --- |
| `aobscan(name, pattern[, module])` | finds `pattern` (hex bytes, `?` wildcards) and stores the address under `name`; `true, address` or `false` |
| `alloc(name, size_bytes[, near_address])` | maps read/write/execute memory inside the target and publishes the address under `name` |
| `dealloc(name)` | unmaps what an `alloc` of the same session created under that name; a foreign address is never unmapped |
| `assemble(address, text[, ...])` | assembles a block of listing-style instructions and writes it at `address` in one go |

`aobscan` reads in 64 KiB chunks, is bounded by the run's time budget, searches a
single module's regions when `module` is given (case-insensitively) and stores
nothing on a miss. `alloc` needs a plugin that can map memory — otherwise it
raises `alloc: the target's plugin cannot allocate memory`; a debug session and an
allocation never overlap, and the allocation disappears when the target detaches.

In `assemble`, `text` holds one instruction per line written the way the
disassembly listing prints it (`MOV RBP, RSP`, `JZ 0x4010`, `MOV RAX, [0x22FE]`);
blank lines and a `;` to the end of a line are ignored, a bare number is hex
(`MOV EAX, 10` writes `0x10`) while `#10` is decimal, and each instruction is
encoded for the address it will occupy. Extra arguments feed Lua's
`string.format`, and the mode follows `mem.pointer_size()`: `4` assembles 32-bit,
anything else the native 64-bit mode. It returns `true, size` or `false, reason`
and writes nothing, naming the 1-based line of a rejected instruction (`"line 3:
unknown mnemonic 'frobnicate'"`). The block is a single write, so assemble into an
`alloc`ed scratch buffer when a partial write would matter.

## Installing a hook in the target

The runtime global `hook` installs and removes a jump-between-code hook in **two
target writes**; the [Hooking an instruction](#hooking-an-instruction) section
shows the facts table it consumes.

- `hook.install(spec)` finds `spec.pattern` (restricted to `spec.module` when not
  empty), requires it to match **exactly once**, verifies `site = match +
  spec.offset` against `spec.original`, maps executable memory near the site (a code
  cave where the target cannot map memory), writes `spec.code`, then
  `spec.trampoline`, then a jump back, and patches the site with a near jump plus
  NOPs. It returns `true` or `false, reason`; a refusal leaves
  the target as it was. On success the site and cave are published as script-local
  labels under `spec.site_name` and `spec.cave_name`.
- `hook.remove(spec)` writes `spec.original` back and frees the cave, resolving
  the site and cave from those labels, so pass the **same table**; when the site's
  label is gone it returns `false, "this hook's site is not known anymore"` and on
  success drops both labels.

A `spec` carries `site_name` and `cave_name` (the labels), `pattern` (the AoB),
`original` (the bytes the site must hold, built with `string.char`), `code` (the
assembler block) and `trampoline` (the replaced instructions, re-encoded), plus
optional `module` (empty scans everywhere), `offset` (default `0`),
`trampoline_args` (one `site`-relative delta per `%X`) and `cave_size`. A missing
field or a wrong type raises `hook.install: …` / `hook.remove: …`.

## Hooking an instruction

The listing's `Hook Instruction...` turns the selected row into an editable hook
script and prefills the Add Script window with it (see
[`docs/UI_DESIGN.md`](docs/UI_DESIGN.md)); nothing reaches the target or the table
until **Add** is clicked. The generated script is the facts table below plus two
one-line hooks, with the work done by
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
        SUB RSP, 0x28
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

Everything you edit is the `code` block; `activate()` is a single
`hook.install(kHook)` call, so the `Active` verdict is the helper's.

The pattern spans the instruction plus up to two neighbours and at least 16 bytes,
wildcarding only *absolute* address fields, so a rebased module still matches
while a relative branch or rip-relative displacement stays literal. The site is
`match + offset`, its bytes checked against `original`, so a site someone else
changed is refused rather than hooked twice, and the cave is sized from the
assembled payload (`cave_size = <bytes>` overrides it). Every address the replaced
instructions print is re-emitted position-independently: a `%X` in `trampoline` is
filled from the matching `trampoline_args` entry (a `site`-relative delta), and a
memory operand the listing left without a size gets its explicit width
(`INC [RBX+1C]` becomes `INC dword ptr [RBX+1C]`). The hook window is whole
instructions — the selected one plus as many followers as a 5-byte near jump
needs — and a window that would run past the decoded bytes is refused.

A live hook's state is the two script-local labels the helper publishes
(`hook_site_<rva>`, `hook_cave_<rva>`), so `deactivate()` reads them back and
nothing is remembered engine-side; being labels they resolve only inside this
script, unlike a symbol the UI's address fields can use. The hook's write is a
direct script write, not a listing patch: it does not show on the listing, is not
in the session patch list, and is undone by `deactivate()` (and when slopkit
closes) rather than `Restore Original Instruction`. A hook script saved before
`hook` existed still works, because every global it used is still there.

## Examples

### Map a scratch buffer and free it again

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

### Keep a value fresh on every tick

```lua
function activate()
    ssymbol("hp", expression("game.exe+1A2B40"))
    return true
end

function update()
    local value = read_i32(symbol("hp"))
    if value < 0 then
        return false, "hp went negative" -- untickes the script and says why
    end
    write(symbol("hp"), 100)
end
```

### Keep a class field topped up

```lua
function update()
    local p = structs.get("Player"):at(expression("game.exe+1A2B40"))
    if p.health < 50 then
        p.health = 100 -- the f32 field named "health"
    end
end
```

## Scripts in a generated trainer

`File > Generate Trainer…` embeds every script entry — description and source —
into the trainer it writes, and the trainer runs it through the same engine
slopkit uses, so a script that reproduces a table's behaviour here reproduces it
there:

- The target can be a game running under Proton/Wine: the trainer finds it by the
  guest executable's name and resolves expressions against the guest's own modules
  with the guest's pointer width, so `expression("game.exe+1A2B40")` means there
  what it means here.
- Scripts start **idle**: nothing runs until one is activated. In the trainer's
  window the `Frozen / Active` checkbox on the `S<N>` row activates or deactivates
  that script, and the repeatable `--activate N` does the same before the
  requested actions run.
- `activate`, `deactivate` and `update` behave as described above, with the
  trainer's `--interval` (250 ms by default) as the fallback cadence;
  `update_interval` still wins, and its "due every pass" floor is the trainer's own
  interval.
- The table's value entries keep the numbers `1..N` and its scripts are `S1..Sm`;
  the two numbering spaces are independent.
- `mem.*`, `aobscan`, `assemble`, `expression`, `validate`, the global symbols
  (`rsymbol` / `ssymbol` / `usymbol`) and `structs` views all work, and a symbol a
  script publishes resolves the table's own value-entry expressions exactly as it
  does in the app.
- The class project travels with the trainer, so `structs.get(name)` reads the same
  definitions; a project the trainer cannot parse is reported once and leaves only
  `structs` unavailable.
- `alloc` / `dealloc` and `hook.install` / `hook.remove` work, and a slot is safe to
  publish: on Windows `alloc` maps a page of its own in the target (`VirtualAllocEx`),
  and `hook.install` maps an executable page for its payload the same way. Both are
  placed near the `near_address` the script passes, because code can only name an
  address within a near jump's reach: `alloc` sites usually pass the hook's site so
  `MOV qword ptr [slot], RBX` fits in the payload. Only where the backend cannot map
  memory — a game the trainer attached to as a Linux process — does an allocation hand
  out a **cave** instead, which is a run of zero bytes in a mapping the target already
  has (an image pads its sections, so there is one near the code). A hook's payload
  takes an **executable** cave and `alloc` a **writable** one, both handed out at a
  16-byte boundary, nearest the `near_address`. A cave is zeroed when it is handed out
  and `dealloc` zeroes it again, while a mapping is unmapped. A request no run of zero
  bytes can satisfy raises `alloc: the target has no free data cave of <size> byte(s)
  in its writable memory`, and a hook cannot be installed where nothing is executable.
  Zero bytes are only *probably* free — a game fills its zero-initialized data as it
  runs — so a slot the game's own code writes into, like the one a hook updates,
  belongs in a mapping: borrowed padding can turn the value a script publishes back
  into the game's data.
- Output and failure are per script: its `print` lines and the last hook's message
  are shown beside it, and a script whose source does not compile or whose hook
  fails is marked `error`, stops ticking and never affects the others. Nothing a
  script does changes the trainer's exit code.
- `--verbose` writes a debug-level log to `<trainer file>.log` beside the
  executable: the payload's counts, the attach outcome, each script's activation
  and deactivation, the lines a hook printed, every hook failure and every
  `alloc` / `free`, which is where to look when a script behaves differently in a
  trainer than it does here.

## Where to look next

The mechanism behind all of this — the engine and its hooks, the type-token codec,
the symbol registry and the worker jobs — is documented in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). For a hands-on target,
`Help > Launch Practice Target` starts the sandbox process.
