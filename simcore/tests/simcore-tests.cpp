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
    testSandboxRuntime();
    testSandboxClockRuntime();
    testCycleUnblock();
    testNodeSlotGroups();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
