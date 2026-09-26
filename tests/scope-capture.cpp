/* Offline checks for the per-cycle capture kernel (src/scope_capture.hpp).

   The kernel is the part of the scope that must never be wrong: a ring the
   simulation thread writes, a reader on the render thread, the trigger, and the
   gap/restart bookkeeping that says whether the capture really saw every cycle.
   The state reader is a function pointer, so this test installs a synthetic
   one and drives the tick by hand - no game, no threads. */
#include "../src/scope_capture.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    ++failures;
    std::cout << "FAIL " << what << "\n";
}

/* A synthetic simulation state: a byte-addressable array the tick reads through
   the installed reader. */
std::vector<uint8_t> gState;

uint64_t readState(uint64_t offset) {
    uint64_t value = 0;
    if (offset + 8 > gState.size()) return 0;
    std::memcpy(&value, gState.data() + offset, sizeof(value));
    return value;
}

void poke(uint64_t offset, uint64_t value) {
    std::memcpy(gState.data() + offset, &value, sizeof(value));
}

tc::scope_capture::Store& store() { return tc::scope_capture::store(); }

TCSimChannelV1 channel(uint64_t id, uint64_t offset, uint32_t bits) {
    TCSimChannelV1 value{};
    value.size = sizeof(value);
    value.version = TCSIM_CHANNEL_VERSION_1;
    value.channel_id = id;
    value.byte_offset = offset;
    value.bits = bits;
    return value;
}

}  // namespace

int main() {
    gState.assign(256, 0);
    auto& capture = store();
    capture.setStateReader(&readState);

    /* Configuration is validated, and a bad one changes nothing. */
    const TCSimChannelV1 channels[2] = {channel(1, 0, 1), channel(2, 8, 8)};
    check(capture.configure(nullptr, 2, 16, nullptr) == TC_SIM_CAPTURE_ERR_ARGUMENT,
          "a null channel list was accepted");
    check(capture.configure(channels, 0, 16, nullptr) == TC_SIM_CAPTURE_ERR_ARGUMENT,
          "an empty channel list was accepted");
    check(capture.configure(channels, 2, 0, nullptr) == TC_SIM_CAPTURE_ERR_RANGE,
          "a zero depth was accepted");
    TCSimChannelV1 bad = channels[0];
    bad.bits = 0;
    check(capture.configure(&bad, 1, 16, nullptr) == TC_SIM_CAPTURE_ERR_ARGUMENT,
          "a zero-width channel was accepted");
    check(capture.configure(channels, 2, 16, nullptr) == TC_SIM_CAPTURE_OK,
          "a capture could not be configured");
    check(capture.start() == TC_SIM_CAPTURE_OK, "the capture could not be started");

    /* Nothing is recorded until the tick sees a cycle, and each tick adds one
       row with that cycle's value. */
    TCCaptureStatusV1 status{};
    capture.fill(&status);
    check(status.recording == 1 && status.rows == 0 && status.channel_count == 2,
          "a started capture did not reset to empty");
    poke(0, 1);
    poke(8, 0xab);
    capture.tick(0);
    poke(0, 0);
    poke(8, 0xcd);
    capture.tick(1);
    poke(0, 1);
    poke(8, 0xef);
    capture.tick(2);
    uint64_t cycles[8] = {};
    uint64_t values[16] = {};
    uint32_t rows = 8;
    check(capture.read(cycles, values, 8, &rows) == TC_SIM_CAPTURE_OK && rows == 3,
          "three ticks did not leave three rows");
    check(cycles[0] == 0 && cycles[1] == 1 && cycles[2] == 2, "the cycles are wrong");
    check(values[0] == 1 && values[1] == 0xab, "the first row is wrong");
    check(values[2] == 0 && values[3] == 0xcd, "the second row is wrong");
    check(values[4] == 1 && values[5] == 0xef, "the third row is wrong");
    capture.fill(&status);
    check(status.gaps == 0 && status.rows == 3 && status.first_cycle == 0 &&
              status.last_cycle == 2,
          "a clean run reported a gap or the wrong window");

    /* A cycle that jumps means cycles were missed: that is the number a scope
       must be able to show as zero, and here it must not be zero. */
    capture.tick(5);
    capture.fill(&status);
    check(status.gaps == 1, "a skipped cycle was not counted as a gap");

    /* The cycle going backwards is a new run: the capture starts over instead
       of splicing two runs together. */
    capture.tick(0);
    capture.fill(&status);
    check(status.rows == 1 && status.restarts == 1 && status.first_cycle == 0,
          "a restart did not clear the capture");

    /* A compiled program carries more than one per-cycle call site, so one
       cycle can tick twice.  That is not a rewind and not a second row: the
       later report rewrites the row.  Treating it as a restart would clear the
       ring every cycle, which is exactly the one-row window this guards. */
    capture.start();
    for (uint64_t cycle = 0; cycle < 4; ++cycle) {
        poke(0, cycle);
        poke(8, cycle + 1);
        capture.tick(cycle);
        poke(8, 0x80 + cycle); /* the settled values arrive with the second site */
        capture.tick(cycle);
    }
    rows = 8;
    check(capture.read(cycles, values, 8, &rows) == TC_SIM_CAPTURE_OK && rows == 4,
          "a doubly reported cycle did not stay one row");
    check(cycles[0] == 0 && cycles[3] == 3, "the doubly reported cycles are wrong");
    check(values[1] == 0x80 && values[7] == 0x83,
          "the later report of a cycle did not rewrite its row");
    capture.fill(&status);
    check(status.restarts == 0 && status.gaps == 0 && status.written == 4,
          "a repeated cycle was counted as a restart or a gap");

    /* The ring keeps the most recent `depth` cycles and reports the oldest
       first, which is the pre-trigger window. */
    check(capture.configure(channels, 2, 4, nullptr) == TC_SIM_CAPTURE_OK,
          "a short capture could not be configured");
    capture.start();
    for (uint64_t cycle = 0; cycle < 10; ++cycle) {
        poke(8, cycle + 1);
        capture.tick(cycle);
    }
    rows = 8;
    check(capture.read(cycles, values, 8, &rows) == TC_SIM_CAPTURE_OK && rows == 4,
          "the ring did not keep exactly its depth");
    check(cycles[0] == 6 && cycles[3] == 9, "the ring kept the wrong window");
    check(values[1] == 7 && values[7] == 10, "the ring rows are misaligned");
    capture.fill(&status);
    check(status.gaps == 0 && status.written == 10 && status.rows == 4,
          "the ring lost or miscounted rows");
    rows = 2;
    check(capture.read(cycles, values, 2, &rows) == TC_SIM_CAPTURE_ERR_RANGE && rows == 2,
          "a reader with too little room was not told");

    /* The trigger: rising edge on channel 0, and the ring is the pre-trigger
       window, so a caller can draw "so many cycles either side". */
    TCCaptureTriggerV1 trigger{};
    trigger.size = sizeof(trigger);
    trigger.version = TCCAPTURE_TRIGGER_VERSION_1;
    trigger.channel = 0;
    trigger.edge = TCCAPTURE_EDGE_RISING;
    check(capture.configure(channels, 2, 8, &trigger) == TC_SIM_CAPTURE_OK,
          "a capture with a trigger could not be configured");
    TCCaptureTriggerV1 badTrigger = trigger;
    badTrigger.size = 4;
    check(capture.configure(channels, 2, 8, &badTrigger) == TC_SIM_CAPTURE_ERR_ARGUMENT,
          "a short trigger struct was accepted");
    badTrigger = trigger;
    badTrigger.channel = 5;
    check(capture.configure(channels, 2, 8, &badTrigger) == TC_SIM_CAPTURE_ERR_RANGE,
          "a trigger on a channel that does not exist was accepted");
    check(capture.configure(channels, 2, 8, &trigger) == TC_SIM_CAPTURE_OK &&
              capture.start() == TC_SIM_CAPTURE_OK,
          "the trigger capture could not be armed");
    for (uint64_t cycle = 0; cycle < 6; ++cycle) {
        poke(0, cycle < 3 ? 0 : 1);
        capture.tick(cycle);
    }
    capture.fill(&status);
    check(status.triggered == 1 && status.trigger_cycle == 3,
          "the rising edge was not caught at cycle 3");
    check(status.rows == 6, "the trigger run did not keep its rows");

    /* Falling edge and value match, on the second channel. */
    trigger.channel = 1;
    trigger.edge = TCCAPTURE_EDGE_FALLING;
    capture.configure(channels, 2, 8, &trigger);
    capture.start();
    for (uint64_t cycle = 0; cycle < 5; ++cycle) {
        poke(8, cycle < 2 ? 7 : 0);
        capture.tick(cycle);
    }
    capture.fill(&status);
    check(status.triggered == 1 && status.trigger_cycle == 2,
          "the falling edge was not caught at cycle 2");
    trigger.edge = TCCAPTURE_EDGE_MATCH;
    trigger.value = 9;
    trigger.mask = 0;
    capture.configure(channels, 2, 8, &trigger);
    capture.start();
    for (uint64_t cycle = 0; cycle < 4; ++cycle) {
        poke(8, cycle == 2 ? 9 : 1);
        capture.tick(cycle);
    }
    capture.fill(&status);
    check(status.triggered == 1 && status.trigger_cycle == 2,
          "a value match was not caught at cycle 2");

    /* Stopping keeps what was captured; a stopped capture records nothing. */
    capture.stop();
    const uint64_t heldBefore = status.rows;
    capture.tick(100);
    capture.fill(&status);
    check(status.recording == 0 && status.rows == heldBefore,
          "a stopped capture kept recording");

    /* The injection: one call before every per-cycle step, and a program with
       no cycle step is left untouched. */
    std::string source =
        "def mode_run(target: Int) None {\n"
        "    while cycle < target {\n"
        "        cycle += 1 // signals the front end\n"
        "    }\n"
        "}\n";
    const int sites = store().inject(source);
    check(sites == 1, "the cycle step was not instrumented");
    check(source.find("game_engine.'tc_scope_tick'(U64 cycle)\n        cycle += 1") !=
              std::string::npos,
          "the tick was not inserted before the cycle step");
    std::string other = "def other() None {\n    var x = 1\n}\n";
    check(store().inject(other) == 0 && other.find("tc_scope_tick") == std::string::npos,
          "a program with no cycle step was rewritten");

    if (failures == 0)
        std::cout << "PASS scope capture: ring window, gaps, restarts, trigger and injection\n";
    return failures == 0 ? 0 : 1;
}
