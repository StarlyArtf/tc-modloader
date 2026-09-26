/* Test-only driver for the per-cycle capture (TC_SERVICE_SIM_CAPTURE).

   It does not navigate anything: example.byte-adder's autotest loads a level,
   compiles it and runs it, and this driver only *captures* - which is exactly
   the division the service is meant to have.  What it reports, once a window
   has filled:

     * the capture's status: rows, first/last cycle, gaps, restarts, trigger;
     * whether the captured cycles are consecutive (gaps = 0 is the assertion a
       scope must be able to make);
     * the row count of the render-thread trace sampler next to the capture's,
       since that reader samples once per frame.

   Channels.  The tick runs inside the compiled program and reads the
   *simulation state buffer* through the game's own reader, so its channels are
   sim-state byte offsets - exactly what tc.sim.channel derives from a wire.
   They are NOT the trace sampler's replay-buffer slot offsets, which is the
   mistake this driver was written to catch: mixing the two spaces captures
   zeros that look like silence.  Without a board handle (this driver never
   enters a level itself) it reads a strip of the state and leaves naming the
   channels to the caller. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../sdk/tc_mod.h"
#include "../sdk/tc_trace.h"
#include "../sdk/tc_simulation.h"
#include "../sdk/tc_sim_capture.h"

#include <cstdio>
#include <string>
#include <vector>

static const TCHost* host = nullptr;
static tc::TCMod mod;
static tc::trace::Sampler trace;
static tc::simulation::Api sim{};
static tc::sim_capture::Api capture{};

static int stage = 0;
static double startedAt = 0.0;
static bool done = false;
static double watchAt = 0.0;
static uint32_t channelCount = 0;
static uint64_t bestRows = 0;
static uint64_t bestGaps = 0;
static int64_t bestFirst = 0;
static int64_t bestLast = 0;

static void say(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

static void finish(const std::string& reason) {
    done = true;
    say("DRIVER: capture " + reason);
}

static TCSimChannelV1 makeChannel(uint64_t id, uint64_t offset, uint32_t bits) {
    TCSimChannelV1 channel{};
    channel.size = sizeof(channel);
    channel.version = TCSIM_CHANNEL_VERSION_1;
    channel.channel_id = id;
    channel.byte_offset = offset;
    channel.bits = bits;
    return channel;
}

/* Reads the ring back and reports it: the numbers a scope would show. */
static void report(const char* why, int traceRows) {
    TCCaptureStatusV1 state{};
    if (tc::sim_capture::status(capture, &state) != TC_SIM_CAPTURE_OK) {
        finish("status unavailable");
        return;
    }
    const uint32_t channels = state.channel_count ? state.channel_count : 1;
    const uint32_t room = state.rows ? static_cast<uint32_t>(state.rows) : 1;
    std::vector<uint64_t> cycles(room);
    std::vector<uint64_t> values(static_cast<std::size_t>(room) * channels);
    uint32_t rows = room;
    const int read = tc::sim_capture::read(capture, cycles.data(), values.data(), room, &rows);
    char line[360];
    std::snprintf(line, sizeof(line),
                  "DRIVER: capture status %s rows=%llu written=%llu first=%lld last=%lld gaps=%llu "
                  "restarts=%llu triggered=%u trigger_cycle=%lld injected=%u channels=%u "
                  "trace_rows=%d read=%d",
                  why, static_cast<unsigned long long>(state.rows),
                  static_cast<unsigned long long>(state.written),
                  static_cast<long long>(state.first_cycle),
                  static_cast<long long>(state.last_cycle),
                  static_cast<unsigned long long>(state.gaps),
                  static_cast<unsigned long long>(state.restarts), state.triggered,
                  static_cast<long long>(state.trigger_cycle), state.injected,
                  state.channel_count, traceRows, read);
    say(line);
    bool consecutive = rows > 0;
    for (uint32_t row = 1; row < rows; ++row)
        if (cycles[row] != cycles[row - 1] + 1) consecutive = false;
    say(std::string("DRIVER: capture consecutive=") + (consecutive ? "1" : "0") +
        " rows_vs_trace=" + std::to_string(static_cast<long long>(rows) - traceRows));
    /* Which channels carried data: a window of constants is not a capture of
       anything, and this is how a caller tells "wrong offset" from "silence". */
    uint32_t live = 0;
    std::string liveList;
    for (uint32_t channel = 0; channel < state.channel_count; ++channel) {
        bool moved = false;
        for (uint32_t row = 1; row < rows && !moved; ++row)
            if (values[static_cast<std::size_t>(row) * channels + channel] !=
                values[channel])
                moved = true;
        if (!moved) continue;
        ++live;
        liveList += (liveList.empty() ? "" : ",") + std::to_string(channel);
    }
    say("DRIVER: capture live channels=" + std::to_string(live) + "/" +
        std::to_string(state.channel_count) + " columns=" + (liveList.empty() ? "-" : liveList));
    const uint32_t shown = rows < 8 ? rows : 8;
    for (uint32_t row = 0; row < shown; ++row) {
        std::string text = "DRIVER: capture row " + std::to_string(row) + " cycle=" +
                           std::to_string(static_cast<long long>(cycles[row])) + " values=";
        for (uint32_t channel = 0; channel < state.channel_count; ++channel) {
            if (channel) text += ",";
            text += std::to_string(static_cast<unsigned long long>(
                values[static_cast<std::size_t>(row) * channels + channel]));
        }
        say(text);
    }
}

static void tick(void*, const TCFrame* frame) {
    if (!frame || done) return;
    if (startedAt == 0.0) {
        startedAt = frame->time_seconds;
        say("DRIVER: capture driver armed");
    }
    const double elapsed = frame->time_seconds - startedAt;
    /* The level's own trace keeps running next to the capture so the two row
       counts can be compared; it is not a channel source here. */
    trace.sample();
    const int traceRows = static_cast<int>(trace.rows());

    if (stage == 0 && elapsed > 6.0) {
        /* A strip of the state, eight bytes apart: the level's own I/O and its
           wires live in the first bytes, and a caller that knows the wires asks
           tc.sim.channel instead. */
        /* The level's own I/O wires live a few hundred bytes into the state
           (the and_gate level's probed wire sits at 260), not at zero, and the
           capture service takes at most 64 channels - so the strip is 64 words
           wide.  A caller with a board handle does not need this: tc.sim.channel
           names the exact slot of the wire it cares about. */
        const uint64_t size = tc::simulation::stateSize(sim);
        const uint64_t scan = size >= 512 ? 512 : size;
        std::vector<TCSimChannelV1> channels;
        for (uint64_t offset = 0; offset + 8 <= scan; offset += 8)
            channels.push_back(makeChannel(offset / 8 + 1, offset, 8));
        if (channels.empty()) {
            finish("gave up: the simulation state buffer is empty");
            return;
        }
        channelCount = static_cast<uint32_t>(channels.size());
        const int configured = tc::sim_capture::configure(capture, channels.data(), channelCount,
                                                          4096, nullptr);
        const int started = configured == TC_SIM_CAPTURE_OK ? tc::sim_capture::start(capture)
                                                            : configured;
        if (configured != TC_SIM_CAPTURE_OK || started != TC_SIM_CAPTURE_OK) {
            finish("could not be armed (configure=" + std::to_string(configured) +
                   ", start=" + std::to_string(started) + ")");
            return;
        }
        say("DRIVER: capture armed on " + std::to_string(channelCount) +
            " channel(s) at state offsets 0.." + std::to_string((channelCount - 1) * 8) +
            " of " + std::to_string(size) + " bytes, trace rows=" + std::to_string(traceRows));
        stage = 1;
        watchAt = elapsed;
        return;
    }
    if (stage != 1) return;

    TCCaptureStatusV1 state{};
    tc::sim_capture::status(capture, &state);
    if (state.rows > bestRows) {
        bestRows = state.rows;
        bestGaps = state.gaps;
        bestFirst = state.first_cycle;
        bestLast = state.last_cycle;
    }
    if (elapsed >= watchAt + 1.0) {
        watchAt = elapsed;
        char line[240];
        std::snprintf(line, sizeof(line),
                      "DRIVER: capture watch t=%.1f rows=%llu written=%llu first=%lld "
                      "last=%lld gaps=%llu restarts=%llu",
                      elapsed, static_cast<unsigned long long>(state.rows),
                      static_cast<unsigned long long>(state.written),
                      static_cast<long long>(state.first_cycle),
                      static_cast<long long>(state.last_cycle),
                      static_cast<unsigned long long>(state.gaps),
                      static_cast<unsigned long long>(state.restarts));
        say(line);
    }
    /* A scope freezes on a window that is full and has no holes in it; waiting
       for a fixed moment instead can catch a window a new run just emptied. */
    if (state.rows >= 30 && state.gaps == 0) {
        tc::sim_capture::stop(capture);
        report("full", traceRows);
        finish("finished after " + std::to_string(state.rows) + " consecutive rows");
        return;
    }
    if (elapsed < 40.0) return;
    tc::sim_capture::stop(capture);
    report("timeout", traceRows);
    finish("gave up: the ring never held 30 consecutive cycles (best " +
           std::to_string(static_cast<unsigned long long>(bestRows)) + " rows, cycles " +
           std::to_string(static_cast<long long>(bestFirst)) + ".." +
           std::to_string(static_cast<long long>(bestLast)) + ", gaps " +
           std::to_string(static_cast<unsigned long long>(bestGaps)) + ")");
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE || !out ||
        out->size < sizeof(TCPlugin))
        return 1;
    host = h;
    if (!mod.load(h)) return 2;
    if (!tc::simulation::table(h, &sim)) {
        say("DRIVER: tc.simulation V2 unavailable; the loader is too old");
        return 3;
    }
    if (!tc::sim_capture::table(h, &capture)) {
        say("DRIVER: tc.sim.capture unavailable; the loader is too old");
        return 3;
    }
    trace.load(h, mod.simulation, mod.state);
    out->on_frame = &tick;
    say("DRIVER: scope capture driver loaded");
    return 0;
}
