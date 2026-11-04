#!/usr/bin/env python3
"""Build a level circuit that puts a built-in kind between a level input and
Output pins, so the game itself can be asked what that kind does.

The sandbox's simulator has to agree with the game about the kinds whose value
does not come from the wires, and the safest way to ask is to let the game load
and run a circuit we wrote.  This tool writes that circuit.

It starts from a **template**: one of the game's own solutions, so the component
records are the game's own records rather than something reconstructed by hand.
The default template is `campaign/overture_decoder/hint_solution.data`, whose
shape is

    Input -> Splitter -> 2 Bit Decoder -> four Outputs        (+ a Maker)

Usage:
  # what the template holds
  python tools/level-circuit.py --show campaign/overture_decoder/hint_solution.data

  # decoder: keep the Input (name it), keep the four Outputs, drop the rest
  python tools/level-circuit.py campaign/overture_decoder/hint_solution.data \
      --keep 0x6a,0x11,0x2c,0x3a --write build/decoder-level/circuit.data --outdir build/decoder-level
"""
from __future__ import annotations

import argparse
import copy
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import circuit_format as cf  # noqa: E402  (path set above)


def show(path: pathlib.Path) -> int:
    circuit = cf.read_circuit(path)
    print(f"{path}: version={circuit.version} components={len(circuit.components)} wires={len(circuit.wires)}")
    for index, component in enumerate(circuit.components):
        name = component.name or ""
        print(f"  c{index} kind=0x{component.kind:02x} at({component.x},{component.y}) bits={component.bits} "
              f"values={component.values[:2]} name={name!r}")
    for index, wire in enumerate(circuit.wires):
        print(f"  w{index} from({wire.x},{wire.y}) segments={len(wire.segments)}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("template", type=pathlib.Path)
    parser.add_argument("--show", action="store_true", help="print the template and stop")
    parser.add_argument("--rename-input", default=None,
                        help="give the level's own Input component this name (it is what the "
                             "level editor shows and what the ISA reads)")
    parser.add_argument("--name-outputs", default=None,
                        help="comma-separated names for the level's Output components, given "
                             "top to bottom.  The level's test reaches its outputs BY NAME "
                             "(output.<name>), so a circuit whose outputs are unnamed cannot be "
                             "driven by a test at all.")
    parser.add_argument("--input-bits", type=int, default=None,
                        help="also set the Input component's declared width")
    parser.add_argument("--keep", default=None,
                        help="comma-separated kinds to keep (default: keep the circuit exactly as "
                             "the game drew it, which is the safest thing to do)")
    parser.add_argument("--write", type=pathlib.Path, default=None, help="where to write circuit.data")
    parser.add_argument("--outdir", type=pathlib.Path, default=None, help="also write a level folder")
    parser.add_argument("--name", default="sandbox", help="the level name the folder stands for")
    args = parser.parse_args(argv)

    circuit = cf.read_circuit(args.template)
    if not args.keep and not args.write and not args.outdir:
        return show(args.template)

    trimmed = copy.deepcopy(circuit)
    if args.rename_input:
        touched = 0
        for component in trimmed.components:
            if component.kind in (0x6A, 0x3D):
                component.name = args.rename_input
                if args.input_bits:
                    component.bits = args.input_bits
                touched += 1
        print(f"renamed {touched} level input component(s) to {args.rename_input!r}")

    if args.name_outputs:
        names = [text.strip() for text in args.name_outputs.split(",") if text.strip()]
        outputs = sorted((component for component in trimmed.components if component.kind in (0x3A, 0x44, 0x45, 0x28)),
                         key=lambda component: component.y)
        if len(names) != len(outputs):
            print(f"note: {len(outputs)} output component(s) but {len(names)} name(s) given",
                  file=sys.stderr)
        for component, name in zip(outputs, names):
            component.name = name
        print("named outputs top to bottom: " + ", ".join(component.name for component in outputs))

    if args.keep:
        keep = {int(text, 16) for text in args.keep.split(",")}
        kept = [component for component in circuit.components if component.kind in keep]
        trimmed.components = kept
        # Keeping a subset means keeping only the wires whose BOTH ends stay on a
        # kept pin cell.  The game's own solutions share bus wires between several
        # components, so this drops more than one might expect - which is why the
        # safer default is to keep the circuit exactly as the game drew it.
        import importlib.util
        spec = importlib.util.spec_from_file_location("netlist_view", HERE / "netlist-view.py")
        netlist_view = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(netlist_view)
        cells: set[tuple[int, int]] = set()
        for component in kept:
            table = netlist_view.KINDS.get(component.kind)
            if not table:
                continue
            for dx, dy in table["in"] + table["out"]:
                cells.add((component.x + dx, component.y + dy))
        survivors = [wire for wire in circuit.wires
                     if all(end in cells for end in netlist_view.wire_ends(wire))]
        trimmed.wires = survivors
        print(f"kept {len(trimmed.components)} component(s), {len(survivors)} wire(s); "
              f"dropped {len(circuit.wires) - len(survivors)} wire(s)")

    if args.write:
        args.write.parent.mkdir(parents=True, exist_ok=True)
        args.write.write_bytes(cf.encode_circuit(trimmed))
        print(f"wrote {args.write} ({args.write.stat().st_size} bytes)")
    if args.outdir:
        args.outdir.mkdir(parents=True, exist_ok=True)
        (args.outdir / "circuit.data").write_bytes(cf.encode_circuit(trimmed))
        # A level folder also needs meta.txt and label.txt, which the campaign's
        # own sandbox folder supplies.
        source = HERE.parent.parent / "campaign" / args.name
        if source.exists():
            for extra in ("meta.txt", "label.txt"):
                if (source / extra).exists():
                    (args.outdir / extra).write_bytes((source / extra).read_bytes())
            print(f"copied {args.name}'s meta.txt/label.txt into {args.outdir}")
        else:
            print(f"note: no campaign/{args.name} to take meta.txt/label.txt from", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
