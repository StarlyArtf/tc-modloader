/* Offline checks for the viewer's own layer.

   simcore/tests already decides whether the simulator is right.  What is new
   here is everything between a board record and a pixel: the cell-to-pixel
   mapping, which pins exist where, which component or wire a click lands on,
   and whether a net can be followed to the state bytes it is written back to.
   That last one is the reason the viewer exists, so it gets an assertion
   rather than a hopeful look at a screenshot. */

#include "../board_file.hpp"
#include "../kind_pins.hpp"
#include "../scene.hpp"
#include "../svg_writer.hpp"

#include "../../include/tcsim/engine.hpp"

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::printf("FAIL %s\n", what.c_str());
}

struct Loaded {
    simcore_view::BoardFile file;
    tcsim::BuildResult built;
    tcsim::NetList values;
    simcore_view::Scene scene;
    bool ran = false;
};

/* Paths are relative to the repository root, which is where the script runs
   the binary from. */
Loaded load(const std::string& path, tcsim::Tick ticks = 200, uint64_t pattern = 0) {
    Loaded loaded;
    loaded.file = simcore_view::readBoardFile(path);
    if (!loaded.file.ok) {
        std::printf("SKIP %s: %s\n", path.c_str(), loaded.file.error.c_str());
        return loaded;
    }
    tcsim::BuildOptions build_options;
    build_options.gate_delay = tcsim::ArcDelay{2, 2};
    /* The flattened view, exactly as the window builds it: a record carrying
       custom definitions simulates the definition, not a refusal. */
    loaded.built = tcsim::buildNetList(simcore_view::netlistView(loaded.file), &simcore_view::pinsForKind,
                                       build_options);
    if (!loaded.built.fatal()) {
        tcsim::Engine engine(loaded.built.netlist, tcsim::EngineOptions{});
        for (size_t index = 0; index < loaded.built.source_nets.size(); ++index) {
            const tcsim::NetId net = loaded.built.source_nets[index];
            if (net == tcsim::kNoNet || net >= engine.netlist().netCount()) continue;
            const uint16_t bits = engine.netlist().net(net).bits;
            const tcsim::Logic logic =
                (pattern >> (index % 64)) & 1u ? tcsim::Logic::kOne : tcsim::Logic::kZero;
            engine.addStimulus(0, net, tcsim::BitVector(bits ? bits : 1, logic));
        }
        engine.runUntil(0);
        if (loaded.built.source_nets.empty() && engine.netlist().netCount() > 0) {
            engine.seedNet(0, tcsim::BitVector(1, tcsim::Logic::kZero));
        }
        engine.runUntil(ticks);
        loaded.values = engine.netlist();
        loaded.ran = true;
    }
    simcore_view::SceneInput input;
    input.board = &loaded.file.board;
    input.built = &loaded.built;
    input.values = loaded.ran ? &loaded.values : nullptr;
    loaded.scene = simcore_view::buildScene(input, simcore_view::SceneOptions{});
    return loaded;
}

const char* kData = "simcore/view/data/";

/* The board's own record (kind 0) is not a component and must not be drawn. */
size_t drawableComponents(const tcsim::BoardView& board) {
    size_t count = 0;
    for (const tcsim::BoardComponent& component : board.components) {
        if (component.kind != 0x00) ++count;
    }
    return count;
}

float lineLength(const simcore_view::SceneLine& line) {
    return std::fabs(line.x2 - line.x1) + std::fabs(line.y2 - line.y1);
}

/* Two segments that run along the same track and share at least a point. */
bool collinearOverlap(const simcore_view::SceneLine& a, const simcore_view::SceneLine& b) {
    const bool a_horizontal = a.y1 == a.y2;
    const bool b_horizontal = b.y1 == b.y2;
    if (a_horizontal != b_horizontal) return false;
    if (a_horizontal) {
        if (a.y1 != b.y1) return false;
        return std::min(a.x1, a.x2) < std::max(b.x1, b.x2) && std::min(b.x1, b.x2) < std::max(a.x1, a.x2);
    }
    if (a.x1 != b.x1) return false;
    return std::min(a.y1, a.y2) < std::max(b.y1, b.y2) && std::min(b.y1, b.y2) < std::max(a.y1, a.y2);
}

std::vector<simcore_view::SceneLine> drawnSegments(const simcore_view::Scene& scene) {
    std::vector<simcore_view::SceneLine> drawn;
    for (const simcore_view::SceneLine& line : scene.lines) {
        if (!line.halo) drawn.push_back(line);
    }
    return drawn;
}

void testAndGate() {
    const Loaded loaded = load(std::string(kData) + "and_gate.txt");
    if (!loaded.file.ok) return;

    check(!loaded.built.fatal(), "the and gate board builds without a fatal note");
    check(loaded.built.notes.empty(), "the and gate board has nothing to report");
    check(loaded.scene.rects.size() == drawableComponents(loaded.file.board),
          "every component gets exactly one rectangle");
    /* A wire is a polyline now: one segment when its ends share a row or a
       column, more once it has to bend around.  Every segment is axis aligned
       and every wire has at least one. */
    const std::vector<simcore_view::SceneLine> drawn = drawnSegments(loaded.scene);
    check(drawn.size() >= loaded.file.board.wires.size(), "every wire gets at least one segment");
    for (const simcore_view::SceneLine& line : drawn) {
        check(line.x1 == line.x2 || line.y1 == line.y2, "every wire segment is horizontal or vertical");
    }
    std::vector<size_t> segments_per_wire(loaded.file.board.wires.size(), 0);
    for (const simcore_view::SceneLine& line : drawn) {
        if (line.has_wire && line.wire_index < segments_per_wire.size()) ++segments_per_wire[line.wire_index];
    }
    for (size_t index = 0; index < segments_per_wire.size(); ++index) {
        check(segments_per_wire[index] >= 1, "wire " + std::to_string(index) + " has at least one segment");
    }
    /* Input 0 in / 2 out, AND 2 in / 1 out, Output 1 in / 0 out. */
    size_t pins = 0;
    for (const simcore_view::SceneDot& dot : loaded.scene.dots) {
        if (!dot.connection) ++pins;
    }
    check(pins == 6, "the pins are drawn: 2 + 3 + 1");
    check(loaded.scene.width > 0.0f && loaded.scene.height > 0.0f, "the scene has a size");

    /* Both input nets carry 0, so the AND drives 0 and the output sees 0. */
    check(loaded.ran, "the engine ran the and gate board");
    check(loaded.values.netCount() == 3, "three nets: two inputs and the output");
    for (tcsim::NetId net = 0; net < loaded.values.netCount(); ++net) {
        check(loaded.values.net(net).value.toString() == "0",
              "net " + std::to_string(net) + " is 0 with both inputs low");
    }

    /* Every wire belongs to a net, and every net that the game reads back has
       the state bytes the fixture declared. */
    check(loaded.built.net_of_wire.size() == 3, "three wires were placed on nets");
    const tcsim::NetId output_net = loaded.built.net_of_wire[2];
    check(output_net != tcsim::kNoNet, "the output wire is on a net");
    const std::vector<uint64_t>& slots = loaded.built.slots_of_net[output_net];
    check(slots.size() == 2 && slots[0] == 260 && slots[1] == 261,
          "the output net writes back to the fixture's state bytes 260 and 261");
}

void testRingOscillates() {
    const Loaded loaded = load(std::string(kData) + "ring3.txt", 200);
    if (!loaded.file.ok) return;

    check(!loaded.built.fatal(), "the ring board builds: simcore does not refuse a loop");
    check(loaded.values.netCount() == 3, "the ring has three nets");
    /* A seeded three-inverter ring with a two tick arc switches twice per
       twelve ticks per stage: a settled circuit would show three or four
       evaluations, not a hundred. */
    check(loaded.ran, "the engine ran the ring board");
    const size_t switches = [&] {
        size_t total = 0;
        for (tcsim::NetId net = 0; net < loaded.values.netCount(); ++net) {
            total += static_cast<size_t>(loaded.values.net(net).changes);
        }
        return total;
    }();
    check(switches >= 40, "the ring oscillates instead of settling (net switch count " +
                              std::to_string(switches) + ")");
}

void testLatchHolds() {
    const Loaded low = load(std::string(kData) + "latch.txt", 200, 0);
    if (!low.file.ok) return;
    check(!low.built.fatal(), "the latch board builds");
    check(low.built.source_nets.size() == 2, "the level input drives two nets");

    /* Wire 0 and wire 3 carry the driven inputs, wire 1 and wire 2 the
       cross-coupled outputs.  Reading by wire rather than by net number keeps
       the assertion about the circuit instead of about the builder's numbering. */
    const auto driven = [&](const Loaded& loaded, size_t wire) {
        const tcsim::NetId net = loaded.built.net_of_wire[wire];
        return net == tcsim::kNoNet ? std::string("?") : loaded.values.net(net).value.toString();
    };
    check(driven(low, 0) == "0" && driven(low, 3) == "0", "both driven inputs are low");
    check(driven(low, 1) == "1" && driven(low, 2) == "1",
          "both NAND outputs hold 1 while an input is low: the loop has a defined state");

    /* The same board with the other input combination: a latch that does not
       move would be a broken loop, and this is the check that would say so. */
    const Loaded high = load(std::string(kData) + "latch.txt", 200, 1);
    check(high.built.source_nets.size() == 2, "the second pattern drives the same two nets");
    check(driven(high, 0) == "1" && driven(high, 3) == "0", "the input pattern actually reached the nets");
    check(driven(high, 1) == "0" && driven(high, 2) == "1", "the latch flipped to the other stable state");
}

void testPicking() {
    const Loaded loaded = load(std::string(kData) + "and_gate.txt");
    if (!loaded.file.ok) return;

    /* Clicking the middle of a body selects that component. */
    for (const simcore_view::SceneRect& rect : loaded.scene.rects) {
        const simcore_view::ScenePick picked = simcore_view::pickScene(
            loaded.scene, rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f);
        check(picked.valid && picked.on_component && picked.component == rect.component_index,
              "clicking component " + std::to_string(rect.component_index) + "'s middle selects it");
    }

    /* A point on a wire, far enough from every body, selects that wire. */
    const simcore_view::SceneLine& line = loaded.scene.lines[0];
    const float mid_x = (line.x1 + line.x2) * 0.5f;
    const float mid_y = (line.y1 + line.y2) * 0.5f;
    const simcore_view::ScenePick picked = simcore_view::pickScene(loaded.scene, mid_x, mid_y);
    check(picked.valid && picked.on_wire && picked.wire == line.wire_index,
          "clicking the first wire's midpoint selects wire 0");

    /* Empty space selects nothing, so a click can also clear the selection. */
    const simcore_view::ScenePick nothing = simcore_view::pickScene(loaded.scene, -500.0f, -500.0f);
    check(!nothing.valid, "clicking far away selects nothing");
}

/* The pin offsets are not symmetric about the component's own cell: the
   prototype table puts an input at x = -1 and an output at x = +2.  A body of a
   fixed width centred on the cell therefore touches the inputs and leaves the
   output stub hanging a whole cell off the right edge, which is exactly what
   the first version of this renderer drew.  These assertions are the shape of
   the fix. */
void testBodySitsOnItsPins() {
    const float cell = 30.0f;
    tcsim::BoardComponent component;
    tcsim::KindPins pins;

    component.kind = 0x04; /* AND: in (-1,-1)/(-1,1), out (2,0) */
    check(simcore_view::pinsForKind(component, pins), "the AND kind has geometry");
    const simcore_view::scene_detail::BodyBox box =
        simcore_view::scene_detail::bodyFor(pins, 0, 0.0f, 0.0f, cell);
    check(std::fabs(box.left - (-0.5f * cell)) < 0.01f, "the body stops half a cell short of the input pin");
    check(std::fabs(box.right - (1.5f * cell)) < 0.01f, "and half a cell short of the output pin");
    check(box.right - box.left > 1.9f * cell, "so the body spans the whole gate, not a cell and a half");

    component.kind = 0x03; /* NOT: both pins on y = 0 */
    check(simcore_view::pinsForKind(component, pins), "the NOT kind has geometry");
    const simcore_view::scene_detail::BodyBox thin =
        simcore_view::scene_detail::bodyFor(pins, 0, 0.0f, 0.0f, cell);
    check(thin.bottom - thin.top >= 1.2f * cell - 0.01f, "a single row of pins still gets a body with height");

    /* A quarter turn has to move the box with the pins, or a rotated component
       keeps an upright body. */
    const simcore_view::scene_detail::BodyBox turned =
        simcore_view::scene_detail::bodyFor(pins, 1, 0.0f, 0.0f, cell);
    check(turned.bottom - turned.top > turned.right - turned.left,
          "a quarter turn makes the body taller than it is wide");
}

void testBodyHoldsItsLabel() {
    const Loaded loaded = load(std::string(kData) + "and_gate.txt");
    if (!loaded.file.ok) return;
    /* Component 0 is the level Input: it has two outputs in the same column, so
       a strictly pin-derived body is a sliver and the name spills out of it. */
    bool seen = false;
    for (const simcore_view::SceneRect& rect : loaded.scene.rects) {
        if (rect.component_index != 0) continue;
        seen = true;
        check(rect.w > 1.5f * 30.0f, "the Input body is widened to hold its own label");
    }
    check(seen, "the Input component was drawn");
    for (const simcore_view::SceneRect& rect : loaded.scene.rects) {
        if (rect.label.empty()) continue;
        check(rect.w >= static_cast<float>(rect.label.size()) * 0.30f * 30.0f,
              "every body is at least as wide as the label it carries");
    }
}

/* A wire ending at a pin has to say so.  Before this the pin was a small dark
   dot whatever was attached to it, so a wire that reached its pin and a wire
   that stopped in mid air looked the same - which is the one question this
   picture exists to answer. */
void testConnectionsAreMarked() {
    const Loaded loaded = load(std::string(kData) + "ring3.txt", 200);
    if (!loaded.file.ok) return;

    size_t connections = 0;
    size_t plain_pins = 0;
    float connection_radius = 0.0f;
    float pin_radius = 0.0f;
    for (const simcore_view::SceneDot& dot : loaded.scene.dots) {
        if (dot.connection) {
            ++connections;
            connection_radius = dot.r;
        } else {
            ++plain_pins;
            pin_radius = dot.r;
        }
    }
    /* Three wires, both ends on a pin, and the ring has no dangling end. */
    check(connections == 6, "every wire end that lands on a pin gets a connection dot");
    check(plain_pins == 6, "the six pins are still drawn");
    check(connection_radius > pin_radius, "a connection dot is bigger than a bare pin");

    /* The tidy grid moves the parts but not what is attached to what, so the
       count has to survive it. */
    simcore_view::SceneInput input;
    input.board = &loaded.file.board;
    input.built = &loaded.built;
    input.values = loaded.ran ? &loaded.values : nullptr;
    simcore_view::SceneOptions options;
    options.tidy_layout = true;
    const simcore_view::Scene tidy = simcore_view::buildScene(input, options);
    size_t tidy_connections = 0;
    for (const simcore_view::SceneDot& dot : tidy.dots) {
        if (dot.connection) ++tidy_connections;
    }
    check(tidy_connections == connections, "the tidy grid keeps the same connections");
}

/* The tidy grid is a reading aid, so the properties worth asserting are that
   it is usable and that it is honestly different from the board. */
/* Where two wires share a track, the shorter has to be drawn last.  Emitting
   them in board order let a long run paint over the short stub that lands on
   it, and a wire ending on another wire then read as a connection to it. */
void testWireDrawOrder() {
    const Loaded loaded = load(std::string(kData) + "ring3.txt", 200);
    if (!loaded.file.ok) return;
    const std::vector<simcore_view::SceneLine> drawn = drawnSegments(loaded.scene);

    for (size_t a = 0; a < drawn.size(); ++a) {
        for (size_t b = a + 1; b < drawn.size(); ++b) {
            if (!collinearOverlap(drawn[a], drawn[b])) continue;
            check(lineLength(drawn[a]) >= lineLength(drawn[b]),
                  "where two wires share a track, the shorter one is drawn last");
        }
    }

    /* A wire that doubles back on itself overlaps its own track, and that is
       the shape that reads as a cut wire.  Any route with a reversal produces
       one, so this is the invariant the cost function's reversal term buys. */
    for (size_t a = 0; a < drawn.size(); ++a) {
        for (size_t b = a + 1; b < drawn.size(); ++b) {
            if (drawn[a].wire_index != drawn[b].wire_index) continue;
            check(!collinearOverlap(drawn[a], drawn[b]),
                  "wire " + std::to_string(drawn[a].wire_index) + " does not double back on its own track");
        }
    }

    /* Every drawn segment carries a casing immediately before it, in the canvas
       colour, so the one on top visibly lies over the one below. */
    size_t halos = 0;
    for (size_t index = 0; index + 1 < loaded.scene.lines.size(); ++index) {
        const simcore_view::SceneLine& line = loaded.scene.lines[index];
        if (!line.halo) continue;
        ++halos;
        const simcore_view::SceneLine& wire = loaded.scene.lines[index + 1];
        check(!wire.halo, "a casing is immediately followed by the wire it belongs to");
        check(wire.wire_index == line.wire_index && wire.x1 == line.x1 && wire.y1 == line.y1 &&
                  wire.x2 == line.x2 && wire.y2 == line.y2,
              "the casing has its wire's own geometry");
        check(line.color.r == simcore_view::kCanvasFill.r && line.color.g == simcore_view::kCanvasFill.g &&
                  line.color.b == simcore_view::kCanvasFill.b,
              "the casing is drawn in the canvas colour");
        check(line.width > wire.width, "the casing is wider than the wire");
    }
    check(halos == drawn.size(), "every drawn segment has exactly one casing");

    /* And a casing is not a thing anyone clicks: picking must ignore it. */
    const simcore_view::SceneLine& first = drawn.front();
    const simcore_view::ScenePick picked = simcore_view::pickScene(
        loaded.scene, (first.x1 + first.x2) * 0.5f, (first.y1 + first.y2) * 0.5f, 2.0f);
    check(!picked.valid || picked.on_wire, "a casing never becomes its own pick target");
}

void testTidyLayout() {
    const Loaded plain = load(std::string(kData) + "latch.txt", 200, 0);
    if (!plain.file.ok) return;

    simcore_view::SceneInput input;
    input.board = &plain.file.board;
    input.built = &plain.built;
    input.values = plain.ran ? &plain.values : nullptr;
    simcore_view::SceneOptions options;
    options.tidy_layout = true;
    const simcore_view::Scene tidy = simcore_view::buildScene(input, options);

    check(tidy.rects.size() == plain.scene.rects.size(), "the tidy grid draws the same components");
    check(tidy.lines.size() == plain.scene.lines.size(), "and the same wires, bent the same way");

    /* Bodies must not land on top of each other - that is the whole point of
       putting them on a grid. */
    for (size_t a = 0; a < tidy.rects.size(); ++a) {
        for (size_t b = a + 1; b < tidy.rects.size(); ++b) {
            const simcore_view::SceneRect& ra = tidy.rects[a];
            const simcore_view::SceneRect& rb = tidy.rects[b];
            const bool apart = ra.x + ra.w <= rb.x || rb.x + rb.w <= ra.x || ra.y + ra.h <= rb.y ||
                               rb.y + rb.h <= ra.y;
            check(apart, "tidy bodies " + std::to_string(a) + " and " + std::to_string(b) + " do not overlap");
        }
    }

    /* A level input has no device in the netlist.  It still has to end up to
       the left of what it drives rather than parked in a trailing column. */
    float input_x = 0.0f;
    float gate_x = 0.0f;
    for (const simcore_view::SceneRect& rect : tidy.rects) {
        if (rect.label == "Input") input_x = rect.x;
        if (rect.label == "NAND") gate_x = std::max(gate_x, rect.x);
    }
    check(input_x < gate_x, "the level input is placed left of the gates it drives");

    /* And the grid has to actually differ, or "整理" is a button that does
       nothing. */
    bool moved = false;
    for (size_t index = 0; index < tidy.rects.size() && index < plain.scene.rects.size(); ++index) {
        if (std::fabs(tidy.rects[index].x - plain.scene.rects[index].x) > 1.0f ||
            std::fabs(tidy.rects[index].y - plain.scene.rects[index].y) > 1.0f) {
            moved = true;
        }
    }
    check(moved, "the tidy grid is not simply the board's own coordinates");
}

/* The memory subsystem, as far as the board record can know it.  The offsets
   are the game's own prototype table (re-dumped 2026-09-29; the dump used to
   stop after four pins, which cut the Store port's fifth input off).  The
   record counts are the ones the live board carries - 0x36 3/2, 0x38 5/1,
   0x76 0/0 - and the builder refuses a board when the two disagree, so that
   last equality is the part of this test that keeps a wrong table from being
   silently accepted. */
void testMemoryGeometry() {
    const simcore_view::BoardFile recorded =
        simcore_view::readBoardFile(std::string(kData) + "memory_fields.txt");
    check(recorded.ok && recorded.board.components.size() == 1,
          "the viewer reads a board record carrying RAM metadata");
    check(recorded.ok && recorded.board.components.size() == 1 &&
              recorded.board.components[0].memory_bits == 8 &&
              recorded.board.components[0].memory_extra_address_bits == 16 &&
              recorded.board.components[0].memory_little_endian,
          "memory width, extra address bits and endian survive board-record replay");
    const auto pins = [](uint8_t kind, uint8_t record_in, uint8_t record_out, tcsim::KindPins& out) {
        tcsim::BoardComponent component;
        component.kind = kind;
        component.input_count = record_in;
        component.output_count = record_out;
        return simcore_view::pinsForKind(component, out) && out.input_count == record_in &&
               out.output_count == record_out;
    };
    tcsim::KindPins load;
    check(pins(0x36, 3, 2, load), "the Load port's geometry matches the record's 3in/2out");
    check(load.in_x[0] == -15 && load.in_y[0] == -1 && load.in_x[1] == -15 && load.in_y[1] == 0 &&
              load.in_x[2] == 0 && load.in_y[2] == 0,
          "the Load port's three inputs are where the game's table puts them");
    check(load.out_x[0] == 16 && load.out_y[0] == -1 && load.out_x[1] == 1 && load.out_y[1] == 0,
          "the Load port's outputs are the 32-bit result and the write-back pin");
    tcsim::KindPins store;
    check(pins(0x38, 5, 1, store), "the Store port's geometry matches the record's 5in/1out");
    check(store.in_x[3] == 0 && store.in_y[3] == 0,
          "the Store port's fourth input is the RAM socket at its own cell");
    check(store.in_x[4] == -1 && store.in_y[4] == 0,
          "the Store port's fifth input exists: in4=(-1,0), the pin the four-wide dump hid");
    tcsim::KindPins ram;
    check(pins(0x76, 0, 0, ram), "the RAM has no pins of its own, exactly as its record says");
    check(std::string(ram.name) == "RAM", "and it is named, so a report can say what is unsupported");

    tcsim::KindPins bit_switch;
    check(pins(0x0c, 2, 1, bit_switch), "the built-in Bit Switch has offline viewer geometry");
    check(bit_switch.in_x[0] == 0 && bit_switch.in_y[0] == 1 && bit_switch.in_x[1] == -1 &&
              bit_switch.in_y[1] == 0 && bit_switch.out_x[0] == 2 && bit_switch.out_y[0] == 0,
          "the Bit Switch enable, data and tri-state output pins match the game catalogue");
    tcsim::KindPins word_switch;
    check(pins(0x19, 2, 1, word_switch), "the built-in Word Switch has offline viewer geometry");

    /* A board whose only components are the memory subsystem: the wiring goes
       through the ports, so the geometry has to cover them, and the RAM has to
       be accepted as a pin-less part rather than reported as unknown. */
    tcsim::BoardView board;
    const auto add = [&board](uint8_t kind, int16_t x, int16_t y, uint8_t in, uint8_t out) {
        tcsim::BoardComponent component;
        component.kind = kind;
        component.x = x;
        component.y = y;
        component.input_count = in;
        component.output_count = out;
        board.components.push_back(component);
    };
    add(0x76, 0, 0, 0, 0);       /* the RAM */
    add(0x38, 0, -10, 5, 1);     /* a Store port on its first socket */
    add(0x36, 0, -13, 3, 2);     /* a Load port on the next one */
    /* A wire on each port's address pin, so the ports really reach a net: a pin
       no wire touches never reaches the device step. */
    board.wires.push_back(tcsim::BoardWire{-15, -10, -15, -6, 32, 256});
    board.wires.push_back(tcsim::BoardWire{-15, -12, -15, -7, 1, 258});
    const tcsim::BuildResult built = tcsim::buildNetList(board, &simcore_view::pinsForKind);
    bool geometry_gap = false;
    for (const tcsim::BuildNote& note : built.notes) {
        if (note.text.find("no pin geometry") != std::string::npos) geometry_gap = true;
        if (note.text.find("pins in its record") != std::string::npos) geometry_gap = true;
    }
    check(!geometry_gap, "the memory board has no geometry gap");
    check(!built.fatal(), "RAM plus attached ports is now a runnable simcore board");
    check(built.memories.size() == 1, "the RAM creates one shared memory image");
    check(built.memories.size() == 1 && built.memories[0].ports.size() == 2,
          "both socket ports are attached to the same memory image");
    check(built.memories.size() == 1 && built.memories[0].ports[0] == 2 &&
              built.memories[0].ports[1] == 1,
          "memory ports execute top-to-bottom, farthest from the RAM first");
}

/* The game's word Mux (kind 0x2a) in the viewer's own path: the frozen table
   has to carry its geometry, and the record a live session left behind has to
   build into a runnable device - the sandbox dump from 2026-09-29 has a mux,
   and before this the viewer refused the whole board as "no device in the
   library". */
void testMuxGeometry() {
    tcsim::BoardComponent component;
    component.kind = 0x2a;
    component.input_count = 3;
    component.output_count = 1;
    tcsim::KindPins mux;
    check(simcore_view::pinsForKind(component, mux), "the frozen table covers the game's Mux");
    check(mux.input_count == 3 && mux.output_count == 1 && mux.in_x[0] == -1 && mux.in_y[0] == -1 &&
              mux.in_x[1] == -1 && mux.in_y[1] == 0 && mux.in_x[2] == -1 && mux.in_y[2] == 1 &&
              mux.out_x[0] == 2 && mux.out_y[0] == 0,
          "the Mux's select, A, B and output pins match the game catalogue");
    check(std::string(mux.name) == "Mux", "and it is named, so a report can say what it is");

    const Loaded loaded = load(std::string(kData) + "mux.txt", 40);
    if (!loaded.file.ok) return;
    const tcsim::BuildResult& built = loaded.built;
    bool reported = false;
    for (const tcsim::BuildNote& note : built.notes) {
        if (note.text.find("no device in the library") != std::string::npos) reported = true;
    }
    check(!reported && !built.fatal(), "a board with a Mux is no longer refused as unsupported");
    check(built.netlist.deviceCount() == 1 && built.netlist.devices()[0].spec->name == "mux32",
          "the replayed Mux takes the 32-bit width its record carries");
    const tcsim::DeviceInstance& device = built.netlist.devices()[0];
    check(device.spec->inputs[0].bits == 1 && device.spec->inputs[1].bits == 32 &&
              device.spec->outputs[0].bits == 32,
          "the record's one-bit wire and its 32-bit wires land on a mux of the right shape");
    check(device.inputs[0] == built.net_of_wire[0] && built.netlist.net(built.net_of_wire[0]).bits == 1 &&
              device.inputs[1] == built.net_of_wire[1] && device.inputs[2] == built.net_of_wire[2] &&
              device.outputs[0] == built.net_of_wire[3],
          "the select keeps the one-bit net and A, B, Y get the wide ones");
    check(loaded.ran, "the engine runs the replayed mux board instead of refusing it");
    /* The select is held low by the level in the wire the record shows, so the
       replay publishes A; B is a 16-bit wire reaching a 32-bit pin, which the
       engine widens by the source's own leftmost bit. */
    check(loaded.values.net(built.net_of_wire[1]).bits == 32 && loaded.values.net(built.net_of_wire[2]).bits == 16,
          "the two data nets keep the widths their wires declared");
}

/* The game's Screen (kind 0x5b) and the two host peripherals beside it (Time
   0x5c, Keyboard 0x5d): a live sandbox board carries one of each, and before
   the frozen table knew them the viewer refused the whole board over "no pin
   geometry". */
void testHostPeripherals() {
    tcsim::BoardComponent screen;
    screen.kind = 0x5b;
    screen.input_count = 3;
    screen.output_count = 0;
    tcsim::KindPins screen_pins;
    check(simcore_view::pinsForKind(screen, screen_pins), "the frozen table covers the game's Screen");
    check(screen_pins.input_count == 3 && screen_pins.output_count == 0 && screen_pins.in_x[0] == -26 &&
              screen_pins.in_y[0] == -19 && screen_pins.in_x[1] == -26 && screen_pins.in_y[1] == -18 &&
              screen_pins.in_x[2] == -26 && screen_pins.in_y[2] == -17,
          "the Screen's Enable, Setting index and Value pins are where the game's table puts them");
    check(std::string(screen_pins.name) == "Screen", "and it is named, so a report can say what it is");

    tcsim::BoardComponent time;
    time.kind = 0x5c;
    time.input_count = 0;
    time.output_count = 1;
    tcsim::KindPins time_pins;
    check(simcore_view::pinsForKind(time, time_pins) && time_pins.output_count == 1 &&
              time_pins.out_x[0] == 2 && time_pins.out_y[0] == 0 && std::string(time_pins.name) == "Time",
          "the Time component is a single output at the catalogue's cell");

    tcsim::BoardComponent keyboard;
    keyboard.kind = 0x5d;
    keyboard.input_count = 1;
    keyboard.output_count = 3;
    tcsim::KindPins keyboard_pins;
    check(simcore_view::pinsForKind(keyboard, keyboard_pins) && keyboard_pins.input_count == 1 &&
              keyboard_pins.out_x[0] == 4 && keyboard_pins.out_y[0] == -3 && keyboard_pins.out_x[1] == 5 &&
              keyboard_pins.out_y[1] == 0 && keyboard_pins.out_x[2] == 5 && keyboard_pins.out_y[2] == 1 &&
              std::string(keyboard_pins.name) == "Keyboard",
          "the Keyboard's input pin and its three event outputs match the catalogue");

    /* A board whose only component is a screen wired the way a live board wires
       it: a 1-bit Enable, 32-bit wires into the Setting and Value pins.  It has
       to build - the pixels themselves are the game's own business for now
       (docs/PLAN-sandbox-simulator.md section 17). */
    tcsim::BoardView board;
    tcsim::BoardComponent wired;
    wired.kind = 0x5b;
    wired.input_count = 3;
    wired.output_count = 0;
    board.components.push_back(wired);
    board.wires.push_back(tcsim::BoardWire{-26, -19, -40, -19, 1, 256});
    board.wires.push_back(tcsim::BoardWire{-26, -18, -17, -18, 32, 258});
    board.wires.push_back(tcsim::BoardWire{-26, -17, -17, -17, 32, 260});
    const tcsim::BuildResult built = tcsim::buildNetList(board, &simcore_view::pinsForKind);
    bool refused = false;
    for (const tcsim::BuildNote& note : built.notes) {
        if (note.text.find("no pin geometry") != std::string::npos) refused = true;
        if (note.text.find("no device in the library") != std::string::npos) refused = true;
    }
    check(!refused && !built.fatal(), "a board with a Screen is no longer refused as unsupported");
    check(built.screens.size() == 1 && built.netlist.deviceCount() == 1,
          "the replayed Screen becomes the device that owns its settings table");
    check(built.observed_nets.size() == 3, "and the three nets it reads are the ones the game reads back");
}

void testUnknownKindIsReported() {
    const Loaded loaded = load("build/unknown-kind.txt");
    if (!loaded.file.ok) {
        std::printf("SKIP the unknown kind board: build/view-tests-unknown.txt is missing\n");
        return;
    }
    check(loaded.built.fatal(), "a kind with no pin geometry makes the build fatal");
    bool named = false;
    for (const tcsim::BuildNote& note : loaded.built.notes) {
        if (note.text.find("0x99") != std::string::npos) named = true;
    }
    check(named, "the fatal note names the kind in hex, not decimal behind a 0x prefix");
    check(!loaded.ran, "the engine is not run on a board the builder refused");
    check(!loaded.scene.rects.empty(), "the unsupported component is still drawn");
}

void testSvgRoundTrip() {
    const Loaded loaded = load(std::string(kData) + "and_gate.txt");
    if (!loaded.file.ok) return;
    const std::string svg = simcore_view::toSvg(loaded.scene);
    check(svg.rfind("<svg", 0) == 0, "the SVG starts with an <svg> element");
    check(svg.find("</svg>") != std::string::npos, "the SVG is closed");
    check(svg.find("AND") != std::string::npos, "the SVG carries the component names");
}

/* A custom component (kind 0x4e) is a definition, not a device: the record the
   live Mod writes carries the definition's own circuit, and the viewer has to
   flatten it the way the Mod does.  Before this worked, any board with a
   foundry component on it was refused outright.

   The fixture's definition is one Input Pin wired straight to one Output Pin,
   so the value that reaches the instance's input pin has to come out of its
   output pin - which is the one thing a refusal cannot fake. */
void testCustomComponentFlattens() {
    const Loaded loaded = load(std::string(kData) + "custom_passthrough.txt");
    if (!loaded.file.ok) return;
    check(!loaded.built.fatal(), "a board with a custom component is not refused");
    check(loaded.file.definitions.size() == 1, "the definition the record carried was read");
    check(loaded.file.board.components.size() == 3, "the board itself keeps its three components");
    check(loaded.built.notes.empty(), "a flattenable custom component reports nothing");
    /* The instance keeps its own slot and the internals are appended after the
       board's components, which is what lets the drawing index the board it was
       handed while the netlist is the flattened one. */
    check(loaded.built.devices_by_component.size() > loaded.file.board.components.size(),
          "the definition's internals were appended after the board's own components");
    check(loaded.built.devices_by_component[2] == tcsim::kNoDevice,
          "the custom instance itself is no device");
    check(loaded.ran, "the flattened board runs");
    const tcsim::NetId output =
        loaded.built.net_of_wire.size() > 1 ? loaded.built.net_of_wire[1] : tcsim::kNoNet;
    if (output == tcsim::kNoNet || output >= loaded.values.netCount()) {
        check(false, "the output wire lands on a net");
        return;
    }
    check(true, "the output wire lands on a net");
    check(loaded.values.net(output).value.toString() == "1",
          "the constant's 1 reaches the output through the custom component");
}

}  // namespace

int main() {
    testAndGate();
    testRingOscillates();
    testLatchHolds();
    testPicking();
    testBodySitsOnItsPins();
    testBodyHoldsItsLabel();
    testConnectionsAreMarked();
    testWireDrawOrder();
    testTidyLayout();
    testMemoryGeometry();
    testMuxGeometry();
    testHostPeripherals();
    testUnknownKindIsReported();
    testSvgRoundTrip();
    testCustomComponentFlattens();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
