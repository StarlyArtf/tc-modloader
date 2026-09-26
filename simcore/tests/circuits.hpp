#pragma once
/* Circuits shared by the unit tests and the iverilog cross-check.

   Staying in one place is the point: `tools/simcore-iverilog.ps1` exports the
   same netlist the unit tests just ran, so "our simulator is right" is decided
   by a Verilog simulator on the same circuit, not by a fixture written to
   match our own answers.  The first four mirror probes that were run against
   iverilog 13.0 by hand while the semantics were pinned down (see
   docs/PLAN-sandbox-simulator.md section 6 and the S1 notes). */

#include "../include/tcsim/devices.hpp"
#include "../include/tcsim/engine.hpp"
#include "../include/tcsim/netlist.hpp"

#include <string>
#include <vector>

namespace tcsim {
namespace fixtures {

struct Fixture {
    std::string name;
    NetList list;
    std::vector<StimulusEntry> stimulus;
    Tick end = 0;
    bool cross_check = true;
};

inline BitVector one(const std::string& text) { return BitVector::fromText(text, 1); }

/* Delays, the rising/falling pair, and inertial pulse filtering: `a` carries a
   pulse too short to reach y1 and a pulse long enough to pass. */
inline Fixture delayChain() {
    Fixture fixture;
    fixture.name = "delay_chain";
    NetList& list = fixture.list;
    const NetId a = list.addNet("a");
    const NetId b = list.addNet("b");
    const NetId y1 = list.addNet("y1");
    const NetId y2 = list.addNet("y2");
    const DeviceId buffer = list.addDevice(&deviceBuffer(ArcDelay{10, 10}), "u_buf");
    list.connect(buffer, "A", a);
    list.connect(buffer, "Y", y1);
    const DeviceId nand = list.addDevice(&logicGate("nand2", 2, '&', true, ArcDelay{8, 12}), "u_nand");
    list.connect(nand, "A", a);
    list.connect(nand, "B", b);
    list.connect(nand, "Y", y2);
    fixture.stimulus = {
        StimulusEntry{0, a, one("0")},   StimulusEntry{0, b, one("0")},
        StimulusEntry{20, a, one("1")},  StimulusEntry{23, a, one("0")},
        StimulusEntry{40, a, one("1")},  StimulusEntry{60, a, one("0")},
        StimulusEntry{90, b, one("1")},
    };
    fixture.end = 140;
    return fixture;
}

/* Two drivers on one net that agree, then disagree, plus a weak driver losing
   to a strong one.  This is the resolution table the plan asks for. */
inline Fixture multiDriver() {
    Fixture fixture;
    fixture.name = "multi_driver";
    NetList& list = fixture.list;
    const NetId a = list.addNet("a");
    const NetId b = list.addNet("b");
    const NetId w0 = list.addNet("w0");
    const NetId w1 = list.addNet("w1");
    const NetId m1 = list.addNet("m1");
    const NetId m2 = list.addNet("m2");
    const NetId m3 = list.addNet("m3");
    const NetId wz = list.addNet("wz");
    const DeviceId drive_a = list.addDevice(&deviceBuffer(ArcDelay{0, 0}), "u_drive_a");
    const DeviceId drive_b = list.addDevice(&deviceBuffer(ArcDelay{0, 0}), "u_drive_b");
    const DeviceId drive_w0 = list.addDevice(&deviceBuffer(ArcDelay{0, 0}), "u_drive_w0");
    const DeviceId drive_w1 = list.addDevice(&deviceBuffer(ArcDelay{0, 0}), "u_drive_w1");
    const DeviceId drive_wz = list.addDevice(&deviceBuffer(ArcDelay{0, 0}), "u_drive_wz");
    list.connect(drive_a, "A", a);
    list.connect(drive_a, "Y", m1, Strength::kStrong);
    list.connect(drive_b, "A", b);
    list.connect(drive_b, "Y", m1, Strength::kStrong);
    list.connect(drive_w0, "A", w0);
    list.connect(drive_w0, "Y", m2, Strength::kWeak);
    list.connect(drive_w1, "A", w1);
    list.connect(drive_w1, "Y", m2, Strength::kStrong);
    list.connect(drive_wz, "A", wz);
    list.connect(drive_wz, "Y", m3, Strength::kStrong);
    fixture.stimulus = {
        StimulusEntry{0, a, one("0")},   StimulusEntry{0, b, one("1")},
        StimulusEntry{0, w0, one("0")},  StimulusEntry{0, w1, one("1")},
        StimulusEntry{0, wz, one("1")},
        StimulusEntry{60, b, one("0")},  StimulusEntry{80, w0, one("1")},
        StimulusEntry{100, w1, one("0")}, StimulusEntry{120, wz, one("z")},
    };
    fixture.end = 160;
    return fixture;
}

/* A gate whose inputs are not 0/1: Z reads as unknown to a gate, 0 dominates an
   AND, 1 dominates an OR - the truth tables the unit tests check bit by bit. */
inline Fixture gateUnknown() {
    Fixture fixture;
    fixture.name = "gate_unknown";
    NetList& list = fixture.list;
    const NetId z = list.addNet("z");
    const NetId x = list.addNet("x");
    const NetId one_net = list.addNet("one");
    const NetId zero_net = list.addNet("zero");
    const NetId nz = list.addNet("nz");
    const NetId g0x = list.addNet("g0x");
    const NetId g1x = list.addNet("g1x");
    const NetId gox = list.addNet("gox");
    const DeviceId tie_one = list.addDevice(&deviceConstant(Logic::kOne), "u_tie_one");
    const DeviceId tie_zero = list.addDevice(&deviceConstant(Logic::kZero), "u_tie_zero");
    list.connect(tie_one, "Y", one_net);
    list.connect(tie_zero, "Y", zero_net);
    const DeviceId nand = list.addDevice(&logicGate("nand2", 2, '&', true, ArcDelay{5, 5}), "u_nand");
    list.connect(nand, "A", z);
    list.connect(nand, "B", one_net);
    list.connect(nand, "Y", nz);
    const DeviceId and0 = list.addDevice(&logicGate("and2", 2, '&', false, ArcDelay{5, 5}), "u_and0");
    list.connect(and0, "A", zero_net);
    list.connect(and0, "B", x);
    list.connect(and0, "Y", g0x);
    const DeviceId and1 = list.addDevice(&logicGate("and2", 2, '&', false, ArcDelay{5, 5}), "u_and1");
    list.connect(and1, "A", one_net);
    list.connect(and1, "B", x);
    list.connect(and1, "Y", g1x);
    const DeviceId or1 = list.addDevice(&logicGate("or2", 2, '|', false, ArcDelay{5, 5}), "u_or1");
    list.connect(or1, "A", one_net);
    list.connect(or1, "B", x);
    list.connect(or1, "Y", gox);
    fixture.stimulus = {
        StimulusEntry{0, z, one("z")},
        StimulusEntry{0, x, one("x")},
    };
    fixture.end = 40;
    return fixture;
}

/* Three inverting stages with equal delays, released by `rst`.  The period is
   six times the stage delay, which both simulators have to agree on. */
inline Fixture ring() {
    Fixture fixture;
    fixture.name = "ring";
    NetList& list = fixture.list;
    const NetId rst = list.addNet("rst");
    const NetId r1 = list.addNet("r1");
    const NetId r2 = list.addNet("r2");
    const NetId r3 = list.addNet("r3");
    const DeviceId stage1 = list.addDevice(&logicGate("nand2", 2, '&', true, ArcDelay{10, 10}), "u_stage1");
    list.connect(stage1, "A", r3);
    list.connect(stage1, "B", rst);
    list.connect(stage1, "Y", r1);
    const DeviceId stage2 = list.addDevice(&deviceNot(ArcDelay{10, 10}), "u_stage2");
    list.connect(stage2, "A", r1);
    list.connect(stage2, "Y", r2);
    const DeviceId stage3 = list.addDevice(&deviceNot(ArcDelay{10, 10}), "u_stage3");
    list.connect(stage3, "A", r2);
    list.connect(stage3, "Y", r3);
    fixture.stimulus = {StimulusEntry{0, rst, one("0")}, StimulusEntry{120, rst, one("1")}};
    fixture.end = 320;
    return fixture;
}

/* Sixty-four zero-delay buffers: one time step, many delta rounds. */
inline Fixture deltaChain(size_t stages = 64) {
    Fixture fixture;
    fixture.name = "delta_chain";
    NetList& list = fixture.list;
    const NetId source = list.addNet("src");
    NetId previous = source;
    for (size_t index = 0; index < stages; ++index) {
        const NetId next = list.addNet("b" + std::to_string(index));
        const DeviceId buffer = list.addDevice(&deviceBuffer(ArcDelay{0, 0}), "u_b" + std::to_string(index));
        list.connect(buffer, "A", previous);
        list.connect(buffer, "Y", next);
        previous = next;
    }
    fixture.stimulus = {StimulusEntry{0, source, one("0")}, StimulusEntry{10, source, one("1")}};
    fixture.end = 40;
    return fixture;
}

/* A shift register: the clock-to-q delay and the NBA layer in one shape. */
inline Fixture dffShift() {
    Fixture fixture;
    fixture.name = "dff_shift";
    NetList& list = fixture.list;
    const NetId d = list.addNet("d");
    const NetId clk = list.addNet("clk");
    const NetId q1 = list.addNet("q1");
    const NetId q2 = list.addNet("q2");
    const NetId q3 = list.addNet("q3");
    const DeviceId ff1 = list.addDevice(&deviceDff(ArcDelay{5, 5}), "u_ff1");
    list.connect(ff1, "D", d);
    list.connect(ff1, "CLK", clk);
    list.connect(ff1, "Q", q1);
    const DeviceId ff2 = list.addDevice(&deviceDff(ArcDelay{5, 5}), "u_ff2");
    list.connect(ff2, "D", q1);
    list.connect(ff2, "CLK", clk);
    list.connect(ff2, "Q", q2);
    const DeviceId ff3 = list.addDevice(&deviceDff(ArcDelay{5, 5}), "u_ff3");
    list.connect(ff3, "D", q2);
    list.connect(ff3, "CLK", clk);
    list.connect(ff3, "Q", q3);
    fixture.stimulus = {
        StimulusEntry{0, clk, one("0")},   StimulusEntry{0, d, one("1")},
        StimulusEntry{10, clk, one("1")},  StimulusEntry{30, clk, one("0")},
        StimulusEntry{40, d, one("0")},    StimulusEntry{50, clk, one("1")},
        StimulusEntry{70, clk, one("0")},  StimulusEntry{90, clk, one("1")},
        StimulusEntry{110, clk, one("0")}, StimulusEntry{130, clk, one("1")},
    };
    fixture.end = 200;
    return fixture;
}

/* A four-bit bus built by a Maker and taken apart by a Splitter: the width
   machinery, and the bit order the plan's pin geometry has to agree with. */
inline Fixture busSplit() {
    Fixture fixture;
    fixture.name = "bus_split";
    NetList& list = fixture.list;
    const NetId packed = list.addNet("packed", 4);
    std::vector<NetId> inputs;
    for (int index = 0; index < 4; ++index) inputs.push_back(list.addNet("in" + std::to_string(index)));
    const DeviceId maker = list.addDevice(&deviceMaker(4), "u_maker");
    for (int index = 0; index < 4; ++index) {
        list.connect(maker, "A" + std::to_string(index), inputs[static_cast<size_t>(index)]);
    }
    list.connect(maker, "Y", packed);
    const DeviceId splitter = list.addDevice(&deviceSplitter(4), "u_splitter");
    list.connect(splitter, "A", packed);
    for (int index = 0; index < 4; ++index) {
        const NetId out = list.addNet("out" + std::to_string(index));
        list.connect(splitter, "Y" + std::to_string(index), out);
    }
    fixture.stimulus = {
        StimulusEntry{0, inputs[0], one("1")}, StimulusEntry{0, inputs[1], one("0")},
        StimulusEntry{0, inputs[2], one("1")}, StimulusEntry{0, inputs[3], one("1")},
        StimulusEntry{20, inputs[0], one("0")}, StimulusEntry{20, inputs[2], one("0")},
    };
    fixture.end = 60;
    return fixture;
}

/* The ring again with every delay set to zero: the engine has to report an
   oscillation instead of spinning.  Nothing to hand to iverilog - it would
   spin as well. */
inline Fixture zeroDelayLoop() {
    Fixture fixture;
    fixture.name = "zero_delay_loop";
    fixture.cross_check = false;
    NetList& list = fixture.list;
    const NetId rst = list.addNet("rst");
    const NetId r1 = list.addNet("r1");
    const NetId r2 = list.addNet("r2");
    const NetId r3 = list.addNet("r3");
    const DeviceId stage1 = list.addDevice(&logicGate("nand2", 2, '&', true, ArcDelay{0, 0}), "u_stage1");
    list.connect(stage1, "A", r3);
    list.connect(stage1, "B", rst);
    list.connect(stage1, "Y", r1);
    const DeviceId stage2 = list.addDevice(&deviceNot(ArcDelay{0, 0}), "u_stage2");
    list.connect(stage2, "A", r1);
    list.connect(stage2, "Y", r2);
    const DeviceId stage3 = list.addDevice(&deviceNot(ArcDelay{0, 0}), "u_stage3");
    list.connect(stage3, "A", r2);
    list.connect(stage3, "Y", r3);
    fixture.stimulus = {StimulusEntry{0, rst, one("0")}, StimulusEntry{10, rst, one("1")}};
    fixture.end = 40;
    return fixture;
}

/* A self-loop that is not an oscillator: one inverter feeding itself settles at
   X and stays there, which is what a real simulator does with `not #0 (y, y)`
   once the value is unknown.  Kept as a fixture because "it must not hang" is
   as much a requirement as "it must converge". */
inline Fixture selfLoop() {
    Fixture fixture;
    fixture.name = "self_loop";
    fixture.cross_check = false;
    NetList& list = fixture.list;
    const NetId source = list.addNet("src");
    const NetId y = list.addNet("y");
    const DeviceId start = list.addDevice(&deviceBuffer(ArcDelay{0, 0}), "u_start");
    list.connect(start, "A", source);
    list.connect(start, "Y", y);
    const DeviceId inverter = list.addDevice(&deviceNot(ArcDelay{0, 0}), "u_inv");
    list.connect(inverter, "A", y);
    list.connect(inverter, "Y", y);
    fixture.stimulus = {StimulusEntry{0, source, one("1")}};
    fixture.end = 20;
    return fixture;
}

inline std::vector<Fixture> all() {
    std::vector<Fixture> fixtures;
    fixtures.push_back(delayChain());
    fixtures.push_back(multiDriver());
    fixtures.push_back(gateUnknown());
    fixtures.push_back(ring());
    fixtures.push_back(deltaChain());
    fixtures.push_back(dffShift());
    fixtures.push_back(busSplit());
    fixtures.push_back(zeroDelayLoop());
    fixtures.push_back(selfLoop());
    return fixtures;
}

}  // namespace fixtures
}  // namespace tcsim
