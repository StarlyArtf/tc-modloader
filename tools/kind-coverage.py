#!/usr/bin/env python3
"""Which built-in kinds does simcore's board builder accept today?

Reads the pinned build's own prototype dump (build/kinds.txt) and the tables in
simcore/include/tcsim/board.hpp, then prints the coverage summary the sandbox
plan wants: supported, hand-modelled elsewhere, inactive slots, and still
missing.

Development aid; the durable record is
docs/research/simcore-kind-coverage.md.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
KINDS = ROOT / "build" / "kinds.txt"
BOARD = ROOT / "simcore" / "include" / "tcsim" / "board.hpp"

# Kinds board.hpp handles with a dedicated branch (not through the device
# library), plus the ones the prototype table marks as inactive.
HANDLED_IN_BOARD = {
    0x0C: "tri-state bit switch",
    0x19: "tri-state word switch",
    0x2A: "word mux (width from record)",
    0x76: "RAM state holder",
    0x5B: "screen settings latch",
    0x01: "OFF constant", 0x02: "ON constant", 0x2E: "Constant",
    0x36: "Load port", 0x38: "Store port",
    0x34: "Push Button (host source)",
    0x3C: "Input", 0x3D: "Input", 0x3F: "Input", 0x40: "Input", 0x41: "Input",
    0x5F: "Verilog Input", 0x5C: "Time", 0x5D: "Keyboard",
    0x28: "Output", 0x3A: "Output", 0x44: "Output", 0x45: "Output",
    0x49: "Output", 0x4A: "Output", 0x4B: "Output",
    0x52: "Memory Probe (bit)", 0x54: "Wire Probe (bit)", 0x55: "Wire Probe",
    0x60: "Verilog Output", 0x46: "Switched Output",
    # The storage family and the Auto-size pair: their devices live in
    # devices.hpp (deviceRegister / deviceCounter / deviceDelayLine) and their
    # pins in the prototype's hidden list, which board.hpp's `storageKindSpec`
    # table names.  See docs/research/simcore-kind-coverage.md section 7.
    0x27: "storage: Register", 0x0E: "storage: Register (bit)",
    0x26: "storage: Counter",
    0x37: "storage: Delay Line", 0x0D: "storage: Delay Line (bit)",
    0x77: "storage: Auto Delay Line",
    0x32: "storage: Auto Register (size config)",
    0x5A: "storage: Static Value (constant)",
    0x5E: "storage: Static Evaluator (constant)",
    0x3B: "level map: Level Component",
    0x29: "level map: Level And Delay Component",
    # Sources and observers whose value is not computed on the board.
    0x6A: "source: level input (word)",
    0x3E: "source: switched level input",
    0x53: "sink: memory probe", 0x33: "sink: disassembling probe",
    0x59: "sink: 7 segment display", 0x57: "sink: halt",
    0x4D: "sink: level output counter", 0x35: "load port (unnamed twin)",
    0x2F: "word splitter (2n -> two n-bit halves)",
    0x75: "sink: Auto Static Indexer configuration block",
}

# Kinds the device library answers for, with the name the table uses.
GATE_KINDS = {0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
              0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18}

ROW = re.compile(r"^0x([0-9a-f]{2}) name=\"([^\"]*)\" gates=(\d+) delay=(\d+) inputs=(\d+) outputs=(\d+)")


def board_table(name: str) -> set[int]:
    """The kinds listed in one of board.hpp's static tables."""
    text = BOARD.read_text(encoding="utf-8")
    block = re.search(name + r"\(uint8_t kind\) \{(.*?)\n\}", text, re.S)
    kinds: set[int] = set()
    if block:
        for match in re.finditer(r"\{(0x[0-9A-Fa-f]{2}),", block.group(1)):
            kinds.add(int(match.group(1), 16))
    return kinds


def board_kind_test(name: str) -> set[int]:
    """The kinds an `inline bool <name>(uint8_t kind)` predicate accepts."""
    text = BOARD.read_text(encoding="utf-8")
    block = re.search(r"inline bool " + name + r"\(uint8_t kind\) \{(.*?)\n\}", text, re.S)
    kinds: set[int] = set()
    if block:
        for match in re.finditer(r"kind == (0x[0-9A-Fa-f]{2})", block.group(1)):
            kinds.add(int(match.group(1), 16))
    return kinds


# Why each still-unsupported kind is unsupported, and what would unblock it.
# Kept here rather than in prose so `--table` and the research note cannot drift
# apart: the note quotes this text.
BLOCKED: dict[int, str] = {
    0x52: "reads memory (bit form); value is not a wire",
    0x2F: "input pin width is an unmeasured sentinel (0x7fff...fe)",
    0x75: "auto-sized static index: emits no code, no board value",
    0x50: "no name, sentinel pin offsets: the game itself refuses to place it",
}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--table", action="store_true",
                        help="print the full inventory (status, kind, name, how it is handled)")
    args = parser.parse_args(argv)
    if not KINDS.exists():
        print(f"missing {KINDS}; run the kind-list playtest first", file=sys.stderr)
        return 2
    word_kinds = board_table("wordKindSpec")
    unary_kinds = board_table("wordUnarySpec")
    leaf_kinds = board_table("leafKindSpec")
    concat_kinds = board_kind_test("isConcatKind")
    indexer_kinds = board_kind_test("isIndexerKind")
    pin_kinds = board_kind_test("isPinKind")
    decoder_kinds = board_table("decoderKindSpec")
    supported, handled, inactive, missing = [], [], [], []
    rows, status, detail = [], {}, {}
    for line in KINDS.read_text(encoding="utf-8").splitlines():
        match = ROW.match(line.strip())
        if not match:
            continue
        kind = int(match.group(1), 16)
        name, gates, delay = match.group(2), int(match.group(3)), int(match.group(4))
        ins, outs = int(match.group(5)), int(match.group(6))
        if kind in HANDLED_IN_BOARD:
            handled.append((kind, name, ins, outs, HANDLED_IN_BOARD[kind]))
        elif kind in GATE_KINDS:
            supported.append((kind, name, ins, outs))
        elif (kind in word_kinds or kind in unary_kinds or kind in concat_kinds or kind in leaf_kinds
              or kind in indexer_kinds or kind in pin_kinds or kind in decoder_kinds):
            supported.append((kind, name, ins, outs))
        elif ins == 0 and outs == 0 and not name:
            inactive.append((kind, name, ins, outs))
        else:
            missing.append((kind, name, ins, outs))
    total = len(supported) + len(handled) + len(inactive) + len(missing)
    print(f"built-in prototypes: {total}")
    print(f"  device library (new word ops) : {len(supported) - len(GATE_KINDS)}")
    print(f"  device library (gate kinds)   : {len(GATE_KINDS)}")
    print(f"  hand-modelled in board.hpp    : {len(handled)}")
    print(f"  inactive slots (0 in / 0 out) : {len(inactive)}")
    print(f"  MISSING from simcore          : {len(missing)}")
    if args.table:
        print("\nfull inventory (status, kind, name, how it is handled):")
        for kind, name, ins, outs in sorted(rows):
            print(f"  {status[kind]:<26} 0x{kind:02x}  {name or chr(40)+chr(117)+chr(110)+chr(110)+chr(97)+chr(109)+chr(101)+chr(100)+chr(41):<24} {detail[kind]}")
        return 0
    print("\nmissing kinds:")
    for kind, name, ins, outs in sorted(missing):
        label = name or "(unnamed)"
        why = BLOCKED.get(kind, "no reason recorded - add one")
        print(f"  0x{kind:02x}  in={ins} out={outs}  {label}  --  {why}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
