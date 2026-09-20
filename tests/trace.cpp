/* Offline tests for sdk/tc_trace.h: slot discovery, value reading and the VCD
   file, driven by synthetic history buffers that look like the ones measured in
   the game (input slots at 0 and 8, output slots at 55 and 64). */
#include "../sdk/tc_trace.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kBufferSize = 0x1000;

struct Fake {
    std::vector<unsigned char> input = std::vector<unsigned char>(kBufferSize, 0);
    std::vector<unsigned char> output = std::vector<unsigned char>(kBufferSize, 0);
    unsigned char* inputPointer = input.data();
    unsigned char* outputPointer = output.data();
    void* inputGlobal = &inputPointer;
    void* outputGlobal = &outputPointer;
    int64_t cycle = -1;
};

Fake g_fake;

int64_t fakeCycle() { return g_fake.cycle; }

/* A stand-in for the game's sim_state_read_u64: a byte-addressed state buffer,
   which is what the real function reads. */
std::vector<unsigned char> g_state(0x1000, 0);

uint64_t fakeStateRead(int64_t offset) {
    uint64_t value = 0;
    if (offset < 0 || static_cast<size_t>(offset) + 8 > g_state.size()) return 0;
    std::memcpy(&value, g_state.data() + offset, sizeof(value));
    return value;
}

void* fakeResolve(void*, const char* symbol) {
    if (std::strcmp(symbol, "sim_state_read_u64__modelZsimulator95types_u159") == 0)
        return reinterpret_cast<void*>(&fakeStateRead);
    return nullptr;
}

void writeSlot(std::vector<unsigned char>& buffer, uint64_t offset, uint64_t value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

}  // namespace

int main() {
    tc::TCSimulationModel simulation;
    simulation.get_cycle = &fakeCycle;
    simulation.input_replay = g_fake.inputGlobal;
    simulation.output_history_pins = g_fake.outputGlobal;
    tc::TCGameStateModel state;
    const uint64_t inputs = 2, outputs = 2;
    state.level_used_input = &inputs;
    state.level_used_outputs = &outputs;
    TCHost host{};
    host.context = &host;
    host.resolve_symbol = &fakeResolve;

    tc::trace::Sampler sampler;
    assert(!sampler.load(nullptr, simulation, state));
    assert(sampler.load(&host, simulation, state));
    assert(!sampler.ready() && sampler.rows() == 0);

    /* Four cycles, exactly like the measured and_gate run: inputs 0,1,2,3,
       outputs 0,0,0,1. */
    const uint64_t inputValues[4] = {0, 1, 2, 3};
    const uint64_t outputValues[4] = {0, 0, 0, 1};
    for (int index = 0; index < 4; ++index) {
        g_fake.cycle = index == 0 ? -1 : index;
        writeSlot(g_fake.input, 0, inputValues[index]);
        writeSlot(g_fake.input, 8, inputValues[index]);
        writeSlot(g_fake.output, 55, outputValues[index]);
        writeSlot(g_fake.output, 64, outputValues[index]);
        sampler.sample();
    }
    /* A row is written one call later, so the state it reads has settled; the
       last captured row is flushed by one more call. */
    sampler.sample();
    assert(sampler.rows() == 4);
    assert(sampler.ready() && sampler.inputCount() == 2 && sampler.outputCount() == 2);
    assert(!sampler.assumedStride());
    assert(sampler.cycleAt(0) == -1 && sampler.cycleAt(3) == 3);
    for (int index = 0; index < 4; ++index) {
        assert(sampler.input(index, 0) == inputValues[index]);
        assert(sampler.input(index, 1) == inputValues[index]);
        assert(sampler.output(index, 0) == outputValues[index]);
        assert(sampler.output(index, 1) == outputValues[index]);
    }
    /* Out-of-range reads are zero, not memory. */
    assert(sampler.input(9, 0) == 0 && sampler.output(0, 7) == 0);

    /* Paused simulation: the same cycle cannot add a row. */
    assert(!sampler.sample());
    assert(sampler.rows() == 4);
    g_fake.cycle = 4;
    writeSlot(g_fake.input, 0, 0);
    writeSlot(g_fake.input, 8, 5);
    writeSlot(g_fake.output, 55, 1);
    writeSlot(g_fake.output, 64, 1);
    assert(!sampler.sample());   /* captured */
    assert(sampler.sample());    /* written */
    assert(sampler.rows() == 5 && sampler.lastCycle() == 4);

    /* Watching a subset: unchecked signals leave the drawing list and the VCD,
       but their samples stay in memory so they can be shown again. */
    sampler.setVisible(true, 1, false);
    assert(sampler.visibleInputs() == 1 && sampler.visibleOutputs() == 2);
    /* Hidden means "not drawn / not exported", not "not sampled". */
    assert(!sampler.visible(true, 1) && sampler.input(4, 1) == 5);
    const char* subsetPath = "build/trace-subset-test.vcd";
    assert(sampler.writeVcd(subsetPath));
    std::ifstream subset(subsetPath, std::ios::binary);
    const std::string subsetText((std::istreambuf_iterator<char>(subset)), {});
    assert(subsetText.find("$var wire 64 i0 in0 $end") != std::string::npos);
    assert(subsetText.find("i1 in1") == std::string::npos);
    assert(subsetText.find("$var wire 64 o1 out1 $end") != std::string::npos);
    assert(subsetText.find("#4\n") != std::string::npos);
    std::remove(subsetPath);
    sampler.setVisible(true, 1, true);
    assert(sampler.visibleInputs() == 2);

    /* Extra probes: a wire's state byte (see the handoff doc for where the
       offset in a wire record comes from).  Probes follow the same rows as the
       level's pins and keep their own width. */
    assert(sampler.canProbe());
    /* The state holds a net's value at its own byte offset (the game reads
       sim_state_read_u64(offset) & ((1 << width) - 1)), so a 1-bit net stores
       0/1 and a 2-bit net stores 0..3. */
    g_state[0x120] = 1;
    g_state[0x121] = 1;
    const int bit = sampler.addBitProbe("w0", 0x120, 1);
    const int word = sampler.addBitProbe("w1", 0x121, 2);
    assert(bit == 0 && word == 1 && sampler.probeCount() == 2);
    assert(sampler.probeValue(sampler.rows() - 1, bit) == 1);
    assert(sampler.probeValue(sampler.rows() - 1, word) == 1);
    g_fake.cycle = 5;
    writeSlot(g_fake.input, 0, 0);
    writeSlot(g_fake.input, 8, 0);
    g_state[0x120] = 0;
    g_state[0x121] = 3;
    assert(!sampler.sample());   /* captured */
    assert(sampler.sample());    /* written, probes read now */
    assert(sampler.probeValue(sampler.rows() - 1, bit) == 0);
    assert(sampler.probeValue(sampler.rows() - 1, word) == 3);
    assert(sampler.probeValue(sampler.rows() - 2, word) == 1);
    const char* probePath = "build/trace-probe-test.vcd";
    assert(sampler.writeVcd(probePath));
    std::ifstream probeFile(probePath, std::ios::binary);
    const std::string probeText((std::istreambuf_iterator<char>(probeFile)), {});
    assert(probeText.find("$var wire 1 p0 w0 $end") != std::string::npos);
    assert(probeText.find("$var wire 2 p1 w1 $end") != std::string::npos);
    assert(probeText.find("b0 p0\n") != std::string::npos);
    assert(probeText.find("b11 p1\n") != std::string::npos);
    std::remove(probePath);
    sampler.clearBitProbes();
    assert(sampler.probeCount() == 0);

    const char* path = "build/trace-test.vcd";
    assert(sampler.writeVcd(path));
    std::ifstream file(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    assert(text.find("$timescale\n  1 cycle\n$end") != std::string::npos);
    assert(text.find("$var wire 64 i0 in0 $end") != std::string::npos);
    assert(text.find("$var wire 64 o1 out1 $end") != std::string::npos);
    /* Row 3 (cycle 3): inputs 3, outputs 1 - written as binary vectors. */
    assert(text.find("#3\n") != std::string::npos);
    assert(text.find("b11 i0\n") != std::string::npos);
    assert(text.find("b1 o0\n") != std::string::npos);
    assert(text.find("b0 o0\n") != std::string::npos);
    std::remove(path);

    /* The host can override the pin counts with what the board really has (the
       game's own globals lie: they reported 2/2 for a one-input one-output
       circuit, measured). */
    sampler.setDeclaredCounts(1, 1);
    sampler.sample();
    sampler.sample();
    assert(sampler.inputCount() == 1 && sampler.outputCount() == 1);
    sampler.setDeclaredCounts(0, 0);
    sampler.reset();
    assert(sampler.inputCount() == 0 && sampler.outputCount() == 0);

    /* Restarting the simulation (the cycle going backwards) starts a new
       waveform instead of appending to the old one. */
    g_fake.cycle = -1;
    writeSlot(g_fake.input, 0, 0);
    writeSlot(g_fake.input, 8, 0);
    sampler.sample();
    sampler.sample();
    g_fake.cycle = 5;
    sampler.sample();
    sampler.sample();
    assert(sampler.rows() == 2 && sampler.lastCycle() == 5);
    g_fake.cycle = -1;                    /* the player restarted the run */
    sampler.sample();
    assert(sampler.takeRestartFlag());
    assert(!sampler.takeRestartFlag());
    assert(sampler.rows() == 0);
    sampler.sample();
    assert(sampler.rows() == 1 && sampler.lastCycle() == -1);

    /* A level whose second element never moves: the sampler still reports the
       declared count and flags that it had to assume the stride. */
    g_fake.input.assign(kBufferSize, 0);
    g_fake.output.assign(kBufferSize, 0);
    tc::trace::Sampler sparse;
    assert(sparse.load(&host, simulation, state));
    for (int index = 0; index < 4; ++index) {
        g_fake.cycle = index;
        writeSlot(g_fake.input, 0, static_cast<uint64_t>(index));
        writeSlot(g_fake.output, 55, 1);
        sparse.sample();
    }
    sparse.sample();
    assert(sparse.ready() && sparse.assumedStride());
    assert(sparse.inputCount() == 2 && sparse.outputCount() == 2);
    assert(sparse.input(3, 0) == 3);

    /* Sampling without any buffers must not resolve or crash. */
    tc::TCSimulationModel empty;
    empty.get_cycle = &fakeCycle;
    tc::trace::Sampler none;
    assert(!none.load(&host, empty, state));
    none.sample();
    assert(!none.ready() && none.rows() == 0);

    std::cout << "PASS simulation trace: slot discovery, values, sparse fallback, VCD export\n";
}
