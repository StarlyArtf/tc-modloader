# Float Ops

`local.float-ops` is an independent native Mod. It is not linked into
`tc-loader.dll` and it does not patch the game or the loader.

- **M0** (see `docs/PLAN-float-components.md`, section 4) measures the public
  component path this Mod needs.
- **M1** (section 5) adds the IEEE 754 binary32 kernel: vendored SoftFloat 3e
  behind `fp/fp32.hpp`, with the project's rounding, flag, NaN and comparison
  semantics on top.
- **M3/M4** (sections 6.2 and 6.3) add the rest of the binary32 catalogue: the
  arithmetic and bit operations, the relations and classification, the integer
  conversions and Split/Make Bits.
- **M6** (section 6.4.1) widens the catalogue to binary16 and binary64.  One
  format is one component type - a type's pin widths are frozen when it
  registers, so there is no width-switching instance - which makes the catalogue
  22 binary16 types, 22 binary32 types and 21 binary64 types (the binary64 FMA
  is absent: three 64-bit inputs are 192 bits against the loader's 128-bit input
  budget).

`components.cpp`'s `kFamilies` table is the single source of truth for all of
them: one row per family - identity, type id, folder, name, description, pins,
callback, configuration layout and what the body prints - expanded over the
formats the family exists in.  The concrete types are built once, at load
(`catalogueStore()`), and every part of the Mod reads them from there: the
registration loop, the render callback, the drawer rows (`editor.cpp`) and the
component pictures (`icons.cpp`).

## M0 probes

Five stateless probes (`state_words=0`), registered through the public
`tc.component.types` service only:

| Probe | Inputs | Outputs |
|---|---|---|
| `FP32 M0 Source` | none | `Word[32]` = `0xDEADBEEF` |
| `FP32 M0 Pass` | `Word[32]` | `Word[32]` (bit-exact) |
| `FP32 M0 Result + Flags` | `Word[32]` | `R[32]`, `Flags[5]` = `0x1F` |
| `FP32 M0 Flags Sink` | `Flags[5]` | none |
| `FP32 M0 Sink` | `Word[32]` | none |

They register as `浮点/M0/FP32 M0 …`, so the game files them into an `M0`
subfolder of the 浮点 folder instead of leaving them loose in `自定义`.

They cover the shapes the production FP32 components need: a pure source, a
32-bit passthrough, two outputs on one instance, a five-bit narrow input and two
pure sinks. Each probe also reports what it saw in the loader log:

- one line per instance for the first cycles of every reset segment, so a
  binding token can be matched with the value it carried;
- the UI refresh path (the paused board still has to show the right value);
- every reset with the totals of the segment it ended, including a mismatch
  count that must stay at zero on the M0 boards.

## M1 kernel

## Board layout

Every component is drawn like the original word parts, because that is what a
player compares them against: a 4.92 x 2.93 cell body, the name
right-aligned in the top right corner, the width badge in the top left, the
value (or the operator) in the middle, and the pin on the 3.0 lane - *outside*
that body, which is where the stock `Constant`/`Static Value` put theirs
(`out0=(3,0)` in the kind table).

Two loader interfaces make that possible and both are documented in
`docs/sdk/services.md`:

* `TCComponentTypeDefinitionV2::pin_lane` asks for the 3.0 lane.  A generated
  pin lands at `round(circuit_x / 8)` board cells, so the loader writes the lane
  into the scaffold's own coordinates and moves the wires that reach the pins
  with them; the default (2.0, the historical `-18 / +13`) is byte-for-byte what
  every earlier release produced.
* `TCComponentRenderDrawV2::text_sized` / `measure_text` draw and measure text
  at an explicit pixel size, which `ImDrawList::AddText_Vec2` cannot do in this
  build.  `components_internal.hpp` carries the measured constants that turn a
  digit height in board cells into that size (0.79 of the asked size is painted,
  a painted digit is 0.66 of that), together with where the numbers came from.

`tests/float-ops-playtest.ps1` captures the board and measures all of this back
out of the picture: body size, the name's corner, the badge's box, the value's
height and the pin's distance from the body edge.

## Editing

There is no window of the Mod's own: selecting one of these components opens the
game's own component panel at the bottom of the screen, and the rows inside it
are the **loader's** (`tc.component.editor`): this Mod hands one field table per
type to `setFields` during `tc_mod_load`, and the host draws, validates and
commits them through the same configuration path the rest of the Mod uses (one
commit = one undo step, and the type's `on_config_changed` fires).  That is what
the rows look like in the panel (`editor.cpp`):

```text
标签      [ ................ ]
值        [ 1.5              ]        (the constant; float over its binary32 bits)
原始位    [ 0x3FC00000       ]        (the same four bytes as hex - the escape
                                       hatch for NaN/Inf/denormal patterns)
舍入模式  (RNE) RNA RTZ RDN RUP      (the adder)
显示方式  (decimal + hex) ...        (the display)
```

The catalogue follows the same shape.  Every type gets the label row, and a type
whose arithmetic rounds (add, subtract, multiply, divide, square root, fma, round
to integral, and both directions of the integer conversions) gets the five-mode
rounding row.  A type with no choice used to show a read-only explanation row;
that sentence now lives in the type's own `description`, which the game's panel
already prints under the title, so there is one surface instead of two.  On the
board every one of them is drawn like the three the vertical slice shipped with:
the stock 4.92-cell face, the width badge of its own format (`16`, `32` or `64`),
the name in the top right, the symbol in the middle and the pins on the 3.0 lane
outside the body.  Only a side with more than two pins (Compare has four outputs,
Split Bits three) makes the face taller, centred on its pins.

All three widths edit their Constant the same way: a **值** row whose text is a
decimal value and a **原始位** row with the same bytes in hexadecimal.  The
decimal row is the loader's width-aware float field
(`TC_COMPONENT_FIELD_FLOAT_WIDTH`, 2/4/8 bytes for binary16/32/64), which this
Mod asked for: it shows the shortest decimal that parses back to the stored
pattern, accepts `inf`/`nan`/`-0`, and rounds what the player types to the row's
own width, so `0.1` typed into an FP16 constant stores `0x2E66` and reads back as
`0.1`.  The hexadecimal row next to it stays the escape hatch for a NaN payload,
a denormal or an exact test vector.  Everything else - the rounding row, the
display-mode row, the label - is the same row in all three widths.

The bottom panel's **"在元件工坊编辑"** button is switched off for these types
(`tc.component.render` V4's `setFoundryButton(..., false)`): a Mod-owned type has
no player-editable schematic behind it, so the button would only open an empty
definition.  The panel's layout is unchanged - the host draws that one button
transparent and inert.

## The 浮点 folder in the game's own component column

The types live in folders the **game itself** builds in its right-hand column:
the pinned build's palette code splits a custom prototype's *name* on `/` and
walks the parts as parent categories
(`add_to_menu_tree__presenterZutilities_u12208`; the measurement and the tree's
node layout are in `docs/research/palette-categories.md`).  So this Mod registers
its types as `浮点/<format>/<kind>/<name>` - `浮点/FP32/计算/FP32 Add`,
`浮点/FP16/转换/I32 To FP16`, `浮点/FP64/比较/FP64 Compare` - and the game files
them under those nodes, which it allocates, draws and destroys itself.  The width
comes first because sixty-five rows under one node is not a menu: `浮点` holds
`FP16`, `FP32` and `FP64`, and each of those holds 计算, 比较, 转换, 位与拆分 and
常量与显示.  The Mod draws no palette, no side panel and no popup, and nothing is
written into the player's save.  `TC_FLOATOPS_PALETTE` overrides the prefix (that
is how the case compares the folded and flat menus with one binary).

```text
node=… byte1=2 tag=2 name="自定义"
  children=1 (the folder is all that is left in CUSTOM)
  node=… byte1=2 tag=2 name="浮点"
    node=… byte1=2 tag=2 name="M0"                 ← the five compatibility probes
      node=… byte1=1 tag=1 id=0x4633325041535331 name="浮点/M0/FP32 M0 Pass"
    node=… byte1=2 tag=2 name="FP16"
      node=… byte1=2 tag=2 name="计算"
        node=… byte1=1 tag=1 id=0x4631364144445f31 name="浮点/FP16/计算/FP16 Add"
      …
    node=… byte1=2 tag=2 name="FP32"
      node=… byte1=2 tag=2 name="计算"
        node=… byte1=1 tag=1 id=0x4633324144445f31 name="浮点/FP32/计算/FP32 Add"
      … (22 types: the binary32 block is the one the earlier releases shipped)
    node=… byte1=2 tag=2 name="FP64"
      … (21 types: no FP64 Fused Multiply Add - three 64-bit inputs are 192 bits)
```

`tests/float-palette-playtest.ps1` is the true-game case: the probe hooks the
game's own `reload_component_menu` and dumps the tree the palette draws, and the
case asserts that `自定义` holds a `浮点` category whose subtree is exactly this
Mod's 70 component ids (65 types plus the five M0 probes), that the FP16/FP32/FP64
folders exist inside `浮点` and the five kind folders inside them, and that none
of the ids is listed anywhere else.

Two things the pinned build does *not* offer, both recorded in
`docs/research/palette-categories.md`: the folder lands *inside* `CUSTOM` (a
custom prototype's menu path always starts at the game's own 自定义 node, so
there is no supported way to be a sixth top-level page), and the prefix is part
of the component's name, so a tooltip in the game reads `浮点/FP32/计算/FP32 Add`.

The label is a configuration field (schema 3): empty means "show the type's
name", which is the stock rule, and anything else is printed in the body's name
slot.  Committing a field writes the configuration and then asks the game for the
refresh a player would press themselves, because the game's compiler has no idea
that a custom component's configuration moved: without that request the wire and
the Display keep the value they were last computed with.

## Leaving a level and coming back

The rows follow the **record**, not the click, and that is what the 2026-09-26
report ("退出再重进后，面板的相关配置不会渲染"，"常量值输出变为默认") needed:

* `serviceBoardChange()` asks for the current Board handle once per frame.  A new
  handle drops the editor rows, the painted boxes and the Display caches, because
  a re-entered level hands the new board the *same* component ids - "the instance
  changed" says nothing there.  The log says so explicitly
  (`float-ops: board entered (handle 3/2); the editor rows were dropped …`, and
  `no board is up; the editor rows and the board caches were dropped` on the way
  out).
* every panel frame compares the record with the bytes the rows were filled from
  (`readConfigSnapshot`/`recordMatchesRows`) and re-reads them when it moved -
  a restored save, an undo, a clone, a re-entered board - without disturbing a
  field the player is typing in.  The line
  `the drawer is editing instance 0x… (type 0x…, label "…", value "12.5")` carries
  what the fields now hold, so a case can read the panel's value out of the log;
  a record that moved afterwards is reported as
  `the drawer re-read instance 0x… … because its record moved`.
* the type of a selected instance comes from the painted boxes when they exist and
  from the host's own instance list otherwise (`typeOfLiveInstance`), so a
  component that has not been drawn yet still opens its rows.

The save the panel asks for goes to the schematic directory the game itself reads
and writes: `schematics/<level kind>/Default/circuit.data`, where the kind comes
from the level's own `campaign/<name>/meta.txt` (the loader's
`levelSchematicKind()`).  Before that, `TC_COMMAND_SAVE` used the level's *name*,
which for "The Sandbox" is a directory the game never looks at - the configuration
was written where nothing reads it, and the next visit showed the default.

The write has to find the instance among the board's live ones, and the host's
instance enumeration is all-or-nothing (it answers OK only when every instance
fitted the caller's buffer, and the set keeps growing while the game binds a
compiled board).  The Mod therefore asks for sixteen handles, and grows and
retries until the host is satisfied - a fixed-size array here is what made every
write fail on a player's 22-component board (`tests/float-panel-playtest.ps1
-Fixture crowd`, the busy-board case).

The board space a type reserves is the footprint declared through
`tc.component.geometry`, and it is what the game hit-tests, drags by and
reserves.  The game lays a generated side out from row 0, so a face with more
pins on one side sits *below* the component's own cell (Compare's four outputs
span rows 0..3, i.e. the cells 0..3): the Mod therefore declares the footprint as
whole cells with that offset (`set_footprint_cells`, geometry V3) instead of a
centred half height the host would round outwards.  The face itself only grows
when a side does not fit the stock 4.92 x 2.93-cell look, and it lands inside
whole cells, so the box is exactly the cells the face and pins occupy:

```text
subtract / constant / fma / split bits / make bits   3 cells vertically (stock-sized face)
compare                                              4 cells (its four-output side spans rows 0..3)
horizontally: 5 face cells + one cell per pin outside the box (7 with input pins, 6 without)
```

`tests/float-pitch-playtest.ps1` asserts those numbers, because they are what a
player feels when two parts are placed next to each other; two mistakes are on
record behind it - a centred half height that the host rounds outwards (four
cells for a 2.93-cell face) and a face grown by a fixed margin (3.2 cells, which
rounds to four).

Those rows are real widgets, and they are drawn in a window of the Mod's own
laid exactly over the panel's row area (no background, so the game's panel shows
through) - not in the game's window.  Measured reason: the drawer never becomes
the ImGui window under the mouse where its rows are (`IsWindowHovered()` is
false for it while the cursor sits on a row the plugin drew in it), so a widget
drawn there is painted but can never be clicked or typed into.  The loader's own
page container needs the same `igSetNextWindowFocus()` call for the same reason.
There is no editor window: the fields commit when they are left, exactly like
the game's own panel fields.  `tests/float-panel-playtest.ps1` drives this with
real mouse and keyboard input and asserts both the commit and that no window
opened over the board.

```text
fp/fp32.hpp              public API: FPRounding, FP32Result (bits + 5 flags),
                         FP32Relation, the ten classification bits
fp/environment.hpp       SoftFloat state per call: rounding mode,
                         tininess-after-rounding, flag word, canonical NaN
fp/fp32.cpp              the twelve operations; compare, classify and the 2019
                         min/max pair are integer code, not library calls
fp/softfloat/platform.h  the upstream Win64-MinGW-w64 configuration plus
                         THREAD_LOCAL, so SoftFloat's state is per-thread
fp/softfloat-sources.txt the 19 compilation units the kernel needs
third_party/berkeley-softfloat-3/   vendored subset (commit and procedure in its
                         README; nothing in it is modified)
```

Supported operations: add, subtract, multiply, divide, square root, fused
multiply-add, IEEE remainder, round-to-integral-exact, negate, absolute,
copy-sign, compare, classify, minimumNumber and maximumNumber. Every one of them
takes and returns raw bit patterns - there is no host `float` in the interface -
and each arithmetic call reports its own five exception flags.  Each of them
exists in binary16, binary32 and binary64 (the integer conversions keep their
32-bit integer side), except the fused multiply-add, which stops at binary32:
three 64-bit inputs are 192 bits against the loader's 128-bit input budget.

The plugin runs a kernel self-check at load time and writes one line to the
loader log, so the shipped DLL proves it links and computes under the loader:

```text
float-ops M1: kernel self-check 1+2=0x40400000/0x00 (1+ulp)^2=0x3F800002/0x01
fma=0x34000001/0x00 1/0=0x7F800000/0x08 sqrt(-1)=0x7FC00000/0x10
```

## Build, test and package

From the repository root:

```powershell
& .\examples\float-ops\build.ps1
```

The script compiles and runs four offline tests - the kernel
(`tests/float-kernel.cpp` against the golden vectors generated by
`tools/float-vectors.py`), the binary32 text layer (`tests/float-decimal.cpp`
against `tools/float-decimal-vectors.py`), the per-width text layer
(`tests/float-text.cpp`: named spellings, every binary16 pattern and a binary64
sample) and the registration/compatibility test (`tests/float-compat.cpp`, which
walks all 65 catalogue types) - then builds `native/float-ops.dll` with the
kernel linked in, generates the 65 component pictures, stages a clean package
tree (`mod.json` + `native/` + `files/`) and calls `tools/Pack-Mod.ps1` to
produce `dist/local.float-ops.mod`. The source directory itself is not a valid
package root.

`build-kernel.ps1` is the one place that knows how to compile the vendored
SoftFloat subset plus `fp/fp32.cpp`; it leaves the object list in
`build/float-ops/float-kernel-objects.rsp`. The Mod DLL, the offline tests and
the true-game drivers all link those same objects, so what the tests exercise is
what the package ships.

## M0 evidence

```powershell
# Package level: discovered, enabled and disabled in isolation.
& .\tests\float-package.ps1

# True game, 21 instances (source -> pass x17 -> result+flags -> sink, plus the
# Flags[5] sink): the second wide output is read back, the paused board refreshes
# with the right values, and the cycles the driver runs after a reset still carry
# the pattern.
& .\tests\float-compat-playtest.ps1

# True game, 42 instances: the diagnostic for the loader's wide-output boundary.
# Tokens 1..32 must hold the pattern; the first output without a slot is recorded
# in build/float-boundary-report.txt instead of being treated as a failure.
& .\tests\float-boundary-playtest.ps1

# True game, one level visited twice (the 2026-09-26 report): the driver edits a
# Constant's value through the drawer, leaves the level with Escape, presses the
# level entry again and then only *selects* the component.  The second visit has
# to show the saved value with no click - on the board and in the drawer's field.
& .\tests\float-reentry-playtest.ps1

# True game, the width cut: one Constant -> Add -> Display chain per format on
# one board.  The loader's own lines have to show 16-bit and 64-bit pins, and
# the three displays have to print 1.0 + 1.0 as 2 0x4000, 2 0x40000000 and
# 2 0x4000000000000000.
& .\tests\float-width-playtest.ps1

# The game's own component column: 浮点 ▸ FP16/FP32/FP64 ▸ the five kind
# folders, with exactly this Mod's 70 ids inside them.
& .\tests\float-palette-playtest.ps1
```

Both true-game scripts build a test-only driver package (the real plugin object
plus the byte-adder level runner) in `build/`; the runner is never part of
`local.float-ops.mod`. Everything runs in an isolated copy of the game with its
own save profile.
