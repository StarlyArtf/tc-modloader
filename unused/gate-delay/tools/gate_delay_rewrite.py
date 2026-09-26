#!/usr/bin/env python3
"""M1a: rewrite the game's generated simulation source for gate-level delay.

The game compiles a board into a small program (see
docs/PLAN-gate-delay.md section 4.1) whose cycle body is one block per
component, delimited by `// <node> <kind> <bits> [LATE] [id] [label]`, with all
state access going through `#SIMULATION_STATE + <offset>`.

This tool does the part that needs no engine change:

  * split the cycle body into blocks and classify them (LATE / side effect);
  * compute each block's **arrival unit** from the offsets it reads and writes
    (`arrival = max(arrival of producers) + delay(kind)`);
  * emit a unit loop with a readiness guard per block;
  * keep LATE and side-effect blocks outside the loop (they run once per cycle,
    which is what today's model means for state and for detection);
  * rename `#SIMULATION_STATE` to `tc_state` inside the loop, so the host can
    hand the body the buffer that is visible *this* unit.

`tc_state` and the per-unit bookkeeping come from host functions the loader
already knows how to inject into the game's compiler table (`tc_scope_tick` is
the existing precedent, see src/native_logic.hpp prepareCompiler): the plan
calls them `tc_delay_begin` / `tc_delay_ready` here, and M1b implements them.

Usage:
    python tools/gate_delay_rewrite.py <source.txt> [--k 8] [--out out.txt]
    python tools/gate_delay_rewrite.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


BLOCK_RE = re.compile(r"^\s*//\s*(\d+)\s+(com_[a-z0-9_]+)\s+(\d+)(.*)$")
READ_RE = re.compile(r"load\(<[^>]+>,\s*#SIMULATION_STATE\s*\+\s*(\d+)\)")
WRITE_RE = re.compile(r"store\(#SIMULATION_STATE\s*\+\s*(\d+)\s*,")
STATE_TOKEN = "#SIMULATION_STATE"

# A block whose code touches one of these is not safe to run once per unit: it
# reports short circuits, reads the wall clock, calls into the host or touches
# the level's input replay.  Those go to the cycle boundary instead.
SIDE_EFFECT_MARKERS = (
    "set_setting(sim_short_circuit",
    "get_time()",
    "tc_dynamic_constant",
    "tc_logic_",
    "input_replay",
    "output_history_pins",
    "ui_buffer",
)

# Delay units per kind, as the game's cost table reports them.  Only the entries
# that are not 1 need to be listed; `--kinds` can override at run time.
KIND_DELAY = {
    "com_delay_line_bit": 4,
    "com_delay_line_word": 4,
    "com_delay_line_word_config": 4,
    "com_register_bit": 1,
    "com_register_word": 1,
    "com_ram": 4,
    "com_static_indexer": 0,
    "com_static_indexer_config": 0,
    # Compiler artifacts: the emitter inserts these to break feedback while it
    # expands a custom component.  They carry a value through unchanged, so they
    # must not add a delay unit (otherwise every level of nesting would look
    # slower than the circuit it contains).
    "com_cc_input_buffer": 0,
    "com_cc_output": 0,
    "com_cc_input": 0,
    "com_cc_level_input": 0,
}


def kind_delay(kind: str) -> int:
    return KIND_DELAY.get(kind, 1)


@dataclass
class Block:
    index: int
    node: int
    kind: str
    bits: int
    tail: str
    lines: list[str] = field(default_factory=list)
    reads: list[int] = field(default_factory=list)
    writes: list[int] = field(default_factory=list)
    arrival: int = 0

    @property
    def late(self) -> bool:
        return " LATE " in f" {self.tail.strip()} "

    @property
    def side_effect(self) -> bool:
        text = "\n".join(self.lines)
        return any(marker in text for marker in SIDE_EFFECT_MARKERS)

    @property
    def delay(self) -> int:
        return kind_delay(self.kind)


@dataclass
class CycleBody:
    """One emitted cycle body: the lines between the loop start and `cycle += 1`."""

    start: int          # index of the first body line in the source
    end: int            # index one past the last body line
    blocks: list[Block]
    other: list[str]    # non-block lines (the `var`/`if` scaffolding the probe saw)


def find_line(lines: list[str], pattern: str, start: int = 0) -> int:
    rx = re.compile(pattern)
    for i in range(start, len(lines)):
        if rx.search(lines[i]):
            return i
    return -1


def extract_blocks(body: list[str], base_index: int) -> tuple[list[Block], list[str]]:
    blocks: list[Block] = []
    other: list[str] = []
    current: Block | None = None
    for offset, line in enumerate(body):
        match = BLOCK_RE.match(line)
        if match:
            if match.group(2) == "com_none":
                # Empty placeholder the emitter writes when a slot has no
                # component: it carries no code and must not get a phase guard.
                current = None
                continue
            current = Block(
                index=base_index + offset,
                node=int(match.group(1)),
                kind=match.group(2),
                bits=int(match.group(3)),
                tail=match.group(4),
            )
            blocks.append(current)
            continue
        if current is None:
            other.append(line)
        else:
            current.lines.append(line)
    for block in blocks:
        text = "\n".join(block.lines)
        block.reads = sorted({int(v) for v in READ_RE.findall(text)})
        block.writes = sorted({int(v) for v in WRITE_RE.findall(text)})
    return blocks, other


def find_cycle_bodies(lines: list[str]) -> list[CycleBody]:
    """Every region that runs once per cycle: the body of mode_refresh and the
    `while cycle < burst_target_cycle { ... }` region inside mode_run."""
    bodies: list[CycleBody] = []
    for name, start_pattern, end_pattern in (
        ("mode_refresh", r"^def mode_refresh\(\) None \{", None),
        ("mode_run", r"while cycle < burst_target_cycle \{", r"^\s*cycle \+= 1"),
    ):
        def_line = find_line(lines, start_pattern)
        if def_line < 0:
            continue
        if name == "mode_refresh":
            # The body ends at the matching closing brace of the function.
            depth = 0
            start = def_line
            end = len(lines)
            for i in range(def_line, len(lines)):
                depth += lines[i].count("{") - lines[i].count("}")
                if depth == 0 and i > def_line:
                    end = i
                    break
        else:
            start = def_line
            end = find_line(lines, end_pattern, start)
            if end < 0:
                end = len(lines)
        body_lines = lines[start + 1 : end]
        blocks, other = extract_blocks(body_lines, start + 1)
        if blocks:
            bodies.append(CycleBody(start=start + 1, end=end, blocks=blocks, other=other))
    return bodies


def compute_arrivals(blocks: list[Block], warnings: list[str],
                     arrivals: dict[int, int] | None = None) -> None:
    """arrival(block) = max(arrival(producer of every offset it reads)) + delay.

    LATE blocks are the cycle boundary, so they are not part of the wave front:
    they keep arrival 0 and are emitted outside the unit loop.

    Blocks that report a short circuit or read the clock are routed to the cycle
    boundary when the program is emitted, but they still *produce* wire values
    in the graph, so they take part in the arrival computation - leaving them
    out would break the chain for every block that reads their wires.

    `arrivals` (node id -> unit) is the table the *loader* fills at run time
    from the game's own preorder pass (the per-node arrival times the game
    already computes for the delay score, port entry +0x38).  Inferring them
    from the generated text is only a fallback: on a real board the two emitted
    bodies give different numbers, which is the sign that the fallback is
    lossy - see section 14.3 of the plan.
    """
    if arrivals:
        for block in blocks:
            if block.late:
                block.arrival = 0
            elif block.node in arrivals:
                block.arrival = arrivals[block.node]
        remaining = [b for b in blocks if not b.late and b.node not in arrivals]
        if not remaining:
            return
    else:
        remaining = [b for b in blocks if not b.late]
    producers: dict[int, Block] = {}
    for block in blocks:
        if block.late:
            continue
        for offset in block.writes:
            producers.setdefault(offset, block)
    pending = remaining
    resolved: dict[int, int] = {}
    for _ in range(len(pending) + 1):
        changed = False
        for block in pending:
            best = 0
            blocked = False
            for offset in block.reads:
                producer = producers.get(offset)
                if producer is None or producer is block:
                    continue
                if producer.node in resolved:
                    best = max(best, resolved[producer.node])
                else:
                    blocked = True
            if blocked and block.node not in resolved:
                continue
            value = best + block.delay
            if resolved.get(block.node) != value:
                resolved[block.node] = value
                changed = True
        if not changed:
            break
    for block in blocks:
        if block.late:
            block.arrival = 0
            continue
        if arrivals and block.node in arrivals:
            continue
        if block.node not in resolved:
            warnings.append(
                f"block {block.node} ({block.kind}) reads an offset no non-LATE block "
                "produces (feedback or a level input); treating it as arrival 0"
            )
            resolved[block.node] = 0
        block.arrival = resolved[block.node]


def emit_unit_loop(body: CycleBody, k: int, indent: str) -> list[str]:
    """The replacement for `body`: snapshot-driven unit loop + boundary blocks."""
    # A block that writes state stays in the wave even when it also reports
    # something (a short-circuit test drives a wire *and* sets a setting):
    # dropping it would break the wire.  Only pure reporting blocks move to the
    # boundary, together with the LATE blocks that commit state for the cycle.
    wave = [b for b in body.blocks if not b.late and (b.writes or not b.side_effect)]
    boundary = [b for b in body.blocks if b.late or (b.side_effect and not b.writes)]
    out: list[str] = []
    out.append(f"{indent}var tc_unit = 0")
    out.append(f"{indent}while tc_unit < {k} {{")
    out.append(
        f"{indent}    let tc_state = game_engine.'tc_delay_begin'(U64 tc_unit)"
        "  // host: publish this unit's visible state"
    )
    for block in sorted(wave, key=lambda b: (b.arrival, b.node)):
        out.append(
            f"{indent}    if game_engine.'tc_delay_ready'(U64 {block.arrival}) != 0 {{"
            f"  // {block.node} {block.kind} delay={block.delay}"
        )
        out.append(f"{indent}        // {block.node} {block.kind} {block.bits}{block.tail}")
        for line in block.lines:
            out.append(f"{indent}    {line.replace(STATE_TOKEN, 'tc_state')}")
        out.append(f"{indent}    }}")
    out.append(f"{indent}    tc_unit += 1")
    out.append(f"{indent}}}")
    for block in sorted(boundary, key=lambda b: b.node):
        marker = "LATE" if block.late else "side effect"
        out.append(f"{indent}// {block.node} {block.kind} {block.bits}{block.tail}  [{marker}]")
        out.extend(block.lines)
    return out


def rewrite(text: str, k: int, arrivals: dict[int, int] | None = None) -> tuple[str, dict]:
    lines = text.splitlines()
    report: dict = {"k": k, "bodies": [], "warnings": []}
    bodies = find_cycle_bodies(lines)
    if not bodies:
        raise SystemExit("no cycle body found: is this a generated simulation source?")
    for body in bodies:
        compute_arrivals(body.blocks, report["warnings"], arrivals)
        report["bodies"].append(
            {
                "blocks": len(body.blocks),
                "wave": sum(1 for b in body.blocks if not b.late and not b.side_effect),
                "late": sum(1 for b in body.blocks if b.late),
                "side_effect": sum(1 for b in body.blocks if b.side_effect and not b.late),
                "max_arrival": max((b.arrival for b in body.blocks), default=0),
                "delays": {b.kind: b.delay for b in body.blocks},
                "arrival_table": {b.node: b.arrival for b in body.blocks if not b.late},
            }
        )
    # Rewrite from the bottom up so earlier indices stay valid.
    for body in sorted(bodies, key=lambda b: b.start, reverse=True):
        indent = re.match(r"\s*", lines[body.start]).group(0)
        replacement = emit_unit_loop(body, k, indent)
        lines[body.start : body.end] = replacement
    return "\n".join(lines) + ("\n" if text.endswith("\n") else ""), report


@dataclass
class CutConnection:
    """One connection the sandbox compiled "open" and the rewrite must close.

    The board is compiled from a cut schematic (plan section 13.6), so the
    emitter folds the value the cut pin would have carried into a constant - both
    ways of cutting were measured: cutting the consumer side gives
    `~((U1 0x0) & (U1 0x0))`, cutting the producer side gives the same thing
    (section 15.1).  Closing it therefore means

      * consumer: replace that operand of the consumer's block with a load from
        the wire's state slot, and
      * producer: append a store of the producer's own result into that same
        slot,

    which needs no per-kind semantics beyond "which operand is which pin" - the
    loader knows the pins from the board record, so the runtime path passes the
    operand index in."""

    consumer_node: int
    operand: int
    offset: int
    bits: int
    producer_node: int


CONST_OPERAND_RE = re.compile(r"\((U\d+) 0x0\)")


def close_connections(text: str, connections: list[CutConnection]) -> tuple[str, dict]:
    """Replace the folded constants of cut pins with loads, and make the producer
    write the wire slot.  Text level, so it runs on the same source the other
    passes work on."""
    lines = text.splitlines()
    report: dict = {"closed": [], "problems": []}
    bodies = find_cycle_bodies(lines)
    for connection in connections:
        for body in bodies:
            for block in body.blocks:
                text_block = "\n".join(block.lines)
                if block.node == connection.consumer_node:
                    matches = list(CONST_OPERAND_RE.finditer(text_block))
                    if connection.operand >= len(matches):
                        report["problems"].append(
                            f"node {connection.consumer_node}: operand "
                            f"{connection.operand} not found (block has {len(matches)} constants)")
                        continue
                    match = matches[connection.operand]
                    replacement = (f"({match.group(1)} "
                                   f"(load(<U{connection.bits}>, tc_state + {connection.offset})))")
                    patched = text_block[: match.start()] + replacement + text_block[match.end() :]
                    block.lines = patched.split("\n")
                    report["closed"].append(
                        f"consumer {connection.consumer_node}: operand {connection.operand} "
                        f"-> load(<U{connection.bits}>, +{connection.offset})")
                if block.node == connection.producer_node:
                    last_let = None
                    for line in block.lines:
                        found = re.search(r"\b(let|var)\s+(\w+)\s*=", line)
                        if found:
                            last_let = found.group(2)
                    if not last_let:
                        report["problems"].append(
                            f"producer {connection.producer_node}: no result variable to store")
                        continue
                    block.lines.append(
                        f"store(tc_state + {connection.offset}, "
                        f"U{connection.bits} ({last_let}))  // re-closed by the gate-delay mode")
                    report["closed"].append(
                        f"producer {connection.producer_node}: writes +{connection.offset}")
    # Re-emit by line numbers, bottom up, so earlier indices stay valid.
    for body in sorted(bodies, key=lambda b: b.start, reverse=True):
        for block in sorted(body.blocks, key=lambda b: b.index, reverse=True):
            start = block.index + 1          # keep the `// <node> <kind>` comment
            end = start + len(block.lines)
            lines[start:end] = block.lines
    return "\n".join(lines) + ("\n" if text.endswith("\n") else ""), report


def self_test() -> int:
    """Invariants that must hold before this is allowed near the game."""
    sample = """def mode_refresh() None {

    // 1 com_nand_bit 2 3
    let value_id256 = ~((U1 0x0) & (U1 0x0))
    store(#SIMULATION_STATE + 256, U1 (value_id256))

}

def mode_run(target_cycle: Int) None {
    while target_cycle > cycle {
        while cycle < burst_target_cycle {

            // 1 com_nand_bit 2 3
            let value_id256 = ~((U1 0x0) & (U1 0x0))
            store(#SIMULATION_STATE + 256, U1 (value_id256))

            // 0 com_none 2 0
            cycle += 1
        }
    }
}
"""
    out, report = rewrite(sample, 4)
    failures = []
    if out.count("tc_delay_begin") != 2:
        failures.append("expected one unit loop per cycle body")
    if out.count("tc_delay_ready") != 2:
        failures.append("expected one readiness guard per wave block")
    if "store(tc_state + 256" not in out:
        failures.append("wave blocks must write through the unit's visible state")
    # A LATE block must never be emitted inside the loop, and it must keep the
    # real state token (it runs at the cycle boundary, not in a unit).
    late_sample = sample.replace("// 1 com_nand_bit 2 3", "// 1 com_register_word 2 LATE 3")
    late_out, late_report = rewrite(late_sample, 2)
    late_guard = late_out.split("tc_unit += 1")[0]
    if "com_register_word" in late_guard:
        failures.append("a LATE block leaked into the unit loop")
    if late_report["bodies"][0]["late"] != 1:
        failures.append("the LATE marker was not classified")
    if "#SIMULATION_STATE" not in late_out:
        failures.append("the boundary path must keep the real state token")
    # A side-effect block (short-circuit reporting) also belongs to the boundary.
    side_sample = sample.replace(
        "store(#SIMULATION_STATE + 256, U1 (value_id256))",
        "set_setting(sim_short_circuit_component_id_1, U64 7)",
    )
    side_out, side_report = rewrite(side_sample, 2)
    if "sim_short_circuit_component_id_1" in side_out.split("tc_unit += 1")[0]:
        failures.append("a side-effect block leaked into the unit loop")
    # The runtime path supplies the game's own arrival times; an explicit table
    # must win over the offset fallback.
    override_out, _ = rewrite(sample, 2, {1: 7})
    if "tc_delay_ready'(U64 7)" not in override_out:
        failures.append("an explicit arrival table was ignored")
    for failure in failures:
        print("FAIL:", failure)
    print("self-test:", "FAIL" if failures else "ok")
    return 1 if failures else 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", help="generated simulation source")
    parser.add_argument("--k", type=int, default=8, help="delay units per cycle")
    parser.add_argument("--out", help="write the rewritten source here")
    parser.add_argument("--arrivals",
                        help="node=unit table to use instead of the offset fallback "
                             "(what the runtime reads from the game's preorder pass)")
    parser.add_argument("--dump-arrivals", help="write the node=unit table that was used")
    parser.add_argument("--close", action="append", default=[],
                        help="close a cut connection: consumer:operand:offset:bits:producer "
                             "(repeatable; see CutConnection)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.source:
        parser.error("give a source file or --self-test")
    text = Path(args.source).read_text(encoding="utf-8", errors="replace")
    arrivals: dict[int, int] | None = None
    if args.arrivals:
        arrivals = {}
        for line in Path(args.arrivals).read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p for p in re.split(r"[=:\s]+", line) if p]
            if len(parts) < 2:
                continue
            arrivals[int(parts[0])] = int(parts[1])
    close_report = None
    if args.close:
        connections = []
        for spec in args.close:
            fields = [f for f in re.split(r"[:,\s]+", spec) if f]
            if len(fields) != 5:
                parser.error(f"--close wants consumer:operand:offset:bits:producer, got {spec!r}")
            connections.append(CutConnection(int(fields[0]), int(fields[1]), int(fields[2]),
                                             int(fields[3]), int(fields[4])))
        text, close_report = close_connections(text, connections)
    out, report = rewrite(text, args.k, arrivals)
    print(f"source: {args.source}")
    for i, body in enumerate(report["bodies"]):
        print(f"  body {i}: blocks={body['blocks']} wave={body['wave']} "
              f"late={body['late']} side_effect={body['side_effect']} "
              f"max_arrival={body['max_arrival']} K={report['k']}")
        print(f"    kinds: {body['delays']}")
    for warning in report["warnings"][:8]:
        print("  warning:", warning)
    if close_report:
        for line in close_report["closed"]:
            print("  closed:", line)
        for line in close_report["problems"]:
            print("  problem:", line)
    if args.dump_arrivals and report["bodies"]:
        table = report["bodies"][-1]["arrival_table"]
        lines = [f"# node=arrival units (K={report['k']})"] + [f"{n}={a}" for n, a in sorted(table.items())]
        Path(args.dump_arrivals).write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"wrote {args.dump_arrivals} ({len(table)} entries)")
    if args.out:
        Path(args.out).write_text(out, encoding="utf-8")
        print("wrote", args.out)
    else:
        print("--- rewritten cycle body (first 40 lines of the run loop) ---")
        start = out.find("while tc_unit")
        print("\n".join(out[start:].splitlines()[:40]) if start >= 0 else "no unit loop?!")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
