#!/usr/bin/env python3
"""Build board fixtures used by the standalone sandbox simulator.

The active S2 tests use the closed ring, cross-coupled latch, and host-clock
boards.  These fixtures deliberately keep their feedback connections intact:
the game may reject its own compile, while `dev.sandbox-sim` reads the board
records directly and advances them on its simulator-owned cycle.

The additional acyclic and mixed-component boards remain useful builder
fixtures, but no loader source rewrite or feedback-edge patch is involved.
"""

from __future__ import annotations

import argparse
import copy
import dataclasses
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import circuit_format as cf  # noqa: E402  (path set above)

NAND_KIND = 0x06
AND_KIND = 0x04
CONST_KIND = 0x2E
# The id examples/clock registers; a 0x4e instance carrying it is a clock.
CLOCK_ID = 0x434C4F4B5F303031
NAND_IN0 = (-1, -1)
NAND_IN1 = (-1, 1)
NAND_OUT = (2, 0)
CONST_OUT = (3, 0)

# A wire segment is one number: (direction << 14) | length, with 0 east, 1 south,
# 2 west, 3 north, and a trailing zero.  Verified against the boards section 13.1
# left behind: a self-loop built as "east 4, south 5, west 7, north 4" from (1,0)
# reads back as [4, 16389, 32775, 49156, 0], and the AND fixture's input wire is
# "east 7" [7, 0].
EAST, SOUTH, WEST, NORTH = 0, 1, 2, 3


def segment(direction: int, length: int) -> int:
    return (direction << 14) | length


def template(path: pathlib.Path, kind: int) -> cf.Component:
    circuit = cf.read_circuit(path)
    for component in circuit.components:
        if component.kind == kind:
            return component
    raise SystemExit(f"{path} carries no kind 0x{kind:02x} to copy from")


def make_component(base: cf.Component, x: int, y: int, identity: int,
                   value: int | None = None, bits: int | None = None) -> cf.Component:
    component = copy.deepcopy(base)
    component.x = x
    component.y = y
    component.identity = identity
    if value is not None:
        component.values = [value]
    if bits is not None:
        component.bits = bits
    return component


def straight(x: int, y: int, length: int, direction: int, color: int) -> cf.Wire:
    """A single-segment wire starting at (x,y)."""
    return cf.Wire(color=color, name="", x=x, y=y, segments=[segment(direction, length), 0])


def two_segment(x: int, y: int, first: int, first_length: int, second: int, second_length: int,
                color: int) -> cf.Wire:
    return cf.Wire(color=color, name="", x=x, y=y,
                   segments=[segment(first, first_length), segment(second, second_length), 0])


def polyline(x: int, y: int, steps: list[tuple[int, int]], color: int) -> cf.Wire:
    """A wire path of any length; `steps` are (direction, length) pairs."""
    return cf.Wire(color=color, name="", x=x, y=y,
                   segments=[segment(direction, length) for direction, length in steps] + [0])


def build_ring(skeleton: cf.Circuit, nand: cf.Component, constant: cf.Component,
               color: int) -> cf.Circuit:
    """A -> B -> C -> A with the C -> A wire cut."""
    circuit = copy.deepcopy(skeleton)
    circuit.components = [
        make_component(nand, -20, 0, 2001),
        make_component(nand, 0, 0, 2002),
        make_component(nand, 20, 0, 2003),
        # The placeholder: it drives A's cut input in the board that compiles.
        make_component(constant, -30, -1, 2004, value=1, bits=1),
        make_component(constant, -30, 1, 2005, value=1, bits=1),     # A's free input
        make_component(constant, -10, 1, 2006, value=1, bits=1),     # B's free input
        make_component(constant, 10, 1, 2007, value=1, bits=1),      # C's free input
    ]
    circuit.wires = [
        # placeholder -> A in0 (the connection the re-close replaces)
        straight(-27, -1, 6, EAST, color),
        # constant -> A in1
        straight(-27, 1, 6, EAST, color),
        # A out -> B in0
        two_segment(-18, 0, EAST, 17, NORTH, 1, color),
        # constant -> B in1
        straight(-7, 1, 6, EAST, color),
        # B out -> C in0
        two_segment(2, 0, EAST, 17, NORTH, 1, color),
        # constant -> C in1
        straight(13, 1, 6, EAST, color),
        # C's output is deliberately left unwired: that is the cut.
    ]
    return circuit


def build_latch(skeleton: cf.Circuit, nand: cf.Component, constant: cf.Component,
                color: int) -> cf.Circuit:
    """A <- B, A -> B, with the B -> A wire cut.

    A's free input is held **low**: a NAND with a 0 input is forced high, which is
    the active-low reset of a NAND latch.  With the loop closed the pair settles
    at A = 1, B = 0 and stays there - the contrast with the ring, which never
    settles."""
    circuit = copy.deepcopy(skeleton)
    circuit.components = [
        make_component(nand, -10, 0, 2101),                          # A
        make_component(nand, 10, 0, 2102),                           # B
        make_component(constant, -20, -1, 2103, value=1, bits=1),    # placeholder for B -> A
        make_component(constant, -20, 1, 2104, value=0, bits=1),     # active-low reset of A
        make_component(constant, 20, 1, 2105, value=1, bits=1),      # set input of B
    ]
    circuit.wires = [
        straight(-17, -1, 6, EAST, color),                           # placeholder -> A in0
        straight(-17, 1, 6, EAST, color),                            # constant -> A in1
        two_segment(-8, 0, EAST, 17, NORTH, 1, color),               # A out -> B in0
        straight(23, 1, 9, WEST, color),                             # constant -> B in1
    ]
    # B's output is deliberately left unwired: that is the cut.
    return circuit


def build_ring_closed(skeleton: cf.Circuit, nand: cf.Component, constant: cf.Component,
                      color: int) -> cf.Circuit:
    """The ring as a player would draw it: three NANDs wired A -> B -> C -> A, with
    the loop closed.  Nothing in this board is cut and there is no placeholder, so
    it is exactly the board the game refuses to compile (circular dependency).
    The loader is what has to make it compilable, by cutting one of the loop's
    wires before the graph is built and telling the transform to put it back."""
    circuit = copy.deepcopy(skeleton)
    circuit.components = [
        make_component(nand, -20, 0, 2001),                          # A
        make_component(nand, 0, 0, 2002),                            # B
        make_component(nand, 20, 0, 2003),                           # C
        make_component(constant, -30, 1, 2005, value=1, bits=1),     # A's free input
        make_component(constant, -10, 1, 2006, value=1, bits=1),     # B's free input
        make_component(constant, 10, 1, 2007, value=1, bits=1),      # C's free input
    ]
    circuit.wires = [
        # C out (22,0) -> A in0 (-21,-1): the wire that closes the loop.
        two_segment(22, 0, WEST, 43, NORTH, 1, color),
        straight(-27, 1, 6, EAST, color),
        two_segment(-18, 0, EAST, 17, NORTH, 1, color),
        straight(-7, 1, 6, EAST, color),
        two_segment(2, 0, EAST, 17, NORTH, 1, color),
        straight(13, 1, 6, EAST, color),
    ]
    return circuit


def build_two_nand(skeleton: cf.Circuit, nand: cf.Component, gate: cf.Component,
                   constant: cf.Component, color: int) -> cf.Circuit:
    """A closed two-NAND latch with unrelated parts on the same board.

    This is the board a player has while building a set/reset latch in the
    sandbox: two cross-coupled NANDs, the rest of the board already carrying
    other kinds, and **both** feedback wires drawn.  The graph the compiler
    walks is therefore cyclic and it refuses to emit anything, which is what
    made the game spin - so the loader has to detach one of the two feedback
    wires and the transform re-close it through the delay model.

    The unrelated parts sit on the board but on no path between the two NANDs:
    an old "any kind we do not know turns automatic cutting off" rule skipped
    the whole board because of them, which is exactly the regression this board
    is here to catch.

    Geometry (a NAND's inputs are at (-1,-1)/(-1,1), its output at (2,0)):

        A at (-3,12)   in0 (-4,11)  in1 (-4,13)  out (-1,12)
        B at (-3,19)   in0 (-4,18)  in1 (-4,20)  out (-1,19)
    """
    circuit = copy.deepcopy(skeleton)
    circuit.components = [
        make_component(nand, -3, 12, 2001),                          # A
        make_component(nand, -3, 19, 2002),                          # B
        make_component(constant, -10, 13, 2003, value=0, bits=1),    # A's active-low reset
        make_component(constant, -10, 20, 2004, value=1, bits=1),    # B's set input
        # Unrelated parts in the same board, the way a sandbox accumulates them.
        make_component(gate, -12, 25, 2005),
        make_component(gate, -9, 26, 2006),
    ]
    circuit.wires = [
        # A out -> B in0: west 4 to the lane at x=-5, down 6, one east onto the pin.
        polyline(-1, 12, [(WEST, 4), (SOUTH, 6), (EAST, 1)], color),
        # B out -> A in0: the lane at x=-9 keeps clear of the reset constant's wire.
        polyline(-1, 19, [(WEST, 8), (NORTH, 8), (EAST, 5)], color),
        # reset constant -> A in1
        straight(-7, 13, 3, EAST, color),
        # set constant -> B in1
        straight(-7, 20, 3, EAST, color),
    ]
    return circuit


def build_loop_through_and(skeleton: cf.Circuit, nand: cf.Component, gate: cf.Component,
                           color: int) -> cf.Circuit:
    """A loop the loader cannot cut: it runs through an AND.

    Automatic cutting only knows NAND and constant pins, so this board's cycle
    is invisible to it and the game's own cycle search is what runs.  That is
    the shape that used to kill the process: the search is one of the functions
    the loader hooks, and a hook that does not hand every argument through makes
    the game dereference whatever the detour left behind (plan section 27).

        NAND1 (0,0) -> NAND2 (16,0) -> AND (8,4) -> NAND1
    """
    circuit = copy.deepcopy(skeleton)
    circuit.components = [
        make_component(nand, 0, 0, 2201),
        make_component(nand, 16, 0, 2202),
        make_component(gate, 8, 4, 2203),
    ]
    circuit.wires = [
        # NAND1 out (2,0) -> NAND2 in1 (15,1): down to the y=6 lane, across, up.
        polyline(2, 0, [(SOUTH, 6), (EAST, 13), (NORTH, 5)], color),
        # AND out (10,4) -> NAND1 in0 (-1,-1): up to the y=-3 lane, then west.
        polyline(10, 4, [(NORTH, 6), (WEST, 11), (SOUTH, 1)], color),
        # NAND2 out (18,0) -> AND in0 (7,3): west along y=0, then down.
        polyline(18, 0, [(WEST, 11), (SOUTH, 3)], color),
    ]
    return circuit


def build_user_board(skeleton: cf.Circuit, nand: cf.Component, constant: cf.Component,
                     custom: cf.Component, color: int) -> cf.Circuit:
    """The board the player had while building the latch: the cross-coupled pair
    plus the rest of that sandbox, which carried custom component instances.

    The custom parts matter to the game: its cycle search asks the board whether
    the node it is walking is a custom prototype, and that question is the one a
    hook with a truncated argument list used to answer with garbage (plan
    section 27).  The instances are not wired into the latch - neither were the
    player's.
    """
    circuit = copy.deepcopy(skeleton)
    circuit.components = [
        make_component(nand, -3, 12, 2001),
        make_component(nand, -3, 19, 2002),
        make_component(constant, -10, 13, 2003, value=0, bits=1),
        make_component(constant, -10, 20, 2004, value=1, bits=1),
        make_component(custom, -6, 21, 3001),
        make_component(custom, -3, 22, 3002),
        make_component(custom, -6, 24, 3003),
    ]
    circuit.wires = [
        polyline(-1, 12, [(WEST, 4), (SOUTH, 6), (EAST, 1)], color),   # A out -> B in0
        polyline(-1, 19, [(WEST, 8), (NORTH, 8), (EAST, 5)], color),   # B out -> A in0
        straight(-7, 13, 3, EAST, color),                              # reset -> A in1
        straight(-7, 20, 3, EAST, color),                              # set -> B in1
    ]
    return circuit


def make_gate(base: cf.Component, kind: int, x: int, y: int, identity: int) -> cf.Component:
    """The same prototype record with another gate kind.

    A two-input one-output 1-bit gate has one record shape for every kind (the
    game's own table gives all of them the same pins), so a NAND record with its
    kind replaced is a valid NOR/OR/XOR/AND.
    """
    component = copy.deepcopy(base)
    component.kind = kind
    component.x = x
    component.y = y
    component.identity = identity
    return component


def build_two_gate_latch(skeleton: cf.Circuit, gate_base: cf.Component, kind: int,
                         constant: cf.Component, color: int) -> cf.Circuit:
    """The closed two-NAND latch with **every gate replaced by another kind**.

    Automatic cutting knows the pin geometry of the game's combinational
    prototypes, so this board has to be cut exactly like the NAND one; replacing
    NOR with OR/XOR/AND must not bring the circular-dependency message back.
    """
    circuit = copy.deepcopy(skeleton)
    circuit.components = [
        make_gate(gate_base, kind, -3, 12, 2301),
        make_gate(gate_base, kind, -3, 19, 2302),
        make_component(constant, -10, 13, 2303, value=0, bits=1),
        make_component(constant, -10, 20, 2304, value=1, bits=1),
    ]
    circuit.wires = [
        polyline(-1, 12, [(WEST, 4), (SOUTH, 6), (EAST, 1)], color),   # A out -> B in0
        polyline(-1, 19, [(WEST, 8), (NORTH, 8), (EAST, 5)], color),   # B out -> A in0
        straight(-7, 13, 3, EAST, color),                              # constant -> A in1
        straight(-7, 20, 3, EAST, color),                              # constant -> B in1
    ]
    return circuit


def build_loop_through_custom(skeleton: cf.Circuit, nand: cf.Component,
                              custom: cf.Component, color: int) -> cf.Circuit:
    """A loop the loader still cannot cut: it runs through a custom component.

    A custom instance is deliberately absent from the pin table - its internals
    are unknown, so nothing says whether it is combinational - and that makes
    this board the one that still reaches the game's own cycle search.  It is the
    regression board for the hook that used to drop arguments (plan section 27):
    the run must end with the game's answer, not with a crash.

        NAND (0,10) -> custom AND2 (12,10) -> NAND
    """
    circuit = copy.deepcopy(skeleton)
    circuit.components = [
        make_component(nand, 0, 10, 2401),
        make_component(custom, 12, 10, 2402),
    ]
    circuit.wires = [
        # NAND out (2,10) -> custom in0 (11,9), approaching from the left so the
        # path does not cross the instance's other input pin.
        polyline(2, 10, [(SOUTH, 2), (EAST, 8), (NORTH, 3), (EAST, 1)], color),
        # custom out (14,9) -> NAND in0 (-1,9), through the lane at y=14 and x=-2.
        polyline(14, 9, [(EAST, 1), (SOUTH, 5), (WEST, 17), (NORTH, 5), (EAST, 1)], color),
    ]
    return circuit


def build_edge_detector(skeleton: cf.Circuit, custom: cf.Component, nand: cf.Component,
                        gate: cf.Component, clock_id: int, color: int) -> cf.Circuit:
    """Clock -> NOT -> AND, with the AND's other input on the clock itself.

    The classic edge detector: the NOT is one unit behind, so both inputs of the
    AND are high for the units right after the clock rises.  If the AND's state
    never moves on this board, the model is broken, not the display.

        clock (0,0) out (2,0) ----> AND (16,0) in1 (15,1)   (the direct input)
        clock            (2,0) -> NOT (6,0) -> AND in0 (15,-1)  (delayed input)
    """
    circuit = copy.deepcopy(skeleton)
    clock = make_component(custom, 0, 0, 2601)
    clock.custom_data = clock_id
    circuit.components = [
        clock,
        make_gate(nand, 0x03, 6, 0, 2602),          # NOT, one unit behind
        make_gate(gate, 0x04, 16, 0, 2603),         # AND (two units now)
    ]
    circuit.wires = [
        # clock out (2,0) -> NOT in (5,0).  Without this the NOT's input dangles
        # and the whole fixture is a no-op (found 2026-09-25: the cell (5,0) was
        # on no wire at all, so the NOT never left 0).
        straight(2, 0, 3, EAST, color),
        # NOT out (8,0) -> AND in0 (15,-1), by way of the lane above.
        polyline(8, 0, [(NORTH, 2), (EAST, 7), (SOUTH, 1)], color),
        # clock out (2,0) -> AND in1 (15,1), by way of the lane below.
        polyline(2, 0, [(SOUTH, 2), (EAST, 13), (NORTH, 1)], color),
        # AND out (18,0) -> two cells further east, so the pulse has a net the
        # report and the trace can show.
        straight(18, 0, 2, EAST, color),
    ]
    return circuit


def build_clock_board(skeleton: cf.Circuit, custom: cf.Component, nand: cf.Component,
                      clock_id: int, color: int) -> cf.Circuit:
    """A board that carries the Mod's clock source and one gate after it.

    The clock is a custom instance whose prototype id is the one examples/clock
    registers; the delay model marks its value slot and answers reads of it with
    a one-unit pulse, so the NOT's published value is the pulse inverted one unit
    later - which is what the per-unit trace is read for.

        clock (0,0) --out (2,0)--> NOT (6,0) in (-1,0) -> (5,0)
    """
    circuit = copy.deepcopy(skeleton)
    clock = make_component(custom, 0, 0, 2501)
    clock.custom_data = clock_id
    circuit.components = [
        clock,
        make_gate(nand, 0x03, 6, 0, 2502),          # NOT 1
        make_gate(nand, 0x03, 12, 0, 2503),         # NOT 2, fed by NOT 1
    ]
    circuit.wires = [
        straight(2, 0, 3, EAST, color),
        straight(8, 0, 3, EAST, color),
    ]
    return circuit


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skeleton", default="build/cycle_ctrl_board.data",
                        help="a board whose header and palette are reused")
    parser.add_argument("--template", default="build/cycle_min_board.data",
                        help="a board carrying the NAND shape and wire colour to copy")
    parser.add_argument("--constant-template", default="",
                        help="a board carrying a Constant (0x2e) to copy; defaults to --template")
    parser.add_argument("--and-template", default="build/and2_component.data",
                        help="a board carrying an AND (0x04) to copy for the two-NAND board")
    parser.add_argument("--custom-template", default="build/and2_solution.data",
                        help="a board carrying a custom component instance (0x4e) to copy")
    parser.add_argument("--out", default="build")
    parser.add_argument("--indent", type=int, default=3)
    args = parser.parse_args()

    skeleton = cf.read_circuit(pathlib.Path(args.skeleton))
    nand = template(pathlib.Path(args.template), NAND_KIND)
    constant = template(pathlib.Path(args.constant_template or args.template), CONST_KIND)
    gate = template(pathlib.Path(args.and_template), AND_KIND)
    custom = template(pathlib.Path(args.custom_template), 0x4E)
    color = skeleton.wires[0].color if skeleton.wires else 0
    out = pathlib.Path(args.out)

    ring = build_ring(skeleton, nand, constant, color)
    cf.write_circuit(out / "sandbox_sim_ring_board.data", ring)
    latch = build_latch(skeleton, nand, constant, color)
    cf.write_circuit(out / "sandbox_sim_latch_board.data", latch)
    closed = build_ring_closed(skeleton, nand, constant, color)
    cf.write_circuit(out / "sandbox_sim_ring_closed_board.data", closed)
    two_nand = build_two_nand(skeleton, nand, gate, constant, color)
    cf.write_circuit(out / "sandbox_sim_two_nand_board.data", two_nand)
    loop = build_loop_through_and(skeleton, nand, gate, color)
    cf.write_circuit(out / "sandbox_sim_loop_through_and_board.data", loop)
    user_board = build_user_board(skeleton, nand, constant, custom, color)
    cf.write_circuit(out / "sandbox_sim_user_board.data", user_board)
    custom_loop = build_loop_through_custom(skeleton, nand, custom, color)
    cf.write_circuit(out / "sandbox_sim_loop_through_custom_board.data", custom_loop)
    for name, kind in (("nor", 0x09), ("or", 0x07), ("xor", 0x0a), ("and", 0x04)):
        latch = build_two_gate_latch(skeleton, nand, kind, constant, color)
        cf.write_circuit(out / ("sandbox_sim_%s_latch_board.data" % name), latch)
    clock_board = build_clock_board(skeleton, custom, nand, CLOCK_ID, color)
    cf.write_circuit(out / "sandbox_sim_clock_board.data", clock_board)
    edge = build_edge_detector(skeleton, custom, nand, gate, CLOCK_ID, color)
    cf.write_circuit(out / "sandbox_sim_edge_detector_board.data", edge)

    for name, circuit in (("ring", ring), ("latch", latch), ("ring-closed", closed),
                          ("two-nand", two_nand), ("loop-through-and", loop),
                          ("user-board", user_board), ("loop-through-custom", custom_loop),
                          ("clock", clock_board)):
        print(f"==== {name} ====")
        print(cf.summarize(circuit))
    print("standalone simulator fixtures written; no loader-side re-close records")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
