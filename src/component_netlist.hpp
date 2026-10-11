#pragma once
/* The internal-netlist builder: model, validator and encoder (M4's second cut,
   the `tc.component.compose` V2 commit path).

   A composite component is a Mod type whose behaviour is *the game's own*
   circuit: instead of a callback, the definition carries nodes (built-in parts,
   addressed by the stable spelling `tc.component.compose` V1 hands out) and the
   wires between their pins.  The game compiles it exactly like a hand-drawn
   board, so nothing here invents a code path: the bytes this writes have the
   same shape `tests/and-component-fixture.cpp` has been writing (and the game
   has been compiling, simulating and scoring) since 2026-09.

   Two coordinate systems appear in one definition, and they are not the same:

     * an *outer* pin (kind 0x4f input / 0x51 output) is placed in "pin units",
       where the game draws it at `round(circuit / 8)` board cells - the loader's
       declarative path uses -18 / +13 for the historical two-cell lane, and a
       pin's connection point sits three units inside its edge (+3 / -3);
     * an *inner* node is placed in plain circuit units, one unit per board cell,
       and its pins sit at `node + the prototype's own board-cell offsets`.

   Measured, not assumed: the shipped three-input definition in
   tests/and-component-fixture.cpp puts a "3 Pin AND" (kind 0x05) at (4,-5) and
   wires its inputs to (3,-6), (3,-5) and (3,-4) - exactly the prototype offsets
   (-1,-1), (-1,0), (-1,1) - and its output to (6,-5) = (2,0).  The offline case
   (tests/component-netlist.cpp) compares this encoder against that fixture's own
   bytes, field by field. */
#include "builtin_catalogue.hpp"
#include "component_definition.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tc::component_netlist {

/* One pin of the type being built (an outer pin) or of an inner node.  `name`
   only matters for the type's own pins. */
struct Pin {
    std::string name;
    uint32_t bits = 1;
};

/* One inner node: a built-in part the game compiles.  `sinks` / `sources` are
   the part's own pin offsets in board cells - exactly what
   tc.component.compose reports - because that is the unit the inner wiring uses.
   `bits` is the width the node's ports use (the fixture writes 8 for its
   word-level placeholders). */
struct Node {
    uint32_t kind = 0;
    /* Non-zero: the node is another Mod type (kind 0x4e) instead of a built-in
       part, and the encoder writes the custom id into the record's extra tail -
       the nesting path. */
    uint64_t customId = 0;
    std::string typeId; /* the spelling it was resolved from, for diagnostics */
    int32_t x = 0, y = 0;
    uint32_t bits = 1;
    std::vector<builtin_catalogue::Pin> sinks, sources;
    /* A node whose kind declares no input pins is state (Delay Line, Register,
       ...): its output is registered, so a path through it is not a
       combinational loop.  This is the flag V1 reports, used for what it
       actually means rather than guessing at kinds. */
    bool stateful = false;
};

/* A wire between two pins of the definition.  Node indices address the whole
   node list: `[0, inputs)` are the type's own input pins (sources only), then
   the inner nodes, then the type's output pins (sinks only) - the same order
   the encoder writes them in.  `fromPin` indexes the source node's `sources`,
   `toPin` its `sinks`. */
struct Connection {
    uint32_t fromNode = 0, fromPin = 0, toNode = 0, toPin = 0;
};

struct Definition {
    uint64_t customId = 0;
    std::string name;
    /* The game recomputes the gate count from the inner circuit when it parses
       the definition; the delay is trusted verbatim (measured - see the fixture
       generator's header), so a composite has to declare its real critical
       path. */
    int64_t gateCost = 1, delay = 1;
    std::vector<Pin> inputs, outputs;
    std::vector<Node> nodes;
    std::vector<Connection> connections;
    /* How wide the part's own pin lane is, in board cells.  Two is the
       historical shape (-18 / +13, unchanged for every type registered before
       this parameter existed); a wider lane moves the input column left and the
       output column right, which is the room the inner circuit is laid out in.
       The declarative API has had the same knob (`pin_lane`) and the wide-lane
       wire positions were verified there, so a composite reuses it. */
    uint32_t pinLane = 2;

    uint32_t inputCount() const { return static_cast<uint32_t>(inputs.size()); }
    uint32_t outputCount() const { return static_cast<uint32_t>(outputs.size()); }
    uint32_t nodeCount() const {
        return inputCount() + static_cast<uint32_t>(nodes.size()) + outputCount();
    }
    uint32_t firstInner() const { return inputCount(); }
    uint32_t firstOutput() const {
        return inputCount() + static_cast<uint32_t>(nodes.size());
    }
};

enum class Error {
    None,
    Argument,        /* malformed definition (counts, widths, ranges) */
    NoNodes,         /* a composite with no inner node has nothing to run */
    PinOutOfRange,   /* a connection names a pin the node does not have */
    DuplicateDriver, /* two wires drive the same sink pin */
    DanglingSink,    /* an inner sink pin nothing drives */
    CombinationalLoop,
    Clearance,       /* a wire would cross another wire or run through a pin */
};

inline const char* errorText(Error error) {
    switch (error) {
        case Error::None: return "ok";
        case Error::Argument: return "the definition is malformed";
        case Error::NoNodes: return "a composite needs at least one inner node";
        case Error::PinOutOfRange: return "a connection names a pin that does not exist";
        case Error::DuplicateDriver: return "two wires drive the same pin";
        case Error::DanglingSink: return "an inner input pin is not driven";
        case Error::CombinationalLoop:
            return "the netlist has a combinational loop (a path with no state in it)";
        case Error::Clearance:
            return "the wire routing would cross another wire, run through a pin, or run through a part";
    }
    return "unknown";
}

/* How many pins of one node the builder can address: the same ceiling the
   catalogue reports for a prototype (`TC_BUILTIN_PIN_LIMIT`). */
inline constexpr uint32_t kMaxPinsPerNode = 32;
/* How many pins of one direction a composite's own outline may declare.  Unlike a
   bridged callback (whose arrays cap it at sixteen), a composite is a plain
   definition, and the game's own importer takes up to thirty-two pins per
   direction - measured, see docs/reference/limits.md «元件定义» and
   docs/research/custom-component-pins.md.  A five-bit decoder needs
   thirty-two outputs, so the builder has to allow what the game allows. */
inline constexpr uint32_t kMaxPinsPerDirection = 32;

/* Where an outer pin sits, and where its connection point is.  Same geometry as
   the declarative encoder, so a composite's pins land where every other Mod
   component's do and its design thumbnail has the same proportions. */
inline bool validPinLane(uint32_t lane) { return lane >= 1 && lane <= 16; }
inline int outerInputX(uint32_t lane) {
    return lane == 2 ? component_definition::kDefaultInputPinX : -8 * static_cast<int>(lane);
}
inline int outerOutputX(uint32_t lane) {
    return lane == 2 ? component_definition::kDefaultOutputPinX : 8 * static_cast<int>(lane);
}
inline int laneY(uint32_t index, uint32_t count) {
    /* Whole board cells apart, centred on the part: an odd count keeps the
       historical `i*8 - (n-1)*4` (unchanged for every type registered before
       this cut), an even count steps by (index - (count-1)/2) cells so no pin
       lands on a half cell. */
    return (static_cast<int>(index) - static_cast<int>((count - 1) / 2)) * 8;
}

struct Point {
    int x = 0, y = 0;
    bool operator==(const Point& other) const { return x == other.x && y == other.y; }
    bool operator!=(const Point& other) const { return !(*this == other); }
};

/* The point a wire has to touch to leave the given source pin. */
inline bool sourcePoint(const Definition& d, uint32_t node, uint32_t pin, Point& out) {
    if (node < d.inputCount()) {
        if (pin != 0) return false;
        out = Point{outerInputX(d.pinLane) + 3, laneY(node, d.inputCount())};
        return true;
    }
    if (node >= d.firstOutput()) return false; /* an output pin consumes */
    const Node& inner = d.nodes[node - d.firstInner()];
    if (pin >= inner.sources.size()) return false;
    out = Point{inner.x + inner.sources[pin].x, inner.y + inner.sources[pin].y};
    return true;
}

/* The point a wire has to touch to reach the given sink pin. */
inline bool sinkPoint(const Definition& d, uint32_t node, uint32_t pin, Point& out) {
    if (node >= d.firstOutput()) {
        const uint32_t index = node - d.firstOutput();
        if (pin != 0 || index >= d.outputs.size()) return false;
        out = Point{outerOutputX(d.pinLane) - 3, laneY(index, d.outputCount())};
        return true;
    }
    if (node < d.inputCount()) return false; /* an input pin drives */
    const Node& inner = d.nodes[node - d.firstInner()];
    if (pin >= inner.sinks.size()) return false;
    out = Point{inner.x + inner.sinks[pin].x, inner.y + inner.sinks[pin].y};
    return true;
}

inline bool onSegment(const Point& p, const Point& a, const Point& b) {
    if (a.x == b.x) return p.x == a.x && p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
    if (a.y == b.y) return p.y == a.y && p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x);
    return false;
}

/* True when two axis-aligned segments share at least one point.  Touching at a
   single point counts: in this game two wires that meet are one net. */
inline bool segmentsTouch(const Point& a, const Point& b, const Point& c, const Point& d) {
    const bool abVertical = a.x == b.x, cdVertical = c.x == d.x;
    if (abVertical && cdVertical) {
        if (a.x != c.x) return false;
        return !(std::max(a.y, b.y) < std::min(c.y, d.y) ||
                 std::max(c.y, d.y) < std::min(a.y, b.y));
    }
    if (!abVertical && !cdVertical) {
        if (a.y != c.y) return false;
        return !(std::max(a.x, b.x) < std::min(c.x, d.x) ||
                 std::max(c.x, d.x) < std::min(a.x, b.x));
    }
    const Point& v0 = abVertical ? a : c;
    const Point& v1 = abVertical ? b : d;
    const Point& h0 = abVertical ? c : a;
    const Point& h1 = abVertical ? d : b;
    return v0.x >= std::min(h0.x, h1.x) && v0.x <= std::max(h0.x, h1.x) &&
           h0.y >= std::min(v0.y, v1.y) && h0.y <= std::max(v0.y, v1.y);
}

/* Every point a wire is allowed to touch: the connection point of every pin of
   every node, plus the outer pins themselves. */
inline void collectPinPoints(const Definition& d, std::vector<Point>& out) {
    out.clear();
    for (uint32_t node = 0; node < d.nodeCount(); ++node) {
        Point point{};
        for (uint32_t pin = 0; sourcePoint(d, node, pin, point); ++pin) out.push_back(point);
        for (uint32_t pin = 0; sinkPoint(d, node, pin, point); ++pin) out.push_back(point);
    }
}

/* A route is a polyline of axis-aligned segments.  This is the plain two-segment
   shape; `viaTrunk` bends through a vertical gutter beside the sink, which is
   what the game's own files do when a node's input column is crowded. */
inline std::vector<Point> routeDirect(const Point& from, const Point& to) {
    std::vector<Point> points{from};
    if (from.x != to.x) points.push_back(Point{to.x, from.y});
    if (from.y != to.y) points.push_back(Point{to.x, to.y});
    if (points.size() == 1) points.push_back(to);
    return points;
}
inline std::vector<Point> routeViaTrunk(const Point& from, const Point& to, int trunkX) {
    std::vector<Point> points{from};
    if (from.x != trunkX) points.push_back(Point{trunkX, from.y});
    if (from.y != to.y || from.x != trunkX) points.push_back(Point{trunkX, to.y});
    if (trunkX != to.x) points.push_back(to);
    if (points.size() == 1) points.push_back(to);
    return points;
}
/* The same idea with the gutter horizontal instead of vertical: needed when the
   straight first step would run along the source node's own pin row (a Delay
   Line's measured input and output share a row, for instance). */
inline std::vector<Point> routeViaRow(const Point& from, const Point& to, int trunkY) {
    std::vector<Point> points{from};
    if (from.y != trunkY) points.push_back(Point{from.x, trunkY});
    if (from.x != to.x || from.y != trunkY) points.push_back(Point{to.x, trunkY});
    if (trunkY != to.y) points.push_back(to);
    if (points.size() == 1) points.push_back(to);
    return points;
}
/* The candidate routes, in the order the builder tries them: straight first,
   then gutters just outside the sink's own column (which keeps a multi-pin
   input column reachable), then gutters further out, then ones beside the
   source.  The first candidate that clears everything wins; the layout is the
   caller's, so a crowded netlist is refused rather than guessed at. */
inline std::vector<std::vector<Point>> candidateRoutes(const Point& from, const Point& to) {
    std::vector<std::vector<Point>> candidates;
    if (from.y == to.y) candidates.push_back(routeDirect(from, to));
    /* Gutters beside the destination, then beside the source, then further out:
       a wide layout needs a free channel somewhere between the two ends, and a
       small netlist is routed by the first candidate that fits. */
    static const int kOffsets[] = {1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 8, -8,
                                   10, -10, 12, -12, 16, -16, 20, -20, 24, -24, 32, -32,
                                   40, -40, 48, -48, 64, -64, 80, -80, 96, -96, 128, -128};
    for (int offset : kOffsets)
        candidates.push_back(routeViaTrunk(from, to, to.x + offset));
    for (int offset : kOffsets)
        candidates.push_back(routeViaTrunk(from, to, from.x + offset));
    for (int offset : kOffsets) {
        candidates.push_back(routeViaRow(from, to, from.y + offset));
        candidates.push_back(routeViaRow(from, to, to.y + offset));
    }
    candidates.push_back(routeDirect(from, to));
    return candidates;
}

/* True when a route runs through a pin point that is not one of its own ends. */
inline bool routeHitsPin(const std::vector<Point>& shape, const Point& from, const Point& to,
                         const std::vector<Point>& pins) {
    for (size_t step = 0; step + 1 < shape.size(); ++step)
        for (const Point& pin : pins) {
            if (pin == from || pin == to) continue;
            if (onSegment(pin, shape[step], shape[step + 1])) return true;
        }
    return false;
}

/* True when a point lies anywhere on a route, ends included. */
inline bool onRoute(const Point& point, const std::vector<Point>& route) {
    for (size_t i = 0; i + 1 < route.size(); ++i)
        if (onSegment(point, route[i], route[i + 1])) return true;
    return false;
}

/* The rectangle a node occupies: the bounding box of its own pins.  A pin sits
   on the edge of that box, so a wire is allowed to touch it and to stop on it -
   that is what reaching a pin is - but a wire running through the inside would
   be drawn across the part.  The first version of the wide layout did exactly
   that (the checker only looked at pins and at other wires), and it is not
   something a player could draw on a board either. */
struct Box {
    int minX = 0, maxX = 0, minY = 0, maxY = 0;
};

inline bool nodeBox(const Node& node, Box& out) {
    bool seen = false;
    const auto include = [&](int x, int y) {
        if (!seen) {
            out.minX = out.maxX = x;
            out.minY = out.maxY = y;
            seen = true;
            return;
        }
        out.minX = std::min(out.minX, x);
        out.maxX = std::max(out.maxX, x);
        out.minY = std::min(out.minY, y);
        out.maxY = std::max(out.maxY, y);
    };
    for (const builtin_catalogue::Pin& pin : node.sinks) include(node.x + pin.x, node.y + pin.y);
    for (const builtin_catalogue::Pin& pin : node.sources) include(node.x + pin.x, node.y + pin.y);
    return seen;
}

/* The box a route has to stay out of: the pin bounding box grown by one board
   cell's worth of clearance.  The pins sit on the part's own edge, but the game
   draws each part a little wider than the pins span, so a wire that only misses
   the pins can still be drawn straight across the part - which is what the first
   version of the wide layout did.  Two units are one board cell. */
inline constexpr int kPartClearance = 2;

inline Box partBox(const Node& node) {
    Box box;
    nodeBox(node, box);
    box.minX -= kPartClearance;
    box.maxX += kPartClearance;
    box.minY -= kPartClearance;
    box.maxY += kPartClearance;
    return box;
}

/* True when a segment has a piece strictly inside the box. */
inline bool segmentEntersBox(const Point& a, const Point& b, const Box& box) {
    if (a.x == b.x) {
        if (a.x <= box.minX || a.x >= box.maxX) return false;
        return std::min(a.y, b.y) < box.maxY && std::max(a.y, b.y) > box.minY;
    }
    if (a.y == b.y) {
        if (a.y <= box.minY || a.y >= box.maxY) return false;
        return std::min(a.x, b.x) < box.maxX && std::max(a.x, b.x) > box.minX;
    }
    return false;
}

/* A wire has to reach its pin, and the pin sits on the part's edge - so the
   piece of the route that ends on that pin is allowed to be inside the part's
   clearance box.  Every *other* piece inside it would be drawn across the part,
   which is what the first version of the wide layout did. */
inline bool routeCutsBox(const std::vector<Point>& route, const Box& box, const Point& ownPin,
                         bool hasOwnPin) {
    for (size_t i = 0; i + 1 < route.size(); ++i) {
        if (!segmentEntersBox(route[i], route[i + 1], box)) continue;
        if (hasOwnPin && (route[i] == ownPin || route[i + 1] == ownPin)) continue;
        return true;
    }
    return false;
}

/* True when two segments run along the same line and overlap over a length: not
   a crossing but two wires drawn on top of each other. */
inline bool segmentsOverlap(const Point& a, const Point& b, const Point& c, const Point& d) {
    const bool abVertical = a.x == b.x, cdVertical = c.x == d.x;
    if (abVertical != cdVertical) return false;
    if (abVertical) {
        if (a.x != c.x) return false;
        return std::min(std::max(a.y, b.y), std::max(c.y, d.y)) >
               std::max(std::min(a.y, b.y), std::min(c.y, d.y));
    }
    if (a.y != c.y) return false;
    return std::min(std::max(a.x, b.x), std::max(c.x, d.x)) >
           std::max(std::min(a.x, b.x), std::min(c.x, d.x));
}

/* When two routes of different nets may meet.

   A wire in this game has exactly two endpoints.  Placing an endpoint on a wire
   splits it, and the new endpoint belongs to both halves - that is what makes a
   fan-out, a trunk with branches, one net.  Two wires that merely *cross* are
   two nets when neither has an endpoint at the crossing, which is why a
   crossing is allowed here: refusing it would refuse layouts the game accepts
   and cannot tell apart from a wrong one (docs/HANDOFF-component-m4.md 5.1).

   What is refused is everything that would join two nets by accident:

     * an endpoint of one route lying anywhere on the other (the T shape the
       editor turns into one net when a player drops an endpoint on a wire);
     * two routes overlapping along a line - no endpoint is involved, so the
       game's answer for that case has not been measured, and two wires drawn on
       top of each other are almost always a layout mistake.

   Routes that leave the same source pin are one net by construction and may
   overlap and cross freely - that is what a bus is. */
inline bool routesClash(const std::vector<Point>& left, const std::vector<Point>& right) {
    if (left.front() == right.front()) return false; /* same net by construction */
    if (onRoute(left.front(), right) || onRoute(left.back(), right)) return true;
    if (onRoute(right.front(), left) || onRoute(right.back(), left)) return true;
    for (size_t i = 0; i + 1 < left.size(); ++i)
        for (size_t j = 0; j + 1 < right.size(); ++j)
            if (segmentsOverlap(left[i], left[i + 1], right[j], right[j + 1])) return true;
    return false;
}

/* The routing decision for a whole definition: one polyline per connection, in
   the connection order.  Deterministic, so validate() and encode() agree. */
struct Plan {
    std::vector<std::vector<Point>> routes;
    Error error = Error::None;
    std::string why;
};

inline Plan planRoutes(const Definition& d) {
    Plan plan;
    std::vector<Point> pins;
    collectPinPoints(d, pins);
    /* The parts are obstacles as well: a route never goes through a node. */
    std::vector<Box> boxes;
    std::vector<uint32_t> boxNode;
    for (uint32_t index = 0; index < d.nodes.size(); ++index) {
        boxes.push_back(partBox(d.nodes[index]));
        boxNode.push_back(d.firstInner() + index);
    }
    for (const Connection& c : d.connections) {
        Point from{}, to{};
        sourcePoint(d, c.fromNode, c.fromPin, from);
        sinkPoint(d, c.toNode, c.toPin, to);
        bool routed = false;
        for (const std::vector<Point>& candidate : candidateRoutes(from, to)) {
            if (candidate.size() < 2) continue;
            if (routeHitsPin(candidate, from, to, pins)) continue;
            bool clash = false;
            for (size_t index = 0; index < boxes.size(); ++index) {
                const bool own = boxNode[index] == c.fromNode || boxNode[index] == c.toNode;
                const Point ownPin = boxNode[index] == c.toNode ? to : from;
                if (routeCutsBox(candidate, boxes[index], ownPin, own)) { clash = true; break; }
            }
            if (clash) continue;
            for (const std::vector<Point>& taken : plan.routes)
                if (routesClash(candidate, taken)) { clash = true; break; }
            if (clash) continue;
            plan.routes.push_back(candidate);
            routed = true;
            break;
        }
        if (!routed) {
            plan.error = Error::Clearance;
            plan.why = "no clear wire from (" + std::to_string(from.x) + "," +
                       std::to_string(from.y) + ") to (" + std::to_string(to.x) + "," +
                       std::to_string(to.y) +
                       "); move the nodes apart (a wire may not cross another net's wire, run "
                       "through a pin, or cut through a part)";
            return plan;
        }
    }
    return plan;
}

/* Validates the shape, the wiring rules and the routing.  `why` receives a short
   explanation naming the offending node/pin, so a Mod author can fix the layout
   without guessing. */
inline Error validate(const Definition& d, std::string& why) {
    why.clear();
    if (!d.customId || d.name.empty()) return Error::Argument;
    if (d.inputs.empty() && d.outputs.empty()) return Error::Argument;
    if (!validPinLane(d.pinLane)) return Error::Argument;
    if (d.gateCost < 0 || d.delay < 0) return Error::Argument;
    if (d.nodes.empty()) return Error::NoNodes;
    if (d.inputs.size() > kMaxPinsPerDirection || d.outputs.size() > kMaxPinsPerDirection)
        return Error::Argument;
    for (const Pin& pin : d.inputs)
        if (pin.bits < 1 || pin.bits > 64) return Error::Argument;
    for (const Pin& pin : d.outputs)
        if (pin.bits < 1 || pin.bits > 64) return Error::Argument;
    for (const Node& node : d.nodes) {
        if (!node.kind || (node.sinks.empty() && node.sources.empty())) {
            why = "node " + node.typeId + " has no pin at all";
            return Error::Argument;
        }
        if (node.bits < 1 || node.bits > 64) return Error::Argument;
    }
    if (d.nodeCount() > 4096) return Error::Argument;

    /* Range and driver checks first, so the routing checks below can assume the
       pins exist. */
    std::vector<uint32_t> drivers(static_cast<size_t>(d.nodeCount()) * kMaxPinsPerNode, 0u);
    for (const Connection& c : d.connections) {
        Point from{}, to{};
        if (!sourcePoint(d, c.fromNode, c.fromPin, from)) {
            why = "wire leaves node " + std::to_string(c.fromNode) + " pin " +
                  std::to_string(c.fromPin) + ", which is not a source";
            return Error::PinOutOfRange;
        }
        if (!sinkPoint(d, c.toNode, c.toPin, to)) {
            why = "wire reaches node " + std::to_string(c.toNode) + " pin " +
                  std::to_string(c.toPin) + ", which is not a sink";
            return Error::PinOutOfRange;
        }
        if (from == to) {
            why = "a wire starts and ends on the same point";
            return Error::Argument;
        }
        uint32_t& slots = drivers[static_cast<size_t>(c.toNode) * kMaxPinsPerNode + c.toPin];
        if (++slots > 1) {
            why = "node " + std::to_string(c.toNode) + " pin " + std::to_string(c.toPin) +
                  " is driven twice";
            return Error::DuplicateDriver;
        }
    }
    /* Every sink has to be driven - an inner input and the type's own output
       pins alike.  An unconnected input is almost always a Mod author's mistake,
       and the game would silently read it as 0; an unconnected output would make
       the whole composite useless. */
    for (uint32_t index = 0; index < d.nodes.size(); ++index) {
        const Node& node = d.nodes[index];
        for (uint32_t pin = 0; pin < node.sinks.size(); ++pin)
            if (drivers[static_cast<size_t>(d.firstInner() + index) * kMaxPinsPerNode + pin] == 0) {
                why = "node " + node.typeId + " pin " + std::to_string(pin) +
                      " has no wire driving it";
                return Error::DanglingSink;
            }
    }
    for (uint32_t pin = 0; pin < d.outputCount(); ++pin)
        if (drivers[static_cast<size_t>(d.firstOutput() + pin) * kMaxPinsPerNode] == 0) {
            why = "the type's output pin " + std::to_string(pin) + " has no wire driving it";
            return Error::DanglingSink;
        }

    /* Combinational loops: walk the graph of inner nodes and refuse a cycle that
       contains no state.  A stateful node (a kind that declares no input pins)
       breaks it, which is exactly how the game's own Delay Line feedback works. */
    const uint32_t inner = static_cast<uint32_t>(d.nodes.size());
    std::vector<std::vector<uint32_t>> edges(inner);
    for (const Connection& c : d.connections) {
        if (c.fromNode < d.firstInner() || c.toNode < d.firstInner()) continue;
        if (c.toNode >= d.firstOutput()) continue;
        const uint32_t from = c.fromNode - d.firstInner();
        const uint32_t to = c.toNode - d.firstInner();
        if (d.nodes[from].stateful) continue; /* registered output: no loop through it */
        edges[from].push_back(to);
    }
    std::vector<int> colour(inner, 0); /* 0 unseen, 1 on stack, 2 done */
    std::vector<uint32_t> stack;
    const std::function<bool(uint32_t)> visit = [&](uint32_t node) -> bool {
        colour[node] = 1;
        stack.push_back(node);
        for (uint32_t next : edges[node]) {
            if (colour[next] == 1) {
                why = "loop through node " + d.nodes[next].typeId;
                return false;
            }
            if (colour[next] == 0 && !visit(next)) return false;
        }
        colour[node] = 2;
        stack.pop_back();
        return true;
    };
    for (uint32_t node = 0; node < inner; ++node)
        if (colour[node] == 0 && !visit(node)) return Error::CombinationalLoop;

    /* Routing last: every wire needs a clear path (see planRoutes). */
    const Plan plan = planRoutes(d);
    if (plan.error != Error::None) {
        why = plan.why;
        return plan.error;
    }
    return Error::None;
}

/* The circuit-file bytes: version 14, a literal-only Snappy payload - the same
   container `component_definition::encode` writes, so the component model's
   importer takes it unchanged.  Returns an empty vector when validate() refuses
   (the caller reports why from validate()'s own message). */
inline std::vector<uint8_t> encode(const Definition& d) {
    std::string why;
    if (validate(d, why) != Error::None) return {};
    component_definition::Writer w;
    w.number(d.customId, 8); w.number(0, 4);
    w.number(static_cast<uint64_t>(d.gateCost), 8);
    w.number(static_cast<uint64_t>(d.delay), 8);
    w.number(1, 1); w.number(10000, 8); w.number(0, 2); w.string("");
    w.number(0, 1); w.number(0, 2); w.number(0, 2); w.string("");
    for (int i = 0; i < 512; ++i) w.number(0, 1);

    const uint32_t inputs = d.inputCount(), outputs = d.outputCount();
    w.number(d.nodeCount(), 8);
    for (uint32_t i = 0; i < inputs; ++i)
        w.component(0x4f, outerInputX(d.pinLane), laneY(i, inputs), 0x1000000000000000ull + i,
                    d.inputs[i].name.c_str(), d.inputs[i].bits, static_cast<int>(i) + 1);
    for (uint32_t i = 0; i < d.nodes.size(); ++i) {
        const Node& node = d.nodes[i];
        w.component(node.kind, node.x, node.y, 0x2000000000000000ull + i, "", node.bits);
        /* A nested Mod type carries its custom id in the record's extra tail:
           the same layout tests/and-component-fixture.cpp writes for a `nested`
           node (kind 0x4e plus one i64 and one u16). */
        if (node.customId) { w.number(node.customId, 8); w.number(0, 2); }
    }
    for (uint32_t i = 0; i < outputs; ++i)
        w.component(0x51, outerOutputX(d.pinLane), laneY(i, outputs),
                    0x3000000000000000ull + i, d.outputs[i].name.c_str(),
                    d.outputs[i].bits, static_cast<int>(i) + 1);

    /* planRoutes() is deterministic, so the routes written here are the ones the
       validation just accepted. */
    const Plan plan = planRoutes(d);
    component_definition::Writer wires;
    uint32_t count = 0;
    for (size_t index = 0; index < d.connections.size(); ++index) {
        const std::vector<Point>& shape = plan.routes[index];
        ++count;
        wires.number(0, 1); wires.string("");
        wires.number(static_cast<uint64_t>(shape.front().x), 2);
        wires.number(static_cast<uint64_t>(shape.front().y), 2);
        for (size_t step = 0; step + 1 < shape.size(); ++step) {
            const int dx = shape[step + 1].x - shape[step].x;
            const int dy = shape[step + 1].y - shape[step].y;
            if (dx) wires.number((dx > 0 ? 0u : 0x8000u) | static_cast<uint32_t>(std::abs(dx)), 2);
            if (dy) wires.number((dy > 0 ? 0x4000u : 0xc000u) | static_cast<uint32_t>(std::abs(dy)), 2);
        }
        wires.number(0, 2);
    }
    w.number(count, 8);
    w.bytes.insert(w.bytes.end(), wires.bytes.begin(), wires.bytes.end());

    std::vector<uint8_t> result{14};
    size_t size = w.bytes.size();
    do {
        uint8_t byte = size & 127;
        size >>= 7;
        result.push_back(uint8_t(byte | (size ? 128 : 0)));
    } while (size);
    for (size_t position = 0; position < w.bytes.size();) {
        const size_t length = std::min<size_t>(60, w.bytes.size() - position);
        result.push_back(uint8_t((length - 1) << 2));
        result.insert(result.end(), w.bytes.begin() + position, w.bytes.begin() + position + length);
        position += length;
    }
    return result;
}

}  // namespace tc::component_netlist
