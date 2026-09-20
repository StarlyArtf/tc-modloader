#ifndef TC_TRACE_H
#define TC_TRACE_H
/* Per-cycle trace of a level's inputs and outputs.

   Where the numbers come from (measured, not guessed): the game keeps the
   level's I/O history in two small buffers reachable from
   tc::TCSimulationModel - `input_replay` and `output_history_pins` - one slot
   per I/O element.  A slot holds the element's value as a little-endian
   64-bit word, and the slots for a level's elements are consecutive:

     and_gate  (declares 2 inputs, 2 outputs)
       input slots at byte 0 and 8     values 0,1,2,3 over cycles -1,1,2,3
       output slots at byte 55 and 64  values 0,0,0,1 over the same cycles

   Those values are exactly what the level's own test drives and expects, which
   is how the layout was confirmed (tests/sim-trace-probe.*).  The slot
   *positions* are not hard-coded here: the sampler discovers them at run time
   by looking for the bytes that move while the level runs, and takes as many
   as the level declares through TCGameStateModel.  A level whose element never
   changes during the sampling window cannot be discovered that way; the
   sampler then assumes the stride it saw between the others (8 for inputs,
   which is what this build uses) and says so through `assumedStride()`.

   Usage:

     tc::trace::Sampler trace;
     // in tc_mod_load, after tc::ui / TCMod load:
     trace.load(host, mod.simulation, mod.state);
     // every frame while the level is on screen:
     trace.sample();
     // once it has a few samples:
     for (size_t i = 0; i < trace.inputCount(); ++i)
         float latest = (float)trace.input(trace.rows() - 1, i);
     trace.writeVcd("C:/path/to/trace.vcd");

   Everything here is read-only: it never writes simulation state. */
#include "tc_mod_api.h"
#include "tc_simulation.h"
#include "tc_game_state.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace tc {
namespace trace {

/* Bytes of each history buffer the sampler looks at.  The measured slots live
   in the first hundred bytes; scanning more would only cost time. */
constexpr uint64_t kScanBytes = 256;
/* Samples needed before the slots can be resolved. */
constexpr size_t kMinimumSamples = 3;

class Sampler {
public:
    bool load(const TCHost* host, const TCSimulationModel& simulation,
              const TCGameStateModel& state) {
        /* Validate before adopting anything: a refused load has to leave the
           sampler invalid, not half-configured. */
        const void* inputGlobal = simulation.inputReplay();
        const void* outputGlobal = simulation.outputHistoryPins();
        if (!host || !inputGlobal || !outputGlobal) {
            host_ = nullptr;
            simulation_ = nullptr;
            return false;
        }
        host_ = host;
        simulation_ = &simulation;
        state_ = &state;
        inputGlobal_ = inputGlobal;
        outputGlobal_ = outputGlobal;
        stateReader_ = host->resolve_symbol
                           ? reinterpret_cast<StateReaderFn>(
                                 host->resolve_symbol(
                                     host->context,
                                     "sim_state_read_u64__modelZsimulator95types_u159"))
                           : nullptr;
        reset();
        return true;
    }

    void reset() {
        samples_.clear();
        cycles_.clear();
        inputSlots_.clear();
        outputSlots_.clear();
        for (Probe& probe : probes_) probe.values.clear();
        pending_ = false;
        pendingSample_ = Sample();
        assumedStride_ = false;
        resolved_ = false;
    }

    bool valid() const { return host_ != nullptr && simulation_ != nullptr; }
    size_t rows() const { return samples_.size(); }
    int inputCount() const { return static_cast<int>(inputSlots_.size()); }
    int outputCount() const { return static_cast<int>(outputSlots_.size()); }
    bool ready() const { return resolved_; }
    bool assumedStride() const { return assumedStride_; }
    /* Resolved slot offsets, for diagnostics and tests. */
    uint64_t inputSlot(int element) const {
        return element >= 0 && element < static_cast<int>(inputSlots_.size())
                   ? inputSlots_[element] : 0;
    }
    uint64_t outputSlot(int element) const {
        return element >= 0 && element < static_cast<int>(outputSlots_.size())
                   ? outputSlots_[element] : 0;
    }
    int64_t cycleAt(size_t row) const { return row < cycles_.size() ? cycles_[row] : -1; }
    int64_t lastCycle() const { return cycles_.empty() ? -1 : cycles_.back(); }
    uint64_t input(size_t row, int element) const { return valueAt(row, element, true); }
    uint64_t output(size_t row, int element) const { return valueAt(row, element, false); }
    /* How many bytes moved in each buffer, i.e. how the slots were found.  A
       count below the declared pin count is what `assumedStride()` warns about,
       and it tells which of the two buffers fell back. */
    int inputMoved() const { return inputMoved_; }
    int outputMoved() const { return outputMoved_; }

    /* Which signals the caller wants to watch.  Hidden elements are still
       sampled and can be shown again later; they are only left out of the
       drawing list and of the VCD. */
    void setVisible(bool isInput, int element, bool visible) {
        std::vector<bool>& flags = isInput ? inputVisible_ : outputVisible_;
        const size_t index = static_cast<size_t>(element);
        if (index >= flags.size()) flags.resize(index + 1, true);
        flags[index] = visible;
    }

    bool visible(bool isInput, int element) const {
        const std::vector<bool>& flags = isInput ? inputVisible_ : outputVisible_;
        const size_t index = static_cast<size_t>(element);
        return index >= flags.size() ? true : flags[index];
    }

    int visibleInputs() const { return countVisible(true); }
    int visibleOutputs() const { return countVisible(false); }

    /* How many level pins to look for.  The game's own `level_used_input` /
       `level_used_outputs` globals are *not* reliable for this: with a one-input
       one-output circuit they still reported 2/2 (measured), so a host that can
       see the board should pass the real counts here.  0 keeps the globals. */
    void setDeclaredCounts(int inputs, int outputs) {
        declaredInputs_ = inputs < 0 ? 0 : inputs;
        declaredOutputs_ = outputs < 0 ? 0 : outputs;
    }

    /* True once after the simulation was restarted (the cycle went backwards),
       in which case the waveform was cleared to start a new run. */
    bool takeRestartFlag() {
        const bool value = restarted_;
        restarted_ = false;
        return value;
    }

    /* Extra signals to watch beside the level's own pins: a "probe" is one
       runtime state slot, addressed by its byte offset in the simulation state
       buffer (a wire record carries that offset; see the handoff doc).  The
       value is read as `width` bits, which is what the game's own
       sim_state_read_bits(index, width) does.  Probes need the state reader, so
       load() has to succeed first. */
    bool canProbe() const { return stateReader_ != nullptr; }

    int addBitProbe(const std::string& label, uint64_t byteOffset, int width) {
        if (!canProbe()) return -1;
        Probe probe;
        probe.label = label;
        probe.offset = byteOffset;
        probe.width = width < 1 ? 1 : (width > 64 ? 64 : width);
        probe.values.assign(samples_.size(), 0);
        probes_.push_back(probe);
        if (!samples_.empty()) {
            probes_.back().values.back() = read(stateReader_, byteOffset, probe.width);
        }
        return static_cast<int>(probes_.size()) - 1;
    }

    void clearBitProbes() { probes_.clear(); }
    int probeCount() const { return static_cast<int>(probes_.size()); }
    const char* probeLabel(int index) const {
        return index >= 0 && index < probeCount() ? probes_[index].label.c_str() : "";
    }
    uint64_t probeOffset(int index) const {
        return index >= 0 && index < probeCount() ? probes_[index].offset : 0;
    }
    int probeWidth(int index) const {
        return index >= 0 && index < probeCount() ? probes_[index].width : 1;
    }
    uint64_t probeValue(size_t row, int index) const {
        if (index < 0 || index >= probeCount()) return 0;
        const std::vector<uint64_t>& values = probes_[index].values;
        return row < values.size() ? values[row] : 0;
    }
    void setProbeVisible(int index, bool visible) {
        if (index >= 0 && index < probeCount()) probes_[index].visible = visible;
    }
    bool probeVisible(int index) const {
        return index >= 0 && index < probeCount() ? probes_[index].visible : false;
    }
    int visibleProbes() const {
        int visibleCount = 0;
        for (const Probe& probe : probes_)
            if (probe.visible) ++visibleCount;
        return visibleCount;
    }

    /* Snapshots both history buffers for the cycle the game is on right now.
       Call it once per frame.

       A row is only added when the simulation actually moved to a new cycle:
       while the game is paused (or before the level's test starts running) the
       cycle does not change, so the panel stops growing instead of filling up
       with identical rows - the waveform is the simulation's, not the render
       loop's.  Returns true when a row was added. */
    bool sample() {
        if (!valid()) return false;
        bool added = false;
        /* The row captured on the previous call is written now: the game updates
           its simulation state as a step finishes, so a probe read at the very
           moment the cycle counter changes still holds the previous cycle's
           value.  Waiting one frame lets the state settle, which is what makes a
           wire probe line up with the level's own history (measured: without the
           wait a probed output wire read 0 where the level's output was 1). */
        if (pending_) {
            samples_.push_back(pendingSample_);
            cycles_.push_back(pendingSample_.cycle);
            for (Probe& probe : probes_)
                probe.values.push_back(read(stateReader_, probe.offset, probe.width));
            pending_ = false;
            added = true;
        }
        const int64_t cycle = simulation_->cycle();
        /* A cycle that goes backwards means the player restarted the simulation:
           start a new waveform instead of appending to the old one. */
        if (!cycles_.empty() && cycle < cycles_.back()) {
            samples_.clear();
            cycles_.clear();
            for (Probe& probe : probes_) probe.values.clear();
            pending_ = false;
            restarted_ = true;
        }
        if (!cycles_.empty() && cycle == cycles_.back()) {
            if (samples_.size() >= 2) resolve();
            return added;
        }
        pendingSample_ = Sample();
        pendingSample_.cycle = cycle;
        read(inputGlobal_, pendingSample_.input);
        read(outputGlobal_, pendingSample_.output);
        pending_ = true;
        /* Re-resolve on every sample instead of freezing the first guess: an
           element that only moves a few cycles in (a level whose test feeds its
           inputs later, or an output that stays 0 for the first cycles) would
           otherwise be missed.  It costs two 256-byte scans per frame and the
           earlier rows are re-read through the corrected slots. */
        if (samples_.size() >= 2) resolve();
        return added;
    }

    /* Standard VCD, one time step per game cycle: openable by GTKWave, Surfer
       and friends.  Returns false when there is nothing to write. */
    bool writeVcd(const char* path, const char* scopeName = "level") const {
        if (!path || samples_.empty()) return false;
        std::FILE* file = std::fopen(path, "wb");
        if (!file) return false;
        std::fprintf(file, "$date\n  generated by tc-modloader\n$end\n");
        std::fprintf(file, "$timescale\n  1 cycle\n$end\n");
        std::fprintf(file, "$scope module %s $end\n", scopeName ? scopeName : "level");
        for (int index = 0; index < inputCount(); ++index) {
            if (!visible(true, index)) continue;
            std::fprintf(file, "$var wire 64 i%d in%d $end\n", index, index);
        }
        for (int index = 0; index < outputCount(); ++index) {
            if (!visible(false, index)) continue;
            std::fprintf(file, "$var wire 64 o%d out%d $end\n", index, index);
        }
        for (int index = 0; index < probeCount(); ++index) {
            if (!probeVisible(index)) continue;
            std::fprintf(file, "$var wire %d p%d %s $end\n", probeWidth(index), index,
                         probeLabel(index));
        }
        std::fprintf(file, "$upscope $end\n$enddefinitions $end\n");
        for (size_t row = 0; row < samples_.size(); ++row) {
            std::fprintf(file, "#%lld\n", static_cast<long long>(cycles_[row]));
            for (int index = 0; index < inputCount(); ++index) {
                if (!visible(true, index)) continue;
                std::fprintf(file, "b%s i%d\n", binary(input(row, index)).c_str(), index);
            }
            for (int index = 0; index < outputCount(); ++index) {
                if (!visible(false, index)) continue;
                std::fprintf(file, "b%s o%d\n", binary(output(row, index)).c_str(), index);
            }
            for (int index = 0; index < probeCount(); ++index) {
                if (!probeVisible(index)) continue;
                std::fprintf(file, "b%s p%d\n", binary(probeValue(row, index)).c_str(), index);
            }
        }
        std::fclose(file);
        return true;
    }

private:
    using StateReaderFn = uint64_t (*)(int64_t);

    struct Sample {
        int64_t cycle = -1;
        unsigned char input[kScanBytes]{};
        unsigned char output[kScanBytes]{};
    };

    struct Probe {
        std::string label;
        uint64_t offset = 0;
        int width = 1;
        bool visible = true;
        std::vector<uint64_t> values;
    };

    /* Mirrors sim_state_read_bits(index, width): read the word at a byte offset
       and keep its low `width` bits. */
    static uint64_t read(StateReaderFn reader, uint64_t byteOffset, int width) {
        if (!reader || width <= 0) return 0;
        const uint64_t word = reader(static_cast<int64_t>(byteOffset));
        if (width >= 64) return word;
        return word & ((static_cast<uint64_t>(1) << width) - 1);
    }

    static void read(const void* global, unsigned char* out) {
        const auto* pointer = static_cast<const unsigned char*>(global);
        unsigned char* buffer = pointer ? *reinterpret_cast<unsigned char* const*>(pointer)
                                        : nullptr;
        if (!buffer || reinterpret_cast<uintptr_t>(buffer) < 0x10000) return;
        std::memcpy(out, buffer, kScanBytes);
    }

    /* VCD vector values are binary, not decimal. */
    static std::string binary(uint64_t value) {
        std::string text;
        for (int bit = 63; bit >= 0; --bit) {
            const bool set = (value >> bit) & 1u;
            if (!set && text.empty()) continue;
            text.push_back(set ? '1' : '0');
        }
        return text.empty() ? std::string("0") : text;
    }

    /* Offsets whose bytes moved while the level ran. */
    std::vector<uint64_t> moved(const bool input) const {
        std::vector<uint64_t> offsets;
        if (samples_.size() < 2) return offsets;
        for (uint64_t offset = 0; offset < kScanBytes; ++offset) {
            const unsigned char first = input ? samples_[0].input[offset]
                                              : samples_[0].output[offset];
            for (size_t row = 1; row < samples_.size(); ++row) {
                const unsigned char value = input ? samples_[row].input[offset]
                                                  : samples_[row].output[offset];
                if (value != first) { offsets.push_back(offset); break; }
            }
        }
        return offsets;
    }

    void resolve() {
        /* The declared counts are read *now*, not at load time: they describe
           the level that is currently loaded, and a plugin may well be alive
           across several levels.  Reading them at load time gave the default
           1-pin level before any level existed - measured, not hypothetical. */
        const auto declared = [&](const uint64_t* value) {
            if (!value) return 0;
            const uint64_t count = *value;
            return count <= 64 ? static_cast<int>(count) : 0;
        };
        expectedInputs_ = declaredInputs_ > 0
                              ? declaredInputs_
                              : declared(state_ ? state_->level_used_input : nullptr);
        expectedOutputs_ = declaredOutputs_ > 0
                               ? declaredOutputs_
                               : declared(state_ ? state_->level_used_outputs : nullptr);
        assumedStride_ = false;
        const std::vector<uint64_t> inputMoved = moved(true);
        const std::vector<uint64_t> outputMoved = moved(false);
        inputMoved_ = static_cast<int>(inputMoved.size());
        outputMoved_ = static_cast<int>(outputMoved.size());
        inputSlots_ = slotsFor(inputMoved, expectedInputs_);
        outputSlots_ = slotsFor(outputMoved, expectedOutputs_);
        resolved_ = !inputSlots_.empty() || !outputSlots_.empty();
    }

    /* The moved offsets are the slots, in order.  When the level declares more
       elements than moved (an element that held still for the whole window),
       the gap between the first two is used as the stride and the rest is
       filled in - the flag records that this happened. */
    std::vector<uint64_t> slotsFor(const std::vector<uint64_t>& offsets, int expected) {
        std::vector<uint64_t> slots = offsets;
        if (expected > 0 && static_cast<int>(slots.size()) > expected)
            slots.resize(static_cast<size_t>(expected));
        if (expected > 0 && static_cast<int>(slots.size()) < expected) {
            assumedStride_ = true;
            const uint64_t first = slots.empty() ? 0 : slots.front();
            const uint64_t stride = slots.size() >= 2 ? slots[1] - slots[0] : 8;
            slots.clear();
            for (int index = 0; index < expected; ++index) slots.push_back(first + stride * index);
        }
        return slots;
    }

    uint64_t valueAt(size_t row, int element, bool input) const {
        const std::vector<uint64_t>& slots = input ? inputSlots_ : outputSlots_;
        if (row >= samples_.size() || element < 0 || element >= static_cast<int>(slots.size()))
            return 0;
        const Sample& entry = samples_[row];
        const unsigned char* bytes = input ? entry.input : entry.output;
        uint64_t value = 0;
        std::memcpy(&value, bytes + slots[element], sizeof(value));
        return value;
    }

    int countVisible(bool isInput) const {
        const int count = isInput ? inputCount() : outputCount();
        int visibleCount = 0;
        for (int index = 0; index < count; ++index)
            if (visible(isInput, index)) ++visibleCount;
        return visibleCount;
    }

    const TCHost* host_ = nullptr;
    const TCSimulationModel* simulation_ = nullptr;
    const TCGameStateModel* state_ = nullptr;
    const void* inputGlobal_ = nullptr;
    const void* outputGlobal_ = nullptr;
    int expectedInputs_ = 0, expectedOutputs_ = 0;
    int declaredInputs_ = 0, declaredOutputs_ = 0;
    int inputMoved_ = 0, outputMoved_ = 0;
    bool resolved_ = false, assumedStride_ = false;
    bool restarted_ = false;
    std::vector<Sample> samples_;
    std::vector<int64_t> cycles_;
    std::vector<uint64_t> inputSlots_, outputSlots_;
    std::vector<bool> inputVisible_, outputVisible_;
    std::vector<Probe> probes_;
    StateReaderFn stateReader_ = nullptr;
    /* Row captured but not yet written: see sample(). */
    bool pending_ = false;
    Sample pendingSample_{};
};

}  // namespace trace
}  // namespace tc

#endif  // TC_TRACE_H
