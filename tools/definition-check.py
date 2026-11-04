#!/usr/bin/env python3
"""Audit a generated component definition against the rules the game applies.

The loader writes a generated inner circuit for every type it registers
(`component_definition::encode`), and hands the same bytes to the foundry so the
component can be opened there.  The game reads that design as an ordinary board:
it merges wires that touch at an endpoint, refuses a net with two drivers, and
refuses a combinational cycle.

This tool decodes such a definition (the game's own v13/v14 container, which is
what `encode` emits), prints every node and wire, and reports what the game
would see: nodes sharing an anchor, nets merged by an endpoint landing on
another wire, more than one driver per net, pins that are on no net, and
feedback loops.

Usage: python tools/definition-check.py <circuit.data> [more.data ...]
"""
from __future__ import annotations

import itertools
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import circuit_format as cf  # noqa: E402  (path set above)

ROOT = pathlib.Path(__file__).resolve().parent.parent
KINDS_FILE = ROOT / "build" / "kinds.txt"


def load_kinds(path: pathlib.Path) -> dict[int, tuple[list[tuple[int, int]], list[tuple[int, int]], str]]:
    """kind -> (input offsets, output offsets, name) from the game's own table."""
    kinds: dict[int, tuple[list[tuple[int, int]], list[tuple[int, int]], str]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.match(r'^(0x[0-9a-f]+) name="([^"]*)"', line)
        if not match:
            continue
        kind = int(match.group(1), 16)
        inputs = [(int(x), int(y)) for x, y in
                  re.findall(r"in\d+=\((-?\d+),(-?\d+),w\d+\)", line)]
        outputs = [(int(x), int(y)) for x, y in
                   re.findall(r"out\d+=\((-?\d+),(-?\d+),w\d+\)", line)]
        kinds[kind] = (inputs, outputs, match.group(2))
    return kinds


def wire_points(wire: cf.Wire) -> list[tuple[int, int]]:
    x, y = wire.x, wire.y
    points = [(x, y)]
    for segment in wire.segments:
        length = segment & 0x1FFF
        if length == 0:
            break
        dx, dy = cf.WIRE_DIRECTIONS[(segment >> 13) & 0x7]
        for _ in range(length):
            x += dx
            y += dy
            points.append((x, y))
    return points


def audit(path: pathlib.Path, kinds) -> int:
    circuit = cf.decode_bytes(path.read_bytes())
    print("=" * 72)
    print(f"{path}")
    print(f"  {len(circuit.components)} node(s), {len(circuit.wires)} wire(s), "
          f"header gates={circuit.header_i64_a} delay={circuit.header_i64_b}")
    problems = 0

    pins: dict[tuple[int, int], list[tuple[int, str]]] = {}
    anchors: dict[tuple[int, int], list[int]] = {}
    for index, component in enumerate(circuit.components):
        inputs, outputs, name = kinds.get(component.kind, ([], [], "?"))
        print(f"  {index:3d} 0x{component.kind:02x} {name:<20} at "
              f"({component.x:5d},{component.y:5d}) bits={component.bits} "
              f"ord={component.value_i16} label={component.name!r}")
        anchors.setdefault((component.x, component.y), []).append(index)
        for dx, dy in inputs:
            pins.setdefault((component.x + dx, component.y + dy), []).append((index, "in"))
        for dx, dy in outputs:
            pins.setdefault((component.x + dx, component.y + dy), []).append((index, "out"))

    for position, group in anchors.items():
        if len(group) > 1:
            problems += 1
            print(f"  !! {len(group)} nodes share one anchor {position}: {group}")

    paths = [wire_points(wire) for wire in circuit.wires]
    for index, points in enumerate(paths):
        print(f"  wire {index:3d}: " + " ".join(f"{x},{y}" for x, y in points))

    parent = list(range(len(paths)))

    def find(a: int) -> int:
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a: int, b: int) -> None:
        root_a, root_b = find(a), find(b)
        if root_a != root_b:
            parent[root_b] = root_a

    for a, b in itertools.combinations(range(len(paths)), 2):
        ends = {paths[a][0], paths[a][-1], paths[b][0], paths[b][-1]}
        shared = set(paths[a]) & set(paths[b])
        if shared & ends:
            union(a, b)

    # An endpoint that lands in the *interior* of another wire is a junction the
    # game makes on its own: the composite validator refuses that layout.
    for a in range(len(paths)):
        interiors = {p for b, other in enumerate(paths) if b != a for p in other[1:-1]}
        for endpoint in (paths[a][0], paths[a][-1]):
            if endpoint in interiors:
                problems += 1
                print(f"  !! wire {a} endpoint {endpoint} lands inside another wire")

    nets: dict[int, list[tuple[int, str]]] = {}
    for point, members in pins.items():
        for index, path in enumerate(paths):
            if point in path:
                nets.setdefault(find(index), []).extend(members)
                break

    edges: dict[int, set[int]] = {}
    driver_of_net: dict[int, list[int]] = {}
    upstream: dict[int, set[int]] = {}
    for root, members in nets.items():
        sources = sorted({index for index, role in members if role == "out"})
        sinks = sorted({index for index, role in members if role == "in"})
        if len(sources) > 1:
            problems += 1
            print(f"  !! net {root} has {len(sources)} drivers: {sources}")
        driver_of_net[root] = sources
        for source in sources:
            edges.setdefault(source, set()).update(sinks)
        for sink in sinks:
            upstream.setdefault(sink, set()).update(sources)

    state = [0] * len(circuit.components)

    def visit(index: int, stack: list[int]) -> None:
        nonlocal problems
        if state[index] == 1:
            problems += 1
            print("  !! CYCLE: " + " -> ".join(str(x) for x in stack + [index]))
            return
        if state[index] == 2:
            return
        state[index] = 1
        for source in sorted(edges.get(index, ())):
            visit(source, stack + [index])
        state[index] = 2

    for index in range(len(circuit.components)):
        visit(index, [])

    # What the wiring says: the set of input pins that can reach each driver.
    kinds_of = [component.kind for component in circuit.components]
    reach: dict[int, set[int]] = {}

    def reaches(from_index: int, seen: set[int]) -> set[int]:
        if from_index in reach:
            return reach[from_index]
        if from_index in seen:
            return set()
        seen = seen | {from_index}
        if kinds_of[from_index] == 0x4F:
            # An input pin drives its collector, not the other way round: what
            # reaches a driver are the *pin indices* found upstream of it.
            return {from_index}
        found: set[int] = set()
        for source in upstream.get(from_index, ()):
            found |= reaches(source, seen)
        reach[from_index] = found
        return found

    # Input-pin components are emitted first, in pin order, so their index is the
    # pin ordinal the caller declared the dependency mask in.  Print, for every
    # output pin, the inputs the wiring says it depends on - that set is what the
    # game's cycle detection sees, and it has to equal the declared mask.
    for index, kind in enumerate(kinds_of):
        if kind != 0x51:
            continue
        inputs = sorted(reaches(index, set()))
        print(f"  output pin {index} depends on input pins {inputs}")

    floating = [point for point in pins if not any(point in path for path in paths)]
    if floating:
        print(f"  note: {len(floating)} pin point(s) on no wire: {floating[:8]}")
    print(f"  -> {problems} problem(s)")
    return problems


def main(argv: list[str]) -> int:
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0
    kinds = load_kinds(KINDS_FILE)
    total = 0
    for argument in argv[1:]:
        total += audit(pathlib.Path(argument), kinds)
    return 1 if total else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
