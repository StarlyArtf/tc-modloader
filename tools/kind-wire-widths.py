#!/usr/bin/env python3
"""Print a kind's pins beside the widths of the wires that reach them.

Why this exists: two families of the prototype table declare `wauto` on their
data pins - the auto-width Splitters (0x63 / 0x64 / 0x2f, "splits a (4n)-bit
value into four n-bit values") and the Word Makers (0x30 / 0x61 / 0x62) - and
the table therefore does not say how wide their data path is.  The sandbox
refuses them rather than guessing (red line 6), which is honest but useless on a
board that really carries one.

The rule is measurable off a *real* board instead: `sandbox-sim-board.txt` (the
record the live Mod writes, see simcore/view/board_file.hpp) carries every
component's record and every wire's width, and the component's pin offsets come
from `build/kinds.txt`.  So for each instance this prints

    kind 0x30 at x,y rotation r  record(width=..)
      in0 (-2,-1) <- wire width 8
      out0 (1,0)  -> wire width 16

and the answers fall out of the numbers: a Word Maker's output is the sum of its
input widths, and an auto Splitter's input is its output count times the width
of one leaf.

Usage:
    python tools/kind-wire-widths.py <sandbox-sim-board.txt> [--kinds 0x30 0x63 ...]
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# The kinds this tool was written for; `--kinds` may ask about others.
DEFAULT_KINDS = (0x2F, 0x30, 0x61, 0x62, 0x63, 0x64)

HEADER = re.compile(
    r'^0x(?P<kind>[0-9a-fA-F]+)\s+name="(?P<name>[^"]*)"'
    r"\s+gates=\d+\s+delay=\d+\s+inputs=(?P<inputs>\d+)\s+outputs=(?P<outputs>\d+)"
)
PIN = re.compile(r"(in|out|hidden)(\d+)=\((-?\d+),(-?\d+),w(\d+)\)")
AUTO = 0x7FFF_FFFF_FFFF_FF00


def prototype_pins() -> dict[int, tuple[str, list[tuple[int, int]]]]:
    """kind -> (name, pins), pins being declared inputs then hidden inputs then outputs."""
    table: dict[int, tuple[str, list[tuple[int, int]]]] = {}
    for line in (ROOT / "build" / "kinds.txt").read_text(encoding="utf-8").splitlines():
        match = HEADER.match(line.strip())
        if not match:
            continue
        kind = int(match.group("kind"), 16)
        found: dict[str, list[tuple[int, int]]] = {"in": [], "hidden": [], "out": []}
        for group, index, x, y, bits in PIN.findall(line):
            found[group].append((int(index), int(x), int(y), int(bits)))
        ins = [pin for pin in sorted(found["in"])] + [pin for pin in sorted(found["hidden"])]
        outs = sorted(found["out"])
        table[kind] = (
            match.group("name"),
            [(x, y, bits) for _, x, y, bits in ins],
            [(x, y, bits) for _, x, y, bits in outs],
        )
    return table


def rotate(x: int, y: int, rotation: int) -> tuple[int, int]:
    """The game rotates a pin offset by `rotation` quarter turns (measured convention:
    the same one simcore's `rotateOffset` uses)."""
    for _ in range(rotation & 3):
        x, y = -y, x
    return x, y


COMPONENT = re.compile(
    r"^(?P<prefix>def)?component\b.*?kind=0x(?P<kind>[0-9a-fA-F]+)"
    r".*?\bx=(?P<x>-?\d+)\s+y=(?P<y>-?\d+)"
    r".*?\brotation=(?P<rotation>-?\d+)"
    r".*?\bword30=(?P<in>\d+)\s+word40=(?P<out>\d+)"
)
WIRE = re.compile(
    r"^(?P<prefix>def)?wire\b.*?\bfrom=(?P<x1>-?\d+),(?P<y1>-?\d+)\s+to=(?P<x2>-?\d+),(?P<y2>-?\d+)"
    r"\s+width=(?P<width>\d+)"
)
DEFINITION = re.compile(r"\bdef=0x(?P<definition>[0-9a-fA-F]+)")


def read_board(path: pathlib.Path):
    components = []  # (kind, x, y, rotation, definition or None)
    wires = []  # (x1, y1, x2, y2, width, definition or None)
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        definition = DEFINITION.search(line)
        scope = definition.group("definition") if definition else None
        match = COMPONENT.match(line)
        if match:
            components.append(
                (
                    int(match.group("kind"), 16),
                    int(match.group("x")),
                    int(match.group("y")),
                    int(match.group("rotation")),
                    scope,
                )
            )
            continue
        match = WIRE.match(line)
        if match:
            wires.append(
                (
                    int(match.group("x1")),
                    int(match.group("y1")),
                    int(match.group("x2")),
                    int(match.group("y2")),
                    int(match.group("width")),
                    scope,
                )
            )
    return components, wires


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("board", type=pathlib.Path)
    parser.add_argument("--kinds", nargs="*", default=None)
    args = parser.parse_args()
    kinds = (
        {int(value, 16) for value in args.kinds} if args.kinds else set(DEFAULT_KINDS)
    )
    if not args.board.exists():
        print(f"no such board file: {args.board}", file=sys.stderr)
        return 2
    table = prototype_pins()
    components, wires = read_board(args.board)
    print(f"{args.board}: {len(components)} component(s), {len(wires)} wire(s)")
    interesting = 0
    for kind, x, y, rotation, scope in components:
        if kind not in kinds:
            continue
        interesting += 1
        name, ins, outs = table.get(kind, ("?", [], []))
        where = f"def 0x{scope}" if scope else "board"
        print(f"\nkind 0x{kind:02X} ({name}) at {x},{y} r{rotation} in {where}")
        for label, pins in (("in", ins), ("out", outs)):
            for index, (px, py, bits) in enumerate(pins):
                dx, dy = rotate(px, py, rotation)
                cell = (x + dx, y + dy)
                hits = [
                    wire
                    for wire in wires
                    if wire[5] == scope
                    and ((wire[0], wire[1]) == cell or (wire[2], wire[3]) == cell)
                ]
                widths = ", ".join(str(wire[4]) for wire in hits) or "not wired"
                declared = "auto" if bits >= AUTO or bits == 0 else str(bits)
                print(f"  {label}{index} {cell} declared={declared:>4}  wire(s): {widths}")
    if interesting == 0:
        print(f"\nnone of the asked kinds ({', '.join(f'0x{k:02X}' for k in sorted(kinds))}) is on this board")
    return 0


if __name__ == "__main__":
    sys.exit(main())
