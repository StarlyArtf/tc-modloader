/* ?????**??**????????????????????????

   ? example.waveform-demo ???????????????? I/O ?????
   ?????????????????? `tc.sim.capture`?????????
   ???????? tick ??????????????????`gaps` ??? 0?
   ?????????

   ???? `tc.sim.channel`??????????????wire record ??
   ?????? + ???????? tick ???????????????????
   ????? wire id??????????

   `tc.sim.capture` ??"???????"????????????????
   ?????????? ? Arm ? ???? ? ??????????Follow ???
   ???????????????? ? ? ???????????? */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../sdk/tc_ui.h"
#include "../../sdk/tc_game_ui.h"
#include "../../sdk/tc_ui_draw.h"
#include "../../sdk/tc_event.h"
#include "../../sdk/tc_handle_api.h"
#include "../../sdk/tc_sim_channel.h"
#include "../../sdk/tc_sim_capture.h"
#include "../../sdk/tc_simulation.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kMaxChannels = 12;      /* lanes the panel can show at once */
constexpr uint64_t kDepth = 1024;     /* cycles the ring keeps (pre-trigger window) */
constexpr int kMaxWires = 512;        /* wire handles read from the board each refresh */
constexpr float kLaneHeight = 26.f;
constexpr float kGutter = 104.f;      /* left column: lane labels */
constexpr int kRefreshFrames = 20;    /* how often the wire set is re-read */

const TCHost* host = nullptr;
tc::simulation::Api sim{};
tc::sim_capture::Api capture{};
tc::sim_channel::Api wires{};
TCBoardApiV6 board{};
bool ready = false;
/* The board model the game handed to level.load.  The handle issued for the same
   event is what the panel should read, but keeping the model lets a failure say
   *which* board was empty instead of just "no wires". */
void* levelBoardModel = nullptr;
std::string lastBoardSignature;

/* Channels of the capture, in lane order. */
std::vector<TCSimChannelV1> channels;
std::vector<TCSimWireChannelV1> wireChannels;
std::vector<std::string> labels;
std::vector<char> visible_;
std::string fingerprint;
std::vector<TCGameHandle> handleBuffer;   /* reused: a board can have thousands of wires */
std::vector<TCGameHandle> componentBuffer;

/* Trigger: -1 = off, otherwise an index into `channels`. */
int triggerChannel = -1;
int triggerEdge = TCCAPTURE_EDGE_RISING;
uint64_t triggerValue = 1;

/* View. */
float cyclesPerPixel = 1.f;
uint64_t panCycles = 0;               /* cycles between the window's right edge and the newest row */
bool follow = true;
int64_t cursorA = -1;
int64_t cursorB = -1;

/* The ring, read back once per frame while armed. */
std::vector<uint64_t> rowCycles;
std::vector<uint64_t> rowValues;
uint32_t rowCount = 0;
TCCaptureStatusV1 status_{};
bool armed = false;
bool statusValid = false;

int refreshCountdown = 0;
int lastReportedRows = -1;
std::string lastStateLine;
bool loggedFirstDraw = false;
bool loggedWindow = false;
bool loggedUnavailable = false;
std::string lastFailure;

void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

/* One line per distinct reason the panel has no lanes yet.  Without it "the
   scope is empty" and "the board service refused" look the same in the log. */
void noteOnce(const std::string& reason) {
    if (reason == lastFailure) return;
    lastFailure = reason;
    report("Scope: " + reason);
}

void reportStatus(const std::string& message, int level) {
    report(message);
    if (host) tc::reportStatus(host, level, message.c_str());
}

void onLevelLoadEvent(TCEvent* event) {
    if (!tc::events::is(event, TC_EVENT_LEVEL_LOAD)) return;
    levelBoardModel = tc::events::levelBoardModel(event);
}

/* A wire's lane label: the endpoints say which wire it is on the board. */
std::string wireLabel(const TCSimWireChannelV1& channel, size_t index) {
    char text[96];
    std::snprintf(text, sizeof(text), "%u:(%d,%d)-(%d,%d) w%u", static_cast<unsigned>(index),
                  channel.x1, channel.y1, channel.x2, channel.y2,
                  static_cast<unsigned>(channel.bits));
    return text;
}

/* Re-reads the board's wires and rebuilds the capture's channels.  Returns true
   when the channel set changed (and the capture was re-armed). */
bool refreshChannels() {
    if (!host || !ready) return false;
    TCGameHandle handle{};
    if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &handle) != TC_HANDLE_OK) {
        noteOnce("no board handle");
        return false;
    }
    /* The board service is a prefix-compatible chain (V6 starts with every V3
       entry), so the shared helper is called through the entry it needs. */
    if (!board.capture_objects ||
        board.size < offsetof(TCBoardApiV3, capture_objects) + sizeof(board.capture_objects)) {
        noteOnce("the board service has no object snapshot");
        return false;
    }
    /* Count first: a capacity that is too small comes back as ERR_CAPACITY
       (measured: a byte-adder board has more than 512 wires, so a fixed guess
       never resolved a single lane). */
    TCBoardObjectBuffersV1 probe{};
    probe.size = sizeof(probe);
    probe.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    TCBoardObjectSnapshotV1 counts{};
    const int countStatus = board.capture_objects(board.context, &handle, &counts,
                                                  static_cast<uint32_t>(sizeof(counts)), &probe);
    /* The count-only call answers ERR_CAPACITY *and* fills the counts in - that
       is how a caller learns how much room it needs. */
    if (countStatus != TC_SNAPSHOT_OK && countStatus != TC_SNAPSHOT_ERR_CAPACITY) {
        noteOnce("the board object count failed (status " + std::to_string(countStatus) + ")");
        return false;
    }
    /* Which board is this, and what does it hold?  One line per distinct answer:
       "the handle is not the board the level loaded" and "the level's board is
       really empty" need different fixes, and the counts alone cannot tell them
       apart. */
    const void* boardRaw = nullptr;
    board.resolve(board.context, &handle, &boardRaw);
    char signature[160];
    std::snprintf(signature, sizeof(signature), "board@%p components=%llu wires=%llu",
                  boardRaw, static_cast<unsigned long long>(counts.component_count),
                  static_cast<unsigned long long>(counts.wire_count));
    if (lastBoardSignature != signature) {
        lastBoardSignature = signature;
        report(std::string("Scope: ") + signature);
    }
    if (counts.wire_count == 0) {
        std::string detail = "the board has no wires yet (components " +
                             std::to_string(counts.component_count) + ")";
        /* Same read, but from the model the level-load event carried: if the two
           disagree, the handle and the level are not the same board and that is
           the thing to fix, not the panel. */
        noteOnce(detail);
        return false;
    }
    /* Both arrays have to fit, even though only the wires are used: the loader
       refuses the whole snapshot when either capacity is short. */
    if (handleBuffer.size() < static_cast<size_t>(counts.wire_count))
        handleBuffer.resize(static_cast<size_t>(counts.wire_count));
    if (componentBuffer.size() < static_cast<size_t>(counts.component_count))
        componentBuffer.resize(static_cast<size_t>(counts.component_count));
    std::vector<TCGameHandle>& handles = handleBuffer;
    TCBoardObjectBuffersV1 buffers{};
    buffers.size = sizeof(buffers);
    buffers.version = TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;
    buffers.components = componentBuffer.data();
    buffers.component_capacity = componentBuffer.size();
    buffers.wires = handles.data();
    buffers.wire_capacity = handles.size();
    TCBoardObjectSnapshotV1 snapshot{};
    int snapshotStatus = board.capture_objects(board.context, &handle, &snapshot,
                                               static_cast<uint32_t>(sizeof(snapshot)), &buffers);
    if (snapshotStatus == TC_SNAPSHOT_ERR_RETRY)
        snapshotStatus = board.capture_objects(board.context, &handle, &snapshot,
                                               static_cast<uint32_t>(sizeof(snapshot)), &buffers);
    if (snapshotStatus != TC_SNAPSHOT_OK) {
        noteOnce("the board object snapshot failed (status " + std::to_string(snapshotStatus) + ")");
        return false;
    }

    std::string candidate;
    std::vector<TCSimWireChannelV1> found;
    const uint64_t count = snapshot.wire_written < handles.size() ? snapshot.wire_written
                                                                  : handles.size();
    for (uint64_t index = 0; index < count && found.size() < kMaxChannels; ++index) {
        TCSimWireChannelV1 channel{};
        if (tc::sim_channel::fromWire(wires, &handles[index], &channel) != TC_SIM_CHANNEL_OK)
            continue;
        if (!tc::sim_channel::live(channel)) continue;
        candidate += std::to_string(channel.wire_id) + ",";
        found.push_back(channel);
    }
    if (found.empty()) {
        noteOnce("the board has " + std::to_string(snapshot.wire_count) +
                 " wire(s), none with a state slot yet");
        return false;
    }
    candidate += "|t" + std::to_string(triggerChannel) + "e" + std::to_string(triggerEdge);
    if (candidate == fingerprint) return false;
    fingerprint = candidate;

    wireChannels = found;
    channels.clear();
    labels.clear();
    visible_.assign(found.size(), 1);
    for (size_t index = 0; index < found.size(); ++index) {
        TCSimChannelV1 channel{};
        channel.size = sizeof(channel);
        channel.version = TCSIM_CHANNEL_VERSION_1;
        channel.channel_id = static_cast<uint64_t>(index) + 1;
        channel.byte_offset = found[index].byte_offset;
        channel.bits = found[index].bits;
        channels.push_back(channel);
        labels.push_back(wireLabel(found[index], index));
    }
    TCCaptureTriggerV1 trigger{};
    trigger.size = sizeof(trigger);
    trigger.version = TCCAPTURE_TRIGGER_VERSION_1;
    TCCaptureTriggerV1* triggerPointer = nullptr;
    if (triggerChannel >= 0 && triggerChannel < static_cast<int>(channels.size())) {
        trigger.channel = static_cast<uint32_t>(triggerChannel);
        trigger.edge = static_cast<uint32_t>(triggerEdge);
        trigger.value = triggerValue;
        trigger.mask = ~uint64_t{0};
        triggerPointer = &trigger;
    }
    const int configured = tc::sim_capture::configure(capture, channels.data(),
                                                      static_cast<uint32_t>(channels.size()),
                                                      static_cast<uint32_t>(kDepth), triggerPointer);
    const int started = configured == TC_SIM_CAPTURE_OK ? tc::sim_capture::start(capture)
                                                        : configured;
    armed = configured == TC_SIM_CAPTURE_OK && started == TC_SIM_CAPTURE_OK;
    rowCount = 0;
    cursorA = cursorB = -1;
    panCycles = 0;
    report("Scope: armed on " + std::to_string(channels.size()) + " wire(s); configure=" +
           std::to_string(configured) + " start=" + std::to_string(started) +
           (triggerPointer ? "; trigger on lane " + std::to_string(triggerChannel) : "; no trigger"));
    return true;
}

/* Copies the ring into the row buffers.  One read per frame: the writer is the
   simulation thread and the handover is the ring's sequence number, so what is
   copied is a consistent window. */
void readRing() {
    statusValid = false;
    if (!armed || channels.empty()) return;
    if (tc::sim_capture::status(capture, &status_) != TC_SIM_CAPTURE_OK) return;
    statusValid = true;
    if (status_.channel_count == 0) return;
    const uint32_t room = static_cast<uint32_t>(kDepth);
    if (rowCycles.size() < room) rowCycles.assign(room, 0);
    const size_t values = static_cast<size_t>(room) * status_.channel_count;
    if (rowValues.size() < values) rowValues.assign(values, 0);
    uint32_t rows = room;
    const int read = tc::sim_capture::read(capture, rowCycles.data(), rowValues.data(), room, &rows);
    rowCount = read == TC_SIM_CAPTURE_OK || read == TC_SIM_CAPTURE_ERR_RANGE ? rows : 0;
}

uint64_t valueAt(uint32_t row, size_t channel) {
    if (!statusValid || row >= rowCount || channel >= status_.channel_count) return 0;
    return rowValues[static_cast<size_t>(row) * status_.channel_count + channel];
}

/* Best value for a channel inside a cycle range: the last row at or before the
   cursor.  A cursor that sits between cycles reads the older one, like a scope
   reading the level at that instant. */
bool valueAtCycle(size_t channel, int64_t cycle, uint64_t* out) {
    if (rowCount == 0 || !statusValid) return false;
    for (uint32_t row = rowCount; row-- > 0;) {
        if (static_cast<int64_t>(rowCycles[row]) <= cycle) {
            *out = valueAt(row, channel);
            return true;
        }
    }
    *out = valueAt(0, channel);
    return true;
}

std::string numberText(uint64_t value, unsigned bits) {
    if (bits <= 1) return value ? "1" : "0";
    char text[32];
    if (bits <= 8) std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
    else std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

tc::ui::Color laneColor(size_t index) {
    using tc::ui::rgba;
    static const tc::ui::Color palette[] = {
        rgba(90, 170, 235), rgba(245, 185, 70), rgba(120, 220, 140), rgba(235, 120, 200),
        rgba(140, 160, 250), rgba(230, 230, 120), rgba(120, 220, 230), rgba(250, 140, 120),
        rgba(200, 150, 250), rgba(160, 230, 180), rgba(240, 210, 140), rgba(160, 200, 250),
    };
    return palette[index % (sizeof(palette) / sizeof(palette[0]))];
}

/* One lane.  A one-bit signal is a square wave; a wider net is a data band with
   its value written where it changes - that is what a bus looks like on a scope. */
void drawLane(tc::ui::Canvas& canvas, float top, size_t channel, uint32_t firstRow,
              uint32_t lastRow, float width) {
    using tc::ui::rgba;
    const tc::ui::Color color = laneColor(channel);
    const unsigned bits = channel < channels.size() ? channels[channel].bits : 1;
    const float labelX = 4.f;
    canvas.text({labelX, top + 2.f}, rgba(200, 205, 215), labels[channel].c_str());
    const float left = kGutter;
    const float right = width - 6.f;
    if (lastRow <= firstRow || right <= left) return;

    const float high = top + 6.f, low = top + kLaneHeight - 8.f;
    const float middle = (high + low) * 0.5f;
    const float perRow = (right - left) / static_cast<float>(lastRow - firstRow);
    float previousX = left;
    uint64_t previousDrawn = valueAt(firstRow, channel);
    for (uint32_t row = firstRow; row < lastRow; ++row) {
        const float x = left + static_cast<float>(row - firstRow) * perRow;
        const uint64_t value = valueAt(row, channel);
        if (value != previousDrawn) {
            if (bits <= 1) {
                const float y0 = previousDrawn ? high : low;
                const float y1 = value ? high : low;
                canvas.line({x, y0}, {x, y1}, color);
                canvas.line({previousX, y0}, {x, y0}, color);
            } else {
                canvas.line({x, previousDrawn ? high : low}, {x, value ? high : low}, color);
                if (x - previousX > 22.f)
                    canvas.text({previousX + 2.f, middle - 8.f}, color,
                                numberText(previousDrawn, bits).c_str());
            }
            previousX = x;
            previousDrawn = value;
        }
    }
    const float lastY = bits <= 1 ? (previousDrawn ? high : low) : middle;
    canvas.line({previousX, lastY}, {right, lastY}, color);
    if (bits > 1 && right - previousX > 22.f)
        canvas.text({previousX + 2.f, middle - 8.f}, color, numberText(previousDrawn, bits).c_str());
}

/* Vertical time grid: a line every `step` cycles with the cycle number. */
void drawTimeGrid(tc::ui::Canvas& canvas, int64_t firstCycle, int64_t lastCycle, float top,
                  float bottom, float left, float right, float pixelsPerCycle) {
    using tc::ui::rgba;
    const tc::ui::Color grid = rgba(52, 57, 70);
    const int64_t span = lastCycle - firstCycle;
    if (span <= 0) return;
    int64_t step = 1;
    while (span / step > 12) step *= (step == 1 ? 5 : 2);
    const int64_t start = ((firstCycle + step - 1) / step) * step;
    for (int64_t cycle = start; cycle <= lastCycle; cycle += step) {
        const float x = left + static_cast<float>(cycle - firstCycle) * pixelsPerCycle;
        if (x < left || x > right) continue;
        canvas.line({x, top}, {x, bottom}, grid);
        canvas.text({x + 2.f, bottom - 12.f}, rgba(140, 145, 155), std::to_string(cycle).c_str());
    }
}

void exportVcd() {
    if (rowCount == 0 || !statusValid) {
        report("Scope: nothing to export yet");
        return;
    }
    const char* folder = host && host->data_directory_utf8 ? host->data_directory_utf8 : ".";
    const std::string path = std::string(folder) + "/scope.vcd";
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        report("Scope: could not write " + path);
        return;
    }
    std::fprintf(file, "$timescale\n  1 cycle\n$end\n$scope module scope $end\n");
    for (size_t channel = 0; channel < channels.size(); ++channel)
        std::fprintf(file, "$var wire %u w%u %s $end\n", channels[channel].bits,
                     static_cast<unsigned>(channel), labels[channel].c_str());
    std::fprintf(file, "$upscope $end\n$enddefinitions $end\n");
    for (uint32_t row = 0; row < rowCount; ++row) {
        std::fprintf(file, "#%lld\n", static_cast<long long>(rowCycles[row]));
        for (size_t channel = 0; channel < channels.size(); ++channel) {
            const uint64_t value = valueAt(row, channel);
            std::string bits;
            for (int bit = static_cast<int>(channels[channel].bits) - 1; bit >= 0; --bit)
                bits.push_back(((value >> bit) & 1u) ? '1' : '0');
            std::fprintf(file, "b%s w%u\n", bits.c_str(), static_cast<unsigned>(channel));
        }
    }
    std::fclose(file);
    report("Scope: exported " + path + " rows=" + std::to_string(rowCount));
}

void drawPanel(void*, const TCFrame* frame, float width, float height) {
    using namespace tc;
    if (!ready) {
        ui::textDisabled("scope: the loader has no sim.capture / sim.channel service");
        return;
    }
    (void)height;
    if (!loggedFirstDraw) {
        loggedFirstDraw = true;
        report("Scope: panel first drawn on frame " +
               std::to_string(frame ? frame->frame_number : -1));
    }
    if (--refreshCountdown <= 0) {
        refreshCountdown = kRefreshFrames;
        refreshChannels();
    }
    readRing();
    /* One line when the first real window arrives: it is the difference between
       "the panel is up" and "the panel is showing the circuit", and it is what
       the playtest asserts. */
    if (!loggedWindow && rowCount >= 8) {
        loggedWindow = true;
        report("Scope: first window rows=" + std::to_string(rowCount) + " gaps=" +
               std::to_string(statusValid ? status_.gaps : 0) + " injected=" +
               std::to_string(statusValid ? status_.injected : 0) + " lanes=" +
               std::to_string(channels.size()));
    }

    /* ---- controls ------------------------------------------------------- */
    if (game_ui::framed_button(armed ? "Stop" : "Arm", {92.f, 0.f})) {
        if (armed) {
            tc::sim_capture::stop(capture);
            armed = false;
        } else {
            fingerprint.clear();   /* force a configure+start */
            refreshCountdown = 0;
        }
    }
    ui::sameLine();
    if (ui::checkbox("Follow", &follow)) {
        if (follow) panCycles = 0;
    }
    ui::sameLine();
    if (ui::button("Latest")) {
        panCycles = 0;
        follow = true;
    }
    ui::sameLine();
    if (ui::button("<")) {
        const uint64_t step = static_cast<uint64_t>((width - kGutter) * cyclesPerPixel * 0.5f) + 1;
        panCycles += step;
        follow = false;
    }
    ui::sameLine();
    if (ui::button(">")) {
        const uint64_t step = static_cast<uint64_t>((width - kGutter) * cyclesPerPixel * 0.5f) + 1;
        panCycles = panCycles > step ? panCycles - step : 0;
        if (panCycles == 0) follow = true;
    }
    ui::sameLine();
    ui::setNextItemWidth(120.f);
    ui::sliderFloat("cycles/px", &cyclesPerPixel, 0.25f, 32.f, "%.2f");

    /* Trigger. */
    ui::text("trigger");
    ui::sameLine();
    if (ui::smallButton("<")) {
        triggerChannel = triggerChannel <= -1 ? static_cast<int>(channels.size()) - 1
                                              : triggerChannel - 1;
        fingerprint.clear();
        refreshCountdown = 0;
    }
    ui::sameLine();
    ui::text(triggerChannel < 0 ? std::string("off")
                                : "lane " + std::to_string(triggerChannel));
    ui::sameLine();
    if (ui::smallButton(">")) {
        triggerChannel = triggerChannel + 1 >= static_cast<int>(channels.size())
                             ? -1
                             : triggerChannel + 1;
        fingerprint.clear();
        refreshCountdown = 0;
    }
    ui::sameLine();
    static const char* const edges[] = {"none", "rise", "fall", "either", "match"};
    if (ui::smallButton(triggerEdge >= 0 && triggerEdge <= 4 ? edges[triggerEdge] : "?")) {
        triggerEdge = (triggerEdge + 1) % 5;
        fingerprint.clear();
        refreshCountdown = 0;
    }
    if (triggerEdge == TCCAPTURE_EDGE_MATCH) {
        ui::sameLine();
        ui::text("value");
        ui::sameLine();
        if (ui::smallButton("-")) {
            triggerValue = triggerValue > 0 ? triggerValue - 1 : 0;
            fingerprint.clear();
            refreshCountdown = 0;
        }
        ui::sameLine();
        ui::text(std::to_string(triggerValue));
        ui::sameLine();
        if (ui::smallButton("+")) {
            ++triggerValue;
            fingerprint.clear();
            refreshCountdown = 0;
        }
    }

    /* Cursors: two vertical markers, moved a cycle at a time, with the values
       each channel held there. */
    ui::separator();
    ui::text("cursor A/B");
    ui::sameLine();
    for (int which = 0; which < 2; ++which) {
        int64_t& cursor = which ? cursorB : cursorA;
        if (ui::smallButton(which ? "B-" : "A-")) {
            if (cursor < 0 && rowCount) cursor = static_cast<int64_t>(rowCycles[rowCount - 1]);
            else if (cursor > static_cast<int64_t>(rowCycles[0])) --cursor;
        }
        ui::sameLine();
        if (ui::smallButton(which ? "B+" : "A+")) {
            if (cursor < 0 && rowCount) cursor = static_cast<int64_t>(rowCycles[rowCount - 1]);
            else if (rowCount && cursor < static_cast<int64_t>(rowCycles[rowCount - 1])) ++cursor;
        }
        ui::sameLine();
    }
    if (ui::smallButton("Clear")) cursorA = cursorB = -1;
    ui::sameLine();
    ui::textDisabled("click = A, right click = B");

    /* ---- status --------------------------------------------------------- */
    const bool injected = statusValid && status_.injected == 1;
    std::string state = "rows " + std::to_string(statusValid ? status_.rows : 0) +
                        "   gaps " + std::to_string(statusValid ? status_.gaps : 0) +
                        "   restarts " + std::to_string(statusValid ? status_.restarts : 0) +
                        "   " + (injected ? "tick in program" : "no tick yet") +
                        (statusValid && status_.triggered
                             ? "   trigger@" + std::to_string(status_.trigger_cycle)
                             : "");
    if (state != lastStateLine) {
        lastStateLine = state;
        if (statusValid && status_.gaps && lastReportedRows != static_cast<int>(status_.rows)) {
            lastReportedRows = static_cast<int>(status_.rows);
            report("Scope: gaps=" + std::to_string(status_.gaps) + " at row " +
                   std::to_string(status_.rows));
        }
    }
    if (injected) ui::text(state);
    else ui::textDisabled(state);
    if (!statusValid || rowCount == 0) {
        ui::textDisabled(armed ? "waiting for the level to run..." : "press Arm, then run the circuit");
    }

    /* ---- the waves ----------------------------------------------------- */
    const size_t lanes = channels.size();
    if (lanes > 0 && width > kGutter + 40.f) {
        const float canvasHeight = kLaneHeight * static_cast<float>(lanes) + 14.f;
        tc::ui::Canvas canvas("scope", {width - 4.f, canvasHeight});
        if (canvas) {
            canvas.rectFilled({0, 0}, canvas.size(), tc::ui::rgba(22, 24, 31));
            const float left = kGutter, right = canvas.size().x - 6.f;
            const float perCycle = 1.f / cyclesPerPixel;
            if (rowCount > 0) {
                const uint32_t span = static_cast<uint32_t>((right - left) * perCycle);
                const uint32_t newest = rowCount - 1;
                uint32_t lastRow = panCycles >= newest ? 0 : newest - static_cast<uint32_t>(panCycles);
                uint32_t firstRow = lastRow > span ? lastRow - span : 0;
                const int64_t firstCycle = static_cast<int64_t>(rowCycles[firstRow]);
                const int64_t lastCycle = static_cast<int64_t>(rowCycles[lastRow]);
                drawTimeGrid(canvas, firstCycle, lastCycle, 2.f, canvasHeight - 12.f, left, right,
                             (right - left) / static_cast<float>(lastCycle - firstCycle + 1));
                if (statusValid && status_.triggered) {
                    const int64_t trigger = status_.trigger_cycle;
                    if (trigger >= firstCycle && trigger <= lastCycle) {
                        const float x = left + static_cast<float>(trigger - firstCycle) *
                                                   ((right - left) /
                                                    static_cast<float>(lastCycle - firstCycle + 1));
                        canvas.line({x, 2.f}, {x, canvasHeight - 12.f}, tc::ui::rgba(240, 200, 90));
                        canvas.text({x + 2.f, 2.f}, tc::ui::rgba(240, 200, 90), "T");
                    }
                }
                for (size_t lane = 0; lane < lanes; ++lane)
                    drawLane(canvas, kLaneHeight * static_cast<float>(lane), lane, firstRow,
                             lastRow, canvas.size().x);
                /* Cursors on top of the waves. */
                for (int which = 0; which < 2; ++which) {
                    const int64_t cursor = which ? cursorB : cursorA;
                    if (cursor < firstCycle || cursor > lastCycle) continue;
                    const float x = left + static_cast<float>(cursor - firstCycle) *
                                               ((right - left) /
                                                static_cast<float>(lastCycle - firstCycle + 1));
                    const tc::ui::Color color = which ? tc::ui::rgba(240, 130, 200)
                                                      : tc::ui::rgba(130, 220, 240);
                    canvas.line({x, 2.f}, {x, canvasHeight - 12.f}, color);
                    canvas.text({x + 2.f, canvasHeight - 24.f}, color, which ? "B" : "A");
                }
                /* Click sets a cursor.  The canvas owns its rectangle, so the
                   click never reaches the circuit underneath. */
                if (canvas.hovered() && (ui::isMouseClicked(ui::Mouse_Left) ||
                                         ui::isMouseClicked(ui::Mouse_Right))) {
                    const float local = canvas.mousePosition().x;
                    if (local >= left && local <= right && lastCycle > firstCycle) {
                        const int64_t cycle = firstCycle + static_cast<int64_t>(
                            (local - left) /
                            ((right - left) / static_cast<float>(lastCycle - firstCycle + 1)));
                        if (ui::isMouseClicked(ui::Mouse_Right)) cursorB = cycle;
                        else cursorA = cycle;
                    }
                }
            } else {
                canvas.text({kGutter, 8.f}, tc::ui::rgba(150, 155, 165),
                            "no cycles captured yet");
            }
        }
    } else {
        ui::textDisabled("no wire with a state slot on this board yet");
        if (!loggedUnavailable) {
            loggedUnavailable = true;
            report("Scope: no wire channel could be resolved - enter a board with wires");
        }
    }

    /* ---- cursor readout -------------------------------------------------- */
    if (cursorA >= 0 && cursorB >= 0) {
        ui::separator();
        ui::text(("dt " + std::to_string(cursorB - cursorA) + " cycle(s)").c_str());
        for (size_t lane = 0; lane < lanes; ++lane) {
            uint64_t valueA = 0, valueB = 0;
            if (!valueAtCycle(lane, cursorA, &valueA) || !valueAtCycle(lane, cursorB, &valueB))
                break;
            char text[160];
            std::snprintf(text, sizeof(text), "%s  A=%s  B=%s", labels[lane].c_str(),
                          numberText(valueA, channels[lane].bits).c_str(),
                          numberText(valueB, channels[lane].bits).c_str());
            ui::textDisabled(text);
        }
    }

    /* ---- exports and lane switches -------------------------------------- */
    ui::separator();
    if (ui::button("Export VCD")) exportVcd();
    ui::sameLine();
    if (ui::button("Clear")) {
        fingerprint.clear();
        refreshCountdown = 0;
        rowCount = 0;
        cursorA = cursorB = -1;
    }
    ui::sameLine();
    ui::textDisabled(("lanes " + std::to_string(lanes) + "/" + std::to_string(kMaxChannels))
                         .c_str());
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE || !out ||
        out->size < sizeof(TCPlugin))
        return 1;
    host = h;
    if (!tc::ui::load(h)) return 2;
    tc::ui::loadDrawing(h);
    tc::game_ui::load(h);
    if (!tc::simulation::table(h, &sim)) {
        reportStatus("Scope: this loader has no tc.simulation V2", 2);
        return 3;
    }
    if (!tc::sim_capture::table(h, &capture)) {
        reportStatus("Scope: this loader has no tc.sim.capture", 2);
        return 4;
    }
    if (!tc::sim_channel::table(h, &wires)) {
        reportStatus("Scope: this loader has no tc.sim.channel", 2);
        return 5;
    }
    if (tc::boardService(h, &board) != TC_SERVICE_OK) {
        reportStatus("Scope: this loader has no board object service", 2);
        return 6;
    }
    ready = true;
    const int subscription = tc::events::onLevelLoad(h, &onLevelLoadEvent, nullptr);
    if (subscription != 0)
        reportStatus("Scope: level-load subscription failed (" +
                         std::string(tc::events::errorText(subscription)) + ")", 1);
    const int registered = tc::ui::registerBoardPanel("scope", "Scope", &drawPanel, nullptr, h);
    if (registered != 0)
        reportStatus("Scope: board panel not registered (" + std::to_string(registered) + ")", 1);
    else
        report("Scope: registered board panel; capture depth " + std::to_string(kDepth) +
               " cycles, up to " + std::to_string(kMaxChannels) + " lanes");
    (void)out;
    return 0;
}

