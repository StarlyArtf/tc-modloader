#!/usr/bin/env python3
"""Build a level circuit that puts built-in kinds between level inputs and
Output components, so the game itself compiles them.

Several kinds of component never appear in the game's own campaign circuits
(`tools/netlist-view.py --scan` counts zero uses for twelve of them), so their
behaviour cannot be read off a shipped board.  What *can* be done is to draw a
board that carries them and let the game compile it: with
TC_MODLOADER_DUMP_SOURCE=1 the loader keeps the program the game generated
(src/native_logic.hpp), and that program is the contract - it shows which pins
are read, in which phase, and what is written where.

The pins come from the pinned build's own prototype dump (`build/kinds.txt`),
including the storage kinds' second pin list at +0x70/+0x78 that
tests/kind-list-probe.cpp now prints as `hidden0=…`.  The level inputs and
outputs are cloned from a campaign level, so their records are the game's own.

Usage:
  python tools/kind-fixture.py --out build/kind-fixture/circuit.data 0x59 0x57
  python tools/kind-fixture.py --list 0x59

Then:
  powershell -File tests/kind-source-dump.ps1 -CircuitPath build/kind-fixture/circuit.data -Tag fixture
"""
from __future__ import annotations

import argparse
import copy
import dataclasses
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import circuit_format as cf  # noqa: E402

KINDS = ROOT / "build" / "kinds.txt"
TEMPLATE = ROOT.parent / "campaign" / "byte_adder" / "circuit.data"

DIRECTIONS = ((1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1))
AUTO = 0x7FFFFFFFFFFFFFFF


@dataclasses.dataclass
class Pin:
    x: int
    y: int
    width: int
    hidden: bool


def parse_kinds() -> dict[int, dict[str, object]]:
    """kind -> name, declared pins and the hidden pin list, from kinds.txt."""
    table: dict[int, dict[str, object]] = {}
    for line in KINDS.read_text(encoding="utf-8").splitlines():
        match = re.match(r"^0x([0-9a-f]+) name=\"([^\"]*)\".*", line)
        if not match:
            continue
        kind = int(match.group(1), 16)
        entry = table.setdefault(kind, {"name": match.group(2), "in": [], "out": [], "hidden": []})
        for group, index, x, y, width in re.findall(
                r"(hidden|in|out)(\d+)=\((-?\d+),(-?\d+),w(\d+)\)", line):
            width = int(width)
            # The sentinel widths: 0x7fff…ff is "auto", and the …fe/…fb variants
            # are the game's other auto markers.  Keep the raw value; the caller
            # decides whether a pin is narrow.
            entry[group].append(Pin(int(x), int(y), width, group == "hidden"))
    return table


def make_input(kind: int, x: int, y: int, bits: int, name: str, order: int,
               level_input: int) -> cf.Component:
    """A level input record, in the shape the campaign's own levels use."""
    return cf.Component(
        kind=kind, x=x, y=y, flags=0, identity=order, name=name, values=[], data=0,
        value_i16=-2 * order, bits=bits, bool_a=True, bool_b=False, init_data=0,
        value_i64_a=None, value_i64_b=None, subcomponents=[], settings={},
        custom_data=None, custom_pins=[])


def route(x0: int, y0: int, x1: int, y1: int) -> cf.Wire | None:
    """An L-shaped wire: horizontal first, then vertical."""
    segments: list[int] = []
    if x1 != x0:
        direction = 0 if x1 > x0 else 4
        segments.append((direction << 13) | abs(x1 - x0))
    if y1 != y0:
        direction = 2 if y1 > y0 else 6
        segments.append((direction << 13) | abs(y1 - y0))
    if not segments:
        return None
    return cf.Wire(color=0, name="", x=x0, y=y0, segments=segments + [0])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("kinds", nargs="*", help="kinds to place, e.g. 0x59 0x57")
    parser.add_argument("--out", type=pathlib.Path, default=None, help="where to write circuit.data")
    parser.add_argument("--list", action="store_true", help="just print each kind's pins")
    parser.add_argument("--bits", type=int, default=8,
                        help="the width written into each placed component's record")
    parser.add_argument("--standalone", action="store_true",
                        help="drive the kinds from a Constant and leave their outputs "
                             "unconnected, instead of using the level's own Input/Output "
                             "components (the sandbox erases those, because a level input "
                             "only exists when the level's ui.txt declares it)")
    args = parser.parse_args(argv)

    table = parse_kinds()
    wanted = [int(text, 16) for text in args.kinds]
    if args.list or not args.out:
        for kind in wanted:
            entry = table.get(kind)
            if not entry:
                print(f"0x{kind:02x}: not in {KINDS}")
                continue
            pins = " ".join(
                f"{'hidden' if pin.hidden else 'in'}{index}=({pin.x},{pin.y},w{pin.width})"
                for group in ("in", "hidden") for index, pin in enumerate(entry[group]))
            outs = " ".join(f"out{index}=({pin.x},{pin.y},w{pin.width})"
                            for index, pin in enumerate(entry["out"]))
            print(f"0x{kind:02x} {entry['name'] or '(unnamed)'}: {pins} {outs}")
        return 0

    circuit = cf.read_circuit(TEMPLATE)
    circuit.components = []
    circuit.wires = []
    identity = 1
    if args.standalone:
        # One Constant (0x2e, out0=(3,0)) drives every data pin, and nothing
        # reads the outputs - the point is the shape of the board, and a level
        # input would be erased by the sandbox unless the level's ui.txt declares
        # it.
        source = make_input(0x2E, -20, -20, args.bits, "value", 1, 0x2E)
        circuit.components.append(source)
        identity = 2  # the source took 1; a repeated identity makes the game drop one
        input_out = {1: (source.x + 3, source.y), 8: (source.x + 3, source.y)}
    else:
        # Two level inputs: one bit wide for control pins, one word wide for data.
        bit_in = make_input(0x3D, -30, -20, 1, "bit_in", 1, 0x3D)
        word_in = make_input(0x3D, -30, -12, 8, "word_in", 2, 0x3D)
        circuit.components += [bit_in, word_in]
        identity = 3  # two identities are taken; a repeat makes the game drop one
        input_out = {
            1: (bit_in.x + 4, bit_in.y),   # 0x3d Input: out0=(4,0)
            8: (word_in.x + 4, word_in.y),
        }
    y = -20
    for kind in wanted:
        entry = table.get(kind)
        if not entry:
            print(f"0x{kind:02x}: not in {KINDS}", file=sys.stderr)
            return 1
        component = make_input(kind, 0, y, args.bits, "", identity, kind)
        identity += 1
        circuit.components.append(component)
        pins: list[Pin] = list(entry["in"]) + list(entry["hidden"])
        for index, pin in enumerate(pins):
            narrow = pin.width == 1
            source = input_out[1 if narrow else 8]
            wire = route(source[0], source[1], component.x + pin.x, component.y + pin.y)
            if wire:
                circuit.wires.append(wire)
        if args.standalone:
            y += 10
            continue
        for index, pin in enumerate(entry["out"]):
            # Outputs go in their own column, one row apart, so two output pins
            # of the same component never land on the same cell.
            out_y = y + (index - (len(entry["out"]) - 1) / 2) * 3
            out_y = int(round(out_y))
            output = make_input(0x45, 30, out_y, 8 if pin.width not in (1,) else 1,
                                f"out_{kind:02x}_{index}", identity, 0x45)
            identity += 1
            circuit.components.append(output)
            # 0x45 Output: in0=(-3,0)
            wire = route(component.x + pin.x, component.y + pin.y, output.x - 3, output.y)
            if wire:
                circuit.wires.append(wire)
        y += 10
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(cf.encode_circuit(circuit))
    print(f"wrote {args.out}: {len(circuit.components)} components, {len(circuit.wires)} wires")
    for component in circuit.components:
        print(f"  kind=0x{component.kind:02x} at ({component.x},{component.y}) bits={component.bits} name={component.name!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
