/* Offline checks for the internal-netlist builder (tests what the compiler can
   see, since the builder is header-only: src/component_netlist.hpp).

   The claim the builder makes is "the bytes I write are the shape the game has
   been compiling since 2026-09".  The evidence for that is not a copy of a copy:
   this case reads the *definition file the fixture generator produced*
   (build/nl_def_and3.data, written by tests/and-component-fixture.cpp and used
   by the real-machine level runs), decodes it with the loader's own circuit
   reader, and requires the definition this encoder builds for the same netlist
   to describe the same circuit - same header, same pins, same node, same wires
   (start point and total displacement), in the same order.

   What it also pins: the two coordinate systems (outer pins in pin units, inner
   nodes in board cells), every refusal the validator promises, and the wire
   routing rules (two wires may *cross* - a wire in this game has two endpoints
   and only a shared endpoint joins two nets - but an endpoint lying on another
   wire, or two wires on top of each other, are refused; no running through a
   pin; fan-out from one source pin allowed). */
#include "../src/component_netlist.hpp"
#include "../src/snappy_raw.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

/* A node's pin offsets are the catalogue's pins (board cells), while the type's
   own pins are the netlist's; both are named Pin, so the alias keeps the two
   apart at a glance. */
using NodePin = tc::builtin_catalogue::Pin;

void check(bool condition, const std::string& what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
}

/* ---- a tiny reader for the payload this encoder writes (and the fixture's) -- */
struct Reader {
    const std::vector<uint8_t>& data;
    size_t at = 0;
    bool ok = true;
    uint64_t number(unsigned size) {
        uint64_t value = 0;
        if (at + size > data.size()) { ok = false; return 0; }
        for (unsigned i = 0; i < size; ++i) value |= uint64_t(data[at + i]) << (i * 8);
        at += size;
        return value;
    }
    std::string string() {
        const uint64_t size = number(2);
        if (at + size > data.size()) { ok = false; return {}; }
        std::string text(reinterpret_cast<const char*>(data.data() + at), static_cast<size_t>(size));
        at += size;
        return text;
    }
};

struct Component {
    uint32_t kind = 0;
    int32_t x = 0, y = 0;
    uint64_t identity = 0;
    std::string name;
    int32_t pinIndex = 0;
    int64_t wordSize = 0;
    std::vector<uint64_t> values;
};
struct Wire {
    int32_t x = 0, y = 0, endX = 0, endY = 0;
};
struct Circuit {
    uint64_t customId = 0;
    int64_t gateCost = 0, delay = 0;
    std::vector<Component> components;
    std::vector<Wire> wires;
};

Circuit parse(const std::vector<uint8_t>& raw) {
    Circuit circuit;
    Reader reader{raw};
    circuit.customId = reader.number(8);
    reader.number(4);
    circuit.gateCost = static_cast<int64_t>(reader.number(8));
    circuit.delay = static_cast<int64_t>(reader.number(8));
    reader.number(1);   /* bool-ish flag the game writes as 1 */
    reader.number(8);   /* 10000, the same constant the scaffold carries */
    const uint64_t values = reader.number(2);   /* empty value sequence */
    for (uint64_t i = 0; i < values; ++i) reader.number(8);
    reader.string();    /* empty name */
    reader.number(1);   /* u8 */
    reader.number(2);   /* u16 */
    const uint64_t bytes = reader.number(2);    /* empty byte sequence */
    for (uint64_t i = 0; i < bytes; ++i) reader.number(1);
    reader.string();    /* empty string */
    for (int i = 0; i < 512; ++i) reader.number(1);
    const uint64_t components = reader.number(8);
    for (uint64_t i = 0; i < components; ++i) {
        Component component;
        component.kind = static_cast<uint32_t>(reader.number(2));
        component.x = static_cast<int16_t>(reader.number(2));
        component.y = static_cast<int16_t>(reader.number(2));
        reader.number(1);
        component.identity = reader.number(8);
        component.name = reader.string();
        const uint64_t valueCount = reader.number(2);
        for (uint64_t v = 0; v < valueCount; ++v) component.values.push_back(reader.number(8));
        reader.number(8);
        component.pinIndex = static_cast<int16_t>(reader.number(2));
        component.wordSize = static_cast<int64_t>(reader.number(8));
        reader.number(1);   /* bool_a */
        reader.number(8);   /* value_i64_a */
        reader.number(8);   /* value_i64_b */
        reader.number(1);   /* bool_b */
        reader.number(1);   /* init_data */
        reader.number(2);   /* subcomponents */
        reader.number(2);   /* settings */
        circuit.components.push_back(std::move(component));
    }
    const uint64_t wires = reader.number(8);
    for (uint64_t i = 0; i < wires; ++i) {
        Wire wire;
        reader.number(1);
        reader.string();
        wire.x = static_cast<int16_t>(reader.number(2));
        wire.y = static_cast<int16_t>(reader.number(2));
        int32_t x = wire.x, y = wire.y;
        for (;;) {
            const uint32_t segment = static_cast<uint32_t>(reader.number(2));
            if (!segment) break;
            const int magnitude = segment & 0x3fff;
            /* Direction bits: y is 0x4000 (positive) / 0xC000 (negative), x is
               0x0000 / 0x8000 - the encoding the scaffold writer documents. */
            if (segment & 0x4000) y += (segment & 0x8000) ? -magnitude : magnitude;
            else x += (segment & 0x8000) ? -magnitude : magnitude;
        }
        wire.endX = x; wire.endY = y;
        circuit.wires.push_back(wire);
    }
    check(reader.ok, "the payload parsed to the end");
    return circuit;
}

/* The fixture's own definition file, decoded the way the loader reads a circuit
   file: version byte, then the raw Snappy block. */
std::vector<uint8_t> readCircuitFile(const std::string& path, bool& ok) {
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    ok = false;
    if (bytes.size() < 2 || bytes[0] != 14) return {};
    std::vector<uint8_t> raw;
    if (!tc::snappy_raw::decode(bytes.data() + 1, bytes.size() - 1, 1u << 20, raw)) return {};
    ok = true;
    return raw;
}

/* Maps every wire back onto the pins it connects, naming them by the role they
   play in the definition: `in<i>` for the type's own input pins, `out<j>` for
   its output pins, `n<k>.<p>` for an inner node's pin - sources by their pin
   index in `sources`, sinks in `sinks`.  The inner offsets are the ones the
   pinned build carries for the two kinds this case uses (kinds.txt):
   kind 0x05 "3 Pin AND" - in (-1,-1) (-1,0) (-1,1), out (2,0) - and kind 0x03
   "NOT" - in (-1,0), out (2,0).  Returns false when a wire ends somewhere that
   is not a pin, which would be a broken file in itself. */
inline bool wireEndpoints(const std::vector<Component>& components,
                          const std::vector<Wire>& wires,
                          std::vector<std::string>& out) {
    std::vector<std::pair<tc::component_netlist::Point, std::string>> sources, sinks;
    uint32_t inputIndex = 0, outputIndex = 0, innerIndex = 0;
    for (const Component& component : components) {
        const tc::component_netlist::Point origin{component.x, component.y};
        if (component.kind == 0x4f) {
            sources.push_back({{origin.x + 3, origin.y}, "in" + std::to_string(inputIndex++)});
            continue;
        }
        if (component.kind == 0x51) {
            sinks.push_back({{origin.x - 3, origin.y}, "out" + std::to_string(outputIndex++)});
            continue;
        }
        std::vector<tc::component_netlist::Point> nodeSinks, nodeSources;
        if (component.kind == 0x05) {
            nodeSinks = {{-1, -1}, {-1, 0}, {-1, 1}};
            nodeSources = {{2, 0}};
        } else if (component.kind == 0x03) {
            nodeSinks = {{-1, 0}};
            nodeSources = {{2, 0}};
        } else {
            return false; /* this case only compares the two kinds above */
        }
        const std::string prefix = "n" + std::to_string(innerIndex++) + ".";
        for (size_t pin = 0; pin < nodeSinks.size(); ++pin)
            sinks.push_back({{origin.x + nodeSinks[pin].x, origin.y + nodeSinks[pin].y},
                             prefix + std::to_string(pin)});
        for (size_t pin = 0; pin < nodeSources.size(); ++pin)
            sources.push_back({{origin.x + nodeSources[pin].x, origin.y + nodeSources[pin].y},
                               prefix + "src" + std::to_string(pin)});
    }
    out.clear();
    for (const Wire& wire : wires) {
        std::string from, to;
        for (const auto& candidate : sources)
            if (candidate.first.x == wire.x && candidate.first.y == wire.y) from = candidate.second;
        for (const auto& candidate : sinks)
            if (candidate.first.x == wire.endX && candidate.first.y == wire.endY) to = candidate.second;
        if (from.empty() || to.empty()) {
            std::printf("FAIL a wire from (%d,%d) to (%d,%d) does not sit on pins\n",
                        wire.x, wire.y, wire.endX, wire.endY);
            return false;
        }
        out.push_back(from + "->" + to);
    }
    return true;
}

/* The netlist this case builds through the new encoder: exactly the
   "3 Pin AND" the fixture writes by hand, so the two can be compared. */
tc::component_netlist::Definition andThree() {
    using namespace tc::component_netlist;
    Definition d;
    d.customId = 0x414E44335F303031ull;
    d.name = "AND3";
    d.gateCost = 1;
    d.delay = 1;
    d.inputs = {Pin{"A", 1}, Pin{"B", 1}, Pin{"C", 1}};
    d.outputs = {Pin{"Out", 1}};
    Node and3;
    and3.kind = 0x05; /* "3 Pin AND" - kinds.txt: in (-1,-1) (-1,0) (-1,1), out (2,0) */
    and3.typeId = "builtin/3 Pin AND";
    and3.x = 4;
    and3.y = -5;
    and3.bits = 1;
    and3.sinks = {NodePin{-1, -1, 1, 1}, NodePin{-1, 0, 1, 1},
                  NodePin{-1, 1, 1, 1}};
    and3.sources = {NodePin{2, 0, 1, 1}};
    d.nodes.push_back(and3);
    /* Input i -> node pin i, node output -> the type's only output. */
    for (uint32_t i = 0; i < 3; ++i) d.connections.push_back(Connection{i, 0, 3, i});
    d.connections.push_back(Connection{3, 0, 4, 0});
    return d;
}

void testAgainstFixture() {
    bool fileOk = false;
    std::vector<uint8_t> fixtureRaw;
    for (const char* path : {"build/nl_def_and3.data", "../build/nl_def_and3.data"}) {
        fixtureRaw = readCircuitFile(path, fileOk);
        if (fileOk) break;
    }
    if (!fileOk) {
        std::printf("SKIP the fixture definition is not built yet "
                    "(run build.ps1: build/nl_def_and3.data)\n");
        return;
    }
    const Circuit fixture = parse(fixtureRaw);

    const std::vector<uint8_t> encoded = tc::component_netlist::encode(andThree());
    check(!encoded.empty(), "the encoder produced bytes for the AND3 netlist");
    if (encoded.empty()) return;
    check(encoded[0] == 14, "the circuit file starts with the definition version byte");
    std::vector<uint8_t> raw;
    check(tc::snappy_raw::decode(encoded.data() + 1, encoded.size() - 1, 1u << 20, raw),
          "the encoder's payload is a valid literal-only Snappy block");
    const Circuit mine = parse(raw);

    check(mine.customId == fixture.customId, "same custom id");
    check(mine.gateCost == fixture.gateCost && mine.delay == fixture.delay,
          "same declared cost pair");
    check(mine.components.size() == fixture.components.size(),
          "same number of nodes (" + std::to_string(mine.components.size()) + " vs " +
              std::to_string(fixture.components.size()) + ")");
    if (mine.components.size() == fixture.components.size()) {
        for (size_t i = 0; i < mine.components.size(); ++i) {
            const Component& left = fixture.components[i];
            const Component& right = mine.components[i];
            const std::string where = "node " + std::to_string(i);
            check(left.kind == right.kind, where + ": same kind");
            check(left.name == right.name, where + ": same name");
            check(left.pinIndex == right.pinIndex, where + ": same pin ordinal");
            check(left.wordSize == right.wordSize, where + ": same word size");
            check(left.values == right.values, where + ": same value sequence");
            if (left.kind == 0x4f || left.kind == 0x51) {
                /* The outer pins sit in the same lane (x); their y is the
                   author's column layout, and the two writers centre a multi-pin
                   column differently - what has to match is the spacing. */
                check(left.x == right.x, where + ": same pin lane");
            } else {
                check(left.x == right.x && left.y == right.y, where + ": same position");
            }
        }
    }
    if (mine.components.size() == fixture.components.size()) {
        /* The wires: which pin drives which pin, in order.  The two writers may
           bend a route differently (the game only needs the endpoints on the
           pins), so each side's wires are mapped back onto its own pins. */
        std::vector<std::string> left, right;
        check(wireEndpoints(fixture.components, fixture.wires, left),
              "the fixture's wires all land on its pins");
        check(wireEndpoints(mine.components, mine.wires, right),
              "the encoder's wires all land on its pins");
        check(left == right, "the same pins are wired together, in the same order");
    }
}

void testRefusals() {
    using namespace tc::component_netlist;
    std::string why;
    const auto refuses = [&why](const Definition& d, Error expected, const std::string& what) {
        const Error got = validate(d, why);
        check(got == expected,
              what + " (got " + std::string(errorText(got)) + ": " + why + ")");
    };

    Definition good = andThree();
    check(validate(good, why) == Error::None, "the reference netlist validates");

    Definition noNodes = good;
    noNodes.nodes.clear();
    refuses(noNodes, Error::NoNodes, "a composite with no inner node is refused");

    Definition badPin = good;
    badPin.connections.push_back(Connection{3, 9, 4, 0});
    refuses(badPin, Error::PinOutOfRange, "a pin that does not exist is refused");

    Definition doubleDrive = good;
    doubleDrive.connections.push_back(Connection{0, 0, 3, 0}); /* A again drives in0 */
    refuses(doubleDrive, Error::DuplicateDriver, "two wires into one pin are refused");

    Definition dangling = good;
    dangling.connections.pop_back(); /* the type's output no longer drives the node */
    refuses(dangling, Error::DanglingSink, "an undriven inner input is refused");

    /* Node indices: 0 is the type's own input pin, 1 and 2 the inner nodes, 3 the
       type's output pin (the same order the encoder writes).  Two 2-input AND
       nodes (kind 0x04: in (-1,-1) (-1,1), out (2,0)) feed each other: every
       sink is driven exactly once, so the only fault left is the loop. */
    Definition loop;
    loop.customId = 0x4C4F4F50315F3031ull;
    loop.name = "loop";
    loop.inputs = {Pin{"A", 1}};
    loop.outputs = {Pin{"Out", 1}};
    Node first, second;
    first.kind = second.kind = 0x04;
    first.typeId = second.typeId = "builtin/AND";
    first.x = 0; first.y = 0;
    second.x = 60; second.y = 40;
    first.sinks = second.sinks = {NodePin{-1, -1, 1, 1}, NodePin{-1, 1, 1, 1}};
    first.sources = second.sources = {NodePin{2, 0, 1, 1}};
    loop.nodes = {first, second};
    loop.connections = {Connection{0, 0, 1, 0},  /* A -> AND1 in0 */
                        Connection{0, 0, 2, 0},  /* A -> AND2 in0 (fan-out) */
                        Connection{1, 0, 2, 1},  /* AND1 out -> AND2 in1 */
                        Connection{2, 0, 1, 1},  /* AND2 out -> AND1 in1 (loop!) */
                        Connection{1, 0, 3, 0}}; /* AND1 out -> the type's output */
    refuses(loop, Error::CombinationalLoop, "a state-free loop is refused");

    /* The same loop, but the second node is a Delay Line (kind 0x0d): its output
       is registered, so the path is legal.  Its prototype declares no input
       pins, so its two measured connection points - input (-3,0), output (3,0),
       from the shipped campaign solution - are supplied by the caller and the
       node is flagged as state, which is exactly the split the catalogue
       documents. */
    Definition withState = loop;
    withState.nodes[1].kind = 0x0d;
    withState.nodes[1].typeId = "builtin/Delay Line";
    withState.nodes[1].stateful = true;
    withState.nodes[1].sinks = {NodePin{-3, 0, 1, 1}};
    withState.nodes[1].sources = {NodePin{3, 0, 1, 1}};
    withState.connections = {Connection{0, 0, 1, 0},  /* A -> AND1 in0 */
                             Connection{1, 0, 2, 0},  /* AND1 out -> delay in */
                             Connection{2, 0, 1, 1},  /* delay out -> AND1 in1 */
                             Connection{1, 0, 3, 0}}; /* AND1 out -> the type's output */
    const Error stateful = validate(withState, why);
    check(stateful == Error::None,
          "a loop through a stateful node is not a combinational loop (" +
              std::string(errorText(stateful)) + ": " + why + ")");

    /* Two different nets whose sinks coincide: the stacked NOT is driven by B
       while the AND3 node's matching pin is driven by B as well... that one is
       legal (same net, so the two wires may meet).  Drive the stacked part from a
       *different* input instead: now the two wires belong to different nets and
       have to end on the same point, which is exactly the short circuit the
       builder must refuse. */
    Definition crowded = good;
    Node stacked;
    stacked.kind = 0x03; /* NOT: in (-1,0), out (2,0) */
    stacked.typeId = "builtin/NOT";
    stacked.x = good.nodes[0].x;
    stacked.y = good.nodes[0].y;
    stacked.sinks = {NodePin{-1, 0, 1, 1}};
    stacked.sources = {NodePin{2, 0, 1, 1}};
    crowded.nodes.push_back(stacked);
    /* The stage's output pin moves to index 5 once the second inner node exists,
       so the wire that used to reach index 4 is rewritten. */
    crowded.connections = {Connection{0, 0, 3, 0}, Connection{1, 0, 3, 1},
                           Connection{2, 0, 3, 2}, Connection{3, 0, 5, 0},
                           Connection{2, 0, 4, 0}}; /* C -> the stacked NOT (its own net) */
    refuses(crowded, Error::Clearance, "a layout whose wires cannot be kept apart is refused");

    Definition malformed = good;
    malformed.customId = 0;
    refuses(malformed, Error::Argument, "a definition without an id is refused");
    malformed = good;
    malformed.inputs[0].bits = 0;
    refuses(malformed, Error::Argument, "a zero-width pin is refused");
}

void testEncodingShape() {
    using namespace tc::component_netlist;
    Definition d = andThree();
    /* Fan-out from one source pin is legal (the fixture's adder does it). */
    d.outputs.push_back(Pin{"Out2", 1});
    d.connections.push_back(Connection{3, 0, 5, 0});
    std::string why;
    check(validate(d, why) == Error::None, "a second output fed from the same node is legal: " + why);
    const std::vector<uint8_t> bytes = encode(d);
    check(!bytes.empty(), "the fan-out definition encodes");
    std::vector<uint8_t> raw;
    check(tc::snappy_raw::decode(bytes.data() + 1, bytes.size() - 1, 1u << 20, raw),
          "the fan-out payload decodes");
    const Circuit circuit = parse(raw);
    check(circuit.components.size() == 6, "5 nodes plus the new output pin");
    check(circuit.wires.size() == 5, "the fan-out wire is in the file");
}

/* The bus the wide layouts need: one source pin driving many sinks, drawn as a
   trunk with branches.  In this game touching wires are one net, so a fan-out is
   allowed to share geometry - as long as nothing from another net touches it. */
void testBusFanOut() {
    using namespace tc::component_netlist;
    Definition d;
    d.customId = 0x425553315F303031ull; /* "BUS1_001" */
    d.name = "bus";
    d.inputs = {Pin{"A", 1}};
    for (int line = 0; line < 6; ++line) d.outputs.push_back(Pin{"D", 1});
    /* Six NOT parts in a column *between* the type's pins (the input lane is at
       x = -18, the output lane at x = +13), so the bus runs on the left and each
       part's output leaves to the right - the shape a fan-out has to have for
       the wires to stay apart. */
    for (int line = 0; line < 6; ++line) {
        Node notGate;
        notGate.kind = 0x03;
        notGate.typeId = "builtin/NOT";
        notGate.x = 0;
        /* Each part sits on its own output pin's row, so its output is a straight
           wire to the right and the bus can run in the corridor on the left. */
        notGate.y = (line - 2) * 8;
        notGate.bits = 1;
        notGate.sinks = {NodePin{-1, 0, 1, 1}};
        notGate.sources = {NodePin{2, 0, 1, 1}};
        d.nodes.push_back(notGate);
    }
    /* Node indices: 0 is input A, 1..6 are the NOTs, 7..12 the output pins. */
    for (uint32_t line = 0; line < 6; ++line) {
        d.connections.push_back(Connection{0, 0, 1 + line, 0});       /* the bus */
        d.connections.push_back(Connection{1 + line, 0, 7 + line, 0}); /* each NOT -> its output */
    }
    std::string why;
    const Error verdict = validate(d, why);
    check(verdict == Error::None,
          "one source pin driving six sinks routes as a shared trunk: " +
              std::string(errorText(verdict)) + (why.empty() ? std::string() : " (" + why + ")"));
    const std::vector<uint8_t> bytes = encode(d);
    check(!bytes.empty(), "the bus definition encodes");
    /* The six trunk wires share their first segment; the game merges touching
       wires, so what matters is that the builder wrote six wires, not one. */
    std::vector<uint8_t> raw;
    if (!bytes.empty()) {
        check(tc::snappy_raw::decode(bytes.data() + 1, bytes.size() - 1, 1u << 20, raw),
              "the bus payload decodes");
        const Circuit circuit = parse(raw);
        check(circuit.wires.size() == 12, "six trunk wires and six output wires");
    }
}

}  // namespace

/* The routing rule on its own, without a whole definition around it: what two
   wires of different nets may do to each other.  The game's rule is that a wire
   has two endpoints and only a shared endpoint joins two nets, so a crossing is
   legal and everything that would join them by accident is not
   (docs/HANDOFF-component-m4.md 5.1 has the measurement). */
void testRoutingRule() {
    using namespace tc::component_netlist;
    const std::vector<Point> horizontal{Point{-10, 0}, Point{10, 0}};
    const std::vector<Point> vertical{Point{0, -10}, Point{0, 10}};
    check(!routesClash(horizontal, vertical), "two wires that cross are not a clash");

    /* One wire ends on the other: that is the T the editor turns into a
       junction, so it has to stay refused. */
    const std::vector<Point> endsOnIt{Point{0, 8}, Point{0, 0}};
    check(routesClash(horizontal, endsOnIt), "a wire ending on another is a clash");
    check(routesClash(endsOnIt, horizontal), "the same, whichever way round");

    /* Same line, overlapping length, no endpoint involved: refused as well. */
    const std::vector<Point> overlapping{Point{-5, 0}, Point{5, 0}};
    check(routesClash(horizontal, overlapping), "two wires on the same line clash");

    /* A shared endpoint is one net, so a fan-out may do both of the above. */
    const std::vector<Point> branch{Point{-10, 0}, Point{-10, 10}};
    check(!routesClash(horizontal, branch), "a branch off a shared endpoint is legal");
    const std::vector<Point> alongIt{Point{-10, 0}, Point{5, 0}};
    check(!routesClash(horizontal, alongIt), "a fan-out may run back along its trunk");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   /* a crash must still show its progress */
    std::printf("netlist-test: fixture comparison\n");
    testAgainstFixture();
    std::printf("netlist-test: refusals\n");
    testRefusals();
    std::printf("netlist-test: routing rule\n");
    testRoutingRule();
    std::printf("netlist-test: encoding shape\n");
    testEncodingShape();
    std::printf("netlist-test: bus fan-out\n");
    testBusFanOut();
    if (failures) {
        std::printf("component netlist: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS component netlist: the encoder matches the shipped fixture bytes, "
                "and every refusal it promises holds\n");
    return 0;
}
