#pragma once
/* A five-input decoder built from the game's own parts - the M4 example.

   Five one-bit inputs (A0..A4) and thirty-two one-bit outputs (D0..D31): output
   i is high exactly when the input word equals i (a one-of-thirty-two decoder).
   Nothing here is a callback: the definition *is* the circuit, and the game
   compiles it like a hand-drawn board.  Two built-in parts are enough:

     * `0x2b` "1 Bit Decoder" - one bit in, two lines out (0-line and 1-line);
     * `0x04` "AND"           - two bits in, one out.

   The shape is the classic tree: stage k decodes input A(k-1) and ANDs its two
   lines with every line the previous stage produced, so stage 5 has the
   thirty-two one-hot lines.  A0's stage needs no AND at all (the two lines of
   its decoder *are* the first two outputs).

   Layout is part of the example, because the netlist builder has no auto-place:
   every line gets the row its final output has, so a line's block of outputs is
   centred on the line itself and the fan-out reads as a bus.  The builder draws
   those buses with shared trunks (same net, so touching is intended) and refuses
   a layout it cannot route cleanly.

   The refusal the builder prints for this layout is the builder's clearance rule
   being stricter than the game, not this circuit being impossible to draw: a
   wire here has two endpoints, an endpoint placed on a wire splits it into two
   wires sharing that endpoint (one net), and two wires that merely cross - no
   endpoint at the crossing - stay two nets.  Under that rule the layout below
   validates and encodes.

   What is still open is the *logic*: with the clearance rule relaxed, the
   offline truth table fails on 24 of the 32 patterns, because the line number
   here carries the newest bit in its least significant position while the type's
   output pins run in `laneY` order.  docs/HANDOFF-component-m4.md section 5.1
   has the measurement, the rule that has to be probed on the real machine, and
   the order to fix things in.

   The header is shared by the example plugin (which registers the netlist
   through `tc.component.compose` V2) and the offline test
   (tests/component-decoder.cpp), which simulates the same netlist for all
   thirty-two input patterns. */
#include "../../src/component_netlist.hpp"
#include <cstdint>

namespace composite_decoder {

using tc::component_netlist::Connection;
using tc::component_netlist::Definition;
using tc::component_netlist::Node;
using tc::component_netlist::Pin;
using NodePin = tc::builtin_catalogue::Pin;

/* The two parts this example uses, with the pins the pinned build reports for
   them (build/kinds.txt).  The plugin cross-checks these against
   `tc.component.compose` at load and complains in the log if they differ, so the
   numbers here cannot silently age. */
constexpr uint32_t kOneBitDecoder = 0x2b; /* in (-1,0); out (1,0) (1,1) */
constexpr uint32_t kAnd = 0x04;           /* in (-1,-1) (-1,1); out (2,0) */

inline void decoderPins(std::vector<NodePin>& sinks, std::vector<NodePin>& sources) {
    sinks = {NodePin{-1, 0, 1, 1}};
    sources = {NodePin{1, 0, 1, 1}, NodePin{1, 1, 1, 1}};
}
inline void andPins(std::vector<NodePin>& sinks, std::vector<NodePin>& sources) {
    sinks = {NodePin{-1, -1, 1, 1}, NodePin{-1, 1, 1, 1}};
    sources = {NodePin{2, 0, 1, 1}};
}

/* The row the final output `index` sits on - the same lane the builder gives the
   outer output pins, so the last stage's wires run straight across. */
inline int finalRow(uint32_t index, uint32_t count = 32) {
    return tc::component_netlist::laneY(index, count);
}
/* The row a stage-k line sits on: the middle of the block of final rows it
   feeds. */
inline int blockRow(uint32_t line, uint32_t stage) {
    const uint32_t block = 32u >> stage; /* rows this line feeds */
    const uint32_t first = line * block, last = first + block - 1;
    return (finalRow(first) + finalRow(last)) / 2;
}

/* Fills `d` with the decoder netlist.  `inputPins`/`outputPins` are the type's
   own pins in order; the caller supplies them so the same generator serves a
   differently named type. */
inline bool build(Definition& d, uint64_t customId, const char* name,
                  std::vector<Pin> inputs = {}, std::vector<Pin> outputs = {}) {
    if (inputs.empty()) {
        for (int bit = 0; bit < 5; ++bit) inputs.push_back(Pin{"A" + std::to_string(bit), 1});
    }
    if (outputs.empty()) {
        for (int line = 0; line < 32; ++line) outputs.push_back(Pin{"D" + std::to_string(line), 1});
    }
    if (inputs.size() != 5 || outputs.size() != 32) return false;
    d = Definition{};
    d.customId = customId;
    d.name = name;
    /* Declared statistics: the game recomputes the gate count from the netlist
       (32 two-input ANDs plus the decoders' own gates), and it trusts the delay,
       so the example declares the depth it actually has: five decoder stages,
       each one gate deep. */
    d.gateCost = 1;
    d.delay = 5;
    /* The wide lane: the inner circuit needs the room between the two pin
       columns (the two-cell default leaves it hanging outside the outline, which
       the routing tolerates but which is not what the type is for). */
    d.pinLane = 16;
    d.inputs = std::move(inputs);
    d.outputs = std::move(outputs);

    /* Column bands, left to right: stage k's decoders and ANDs.  With the wide
       lane the two pin columns sit at x = -125 and x = 125, and the whole tree
       has to live between them - the first version of this file laid it out at
       x = 64..290, which the game accepted but which put the circuit outside the
       outline (and outside the design thumbnail's frame). */
    const auto decoderX = [](uint32_t stage) {
        return -54 + 22 * static_cast<int>(stage - 1);
    };
    const auto andX = [&](uint32_t stage) {
        return decoderX(stage) + 11;
    };

    /* Stage 1..5.  `lines` holds the node index of the line that stage k's line
       `l` came from... rather, of the node that *drives* it: for stage 1 the two
       lines are driven by the decoder itself, so they are recorded as the
       decoder's node index plus which output pin drove them. */
    struct Line {
        uint32_t node = 0; /* absolute node index */
        uint32_t pin = 0;  /* source pin of that node */
    };
    std::vector<Line> previous; /* the lines of the stage before */
    for (uint32_t stage = 1; stage <= 5; ++stage) {
        Node decoder;
        decoder.kind = kOneBitDecoder;
        decoder.typeId = "builtin/1 Bit Decoder";
        decoder.x = decoderX(stage);
        decoder.y = 0; /* the middle of the band: its bus spreads both ways */
        decoder.bits = 1;
        decoderPins(decoder.sinks, decoder.sources);
        d.nodes.push_back(decoder);
        const uint32_t decoderIndex = d.firstInner() + static_cast<uint32_t>(d.nodes.size()) - 1;
        /* The decoder reads the stage's input bit.  Stage 1 builds the most
           significant bit of the line number (see the line index below), so it
           takes the *last* input pin: that is what makes line `i` the line whose
           input word is `i` with A0 as the least significant bit, which is the
           order the output pins run in.  Taking the pins in the other order
           builds the same tree with the bits reversed - the first version of
           this file did that, and its truth table lit the wrong line for 24 of
           the 32 patterns. */
        d.connections.push_back(Connection{5 - stage, 0, decoderIndex, 0});

        std::vector<Line> lines;
        if (stage == 1) {
            /* No AND: the decoder's two lines are the outputs. */
            lines.push_back(Line{decoderIndex, 0});
            lines.push_back(Line{decoderIndex, 1});
        } else {
            const uint32_t count = 1u << stage;
            lines.reserve(count);
            for (uint32_t line = 0; line < count; ++line) {
                Node gate;
                gate.kind = kAnd;
                gate.typeId = "builtin/AND";
                gate.x = andX(stage);
                gate.y = blockRow(line, stage);
                gate.bits = 1;
                andPins(gate.sinks, gate.sources);
                d.nodes.push_back(gate);
                const uint32_t gateIndex = d.firstInner() + static_cast<uint32_t>(d.nodes.size()) - 1;
                const Line from = previous[line / 2];
                d.connections.push_back(Connection{from.node, from.pin, gateIndex, 0});
                d.connections.push_back(Connection{decoderIndex, line % 2, gateIndex, 1});
                lines.push_back(Line{gateIndex, 0});
            }
        }
        previous = std::move(lines);
    }
    /* The thirty-two lines reach the type's own output pins, in order. */
    for (uint32_t line = 0; line < 32; ++line) {
        const uint32_t outputNode = d.firstOutput() + line;
        const Line from = previous[line];
        d.connections.push_back(Connection{from.node, from.pin, outputNode, 0});
    }
    return true;
}

/* How many two-input ANDs the netlist contains - the number the game's own
   parser is expected to count (plus whatever the decoders count as). */
inline uint32_t gateCount(const Definition& d) {
    uint32_t count = 0;
    for (const Node& node : d.nodes)
        if (node.kind == kAnd) ++count;
    return count;
}

/* The netlist's behaviour, evaluated straight from the definition: per-pin
   values, so a part with two outputs behaves correctly.  Only the two parts the
   example uses are modelled.

   `out0`/`out1` of the decoder are modelled as the 0-line and the 1-line - the
   reading the truth table below assumes, and the one the real-machine gate
   confirms.  If the pinned build's decoder turned out to be the other way round,
   that gate would report an inverted table and this model would flip with it. */
inline bool evaluate(const Definition& d, uint32_t inputBits, std::vector<uint8_t>& outputs) {
    const uint32_t nodes = d.nodeCount();
    std::vector<std::vector<uint8_t>> value(nodes);
    for (uint32_t node = 0; node < nodes; ++node) {
        const uint32_t width = node < d.inputCount() ? 1u
                              : node >= d.firstOutput() ? 0u
                              : static_cast<uint32_t>(d.nodes[node - d.firstInner()].sources.size());
        value[node].assign(width, 0u);
    }
    for (uint32_t bit = 0; bit < d.inputCount(); ++bit)
        value[bit][0] = static_cast<uint8_t>((inputBits >> bit) & 1u);

    /* The netlist is a feed-forward tree; nodeCount passes are more than enough
       for it to settle, and the loop is written so a mistake in the wiring shows
       up as an unstable answer rather than as a hang. */
    for (uint32_t pass = 0; pass < nodes + 1; ++pass) {
        for (const Node& node : d.nodes) {
            const uint32_t nodeIndex =
                d.firstInner() + static_cast<uint32_t>(&node - d.nodes.data());
            std::vector<uint8_t> sinks(node.sinks.size(), 0u);
            for (const Connection& c : d.connections) {
                if (c.toNode != nodeIndex || c.toPin >= sinks.size()) continue;
                const auto& source = value[c.fromNode];
                sinks[c.toPin] = c.fromPin < source.size() ? source[c.fromPin] : 0u;
            }
            if (node.kind == kAnd && sinks.size() == 2) {
                value[nodeIndex] = {static_cast<uint8_t>(sinks[0] && sinks[1])};
            } else if (node.kind == kOneBitDecoder && sinks.size() == 1) {
                value[nodeIndex] = {static_cast<uint8_t>(sinks[0] ? 0u : 1u),
                                    static_cast<uint8_t>(sinks[0] ? 1u : 0u)};
            }
        }
    }
    outputs.assign(d.outputCount(), 0u);
    for (const Connection& c : d.connections) {
        if (c.toNode < d.firstOutput()) continue;
        const uint32_t line = c.toNode - d.firstOutput();
        if (line >= outputs.size()) continue;
        const auto& source = value[c.fromNode];
        outputs[line] = c.fromPin < source.size() ? source[c.fromPin] : 0u;
    }
    return true;
}

}  // namespace composite_decoder
