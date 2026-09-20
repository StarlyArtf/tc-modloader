/* In-process driver for the waveform demo: enters a level on its own, runs the
   simulation far enough for the level's test to produce a known sequence, asks
   the plugin to export a VCD and reports what the panel traced.

   It exists because the interesting assertions are about *data* (the level's
   inputs and outputs per cycle), and only the game can produce that data; the
   playtest asserts on the DRIVER: lines this writes.

   Level entry: press one of the home page's own invisible buttons (the trick the
   other probes use), capture the board model from handle_update_wire and then
   ask the plugin to load and_gate - whose own test feeds inputs 0,1,2,3 and
   expects outputs 0,0,0,1. */
#pragma once
#ifdef TC_WAVE_DRIVER
#include "../sdk/tc_trace.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace tc_wave_driver {

struct V2 { float x, y; };

struct Driver {
    const TCHost* host = nullptr;
    void (*log)(const char*) = nullptr;
    void (*loadLevel)(void*) = nullptr;
    void (*runTo)(int64_t) = nullptr;
    void (*pauseSim)(void*) = nullptr;
    void (*pickWire)(int) = nullptr;
    int probeIndex = -1;
    const tc::trace::Sampler* sampler = nullptr;
    bool dumpWires = false;

    HWND window = nullptr;
    bool installed = false, started = false, done = false, entered = false;
    bool dumped = false;
    /* "A paused simulation adds no rows" is measured, not assumed: the driver
       notes the row count, waits two seconds with the game paused, and notes it
       again. */
    size_t pausedRows = 0, pausedRowsLater = 0;
    double pauseCheckTime = 0;
    bool pausedDumped = false;
    int pauseDumpFrame = 0;
    /* Settle check: pause at an earlier cycle too, so a probe byte can be told
       apart from "a byte that happens to be 1 later". */
    bool settleDumped = false;
    bool settleRequested = false;
    int settleFrame = 0;
    int start_frame = 0, stage = 0, menu_frame = -1, menu_clicks = 0;
    double start_time = 0, elapsed = 0;
    void* model = nullptr;

    bool (*invisibleOriginal)(const char*, V2, int) = nullptr;
    bool (*updateOriginal)(void*, void*, void*, uint32_t, uint8_t) = nullptr;
    /* Which of the board-module entry points can hand a mod the board model?
       The driver already knows the real model (captured from
       handle_update_wire), so it can answer that by comparison instead of
       guessing from disassembly. */
    void (*candidateOriginal)(void*) = nullptr;
    const char* candidateName = nullptr;
    bool candidateLogged = false;

    void noteCandidate(const char* name, void* a0, void* a1, void* a2, void* a3) {
        if (candidateLogged) return;
        candidateLogged = true;
        const auto matches = [&](void* candidate) {
            return candidate && model && candidate == model ? "=model" : "";
        };
        say(std::string("DRIVER: ") + name + " a0=" + hex64((uint64_t)a0) + matches(a0) +
            " a1=" + hex64((uint64_t)a1) + matches(a1) +
            " a2=" + hex64((uint64_t)a2) + matches(a2) +
            " a3=" + hex64((uint64_t)a3) + matches(a3));
    }

    void* candidateA = nullptr;
    void* candidateB = nullptr;
    bool candidateALogged = false;
    bool candidateBLogged = false;

    void noteA(void* a0, void* a1, void* a2, void* a3) {
        if (candidateALogged) return;
        candidateALogged = true;
        note("update_wire__modelZboardZboard_u19554", a0, a1, a2, a3);
    }

    void noteB(void* a0, void* a1, void* a2, void* a3) {
        if (candidateBLogged) return;
        candidateBLogged = true;
        note("board_update_wires__modelZboardZboard_u22494", a0, a1, a2, a3);
    }

    void note(const char* name, void* a0, void* a1, void* a2, void* a3) {
        const auto matches = [&](void* candidate) -> const char* {
            return (candidate && model && candidate == model) ? "=model" : "";
        };
        say(std::string("DRIVER: candidate ") + name +
            " a0=" + hex64(reinterpret_cast<uint64_t>(a0)) + matches(a0) +
            " a1=" + hex64(reinterpret_cast<uint64_t>(a1)) + matches(a1) +
            " a2=" + hex64(reinterpret_cast<uint64_t>(a2)) + matches(a2) +
            " a3=" + hex64(reinterpret_cast<uint64_t>(a3)) + matches(a3));
    }

    /* Trace observations, filled by the plugin every time it draws. */
    size_t observedRows = 0;
    int observedInputs = 0, observedOutputs = 0;
    /* The level's own test drives the inputs up to 3 (both pins high) and only
       then does and_gate's output go high.  The run waits for that instead of
       exporting as soon as a handful of rows exist: the panel samples every
       frame, so a paused level produces rows that all read zero. */
    uint64_t observedMaxInput = 0;
    bool observedHighOutput = false;
    std::string lastRow, lastLoggedRow;
    int loggedRows = 0;
    bool exportRequested = false, exportReported = false;
    size_t exportedRows = 0;
    std::string exportPath;
    /* The in-game screenshot comes from the plugin (glReadPixels); see
       examples/waveform-demo/plugin.cpp for why an outside capture cannot
       see this window. */
    bool captureRequested = false, captured = false;
    std::string imagePath;
    double captureRequestTime = 0;
    double captureAt = 0;
    int exportFrame = -1;

    void say(const std::string& message) { if (log) log(message.c_str()); }

    static std::string hex64(uint64_t value) {
        char text[32];
        std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(value));
        return text;
    }

    static std::string hex16(uint16_t value) {
        char text[16];
        std::snprintf(text, sizeof(text), "%x", value);
        return text;
    }

    /* One-off measurement of the board's wire table, enabled with TC_WAVE_DUMP=1:
       what the game's own get_wire() answers for a wire's own endpoint, what the
       record's runtime state index holds and what the state reader returns for
       it.  This is what a "monitor this wire" target would have to use. */
    void dumpBoard(const char* phase) {
        if (!model || !host) return;
        auto* base = static_cast<unsigned char*>(model);
        uint64_t components = 0, wires = 0;
        void* componentData = nullptr;
        void* wireData = nullptr;
        std::memcpy(&components, base + 0x78, sizeof(components));
        std::memcpy(&componentData, base + 0x80, sizeof(componentData));
        std::memcpy(&wires, base + 0x98, sizeof(wires));
        std::memcpy(&wireData, base + 0xa0, sizeof(wireData));
        say(std::string("DRIVER: board ") + phase + " components=" + std::to_string(components) +
            " wires=" + std::to_string(wires) +
            " componentData=" + (componentData ? "yes" : "no") +
            " wireData=" + (wireData ? "yes" : "no"));
        if (!wireData || wires == 0 || wires > 4096) return;
        auto* getWire = reinterpret_cast<int64_t (*)(void*, uint32_t)>(
            host->resolve_symbol(host->context,
                                 "get_wire__presenterZutilitiesZhelper95functions_u1916"));
        auto* readState = reinterpret_cast<uint64_t (*)(int64_t)>(
            host->resolve_symbol(host->context,
                                 "sim_state_read_u64__modelZsimulator95types_u159"));
        auto* readBits = reinterpret_cast<int64_t (*)(int64_t, int64_t)>(
            host->resolve_symbol(host->context,
                                 "sim_state_read_bits__modelZsimulationZcontroller_u76"));
        say("DRIVER: get_wire=" + std::string(getWire ? "yes" : "no") +
            " read_state=" + std::string(readState ? "yes" : "no") +
            " read_bits=" + std::string(readBits ? "yes" : "no"));
        const auto* records = static_cast<const unsigned char*>(wireData) + 8;
        /* Component records: the game's own "watchee" path reads a component's
           state through a pointer it keeps at +0x20 (see get_sim_state /
           get_state_index in the pinned build).  Dumping them next to the wires
           is how that assumption gets checked instead of believed. */
        if (componentData) {
            const auto* components8 = static_cast<const unsigned char*>(componentData) + 8;
            for (uint64_t i = 0; i < components && i < 8; ++i) {
                const unsigned char* c = components8 + i * 0x238;
                uint16_t kind = 0;
                int16_t x = 0, y = 0;
                std::memcpy(&kind, c, 2);
                std::memcpy(&x, c + 2, 2);
                std::memcpy(&y, c + 4, 2);
                uint64_t p20 = 0, p28 = 0, p30 = 0, w38 = 0;
                std::memcpy(&p20, c + 0x20, sizeof(p20));
                std::memcpy(&p28, c + 0x28, sizeof(p28));
                std::memcpy(&p30, c + 0x30, sizeof(p30));
                std::memcpy(&w38, c + 0x38, sizeof(w38));
                std::string text = "DRIVER: component " + std::string(phase) + " " +
                                   std::to_string(i) + " kind=0x" + hex16(kind) +
                                   " at=" + std::to_string(x) + "," + std::to_string(y) +
                                   " +20=0x" + hex64(p20) + " +28=0x" + hex64(p28) +
                                   " +30=0x" + hex64(p30) + " +38=" + std::to_string(w38);
                if (readBits && p20 > 0x10000 && p20 < 0x7fffffffffffULL) {
                    const auto* bytes = reinterpret_cast<const unsigned char*>(p20);
                    unsigned first = 0;
                    std::memcpy(&first, bytes, 1);
                    text += " state_byte=" + std::to_string(first) +
                            " value=" + std::to_string(readBits(first, 8));
                }
                say(text);
            }
        }
        for (uint64_t i = 0; i < wires && i < 24; ++i) {
            const unsigned char* wire = records + i * 0x68;
            if (i < 3) {
                std::string dump = "DRIVER: wirehex " + std::string(phase) + " " +
                                   std::to_string(i) + " ";
                char byte[4];
                for (int k = 0; k < 0x48; ++k) {
                    std::snprintf(byte, sizeof(byte), "%02x", wire[k]);
                    dump += byte;
                    if ((k % 8) == 7) dump += ' ';
                }
                say(dump);
            }
            int16_t ax = 0, ay = 0, bx = 0, by = 0;
            std::memcpy(&ax, wire + 0x18, 2);
            std::memcpy(&ay, wire + 0x1a, 2);
            std::memcpy(&bx, wire + 0x1c, 2);
            std::memcpy(&by, wire + 0x1e, 2);
            uint64_t index = 0;
            std::memcpy(&index, wire + 0x38, sizeof(index));
            /* The game's own watcher path reads a state offset out of a byte
               string the record points at (get_sim_state -> get_state_index ->
               table[0]).  Try the record's own pointers instead of assuming the
               +0x38 field is that offset. */
            std::string probes;
            for (int slot = 0x20; slot <= 0x30; slot += 8) {
                uint64_t pointer = 0;
                std::memcpy(&pointer, wire + slot, sizeof(pointer));
                if (!readBits || pointer <= 0x10000 || pointer >= 0x7fffffffffffULL) continue;
                const auto* bytes = reinterpret_cast<const unsigned char*>(pointer);
                unsigned char head[4] = {0, 0, 0, 0};
                std::memcpy(head, bytes, sizeof(head));
                probes += " +" + std::to_string(slot) + "->[" + std::to_string(head[0]) + "," +
                          std::to_string(head[1]) + "," + std::to_string(head[2]) + "," +
                          std::to_string(head[3]) + "] r0=" +
                          std::to_string(readBits(head[0], 1)) + " r1=" +
                          std::to_string(readBits(head[1], 1));
            }
            const uint32_t keyA = static_cast<uint32_t>(static_cast<uint16_t>(ax)) |
                                  (static_cast<uint32_t>(static_cast<uint16_t>(ay)) << 16);
            const uint32_t keyB = static_cast<uint32_t>(static_cast<uint16_t>(bx)) |
                                  (static_cast<uint32_t>(static_cast<uint16_t>(by)) << 16);
            const int64_t idA = getWire ? getWire(model, keyA) : -1;
            const int64_t idB = getWire ? getWire(model, keyB) : -1;
            const uint64_t value = readState ? readState(static_cast<int64_t>(index)) : 0;
            std::string bits;
            if (readBits) {
                for (int width : {1, 2, 4, 8}) {
                    bits += " b" + std::to_string(width) + "=" +
                            std::to_string(readBits(static_cast<int64_t>(index), width));
                }
                /* Which readable indices near the record's own index are set?
                   Scanning a window is how the index→signal relation is found
                   instead of assumed. */
            bits += " set=[";
            int found = 0;
                for (int64_t k = 0; k <= 700 && found < 40; ++k) {
                    if (k >= 0 && readBits(k, 1) != 0) {
                        bits += std::to_string(k) + " ";
                        ++found;
                    }
                }
                bits += "]";
            }
            say(std::string("DRIVER: wire ") + phase + " " + std::to_string(i) + " a=" +
                std::to_string(ax) + "," +
                std::to_string(ay) + " b=" + std::to_string(bx) + "," + std::to_string(by) +
                " index=" + std::to_string(index) + " id(a)=" + std::to_string(idA) +
                " id(b)=" + std::to_string(idB) + " value=" + std::to_string(value) + bits +
                probes);
        }
    }

    /* Is the board model reachable from a global?  A plugin would rather read
       one variable than hook something (hooks are exclusive, see the handoff
       doc).  Candidate symbols are checked by comparing their contents with the
       model the driver already knows. */
    void probeModelGlobals() {
        if (!model || !host) return;
        static const char* names[] = {
            "undo_pointer__modelZboardZboard_u75",
            "selected_wires__modelZboardZboard_u30",
            "selected_components__modelZboardZboard_u22",
            "prev_selected_wires__modelZboardZboard_u44",
            "selection_kind_cache__modelZboardZboard_u9060",
            "SENTINEL__modelZboardZboard_u12742",
            "simulation_state__modelZsimulator95types_u81",
            "level_used_input__modelZsimulationZcontroller_u3",
            "level_used_outputs__modelZsimulationZcontroller_u4",
            nullptr};
        for (int index = 0; names[index]; ++index) {
            void* address = host->resolve_symbol(host->context, names[index]);
            if (!address) {
                say(std::string("DRIVER: global ") + names[index] + " -> not resolved");
                continue;
            }
            uint64_t contents = 0;
            std::memcpy(&contents, address, sizeof(contents));
            say(std::string("DRIVER: global ") + names[index] + " at=" +
                hex64(reinterpret_cast<uint64_t>(address)) + " contents=" + hex64(contents) +
                (reinterpret_cast<void*>(contents) == model ? " == model" : "") +
                ((address == model) ? " IS model" : ""));
        }
    }

    /* Picks the wire that sits on the level's output side (largest x of its two
       endpoints) so the test has a wire whose signal the level's own output
       history already tells us.  Returns its index in the wire table. */
    int outputSideWire() {
        if (!model || !host) return -1;
        auto* base = static_cast<unsigned char*>(model);
        uint64_t count = 0;
        void* data = nullptr;
        std::memcpy(&count, base + 0x98, sizeof(count));
        std::memcpy(&data, base + 0xa0, sizeof(data));
        if (!data || count == 0 || count > 4096) return -1;
        const auto* records = static_cast<const unsigned char*>(data) + 8;
        int best = -1;
        int bestX = -0x7fffffff;
        for (uint64_t i = 0; i < count; ++i) {
            const unsigned char* wire = records + i * 0x68;
            int16_t ax = 0, bx = 0;
            std::memcpy(&ax, wire + 0x18, 2);
            std::memcpy(&bx, wire + 0x1c, 2);
            const int x = ax > bx ? ax : bx;
            if (x > bestX) {
                bestX = x;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    void observeTrace(const tc::trace::Sampler& sampler) {
        this->sampler = &sampler;
        observedRows = sampler.rows();
        observedInputs = sampler.inputCount();
        observedOutputs = sampler.outputCount();
        if (observedRows == 0) return;
        const size_t last = observedRows - 1;
        for (int index = 0; index < observedInputs; ++index)
            observedMaxInput = std::max<uint64_t>(observedMaxInput, sampler.input(last, index));
        for (int index = 0; index < observedOutputs; ++index)
            if (sampler.output(last, index) != 0) observedHighOutput = true;
        std::string text = "cycle=" + std::to_string(sampler.cycleAt(last)) + " in=";
        for (int index = 0; index < observedInputs; ++index)
            text += (index ? "," : "") + std::to_string(sampler.input(last, index));
        text += " out=";
        for (int index = 0; index < observedOutputs; ++index)
            text += (index ? "," : "") + std::to_string(sampler.output(last, index));
        lastRow = text;
        /* Every row the panel would draw, in order: the playtest asserts on this
           sequence instead of on a single last value. */
        if (text != lastLoggedRow && loggedRows < 48) {
            lastLoggedRow = text;
            ++loggedRows;
            say("DRIVER: row " + std::to_string(loggedRows) + " " + text);
            /* While measuring the wire table, print what each wire's state slot
               holds for this row, next to the level's own input/output values. */
            if (dumpWires && loggedRows <= 8) dumpBoard(("row" + std::to_string(loggedRows)).c_str());
        }
    }

    static BOOL CALLBACK windowCallback(HWND candidate, LPARAM data) {
        DWORD owner = 0;
        GetWindowThreadProcessId(candidate, &owner);
        if (owner != GetCurrentProcessId()) return TRUE;
        if (!IsWindowVisible(candidate)) return TRUE;
        RECT rect{};
        if (!GetClientRect(candidate, &rect)) return TRUE;
        if (rect.right < 320 || rect.bottom < 240) return TRUE;
        *reinterpret_cast<HWND*>(data) = candidate;
        return FALSE;
    }

    void tick(void*, const TCFrame* frame) {
        if (!frame || done) return;
        if (!started) {
            started = true;
            start_frame = frame->frame_number;
            start_time = frame->time_seconds;
            HWND found = nullptr;
            EnumWindows(windowCallback, reinterpret_cast<LPARAM>(&found));
            window = found;
            if (window)
                SetWindowPos(window, nullptr, 0, 0, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            say("DRIVER: started frame=" + std::to_string(frame->frame_number));
        }
        elapsed = frame->time_seconds - start_time;
        /* Nothing in this driver blocks the game, so a level that never produces
           a trace has to end the run by itself; otherwise the playtest waits for
           its whole deadline to learn nothing. */
        if (elapsed > 30.0) {
            done = true;
            say("DRIVER: giving up after " + std::to_string(static_cast<int>(elapsed)) +
                "s, rows=" + std::to_string(observedRows));
            return;
        }
        if (stage == 0 && elapsed > 4.0) stage = 1;
        if (stage == 1 && model && elapsed > 5.0) {
            entered = true;
            stage = 2;
            if (loadLevel) loadLevel(model);
            say("DRIVER: asked the plugin to load the level");
            return;
        }
        if (stage == 2 && elapsed > 8.0) {
            if (dumpWires && !dumped) {
                dumped = true;
                probeModelGlobals();
                dumpBoard("before");
            }
            if (pauseCheckTime == 0) {
                pauseCheckTime = elapsed;
                pausedRows = observedRows;
                say("DRIVER: paused rows=" + std::to_string(pausedRows));
                return;
            }
            if (elapsed < pauseCheckTime + 2.0) return;
            pausedRowsLater = observedRows;
            say("DRIVER: paused rows after 2s=" + std::to_string(pausedRowsLater));
            if (pickWire) {
                const int wire = outputSideWire();
                if (wire >= 0) {
                    say("DRIVER: asking the panel to probe wire " + std::to_string(wire) +
                        " (the one on the level's output side)");
                    pickWire(wire);
                }
            }
            stage = 3;
            if (runTo) runTo(6);
            say("DRIVER: running the simulation to cycle 6");
            return;
        }
        if (stage == 3 && observedRows >= 3 && observedMaxInput >= 3 && observedHighOutput) {
            stage = 4;
            if (dumpWires) {
                /* Stop the simulation first: a dump taken while it runs mixes
                   samples from different cycles (learned the hard way). */
                if (pauseSim) pauseSim(model);
                pausedDumped = false;
                pauseDumpFrame = frame ? frame->frame_number : 0;
            }
            say("DRIVER: traced " + std::to_string(observedRows) + " rows, inputs=" +
                std::to_string(observedInputs) + " outputs=" +
                std::to_string(observedOutputs) + ", last row " + lastRow);
            /* With a probe in place, every row can be compared against the level's
               own output history: that is what turns "the panel shows something"
               into "the panel shows the wire's signal". */
            if (sampler && probeIndex >= 0) {
                for (size_t row = 0; row < sampler->rows(); ++row) {
                    say("DRIVER: probe row=" + std::to_string(row) + " cycle=" +
                        std::to_string(sampler->cycleAt(row)) + " value=" +
                        std::to_string(sampler->probeValue(row, probeIndex)) + " out0=" +
                        std::to_string(sampler->output(row, 0)));
                }
            }
            exportRequested = true;
            say("DRIVER: asked for a VCD export");
            return;
        }
        /* Before the trace is complete, once the simulation has reached cycle 2
           (output still low) pause and take a settled dump: comparing it with the
           cycle-3 dump says whether a probe byte really follows the signal. */
        if (dumpWires && !settleDumped && stage == 3) {
            if (!settleRequested) {
                if (sampler && sampler->rows() > 0 && sampler->lastCycle() >= 2) {
                    if (pauseSim) pauseSim(model);
                    settleRequested = true;
                    settleFrame = frame ? frame->frame_number : 0;
                }
            } else if (frame && frame->frame_number >= settleFrame + 6) {
                settleDumped = true;
                dumpBoard("cycle2");
                if (runTo) runTo(6);
            }
            if (settleRequested && !settleDumped) return;
        }
        if (dumpWires && !pausedDumped && stage >= 4 &&
            frame && frame->frame_number >= pauseDumpFrame + 6) {
            pausedDumped = true;
            dumpBoard("paused");
        }
        if (stage == 4 && exportReported) {
            stage = 5;
            say("DRIVER: export rows=" + std::to_string(exportedRows) + " path=" + exportPath);
            /* The framebuffer read returns the frame from two frames ago, so the
               capture is asked for a few frames after the last row arrived: the
               picture then shows the finished waveform instead of the frame
               before its last step.  A few frames, not a second: this level's
               test finishes right after that and the scene then changes. */
            exportFrame = frame ? frame->frame_number : 0;
            return;
        }
        if (stage == 5) {
            if (frame && frame->frame_number < exportFrame + 4) return;
            captureRequested = true;
            captureRequestTime = elapsed;
            say("DRIVER: asked for a framebuffer capture");
            stage = 6;
            return;
        }
        if (stage == 6) {
            if (captured) {
                stage = 7;
                done = true;
                say("DRIVER: image " + imagePath);
                say("DRIVER: done");
            } else if (elapsed > captureRequestTime + 6.0) {
                done = true;
                say("DRIVER: no framebuffer capture arrived");
            }
        }
    }
};

inline Driver& driver() { static Driver value; return value; }

inline bool hookInvisible(const char* id, V2 size, int flags) {
    auto& d = driver();
    const bool result = d.invisibleOriginal ? d.invisibleOriginal(id, size, flags) : false;
    const auto rva = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                     reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!d.entered && rva >= 0x449df0 && rva < 0x44b610 && d.elapsed > 4.0) {
        const auto get_frame = reinterpret_cast<int (*)()>(
            d.host->engine_proc(d.host->context, "igGetFrameCount"));
        const int frame = get_frame ? get_frame() : 0;
        if (frame != d.menu_frame) { d.menu_frame = frame; d.menu_clicks = 0; }
        if (++d.menu_clicks == 2) {
            d.say("DRIVER: pressing a home page entry to reach a board");
            return true;
        }
    }
    return result;
}

inline bool hookUpdateWire(void* m, void* context, void* input, uint32_t point, uint8_t fifth) {
    auto& d = driver();
    if (!d.model) d.model = m;
    return d.updateOriginal ? d.updateOriginal(m, context, input, point, fifth) : false;
}

inline void hookCandidateA(void* a0, void* a1, void* a2, void* a3) {
    auto& d = driver();
    d.noteA(a0, a1, a2, a3);
    if (d.candidateA) reinterpret_cast<void (*)(void*, void*, void*, void*)>(d.candidateA)(a0, a1, a2, a3);
}

inline void hookCandidateB(void* a0, void* a1, void* a2, void* a3) {
    auto& d = driver();
    d.noteB(a0, a1, a2, a3);
    if (d.candidateB) reinterpret_cast<void (*)(void*, void*, void*, void*)>(d.candidateB)(a0, a1, a2, a3);
}

inline bool start(const TCHost* host, void (*logFunction)(const char*),
                  void (*loadLevel)(void*), void (*runTo)(int64_t),
                  void (*pauseSim)(void*) = nullptr,
                  void (*pickWire)(int) = nullptr) {
    auto& d = driver();
    if (d.installed) return false;
    d.host = host;
    d.log = logFunction;
    d.loadLevel = loadLevel;
    d.runTo = runTo;
    d.pauseSim = pauseSim;
    d.pickWire = pickWire;
    d.dumpWires = std::getenv("TC_WAVE_DUMP") != nullptr;
    struct Target { const char* symbol; void* detour; void** original; };
    const Target targets[] = {
        {"igInvisibleButton", reinterpret_cast<void*>(hookInvisible),
         reinterpret_cast<void**>(&d.invisibleOriginal)},
        {"handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5",
         reinterpret_cast<void*>(hookUpdateWire),
         reinterpret_cast<void**>(&d.updateOriginal)},
        {"update_wire__modelZboardZboard_u19554", reinterpret_cast<void*>(hookCandidateA),
         reinterpret_cast<void**>(&d.candidateA)},
        {"board_update_wires__modelZboardZboard_u22494", reinterpret_cast<void*>(hookCandidateB),
         reinterpret_cast<void**>(&d.candidateB)},
    };
    for (const auto& target : targets) {
        void* address = host->resolve_symbol(host->context, target.symbol);
        if (!address) {
            d.say(std::string("DRIVER: no symbol ") + target.symbol);
            continue;
        }
        if (host->create_hook(host->context, address, target.detour, target.original) != 0) {
            d.say(std::string("DRIVER: cannot hook ") + target.symbol);
        }
    }
    d.installed = true;
    return true;
}

inline void tick(void*, const TCFrame* frame) { driver().tick(nullptr, frame); }

}  // namespace tc_wave_driver
#endif  // TC_WAVE_DRIVER
