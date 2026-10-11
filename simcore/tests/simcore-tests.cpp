/* The offline gate for simcore (docs/PLAN-sandbox-simulator.md section 7, S1).

   Everything here runs without the game, and it is deliberately not the last
   word: `--emit-verilog` writes the same circuits out for
   tools/simcore-iverilog.ps1, where iverilog decides whether the delays, the
   resolution and the oscillation period are right.  The unit tests pin down
   what a waveform cannot show - the resolution table itself, the scheduler's
   ordering rules, which violation was reported and why. */

#include "circuits.hpp"

#include "../include/tcsim/devices.hpp"
#include "../include/tcsim/engine.hpp"
#include "../include/tcsim/board.hpp"
#include "../include/tcsim/netlist.hpp"
#include "../include/tcsim/observe.hpp"
#include "../include/tcsim/runtime.hpp"
#include "../include/tcsim/scheduler.hpp"
#include "../include/tcsim/value.hpp"
#include "../include/tcsim/verilog.hpp"
#include "../../examples/sandbox-sim/cycle_unblock.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what, const std::string& detail = std::string()) {
    ++checks;
    if (condition) return;
    ++failures;
    std::printf("FAIL %s", what.c_str());
    if (!detail.empty()) std::printf(" -- %s", detail.c_str());
    std::printf("\n");
}

/* Builds an engine over a fixture's netlist with its stimulus loaded. */
tcsim::Engine engineFor(const tcsim::fixtures::Fixture& fixture, bool trace = false,
                        uint32_t max_rounds = 1000) {
    tcsim::EngineOptions options;
    options.trace = trace;
    options.max_delta_rounds = max_rounds;
    tcsim::Engine engine(fixture.list, options);
    for (const tcsim::StimulusEntry& entry : fixture.stimulus) engine.addStimulus(entry.when, entry.net, entry.value);
    return engine;
}

bool hasViolation(const tcsim::Engine& engine, tcsim::ViolationKind kind) {
    for (const tcsim::Violation& violation : engine.violations()) {
        if (violation.kind == kind) return true;
    }
    return false;
}

size_t violationCount(const tcsim::Engine& engine, tcsim::ViolationKind kind) {
    size_t count = 0;
    for (const tcsim::Violation& violation : engine.violations()) {
        if (violation.kind == kind) ++count;
    }
    return count;
}

void testValueSystem() {
    using tcsim::Drive;
    using tcsim::Logic;
    using tcsim::Strength;
    const auto resolve = [](std::vector<Drive> drives) {
        return tcsim::resolveBit(drives.data(), drives.size());
    };
    check(resolve({}).logic == Logic::kZ, "no driver resolves to Z");
    check(resolve({{Logic::kZ, Strength::kStrong}}).logic == Logic::kZ, "a high impedance driver does not drive");
    check(resolve({{Logic::kZero, Strength::kStrong}}).logic == Logic::kZero, "one strong 0 is 0");
    check(resolve({{Logic::kZero, Strength::kStrong}, {Logic::kOne, Strength::kStrong}}).logic == Logic::kX,
          "0 and 1 at the same strength resolve to X");
    check(resolve({{Logic::kZero, Strength::kWeak}, {Logic::kOne, Strength::kStrong}}).logic == Logic::kOne,
          "a strong driver beats a weak one");
    check(resolve({{Logic::kZero, Strength::kWeak}, {Logic::kOne, Strength::kWeak}}).logic == Logic::kX,
          "two weak drivers that disagree resolve to X");
    check(resolve({{Logic::kZero, Strength::kStrong}, {Logic::kZero, Strength::kWeak}}).logic == Logic::kZero,
          "two 0 drivers agree whatever their strength");
    check(resolve({{Logic::kX, Strength::kWeak}, {Logic::kZero, Strength::kStrong}}).logic == Logic::kZero,
          "a strong 0 beats a weak X");
    check(resolve({{Logic::kX, Strength::kStrong}, {Logic::kOne, Strength::kStrong}}).logic == Logic::kX,
          "a strong X ties with a strong 1");

    const tcsim::BitVector vector = tcsim::BitVector::fromText("10x1", 4);
    check(vector.size() == 4 && vector[0] == Logic::kOne && vector[1] == Logic::kZero && vector[2] == Logic::kX &&
              vector[3] == Logic::kOne,
          "bit vectors read MSB first and keep x");
    check(vector.toString() == "10x1", "a bit vector prints back as it was written");
    check(!vector.isKnown() && tcsim::BitVector::fromText("101", 3).isKnown(), "isKnown tells X and Z apart");
    check(tcsim::BitVector::fromText("1", 4).toString() == "1111", "a short literal extends with its own leftmost bit");
}

void testTruthTables() {
    using tcsim::Logic;
    const Logic values[4] = {Logic::kZero, Logic::kOne, Logic::kX, Logic::kZ};
    for (Logic left : values) {
        for (Logic right : values) {
            const Logic and_value = tcsim::logicAnd(left, right);
            const Logic or_value = tcsim::logicOr(left, right);
            if (left == Logic::kZero || right == Logic::kZero) {
                check(and_value == Logic::kZero, "AND with a 0 is 0");
            } else if (left == Logic::kOne && right == Logic::kOne) {
                check(and_value == Logic::kOne, "AND of two ones is one");
            } else {
                check(and_value == Logic::kX, "AND with anything unknown is X");
            }
            if (left == Logic::kOne || right == Logic::kOne) {
                check(or_value == Logic::kOne, "OR with a 1 is 1");
            } else if (left == Logic::kZero && right == Logic::kZero) {
                check(or_value == Logic::kZero, "OR of two zeros is zero");
            } else {
                check(or_value == Logic::kX, "OR with anything unknown is X");
            }
            const bool known = (left == Logic::kZero || left == Logic::kOne) &&
                               (right == Logic::kZero || right == Logic::kOne);
            const Logic xor_value = tcsim::logicXor(left, right);
            if (!known) {
                check(xor_value == Logic::kX, "XOR with anything unknown is X");
            } else {
                check(xor_value == (left == right ? Logic::kZero : Logic::kOne), "XOR is parity");
            }
        }
    }
    check(tcsim::logicNot(Logic::kZero) == Logic::kOne && tcsim::logicNot(Logic::kOne) == Logic::kZero &&
              tcsim::logicNot(Logic::kX) == Logic::kX && tcsim::logicNot(Logic::kZ) == Logic::kX,
          "NOT inverts 0 and 1 and turns Z into X");
    check(tcsim::logicMux(Logic::kOne, Logic::kZero, Logic::kOne) == Logic::kZero, "a mux passes the selected branch");
    check(tcsim::logicMux(Logic::kX, Logic::kOne, Logic::kOne) == Logic::kOne,
          "a mux with an unknown select still resolves matching branches");
    check(tcsim::logicMux(Logic::kX, Logic::kOne, Logic::kZero) == Logic::kX, "a mux with an unknown select is X");
}

void testScheduler() {
    tcsim::Scheduler scheduler(8);
    tcsim::Event first;
    first.when = 100; /* beyond the wheel: the overflow heap */
    first.layer = tcsim::Layer::kActive;
    first.device = 1;
    scheduler.push(first);
    for (int index = 0; index < 3; ++index) {
        tcsim::Event event;
        event.when = 5;
        event.layer = index == 2 ? tcsim::Layer::kNBA : tcsim::Layer::kActive;
        event.device = static_cast<uint32_t>(10 + index);
        scheduler.push(event);
    }
    tcsim::Event late;
    late.when = 5;
    late.layer = tcsim::Layer::kNBA;
    late.device = 99;
    scheduler.push(late);
    check(scheduler.nextWhen() == 5, "the earliest deadline wins even when it is in the wheel");
    const uint32_t first_device = scheduler.pop().device;
    const uint32_t second_device = scheduler.pop().device;
    const uint32_t third_device = scheduler.pop().device;
    check(first_device == 10 && second_device == 11,
          "same time and layer come out in insertion order");
    check(third_device == 12, "the NBA layer waits for every active event at that time");
    const tcsim::Event last = scheduler.pop();
    check(last.device == 99, "the second NBA event follows the first");
    check(scheduler.nextWhen() == 100, "the overflow heap holds what the wheel cannot");
    check(scheduler.pop().device == 1, "the far deadline arrives last");
    check(scheduler.empty(), "the scheduler is empty once every event has been popped");
    check(scheduler.clamped() == 0 && scheduler.dropped() == 0, "no deadline was rewritten or dropped");

    tcsim::Scheduler past(8);
    tcsim::Event early;
    early.when = 3;
    past.push(early);
    past.pop();
    tcsim::Event backwards;
    backwards.when = 1;
    past.push(backwards);
    check(past.clamped() == 1, "an event scheduled in the past is counted, not hidden");
}

void testDelayChain() {
    tcsim::fixtures::Fixture fixture = tcsim::fixtures::delayChain();
    tcsim::Engine engine = engineFor(fixture, true);
    engine.runUntil(25);
    check(engine.value("y1").toString() == "0", "a buffer settles after its own delay");
    check(engine.value("y2").toString() == "1", "the nand settles after its rising delay");
    engine.runUntil(45);
    check(engine.value("y1").toString() == "0", "a pulse narrower than the delay never reaches the output");
    engine.runUntil(55);
    check(engine.value("y1").toString() == "1", "a pulse wider than the delay does reach the output");
    engine.runUntil(75);
    check(engine.value("y1").toString() == "0", "the falling edge takes its own delay");
    check(engine.scheduler().clamped() == 0, "nothing was scheduled in the past");
    check(engine.violations().empty(), "a well formed circuit reports nothing");

    /* The narrow pulse left a pending update that a later evaluation cancelled:
       that is what makes it inertial rather than a queue that always lands. */
    check(engine.staleUpdates() >= 1, "the cancelled update was dropped as stale");
}

void testMultiDriver() {
    tcsim::fixtures::Fixture fixture = tcsim::fixtures::multiDriver();
    tcsim::Engine engine = engineFor(fixture);
    engine.runUntil(5);
    check(engine.value("m1").toString() == "x", "two drivers that disagree resolve the net to X");
    check(engine.value("m2").toString() == "1", "the strong driver wins over the weak one");
    check(hasViolation(engine, tcsim::ViolationKind::kMultiDriverConflict),
          "the conflict is reported, not just resolved");
    engine.runUntil(65);
    check(engine.value("m1").toString() == "0", "drivers that agree drive the net");
    engine.runUntil(85);
    check(engine.value("m2").toString() == "1", "the weak driver alone cannot pull the net down");
    engine.runUntil(105);
    check(engine.value("m2").toString() == "0", "the strong driver wins once it pulls low");
    check(engine.value("m3").toString() == "1", "a single driver holds the net");
    engine.runUntil(125);
    check(engine.value("m3").toString() == "z", "a driver that turns off leaves the net floating");
}

void testTriStateSwitch() {
    tcsim::NetList list;
    const tcsim::NetId enable_a = list.addNet("switch_enable_a", 1);
    const tcsim::NetId data_a = list.addNet("switch_data_a", 4);
    const tcsim::NetId enable_b = list.addNet("switch_enable_b", 1);
    const tcsim::NetId data_b = list.addNet("switch_data_b", 4);
    const tcsim::NetId bus = list.addNet("switch_bus", 4);
    const tcsim::DeviceSpec& spec = tcsim::deviceTriState(4);
    const tcsim::DeviceId a = list.addDevice(&spec, "switch_a");
    const tcsim::DeviceId b = list.addDevice(&spec, "switch_b");
    list.connect(a, "S", enable_a);
    list.connect(a, "A", data_a);
    list.connect(a, "Y", bus);
    list.connect(b, "S", enable_b);
    list.connect(b, "A", data_b);
    list.connect(b, "Y", bus);

    tcsim::Engine engine(list, tcsim::EngineOptions{});
    engine.addStimulus(0, enable_a, tcsim::BitVector::fromText("0", 1));
    engine.addStimulus(0, data_a, tcsim::BitVector::fromText("1010", 4));
    engine.addStimulus(0, enable_b, tcsim::BitVector::fromText("0", 1));
    engine.addStimulus(0, data_b, tcsim::BitVector::fromText("0101", 4));
    engine.addStimulus(10, enable_a, tcsim::BitVector::fromText("1", 1));
    engine.addStimulus(20, enable_b, tcsim::BitVector::fromText("1", 1));
    engine.addStimulus(30, enable_a, tcsim::BitVector::fromText("0", 1));
    engine.runUntil(5);
    check(engine.value("switch_bus").toString() == "zzzz", "disabled game switches drive high impedance");
    engine.runUntil(15);
    check(engine.value("switch_bus").toString() == "1010", "an enabled word switch passes every data bit");
    engine.runUntil(25);
    check(engine.value("switch_bus").toString() == "xxxx", "two enabled switches still use normal bus resolution");
    engine.runUntil(35);
    check(engine.value("switch_bus").toString() == "0101", "disabling one switch leaves the other driver intact");

    tcsim::BoardView board;
    tcsim::BoardComponent component;
    component.kind = 0x19;
    component.input_count = 2;
    component.output_count = 1;
    component.memory_bits = 32;
    board.components.push_back(component);
    board.wires.push_back(tcsim::BoardWire{0, 1, 0, 3, 1, 100});
    board.wires.push_back(tcsim::BoardWire{-1, 0, -3, 0, 32, 102});
    board.wires.push_back(tcsim::BoardWire{2, 0, 4, 0, 32, 106});
    const auto geometry = [](const tcsim::BoardComponent& candidate, tcsim::KindPins& pins) {
        if (candidate.kind != 0x19) return false;
        pins = tcsim::KindPins{};
        pins.kind = candidate.kind;
        pins.name = "Switch";
        pins.input_count = 2;
        pins.output_count = 1;
        pins.in_x[0] = 0;
        pins.in_y[0] = 1;
        pins.in_x[1] = -1;
        pins.out_x[0] = 2;
        return true;
    };
    const tcsim::BuildResult built = tcsim::buildNetList(board, geometry);
    check(!built.fatal() && built.netlist.deviceCount() == 1,
          "the game's 0x19 Word Switch builds as a supported device");
    check(!built.netlist.devices().empty() && built.netlist.devices()[0].spec->inputs[1].bits == 32 &&
              built.netlist.devices()[0].spec->outputs[0].bits == 32,
          "the board builder preserves the game's resolved word-switch width");
}

/* The game's word Mux (kind 0x2a), at both layers.  The device level pins the
   select/data widths and the three phases of the truth table the `byte_mux`
   level is built on; the board level pins that the builder gives the mux the
   width the record resolved instead of leaving it one bit wide. */
void testMux() {
    const tcsim::DeviceSpec& spec = tcsim::deviceMux(4, tcsim::ArcDelay{5, 5});
    check(spec.name == "mux4" && spec.inputs.size() == 3 && spec.inputs[0].bits == 1 &&
              spec.inputs[1].bits == 4 && spec.inputs[2].bits == 4 && spec.outputs[0].bits == 4,
          "the word mux keeps the select at one bit and both data pins at the bus width");
    check(&tcsim::deviceMux2() == &tcsim::deviceMux(1) && tcsim::builtinDevice("mux2") == &tcsim::deviceMux(1),
          "the catalogue's mux2 is the one-bit member of the same family");

    tcsim::NetList list;
    const tcsim::NetId select = list.addNet("mux_sel");
    const tcsim::NetId a = list.addNet("mux_a", 4);
    const tcsim::NetId b = list.addNet("mux_b", 4);
    const tcsim::NetId y = list.addNet("mux_y", 4);
    const tcsim::DeviceId mux = list.addDevice(&spec, "u_mux");
    list.connect(mux, "S", select);
    list.connect(mux, "A", a);
    list.connect(mux, "B", b);
    list.connect(mux, "Y", y);
    tcsim::Engine engine(list);
    engine.addStimulus(0, select, tcsim::fixtures::one("0"));
    engine.addStimulus(0, a, tcsim::BitVector::fromText("1010", 4));
    engine.addStimulus(0, b, tcsim::BitVector::fromText("1100", 4));
    engine.addStimulus(20, select, tcsim::fixtures::one("1"));
    engine.addStimulus(40, select, tcsim::fixtures::one("x"));
    engine.addStimulus(40, b, tcsim::BitVector::fromText("1110", 4));
    engine.addStimulus(60, select, tcsim::fixtures::one("0"));
    engine.addStimulus(80, a, tcsim::BitVector::fromText("1111", 4));
    engine.runUntil(15);
    check(engine.value("mux_y").toString() == "1010", "S=0 passes A down the whole bus");
    engine.runUntil(35);
    check(engine.value("mux_y").toString() == "1100", "S=1 passes B");
    engine.runUntil(55);
    check(engine.value("mux_y").toString() == "1x10",
          "an unknown select keeps the bit positions the two branches agree on");
    engine.runUntil(75);
    check(engine.value("mux_y").toString() == "1010", "a settled select takes its branch again");
    engine.runUntil(95);
    check(engine.value("mux_y").toString() == "1111", "and it follows a change on that branch");
    check(!engine.aborted() && engine.violations().empty(), "a mux on a settled board reports nothing");

    /* Board level: a 0x2a record whose `+0xe0` carries the width the game
       resolved (a live board's 32-bit mux does), and the same record with that
       field empty so the width has to come off the data wires. */
    const auto build_mux_board = [](uint16_t resolved_bits, uint16_t data_bits) {
        tcsim::BoardView board;
        tcsim::BoardComponent component;
        component.kind = 0x2a;
        component.x = 4;
        component.input_count = 3;
        component.output_count = 1;
        component.memory_bits = resolved_bits;
        board.components.push_back(component);
        /* Select (one bit), A, B and the output, all ending on the mux's pins. */
        board.wires.push_back(tcsim::BoardWire{3, -1, 0, -1, 1, 256});
        board.wires.push_back(tcsim::BoardWire{3, 0, 0, 0, data_bits, 258});
        board.wires.push_back(tcsim::BoardWire{3, 1, 0, 1, data_bits, 260});
        board.wires.push_back(tcsim::BoardWire{6, 0, 9, 0, data_bits, 262});
        const auto geometry = [](const tcsim::BoardComponent& candidate, tcsim::KindPins& pins) {
            if (candidate.kind != 0x2a) return false;
            pins = tcsim::KindPins{};
            pins.kind = candidate.kind;
            pins.name = "Mux";
            pins.input_count = 3;
            pins.output_count = 1;
            pins.in_x[0] = pins.in_x[1] = pins.in_x[2] = -1;
            pins.in_y[0] = -1;
            pins.in_y[1] = 0;
            pins.in_y[2] = 1;
            pins.in_bits[0] = 1;
            pins.in_bits[1] = pins.in_bits[2] = 8;
            pins.out_x[0] = 2;
            pins.out_bits[0] = 8;
            return true;
        };
        return tcsim::buildNetList(board, geometry);
    };

    const tcsim::BuildResult resolved = build_mux_board(8, 8);
    check(!resolved.fatal() && resolved.netlist.deviceCount() == 1,
          "the game's 0x2a Mux builds as a supported device");
    const tcsim::DeviceInstance& device = resolved.netlist.devices()[0];
    check(device.spec->name == "mux8" && device.spec->inputs[0].bits == 1 && device.spec->inputs[1].bits == 8 &&
              device.spec->outputs[0].bits == 8,
          "the built mux takes the width the record resolved and keeps the select at one bit");
    check(device.inputs[0] == resolved.net_of_wire[0] && device.inputs[1] == resolved.net_of_wire[1] &&
              device.inputs[2] == resolved.net_of_wire[2] && device.outputs[0] == resolved.net_of_wire[3],
          "its pins are wired select, A, B and output, in the catalogue's order");

    tcsim::Engine built_engine(resolved.netlist);
    built_engine.addStimulus(0, resolved.net_of_wire[0], tcsim::fixtures::one("0"));
    built_engine.addStimulus(0, resolved.net_of_wire[1], tcsim::BitVector::fromText("10100101", 8));
    built_engine.addStimulus(0, resolved.net_of_wire[2], tcsim::BitVector::fromText("11000011", 8));
    built_engine.runUntil(10);
    check(built_engine.netlist().net(resolved.net_of_wire[3]).value == tcsim::BitVector::fromText("10100101", 8),
          "a board-built mux passes A while its select is low");
    built_engine.addStimulus(20, resolved.net_of_wire[0], tcsim::fixtures::one("1"));
    built_engine.runUntil(30);
    check(built_engine.netlist().net(resolved.net_of_wire[3]).value ==
              tcsim::BitVector::fromText("11000011", 8),
          "and B once the select goes high");

    const tcsim::BuildResult recovered = build_mux_board(0, 8);
    check(!recovered.fatal() && recovered.netlist.deviceCount() == 1 &&
              recovered.netlist.devices()[0].spec->name == "mux8",
          "a record with no resolved width recovers it from the data wires");
    const tcsim::BuildResult one_bit = build_mux_board(0, 1);
    check(one_bit.netlist.devices()[0].spec->name == "mux1",
          "a mux whose data wires are one bit wide stays a one-bit mux");
    const tcsim::BuildResult widened = build_mux_board(1, 8);
    check(widened.netlist.devices()[0].spec->name == "mux8",
          "a wider data wire is never truncated to a narrower record field");
}

/* The game's word operations (docs/research/simcore-kind-coverage.md).  These
   are the kinds a player's board is full of the moment it leaves the gate
   levels, so every one of them gets a row here: the operation itself, the
   four-state rule where an operand has not settled, and the facts a wrong guess
   would hide (division by zero, a carry in, a shift amount wider than the
   word). */
void testWordOps() {
    using tcsim::BitVector;
    using tcsim::DeviceSpec;

    /* Runs one device's behavior directly: these tests are about the truth
       table, and the engine's own scheduling is covered elsewhere. */
    const auto apply = [](const DeviceSpec& spec, const std::vector<BitVector>& inputs) {
        std::vector<BitVector> outputs;
        tcsim::EvalContext context;
        spec.behavior(context, inputs, outputs);
        std::string text;
        for (const BitVector& output : outputs) {
            if (!text.empty()) text += " ";
            text += output.toString();
        }
        return text;
    };
    const auto word = [](const char* text, size_t bits) { return BitVector::fromText(text, bits); };
    const auto single = [](const char* text) { return BitVector::fromText(text, 1); };

    /* One Add, 4 bits wide, carry in and out: kind 0x1e's own shape. */
    const DeviceSpec& add = tcsim::deviceWordBinary(tcsim::WordOp::kAdd, 4, 4);
    check(add.name == "wordbinary0_4" && add.inputs.size() == 3 && add.outputs.size() == 2 &&
              add.inputs[0].bits == 4 && add.inputs[1].bits == 4 && add.inputs[2].bits == 1 &&
              add.outputs[0].bits == 4 && add.outputs[1].bits == 1,
          "Add carries a one-bit carry in and out beside the 4-bit data path");
    check(apply(add, {word("1010", 4), word("0110", 4), single("0")}) == "0000 1",
          "10 + 6 wraps to 0 with the carry out set at 4 bits");
    check(apply(add, {word("0010", 4), word("0011", 4), single("1")}) == "0110 0",
          "the carry in is added: 2 + 3 + 1 = 6");
    check(apply(add, {word("1111", 4), word("0001", 4), single("0")}) == "0000 1",
          "15 + 1 carries out of a 4-bit adder");
    check(apply(add, {word("0001", 4), word("0000", 4), single("x")}) == "xxxx x",
          "an unknown carry in poisons the whole word, which is what the game's own C does", "got " +
          apply(add, {word("0001", 4), word("0000", 4), single("x")}));

    /* The comparisons: one bit out, and X only where the orderings disagree. */
    const DeviceSpec& equal = tcsim::deviceWordBinary(tcsim::WordOp::kEqual, 4, 4);
    check(apply(equal, {word("1010", 4), word("1010", 4)}) == "1", "Equal is 1 on equal words");
    check(apply(equal, {word("1010", 4), word("1011", 4)}) == "0", "Equal is 0 on different words");
    check(apply(equal, {word("101x", 4), word("1010", 4)}) == "x",
          "Equal is X while an unknown bit could still change the answer");
    check(apply(equal, {word("1011", 4), word("101x", 4)}) == "x",
          "Equal is X when a bit of either operand has not settled",
          "got " + apply(equal, {word("1011", 4), word("101x", 4)}));
    const DeviceSpec& less_unsigned = tcsim::deviceWordBinary(tcsim::WordOp::kLessUnsigned, 4, 4);
    check(apply(less_unsigned, {word("0011", 4), word("0100", 4)}) == "1", "unsigned < on 3 vs 4");
    check(apply(less_unsigned, {word("1111", 4), word("0000", 4)}) == "0", "unsigned 15 is not < 0");
    const DeviceSpec& less_signed = tcsim::deviceWordBinary(tcsim::WordOp::kLessSigned, 4, 4);
    check(apply(less_signed, {word("1111", 4), word("0000", 4)}) == "1", "signed -1 is < 0");
    check(apply(less_signed, {word("0111", 4), word("1000", 4)}) == "0", "signed 7 is not < -8");

    /* Unary: Negate, Increment, NOT, and the two counts. */
    check(apply(tcsim::deviceWordUnary(tcsim::WordOp::kNegate, 4), {word("0001", 4)}) == "1111",
          "Negate is two's complement: -1 in 4 bits");
    check(apply(tcsim::deviceWordUnary(tcsim::WordOp::kIncrement, 4), {word("1111", 4)}) == "0000",
          "Increment wraps at the declared width");
    check(apply(tcsim::deviceWordUnary(tcsim::WordOp::kNot, 4), {word("1010", 4)}) == "0101",
          "the word NOT inverts every bit",
          "got " + apply(tcsim::deviceWordUnary(tcsim::WordOp::kNot, 4), {word("1010", 4)}));
    check(apply(tcsim::deviceWordUnary(tcsim::WordOp::kNot, 4), {word("10x0", 4)}) == "xxxx",
          "the word NOT poisons the whole word too: it is one C operation on a word");
    check(apply(tcsim::deviceWordUnary(tcsim::WordOp::kCountLeadingZero, 4), {word("0010", 4)}) == "0010",
          "count leading zeroes of 0010 is 2");
    check(apply(tcsim::deviceWordUnary(tcsim::WordOp::kCountLeadingZero, 4), {word("0000", 4)}) == "0100",
          "an all-zero word has as many leading zeroes as it has bits");
    check(apply(tcsim::deviceWordUnary(tcsim::WordOp::kCountTrailingZero, 4), {word("0100", 4)}) == "0010",
          "count trailing zeroes of 0100 is 2");

    /* The shifts and rotates.  A shift amount at the word's own width is the
       corner that separates the two families. */
    const DeviceSpec& shl = tcsim::deviceWordBinary(tcsim::WordOp::kShiftLeft, 4, 2);
    check(apply(shl, {word("0011", 4), word("01", 2)}) == "0110", "shift left by 1");
    check(apply(shl, {word("0011", 4), word("11", 2)}) == "1000", "shift left by 3");
    check(apply(shl, {word("0011", 4), word("00", 2)}) == "0011", "shift left by 0 keeps the word");
    const DeviceSpec& rol = tcsim::deviceWordBinary(tcsim::WordOp::kRotateLeft, 4, 2);
    check(apply(rol, {word("1001", 4), word("01", 2)}) == "0011", "rotate left by 1 wraps the top bit round");
    check(apply(rol, {word("1001", 4), word("00", 2)}) == "1001", "rotate left by 0 is the identity");
    const DeviceSpec& asr = tcsim::deviceWordBinary(tcsim::WordOp::kArithmeticShiftRight, 4, 2);
    check(apply(asr, {word("1000", 4), word("01", 2)}) == "1100", "arithmetic shift right copies the sign bit");
    check(apply(asr, {word("0100", 4), word("10", 2)}) == "0001", "arithmetic shift right of a positive word");

    /* Multiply, Divide and Modulo.  Division by zero is worth pinning because
       the level's own test names it; see the coverage note for what is still
       unmeasured about the signed readings. */
    const DeviceSpec& multiply = tcsim::deviceWordBinary(tcsim::WordOp::kMultiply, 4, 4);
    check(apply(multiply, {word("0011", 4), word("0100", 4)}) == "1100", "3 x 4 = 12");
    check(apply(multiply, {word("1111", 4), word("0010", 4)}) == "1110", "15 x 2 wraps to 14 in 4 bits");
    const DeviceSpec& divide = tcsim::deviceWordBinary(tcsim::WordOp::kDivide, 4, 4);
    check(apply(divide, {word("1100", 4), word("0011", 4)}) == "0100", "12 / 3 = 4");
    check(apply(divide, {word("0000", 4), word("0000", 4)}) == "1111",
          "division by zero is all ones, which is what the byte_divide level's test describes");
    const DeviceSpec& modulo = tcsim::deviceWordBinary(tcsim::WordOp::kModulo, 4, 4);
    check(apply(modulo, {word("1101", 4), word("0101", 4)}) == "0011", "13 mod 5 = 3");

    /* The word-wide bitwise gates.  Known operands are per-bit, exactly like
       the gate of the same name. */
    const DeviceSpec& and_word = tcsim::deviceWordBinary(tcsim::WordOp::kAnd, 4, 4);
    check(apply(and_word, {word("1010", 4), word("1100", 4)}) == "1000", "the word AND is per bit",
          "got " + apply(and_word, {word("1010", 4), word("1100", 4)}));
    check(apply(and_word, {word("1010", 4), word("x100", 4)}) == "xxxx",
          "the word AND is still one C operation on a word: an unknown bit poisons it");
    const DeviceSpec& or_word = tcsim::deviceWordBinary(tcsim::WordOp::kOr, 4, 4);
    check(apply(or_word, {word("1010", 4), word("0101", 4)}) == "1111", "the word OR is per bit");
    const DeviceSpec& xor_word = tcsim::deviceWordBinary(tcsim::WordOp::kXor, 4, 4);
    check(apply(xor_word, {word("1111", 4), word("0000", 4)}) == "1111", "the word XOR is per bit");

    /* The 2-in Concatenator: the first pin is the high half. */
    const DeviceSpec& concat = tcsim::deviceConcat(4, 2);
    check(concat.inputs[0].bits == 4 && concat.inputs[1].bits == 2 && concat.outputs[0].bits == 6,
          "the concatenator's result is as wide as its two pins together");
    check(apply(concat, {word("1010", 4), word("11", 2)}) == "101011", "in0 lands in the high bits");

    /* The library publishes the new names, so a caller can ask before it
       builds. */
    const std::vector<std::string> names = tcsim::builtinWordDeviceNames();
    check(std::find(names.begin(), names.end(), "addW") != names.end() &&
              std::find(names.begin(), names.end(), "mulW") != names.end(),
          "the word operation names are enumerable");
    check(tcsim::builtinWordDevice("addW", 8, 8) != nullptr && tcsim::builtinWordDevice("nope", 8, 8) == nullptr,
          "an unknown word name is null, never a guess");
}

/* The game's Screen (kind 0x5b) and the two host peripherals beside it.  The
   settings latched by its three pins and the pixels those settings describe are
   both taken from the game's own files - its manual page, the level's ISA and
   the prototype table - so none of this is a guess; see screen.hpp. */
void testScreen() {
    const auto bits = [](uint64_t value, uint16_t width) {
        std::string text;
        for (int position = static_cast<int>(width) - 1; position >= 0; --position) {
            text.push_back(((value >> position) & 1u) ? '1' : '0');
        }
        return tcsim::BitVector::fromText(text, width);
    };

    /* Device level: the three pins, and nothing driven back. */
    const std::shared_ptr<tcsim::ScreenSettings> settings = std::make_shared<tcsim::ScreenSettings>();
    const std::unique_ptr<tcsim::DeviceSpec> spec = tcsim::screenSpec(settings, tcsim::ArcDelay{0, 0});
    check(spec->inputs.size() == 3 && spec->inputs[0].bits == 1 && spec->inputs[1].bits == 8 &&
              spec->inputs[2].bits == 64 && spec->outputs.empty(),
          "the screen keeps the catalogue's Enable/Setting/Value pins and drives nothing");
    tcsim::NetList list;
    const tcsim::NetId enable = list.addNet("screen_enable");
    const tcsim::NetId index = list.addNet("screen_index", 8);
    const tcsim::NetId value = list.addNet("screen_value", 64);
    const tcsim::DeviceId screen = list.addDevice(spec.get(), "u_screen");
    list.connect(screen, "Enable", enable);
    list.connect(screen, "Setting", index);
    list.connect(screen, "Value", value);
    tcsim::Engine engine(list);
    engine.addStimulus(0, enable, tcsim::fixtures::one("0"));
    engine.addStimulus(0, index, bits(2, 8));
    engine.addStimulus(0, value, bits(19, 64));
    engine.runUntil(10);
    check(settings->at(2) == 0, "a screen whose Enable is low keeps its settings");
    engine.addStimulus(20, enable, tcsim::fixtures::one("1"));
    engine.runUntil(30);
    check(settings->at(2) == 19, "Enable high stores Value in Setting index");
    engine.addStimulus(40, enable, tcsim::fixtures::one("0"));
    engine.addStimulus(40, value, bits(7, 64));
    engine.runUntil(50);
    check(settings->at(2) == 19, "dropping Enable again stops the writes");
    engine.addStimulus(60, enable, tcsim::fixtures::one("x"));
    engine.addStimulus(60, value, bits(21, 64));
    engine.runUntil(70);
    check(settings->at(2) == 19, "an unknown Enable writes nothing rather than guessing");
    engine.addStimulus(80, enable, tcsim::fixtures::one("1"));
    engine.addStimulus(80, index, bits(200, 8));
    engine.runUntil(90);
    check(settings->unlisted_index == 200 && settings->at(2) == 19,
          "a setting index outside the table is recorded, not invented",
          "unlisted=" + std::to_string(settings->unlisted_index));
    check(engine.violations().empty(), "a screen on a settled board reports nothing");

    /* Renderer: the two pixel modes the manual documents, from the RAM the
       settings point at. */
    tcsim::MemoryImage ram;
    ram.address_bits = 14; /* 16 KiB, enough for the biggest documented picture */
    tcsim::ScreenSettings pixel8;
    pixel8.setting[0] = 2;  /* Pixel 8 */
    pixel8.setting[1] = 40; /* data offset */
    pixel8.setting[2] = 19; /* the resolution the level's own program sets */
    ram.writeByte(40, 0xE0); /* RRR = 7, GGG = 0, BB = 0 */
    ram.writeByte(41, 0x1C); /* RRR = 0, GGG = 7, BB = 0 */
    ram.writeByte(42, 0x03); /* RRR = 0, GGG = 0, BB = 3 */
    tcsim::ScreenImage image;
    std::string note;
    check(tcsim::renderScreen(pixel8, ram, image, note) && note.empty(),
          "a Pixel 8 screen renders from the RAM its settings point at");
    check(image.width == 80 && image.height == 60,
          "resolution setting 19 is the 80x60 the level's drawing code steps through");
    check(image.pixels.size() == 4800 && image.pixels[0] == 0xFF0000u && image.pixels[1] == 0x00FF00u &&
              image.pixels[2] == 0x0000FFu && image.pixels[3] == 0,
          "RRRGGGBB spreads back over the three channels and unwritten bytes stay black");

    tcsim::ScreenSettings pixel32;
    pixel32.setting[0] = 3; /* Pixel 32 */
    pixel32.setting[1] = 100;
    pixel32.setting[2] = 0; /* 4x3 */
    ram.writeByte(100, 0x11);
    ram.writeByte(101, 0x22);
    ram.writeByte(102, 0x33);
    ram.writeByte(103, 0xEE); /* the padding byte the manual names */
    check(tcsim::renderScreen(pixel32, ram, image, note) && image.width == 4 && image.height == 3 &&
              image.pixels[0] == 0x112233u,
          "a Pixel 32 screen takes red, green and blue and leaves the padding out");

    tcsim::ScreenSettings ascii = pixel8;
    ascii.setting[0] = 0;
    check(!tcsim::renderScreen(ascii, ram, image, note) && note.find("font") != std::string::npos,
          "ASCII mode is reported as needing the game's character font, not drawn");
    tcsim::ScreenSettings unlisted = pixel8;
    unlisted.setting[0] = 7;
    check(!tcsim::renderScreen(unlisted, ram, image, note) && note.find("not one of the four") != std::string::npos,
          "a mode number the manual does not list is reported by number");

    /* Board level: the pins are wired the way a live board wires them - a 1-bit
       Enable and 32-bit wires into the Setting and Value pins - and the built
       screen latches what the game's own program writes. */
    const auto build_screen_board = []() {
        tcsim::BoardView board;
        tcsim::BoardComponent component;
        component.kind = 0x5b;
        component.input_count = 3;
        board.components.push_back(component);
        board.wires.push_back(tcsim::BoardWire{-26, -19, -40, -19, 1, 256});
        board.wires.push_back(tcsim::BoardWire{-26, -18, -17, -18, 32, 258});
        board.wires.push_back(tcsim::BoardWire{-26, -17, -17, -17, 32, 260});
        const auto geometry = [](const tcsim::BoardComponent& candidate, tcsim::KindPins& pins) {
            if (candidate.kind != 0x5b) return false;
            pins = tcsim::KindPins{};
            pins.kind = candidate.kind;
            pins.name = "Screen";
            pins.input_count = 3;
            pins.in_x[0] = pins.in_x[1] = pins.in_x[2] = -26;
            pins.in_y[0] = -19;
            pins.in_y[1] = -18;
            pins.in_y[2] = -17;
            pins.in_bits[0] = 1;
            pins.in_bits[1] = 8;
            pins.in_bits[2] = 64;
            return true;
        };
        return tcsim::buildNetList(board, geometry);
    };
    const tcsim::BuildResult built = build_screen_board();
    check(!built.fatal() && built.netlist.deviceCount() == 1,
          "the game's 0x5b Screen builds as a supported device");
    check(built.screens.size() == 1 && built.screens[0].settings != nullptr &&
              built.screens[0].enable == built.net_of_wire[0] &&
              built.screens[0].setting_index == built.net_of_wire[1] &&
              built.screens[0].value == built.net_of_wire[2],
          "the screen's binding keeps the three nets its settings come from");
    check(built.observed_nets.size() == 3, "all three screen inputs are nets the game reads back");
    tcsim::Engine built_engine(built.netlist);
    built_engine.addStimulus(0, built.net_of_wire[0], tcsim::fixtures::one("1"));
    built_engine.addStimulus(0, built.net_of_wire[1], bits(1, 32));
    built_engine.addStimulus(0, built.net_of_wire[2], bits(16384, 32));
    built_engine.runUntil(10);
    check(built.screens[0].settings->data_offset() == 16384,
          "a board-built screen latches the data offset, however wide the wire feeding it is");
    built_engine.addStimulus(20, built.net_of_wire[1], bits(2, 32));
    built_engine.addStimulus(20, built.net_of_wire[2], bits(19, 32));
    built_engine.runUntil(30);
    check(built.screens[0].settings->resolution() == 19 &&
              tcsim::screenPixelWidth(built.screens[0].settings->resolution()) == 80,
          "and the resolution its own program sets, which is the 80 pixel wide screen");

    /* Time and Keyboard are host peripherals: their outputs come from the game,
       the Keyboard's input pin is somebody else's output, and neither is a
       device simcore pretends to know. */
    tcsim::BoardView hosts;
    const auto add_host = [&hosts](uint8_t kind, int16_t x, int16_t y, uint8_t in, uint8_t out) {
        tcsim::BoardComponent component;
        component.kind = kind;
        component.x = x;
        component.y = y;
        component.input_count = in;
        component.output_count = out;
        hosts.components.push_back(component);
    };
    add_host(0x5c, 8, 0, 0, 1);   /* Time */
    add_host(0x5d, 8, 8, 1, 3);   /* Keyboard */
    const auto wire = [&hosts](int x1, int y1, int x2, int y2, uint16_t width) {
        hosts.wires.push_back(tcsim::BoardWire{static_cast<int16_t>(x1), static_cast<int16_t>(y1),
                                              static_cast<int16_t>(x2), static_cast<int16_t>(y2), width,
                                              static_cast<uint64_t>(256 + hosts.wires.size() * 2)});
    };
    wire(10, 0, 20, 0, 64);   /* Time output */
    wire(2, 6, -10, 6, 1);    /* Keyboard input */
    wire(12, 5, 20, 5, 1);    /* Keyboard outputs */
    wire(13, 8, 20, 8, 1);
    wire(13, 9, 20, 9, 8);
    const auto host_geometry = [](const tcsim::BoardComponent& candidate, tcsim::KindPins& pins) {
        pins = tcsim::KindPins{};
        pins.kind = candidate.kind;
        if (candidate.kind == 0x5c) {
            pins.name = "Time";
            pins.output_count = 1;
            pins.out_x[0] = 2;
            pins.out_y[0] = 0;
            pins.out_bits[0] = 64;
            return true;
        }
        if (candidate.kind == 0x5d) {
            pins.name = "Keyboard";
            pins.input_count = 1;
            pins.in_x[0] = -6;
            pins.in_y[0] = -2;
            pins.in_bits[0] = 1;
            pins.output_count = 3;
            pins.out_x[0] = 4;
            pins.out_y[0] = -3;
            pins.out_x[1] = 5;
            pins.out_y[1] = 0;
            pins.out_x[2] = 5;
            pins.out_y[2] = 1;
            pins.out_bits[0] = pins.out_bits[1] = 1;
            pins.out_bits[2] = 8;
            return true;
        }
        return false;
    };
    const tcsim::BuildResult host_build = tcsim::buildNetList(hosts, host_geometry);
    check(!host_build.fatal() && host_build.netlist.deviceCount() == 0,
          "Time and Keyboard build with no device of their own - the game is their source");
    check(host_build.source_nets.size() == 4,
          "their outputs are the externally driven nets: one from the Time, three from the Keyboard");
    check(std::find(host_build.source_nets.begin(), host_build.source_nets.end(),
                    host_build.net_of_wire[1]) == host_build.source_nets.end(),
          "the Keyboard's own input pin is not mistaken for something it drives");
}

void testGateUnknown() {
    tcsim::fixtures::Fixture fixture = tcsim::fixtures::gateUnknown();
    tcsim::Engine engine = engineFor(fixture);
    engine.runUntil(10);
    check(engine.value("one").toString() == "1" && engine.value("zero").toString() == "0", "tie-off devices drive");
    check(engine.value("nz").toString() == "x", "a gate with a Z input answers X");
    check(engine.value("g0x").toString() == "0", "0 dominates an AND whatever the other input is");
    check(engine.value("g1x").toString() == "x", "1 and X leave an AND unknown");
    check(engine.value("gox").toString() == "1", "1 dominates an OR");
}

void testRing() {
    tcsim::fixtures::Fixture fixture = tcsim::fixtures::ring();
    tcsim::Engine engine = engineFor(fixture, true);
    engine.runUntil(35);
    check(engine.value("r1").toString() == "1" && engine.value("r2").toString() == "0" &&
              engine.value("r3").toString() == "1",
          "a held nand keeps the ring quiet");
    engine.runUntil(131);
    check(engine.value("r1").toString() == "0", "releasing the ring moves the first stage");
    engine.runUntil(161);
    check(engine.value("r1").toString() == "1", "one stage delay later the loop comes back around");
    engine.runUntil(181);
    check(engine.value("r3").toString() == "1", "three inverting stages, six delays per period");
    /* The transitions the cross-check compares against iverilog: 130, 140,
       150, 160, ... - a period of six stage delays. */
    std::vector<tcsim::Tick> transition;
    for (const tcsim::TraceRecord& record : engine.trace()) {
        if (engine.netlist().net(record.net).name == "r1") transition.push_back(record.when);
    }
    size_t edges = 0;
    for (size_t index = 1; index < transition.size(); ++index) {
        if (transition[index] != transition[index - 1]) ++edges;
    }
    check(edges >= 3, "the ring kept switching after release");
    engine.runUntil(320);
    check(engine.value("r1").toString() == "0" || engine.value("r1").toString() == "1", "the ring is still running");
    check(!engine.aborted(), "an oscillating ring is not a zero-delay oscillation");
}

void testDeltaChain() {
    tcsim::fixtures::Fixture fixture = tcsim::fixtures::deltaChain(64);
    tcsim::Engine engine = engineFor(fixture);
    engine.runUntil(5);
    check(engine.value("b63").toString() == "0", "64 zero-delay stages settle inside one time step");
    engine.runUntil(20);
    check(engine.value("b63").toString() == "1", "the same chain carries the new value the same way");
    check(engine.deltaRounds() > 60, "every stage needed its own delta round, not a recursive call");
}

void testDffShift() {
    tcsim::fixtures::Fixture fixture = tcsim::fixtures::dffShift();
    tcsim::Engine engine = engineFor(fixture);
    engine.runUntil(25);
    check(engine.value("q1").toString() == "1", "the first register latched on the rising edge");
    check(engine.value("q2").toString() == "x", "the second register waited for the next edge");
    engine.runUntil(60);
    check(engine.value("q1").toString() == "0" && engine.value("q2").toString() == "1",
          "the shift register moves one stage per edge");
    engine.runUntil(100);
    check(engine.value("q1").toString() == "0" && engine.value("q2").toString() == "0" &&
              engine.value("q3").toString() == "1",
          "three edges, three stages");
}

void testSetupHold() {
    using tcsim::fixtures::one;
    tcsim::NetList list;
    const tcsim::NetId d = list.addNet("d");
    const tcsim::NetId clk = list.addNet("clk");
    const tcsim::NetId q = list.addNet("q");
    const tcsim::DeviceId ff = list.addDevice(&tcsim::deviceDff(tcsim::ArcDelay{0, 0}, tcsim::ArcDelay{20, 20},
                                                               tcsim::ArcDelay{5, 5}),
                                              "u_ff");
    list.connect(ff, "D", d);
    list.connect(ff, "CLK", clk);
    list.connect(ff, "Q", q);
    check(list.problems.empty(), "the setup/hold fixture builds cleanly");
    tcsim::EngineOptions options;
    options.trace = true;
    tcsim::Engine engine(list, options);
    engine.addStimulus(0, clk, one("0"));
    engine.addStimulus(5, d, one("1"));   /* 5 ticks before a 20 tick setup window */
    engine.addStimulus(10, clk, one("1")); /* the edge */
    engine.addStimulus(12, d, one("0"));   /* inside a 5 tick hold window */
    engine.runUntil(40);
    check(violationCount(engine, tcsim::ViolationKind::kSetupViolation) == 1, "a setup violation is reported once");
    check(violationCount(engine, tcsim::ViolationKind::kHoldViolation) == 1, "a hold violation is reported once");
    check(engine.value("q").toString() == "1", "the register still latches what it saw");

    tcsim::EngineOptions strict_options;
    tcsim::NetList strict_list;
    const tcsim::NetId strict_d = strict_list.addNet("d");
    const tcsim::NetId strict_clk = strict_list.addNet("clk");
    const tcsim::NetId strict_q = strict_list.addNet("q");
    const tcsim::DeviceId strict_ff =
        strict_list.addDevice(&tcsim::deviceDff(tcsim::ArcDelay{0, 0}, tcsim::ArcDelay{20, 20},
                                                tcsim::ArcDelay{0, 0}, true),
                              "u_ff");
    strict_list.connect(strict_ff, "D", strict_d);
    strict_list.connect(strict_ff, "CLK", strict_clk);
    strict_list.connect(strict_ff, "Q", strict_q);
    tcsim::Engine strict(strict_list, strict_options);
    strict.addStimulus(0, strict_clk, one("0"));
    strict.addStimulus(5, strict_d, one("1"));
    strict.addStimulus(10, strict_clk, one("1"));
    strict.runUntil(30);
    check(strict.value("q").toString() == "x", "with x_on_violation a violated register drives X");
}

void testZeroDelayLoop() {
    tcsim::fixtures::Fixture fixture = tcsim::fixtures::zeroDelayLoop();
    tcsim::Engine engine = engineFor(fixture, false, 200);
    engine.runUntil(10);
    check(engine.aborted(), "a zero-delay loop stops the run instead of spinning");
    check(hasViolation(engine, tcsim::ViolationKind::kZeroDelayOscillation),
          "the zero-delay oscillation is reported");

    /* The other side of the same coin: an inverter feeding itself with no
       startup path settles on X and stays there.  A simulator that reports an
       oscillation here would be wrong, and one that hangs on the circuit above
       would be just as wrong. */
    tcsim::fixtures::Fixture loop = tcsim::fixtures::selfLoop();
    tcsim::Engine settled = engineFor(loop, false, 200);
    settled.runUntil(20);
    check(!settled.aborted(), "an inverter feeding itself does not oscillate");
    check(settled.value("y").toString() == "x", "it settles on X");
}

void testPowerOnSeed() {
    tcsim::NetList list;
    const tcsim::NetId a = list.addNet("a");
    const tcsim::NetId b = list.addNet("b");
    const tcsim::NetId c = list.addNet("c");
    const tcsim::DeviceSpec* inverter = &tcsim::deviceNot(tcsim::ArcDelay{8, 8});
    const tcsim::DeviceId u0 = list.addDevice(inverter, "u0");
    const tcsim::DeviceId u1 = list.addDevice(inverter, "u1");
    const tcsim::DeviceId u2 = list.addDevice(inverter, "u2");
    list.connectInput(u0, 0, c);
    list.connectOutput(u0, 0, a);
    list.connectInput(u1, 0, a);
    list.connectOutput(u1, 0, b);
    list.connectInput(u2, 0, b);
    list.connectOutput(u2, 0, c);
    tcsim::EngineOptions options;
    options.trace = true;
    tcsim::Engine engine(list, options);
    engine.runUntil(32);
    check(engine.value("a").toString() == "x", "an unseeded odd ring stays at the four-state X fixed point");
    check(engine.seedNet(a, tcsim::BitVector(1, tcsim::Logic::kZero)),
          "a settled ring accepts an explicit power-on seed");
    engine.runUntil(200);
    std::vector<tcsim::Tick> edges;
    for (const tcsim::TraceRecord& event : engine.trace()) {
        if (!event.initial && event.net == a && event.when > 32 &&
            (event.drive == tcsim::Logic::kZero || event.drive == tcsim::Logic::kOne))
            edges.push_back(event.when);
    }
    check(edges.size() >= 5, "one transient seed starts the three-stage ring");
    bool period_ok = edges.size() >= 5;
    for (size_t index = 1; index < edges.size(); ++index) {
        if (edges[index] - edges[index - 1] != 24) period_ok = false;
    }
    check(period_ok, "successive edges are ring length times per-stage delay apart");
    check(!engine.aborted(), "a delayed seeded ring runs without the delta-loop guard");
}

void testTransportDelays() {
    tcsim::NetList list;
    const tcsim::NetId a = list.addNet("a");
    const tcsim::NetId y = list.addNet("y");
    const tcsim::DeviceId buffer = list.addDevice(
        &tcsim::deviceBuffer(tcsim::ArcDelay{10, 10}, tcsim::DelayKind::kTransport), "u_buf");
    list.connect(buffer, "A", a);
    list.connect(buffer, "Y", y);
    tcsim::Engine engine(list);
    engine.addStimulus(0, a, tcsim::fixtures::one("0"));
    engine.addStimulus(20, a, tcsim::fixtures::one("1"));
    engine.addStimulus(23, a, tcsim::fixtures::one("0"));
    engine.runUntil(31);
    check(engine.value("y").toString() == "1",
          "with transport delay a pulse narrower than the delay still arrives");
    engine.runUntil(35);
    check(engine.value("y").toString() == "0", "and it leaves again, late");
}

void testUndrivenNet() {
    tcsim::NetList list;
    const tcsim::NetId a = list.addNet("a");
    const tcsim::NetId y = list.addNet("y");
    const tcsim::NetId dangling = list.addNet("dangling");
    const tcsim::NetId unused = list.addNet("unused");
    const tcsim::DeviceId buffer = list.addDevice(&tcsim::deviceBuffer(tcsim::ArcDelay{1, 1}), "u_buf");
    list.connect(buffer, "A", a);
    list.connect(buffer, "Y", y);
    const tcsim::DeviceId gate = list.addDevice(&tcsim::deviceNot(tcsim::ArcDelay{1, 1}), "u_gate");
    list.connect(gate, "A", dangling);
    list.connect(gate, "Y", unused);
    /* A net with a load and no driver at all: the same read as Z, but it is a
       hole in the board and the plan wants it named. */
    tcsim::Engine engine(list);
    engine.addStimulus(0, a, tcsim::fixtures::one("1"));
    engine.runUntil(10);
    check(engine.value("y").toString() == "1", "the wired part still works");
    check(engine.undrivenNets().size() == 1, "only the pin nobody drives is reported");
    check(engine.netlist().net(engine.undrivenNets().front()).name == "dangling",
          "the reported net is the one with no driver");
    check(hasViolation(engine, tcsim::ViolationKind::kUndrivenNet), "the undriven net is a violation");
}

void testVcd() {
    tcsim::fixtures::Fixture fixture = tcsim::fixtures::delayChain();
    tcsim::Engine engine = engineFor(fixture, true);
    engine.runUntil(80);
    const std::string vcd = tcsim::writeVcd(engine.netlist(), engine.trace(), "tb", "1ps");
    check(vcd.find("$timescale 1ps $end") != std::string::npos, "the VCD declares its timescale");
    check(vcd.find("$var wire 1") != std::string::npos, "the VCD declares its signals");
    check(vcd.find("#0\n") != std::string::npos, "the VCD starts at time 0");
    check(vcd.find("#10\n") != std::string::npos, "the VCD carries the buffer's own deadline");
    check(vcd.find("y1") != std::string::npos, "the VCD names the nets the netlist named");
    /* Every net has a value at time 0 - the same promise $dumpvars makes, and
       what lets the iverilog comparison start from a known state. */
    size_t zero_records = 0;
    for (const tcsim::TraceRecord& record : engine.trace()) {
        if (record.when == 0) ++zero_records;
    }
    check(zero_records >= engine.netlist().netCount(), "every net is reported at time 0");
}

void testVerilogExport() {
    tcsim::fixtures::Fixture fixture = tcsim::fixtures::delayChain();
    const tcsim::VerilogExport exported = tcsim::exportVerilog(fixture.list, fixture.stimulus, fixture.end, "tb");
    check(exported.problems.empty(), "the delay chain exports without a caveat");
    check(exported.text.find("module tb;") != std::string::npos, "the export is a module");
    check(exported.text.find("assign #(10,10) y1 = a;") != std::string::npos, "the buffer keeps its delay");
    check(exported.text.find("assign #(8,12) y2 = ~(a & b);") != std::string::npos,
          "the nand keeps its rising and falling delays");
    check(exported.text.find("$dumpvars(0, tb);") != std::string::npos, "the testbench dumps every net");

    tcsim::fixtures::Fixture ring_fixture = tcsim::fixtures::ring();
    const tcsim::VerilogExport ring_export =
        tcsim::exportVerilog(ring_fixture.list, ring_fixture.stimulus, ring_fixture.end, "tb");
    check(ring_export.problems.empty(), "a closed loop exports as happily as a tree");
    check(ring_export.text.find("assign #(10,10) r1 = ~(r3 & rst);") != std::string::npos,
          "the ring's first stage keeps its delay");

    tcsim::fixtures::Fixture dff_fixture = tcsim::fixtures::dffShift();
    const tcsim::VerilogExport dff_export =
        tcsim::exportVerilog(dff_fixture.list, dff_fixture.stimulus, dff_fixture.end, "tb");
    check(dff_export.problems.empty(), "the shift register exports");
    check(dff_export.text.find("reg q1;") != std::string::npos, "a register output is a reg in the export");
    check(dff_export.text.find("always @(posedge clk) q1 <= #(5) d;") != std::string::npos,
          "the flip-flop keeps its clock-to-q delay");
}

void testCanonicalEvents() {
    tcsim::fixtures::Fixture fixture = tcsim::fixtures::ring();
    tcsim::Engine engine = engineFor(fixture, true);
    engine.runUntil(200);
    const std::string events = tcsim::canonicalEvents(engine.netlist(), engine.trace());
    check(events.find("0 rst 0\n") != std::string::npos, "the event list starts with the initial value");
    check(events.find("10 r1 1\n") != std::string::npos, "the first stage is reported at its own deadline");
    check(events.find("130 r1 0\n") != std::string::npos, "the released ring is reported at its own deadline");
    check(events.find("180 r3 1\n") != std::string::npos, "the ring's period shows up in the event list");
    check(events.back() == '\n', "the event list is line based");
}

/* The S2 netlist builder, run against records a real session produced
   (tests/data/and_gate-board.txt, extracted by tools/extract-board-fixture.ps1).
   The geometry table is the one build/kinds.txt reports for those kinds. */
bool testBoardNetlist() {
    std::ifstream fixture("tests/data/and_gate-board.txt");
    if (!fixture) {
        std::printf("SKIP the board netlist test: tests/data/and_gate-board.txt is missing\n");
        return false;
    }
    tcsim::BoardView board;
    std::string line;
    while (std::getline(fixture, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("component", 0) == 0) {
            tcsim::BoardComponent component;
            unsigned kind = 0;
            long long x = 0, y = 0, rotation = 0;
            std::sscanf(line.c_str(), "component index=%*d kind=0x%x x=%lld y=%lld rotation=%lld", &kind, &x, &y,
                        &rotation);
            component.kind = static_cast<uint8_t>(kind);
            component.x = static_cast<int16_t>(x);
            component.y = static_cast<int16_t>(y);
            component.rotation = static_cast<uint8_t>(rotation);
            /* The record's own pin counts, which the builder cross-checks. */
            size_t at = line.find(" word30=");
            const size_t inputs_at = line.find(" word30=");
            const size_t outputs_at = line.find(" word40=");
            if (at == std::string::npos || inputs_at == std::string::npos || outputs_at == std::string::npos) continue;
            component.input_count = static_cast<uint8_t>(std::stoul(line.substr(inputs_at + 8)));
            component.output_count = static_cast<uint8_t>(std::stoul(line.substr(outputs_at + 8)));
            board.components.push_back(component);
            continue;
        }
        if (line.rfind("wire", 0) == 0) {
            tcsim::BoardWire wire;
            long long x1 = 0, y1 = 0, x2 = 0, y2 = 0, width = 0;
            unsigned long long slot = 0;
            std::sscanf(line.c_str(), "wire index=%*d from=%lld,%lld to=%lld,%lld width=%lld slot=%llu", &x1, &y1, &x2,
                        &y2, &width, &slot);
            wire.x1 = static_cast<int16_t>(x1);
            wire.y1 = static_cast<int16_t>(y1);
            wire.x2 = static_cast<int16_t>(x2);
            wire.y2 = static_cast<int16_t>(y2);
            wire.width = static_cast<uint16_t>(width);
            wire.slot = slot;
            board.wires.push_back(wire);
        }
    }
    check(board.components.size() == 6 && board.wires.size() == 3, "the captured board has six components and three wires");

    const auto geometry = [](const tcsim::BoardComponent& component, tcsim::KindPins& pins) {
        const uint8_t kind = component.kind;
        switch (kind) {
            case 0x3f: /* Input: two outputs, no input */
                pins.kind = kind;
                pins.name = "Input";
                pins.output_count = 2;
                pins.out_x[0] = 0;
                pins.out_y[0] = -1;
                pins.out_x[1] = 0;
                pins.out_y[1] = 1;
                pins.out_bits[0] = pins.out_bits[1] = 1;
                return true;
            case 0x44: /* Output: one input */
                pins.kind = kind;
                pins.name = "Output";
                pins.input_count = 1;
                pins.in_x[0] = -1;
                pins.in_y[0] = 0;
                pins.in_bits[0] = 1;
                return true;
            case 0x04: /* AND */
                pins.kind = kind;
                pins.name = "AND";
                pins.input_count = 2;
                pins.in_x[0] = -1;
                pins.in_y[0] = -1;
                pins.in_x[1] = -1;
                pins.in_y[1] = 1;
                pins.in_bits[0] = pins.in_bits[1] = 1;
                pins.output_count = 1;
                pins.out_x[0] = 2;
                pins.out_y[0] = 0;
                pins.out_bits[0] = 1;
                return true;
            default: break;
        }
        return false;
    };
    const tcsim::BuildResult result = tcsim::buildNetList(board, geometry);
    check(result.netlist.netCount() == 3, "three wires make three nets");
    check(result.netlist.deviceCount() == 1, "the only gate on the board becomes one device");
    check(result.net_of_wire.size() == 3 && result.net_of_wire[0] != result.net_of_wire[1] &&
              result.net_of_wire[1] != result.net_of_wire[2] && result.net_of_wire[0] != result.net_of_wire[2],
          "every wire lands on its own net");
    const auto slots = [&](size_t wire) {
        std::vector<uint64_t> values = result.slots_of_net[result.net_of_wire[wire]];
        std::sort(values.begin(), values.end());
        return values;
    };
    check(slots(0) == std::vector<uint64_t>({256, 257}), "wire 0 owns the state bytes 256 and 257");
    check(slots(1) == std::vector<uint64_t>({258, 259}), "wire 1 owns 258 and 259");
    check(slots(2) == std::vector<uint64_t>({260, 261}), "wire 2 owns 260 and 261");
    /* Inputs are sources, so the two wires they drive are read from the game's
       array; the Output's wire is what the game reads back. */
    check(result.source_nets.size() == 2, "the two input wires are sources");
    check(result.observed_nets.size() == 1 && result.observed_nets[0] == result.net_of_wire[2],
          "the output wire is the observed net");
    check(!result.fatal(), "the captured board builds a runnable netlist");
    size_t shared = 0;
    for (const tcsim::BuildNote& note : result.notes) {
        if (note.text.find("share the cell") != std::string::npos) ++shared;
    }
    check(shared == 3, "the three cells two pins share are reported, not guessed");
    check(result.unconnected_pins == 0, "every pin on this board is reached by a wire");

    /* The gate the builder made is wired to the wire nets it should be: A and B
       to the two input wires, Y to the output wire. */
    const tcsim::DeviceInstance& gate = result.netlist.devices()[0];
    check(gate.spec->name == "and2", "the AND kind maps to the and2 device");
    check(gate.inputs[0] == result.net_of_wire[0] && gate.inputs[1] == result.net_of_wire[1],
          "the gate reads the two input wires");
    check(gate.outputs[0] == result.net_of_wire[2], "the gate drives the output wire");

    /* Rotation is the editor's own byte; a quarter turn clockwise moves a pin
       at (-1,-1) to (1,-1) and back around. */
    check(tcsim::rotateOffset(-1, -1, 0) == std::make_pair(-1, -1), "rotation 0 keeps the pin offset");
    check(tcsim::rotateOffset(-1, -1, 1) == std::make_pair(1, -1), "one quarter turn rotates clockwise");
    check(tcsim::rotateOffset(-1, -1, 3) == std::make_pair(-1, 1), "three quarter turns rotate the other way");
    return true;
}

/* The word Splitter (0x2f): "Splits a (2n)-bit value into two n-bit values",
   the game's own description of the kind.  Its prototype declares the input as
   one of the width sentinels and both outputs `wauto`, so the halved width has
   to come from the wires - which is what the builder does, and this pins the
   halves the device then produces. */
void testWordSplitter() {
    using tcsim::BitVector;
    tcsim::NetList list;
    const tcsim::NetId a = list.addNet("a", 8);
    const tcsim::NetId high = list.addNet("high", 4);
    const tcsim::NetId low = list.addNet("low", 4);
    const tcsim::DeviceId split = list.addDevice(&tcsim::deviceWordSplit(4), "u_split");
    list.connect(split, "A", a);
    list.connect(split, "Y0", high);
    list.connect(split, "Y1", low);
    check(list.problems.empty(), "the word Splitter fixture builds cleanly");
    tcsim::Engine engine(list);
    engine.addStimulus(0, a, BitVector::fromText("10100011", 8));
    engine.runUntil(10);
    check(engine.netValue(high).toString() == "1010", "the top output is the high half");
    check(engine.netValue(low).toString() == "0011", "and the bottom output the low half");
    /* Four-state: an unsettled input bit poisons the half it belongs to and
       leaves the other half alone. */
    engine.addStimulus(20, a, BitVector::fromText("1x100011", 8));
    engine.runUntil(30);
    check(engine.netValue(high).toString() == "1x10", "an unknown bit poisons its own half");
    check(engine.netValue(low).toString() == "0011", "and leaves the other half untouched");
}

/* The storage family: Register (0x27), Counter (0x26) and Delay Line (0x37),
   the three kinds the game's own boards lean on and whose pins the prototype
   table keeps out of its declared input list.

   Two things are asserted here, and they are the two the plan called
   unmeasured: where the pins are, and when the parts latch.  The pins come from
   the prototype's hidden list (`hidden0=(-3,-1,w1)` and friends in
   build/kinds.txt); the latching is the game's own LATE phase, which the
   runtime models by toggling one cycle net - so this test drives that net by
   hand and checks the register holds, the delay line stores, and the counter
   counts and resets. */
/* The two families the prototype table sizes with a sentinel.  Both numbers
   were measured on the player's own board (tools/kind-wire-widths.py,
   2026-09-30):
       kind 0x2F Splitter   at 138,-2  in0 64 bits -> out0 32, out1 32
       kind 0x30 Word Maker at -10,37  in0 8, in1 8 -> out0 16
   with the record's own `+0xe0` agreeing (32 = the leaf width, 16 = the output
   width).  So a Splitter's input is `leaves * n` and a Word Maker's output is
   the sum of its inputs - and both widths come off the wires, which is what the
   board half of this test checks. */
void testAutoWidthSplitterAndMaker() {
    using tcsim::BitVector;
    using tcsim::BoardComponent;
    using tcsim::BoardView;
    using tcsim::BoardWire;

    const auto apply = [](const tcsim::DeviceSpec& spec, const std::vector<BitVector>& inputs) {
        std::vector<BitVector> outputs;
        tcsim::EvalContext context;
        spec.behavior(context, inputs, outputs);
        std::string text;
        for (const BitVector& output : outputs) {
            if (!text.empty()) text += " ";
            text += output.toString();
        }
        return text;
    };

    const std::vector<uint16_t> maker_widths{8, 8};
    const tcsim::DeviceSpec& maker = tcsim::deviceWordMaker(maker_widths);
    check(maker.name == "wordmaker16" && maker.inputs.size() == 2 && maker.outputs.size() == 1 &&
              maker.inputs[0].bits == 8 && maker.inputs[1].bits == 8 && maker.outputs[0].bits == 16,
          "a Word Maker over two 8-bit values is a 16-bit device");
    check(apply(maker, {BitVector::fromText("10101011", 8), BitVector::fromText("11001101", 8)}) ==
              "1010101111001101",
          "the topmost input is the most significant half of the word");
    check(apply(maker, {BitVector::fromText("1", 1), BitVector::fromText("0", 1)}) ==
              "1xxxxxxx0xxxxxxx",
          "an input narrower than the pin leaves the missing bits unknown");

    const tcsim::DeviceSpec& split = tcsim::deviceWordSplitter(4, 4);
    check(split.name == "wordsplit4x4" && split.inputs.size() == 1 && split.outputs.size() == 4 &&
              split.inputs[0].bits == 16 && split.outputs[0].bits == 4,
          "a four-way word Splitter takes 4n bits and gives four n-bit leaves");
    check(apply(split, {BitVector::fromText("1010101111001101", 16)}) == "1010 1011 1100 1101",
          "the topmost leaf is the most significant slice");

    const auto component = [](uint8_t kind, int16_t x, int16_t y, uint8_t inputs, uint8_t outputs) {
        BoardComponent value;
        value.kind = kind;
        value.x = x;
        value.y = y;
        value.input_count = inputs;
        value.output_count = outputs;
        return value;
    };
    const auto wire = [](int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t width) {
        BoardWire value;
        value.x1 = x1;
        value.y1 = y1;
        value.x2 = x2;
        value.y2 = y2;
        value.width = width;
        return value;
    };
    const auto geometry = [](const BoardComponent& entry, tcsim::KindPins& pins) {
        pins = tcsim::KindPins{};
        pins.kind = entry.kind;
        if (entry.kind == 0x30) {
            pins.name = "Word Maker";
            pins.input_count = 2;
            pins.output_count = 1;
            pins.in_x[0] = -1; pins.in_y[0] = -1; pins.in_bits[0] = 1;
            pins.in_x[1] = -1; pins.in_y[1] = 0;  pins.in_bits[1] = 1;
            pins.out_x[0] = 1; pins.out_y[0] = 0;  pins.out_bits[0] = 1;
            return true;
        }
        if (entry.kind == 0x63) {
            pins.name = "Splitter";
            pins.input_count = 1;
            pins.output_count = 4;
            pins.in_x[0] = -1; pins.in_y[0] = 0; pins.in_bits[0] = 1;
            for (uint8_t pin = 0; pin < 4; ++pin) {
                pins.out_x[pin] = 1;
                pins.out_y[pin] = static_cast<int8_t>(-1 + pin);
                pins.out_bits[pin] = 1;
            }
            return true;
        }
        return false;
    };

    BoardView board;
    board.components.push_back(component(0x30, 0, 0, 2, 1));
    board.components.push_back(component(0x63, 0, 30, 1, 4));
    board.wires.push_back(wire(-4, -1, -1, -1, 8));   /* Word Maker in0: 8 bits */
    board.wires.push_back(wire(-4, 0, -1, 0, 8));     /* Word Maker in1: 8 bits */
    board.wires.push_back(wire(1, 0, 6, 0, 16));      /* its output: 16 bits */
    board.wires.push_back(wire(-4, 30, -1, 30, 16));  /* Splitter input: 16 bits */
    board.wires.push_back(wire(1, 29, 6, 29, 4));     /* four 4-bit leaves */
    board.wires.push_back(wire(1, 30, 6, 30, 4));
    board.wires.push_back(wire(1, 31, 6, 31, 4));
    board.wires.push_back(wire(1, 32, 6, 32, 4));

    const tcsim::BuildResult built = tcsim::buildNetList(board, geometry);
    check(!built.fatal(), "the `wauto` board builds once the widths come off its wires");
    const tcsim::DeviceId maker_device = built.devices_by_component[0];
    const tcsim::DeviceId split_device = built.devices_by_component[1];
    check(maker_device != tcsim::kNoDevice &&
              built.netlist.device(maker_device).spec->name == "wordmaker16",
          "an 8+8 Word Maker is built as a 16-bit concatenation");
    check(maker_device == tcsim::kNoDevice ||
              (built.netlist.device(maker_device).spec->inputs[0].bits == 8 &&
               built.netlist.device(maker_device).spec->inputs[1].bits == 8),
          "each Word Maker input keeps the width of the wire that reaches it");
    check(split_device != tcsim::kNoDevice &&
              built.netlist.device(split_device).spec->name == "wordsplit4x4",
          "a four-way Splitter takes its leaf width from the wires (16 / 4 = 4)");

    /* A definition's wires carry no width at all (measured on the player's
       foundry definitions: every internal wire of every one of them reads 0), so
       a net has to take the width its pins ask for.  Without that every
       internal 32-bit path inside a custom component would be built as one-bit
       nets. */
    const auto geometry4 = [](const tcsim::BoardComponent& entry, tcsim::KindPins& pins) {
        if (entry.kind != 0x6e) return false;
        pins = tcsim::KindPins{};
        pins.kind = entry.kind;
        pins.name = "Splitter";
        pins.input_count = 1;
        pins.output_count = 4;
        pins.in_x[0] = -1;
        pins.in_y[0] = 0;
        pins.in_bits[0] = 4;
        for (uint8_t pin = 0; pin < 4; ++pin) {
            pins.out_x[pin] = 1;
            pins.out_y[pin] = static_cast<int8_t>(-1 + pin);
            pins.out_bits[pin] = 1;
        }
        return true;
    };
    tcsim::BoardView definition_style;
    definition_style.components.push_back(component(0x6e, 0, 0, 1, 4));
    definition_style.wires.push_back(wire(-4, 0, -1, 0, 0)); /* the input, width unknown */
    for (int pin = 0; pin < 4; ++pin) {
        definition_style.wires.push_back(wire(1, static_cast<int16_t>(pin - 1), 6, static_cast<int16_t>(pin - 1), 0));
    }
    const tcsim::BuildResult built_zero = tcsim::buildNetList(definition_style, geometry4);
    check(!built_zero.fatal() && built_zero.netlist.deviceCount() == 1 &&
              built_zero.netlist.devices()[0].spec->name == "splitter4",
          "a Splitter whose wires carry no width still builds at its declared four bits");
    check(built_zero.net_of_wire.size() > 0 && built_zero.net_of_wire[0] != tcsim::kNoNet &&
              built_zero.netlist.net(built_zero.net_of_wire[0]).bits == 4,
          "the net a width-less wire reaches takes the width of the pin it lands on");
}

void testStorageFamily() {
    using tcsim::BoardComponent;
    using tcsim::BoardView;
    using tcsim::BoardWire;
    using tcsim::BitVector;
    using tcsim::KindPins;

    BoardView board;
    const auto component = [](uint8_t kind, int16_t x, int16_t y, uint16_t width) {
        BoardComponent value;
        value.kind = kind;
        value.x = x;
        value.y = y;
        /* The record counts the hidden pins: a register and a counter carry two
           (enable/data, reset/data), a delay line one. */
        value.input_count = kind == 0x37 ? 1 : 2;
        value.output_count = 1;
        value.memory_bits = width;
        return value;
    };
    board.components.push_back(component(0x27, 0, 0, 8));    /* Register */
    board.components.push_back(component(0x37, 0, 10, 8));   /* Delay Line */
    board.components.push_back(component(0x26, 0, 20, 8));   /* Counter */
    const auto wire = [](int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t width, uint64_t slot) {
        BoardWire value;
        value.x1 = x1;
        value.y1 = y1;
        value.x2 = x2;
        value.y2 = y2;
        value.width = width;
        value.slot = slot;
        return value;
    };
    board.wires.push_back(wire(-2, -1, -3, -1, 1, 100));   /* register enable */
    board.wires.push_back(wire(-8, 0, -3, 0, 8, 110));     /* register data */
    board.wires.push_back(wire(3, 0, 6, 0, 8, 120));       /* register output */
    board.wires.push_back(wire(-8, 10, -3, 10, 8, 130));   /* delay line data */
    board.wires.push_back(wire(3, 10, 6, 10, 8, 140));     /* delay line output */
    board.wires.push_back(wire(-2, 19, -3, 19, 1, 150));   /* counter reset */
    board.wires.push_back(wire(-8, 20, -3, 20, 8, 160));   /* counter data (not read) */
    board.wires.push_back(wire(3, 20, 6, 20, 8, 170));     /* counter output */

    const auto geometry = [](const BoardComponent& entry, KindPins& pins) {
        pins.kind = entry.kind;
        pins.declared_inputs = 0;
        pins.output_count = 1;
        pins.out_x[0] = 3;
        pins.out_y[0] = 0;
        pins.out_bits[0] = 0x7fff; /* wauto */
        if (entry.kind == 0x27 || entry.kind == 0x26) {
            pins.name = entry.kind == 0x27 ? "Register" : "Counter";
            pins.hidden_count = 2;
            pins.input_count = 2;
            pins.hidden_x[0] = -3; pins.hidden_y[0] = -1; pins.hidden_bits[0] = 1;
            pins.hidden_x[1] = -3; pins.hidden_y[1] = 0;  pins.hidden_bits[1] = 0x7fff;
            return true;
        }
        if (entry.kind == 0x37) {
            pins.name = "Delay Line";
            pins.hidden_count = 1;
            pins.input_count = 1;
            pins.hidden_x[0] = -3; pins.hidden_y[0] = 0; pins.hidden_bits[0] = 0x7fff;
            return true;
        }
        return false;
    };
    const tcsim::BuildResult built = tcsim::buildNetList(board, geometry);
    check(!built.fatal(), "the storage board builds a runnable netlist");
    check(built.cycle_token != tcsim::kNoNet, "the storage family creates the per-cycle clock net");
    check(built.netlist.deviceCount() == 3, "the register, the delay line and the counter each become a device");
    /* `publish()` walks the slot table by net index, so every net the builder
       adds needs an entry there.  The `#cycle` token was added without one:
       publish then read a vector past the end, which in the running game is an
       intermittent null dereference (`dev.sandbox-sim in on_frame: access
       violation at 0x0`, measured 2026-09-30 on a player's board).  The
       toolchain's own bounds assertions make a repeat a hard failure here. */
    check(built.slots_of_net.size() == built.netlist.netCount(),
          "every net has a slot list, the cycle token included");
    {
        tcsim::SandboxRuntime runtime;
        check(runtime.bind(board, geometry), "the storage board binds a runtime");
        runtime.advance(runtime.stepTicksFor(1),
                        [](tcsim::NetId, const std::vector<uint64_t>&) { return uint64_t{0}; });
        size_t written = 0;
        runtime.publish([&written](uint64_t slot, unsigned char byte) {
            ++written;
            (void)slot;
            (void)byte;
        });
        check(written > 0, "publishing a board with a cycle token walks every net");
    }

    const tcsim::DeviceInstance& register_device = built.netlist.devices()[0];
    check(register_device.spec->name == "register8" && register_device.spec->inputs.size() == 3 &&
              register_device.spec->inputs[0].name == "EN" && register_device.spec->inputs[0].bits == 1 &&
              register_device.spec->inputs[1].name == "D" && register_device.spec->inputs[1].bits == 8,
          "the register takes a 1-bit enable and an 8-bit data pin", register_device.spec->name);
    check(register_device.inputs[0] == built.net_of_wire[0] && register_device.inputs[1] == built.net_of_wire[1],
          "its hidden pins are wired where the prototype's hidden list puts them");
    check(register_device.inputs[2] == built.cycle_token, "and its clock is the cycle net");
    const tcsim::DeviceInstance& delay_device = built.netlist.devices()[1];
    check(delay_device.spec->name == "delayline8" && delay_device.inputs[0] == built.net_of_wire[3] &&
              delay_device.inputs[1] == built.cycle_token,
          "the delay line reads its data pin and clocks on the cycle net", delay_device.spec->name);
    const tcsim::DeviceInstance& counter_device = built.netlist.devices()[2];
    check(counter_device.spec->name == "counter8" && counter_device.spec->inputs[0].bits == 1 &&
              counter_device.spec->inputs[1].bits == 8 && counter_device.inputs[2] == built.cycle_token,
          "the counter takes a 1-bit reset beside its 8-bit data pin", counter_device.spec->name);

    /* Now run it.  The cycle net is driven by hand so the test says exactly
       which edge does what; the runtime drives this same net once per cycle. */
    tcsim::Engine engine(built.netlist);
    const tcsim::NetId token = built.cycle_token;
    const tcsim::NetId enable = built.net_of_wire[0];
    const tcsim::NetId data = built.net_of_wire[1];
    const tcsim::NetId register_out = built.net_of_wire[2];
    const tcsim::NetId delay_in = built.net_of_wire[3];
    const tcsim::NetId delay_out = built.net_of_wire[4];
    const tcsim::NetId reset = built.net_of_wire[5];
    const tcsim::NetId counter_out = built.net_of_wire[7];
    const auto byte = [](const char* bits) { return BitVector::fromText(bits, 8); };
    engine.addStimulus(0, token, tcsim::fixtures::one("0"));
    engine.addStimulus(0, enable, tcsim::fixtures::one("1"));
    engine.addStimulus(0, data, byte("00010101"));
    engine.addStimulus(0, reset, tcsim::fixtures::one("0"));
    engine.runUntil(1);
    check(engine.netValue(register_out).toString() == "00000000",
          "a register starts at zero and does not follow its data pin between cycles");

    engine.addStimulus(1, token, tcsim::fixtures::one("1")); /* the cycle boundary */
    engine.runUntil(2);
    check(engine.netValue(register_out).toString() == "00010101", "the register latches on the cycle edge");

    engine.addStimulus(2, enable, tcsim::fixtures::one("0"));
    engine.addStimulus(2, data, byte("10101010"));
    engine.addStimulus(2, token, tcsim::fixtures::one("0"));
    engine.runUntil(3);
    engine.addStimulus(3, token, tcsim::fixtures::one("1"));
    engine.runUntil(4);
    check(engine.netValue(register_out).toString() == "00010101",
          "with its enable low the register holds what it latched");

    engine.addStimulus(4, enable, tcsim::fixtures::one("1"));
    engine.addStimulus(4, token, tcsim::fixtures::one("0"));
    engine.runUntil(5);
    engine.addStimulus(5, token, tcsim::fixtures::one("1"));
    engine.runUntil(6);
    check(engine.netValue(register_out).toString() == "10101010", "and with it high it takes the new value");

    engine.addStimulus(6, delay_in, byte("00111100"));
    engine.addStimulus(6, token, tcsim::fixtures::one("0"));
    engine.runUntil(7);
    engine.addStimulus(7, token, tcsim::fixtures::one("1"));
    engine.runUntil(8);
    check(engine.netValue(delay_out).toString() == "00111100", "the delay line publishes what it saw one cycle earlier");

    /* The counter counts every cycle edge, so the checks above (which already
       made several edges) have to be cleared out of the way first: hold its
       reset across one edge and measure from there. */
    engine.addStimulus(8, reset, tcsim::fixtures::one("1"));
    engine.addStimulus(8, token, tcsim::fixtures::one("0"));
    engine.runUntil(9);
    engine.addStimulus(9, token, tcsim::fixtures::one("1"));
    engine.runUntil(10);
    check(engine.netValue(counter_out).toString() == "00000000",
          "the counter's reset holds it at zero across an edge");
    engine.addStimulus(10, reset, tcsim::fixtures::one("0"));
    engine.addStimulus(10, token, tcsim::fixtures::one("0"));
    engine.runUntil(11);
    engine.addStimulus(11, token, tcsim::fixtures::one("1"));
    engine.runUntil(12);
    check(engine.netValue(counter_out).toString() == "00000001", "one cycle edge counts one");
    engine.addStimulus(12, token, tcsim::fixtures::one("0"));
    engine.runUntil(13);
    engine.addStimulus(13, token, tcsim::fixtures::one("1"));
    engine.runUntil(14);
    check(engine.netValue(counter_out).toString() == "00000010", "and the next counts two");
    engine.addStimulus(14, reset, tcsim::fixtures::one("1"));
    engine.addStimulus(14, token, tcsim::fixtures::one("0"));
    engine.runUntil(15);
    engine.addStimulus(15, token, tcsim::fixtures::one("1"));
    engine.runUntil(16);
    check(engine.netValue(counter_out).toString() == "00000000", "reset takes it back to zero");
}

void testWordWidthResolution() {
    using tcsim::BoardComponent;
    using tcsim::BoardView;
    using tcsim::BoardWire;
    using tcsim::KindPins;

    /* A synthetic board with one word Mux whose record carries the width the
       game resolved (32) and one word Add whose record does not.  The three
       authorities are exercised in one go: the record's own number, the wire
       widths, and the prototype's declared width. */
    BoardView board;
    const auto add_component = [](uint8_t kind, int16_t x, int16_t y, uint8_t inputs, uint8_t outputs,
                                  uint16_t memory_bits) {
        BoardComponent component;
        component.kind = kind;
        component.x = x;
        component.y = y;
        component.input_count = inputs;
        component.output_count = outputs;
        component.memory_bits = memory_bits;
        return component;
    };
    board.components.push_back(add_component(0x2a, 0, 0, 3, 1, 32));  /* Mux */
    board.components.push_back(add_component(0x1e, 20, 0, 3, 2, 0));  /* Add */
    const auto wire = [](int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t width, uint64_t slot) {
        BoardWire value;
        value.x1 = x1;
        value.y1 = y1;
        value.x2 = x2;
        value.y2 = y2;
        value.width = width;
        value.slot = slot;
        return value;
    };
    /* Mux: select 1 bit, A and B 32, output 32. */
    board.wires.push_back(wire(-5, -1, -1, -1, 1, 300));
    board.wires.push_back(wire(-5, 0, -1, 0, 32, 320));
    board.wires.push_back(wire(-5, 1, -1, 1, 32, 360));
    board.wires.push_back(wire(2, 0, 8, 0, 32, 400));
    /* Add: A and B come in as 8-bit wires, the carry in as one bit. */
    board.wires.push_back(wire(15, -1, 19, -1, 8, 500));
    board.wires.push_back(wire(15, 1, 19, 1, 8, 540));
    board.wires.push_back(wire(20, -2, 20, -2, 1, 580));
    board.wires.push_back(wire(22, 0, 28, 0, 8, 600));
    board.wires.push_back(wire(21, 2, 21, 2, 1, 620));

    const auto geometry = [](const BoardComponent& component, KindPins& pins) {
        pins.kind = component.kind;
        if (component.kind == 0x2a) {
            pins.name = "Mux";
            pins.input_count = 3;
            pins.in_x[0] = -1; pins.in_y[0] = -1; pins.in_bits[0] = 0x7fff;
            pins.in_x[1] = -1; pins.in_y[1] = 0;  pins.in_bits[1] = 0x7fff;
            pins.in_x[2] = -1; pins.in_y[2] = 1;  pins.in_bits[2] = 0x7fff;
            pins.output_count = 1;
            pins.out_x[0] = 2; pins.out_y[0] = 0; pins.out_bits[0] = 0x7fff;
            return true;
        }
        if (component.kind == 0x1e) {
            pins.name = "Add";
            pins.input_count = 3;
            pins.in_x[0] = -1; pins.in_y[0] = -1; pins.in_bits[0] = 0x7fff;
            pins.in_x[1] = -1; pins.in_y[1] = 1;  pins.in_bits[1] = 0x7fff;
            pins.in_x[2] = 0;  pins.in_y[2] = -2; pins.in_bits[2] = 1;
            pins.output_count = 2;
            pins.out_x[0] = 2; pins.out_y[0] = 0; pins.out_bits[0] = 0x7fff;
            pins.out_x[1] = 1; pins.out_y[1] = 2; pins.out_bits[1] = 1;
            return true;
        }
        if (component.kind == 0x1a) { /* Equal, deliberately unwired */
            pins.name = "Equal";
            pins.input_count = 2;
            pins.in_x[0] = -1; pins.in_y[0] = -1; pins.in_bits[0] = 0x7fff;
            pins.in_x[1] = -1; pins.in_y[1] = 1;  pins.in_bits[1] = 0x7fff;
            pins.output_count = 1;
            pins.out_x[0] = 2; pins.out_y[0] = 0; pins.out_bits[0] = 1;
            return true;
        }
        return false;
    };
    board.components.push_back(add_component(0x1a, 40, 0, 2, 1, 0)); /* Equal, unwired */
    /* A third word component with neither a record width nor a single wire: the
       one shape where the builder must refuse to invent a width. */
    board.components.push_back(add_component(0x1a, 40, 0, 2, 1, 0)); /* Equal, unwired */

    const tcsim::BuildResult result = tcsim::buildNetList(board, geometry);
    check(!result.fatal(), "the synthetic word board builds a runnable netlist");
    /* Three authorities, three answers: the Mux uses the record's 32, the Add
       falls back to its 8-bit wires, and the unwired Equal is reported rather
       than built at an invented width. */
    check(result.netlist.deviceCount() == 2, "the wired word components become devices and the unwired one does not");
    const tcsim::DeviceInstance& mux = result.netlist.devices()[0];
    check(mux.spec->name == "mux32" && mux.spec->inputs[0].bits == 1 && mux.spec->inputs[1].bits == 32,
          "the Mux's select stays at one bit while its data path takes the record's 32", mux.spec->name);
    const tcsim::DeviceInstance& add = result.netlist.devices()[1];
    check(add.spec->name == "wordbinary0_8" && add.spec->inputs[0].bits == 8 && add.spec->inputs[2].bits == 1 &&
              add.spec->outputs.size() == 2 && add.spec->outputs[1].bits == 1,
          "the Add takes its width from its 8-bit wires and keeps its carry at one bit", add.spec->name);
    bool reported_unresolved = false;
    for (const tcsim::BuildNote& note : result.notes) {
        if (note.text.find("no resolved word width") != std::string::npos) reported_unresolved = true;
    }
    check(reported_unresolved, "an unresolved word width is reported instead of built at one bit");
}

/* The leaf kinds (Splitter / Maker / Concatenator).  Their width comes from
   their own pin shape, and the one convention between them - the top leaf is
   the most significant bit - is what this test pins down. */
void testLeafKinds() {
    using tcsim::BoardComponent;
    using tcsim::BoardView;
    using tcsim::BoardWire;
    using tcsim::BitVector;
    using tcsim::DeviceSpec;
    using tcsim::KindPins;

    const auto apply = [](const DeviceSpec& spec, const std::vector<BitVector>& inputs) {
        std::vector<BitVector> outputs;
        tcsim::EvalContext context;
        spec.behavior(context, inputs, outputs);
        std::string text;
        for (const BitVector& output : outputs) {
            if (!text.empty()) text += " ";
            text += output.toString();
        }
        return text;
    };

    /* A 4-bit Splitter: leaf 0 is the top pin and the most significant bit. */
    const DeviceSpec& split = tcsim::deviceSplitter(4);
    check(split.inputs.size() == 1 && split.inputs[0].bits == 4 && split.outputs.size() == 4 &&
              split.outputs[0].bits == 1,
          "the Splitter takes one 4-bit word and gives four one-bit leaves");
    check(apply(split, {BitVector::fromText("1010", 4)}) == "1 0 1 0",
          "leaf 0 carries the most significant bit, top to bottom", apply(split, {BitVector::fromText("1010", 4)}));
    const DeviceSpec& make = tcsim::deviceMaker(4);
    check(apply(make, {BitVector::fromText("1", 1), BitVector::fromText("0", 1), BitVector::fromText("1", 1),
                       BitVector::fromText("0", 1)}) == "1010",
          "the Maker's first pin is the high end, so it is the Splitter's exact inverse");
    check(apply(make, {BitVector::fromText("1", 1), BitVector::fromText("x", 1), BitVector::fromText("1", 1),
                       BitVector::fromText("0", 1)}) == "1x10",
          "a Maker keeps per-bit truth: a leaf moves one bit and only that bit");

    /* The board builder: an 8-bit Splitter whose wires are the prototype's own
       shape, then the same board with the unmeasured sentinel width. */
    const auto geometry = [](const BoardComponent& component, KindPins& pins) {
        pins.kind = component.kind;
        if (component.kind == 0x11) { /* Splitter: in (1,-1) w8, out (1,-1)..(1,2) w1 */
            pins.name = "Splitter";
            pins.input_count = 1;
            pins.in_x[0] = -1; pins.in_y[0] = 0; pins.in_bits[0] = 8;
            pins.output_count = 8;
            for (uint8_t pin = 0; pin < 8; ++pin) {
                pins.out_x[pin] = 1;
                pins.out_y[pin] = static_cast<int8_t>(pin == 0 ? -3 : (pin == 1 ? -2 : (pin == 2 ? -1 : (pin == 3 ? 0 : pin - 3))));
                pins.out_bits[pin] = 1;
            }
            return true;
        }
        if (component.kind == 0x2f) { /* Splitter whose input is the sentinel */
            pins.name = "Splitter";
            pins.input_count = 1;
            pins.in_x[0] = -1; pins.in_y[0] = 0; pins.in_bits[0] = 0x7ffeu;
            pins.output_count = 2;
            pins.out_x[0] = 1; pins.out_y[0] = -1; pins.out_bits[0] = 0x7fff;
            pins.out_x[1] = 1; pins.out_y[1] = 0;  pins.out_bits[1] = 0x7fff;
            return true;
        }
        return false;
    };
    BoardView board;
    BoardComponent splitter;
    splitter.kind = 0x11;
    splitter.x = 0;
    splitter.y = 0;
    splitter.input_count = 1;
    splitter.output_count = 8;
    board.components.push_back(splitter);
    /* The word arrives on the input pin at (-1,0), and the eight leaves leave
       from (1,-3) down to (1,4).  No leaf sits on the input's own cell, which
       is why the board is legal: a pin cell belongs to exactly one net. */
    BoardWire bus;
    bus.x1 = -4;
    bus.y1 = 0;
    bus.x2 = -1;
    bus.y2 = 0;
    bus.width = 8;
    bus.slot = 800;
    board.wires.push_back(bus);
    for (uint8_t pin = 0; pin < 8; ++pin) {
        BoardWire wire;
        wire.x1 = 1;
        wire.y1 = static_cast<int16_t>(pin - 3);
        wire.x2 = 5;
        wire.y2 = wire.y1;
        wire.width = 1;
        wire.slot = 700 + pin * 2;
        board.wires.push_back(wire);
    }
    const tcsim::BuildResult built = tcsim::buildNetList(board, geometry);
    check(built.netlist.deviceCount() == 1 && built.netlist.devices()[0].spec->name == "splitter8",
          "the Splitter's width comes from its own declared pins, not from a wire");
    check(!built.fatal(), "the Splitter board builds a runnable netlist");

    /* The word Splitters (0x2f / 0x63 / 0x64) are the leaves whose width the
       prototype does *not* pin down - their input is a sentinel and their
       outputs are `wauto` - so the leaf width comes off the board, measured as
       "input = leaves * n" (`kind 0x2F … in0 64 bits -> out0 32, out1 32`).
       With no wire and no record width it must be reported rather than
       invented; with wires it becomes `wordsplit<leaves>x<n>`. */
    BoardView sentinel;
    BoardComponent odd;
    odd.kind = 0x2f;
    odd.input_count = 1;
    odd.output_count = 2;
    sentinel.components.push_back(odd);
    const tcsim::BuildResult refused = tcsim::buildNetList(sentinel, geometry);
    check(refused.netlist.deviceCount() == 0, "an unwired word Splitter builds no device");
    bool reported = false;
    for (const tcsim::BuildNote& note : refused.notes) {
        if (note.text.find("no resolved leaf width") != std::string::npos) reported = true;
    }
    check(reported, "a word Splitter whose leaves are unmeasured is reported, never invented");

    BoardView halves;
    BoardComponent word_splitter;
    word_splitter.kind = 0x2f;
    word_splitter.input_count = 1;
    word_splitter.output_count = 2;
    halves.components.push_back(word_splitter);
    for (int pin = 0; pin < 2; ++pin) {
        BoardWire wire;
        wire.x1 = 1;
        wire.y1 = static_cast<int16_t>(pin - 1);
        wire.x2 = 5;
        wire.y2 = wire.y1;
        wire.width = 4;   /* the halves are four bits each */
        wire.slot = 900 + pin * 2;
        halves.wires.push_back(wire);
    }
    const tcsim::BuildResult built_halves = tcsim::buildNetList(halves, geometry);
    check(built_halves.netlist.deviceCount() == 1 &&
              built_halves.netlist.devices()[0].spec->name == "wordsplit2x4",
          "the word Splitter takes its leaf width from the wires",
          built_halves.netlist.devices().empty() ? "" : built_halves.netlist.devices()[0].spec->name);
}

/* The Static Indexer (0x65).  Its rule is read off the game's own files: the
   `symphony_1_nand` ISA decodes with `(instruction >> 24) & 0b1111`, the level's
   `hint_custom/Instruction Decoder` solution configures its seven indexers with
   exactly those offsets and widths, and `campaign/multiply`'s solution shifts
   left with negative indexes.  See `deviceIndexer`. */
void testStaticIndexer() {
    using tcsim::BitVector;
    using tcsim::DeviceSpec;

    const auto apply = [](const DeviceSpec& spec, const char* text, size_t bits) {
        std::vector<BitVector> outputs;
        tcsim::EvalContext context;
        spec.behavior(context, {BitVector::fromText(text, bits)}, outputs);
        return outputs.empty() ? std::string("-") : outputs[0].toString();
    };

    /* The five fields the ISA decodes, at the widths the solution configures.
       The word below reads `0011` at bit 24, `00` at 29, `1` at 28 and
       `1111_0000_1010_0101` at 0 - worked out from the bit numbering, not from
       this device, so the test can fail. */
    const DeviceSpec& opcode = tcsim::deviceIndexer(24, 4);
    check(apply(opcode, "10010011101011001111000010100101", 32) == "0011",
          "the indexer reads the field at bit 24, four bits wide (the ISA's opcode)");
    const DeviceSpec& mode = tcsim::deviceIndexer(29, 2);
    check(apply(mode, "10010011101011001111000010100101", 32) == "00",
          "the mode field is two bits at bit 29");
    const DeviceSpec& imm_flag = tcsim::deviceIndexer(28, 1);
    check(apply(imm_flag, "10010011101011001111000010100101", 32) == "1",
          "the immediate flag is the single bit at 28");
    check(apply(tcsim::deviceIndexer(20, 4), "10010011101011001111000010100101", 32) == "1010",
          "the destination field is four bits at 20");
    const DeviceSpec& immediate = tcsim::deviceIndexer(0, 16);
    check(apply(immediate, "10010011101011001111000010100101", 32) == "1111000010100101",
          "index 0 with a 16-bit width is the low half of the instruction");
    /* Reading past the input's own width gives zeroes, not garbage. */
    check(apply(tcsim::deviceIndexer(6, 4), "1010", 4) == "0000",
          "a field that starts past the input reads as zeroes");
    /* A negative index shifts left, which is how the multiply table is built. */
    check(apply(tcsim::deviceIndexer(-1, 8), "00000011", 8) == "00000110",
          "index -1 is a left shift by one (the multiply solution's table)");
    check(apply(tcsim::deviceIndexer(-7, 8), "00000001", 8) == "10000000",
          "index -7 shifts the top of a byte off the end, leaving only the sign bit");
    check(apply(tcsim::deviceIndexer(0, 8), "10110011", 8) == "10110011",
          "width 0-ish: index 0 with the full width is the identity");
    /* One unsettled bit settles nothing, the same rule as the word operations. */
    check(apply(opcode, "1001001110101100111100001010x101", 32) == "xxxx",
          "an unknown bit in the word makes the field X");
}

/* The custom component's interface pins (0x4f Input Pin, 0x51 Output Pin).
   Both pass one signal through; the evidence is structural and covers the whole
   corpus (see `devicePin`): every Output Pin in the game's own circuits has its
   input wired and its output unwired, and every Input Pin the other way round,
   which is what a pass-through looks like and what a source or sink does not. */
void testInterfacePins() {
    using tcsim::BitVector;
    using tcsim::DeviceSpec;

    const auto apply = [](const DeviceSpec& spec, const BitVector& input) {
        std::vector<BitVector> outputs;
        tcsim::EvalContext context;
        spec.behavior(context, {input}, outputs);
        return outputs.empty() ? std::string("-") : outputs[0].toString();
    };

    const DeviceSpec& pin = tcsim::devicePin(8);
    check(pin.inputs.size() == 1 && pin.outputs.size() == 1 && pin.inputs[0].bits == 8 &&
              pin.outputs[0].bits == 8,
          "an interface pin is one signal in and the same signal out");
    check(apply(pin, BitVector::fromText("10110011", 8)) == "10110011", "the pin passes its value through");
    check(apply(tcsim::devicePin(1), BitVector::fromText("1", 1)) == "1", "a one-bit pin passes one bit");
    check(apply(pin, BitVector::fromText("1011xx11", 8)) == "1011xx11",
          "a pin keeps per-bit truth: it moves a value, it does not compute one");
    /* A pin with nothing driving it passes the unsettled value on, exactly like
       the input of a composite that nothing outside is feeding yet. */
    const DeviceSpec& floating = tcsim::devicePin(4);
    std::vector<BitVector> outputs;
    tcsim::EvalContext context;
    floating.behavior(context, {}, outputs);
    check(outputs.size() == 1 && outputs[0].toString() == "zzzz",
          "an undriven pin leaves its output high-impedance");

    /* The board builder resolves the pin's width from the signal it carries.
       An Input Pin's output is the only wired pin (the input is the composite's
       boundary), so the width comes from that one wire. */
    tcsim::BoardView board;
    tcsim::BoardComponent pin_component;
    pin_component.kind = 0x4f;
    pin_component.input_count = 1;
    pin_component.output_count = 1;
    board.components.push_back(pin_component);
    tcsim::BoardWire wire;
    wire.x1 = 3;
    wire.y1 = 0;
    wire.x2 = 9;
    wire.y2 = 0;
    wire.width = 16;
    wire.slot = 900;
    board.wires.push_back(wire);
    const auto geometry = [](const tcsim::BoardComponent& component, tcsim::KindPins& pins) {
        pins.kind = component.kind;
        pins.name = "Input Pin";
        pins.input_count = 1;
        pins.in_x[0] = 0; pins.in_y[0] = 0; pins.in_bits[0] = 0x7fff;
        pins.output_count = 1;
        pins.out_x[0] = 3; pins.out_y[0] = 0; pins.out_bits[0] = 0x7fff;
        return true;
    };
    const tcsim::BuildResult built = tcsim::buildNetList(board, geometry);
    check(built.netlist.deviceCount() == 1 && built.netlist.devices()[0].spec->name == "pin16",
          "the Input Pin takes its width from the wire it drives");
    check(!built.fatal(), "a board with an interface pin builds a runnable netlist");
}

/* The Decoder family (0x2b one address bit, 0x2c two, 0x2d three plus an
   enable).  The shape is declared by the prototype table; the one-hot behaviour
   is what the game's own ALU solutions use (a decoder's outputs join the high
   leaves of a 4-input Maker whose low leaf comes from a Splitter, i.e. the
   selection word of a mux).  See `deviceDecoder`. */
void testDecoders() {
    using tcsim::BitVector;
    using tcsim::DeviceSpec;

    const auto apply = [](const DeviceSpec& spec, const std::vector<BitVector>& inputs) {
        std::vector<BitVector> outputs;
        tcsim::EvalContext context;
        spec.behavior(context, inputs, outputs);
        std::string text;
        for (const BitVector& output : outputs) {
            if (!text.empty()) text += " ";
            text += output.toString();
        }
        return text;
    };

    const DeviceSpec& one = tcsim::deviceDecoder(1, false);
    check(one.inputs.size() == 1 && one.inputs[0].bits == 1 && one.outputs.size() == 2,
          "the 1 Bit Decoder is one address bit and two one-bit outputs");
    check(apply(one, {BitVector::fromText("0", 1)}) == "1 0", "address 0 lights the first output");
    check(apply(one, {BitVector::fromText("1", 1)}) == "0 1", "address 1 lights the second output");
    check(apply(one, {BitVector::fromText("x", 1)}) == "x x", "an unsettled address lights nothing for sure");

    const DeviceSpec& two = tcsim::deviceDecoder(2, false);
    check(apply(two, {BitVector::fromText("00", 2)}) == "1 0 0 0" &&
              apply(two, {BitVector::fromText("01", 2)}) == "0 1 0 0" &&
              apply(two, {BitVector::fromText("10", 2)}) == "0 0 1 0" &&
              apply(two, {BitVector::fromText("11", 2)}) == "0 0 0 1",
          "the 2 Bit Decoder is one-hot across four outputs");

    const DeviceSpec& three = tcsim::deviceDecoder(3, true);
    check(three.inputs.size() == 2 && three.inputs[0].bits == 3 && three.inputs[1].bits == 1 &&
              three.outputs.size() == 8,
          "the 3 Bit Decoder is a 3-bit address, an enable and eight outputs");
    check(apply(three, {BitVector::fromText("101", 3), BitVector::fromText("1", 1)}) ==
              "0 0 0 0 0 1 0 0",
          "address 5 lights the sixth output");
    check(apply(three, {BitVector::fromText("101", 3), BitVector::fromText("0", 1)}) ==
              "0 0 0 0 0 0 0 0",
          "a low enable holds every output at 0");
    check(apply(three, {BitVector::fromText("000", 3), BitVector::fromText("x", 1)}) ==
              "x x x x x x x x",
          "an unsettled enable settles nothing");
    /* With the enable left unwired the gate stays enabled, which is the reading
       the corpus supports (it is wired once in eleven appearances). */
    check(apply(three, {BitVector::fromText("000", 3)}) == "1 0 0 0 0 0 0 0",
          "an unwired enable leaves the decoder enabled");
}

void testGameMemoryPorts() {
    tcsim::MemoryImage endian;
    endian.address_bits = 8;
    endian.little_endian = false;
    endian.write(4, 32, 0x11223344u);
    check(endian.readByte(4) == 0x11 && endian.readByte(7) == 0x44,
          "a big-endian game RAM stores the most significant byte first");
    check(endian.read(4, 32) == 0x11223344u, "a big-endian RAM round-trips a 32-bit port value");
    endian.little_endian = true;
    endian.bytes.clear();
    endian.write(4, 32, 0x11223344u);
    check(endian.readByte(4) == 0x44 && endian.readByte(7) == 0x11,
          "a little-endian game RAM stores the least significant byte first");
    check(endian.read(0x104, 32) == 0x11223344u, "the port masks its starting address to RAM capacity");
    endian.write(255, 16, 0xaabbu);
    check(endian.readByte(255) == 0xbb && endian.readByte(256) == 0,
          "a multi-byte access uses the game's zero guard instead of wrapping its tail");

    tcsim::BoardView board;
    tcsim::BoardComponent ram;
    ram.kind = 0x76;
    ram.id = 100;
    ram.memory_bits = 8;
    board.components.push_back(ram);
    const auto add_port = [&](uint8_t kind, int16_t y) {
        tcsim::BoardComponent port;
        port.kind = kind;
        port.y = y;
        port.memory_bits = 32;
        port.input_count = kind == 0x36 ? 3 : 5;
        port.output_count = kind == 0x36 ? 2 : 1;
        board.components.push_back(port);
    };
    add_port(0x36, -16); /* load before the store: observes old data */
    add_port(0x38, -13); /* store */
    add_port(0x36, -10); /* load after the store: observes new data */
    const auto add_wire = [&](int x, int y, uint16_t width) {
        const uint64_t slot = 1000 + board.wires.size() * 16;
        board.wires.push_back(tcsim::BoardWire{static_cast<int16_t>(x), static_cast<int16_t>(y),
                                               static_cast<int16_t>(x - 1), static_cast<int16_t>(y), width, slot});
    };
    /* old load: Enable, Address, Output */
    add_wire(-15, -17, 1);
    add_wire(-15, -16, 32);
    add_wire(16, -17, 32);
    /* store: Enable, Address, Value */
    add_wire(-15, -14, 1);
    add_wire(-15, -13, 32);
    add_wire(-15, -12, 32);
    /* new load: Enable, Address, Output */
    add_wire(-15, -11, 1);
    add_wire(-15, -10, 32);
    add_wire(16, -11, 32);

    const auto geometry = [](const tcsim::BoardComponent& component, tcsim::KindPins& pins) {
        pins = tcsim::KindPins{};
        pins.kind = component.kind;
        if (component.kind == 0x76) { pins.name = "RAM"; return true; }
        if (component.kind == 0x36) {
            pins.name = "Load port";
            pins.input_count = 3;
            pins.output_count = 2;
            pins.in_x[0] = pins.in_x[1] = -15;
            pins.in_y[0] = -1;
            pins.in_y[1] = 0;
            pins.in_x[2] = 0;
            pins.out_x[0] = 16;
            pins.out_y[0] = -1;
            pins.out_x[1] = 1;
            return true;
        }
        if (component.kind == 0x38) {
            pins.name = "Store port";
            pins.input_count = 5;
            pins.output_count = 1;
            pins.in_x[0] = pins.in_x[1] = pins.in_x[2] = -15;
            pins.in_y[0] = -1;
            pins.in_y[1] = 0;
            pins.in_y[2] = 1;
            pins.in_x[3] = 0;
            pins.in_x[4] = -1;
            pins.out_x[0] = 1;
            return true;
        }
        return false;
    };
    tcsim::BuildOptions options;
    options.initialize_memory = [](const tcsim::BoardComponent&, tcsim::MemoryImage& image) {
        image.write(4, 32, 0x11223344u);
    };
    tcsim::BuildResult built = tcsim::buildNetList(board, geometry, options);
    check(!built.fatal() && built.memories.size() == 1, "a RAM and its three socket ports build as one memory");
    check(built.memories[0].ports == std::vector<size_t>({1, 2, 3}),
          "memory ports execute from the top of the socket toward the RAM");
    check(built.netlist.deviceCount() == 3, "the RAM chain contains its three port devices");

    tcsim::Engine engine(built.netlist);
    const auto drive = [&](size_t wire, uint64_t value) {
        const tcsim::NetId net = built.net_of_wire[wire];
        engine.addStimulus(0, net, tcsim::memoryVector(value, built.netlist.net(net).bits));
    };
    drive(0, 1); drive(1, 4);
    drive(3, 1); drive(4, 4); drive(5, 0xaabbccddu);
    drive(6, 1); drive(7, 4);
    engine.addStimulus(0, built.memories[0].order_root, tcsim::memoryVector(0, 8));
    engine.runUntil(0);
    check(engine.netValue(built.net_of_wire[2]).toUint64() == 0x11223344u,
          "a Load port above a Store port reads the old same-address value",
          engine.netValue(built.net_of_wire[2]).toString());
    check(engine.netValue(built.net_of_wire[8]).toUint64() == 0xaabbccddu,
          "a Load port below a Store port reads the newly stored value",
          engine.netValue(built.net_of_wire[8]).toString());
    check(built.memories[0].image->read(4, 32) == 0xaabbccddu,
          "the Store port updates the RAM's persistent private byte state",
          std::to_string(built.memories[0].image->read(4, 32)));
    check(!engine.aborted() && !hasViolation(engine, tcsim::ViolationKind::kIncompleteDevice),
          "all visible and implicit memory-port pins are connected");

    /* The game-facing runtime, rather than a test manually poking the private
       root, must re-run the whole port chain at every cycle boundary. */
    tcsim::RuntimeConfig runtime_config;
    runtime_config.ticks_per_unit = 1;
    runtime_config.cycle_units = 1;
    runtime_config.gate_delay = tcsim::ArcDelay{};
    runtime_config.initialize_memory = options.initialize_memory;
    tcsim::SandboxRuntime memory_runtime;
    check(memory_runtime.bind(board, geometry, runtime_config),
          "the SandboxRuntime binds the game memory board");
    const auto runtime_drive = [&](size_t wire, uint64_t value) {
        const tcsim::NetId net = memory_runtime.build().net_of_wire[wire];
        memory_runtime.engine().addStimulus(
            0, net, tcsim::memoryVector(value, memory_runtime.netlist().net(net).bits));
    };
    runtime_drive(0, 1); runtime_drive(1, 4);
    runtime_drive(3, 1); runtime_drive(4, 4); runtime_drive(5, 0xaabbccddu);
    runtime_drive(6, 1); runtime_drive(7, 4);
    const auto no_sources = [](tcsim::NetId, const std::vector<uint64_t>&) { return uint64_t{0}; };
    memory_runtime.stepCycle(no_sources);
    memory_runtime.stepCycle(no_sources);
    check(memory_runtime.engine().netValue(memory_runtime.build().net_of_wire[2]).toUint64() ==
              0xaabbccddu,
          "runtime cycle two re-runs a held Load and observes cycle one's Store");

    tcsim::BoardView loose;
    tcsim::BoardComponent loose_port;
    loose_port.kind = 0x36;
    loose_port.input_count = 3;
    loose_port.output_count = 2;
    loose_port.memory_bits = 8;
    loose.components.push_back(loose_port);
    const tcsim::BuildResult refused = tcsim::buildNetList(loose, geometry);
    check(refused.fatal(), "an unattached memory port is reported instead of being approximated");
}

/* "The component's own node slot": the emitted program publishes each node into
   the slots named after it, and a node that owns both a pin slot and a wire slot
   stores the same variable twice.  This ties the two sources of truth together -
   the slots the board records hand us, and the slots the program really writes. */
bool testNodeSlotGroups() {
    std::ifstream stores("tests/data/and_gate-stores.txt");
    std::ifstream fixture("tests/data/and_gate-board.txt");
    if (!stores || !fixture) {
        std::printf("SKIP the node slot test: tests/data/and_gate-stores.txt is missing\n");
        return false;
    }
    std::map<std::string, std::vector<uint64_t>> groups; /* variable -> slots */
    std::string line;
    while (std::getline(stores, line)) {
        if (line.empty() || line[0] == '#') continue;
        unsigned long long slot = 0;
        char variable[64] = {0};
        if (std::sscanf(line.c_str(), "store(#SIMULATION_STATE + %llu, U1 (%63[^)]))", &slot, variable) != 2) continue;
        groups[variable].push_back(slot);
    }
    for (auto& entry : groups) {
        std::sort(entry.second.begin(), entry.second.end());
        entry.second.erase(std::unique(entry.second.begin(), entry.second.end()), entry.second.end());
    }
    check(groups.size() == 5, "the program publishes five nodes for the captured board");

    tcsim::BoardView board;
    while (std::getline(fixture, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("component", 0) == 0) {
            tcsim::BoardComponent component;
            unsigned kind = 0;
            long long x = 0, y = 0, rotation = 0;
            std::sscanf(line.c_str(), "component index=%*d kind=0x%x x=%lld y=%lld rotation=%lld", &kind, &x, &y,
                        &rotation);
            component.kind = static_cast<uint8_t>(kind);
            component.x = static_cast<int16_t>(x);
            component.y = static_cast<int16_t>(y);
            component.rotation = static_cast<uint8_t>(rotation);
            const size_t inputs_at = line.find(" word30=");
            const size_t outputs_at = line.find(" word40=");
            if (inputs_at == std::string::npos || outputs_at == std::string::npos) continue;
            component.input_count = static_cast<uint8_t>(std::stoul(line.substr(inputs_at + 8)));
            component.output_count = static_cast<uint8_t>(std::stoul(line.substr(outputs_at + 8)));
            board.components.push_back(component);
            continue;
        }
        if (line.rfind("wire", 0) == 0) {
            tcsim::BoardWire wire;
            long long x1 = 0, y1 = 0, x2 = 0, y2 = 0, width = 0;
            unsigned long long slot = 0;
            std::sscanf(line.c_str(), "wire index=%*d from=%lld,%lld to=%lld,%lld width=%lld slot=%llu", &x1, &y1, &x2,
                        &y2, &width, &slot);
            wire.x1 = static_cast<int16_t>(x1);
            wire.y1 = static_cast<int16_t>(y1);
            wire.x2 = static_cast<int16_t>(x2);
            wire.y2 = static_cast<int16_t>(y2);
            wire.width = static_cast<uint16_t>(width);
            wire.slot = slot;
            board.wires.push_back(wire);
        }
    }
    const auto geometry = [](const tcsim::BoardComponent& component, tcsim::KindPins& pins) {
        const uint8_t kind = component.kind;
        switch (kind) {
            case 0x3f:
                pins.kind = kind;
                pins.name = "Input";
                pins.output_count = 2;
                pins.out_x[0] = 0;
                pins.out_y[0] = -1;
                pins.out_x[1] = 0;
                pins.out_y[1] = 1;
                return true;
            case 0x44:
                pins.kind = kind;
                pins.name = "Output";
                pins.input_count = 1;
                pins.in_x[0] = -1;
                pins.in_y[0] = 0;
                return true;
            case 0x04:
                pins.kind = kind;
                pins.name = "AND";
                pins.input_count = 2;
                pins.in_x[0] = -1;
                pins.in_y[0] = -1;
                pins.in_x[1] = -1;
                pins.in_y[1] = 1;
                pins.output_count = 1;
                pins.out_x[0] = 2;
                pins.out_y[0] = 0;
                return true;
            default: break;
        }
        return false;
    };
    const tcsim::BuildResult result = tcsim::buildNetList(board, geometry);
    std::vector<uint64_t> wire_slots;
    size_t matched = 0;
    for (size_t wire = 0; wire < board.wires.size(); ++wire) {
        std::vector<uint64_t> ours = result.slots_of_net[result.net_of_wire[wire]];
        std::sort(ours.begin(), ours.end());
        for (uint64_t slot : ours) wire_slots.push_back(slot);
        bool found = false;
        for (const auto& entry : groups) {
            if (std::find(entry.second.begin(), entry.second.end(), board.wires[wire].slot) == entry.second.end()) {
                continue;
            }
            found = true;
            check(ours == entry.second, "wire " + std::to_string(wire) +
                                            "'s state bytes are exactly the node group the program writes");
        }
        if (found) ++matched;
    }
    check(matched == 3, "every wire matched a node group");
    size_t orphan = 0;
    for (const auto& entry : groups) {
        for (uint64_t slot : entry.second) {
            if (std::find(wire_slots.begin(), wire_slots.end(), slot) != wire_slots.end()) continue;
            ++orphan;
            check(entry.second.size() == 1, "a slot no wire owns is a single-slot node");
        }
    }
    check(orphan == 2, "the level's own two input pins are the only slots no wire owns");
    return true;
}

/* The S2 runtime core, driven over the same captured board with a stand-in for
   the game's state array.  The array starts as the game leaves it, the runtime
   feeds the two source nets, and after a cycle the AND's own wire must carry
   the AND's value in exactly the bytes the game reads. */
void testSandboxRuntime() {
    std::ifstream fixture("tests/data/and_gate-board.txt");
    if (!fixture) {
        std::printf("SKIP the sandbox runtime test: tests/data/and_gate-board.txt is missing\n");
        return;
    }
    tcsim::BoardView board;
    std::string line;
    while (std::getline(fixture, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("component", 0) == 0) {
            tcsim::BoardComponent component;
            unsigned kind = 0;
            long long x = 0, y = 0, rotation = 0;
            std::sscanf(line.c_str(), "component index=%*d kind=0x%x x=%lld y=%lld rotation=%lld", &kind, &x, &y,
                        &rotation);
            component.kind = static_cast<uint8_t>(kind);
            component.x = static_cast<int16_t>(x);
            component.y = static_cast<int16_t>(y);
            component.rotation = static_cast<uint8_t>(rotation);
            const size_t inputs_at = line.find(" word30=");
            const size_t outputs_at = line.find(" word40=");
            if (inputs_at == std::string::npos || outputs_at == std::string::npos) continue;
            component.input_count = static_cast<uint8_t>(std::stoul(line.substr(inputs_at + 8)));
            component.output_count = static_cast<uint8_t>(std::stoul(line.substr(outputs_at + 8)));
            board.components.push_back(component);
            continue;
        }
        if (line.rfind("wire", 0) == 0) {
            tcsim::BoardWire wire;
            long long x1 = 0, y1 = 0, x2 = 0, y2 = 0, width = 0;
            unsigned long long slot = 0;
            std::sscanf(line.c_str(), "wire index=%*d from=%lld,%lld to=%lld,%lld width=%lld slot=%llu", &x1, &y1, &x2,
                        &y2, &width, &slot);
            wire.x1 = static_cast<int16_t>(x1);
            wire.y1 = static_cast<int16_t>(y1);
            wire.x2 = static_cast<int16_t>(x2);
            wire.y2 = static_cast<int16_t>(y2);
            wire.width = static_cast<uint16_t>(width);
            wire.slot = slot;
            board.wires.push_back(wire);
        }
    }
    const auto geometry = [](const tcsim::BoardComponent& component, tcsim::KindPins& pins) {
        const uint8_t kind = component.kind;
        switch (kind) {
            case 0x3f:
                pins.kind = kind;
                pins.name = "Input";
                pins.output_count = 2;
                pins.out_x[0] = 0;
                pins.out_y[0] = -1;
                pins.out_x[1] = 0;
                pins.out_y[1] = 1;
                return true;
            case 0x44:
                pins.kind = kind;
                pins.name = "Output";
                pins.input_count = 1;
                pins.in_x[0] = -1;
                pins.in_y[0] = 0;
                return true;
            case 0x04:
                pins.kind = kind;
                pins.name = "AND";
                pins.input_count = 2;
                pins.in_x[0] = -1;
                pins.in_y[0] = -1;
                pins.in_x[1] = -1;
                pins.in_y[1] = 1;
                pins.output_count = 1;
                pins.out_x[0] = 2;
                pins.out_y[0] = 0;
                return true;
            default: break;
        }
        return false;
    };

    /* The game's state array, as a byte map: only the bytes the board owns. */
    std::map<uint64_t, unsigned char> memory;
    tcsim::SandboxRuntime runtime;
    tcsim::RuntimeConfig config;
    config.gate_delay = tcsim::ArcDelay{8, 8};
    check(runtime.bind(board, geometry, config), "the runtime binds to the captured board");
    check(runtime.sourceNets().size() == 2 && runtime.observedNets().size() == 1,
          "the runtime found the two sources and the observed net");
    /* The game's own view before we drive anything: both inputs low, so the AND
       is low, and the outputs wire follows. */
    memory[256] = 0;
    memory[258] = 1; /* the second input is high */
    const auto read_source = [&memory](tcsim::NetId, const std::vector<uint64_t>& slots) -> uint64_t {
        return slots.empty() ? 0 : memory[slots[0]] & 1u;
    };
    runtime.stepCycle(read_source);
    const size_t written = runtime.publish([&memory](uint64_t slot, unsigned char byte) { memory[slot] = byte; });
    check(written == 6, "six bytes are published for three nets (the wire slot and its neighbour)");
    const tcsim::NetId output = runtime.netlist().netId("net" + std::to_string(2));
    /* net name comes from the union-find group id, so find it through the wire. */
    const tcsim::NetId by_wire = runtime.build().net_of_wire[2];
    check(by_wire != tcsim::kNoNet && runtime.build().net_of_wire[0] != by_wire,
          "the output wire is its own net");
    (void)output;
    check(memory[260] == 0 && memory[261] == 0, "one input low means the AND's wire reads low");
    memory[256] = 1;
    runtime.stepCycle(read_source);
    runtime.publish([&memory](uint64_t slot, unsigned char byte) { memory[slot] = byte; });
    check(memory[260] == 1 && memory[261] == 1, "both inputs high means the AND's wire reads high");
    check(memory[256] == 1 && memory[257] == 1, "the source wires keep the value the game's array had");
    /* The inverted publish is the ownership experiment, offline: the bytes take
       the opposite value, which is what a real session showed too. */
    runtime.publish([&memory](uint64_t slot, unsigned char byte) { memory[slot] = byte; }, true);
    check(memory[260] == 0, "the test-only inversion reaches the board bytes");
    check(runtime.cycles() == 2, "two cycles were stepped");
    check(!runtime.engine().aborted(), "the captured board simulates without a guard tripping");
}

void testSandboxClockRuntime() {
    tcsim::BoardView board;
    tcsim::BoardComponent clock;
    clock.kind = 0x4e;
    clock.custom_id = 0x434C4F4B5F303031ULL;
    clock.output_count = 1;
    board.components.push_back(clock);
    tcsim::BoardComponent inverter;
    inverter.kind = 0x03;
    inverter.x = 4;
    inverter.input_count = 1;
    inverter.output_count = 1;
    board.components.push_back(inverter);
    tcsim::BoardComponent output;
    output.kind = 0x44;
    output.x = 8;
    output.input_count = 1;
    board.components.push_back(output);
    board.wires.push_back(tcsim::BoardWire{2, 0, 3, 0, 1, 300});
    board.wires.push_back(tcsim::BoardWire{6, 0, 7, 0, 1, 302});
    const auto geometry = [](const tcsim::BoardComponent& component, tcsim::KindPins& pins) {
        pins = tcsim::KindPins{};
        pins.kind = component.kind;
        if (component.kind == 0x4e) {
            pins.name = "Clock";
            pins.output_count = 1;
            pins.out_x[0] = 2;
            return true;
        }
        if (component.kind == 0x03) {
            pins.name = "NOT";
            pins.input_count = pins.output_count = 1;
            pins.in_x[0] = -1;
            pins.out_x[0] = 2;
            return true;
        }
        if (component.kind == 0x44) {
            pins.name = "Output";
            pins.input_count = 1;
            pins.in_x[0] = -1;
            return true;
        }
        return false;
    };
    tcsim::RuntimeConfig config;
    config.ticks_per_unit = 1;
    config.cycle_units = 16;
    config.gate_delay = tcsim::ArcDelay{8, 8};
    tcsim::SandboxRuntime runtime;
    check(runtime.bind(board, geometry, config), "the host clock board binds through custom-id geometry");
    check(runtime.clockNets().size() == 1, "the clock output is an owned event source");
    const auto no_sources = [](tcsim::NetId, const std::vector<uint64_t>&) -> uint64_t { return 0; };
    runtime.stepCycle(no_sources);
    check(runtime.boardByte(runtime.build().net_of_wire[0]) == 1 &&
              runtime.boardByte(runtime.build().net_of_wire[1]) == 0,
          "the first clock edge reaches the NOT output after its arc delay");
    runtime.stepCycle(no_sources);
    check(runtime.boardByte(runtime.build().net_of_wire[0]) == 0 &&
              runtime.boardByte(runtime.build().net_of_wire[1]) == 1,
          "the next owned cycle flips both clock and delayed board output");

    tcsim::BoardView interactive;
    tcsim::BoardComponent source;
    source.kind = 0x4e;
    source.custom_id = 0x535749545F303031ULL; /* SWIT_001 */
    source.output_count = 1;
    interactive.components.push_back(source);
    tcsim::BoardComponent sink;
    sink.kind = 0x44;
    sink.x = 4;
    sink.input_count = 1;
    interactive.components.push_back(sink);
    interactive.wires.push_back(tcsim::BoardWire{2, 0, 3, 0, 1, 400});
    tcsim::SandboxRuntime interactive_runtime;
    check(interactive_runtime.bind(interactive, geometry, config),
          "the host self-lock switch binds as an external simulator source");
    check(interactive_runtime.sourceNets().size() == 1 && interactive_runtime.clockNets().empty(),
          "the self-lock switch is a source, not a simulator-owned clock");
    check(tcsim::customIsInteractiveSource(0x4255544E5F303031ULL),
          "the host momentary button is also an external simulator source");
    interactive_runtime.stepCycle([](tcsim::NetId, const std::vector<uint64_t>& slots) -> uint64_t {
        return !slots.empty() && slots[0] == 400 ? 1 : 0;
    });
    check(interactive_runtime.boardByte(interactive_runtime.build().net_of_wire[0]) == 1,
          "the self-lock switch level is read from its published wire slot");
}

void testCycleUnblock() {
    constexpr size_t component_stride = sandbox_cycle_unblock::kComponentStride;
    constexpr size_t wire_stride = sandbox_cycle_unblock::kWireStride;
    std::vector<unsigned char> component_payload(8 + 3 * component_stride, 0);
    std::vector<unsigned char> wire_payload(8 + 3 * wire_stride, 0);
    const auto put_u64 = [](unsigned char* at, size_t offset, uint64_t value) {
        std::memcpy(at + offset, &value, sizeof(value));
    };
    const auto put_i16 = [](unsigned char* at, size_t offset, int16_t value) {
        std::memcpy(at + offset, &value, sizeof(value));
    };
    for (size_t index = 0; index < 3; ++index) {
        unsigned char* component = component_payload.data() + 8 + index * component_stride;
        component[0] = 0x03; /* NOT */
        put_i16(component, 2, static_cast<int16_t>(index * 4));
        put_u64(component, 8, 100 + index);
        put_u64(component, 0x30, 1);
        put_u64(component, 0x40, 1);
    }
    const int16_t endpoints[3][4] = {{2, 0, 3, 0}, {6, 0, 7, 0}, {10, 0, -1, 0}};
    for (size_t index = 0; index < 3; ++index) {
        unsigned char* wire = wire_payload.data() + 8 + index * wire_stride;
        put_i16(wire, 0x18, endpoints[index][0]);
        put_i16(wire, 0x1a, endpoints[index][1]);
        put_i16(wire, 0x1c, endpoints[index][2]);
        put_i16(wire, 0x1e, endpoints[index][3]);
    }
    unsigned char component_table[16] = {};
    unsigned char wire_table[16] = {};
    put_u64(component_table, 0, 3);
    put_u64(component_table, 8, reinterpret_cast<uint64_t>(component_payload.data()));
    put_u64(wire_table, 0, 3);
    put_u64(wire_table, 8, reinterpret_cast<uint64_t>(wire_payload.data()));
    const auto geometry = [](const tcsim::BoardComponent& component, tcsim::KindPins& pins) {
        if (component.kind != 0x03) return false;
        pins = tcsim::KindPins{};
        pins.kind = component.kind;
        pins.name = "NOT";
        pins.input_count = pins.output_count = 1;
        pins.in_x[0] = -1;
        pins.out_x[0] = 2;
        return true;
    };
    std::vector<sandbox_cycle_unblock::Cut> cuts =
        sandbox_cycle_unblock::cutTables(component_table, wire_table, geometry);
    check(cuts.size() == 1, "the cycle-unblock pass cuts one DFS back edge in a three-NOT ring");
    check(sandbox_cycle_unblock::i16(cuts[0].wire, 0x1c) != cuts[0].x2,
          "the temporary cut moves one wire endpoint away from its pin");
    sandbox_cycle_unblock::restore(cuts);
    check(cuts.empty() && sandbox_cycle_unblock::i16(wire_payload.data() + 8 + 2 * wire_stride, 0x1c) == -1,
          "restoring the cycle-unblock cut leaves the player's closed ring intact");
}

/* Writes one .v and one .expected per fixture for the cross-check script. */
int emitVerilog(const std::string& directory) {
    for (tcsim::fixtures::Fixture& fixture : tcsim::fixtures::all()) {
        if (!fixture.cross_check) continue;
        const tcsim::VerilogExport exported =
            tcsim::exportVerilog(fixture.list, fixture.stimulus, fixture.end, "tb");
        if (!exported.problems.empty()) {
            for (const std::string& problem : exported.problems) {
                std::printf("EXPORT %s: %s\n", fixture.name.c_str(), problem.c_str());
            }
            return 1;
        }
        std::ofstream source(directory + "/" + fixture.name + ".v", std::ios::binary);
        source << exported.text;
        source.close();
        tcsim::EngineOptions options;
        options.trace = true;
        tcsim::Engine engine(fixture.list, options);
        for (const tcsim::StimulusEntry& entry : fixture.stimulus) engine.addStimulus(entry.when, entry.net, entry.value);
        engine.runUntil(fixture.end);
        if (engine.aborted()) {
            std::printf("EXPORT %s: the engine aborted before the end of the run\n", fixture.name.c_str());
            return 1;
        }
        std::ofstream expected(directory + "/" + fixture.name + ".expected", std::ios::binary);
        expected << tcsim::canonicalEvents(engine.netlist(), engine.trace());
        expected.close();
        std::printf("EXPORT %s: %zu nets, %zu events\n", fixture.name.c_str(), engine.netlist().netCount(),
                    engine.trace().size());
    }
    return failures == 0 ? 0 : 1;
}

int printTrace(const std::string& name) {
    for (tcsim::fixtures::Fixture& fixture : tcsim::fixtures::all()) {
        if (fixture.name != name) continue;
        tcsim::Engine engine = engineFor(fixture, true, 200);
        engine.runUntil(fixture.end);
        std::printf("%s", tcsim::canonicalEvents(engine.netlist(), engine.trace()).c_str());
        std::printf("%s", tcsim::violationReport(engine.netlist(), engine.violations()).c_str());
        return engine.aborted() ? 1 : 0;
    }
    std::printf("no fixture named %s\n", name.c_str());
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--emit-verilog") == 0) return emitVerilog(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "--trace") == 0) return printTrace(argv[2]);

    testValueSystem();
    testTruthTables();
    testScheduler();
    testDelayChain();
    testMultiDriver();
    testTriStateSwitch();
    testMux();
    testWordOps();
    testScreen();
    testGateUnknown();
    testRing();
    testDeltaChain();
    testDffShift();
    testSetupHold();
    testZeroDelayLoop();
    testPowerOnSeed();
    testTransportDelays();
    testUndrivenNet();
    testVcd();
    testVerilogExport();
    testCanonicalEvents();
    testBoardNetlist();
    testGameMemoryPorts();
    testWordSplitter();
    testAutoWidthSplitterAndMaker();
    testStorageFamily();
    testWordWidthResolution();
    testLeafKinds();
    testStaticIndexer();
    testInterfacePins();
    testDecoders();
    testSandboxRuntime();
    testSandboxClockRuntime();
    testCycleUnblock();
    testNodeSlotGroups();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
