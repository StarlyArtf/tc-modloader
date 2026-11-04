#!/usr/bin/env python3
"""Locate the pin-count fields inside a custom definition's component records.

tests/custom-prototype-probe.cpp dumps, for each custom prototype, the two
counted arrays the game keeps its definition in (`+0xe0`/`+0xe8` components,
`+0x100`/`+0x108` wires).  The elements use the board's own record layouts, but
the question this answers is where each field sits *within* them - the pin
counts in particular, because the netlist builder cross-checks them against the
prototype table and refuses a record whose numbers disagree.

The check is the game's own prototype dump: `build/kinds.txt` says every built-in
kind's pin counts, so for each offset the script counts how many definition
records agree with their kind's counts.  The offset where every record agrees is
the answer; the script prints all candidates so a wrong guess cannot hide.

Usage:
  python tools/definition-layout.py build/custom-prototype-dump/*definition*.txt
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def builtin_counts() -> dict[int, tuple[str, int, int]]:
    table: dict[int, tuple[str, int, int]] = {}
    line_re = re.compile(r"^0x([0-9a-f]+) name=\"([^\"]*)\".*inputs=(\d+) outputs=(\d+)")
    for line in (ROOT / "build" / "kinds.txt").read_text(encoding="utf-8").splitlines():
        match = line_re.match(line.strip())
        if match:
            table[int(match.group(1), 16)] = (match.group(2), int(match.group(3)), int(match.group(4)))
    return table


def components(path: pathlib.Path) -> list[bytes]:
    out: list[bytes] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("component["):
            continue
        _, _, rest = line.partition(" ")
        out.append(bytes(int(byte, 16) for byte in rest.split()))
    return out


def main(argv: list[str]) -> int:
    paths = [pathlib.Path(text) for text in argv] or sorted(
        (ROOT / "build" / "custom-prototype-dump").glob("*definition*.txt"))
    if not paths:
        print("no dumps; run tests/custom-prototype-playtest.ps1 first", file=sys.stderr)
        return 2
    table = builtin_counts()
    for path in paths:
        records = components(path)
        if not records:
            print(f"{path.name}: no component records")
            continue
        kinds = sorted({record[8] for record in records})
        print(f"{path.name}: {len(records)} record(s), kinds " +
              " ".join(f"0x{kind:02x}" for kind in kinds))
        for offset in range(0x20, 0x100, 8):
            inputs_ok = outputs_ok = total = 0
            for record in records:
                entry = table.get(record[8])
                if not entry or entry[1] > 64 or entry[2] > 64:
                    continue
                total += 1
                value = int.from_bytes(record[offset:offset + 8], "little")
                if value == entry[1]:
                    inputs_ok += 1
                if value == entry[2]:
                    outputs_ok += 1
            if total and (inputs_ok == total or outputs_ok == total):
                role = "inputs" if inputs_ok == total else "outputs"
                print(f"   +0x{offset:02x} matches every record's {role} ({total} record(s))")
        # The id and the coordinates are what the nets and the bridges need; print
        # where they sit as a second check of the shift the reader applies.
        sample = records[0]
        print("   first record: bytes 0x00..0x20 = " + " ".join(f"{byte:02x}" for byte in sample[:0x20]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
