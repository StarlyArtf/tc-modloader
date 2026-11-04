#!/usr/bin/env python3
"""Read a level's own circuit and print which pin of which component feeds which.

The campaign's `hint_*.data`, `hint_solution.data` and the `hint_custom/*`
folders are the **game's own** circuits, so they are real boards drawn by the
game: the pins come from its prototype table (transcribed below from
`build/kinds.txt`, the same table `simcore/include/tcsim/board.hpp` takes its
geometry from) and the wires come from the file.

That makes this tool the offline half of the sandbox's measurement problem.  Two
questions the prototype table cannot answer are answered by looking at what the
game itself drew:

  * **which leaf joins which leaf** - a Maker's `in0` is its topmost pin (it
    sits at `(-1,-3)` and comes first in the table), so a solution that shifts a
    word down by one tells us which end of the word the top pin is;
  * **which pins a component really uses** - e.g. whether a custom component's
    Input Pin (`0x4f`) reads from its outside neighbour or passes a value.

Usage:
  python tools/netlist-view.py campaign/byte_lsr/hint_solution.data
  python tools/netlist-view.py <circuit> --pins 0x10 0x11   # only these kinds
  python tools/netlist-view.py <circuit> --all              # every net, unsorted
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys
from collections import defaultdict

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import circuit_format as cf  # noqa: E402  (path set above)

ROOT = pathlib.Path(__file__).resolve().parent.parent
KINDS_FILE = ROOT / "build" / "kinds.txt"
AUTO = 0x7FFF_FFFF_FFFF_FFFF

ROW = re.compile(r"^0x([0-9a-f]{1,2}) name=\"([^\"]*)\".*?inputs=(\d+) outputs=(\d+)(.*)$")
PIN = re.compile(r"\b(in|out)(\d+)=\((-?\d+),(-?\d+),w(\d+)\)")


def load_kinds() -> dict[int, dict]:
    """The pinned build's prototype table: offsets and declared widths."""
    table: dict[int, dict] = {}
    if not KINDS_FILE.exists():
        return table
    for line in KINDS_FILE.read_text(encoding="utf-8").splitlines():
        match = ROW.match(line.strip())
        if not match:
            continue
        kind = int(match.group(1), 16)
        entry = {
            "name": match.group(2) or f"kind0x{kind:02x}",
            "in": [],
            "out": [],
            "in_bits": [],
            "out_bits": [],
        }
        for side, index, x, y, bits in PIN.findall(match.group(5)):
            del index
            if side == "in":
                entry["in"].append((int(x), int(y)))
                entry["in_bits"].append(int(bits))
            else:
                entry["out"].append((int(x), int(y)))
                entry["out_bits"].append(int(bits))
        table[kind] = entry
    return table


KINDS = load_kinds()
DIRECTIONS = ((1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1))


def wire_ends(wire: cf.Wire) -> tuple[tuple[int, int], tuple[int, int]]:
    x, y = wire.x, wire.y
    for segment in wire.segments:
        length = segment & 0x1FFF
        if length == 0:
            break
        dx, dy = DIRECTIONS[(segment >> 13) & 0x7]
        x += dx * length
        y += dy * length
    return (wire.x, wire.y), (x, y)


def pin_cells(circuit: cf.Circuit) -> dict[tuple[int, int], list[tuple[int, str, int]]]:
    cells: dict[tuple[int, int], list[tuple[int, str, int]]] = defaultdict(list)
    for index, component in enumerate(circuit.components):
        table = KINDS.get(component.kind)
        if not table:
            continue
        for pin, (dx, dy) in enumerate(table["in"]):
            cells[(component.x + dx, component.y + dy)].append((index, "in", pin))
        for pin, (dx, dy) in enumerate(table["out"]):
            cells[(component.x + dx, component.y + dy)].append((index, "out", pin))
    return cells


def label(circuit: cf.Circuit, index: int) -> str:
    component = circuit.components[index]
    table = KINDS.get(component.kind)
    name = component.name or (table["name"] if table else "")
    if not name:
        name = f"kind0x{component.kind:02x}"
    return f"c{index}:{name}@({component.x},{component.y})"


def collect_nets(circuit: cf.Circuit) -> dict[tuple[int, int], list[tuple[int, str, int]]]:
    cells = pin_cells(circuit)
    parent: dict[tuple[int, int], tuple[int, int]] = {}

    def find(point: tuple[int, int]) -> tuple[int, int]:
        parent.setdefault(point, point)
        while parent[point] != point:
            parent[point] = parent[parent[point]]
            point = parent[point]
        return point

    for wire in circuit.wires:
        start, end = wire_ends(wire)
        root_a, root_b = find(start), find(end)
        if root_a != root_b:
            parent[root_b] = root_a

    nets: dict[tuple[int, int], list[tuple[int, str, int]]] = defaultdict(list)
    for point, pins in cells.items():
        nets[find(point)].extend(pins)
    return nets


# The leaf kinds whose pin order the top-leaf-high convention is about.
SPLITTERS = {0x11, 0x63, 0x64, 0x6D, 0x6E}
MAKERS = {0x10, 0x61, 0x62, 0x6F, 0x70, 0x72, 0x73, 0x74}


def scan(directory: pathlib.Path, kinds: list[int]) -> int:
    """Walk every readable circuit under `directory` and report what it says.

    Four things the prototype table cannot answer, counted over the game's own
    circuits: how the leaves of a Splitter and a Maker are joined, how often each
    kind appears, **how each kind's pins are actually wired** (a pin with no wire
    is a boundary or a configuration input, not a signal), and **what its records
    carry** in the fields the file format exposes (`bits`, `value_i16`, the two
    bools, the settings values) - which is where a stateful kind keeps its
    configuration.
    """
    usage: dict[int, int] = defaultdict(int)
    deltas: dict[int, int] = defaultdict(int)
    indexers: dict[int, int] = defaultdict(int)
    wired: dict[int, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    fields: dict[int, dict[tuple, int]] = defaultdict(lambda: defaultdict(int))
    circuits = 0
    for path in sorted(directory.rglob("*.data")):
        payload = path.read_bytes()
        if not payload or payload[0] not in (13, 14):
            continue
        try:
            circuit = cf.decode_bytes(payload)
        except cf.CircuitError:
            continue
        circuits += 1
        nets = collect_nets(circuit)
        # Which component indexes have a wire on each of their pins.
        driven: dict[tuple[int, str, int], bool] = {}
        for pins in nets.values():
            for index, role, pin in pins:
                driven[(index, role, pin)] = len(pins) > 1
        for index, component in enumerate(circuit.components):
            usage[component.kind] += 1
            table = KINDS.get(component.kind)
            if table:
                for pin in range(len(table["in"])):
                    wired[component.kind]["in%d" % pin] += 1 if driven.get((index, "in", pin)) else 0
                for pin in range(len(table["out"])):
                    wired[component.kind]["out%d" % pin] += 1 if driven.get((index, "out", pin)) else 0
            if component.values or component.value_i16 or component.bool_a or component.bool_b:
                key = (tuple(component.values[:2]), component.bits, component.value_i16,
                       component.bool_a, component.bool_b)
                fields[component.kind][key] += 1
            if component.kind == 0x65 and component.values:
                indexers[int(component.values[0])] += 1
        for pins in nets.values():
            for index, role, pin in pins:
                if role != "out" or circuit.components[index].kind not in SPLITTERS:
                    continue
                for other, other_role, other_pin in pins:
                    if other_role == "in" and circuit.components[other].kind in MAKERS:
                        deltas[other_pin - pin] += 1
    print(f"scanned {circuits} circuit(s) under {directory}")
    print("splitter.out -> maker.in leaf delta: " + ", ".join(f"{k:+d}:{v}" for k, v in sorted(deltas.items())))
    print("kinds asked about:")
    for kind in kinds:
        table = KINDS.get(kind, {})
        print(f"  0x{kind:02x} {table.get('name', '?')!r}: {usage.get(kind, 0)} use(s)")
        if wired.get(kind):
            pins = ", ".join(f"{pin}:{count}" for pin, count in sorted(wired[kind].items()))
            print(f"      wired pins (wired/appearances): {pins}")
        for key, count in sorted(fields.get(kind, {}).items(), key=lambda item: -item[1])[:3]:
            values, bits, value_i16, bool_a, bool_b = key
            print(f"      record x{count}: values={list(values)} bits={bits} i16={value_i16} "
                  f"boolA={bool_a} boolB={bool_b}")
    if indexers:
        print("Static Indexer (0x65) configured indexes: " +
              ", ".join(f"{k}:{v}" for k, v in sorted(indexers.items())[:16]))
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=pathlib.Path, nargs="?", default=None)
    parser.add_argument("--pins", nargs="*", default=None, help="only nets touching these kinds, e.g. 0x10 0x11")
    parser.add_argument("--all", action="store_true", help="list every net, including one-pin ones")
    parser.add_argument("--scan", metavar="DIR", type=pathlib.Path, default=None,
                        help="walk a directory of circuits and report leaf deltas and kind usage")
    parser.add_argument("--kinds", nargs="*", default=[], help="kinds the --scan report should name")
    args = parser.parse_args(argv)

    if args.scan is not None:
        return scan(args.scan, [int(text, 16) for text in args.kinds])
    if args.path is None:
        parser.error("a circuit path is required unless --scan is used")

    only = {int(text, 16) for text in args.pins} if args.pins else None
    circuit = cf.read_circuit(args.path)
    nets = collect_nets(circuit)

    print(f"{args.path}")
    print(f"  {len(circuit.components)} components, {len(circuit.wires)} wires, {len(nets)} nets, "
          f"{len(KINDS)} kinds known")
    known = sum(1 for component in circuit.components if component.kind in KINDS)
    if known != len(circuit.components):
        missing = sorted({hex(c.kind) for c in circuit.components if c.kind not in KINDS})
        print(f"  WARNING: {len(circuit.components) - known} component(s) have no pin table: {missing}")
    index = 0
    for pins in sorted(nets.values(), key=lambda group: -len(group)):
        if not args.all and len(pins) < 2 and only is None:
            continue
        if only is not None and not any(circuit.components[pin[0]].kind in only for pin in pins):
            continue
        index += 1
        parts = []
        for component_index, role, pin in sorted(pins):
            table = KINDS.get(circuit.components[component_index].kind)
            if table:
                bits = table[f"{role}_bits"][pin]
                bits_text = "auto" if bits >= AUTO else str(bits)
            else:
                bits_text = "?"
            parts.append(f"{label(circuit, component_index)}.{role}{pin}(w{bits_text})")
        print(f"  net{index}: " + "  <->  ".join(parts))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
