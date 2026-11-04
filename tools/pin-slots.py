#!/usr/bin/env python3
"""Print the state slot of every wire that lands on a component's pin.

The sandbox simulator reads and writes the game's state array by byte offset, and
a wire's offset is the only place the game keeps it.  `build/state-map.txt` is a
whole board captured that way, but a fresh level has no map yet: the offsets are
allocated when the game loads the circuit.  This tool answers "which slot does the
output of component N land on" from a level's own `circuit.data` by joining the
wire endpoints with the pinned cells of the components around them - the same pin
table `tools/netlist-view.py` uses.

Usage:
  python tools/pin-slots.py campaign/overture_decoder/hint_solution.data
  python tools/pin-slots.py <circuit> --kind 0x2c          # only this kind's pins
  python tools/pin-slots.py <circuit> --dump-json out.json # also write the records
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys
from collections import defaultdict

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import circuit_format as cf  # noqa: E402  (path set above)

_spec = importlib.util.spec_from_file_location("netlist_view", HERE / "netlist-view.py")
netlist_view = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(netlist_view)

KINDS = netlist_view.KINDS


def known_pins() -> dict[int, dict]:
    return KINDS


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=pathlib.Path)
    parser.add_argument("--kind", default=None, help="only this kind's pins, e.g. 0x2c")
    parser.add_argument("--dump-json", type=pathlib.Path, default=None,
                        help="write the decoded circuit as JSON for hand editing")
    args = parser.parse_args(argv)

    only = int(args.kind, 16) if args.kind else None
    circuit = cf.read_circuit(args.path)
    if args.dump_json:
        args.dump_json.write_text(json.dumps(cf.circuit_to_dict(circuit), indent=2, ensure_ascii=False),
                                  encoding="utf-8")

    # Every pin cell of every component whose prototype is known.
    cells: dict[tuple[int, int], list[tuple[int, str, int]]] = defaultdict(list)
    for index, component in enumerate(circuit.components):
        table = KINDS.get(component.kind)
        if not table:
            continue
        for pin, (dx, dy) in enumerate(table["in"]):
            cells[(component.x + dx, component.y + dy)].append((index, "in", pin))
        for pin, (dx, dy) in enumerate(table["out"]):
            cells[(component.x + dx, component.y + dy)].append((index, "out", pin))

    print(f"{args.path}: {len(circuit.components)} components, {len(circuit.wires)} wires")
    print("NOTE: a level circuit carries no state offsets yet - the game allocates them while")
    print("      loading, so the slot column below is an index, not a live offset.")
    index = 0
    for wire in circuit.wires:
        ends = netlist_view.wire_ends(wire)
        touching: list[tuple[int, str, int]] = []
        for point in ends:
            touching.extend(cells.get(point, []))
        if not touching:
            continue
        if only is not None and not any(circuit.components[i].kind == only for i, _, _ in touching):
            continue
        index += 1
        parts = []
        for component_index, role, pin in sorted(touching):
            component = circuit.components[component_index]
            table = KINDS.get(component.kind, {})
            name = component.name or table.get("name", f"kind0x{component.kind:02x}")
            parts.append(f"c{component_index}:{name}.{role}{pin}")
        print(f"  wire{index} ends={ends[0]}->{ends[1]}: " + "  ".join(parts))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
