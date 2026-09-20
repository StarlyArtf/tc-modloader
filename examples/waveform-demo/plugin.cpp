/* Live waveform of the level's inputs and outputs, plus VCD export.

   The data comes from tc::trace::Sampler (see sdk/tc_trace.h): the game keeps
   the level's I/O history in two buffers, one 64-bit slot per element, and the
   sampler finds those slots at run time from the counts the level declares.
   Nothing here writes simulation state - the panel only reads and draws.

   The panel is a board side panel, so it inherits everything the host provides
   there: clipping, the scroll strip, the collapse toggle and the guarantee that
   clicks on it do not reach the circuit board. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../sdk/tc_mod.h"
#include "../../sdk/tc_ui.h"
#include "../../sdk/tc_game_ui.h"
#include "../../sdk/tc_ui_draw.h"
#include "../../sdk/tc_trace.h"
#include <cstdio>
#include <string>
#include <vector>
#ifdef TC_WAVE_DRIVER
#include "../../tests/waveform-driver.hpp"
#endif

/* How many samples the panel draws and how wide one is. */
static constexpr int kVisibleSamples = 64;
static constexpr float kStep = 12.f;
static constexpr float kLaneHeight = 26.f;
static constexpr float kGutter = 52.f;

static const TCHost* host;
tc::TCMod mod;                 /* shared with the driver build */
tc::trace::Sampler trace;
static bool loggedFirst;
static std::string lastSlotReport;
static int lastRowCount;
static double lastRowTime;
static bool simulating;
static std::string lastExportPath;
static std::string lastImagePath;
static bool loggedProbeHint;

/* The board model, captured from the host's level-load event.

   A plugin needs it to look at the board's wire table (wire records carry the
   state byte offset a probe reads).  This subscribes instead of hooking: the
   loader watches level loading once for every plugin that cares, and the game's
   own load is not disturbed by watching it. */
static void* boardModel;

static void onLevelLoadEvent(TCEvent* event) {
    if (!tc::events::is(event, TC_EVENT_LEVEL_LOAD)) return;
    boardModel = tc::events::levelBoardModel(event);
}

static void report(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

/* Same text, plus the loader's Mods page: a level with no I/O history or a busy
   hook target is something the player should see without opening the log.
   Load-time state only; level: 0 info, 1 warning, 2 error. */
static void reportStatus(const std::string& message, int level) {
    report(message);
    if (host) tc::reportStatus(host, level, message.c_str());
}

#ifdef TC_WAVE_DRIVER
static void driverLoadLevel(void* model);
static void driverRunTo(int64_t cycle);
static void driverLog(const char* message) {
    if (host && host->log) host->log(host->context, message);
}
#endif

static const char* dataFolder() {
    return host && host->data_directory_utf8 ? host->data_directory_utf8 : ".";
}

static void exportVcd() {
    const std::string path = std::string(dataFolder()) + "/waveform.vcd";
    if (trace.writeVcd(path.c_str(), "level")) {
        lastExportPath = path;
        report("Waveform: exported " + path + " rows=" + std::to_string(trace.rows()));
    } else {
        report("Waveform: nothing to export yet");
    }
}

/* Framebuffer capture, for the picture the playtest saves next to the VCD.

   The panel cannot be screenshotted from outside: this build runs a borderless
   fullscreen GL window in independent-flip mode, so neither PrintWindow nor a
   desktop grab sees a single pixel of it (measured).  Reading the OpenGL
   backbuffer from inside the process does work.  The read happens while the
   ImGui frame is still being built, so the image is the frame from two frames
   ago - which is exactly what we want here, because the panel has already been
   drawn into it and never moves. */
namespace capture {

using ReadPixels = void (*)(int, int, int, int, unsigned int, unsigned int, void*);
using GetIntegerv = void (*)(unsigned int, int*);
using PixelStorei = void (*)(unsigned int, int);

constexpr unsigned int kViewport = 0x0BA2;
constexpr unsigned int kRgba = 0x1908;
constexpr unsigned int kUnsignedByte = 0x1401;
constexpr unsigned int kPackAlignment = 0x0D05;

ReadPixels readPixels;
GetIntegerv getIntegerv;
PixelStorei pixelStorei;

bool load() {
    if (readPixels) return true;
    HMODULE module = GetModuleHandleA("opengl32.dll");
    if (!module) module = LoadLibraryA("opengl32.dll");
    if (!module) return false;
    /* OpenGL 1.1 entry points are real exports of opengl32.dll, so no
       wglGetProcAddress dance is needed for the three used here. */
    /* Via void*: casting FARPROC straight to another function type warns. */
    readPixels = reinterpret_cast<ReadPixels>(
        reinterpret_cast<void*>(GetProcAddress(module, "glReadPixels")));
    getIntegerv = reinterpret_cast<GetIntegerv>(
        reinterpret_cast<void*>(GetProcAddress(module, "glGetIntegerv")));
    pixelStorei = reinterpret_cast<PixelStorei>(
        reinterpret_cast<void*>(GetProcAddress(module, "glPixelStorei")));
    return readPixels && getIntegerv && pixelStorei;
}

void writeLe32(unsigned char* out, unsigned value) {
    out[0] = static_cast<unsigned char>(value & 0xff);
    out[1] = static_cast<unsigned char>((value >> 8) & 0xff);
    out[2] = static_cast<unsigned char>((value >> 16) & 0xff);
    out[3] = static_cast<unsigned char>((value >> 24) & 0xff);
}

/* A 32-bit BMP with a negative height: the first row written is the top one,
   which is the order glReadPixels hands back (bottom-up). */
bool writeBmp(const char* path, int width, int height, const std::vector<unsigned char>& rgba) {
    std::FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    const unsigned pixelBytes = static_cast<unsigned>(width) * static_cast<unsigned>(height) * 4u;
    const unsigned offset = 14u + 40u;
    unsigned char header[54]{};
    header[0] = 'B'; header[1] = 'M';
    writeLe32(header + 2, offset + pixelBytes);
    writeLe32(header + 10, offset);
    writeLe32(header + 14, 40);
    writeLe32(header + 18, static_cast<unsigned>(width));
    writeLe32(header + 22, static_cast<unsigned>(-height));
    header[26] = 1;
    header[28] = 32;
    writeLe32(header + 34, pixelBytes);
    if (std::fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
        std::fclose(file);
        return false;
    }
    std::vector<unsigned char> row(static_cast<size_t>(width) * 4u);
    for (int y = 0; y < height; ++y) {
        const unsigned char* source = rgba.data() + static_cast<size_t>(height - 1 - y) * width * 4u;
        for (int x = 0; x < width; ++x) {
            row[x * 4 + 0] = source[x * 4 + 2];   /* B */
            row[x * 4 + 1] = source[x * 4 + 1];   /* G */
            row[x * 4 + 2] = source[x * 4 + 0];   /* R */
            row[x * 4 + 3] = 255;
        }
        if (std::fwrite(row.data(), 1, row.size(), file) != row.size()) {
            std::fclose(file);
            return false;
        }
    }
    std::fclose(file);
    return true;
}

bool save(const char* path) {
    if (!load()) return false;
    int viewport[4] = {0, 0, 0, 0};
    getIntegerv(kViewport, viewport);
    const int width = viewport[2], height = viewport[3];
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4u);
    pixelStorei(kPackAlignment, 1);
    readPixels(viewport[0], viewport[1], width, height, kRgba, kUnsignedByte, pixels.data());
    return writeBmp(path, width, height, pixels);
}

}  // namespace capture

/* The picture the panel would show, saved next to the VCD. */
static void exportImage() {
    const std::string path = std::string(dataFolder()) + "/waveform.bmp";
    if (capture::save(path.c_str())) {
        lastImagePath = path;
        report("Waveform: exported " + path + " (framebuffer)");
    } else {
        report("Waveform: framebuffer capture unavailable");
    }
}

/* Wire probing: a wire record in the board model carries the byte offset of its
   state slot at +0x38 and the net's width at +0x30 (measured; see the handoff
   doc).  The value is then `read_u64(offset) & ((1 << width) - 1)`, exactly what
   the game's own sim_state_read_bits does. */
static bool wireSlot(int64_t wireId, uint64_t* offset, int* width, int* x = nullptr,
                     int* y = nullptr) {
    if (!boardModel || wireId < 0) return false;
    auto* base = static_cast<unsigned char*>(boardModel);
    uint64_t count = 0;
    void* data = nullptr;
    std::memcpy(&count, base + 0x98, sizeof(count));
    std::memcpy(&data, base + 0xa0, sizeof(data));
    if (!data || count > 4096 || static_cast<uint64_t>(wireId) >= count) return false;
    const auto* wire = static_cast<const unsigned char*>(data) + 8 +
                       static_cast<uint64_t>(wireId) * 0x68;
    uint64_t stateOffset = 0, recordWidth = 0;
    std::memcpy(&stateOffset, wire + 0x38, sizeof(stateOffset));
    std::memcpy(&recordWidth, wire + 0x30, sizeof(recordWidth));
    if (x && y) {
        int16_t ax = 0, ay = 0;
        std::memcpy(&ax, wire + 0x18, 2);
        std::memcpy(&ay, wire + 0x1a, 2);
        *x = ax;
        *y = ay;
    }
    if (stateOffset == 0) return false;
    *offset = stateOffset;
    *width = (recordWidth >= 1 && recordWidth <= 64) ? static_cast<int>(recordWidth) : 1;
    return true;
}

/* How many level pins the board really has.

   The game's `level_used_input` / `level_used_outputs` globals cannot be trusted
   for this: with a one-input one-output circuit they still reported 2/2
   (measured), which is why the panel used to draw four lanes for two signals.
   The board's own IO components say it exactly: kind 0x3f is a level input and
   0x44 a level output (the same kinds the verified custom-logic netlist builder
   uses). */
static bool boardIoCounts(int* inputs, int* outputs) {
    if (!boardModel) return false;
    auto* base = static_cast<unsigned char*>(boardModel);
    uint64_t count = 0;
    void* data = nullptr;
    std::memcpy(&count, base + 0x78, sizeof(count));
    std::memcpy(&data, base + 0x80, sizeof(data));
    if (!data || count == 0 || count > 4096) return false;
    int in = 0, out = 0;
    const auto* records = static_cast<const unsigned char*>(data) + 8;
    for (uint64_t index = 0; index < count; ++index) {
        uint16_t kind = 0;
        std::memcpy(&kind, records + index * 0x238, sizeof(kind));
        if (kind == 0x3f) ++in;
        else if (kind == 0x44) ++out;
    }
    if (in + out == 0) return false;
    *inputs = in;
    *outputs = out;
    return true;
}

/* Adds every currently selected wire as a probe target, so the player picks a
   wire with the game's own selection and then presses the button. */
static void pickSelectedWires() {
    if (!boardModel) {
        report("Waveform: no board model yet - enter a level, then pick a wire again");
        return;
    }
    if (!trace.canProbe()) {
        report("Waveform: state reader unavailable, cannot probe wires");
        return;
    }
    const uint64_t selected = mod.board.selectedWireCount();
    if (selected == 0) {
        report("Waveform: nothing selected - click a wire on the board first");
        return;
    }
    int added = 0;
    for (uint64_t index = 0; index < selected && added < 8; ++index) {
        const uint64_t id = mod.board.selectedWireIdAt(index);
        uint64_t offset = 0;
        int width = 1;
        int x = 0, y = 0;
        if (!wireSlot(static_cast<int64_t>(id), &offset, &width, &x, &y)) continue;
        /* Coordinates in the label so two probes are tellable apart on the board. */
        const std::string label = "w" + std::to_string(id) + "@" + std::to_string(x) + "," +
                                  std::to_string(y);
        if (trace.addBitProbe(label, offset, width) >= 0) {
            ++added;
            report("Waveform: probing " + label + " state@" + std::to_string(offset) +
                   " width=" + std::to_string(width));
        }
    }
    if (added == 0) report("Waveform: no wire slot could be resolved for the selection");
}

/* One digital lane: a square wave plus its label. */
static void drawLane(tc::ui::Canvas& canvas, float top, const char* label,
                     tc::trace::Sampler& sampler, int element, bool input, float width,
                     size_t firstSample) {
    using tc::ui::rgba;
    const tc::ui::Color color = input ? rgba(90, 170, 235) : rgba(245, 185, 70);
    const tc::ui::Color grid = rgba(52, 57, 70);
    canvas.text({4.f, top + 2.f}, rgba(200, 205, 215), label);
    const float high = top + 5.f, low = top + kLaneHeight - 7.f;
    float previousX = 0.f, previousY = 0.f;
    bool havePrevious = false;
    const size_t samples = sampler.rows();
    for (size_t offset = 0; offset < samples - firstSample; ++offset) {
        const size_t row = firstSample + offset;
        const float x = kGutter + static_cast<float>(offset) * kStep;
        if (x > width - 4.f) break;
        const uint64_t value = input ? sampler.input(row, element) : sampler.output(row, element);
        const float y = value ? high : low;
        if (havePrevious) {
            if (previousY != y) {
                canvas.line({previousX, previousY}, {previousX, y}, color);
                canvas.line({previousX, y}, {x, y}, color);
            } else {
                canvas.line({previousX, previousY}, {x, y}, color);
            }
        }
        previousX = x;
        previousY = y;
        havePrevious = true;
    }
    /* Time grid every eight samples, drawn under the trace. */
    for (int index = 0; index < kVisibleSamples; index += 8)
        canvas.line({kGutter + index * kStep, top + 2.f},
                    {kGutter + index * kStep, top + kLaneHeight - 4.f}, grid);
}

/* A probed wire's lane: same drawing rules, but the value comes from the probe
   (any non-zero reads as high, so a word net still shows its activity). */
static void drawProbeLane(tc::ui::Canvas& canvas, float top, const char* label,
                          tc::trace::Sampler& sampler, int probe, float width,
                          size_t firstSample) {
    using tc::ui::rgba;
    const tc::ui::Color color = rgba(120, 220, 140);
    const tc::ui::Color grid = rgba(52, 57, 70);
    canvas.text({4.f, top + 2.f}, rgba(200, 205, 215), label);
    const float high = top + 5.f, low = top + kLaneHeight - 7.f;
    float previousX = 0.f, previousY = 0.f;
    bool havePrevious = false;
    const size_t samples = sampler.rows();
    for (size_t offset = 0; offset < samples - firstSample; ++offset) {
        const size_t row = firstSample + offset;
        const float x = kGutter + static_cast<float>(offset) * kStep;
        if (x > width - 4.f) break;
        const float y = sampler.probeValue(row, probe) ? high : low;
        if (havePrevious) {
            if (previousY != y) {
                canvas.line({previousX, previousY}, {previousX, y}, color);
                canvas.line({previousX, y}, {x, y}, color);
            } else {
                canvas.line({previousX, previousY}, {x, y}, color);
            }
        }
        previousX = x;
        previousY = y;
        havePrevious = true;
    }
    for (int index = 0; index < kVisibleSamples; index += 8)
        canvas.line({kGutter + index * kStep, top + 2.f},
                    {kGutter + index * kStep, top + kLaneHeight - 4.f}, grid);
}

static void drawPanel(void*, const TCFrame* frame, float width, float height) {
    (void)height;   /* the host's scroll strip handles a taller panel */
    if (!loggedFirst) {
        loggedFirst = true;
        report("Waveform: panel first drawn on frame " +
               std::to_string(frame ? frame->frame_number : -1));
    }
    /* One sample per frame, but a row is only stored when the simulation moved
       to a new cycle: while the game is paused the waveform stands still. */
    int boardInputs = 0, boardOutputs = 0;
    static int lastBoardInputs = -1, lastBoardOutputs = -1;
    if (boardIoCounts(&boardInputs, &boardOutputs)) {
        trace.setDeclaredCounts(boardInputs, boardOutputs);
        if (boardInputs != lastBoardInputs || boardOutputs != lastBoardOutputs) {
            lastBoardInputs = boardInputs;
            lastBoardOutputs = boardOutputs;
            report("Waveform: the board has " + std::to_string(boardInputs) +
                   " input(s) and " + std::to_string(boardOutputs) + " output(s)");
        }
    }
    if (trace.sample()) {
        lastRowTime = frame ? frame->time_seconds : 0.0;
    }
    if (trace.takeRestartFlag())
        report("Waveform: simulation restarted - waveform cleared");
    simulating = frame && lastRowCount != 0 &&
                 (frame->time_seconds - lastRowTime) < 0.6;
    lastRowCount = static_cast<int>(trace.rows());
    const size_t rows = trace.rows();
    /* Logged whenever the resolution changes, not just once: the first samples
       arrive before the level has run, so the first guess is only a guess and
       the interesting line is the later one. */
    if (trace.ready()) {
        std::string slots = "Waveform: resolved slots";
        for (int index = 0; index < trace.inputCount(); ++index)
            slots += " in" + std::to_string(index) + "@" + std::to_string(trace.inputSlot(index));
        slots += " (moved " + std::to_string(trace.inputMoved()) + ")";
        for (int index = 0; index < trace.outputCount(); ++index)
            slots += " out" + std::to_string(index) + "@" + std::to_string(trace.outputSlot(index));
        slots += " (moved " + std::to_string(trace.outputMoved()) + ")";
        slots += trace.assumedStride() ? " (stride estimated)" : " (all found by movement)";
        if (slots != lastSlotReport) {
            lastSlotReport = slots;
            report(slots);
        }
    }
    /* "simulating" is the honest answer to "is this waveform moving because the
       circuit is running, or because time is passing?": a row was stored within
       the last 0.6 s of game time.  One row per simulation cycle, so the sample
       count is the number of cycles the panel has seen. */
    std::string header = "samples " + std::to_string(rows) + "   " +
                         (simulating ? "simulating" : "paused (waveform held)");
    if (rows > 0)
        header += "   cycle " + std::to_string(trace.cycleAt(0)) + ".." +
                  std::to_string(trace.lastCycle());
    tc::ui::text(header);
    tc::ui::textDisabled(("inputs " + std::to_string(trace.inputCount()) +
                          "   outputs " + std::to_string(trace.outputCount()) +
                          (trace.assumedStride() ? "   (slots estimated)" : ""))
                             .c_str());
    if (!trace.ready()) {
        tc::ui::textDisabled("waiting for the level's I/O history...");
    }
    /* Short labels: the panel is narrow and the game's button text does not clip
       gracefully (measured - "Export VCD" and "Export image" both rendered as
       "Export"). */
    if (tc::game_ui::framed_button("VCD", {110.f, 0.f})) exportVcd();
    tc::ui::sameLine();
    if (tc::game_ui::framed_button("BMP", {110.f, 0.f})) exportImage();
    tc::ui::sameLine();
    if (tc::game_ui::framed_button("Probe", {110.f, 0.f})) pickSelectedWires();
    tc::ui::sameLine();
    if (tc::ui::button("Clear")) {
        trace.reset();
        report("Waveform: cleared");
    }
    tc::ui::separator();
    /* The waves come first: the canvas is what the panel is for, and the panel
       is only as tall as the host gives it (a long control block above the
       canvas used to push the waves out of the visible content area). */
    const int lanes = trace.visibleInputs() + trace.visibleOutputs() + trace.visibleProbes();
    if (lanes > 0 && width > kGutter + kStep) {
        /* A little room under the last lane: the canvas is the panel's clipping
           rectangle, so a lane drawn flush with its bottom edge reads as cut. */
        const float canvasHeight = kLaneHeight * static_cast<float>(lanes) + 10.f;
        tc::ui::Canvas canvas("waveform", {width - 4.f, canvasHeight});
        if (canvas) {
            using tc::ui::rgba;
            canvas.rectFilled({0, 0}, canvas.size(), rgba(24, 26, 33));
            const int visible = static_cast<int>((width - kGutter - 8.f) / kStep);
            const size_t window = static_cast<size_t>(kVisibleSamples < visible ? kVisibleSamples
                                                                               : visible);
            const size_t firstSample = rows > window ? rows - window : 0;
            int lane = 0;
            for (int index = 0; index < trace.inputCount(); ++index) {
                if (!trace.visible(true, index)) continue;
                drawLane(canvas, kLaneHeight * lane, ("in" + std::to_string(index)).c_str(),
                         trace, index, true, width, firstSample);
                ++lane;
            }
            for (int index = 0; index < trace.outputCount(); ++index) {
                if (!trace.visible(false, index)) continue;
                drawLane(canvas, kLaneHeight * lane, ("out" + std::to_string(index)).c_str(),
                         trace, index, false, width, firstSample);
                ++lane;
            }
            for (int index = 0; index < trace.probeCount(); ++index) {
                if (!trace.probeVisible(index)) continue;
                drawProbeLane(canvas, kLaneHeight * lane, trace.probeLabel(index), trace,
                              index, width, firstSample);
                ++lane;
            }
            if (firstSample > 0)
                canvas.text({kGutter, canvasHeight - 12.f}, rgba(160, 165, 175),
                            ("showing the last " + std::to_string(window) + " of " +
                             std::to_string(rows) + " samples").c_str());
        }
    } else {
        tc::ui::textDisabled(lanes == 0 ? "no lane selected" : "no level I/O traced yet");
    }
    /* Which signals to watch.  Everything is sampled either way; unchecked lanes
       are left out of the drawing and of the VCD, so a level with a lot of pins
       can be narrowed down to the few that matter. */
    const int totalPins = trace.inputCount() + trace.outputCount();
    if (totalPins > 0) {
        tc::ui::separator();
        for (int index = 0; index < trace.inputCount(); ++index) {
            bool on = trace.visible(true, index);
            if (tc::ui::checkbox(("in" + std::to_string(index)).c_str(), &on))
                trace.setVisible(true, index, on);
            tc::ui::sameLine();
        }
        for (int index = 0; index < trace.outputCount(); ++index) {
            bool on = trace.visible(false, index);
            if (tc::ui::checkbox(("out" + std::to_string(index)).c_str(), &on))
                trace.setVisible(false, index, on);
            tc::ui::sameLine();
        }
        tc::ui::newLine();
        /* The level's own pins are shown as a reference by default; these two
           buttons hide or show all of them at once, which is what "I only want my
           probe" needs. */
        if (tc::ui::button("I/O all")) {
            for (int index = 0; index < trace.inputCount(); ++index)
                trace.setVisible(true, index, true);
            for (int index = 0; index < trace.outputCount(); ++index)
                trace.setVisible(false, index, true);
        }
        tc::ui::sameLine();
        if (tc::ui::button("I/O none")) {
            for (int index = 0; index < trace.inputCount(); ++index)
                trace.setVisible(true, index, false);
            for (int index = 0; index < trace.outputCount(); ++index)
                trace.setVisible(false, index, false);
        }
        tc::ui::sameLine();
        tc::ui::textDisabled(("watching " + std::to_string(trace.visibleInputs()) + "/" +
                              std::to_string(trace.inputCount()) + " in, " +
                              std::to_string(trace.visibleOutputs()) + "/" +
                              std::to_string(trace.outputCount()) +
                              " out - unchecked lanes are not drawn or exported")
                                 .c_str());
    }
    /* Wire probes: pick with the game's own selection (click a wire on the board),
       then add it here; each probe gets its own lane in green. */
    if (trace.probeCount() > 0) {
        tc::ui::separator();
        tc::ui::text((std::to_string(trace.probeCount()) + " wire probe(s): ").c_str());
        for (int index = 0; index < trace.probeCount(); ++index) {
            bool on = trace.probeVisible(index);
            if (tc::ui::checkbox(trace.probeLabel(index), &on))
                trace.setProbeVisible(index, on);
            tc::ui::sameLine();
        }
        tc::ui::newLine();
        if (tc::ui::button("Clear probes")) trace.clearBitProbes();
    } else if (!boardModel && !loggedProbeHint) {
        loggedProbeHint = true;
        report("Waveform: wire probing needs a level load; enter a level first");
    }
#ifdef TC_WAVE_DRIVER
    {
        auto& driver = tc_wave_driver::driver();
        driver.observeTrace(trace);
        if (driver.exportRequested) {
            driver.exportRequested = false;
            exportVcd();
            driver.exportedRows = trace.rows();
            driver.exportPath = lastExportPath;
            driver.exportReported = true;
        }
        if (driver.captureRequested) {
            driver.captureRequested = false;
            exportImage();
            driver.imagePath = lastImagePath;
            driver.captured = true;
        }
    }
#endif
}

#ifdef TC_WAVE_DRIVER
static void driverLoadLevel(void* model) {
    /* Which level to load is the playtest's choice (TC_WAVE_LEVEL); and_gate is
       the fixture the level I/O slots were measured on. */
    const char* name = std::getenv("TC_WAVE_LEVEL") ? std::getenv("TC_WAVE_LEVEL") : "and_gate";
    tc::TCNimString level{};
    const size_t length = std::strlen(name);
    mod.game.raw_new_string(&level, static_cast<int64_t>(length));
    std::memcpy(static_cast<unsigned char*>(level.data) + 8, name, length);
    level.length = length;
    static_cast<unsigned char*>(level.data)[8 + length] = 0;
    auto* load = reinterpret_cast<void (*)(void*, const tc::TCNimString*)>(
        host->resolve_symbol(host->context, "load_level__modelZutilities_u7740"));
    auto* test = reinterpret_cast<void (*)(void*, int64_t, uint8_t)>(
        host->resolve_symbol(host->context, "set_sim_test__modelZutilities_u6840"));
    auto* compile = reinterpret_cast<void (*)(void*, const tc::TCNimString*, uint8_t)>(
        host->resolve_symbol(host->context, "preorder__modelZsimulationZcompile95thread_u3523"));
    if (load) load(model, &level);
    if (test) test(model, 0, 1);
    tc::TCNimString progress{};
    if (compile) compile(model, &progress, 0);
    report("Waveform: loaded and_gate and requested compilation");
}

static void driverRunTo(int64_t cycle) {
    auto& driver = tc_wave_driver::driver();
    if (driver.model) mod.simulation.run(driver.model, cycle);
}

static void driverPause(void*) {
    auto& driver = tc_wave_driver::driver();
    if (driver.model) mod.simulation.pause(driver.model);
}

/* Test-only path into the same probe the "Probe wire" button adds: the driver
   names a wire index instead of the player selecting a wire on the board. */
static void driverPickWire(int wireId) {
    auto& driver = tc_wave_driver::driver();
    uint64_t offset = 0;
    int width = 1;
    if (!wireSlot(wireId, &offset, &width)) {
        report("Waveform: driver wire " + std::to_string(wireId) + " has no state slot");
        return;
    }
    const int probe = trace.addBitProbe("w" + std::to_string(wireId), offset, width);
    driver.probeIndex = probe;
    report("Waveform: driver probes wire " + std::to_string(wireId) + " state@" +
           std::to_string(offset) + " width=" + std::to_string(width));
}
#endif

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE || !out ||
        out->size < sizeof(TCPlugin))
        return 1;
    host = h;
    /* The panel lives in a board slot and draws with the canvas API; both are
       hard requirements, so say so before touching anything. */
    if (!tc::hostHas(h, TC_CAP_UI_SLOT)) return 2;
    if (!tc::ui::load(h)) {
        reportStatus("Waveform: tc::ui::load failed: " + tc::ui::missing(), 2);
        return 3;
    }
    if (!tc::ui::loadDrawing(h)) reportStatus("Waveform: drawing unavailable", 1);
    if (!tc::game_ui::load(h)) reportStatus("Waveform: game widgets unavailable", 1);
    if (!mod.load(h) || !mod.valid()) {
        reportStatus("Waveform: game model unavailable", 2);
        return 4;
    }
    if (!trace.load(h, mod.simulation, mod.state)) {
        reportStatus("Waveform: trace sampler unavailable (no level I/O history)", 2);
        return 5;
    }
    /* Wire probing needs the board model; the level-load event hands it over.
       The panel still works without it - only probing is off - which the player
       is told through report_status. */
    const int subscription = tc::events::onLevelLoad(h, &onLevelLoadEvent, nullptr);
    if (subscription == TC_EVENT_OK)
        report("Waveform: board model capture subscribed to the level-load event");
    else
        reportStatus(std::string("Waveform: no level-load event (") +
                         tc::events::errorText(subscription) + "); wire probing unavailable",
                     1);
    const int result = tc::ui::registerBoardPanel("waveform", "Waveform", drawPanel, nullptr, h,
                                                 480.f, 420.f);
    if (result != 0) {
        report("Waveform: registerBoardPanel failed with " + std::to_string(result));
        return 6;
    }
    report("Waveform: registered board panel");
#ifdef TC_WAVE_DRIVER
    if (!tc_wave_driver::start(h, driverLog, driverLoadLevel, driverRunTo, driverPause,
                               driverPickWire))
        return 9;
    out->on_frame = tc_wave_driver::tick;
#endif
    return 0;
}
