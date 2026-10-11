#!/usr/bin/env python3
"""Generate simcore/view/kind_pins_generated.hpp from build/kinds.txt.

The viewer runs with no game attached, so it cannot ask the game's own
prototype table for pin geometry the way the live sandbox-sim Mod does
(`prototypeGeometry` in examples/sandbox-sim/plugin.cpp).  It used to carry a
hand-written copy of that table instead, and the copy fell behind: a board
carrying a 3 Bit Decoder, a Register or a Splitter was refused with "no pin
geometry in the frozen table" / "has no device in the library" while the live
Mod simulated the very same board.  This script makes the copy reproducible.

Usage:
    python tools/kind-pins.py            # write the header
    python tools/kind-pins.py --check    # fail if it is out of date

Rules, all of them taken from how the game's table is read (see
docs/research/simcore-kind-coverage.md sections 3 and 7):

  * The record counts a component's *hidden* pins too (the storage family
    declares `inputs=0` and still carries two), so a row's input list is the
    declared pins followed by the hidden ones, in that order.
  * A width the prototype writes as one of the "auto" sentinels or as `w0`
    becomes 1, which is what `prototypeGeometry` does with the live table.  That
    is deliberately a *placeholder*, not a measurement: the kinds that declare
    `wauto` (the auto-width Splitters 0x63/0x64/0x2f and the Word Makers
    0x30/0x61/0x62) are then refused by the builder's own width check, exactly
    as they are live, rather than being built at a width nobody measured.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KINDS = ROOT / "build" / "kinds.txt"
OUTPUT = ROOT / "simcore" / "view" / "kind_pins_generated.hpp"

HEADER = re.compile(
    r'^0x(?P<kind>[0-9a-fA-F]+)\s+name="(?P<name>[^"]*)"'
    r"\s+gates=\d+\s+delay=\d+\s+inputs=(?P<inputs>\d+)\s+outputs=(?P<outputs>\d+)"
)
PIN = re.compile(r"(in|out|hidden)(\d+)=\((-?\d+),(-?\d+),w(\d+)\)")

AUTO = 0x7FFF_FFFF_FFFF_FF00


def width_of(raw):
    """The pin's width as `prototypeGeometry` reads it from the live table.

    An "auto" sentinel and `w0` both mean "the table does not name a width
    here", and both become 1 - the same placeholder the live Mod writes, so the
    two tables cannot disagree about which components are buildable."""
    return 1 if raw >= AUTO or raw == 0 else min(raw, 64)


# The prototype table writes 32767 where it has no offset to give: the one slot
# the game itself refuses to place (0x50) is the only row that uses it.
NO_GEOMETRY = 32767


def usable(row):
    """False for a row the table carries without any geometry to carry.

    Two shapes end up here: the unnamed 0-in/0-out slots (0x4e the custom kind
    among them - its pins are whatever the Mod declared, so a frozen 0/0 row
    would quietly turn it into a two-pin blob instead of reporting it), and
    rows whose offsets are the "no offset" sentinel 32767 (kind 0x50, the slot
    the game itself refuses to place).  Neither is something the viewer may
    invent pins for.
    """
    if any(pin[0] == NO_GEOMETRY or pin[1] == NO_GEOMETRY
           for pin in row["inputs"] + row["outputs"]):
        return False
    return bool(row["inputs"] or row["outputs"] or row["name"])


def parse(text):
    rows = []
    for line in text.splitlines():
        match = HEADER.match(line.strip())
        if not match:
            continue
        kind = int(match.group("kind"), 16)
        declared = int(match.group("inputs"))
        outputs = int(match.group("outputs"))
        pins = {"in": {}, "out": {}, "hidden": {}}
        for group, index, x, y, bits in PIN.findall(line):
            pins[group][int(index)] = (int(x), int(y), int(bits))
        inputs = [pins["in"][index] for index in sorted(pins["in"])]
        hidden = [pins["hidden"][index] for index in sorted(pins["hidden"])]
        outs = [pins["out"][index] for index in sorted(pins["out"])]
        if len(inputs) != declared or len(outs) != outputs:
            print(
                f"warning: kind 0x{kind:02X} declares {declared}/{outputs} but lists "
                f"{len(inputs)}/{len(outs)}",
                file=sys.stderr,
            )
        rows.append(
            {
                "kind": kind,
                "name": match.group("name"),
                "inputs": inputs + hidden,
                "declared": declared,
                "outputs": outs,
            }
        )
    return sorted((row for row in rows if usable(row)), key=lambda row: row["kind"])


def row_text(row):
    kind = row["kind"]
    inputs = row["inputs"]
    outs = row["outputs"]
    in_x = [pin[0] for pin in inputs]
    in_y = [pin[1] for pin in inputs]
    out_x = [pin[0] for pin in outs]
    out_y = [pin[1] for pin in outs]
    in_bits = []
    for pin in inputs:
        in_bits.append(width_of(pin[2]))
    out_bits = []
    for pin in outs:
        out_bits.append(width_of(pin[2]))
    name = row["name"].replace("\\", "\\\\").replace('"', '\\"')
    parts = [
        f"0x{kind:02X}",
        str(len(inputs)),
        str(len(outs)),
        "{" + ", ".join(str(value) for value in in_x) + "}",
        "{" + ", ".join(str(value) for value in in_y) + "}",
        "{" + ", ".join(str(value) for value in out_x) + "}",
        "{" + ", ".join(str(value) for value in out_y) + "}",
        f'"{name}"',
        "{" + ", ".join(str(value) for value in in_bits) + "}",
        "{" + ", ".join(str(value) for value in out_bits) + "}",
    ]
    return "    {" + ", ".join(parts) + "},"


def render(rows):
    body = "\n".join(row_text(row) for row in rows)
    return f"""#pragma once
/* Generated by tools/kind-pins.py from build/kinds.txt.  Do not edit by hand;
   run `python tools/kind-pins.py` instead.

   Why this file exists: the viewer runs with no game attached, so it cannot ask
   the game's prototype table the way the live Mod does
   (examples/sandbox-sim/plugin.cpp, `prototypeGeometry`).  A hand-written copy
   of that table is exactly the thing that drifts, and it did: the viewer
   refused boards carrying a 3 Bit Decoder, a Register or a Splitter while the
   live Mod simulated them happily.  Now the copy is reproducible.

   A board file that carries its own `inpins=`/`outpins=` - the live Mod writes
   them for every component - stays more authoritative than this table.  See
   kind_pins.hpp. */

inline const KindGeometry kKindGeometry[] = {{
{body}
}};
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="verify the header is current")
    args = parser.parse_args()
    if not KINDS.exists():
        print(f"{KINDS} is missing; run tests/kind-list-playtest.ps1 to dump it", file=sys.stderr)
        return 2
    text = render(parse(KINDS.read_text(encoding="utf-8")))
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current != text:
            print(f"{OUTPUT} is out of date; run python tools/kind-pins.py", file=sys.stderr)
            return 1
        print(f"{OUTPUT} is current")
        return 0
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUTPUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
