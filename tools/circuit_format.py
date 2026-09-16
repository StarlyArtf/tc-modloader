#!/usr/bin/env python3
"""Inspect and build the pinned game's snappy-compressed circuit.data.

This is development tooling for the reverse-engineered save format used by
Turing Complete 2.1.334.  Circuit files start with a one-byte format version;
the remaining bytes are a raw Snappy block.  Versions 13 and 14 share the
layout implemented here.

The parser intentionally keeps every unknown field instead of dropping it so a
decoded circuit can be written back without losing data.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import struct
import sys
from pathlib import Path
from typing import Any

try:
    import snappy  # type: ignore
except ImportError as exc:  # pragma: no cover - exercised by the CLI
    raise SystemExit("python-snappy is required: python -m pip install python-snappy") from exc


SUPPORTED_VERSIONS = (13, 14)
KIND_INPUT = 0x3F
KIND_OUTPUT = 0x44


class CircuitError(ValueError):
    pass


class Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0

    def take(self, size: int) -> bytes:
        if size < 0 or self.pos + size > len(self.data):
            raise CircuitError(f"truncated circuit at offset 0x{self.pos:x}")
        result = self.data[self.pos : self.pos + size]
        self.pos += size
        return result

    def u8(self) -> int:
        return self.take(1)[0]

    def bool(self) -> bool:
        return self.u8() != 0

    def i16(self) -> int:
        return struct.unpack("<h", self.take(2))[0]

    def u16(self) -> int:
        return struct.unpack("<H", self.take(2))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def i64(self) -> int:
        return struct.unpack("<q", self.take(8))[0]

    def string(self) -> str:
        return self.take(self.u16()).decode("utf-8", "replace")

    def seq_i64(self) -> list[int]:
        return [self.i64() for _ in range(self.u16())]

    def seq_u8(self) -> list[int]:
        return list(self.take(self.u16()))


class Writer:
    def __init__(self) -> None:
        self.parts: list[bytes] = []

    def add(self, data: bytes) -> None:
        self.parts.append(data)

    def u8(self, value: int) -> None:
        self.add(bytes((value & 0xFF,)))

    def bool(self, value: bool) -> None:
        self.u8(1 if value else 0)

    def i16(self, value: int) -> None:
        self.add(struct.pack("<h", value))

    def u16(self, value: int) -> None:
        self.add(struct.pack("<H", value))

    def u32(self, value: int) -> None:
        self.add(struct.pack("<I", value))

    def i64(self, value: int) -> None:
        self.add(struct.pack("<q", value))

    def string(self, value: str) -> None:
        data = value.encode("utf-8")
        self.u16(len(data))
        self.add(data)

    def seq_i64(self, values: list[int]) -> None:
        self.u16(len(values))
        for value in values:
            self.i64(value)

    def seq_u8(self, values: list[int]) -> None:
        self.u16(len(values))
        self.add(bytes(values))

    def build(self) -> bytes:
        return b"".join(self.parts)


@dataclasses.dataclass
class SubComponent:
    parent_id: int
    child_id: int
    name: str
    mode: int
    bits: int


@dataclasses.dataclass
class Component:
    kind: int
    x: int
    y: int
    flags: int
    identity: int
    name: str
    values: list[int]
    data: int
    value_i16: int
    bits: int
    bool_a: bool
    bool_b: bool
    init_data: int
    value_i64_a: int | None
    value_i64_b: int | None
    subcomponents: list[SubComponent]
    settings: dict[str, str]
    custom_data: int | None
    custom_pins: list[tuple[int, int]]

    @property
    def is_input(self) -> bool:
        return self.kind == KIND_INPUT

    @property
    def is_output(self) -> bool:
        return self.kind == KIND_OUTPUT


@dataclasses.dataclass
class Wire:
    color: int
    name: str
    x: int
    y: int
    segments: list[int]


@dataclasses.dataclass
class Circuit:
    version: int
    seed: int
    header_u32: int
    header_i64_a: int
    header_i64_b: int
    header_bool: bool
    header_u64: int
    header_ints: list[int]
    header_string: str
    header_u8: int
    header_u16: int
    header_bytes: list[int]
    header_string2: str
    palette: list[int]
    components: list[Component]
    wires: list[Wire]


def decode_bytes(payload: bytes) -> Circuit:
    if not payload:
        raise CircuitError("empty circuit payload")
    version = payload[0]
    if version not in SUPPORTED_VERSIONS:
        raise CircuitError(f"unsupported circuit version {version}")
    raw = snappy.decompress(payload[1:])
    reader = Reader(raw)
    seed = reader.i64()
    header_u32 = reader.u32()
    header_i64_a = reader.i64()
    header_i64_b = reader.i64()
    header_bool = reader.bool()
    header_u64 = reader.i64()
    header_ints = reader.seq_i64()
    header_string = reader.string()
    header_u8 = reader.u8()
    header_u16 = reader.u16()
    header_bytes = reader.seq_u8()
    header_string2 = reader.string()
    palette: list[int] = []
    if seed:
        palette = list(reader.take(512))

    components: list[Component] = []
    for _ in range(reader.i64()):
        kind = reader.u16()
        x = reader.i16()
        y = reader.i16()
        flags = reader.u8()
        identity = reader.i64()
        name = reader.string()
        values = [reader.i64() for _ in range(reader.u16())]
        data = reader.i64()
        value_i16 = reader.i16()
        bits = reader.i64()
        bool_a = reader.bool()
        value_i64_a = None
        value_i64_b = None
        if version == 14:
            value_i64_a = reader.i64()
            value_i64_b = reader.i64()
            bool_b = reader.bool()
        else:
            bool_b = reader.bool()
        init_data = reader.u8()
        subcomponents = []
        for _ in range(reader.u16()):
            subcomponents.append(
                SubComponent(
                    parent_id=reader.i64(),
                    child_id=reader.i64(),
                    name=reader.string(),
                    mode=reader.i64(),
                    bits=reader.i64(),
                )
            )
        settings = {}
        for _ in range(reader.u16()):
            settings[reader.string()] = reader.string()
        custom_data = None
        custom_pins: list[tuple[int, int]] = []
        if kind == 0x4E:
            custom_data = reader.i64()
            for _ in range(reader.u16()):
                custom_pins.append((reader.i64(), reader.i64()))
        components.append(
            Component(
                kind=kind,
                x=x,
                y=y,
                flags=flags,
                identity=identity,
                name=name,
                values=values,
                data=data,
                value_i16=value_i16,
                bits=bits,
                bool_a=bool_a,
                bool_b=bool_b,
                init_data=init_data,
                value_i64_a=value_i64_a,
                value_i64_b=value_i64_b,
                subcomponents=subcomponents,
                settings=settings,
                custom_data=custom_data,
                custom_pins=custom_pins,
            )
        )

    wires: list[Wire] = []
    for _ in range(reader.i64()):
        color = reader.u8()
        name = reader.string()
        x = reader.i16()
        y = reader.i16()
        segments: list[int] = []
        while True:
            segment = reader.u16()
            segments.append(segment)
            if segment & 0x1FFF == 0:
                break
        wires.append(
            Wire(
                color=color,
                name=name,
                x=x,
                y=y,
                segments=segments,
            )
        )
    if reader.pos != len(raw):
        raise CircuitError(
            f"circuit parser stopped at 0x{reader.pos:x}, payload is 0x{len(raw):x}"
        )
    return Circuit(
        version=version,
        seed=seed,
        header_u32=header_u32,
        header_i64_a=header_i64_a,
        header_i64_b=header_i64_b,
        header_bool=header_bool,
        header_u64=header_u64,
        header_ints=header_ints,
        header_string=header_string,
        header_u8=header_u8,
        header_u16=header_u16,
        header_bytes=header_bytes,
        header_string2=header_string2,
        palette=palette,
        components=components,
        wires=wires,
    )


def encode_circuit(circuit: Circuit) -> bytes:
    if circuit.version not in SUPPORTED_VERSIONS:
        raise CircuitError(f"unsupported circuit version {circuit.version}")
    writer = Writer()
    writer.i64(circuit.seed)
    writer.u32(circuit.header_u32)
    writer.i64(circuit.header_i64_a)
    writer.i64(circuit.header_i64_b)
    writer.bool(circuit.header_bool)
    writer.i64(circuit.header_u64)
    writer.seq_i64(circuit.header_ints)
    writer.string(circuit.header_string)
    writer.u8(circuit.header_u8)
    writer.u16(circuit.header_u16)
    writer.seq_u8(circuit.header_bytes)
    writer.string(circuit.header_string2)
    if circuit.seed:
        if len(circuit.palette) != 512:
            raise CircuitError("a non-zero seed requires a 512-byte palette")
        writer.add(bytes(circuit.palette))
    writer.i64(len(circuit.components))
    for component in circuit.components:
        writer.u16(component.kind)
        writer.i16(component.x)
        writer.i16(component.y)
        writer.u8(component.flags)
        writer.i64(component.identity)
        writer.string(component.name)
        writer.u16(len(component.values))
        for value in component.values:
            writer.i64(value)
        writer.i64(component.data)
        writer.i16(component.value_i16)
        writer.i64(component.bits)
        writer.bool(component.bool_a)
        if circuit.version == 14:
            writer.i64(component.value_i64_a or 0)
            writer.i64(component.value_i64_b or 0)
            writer.bool(component.bool_b)
        else:
            writer.bool(component.bool_b)
        writer.u8(component.init_data)
        writer.u16(len(component.subcomponents))
        for subcomponent in component.subcomponents:
            writer.i64(subcomponent.parent_id)
            writer.i64(subcomponent.child_id)
            writer.string(subcomponent.name)
            writer.i64(subcomponent.mode)
            writer.i64(subcomponent.bits)
        writer.u16(len(component.settings))
        for key, value in component.settings.items():
            writer.string(key)
            writer.string(value)
        if component.kind == 0x4E:
            writer.i64(component.custom_data or 0)
            writer.u16(len(component.custom_pins))
            for custom_id, bits in component.custom_pins:
                writer.i64(custom_id)
                writer.i64(bits)
    writer.i64(len(circuit.wires))
    for wire in circuit.wires:
        writer.u8(wire.color)
        writer.string(wire.name)
        writer.i16(wire.x)
        writer.i16(wire.y)
        for segment in wire.segments:
            writer.u16(segment)
    raw = writer.build()
    return bytes((circuit.version,)) + snappy.compress(raw)


def read_circuit(path: Path) -> Circuit:
    return decode_bytes(path.read_bytes())


def write_circuit(path: Path, circuit: Circuit) -> None:
    path.write_bytes(encode_circuit(circuit))


def component_to_dict(component: Component) -> dict[str, Any]:
    result = dataclasses.asdict(component)
    result["kind_hex"] = f"0x{component.kind:02x}"
    result["identity_hex"] = f"0x{component.identity:016x}"
    result["is_input"] = component.is_input
    result["is_output"] = component.is_output
    return result


def circuit_to_dict(circuit: Circuit) -> dict[str, Any]:
    return {
        "version": circuit.version,
        "seed": circuit.seed,
        "header_u32": circuit.header_u32,
        "header_i64_a": circuit.header_i64_a,
        "header_i64_b": circuit.header_i64_b,
        "header_bool": circuit.header_bool,
        "header_u64": circuit.header_u64,
        "header_ints": circuit.header_ints,
        "header_string": circuit.header_string,
        "header_u8": circuit.header_u8,
        "header_u16": circuit.header_u16,
        "header_bytes": circuit.header_bytes,
        "header_string2": circuit.header_string2,
        "palette_bytes": len(circuit.palette),
        "components": [component_to_dict(c) for c in circuit.components],
        "wires": [dataclasses.asdict(w) for w in circuit.wires],
    }


def summarize(circuit: Circuit) -> str:
    lines = [
        f"version={circuit.version} components={len(circuit.components)} "
        f"wires={len(circuit.wires)} palette={len(circuit.palette)}",
    ]
    for index, component in enumerate(circuit.components):
        label = component.name or ("Input" if component.is_input else "Output" if component.is_output else "")
        lines.append(
            f"component[{index}] kind=0x{component.kind:02x} "
            f"pos=({component.x},{component.y}) id=0x{component.identity:016x} "
            f"name={label!r}"
        )
    for index, wire in enumerate(circuit.wires):
        lines.append(
            f"wire[{index}] color={wire.color} pos=({wire.x},{wire.y}) "
            f"segments={len(wire.segments)} name={wire.name!r}"
        )
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Design statistics policy
#
# A definition header caches the design's (gate count, delay).  The game
# recomputes the gate count while parsing but keeps the stored delay verbatim,
# and both values are what the UI reports for the component, so a definition
# must carry its real critical path.
#
# The rules below only cover what has been measured on the pinned build.  When
# a design cannot be reduced by those rules it keeps its native statistics
# instead of receiving an invented pair: that is the safe default for feedback
# loops, multiple drivers, unknown kinds and nested custom components.
# ---------------------------------------------------------------------------

# (gates, delay) contributed by a component, limited to kinds the fixtures
# exercise directly.  Anything outside this table keeps its native statistics:
# the runtime timing shim asks the game for each node's own cost instead of
# relying on a table, so this is only the design-time check for our fixtures.
KIND_COST: dict[int, tuple[int, int]] = {
    0x03: (1, 1),  # NOT
    0x04: (1, 1),  # AND
    0x3F: (0, 0),  # level input pin
    0x44: (0, 0),  # level output pin
    0x46: (0, 0),
    0x4F: (0, 0),  # custom input pin
    0x50: (0, 0),
    0x51: (0, 0),  # custom output pin
}

# Verified pin offsets relative to the component origin: (inputs, outputs).
KIND_PINS: dict[int, tuple[tuple[tuple[int, int], ...], tuple[tuple[int, int], ...]]] = {
    0x03: (((-1, 0),), ((2, 0),)),
    0x04: (((-1, -1), (-1, 1)), ((2, 0),)),
    0x3F: ((), ((0, -1), (0, 1))),
    0x44: (((-1, 0),), ()),
    0x4F: ((), ((3, 0),)),
    0x51: (((-3, 0),), ()),
}

# 0=East, 1=South-East, 2=South, 3=South-West, 4=West, 5=North-West, 6=North, 7=North-East
WIRE_DIRECTIONS = ((1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1))


def wire_endpoints(wire: Wire) -> tuple[tuple[int, int], tuple[int, int]]:
    """Returns the two ends of a wire path in schematic grid coordinates."""
    x, y = wire.x, wire.y
    for segment in wire.segments:
        length = segment & 0x1FFF
        if length == 0:
            break
        dx, dy = WIRE_DIRECTIONS[(segment >> 13) & 0x7]
        x += dx * length
        y += dy * length
    return (wire.x, wire.y), (x, y)


def analyze_design(circuit: Circuit) -> dict[str, Any]:
    """Decides whether the verified rules apply to a design.

    Returns a report with either the verified (gates, delay) pair or the
    instruction to keep the native statistics, together with the reason.
    """
    report: dict[str, Any] = {
        "components": len(circuit.components),
        "wires": len(circuit.wires),
        "verified": False,
        "reason": "",
        "native_gates": circuit.header_i64_a,
        "native_delay": circuit.header_i64_b,
    }

    unknown = sorted(
        {
            component.kind
            for component in circuit.components
            if component.kind not in KIND_COST or component.kind not in KIND_PINS
        }
    )
    if unknown:
        kinds = ", ".join(f"0x{kind:02x}" for kind in unknown)
        report["reason"] = f"unverified component kind(s) {kinds}"
        return report

    pin_map: dict[tuple[int, int], list[tuple[int, str]]] = {}
    for index, component in enumerate(circuit.components):
        inputs, outputs = KIND_PINS[component.kind]
        for dx, dy in inputs:
            pin_map.setdefault((component.x + dx, component.y + dy), []).append((index, "in"))
        for dx, dy in outputs:
            pin_map.setdefault((component.x + dx, component.y + dy), []).append((index, "out"))

    parent: dict[tuple[int, int], tuple[int, int]] = {}

    def find(point: tuple[int, int]) -> tuple[int, int]:
        parent.setdefault(point, point)
        while parent[point] != point:
            parent[point] = parent[parent[point]]
            point = parent[point]
        return point

    def union(a: tuple[int, int], b: tuple[int, int]) -> None:
        root_a, root_b = find(a), find(b)
        if root_a != root_b:
            parent[root_b] = root_a

    for wire in circuit.wires:
        start, end = wire_endpoints(wire)
        union(start, end)

    nets: dict[tuple[int, int], list[tuple[int, str]]] = {}
    for point in parent:
        if point in pin_map:
            nets.setdefault(find(point), []).extend(pin_map[point])

    drivers: dict[int, list[int]] = {}
    for pins in nets.values():
        sources = [index for index, role in pins if role == "out"]
        sinks = [index for index, role in pins if role == "in"]
        if len(sources) > 1:
            report["reason"] = (
                f"multiple drivers on one net ({len(sources)} outputs)"
            )
            return report
        for source in sources:
            drivers.setdefault(source, []).extend(sinks)

    # Longest combinational path; a cycle means the design keeps native stats.
    state = [0] * len(circuit.components)
    delay = [0] * len(circuit.components)

    def visit(index: int) -> int:
        if state[index] == 1:
            raise CircuitError("feedback loop detected")
        if state[index] == 2:
            return delay[index]
        state[index] = 1
        best = 0
        for source, sinks in drivers.items():
            if index in sinks:
                best = max(best, visit(source))
        state[index] = 2
        delay[index] = best + KIND_COST[circuit.components[index].kind][1]
        return delay[index]

    try:
        total_delay = 0
        for index in range(len(circuit.components)):
            total_delay = max(total_delay, visit(index))
    except CircuitError as exc:
        report["reason"] = str(exc)
        return report

    gates = sum(KIND_COST[component.kind][0] for component in circuit.components)
    report.update(
        {
            "verified": True,
            "gates": gates,
            "delay": total_delay,
            "reason": "verified component kinds, single driver per net, acyclic",
            "native_gates": circuit.header_i64_a,
            "native_delay": circuit.header_i64_b,
        }
    )
    return report


def policy_line(report: dict[str, Any]) -> str:
    if report["verified"]:
        return (
            f"POLICY=verified gates={report['gates']} delay={report['delay']} "
            f"native=({report['native_gates']},{report['native_delay']}) "
            f"reason=\"{report['reason']}\""
        )
    return (
        f"POLICY=keep_native reason=\"{report['reason']}\" "
        f"native=({report['native_gates']},{report['native_delay']})"
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="circuit.data to inspect")
    parser.add_argument("--json", action="store_true", help="print full decoded JSON")
    parser.add_argument(
        "--analyze",
        action="store_true",
        help="report whether the verified design-statistic rules apply",
    )
    args = parser.parse_args(argv)
    circuit = read_circuit(args.path)
    if args.analyze:
        print(policy_line(analyze_design(circuit)))
    elif args.json:
        print(json.dumps(circuit_to_dict(circuit), indent=2, ensure_ascii=False))
    else:
        print(summarize(circuit))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except CircuitError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
