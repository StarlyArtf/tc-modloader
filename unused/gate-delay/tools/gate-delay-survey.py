"""Survey a generated simulation program for the gate-delay rewrite.

Read-only analysis of a `native-logic-source-*.txt` dump: for both cycle bodies
(`mode_refresh`, the `while cycle < burst_target_cycle` region of `mode_run`) it
lists the blocks the emitter marked with `// <node> <kind> <bits> [LATE] [id]`
and answers the questions the rewrite depends on:

  * does a block read its inputs from state slots (`load(<T>, #SIMULATION_STATE + N)`)
    or from the body's local wave variables (`vidN` / `value_idN`)?
  * which state slots does a block write, and does the slot number equal the
    node number used by the local variable name?
  * which blocks carry side effects (`set_setting(sim_short_circuit_*)`,
    `get_time()`, host calls, `input_replay`, `halt()`)?

Usage: python tools/gate-delay-survey.py <dump.txt> [--node N]
"""
import argparse
import re

BLOCK = re.compile(r"//\s*(\d+)\s+(com_\w+)\s+(\d+)\s*(.*)$")
LOAD = re.compile(r"load\(<U(\d+)>,\s*#SIMULATION_STATE\s*\+\s*(\d+)\)")
# The emitter writes both typed stores (`store(ptr + N, U32 (expr))`) and the
# register's untyped output cache (`store(ptr + N, value_94)`), so the width is
# optional here.
STORE = re.compile(r"store\(#SIMULATION_STATE\s*\+\s*(\d+),\s*(?:U(\d+))?")
VID = re.compile(r"\b(?:vid|value_id)(\d+)\b")
SIDE = ("set_setting(sim_short_circuit", "get_time()", "game_engine.'tc_",
        "input_replay", "output_history_pins", "halt()")


def bodies(lines):
    """Yields (name, first, last) for the two cycle bodies."""
    for i, line in enumerate(lines):
        if line.strip() == "def mode_refresh() None {":
            depth = 0
            for j in range(i, len(lines)):
                depth += lines[j].count("{") - lines[j].count("}")
                if depth == 0 and j > i:
                    yield "mode_refresh", i + 1, j
                    break
        if line.strip().startswith("while cycle < burst_target_cycle {"):
            for j in range(i + 1, len(lines)):
                if lines[j].strip().startswith("cycle += 1"):
                    yield "mode_run", i + 1, j
                    break


def blocks(lines, start, end):
    """Yields (header, node, kind, bits, tail, body_lines)."""
    heads = []
    for i in range(start, end):
        m = BLOCK.match(lines[i].strip())
        if m:
            heads.append((i, m))
    for index, (i, m) in enumerate(heads):
        stop = heads[index + 1][0] if index + 1 < len(heads) else end
        yield m, lines[i + 1:stop]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dump")
    parser.add_argument("--node", type=int, default=None,
                        help="print the full text of this node's blocks")
    args = parser.parse_args()

    with open(args.dump, "r", encoding="utf-8", errors="replace") as handle:
        lines = [line.rstrip("\n") for line in handle]

    for name, start, end in bodies(lines):
        print(f"==== {name} (lines {start}..{end}) ====")
        wave = late = side = 0
        slot_by_node = {}
        for m, body in blocks(lines, start, end):
            node, kind, bits, tail = int(m.group(1)), m.group(2), int(m.group(3)), m.group(4)
            if kind == "com_none":
                continue
            text = "\n".join(body)
            reads = {int(o) for _, o in LOAD.findall(text)}
            writes = {int(o): (int(w) if w else 0) for o, w in STORE.findall(text)}
            local = {int(v) for v in VID.findall(text)}
            markers = [s for s in SIDE if s in text]
            is_late = " LATE " in f" {tail} "
            if is_late:
                late += 1
            elif markers and not writes:
                side += 1
            else:
                wave += 1
            slot_by_node[node] = sorted(writes)
            local.discard(node)
            print(f"  node {node:4d} {kind:22s} bits={bits:2d} "
                  f"{'LATE' if is_late else '    '} "
                  f"store={sorted(writes.items())} read_slots={sorted(reads)} "
                  f"local_vars={sorted(local)[:8]} side={markers}")
        print(f"  -- wave={wave} late={late} side={side}")
        mismatched = {n: s for n, s in slot_by_node.items()
                      if s and n not in s}
        print(f"  -- nodes whose slot list does not start with their own number: {mismatched}")

    if args.node is not None:
        for name, start, end in bodies(lines):
            for m, body in blocks(lines, start, end):
                if int(m.group(1)) == args.node:
                    print(f"==== {name} node {args.node} ====")
                    print(m.group(0).strip())
                    print("\n".join(line for line in body if line.strip()))


if __name__ == "__main__":
    main()
