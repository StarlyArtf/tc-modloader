/* simcore-view: simcore's own window.

   The game is not involved anywhere in this program.  It reads a board
   fixture, asks simcore's netlist builder what the board is, optionally runs
   simcore's engine on it, and draws the result with Win32 GDI - a renderer
   with nothing in common with the game's ImGui pass.  That independence is
   the reason it exists: when the game's board and this picture disagree about
   a value, the disagreement is information, not a rendering artefact.

   The chrome (toolbar, panel, status bar) is drawn by hand rather than built
   from dialog resources.  There is no .rc file to keep in step, the build stays
   one g++ invocation, and the buttons look like the rest of the tool instead of
   like a 1995 installer.

   Modes:
     simcore-view                            open the picker, then a window
     simcore-view --board FILE               open a window
     simcore-view --board FILE --svg OUT     also/instead write an SVG
     simcore-view --board FILE --bmp OUT     also/instead write a BMP frame
     simcore-view --board FILE --no-window   headless (for scripts) */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <windowsx.h>

#include "board_file.hpp"
#include "kind_pins.hpp"
#include "scene.hpp"
#include "svg_writer.hpp"

#include "../include/tcsim/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

/* ------------------------------------------------------------------ options */

struct Options {
    std::string board;
    std::string svg;
    std::string bmp;
    bool window = true;
    tcsim::Tick ticks = 200;
    bool tidy = false;
    int select_component = -1;
    int select_wire = -1;
    /* --watch: the board record is written by the game while it runs (the
       sandbox simulator's `dump`), so re-read it whenever the file changes and
       keep the camera and the selection - the point is to watch the values move,
       not to lose one's place every time they do. */
    bool watch = false;
    int width = 0;
    int height = 0;
};

/* -------------------------------------------------------------------- state */

/* Where the picture sits on the canvas.  World coordinates are the scene's own
   pixel coordinates at 1:1, so a view is just a zoom and the world point that
   the canvas' top-left corner shows. */
struct View {
    float zoom = 1.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
};

struct Runner {
    std::string board_path;
    simcore_view::BoardFile file;
    tcsim::BuildResult built;
    tcsim::NetList values;
    simcore_view::Scene scene;
    View view;
    bool fatal = false;
    bool ran = false;
    uint64_t pattern = 0;
    tcsim::Tick now = 0;
    tcsim::Tick ticks = 200;
    bool tidy = false;
    simcore_view::ScenePick selected;
    std::string status;
};

void rebuild(Runner& runner) {
    const tcsim::Tick ticks = runner.ticks;
    runner.scene = simcore_view::Scene{};
    runner.ran = false;
    runner.now = 0;
    runner.status.clear();
    if (!runner.file.ok) {
        runner.status = runner.file.error;
        return;
    }

    tcsim::BuildOptions build_options;
    /* A gate gets a real one-tick arc here rather than settling through delta
       rounds: the point of the picture is to show propagation, and a
       zero-delay ring would (correctly) be reported as an oscillation instead
       of swinging. */
    build_options.gate_delay = tcsim::ArcDelay{2, 2};
    /* The record's own pins and any custom definitions behind its components
       are registered and flattened by netlistView, so the window and the offline
       tests run one path (see board_file.hpp). */
    const tcsim::BoardView netlist_view = simcore_view::netlistView(runner.file);
    runner.built = tcsim::buildNetList(netlist_view, &simcore_view::pinsForKind, build_options);
    runner.fatal = runner.built.fatal();

    if (!runner.fatal) {
        tcsim::Engine engine(runner.built.netlist, tcsim::EngineOptions{});
        bool seeded = false;
        for (size_t index = 0; index < runner.built.source_nets.size(); ++index) {
            const tcsim::NetId net = runner.built.source_nets[index];
            if (net == tcsim::kNoNet || net >= engine.netlist().netCount()) continue;
            const uint16_t bits = engine.netlist().net(net).bits;
            /* What drives this source, in order of authority: the value the
               record carries for its net (what the board shows right now), then
               the level the record carries for a Mod switch/button, then the
               Space-key pattern for a board the record does not describe. */
            int level = -1;
            for (size_t wire = 0; wire < runner.file.wire_values.size(); ++wire) {
                if (runner.file.wire_values[wire] > 1 || wire >= runner.built.net_of_wire.size()) continue;
                if (runner.built.net_of_wire[wire] == net) {
                    level = runner.file.wire_values[wire];
                    break;
                }
            }
            if (level < 0 && index < runner.built.source_origins.size()) {
                const uint64_t instance = runner.built.source_origins[index].instance_id;
                const size_t count = std::min(runner.file.board.components.size(),
                                              runner.file.component_levels.size());
                for (size_t component = 0; component < count; ++component) {
                    if (runner.file.board.components[component].id != instance) continue;
                    if (runner.file.component_levels[component] > 1) continue;
                    level = runner.file.component_levels[component];
                }
            }
            if (level < 0) level = static_cast<int>((runner.pattern >> (index % 64)) & 1u);
            const tcsim::Logic logic = level ? tcsim::Logic::kOne : tcsim::Logic::kZero;
            engine.addStimulus(0, net, tcsim::BitVector(bits ? bits : 1, logic), tcsim::Strength::kStrong);
        }
        /* A board with no external source is a feedback-only circuit (the ring
           demo).  Its nets resolve to X and stay there - simcore's documented
           behaviour for an unseeded loop, not an oscillation.  The engine's own
           seedNet is the startup aid for exactly this case: it hands the loop a
           deterministic starting point, and the first real device update
           replaces it completely. */
        engine.runUntil(0);
        /* The board's own values, once the first evaluation has run: a latch
           holds whatever it held when the record was written, and replaying from
           a blank slate would lose it.  Seeding *before* the first evaluation
           would not survive it - the engine says as much where it offers this. */
        for (size_t index = 0; index < runner.file.wire_values.size(); ++index) {
            if (runner.file.wire_values[index] > 1 || index >= runner.built.net_of_wire.size()) continue;
            const tcsim::NetId net = runner.built.net_of_wire[index];
            if (net == tcsim::kNoNet || net >= engine.netlist().netCount()) continue;
            /* A source net is already driven above; only feedback nets are left
               to be given a starting state. */
            if (std::find(runner.built.source_nets.begin(), runner.built.source_nets.end(), net) !=
                runner.built.source_nets.end())
                continue;
            const uint16_t wire_bits = engine.netlist().net(net).bits;
            engine.seedNet(net, tcsim::BitVector(wire_bits ? wire_bits : 1,
                                                 runner.file.wire_values[index] ? tcsim::Logic::kOne
                                                                               : tcsim::Logic::kZero));
        }
        if (runner.built.source_nets.empty() && engine.netlist().netCount() > 0) {
            const uint16_t bits = engine.netlist().net(0).bits;
            const tcsim::Logic logic = (runner.pattern & 1u) ? tcsim::Logic::kOne : tcsim::Logic::kZero;
            seeded = engine.seedNet(0, tcsim::BitVector(bits ? bits : 1, logic));
        }
        engine.runUntil(ticks);
        runner.values = engine.netlist();
        runner.now = engine.now();
        runner.ran = true;
        runner.status = "tick " + std::to_string(runner.now) + "   " +
                        std::to_string(runner.built.netlist.netCount()) + " nets   " +
                        std::to_string(engine.evaluations()) + " evaluations";
        if (seeded) runner.status += "   seeded net 0";
        if (engine.aborted()) runner.status += "   ENGINE ABORTED ON A VIOLATION";
    } else {
        runner.status = "the netlist builder refused this board - see the notes";
    }

    simcore_view::SceneInput input;
    input.board = &runner.file.board;
    input.built = &runner.built;
    input.values = runner.ran ? &runner.values : nullptr;
    input.wire_paths = &runner.file.wire_paths;
    simcore_view::SceneOptions scene_options;
    scene_options.tidy_layout = runner.tidy;
    runner.scene = simcore_view::buildScene(input, scene_options);
}

/* What a click on a component or a wire is worth knowing.

   This is where the picture stops being a picture and starts being a
   diagnostic: a wire's net is what simcore resolved, and `slots_of_net` is
   where that value is written back into the game's state array.  Reading both
   on the same screen is what tells the three failure modes apart - a wrong
   value, a right value written to the wrong place, and a right value the game
   then draws wrongly. */
std::vector<std::string> selectionLines(const Runner& runner) {
    std::vector<std::string> lines;
    if (!runner.selected.valid) return lines;
    const tcsim::BuildResult& built = runner.built;
    const tcsim::NetList* values = runner.ran ? &runner.values : nullptr;

    const auto netText = [&](tcsim::NetId net) {
        if (net == tcsim::kNoNet) return std::string("(unconnected)");
        std::string text = "net " + std::to_string(net);
        if (values && net < values->netCount()) text += " = " + values->net(net).value.toString();
        return text;
    };
    const auto flagsOf = [&](tcsim::NetId net) {
        const auto listed = [&](const std::vector<tcsim::NetId>& list) {
            return std::find(list.begin(), list.end(), net) != list.end();
        };
        std::string flags;
        if (listed(built.source_nets)) flags += "  [外部驱动]";
        if (listed(built.observed_nets)) flags += "  [游戏会读回]";
        if (listed(built.clock_nets)) flags += "  [时钟]";
        return flags;
    };
    const auto kindText = [](uint8_t kind) {
        char text[8] = {};
        std::snprintf(text, sizeof(text), "0x%02X", static_cast<unsigned>(kind));
        return std::string(text);
    };

    if (runner.selected.on_component) {
        const size_t index = runner.selected.component;
        if (index >= runner.file.board.components.size()) return lines;
        const tcsim::BoardComponent& component = runner.file.board.components[index];
        tcsim::KindPins pins;
        const bool known = simcore_view::pinsForKind(component, pins);
        char header[200] = {};
        std::snprintf(header, sizeof(header), "元件 %zu   %s", index,
                      known ? pins.name : simcore_view::scene_detail::fallbackName(component).c_str());
        lines.push_back(header);
        char where[200] = {};
        std::snprintf(where, sizeof(where), "kind %s   位置 %d,%d   旋转 %d°", kindText(component.kind).c_str(),
                      component.x, component.y, component.rotation * 90);
        lines.push_back(where);
        lines.push_back("引脚 入 " + std::to_string(component.input_count) + "  出 " +
                        std::to_string(component.output_count));
        const tcsim::DeviceId device =
            index < built.devices_by_component.size() ? built.devices_by_component[index] : tcsim::kNoDevice;
        if (device == tcsim::kNoDevice) {
            lines.push_back("没有对应器件：simcore 不仿真这个元件");
        } else if (!values || device >= values->deviceCount()) {
            lines.push_back("器件 " + std::to_string(device) + "（引擎没有运行）");
        } else {
            const tcsim::DeviceInstance& instance = values->device(device);
            lines.push_back("器件 " + std::to_string(device) + "  " + instance.name);
            for (size_t pin = 0; pin < instance.inputs.size(); ++pin) {
                lines.push_back("  入" + std::to_string(pin) + " -> " + netText(instance.inputs[pin]));
            }
            for (size_t pin = 0; pin < instance.outputs.size(); ++pin) {
                lines.push_back("  出" + std::to_string(pin) + " -> " + netText(instance.outputs[pin]));
            }
        }
        return lines;
    }

    if (runner.selected.on_wire) {
        const size_t index = runner.selected.wire;
        if (index >= runner.file.board.wires.size()) return lines;
        const tcsim::BoardWire& wire = runner.file.board.wires[index];
        char header[220] = {};
        std::snprintf(header, sizeof(header), "导线 %zu   %d,%d → %d,%d   %u 位", index, wire.x1, wire.y1, wire.x2,
                      wire.y2, static_cast<unsigned>(wire.width));
        lines.push_back(header);
        const tcsim::NetId net = index < built.net_of_wire.size() ? built.net_of_wire[index] : tcsim::kNoNet;
        if (net == tcsim::kNoNet) {
            lines.push_back("不属于任何 net");
            return lines;
        }
        lines.push_back(netText(net) + flagsOf(net));
        std::string slots = "写回状态字节：";
        if (net < built.slots_of_net.size()) {
            if (built.slots_of_net[net].empty()) {
                slots += "无（没有东西把它读回去）";
            } else {
                for (uint64_t slot : built.slots_of_net[net]) slots += " " + std::to_string(slot);
            }
        }
        lines.push_back(slots);
        return lines;
    }
    return lines;
}

/* ------------------------------------------------------------------ paths --
   This program is meant to be opened by a person, not only driven by a script,
   so the board argument is a name as often as it is a path: "ring3.txt" has to
   work from wherever a shortcut happened to start.  The fixtures that ship
   next to the source are also searched relative to the executable. */

bool fileReadable(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return static_cast<bool>(file);
}

std::string executableDirectory() {
    char buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    const std::string path(buffer, length);
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string resolveBoardPath(const std::string& given) {
    if (fileReadable(given)) return given;
    const std::string here = executableDirectory();
    if (!here.empty()) {
        const std::string candidates[] = {
            here + "\\" + given,
            here + "\\..\\" + given,
            here + "\\..\\..\\" + given,
            here + "\\data\\" + given,
            here + "\\..\\simcore\\view\\data\\" + given,
            here + "\\..\\..\\simcore\\view\\data\\" + given,
            /* open.cmd's own help suggests "--board data\\latch.txt", which is
               relative to the view directory rather than to the working
               directory, so both spellings have to resolve. */
            here + "\\..\\simcore\\view\\" + given,
            here + "\\..\\..\\simcore\\view\\" + given,
        };
        for (const std::string& candidate : candidates) {
            if (fileReadable(candidate)) return candidate;
        }
    }
    return {};
}

/* A file picker, so the program can be started by double-clicking it with no
   arguments at all and still be usable. */
std::string chooseBoardFile(const std::string& start_directory) {
    char chosen[MAX_PATH] = {};
    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = "Board fixtures (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = chosen;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrInitialDir = start_directory.empty() ? nullptr : start_directory.c_str();
    dialog.lpstrTitle = "Open a simcore board fixture";
    /* OFN_NOCHANGEDIR matters here: a picker that quietly moved the process's
       working directory would break the runner's own relative paths. */
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&dialog)) return {};
    return chosen;
}

std::string defaultFixtureDirectory() {
    const std::string here = executableDirectory();
    return here.empty() ? std::string() : here + "\\..\\simcore\\view\\data";
}

/* Just the file name: the full path is in the title bar and on stdout, and the
   toolbar is a narrow strip that the path would always overflow. */
std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

Options parseArguments(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto value_of = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                return {};
            }
            return argv[++index];
        };
        if (argument == "--board") {
            options.board = value_of("--board");
        } else if (argument == "--svg") {
            options.svg = value_of("--svg");
        } else if (argument == "--bmp") {
            options.bmp = value_of("--bmp");
        } else if (argument == "--ticks") {
            options.ticks = std::strtoll(value_of("--ticks").c_str(), nullptr, 10);
        } else if (argument == "--tidy") {
            options.tidy = true;
        } else if (argument == "--select-component") {
            options.select_component = static_cast<int>(std::strtol(value_of("--select-component").c_str(), nullptr, 10));
        } else if (argument == "--select-wire") {
            options.select_wire = static_cast<int>(std::strtol(value_of("--select-wire").c_str(), nullptr, 10));
        } else if (argument == "--size") {
            const std::string size = value_of("--size");
            const size_t at = size.find('x');
            if (at != std::string::npos) {
                options.width = static_cast<int>(std::strtol(size.substr(0, at).c_str(), nullptr, 10));
                options.height = static_cast<int>(std::strtol(size.substr(at + 1).c_str(), nullptr, 10));
            }
        } else if (argument == "--no-window") {
            options.window = false;
        } else if (argument == "--watch") {
            options.watch = true;
        } else if (argument == "--help" || argument == "-h") {
            std::printf("simcore-view [--board FILE] [--svg OUT] [--bmp OUT] [--ticks N] [--size WxH]\n"
                        "             [--tidy] [--select-component N] [--select-wire N] [--no-window]\n"
                        "             [--watch]\n");
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
            std::exit(2);
        }
    }
    return options;
}

/* ------------------------------------------------------------------ chrome */

constexpr int kToolbarHeight = 42;
constexpr int kStatusHeight = 26;
constexpr int kPanelMinimum = 400;
constexpr int kPanelMaximum = 620;

inline const COLORREF kChrome = RGB(247, 247, 250);
inline const COLORREF kChromeEdge = RGB(212, 212, 220);
inline const COLORREF kToolbarFace = RGB(241, 241, 246);
inline const COLORREF kButtonFace = RGB(255, 255, 255);
inline const COLORREF kButtonHover = RGB(230, 239, 254);
inline const COLORREF kButtonDown = RGB(206, 225, 252);
inline const COLORREF kButtonEdge = RGB(202, 202, 212);
/* Filled with the scene's own canvas colour so a wire's casing disappears
   against it. */
inline const COLORREF kCanvas =
    RGB(simcore_view::kCanvasFill.r, simcore_view::kCanvasFill.g, simcore_view::kCanvasFill.b);
inline const COLORREF kInk = RGB(24, 24, 32);
inline const COLORREF kInkSoft = RGB(104, 104, 116);
inline const COLORREF kAccent = RGB(198, 100, 0);
inline const COLORREF kActiveInk = RGB(24, 90, 190);
inline const COLORREF kNoteInk = RGB(176, 40, 40);
inline const COLORREF kGoodInk = RGB(30, 128, 60);

struct Layout {
    RECT toolbar{};
    RECT canvas{};
    RECT panel{};
    RECT status{};
};

struct ToolButton {
    RECT rect{};
    std::string label;
    int action = 0;
};

enum {
    kActionOpen = 1,
    kActionReload,
    kActionPrevInput,
    kActionNextInput,
    kActionResetInput,
    kActionZoomOut,
    kActionZoomIn,
    kActionFit,
    kActionTidy,
    kActionSaveSvg,
    kActionSaveBmp,
};

struct UiState {
    Layout layout{};
    std::vector<ToolButton> buttons;
    int hover_button = -1;
    int pressed_button = -1;
    simcore_view::ScenePick hover_pick;
    float cursor_x = 0.0f;
    float cursor_y = 0.0f;
    bool cursor_on_canvas = false;
    bool panning = false;
    bool pan_moved = false;
    POINT pan_from{};
    View pan_start{};
};

Runner* g_runner = nullptr;
UiState g_ui;
std::string g_svg_path;
std::string g_bmp_path;

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), needed);
    return wide;
}

/* Text alignment is done here rather than with TA_* flags.  The reason is that
   TA_CENTER and VTA_CENTER share a value, so `TA_LEFT | TA_CENTER` is not
   "left and vertically centred" - it is plain TA_CENTER, which centres on both
   axes and pushes left-aligned status text off the edge of the window.  Asking
   for the extent and placing the origin is unambiguous. */
enum {
    kAlignLeft = 0,
    kAlignCenterX = 1,
    kAlignRightX = 2,
    kAlignMiddleY = 4,
};

void drawText(HDC device, int x, int y, const std::string& text, int align = kAlignLeft) {
    const std::wstring wide = widen(text);
    if (wide.empty()) return;
    SIZE size{};
    GetTextExtentPoint32W(device, wide.c_str(), static_cast<int>(wide.size()), &size);
    int draw_x = x;
    int draw_y = y;
    if (align & kAlignCenterX) draw_x -= size.cx / 2;
    if (align & kAlignRightX) draw_x -= size.cx;
    if (align & kAlignMiddleY) draw_y -= size.cy / 2;
    const UINT saved = SetTextAlign(device, TA_LEFT | TA_TOP);
    TextOutW(device, draw_x, draw_y, wide.c_str(), static_cast<int>(wide.size()));
    SetTextAlign(device, saved);
}

int textWidth(HDC device, const std::string& text) {
    const std::wstring wide = widen(text);
    if (wide.empty()) return 0;
    SIZE size{};
    GetTextExtentPoint32W(device, wide.c_str(), static_cast<int>(wide.size()), &size);
    return static_cast<int>(size.cx);
}

/* Cut a string down to fit a width, on a UTF-8 character boundary so a
   Chinese path does not end in half a glyph. */
std::string elide(HDC device, const std::string& text, int max_width) {
    if (max_width <= 0) return {};
    if (textWidth(device, text) <= max_width) return text;
    std::string result = text;
    while (!result.empty()) {
        result.pop_back();
        while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xC0) == 0x80) result.pop_back();
        if (textWidth(device, result + "...") <= max_width) break;
    }
    return result + "...";
}

void fillRect(HDC device, const RECT& rect, COLORREF colour) {
    HBRUSH brush = CreateSolidBrush(colour);
    FillRect(device, &rect, brush);
    DeleteObject(brush);
}

void strokeRect(HDC device, const RECT& rect, COLORREF colour, int thickness = 1) {
    HPEN pen = CreatePen(PS_SOLID, thickness, colour);
    HGDIOBJ old_pen = SelectObject(device, pen);
    HGDIOBJ old_brush = SelectObject(device, GetStockObject(NULL_BRUSH));
    Rectangle(device, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(device, old_brush);
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

int panelWidthFor(const Runner& runner) {
    size_t longest = 0;
    for (const std::string& note : runner.scene.notes) longest = std::max(longest, note.size());
    for (const std::string& line : selectionLines(runner)) longest = std::max(longest, line.size());
    /* Chinese notes are three UTF-8 bytes per glyph but only about one glyph
       wide, so a byte count is already a generous estimate. */
    const int wanted = static_cast<int>(longest) * 7 + 48;
    return std::min(kPanelMaximum, std::max(kPanelMinimum, wanted));
}

/* The panel is the half of the window that carries the answer, so the frame is
   never allowed to be shorter than the panel needs.  A board with three wires
   would otherwise squeeze the net/slot block into the hints at the bottom. */
constexpr size_t kMaxSelectionLines = 10;
constexpr size_t kMaxNotes = 12;
/* Room for a 13 px font with air between lines; tighter and the Chinese glyphs
   in a slot list touch the line above. */
constexpr int kLineHeight = 19;

size_t visibleSelectionLines(const Runner& runner) {
    return std::min(selectionLines(runner).size(), kMaxSelectionLines);
}
size_t visibleNotes(const Runner& runner) {
    return std::min(runner.scene.notes.size(), kMaxNotes);
}

int panelContentHeight(const Runner& runner) {
    int height = 14 + 28; /* top margin, then "选中" and its rule */
    const size_t selection = visibleSelectionLines(runner);
    height += selection == 0 ? 44 : static_cast<int>(selection) * kLineHeight + 10;
    height += 34; /* the value legend row, measured from its own top */
    height += 28; /* "备注" and its rule */
    height += std::max<size_t>(1, visibleNotes(runner)) * kLineHeight;
    height += 76; /* the two hint lines and a bottom margin */
    return height;
}

/* How wide the button row wants to be, so the frame can be widened rather than
   letting buttons run under the board name. */
/* One table, so the row that is drawn and the row that is measured cannot
   drift apart. */
struct ButtonSpec {
    const char* label;
    int action;
    bool separator_before;
};

inline const ButtonSpec kButtons[] = {
    {"打开", kActionOpen, false},
    {"重载", kActionReload, false},
    {"输入 ◀", kActionPrevInput, true},
    {"输入 ▶", kActionNextInput, false},
    {"复位", kActionResetInput, false},
    {"－", kActionZoomOut, true},
    {"＋", kActionZoomIn, false},
    {"适应", kActionFit, false},
    {"整理", kActionTidy, false},
    {"存 SVG", kActionSaveSvg, true},
    {"存 BMP", kActionSaveBmp, false},
};

int toolbarNaturalWidth() {
    HDC device = GetDC(nullptr);
    HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HGDIOBJ old_font = SelectObject(device, font);
    int width = 12;
    for (const ButtonSpec& button : kButtons) {
        if (button.separator_before) width += 17;
        width += textWidth(device, button.label) + 22 + 6;
    }
    SelectObject(device, old_font);
    DeleteObject(font);
    ReleaseDC(nullptr, device);
    return width + 24;
}

Layout layoutFor(const Runner& runner, int width, int height) {
    Layout layout;
    const int panel = std::min(panelWidthFor(runner), std::max(160, width / 2));
    layout.toolbar = RECT{0, 0, width, kToolbarHeight};
    layout.status = RECT{0, height - kStatusHeight, width, height};
    layout.panel = RECT{width - panel, kToolbarHeight, width, height - kStatusHeight};
    layout.canvas = RECT{0, kToolbarHeight, layout.panel.left, height - kStatusHeight};
    return layout;
}

void buildToolbar(HDC device, const Runner& runner) {
    g_ui.buttons.clear();
    const int top = 8;
    const int height = 26;
    int x = 12;
    for (const ButtonSpec& spec : kButtons) {
        if (spec.separator_before) {
            x += 8;
            const RECT line{x, top + 3, x + 1, top + height - 3};
            fillRect(device, line, kChromeEdge);
            x += 9;
        }
        ToolButton button;
        const int wide = textWidth(device, spec.label) + 22;
        button.rect = RECT{x, top, x + wide, top + height};
        button.label = spec.label;
        button.action = spec.action;
        g_ui.buttons.push_back(button);
        x += wide + 6;
    }
    (void)runner;
}

/* ------------------------------------------------------------ view mapping */

inline float toCanvasX(const Layout& layout, const View& view, float world_x) {
    return static_cast<float>(layout.canvas.left) + (world_x - view.offset_x) * view.zoom;
}
inline float toCanvasY(const Layout& layout, const View& view, float world_y) {
    return static_cast<float>(layout.canvas.top) + (world_y - view.offset_y) * view.zoom;
}
inline float toWorldX(const Layout& layout, const View& view, float canvas_x) {
    return view.offset_x + (canvas_x - static_cast<float>(layout.canvas.left)) / view.zoom;
}
inline float toWorldY(const Layout& layout, const View& view, float canvas_y) {
    return view.offset_y + (canvas_y - static_cast<float>(layout.canvas.top)) / view.zoom;
}

void fitView(Runner& runner, const Layout& layout) {
    const float canvas_w = static_cast<float>(std::max<LONG>(1, layout.canvas.right - layout.canvas.left));
    const float canvas_h = static_cast<float>(std::max<LONG>(1, layout.canvas.bottom - layout.canvas.top));
    const float scene_w = std::max(1.0f, runner.scene.width);
    const float scene_h = std::max(1.0f, runner.scene.height);
    const float zoom = std::min((canvas_w - 28.0f) / scene_w, (canvas_h - 28.0f) / scene_h);
    runner.view.zoom = std::min(4.0f, std::max(0.2f, zoom));
    runner.view.offset_x = (scene_w - canvas_w / runner.view.zoom) * 0.5f;
    runner.view.offset_y = (scene_h - canvas_h / runner.view.zoom) * 0.5f;
}

void zoomAt(Runner& runner, const Layout& layout, float canvas_x, float canvas_y, float factor) {
    const float before_x = toWorldX(layout, runner.view, canvas_x);
    const float before_y = toWorldY(layout, runner.view, canvas_y);
    runner.view.zoom = std::min(6.0f, std::max(0.15f, runner.view.zoom * factor));
    runner.view.offset_x = before_x - (canvas_x - static_cast<float>(layout.canvas.left)) / runner.view.zoom;
    runner.view.offset_y = before_y - (canvas_y - static_cast<float>(layout.canvas.top)) / runner.view.zoom;
}

/* -------------------------------------------------------------- painters -- */

void paintToolbar(HDC device, const Runner& runner) {
    const Layout& layout = g_ui.layout;
    fillRect(device, layout.toolbar, kToolbarFace);
    fillRect(device, RECT{layout.toolbar.left, layout.toolbar.bottom - 1, layout.toolbar.right,
                          layout.toolbar.bottom}, kChromeEdge);

    HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HGDIOBJ old_font = SelectObject(device, font);
    SetBkMode(device, TRANSPARENT);

    for (size_t index = 0; index < g_ui.buttons.size(); ++index) {
        const ToolButton& button = g_ui.buttons[index];
        const bool hovered = static_cast<int>(index) == g_ui.hover_button;
        const bool pressed = static_cast<int>(index) == g_ui.pressed_button;
        /* A toggle has to look like one: "整理" stays lit while the tidy grid
           is what is on screen, because that grid is not the board. */
        const bool active = button.action == kActionTidy && runner.tidy;
        fillRect(device, button.rect, (pressed || active) ? kButtonDown : (hovered ? kButtonHover : kButtonFace));
        strokeRect(device, button.rect, (hovered || pressed || active) ? kAccent : kButtonEdge);
        SetTextColor(device, kInk);
        drawText(device, (button.rect.left + button.rect.right) / 2, (button.rect.top + button.rect.bottom) / 2,
                 button.label, kAlignCenterX | kAlignMiddleY);
    }

    /* The board name lives in the toolbar rather than only in a title bar: the
       title bar is what a screenshot crops off. */
    /* The board name belongs in the toolbar rather than only in the title bar:
       the title bar is the first thing a screenshot crops off.  It is elided,
       never allowed to run underneath the buttons. */
    const int last_right = g_ui.buttons.empty() ? 0 : static_cast<int>(g_ui.buttons.back().rect.right);
    const int room = static_cast<int>(layout.toolbar.right) - 16 - last_right - 16;
    if (room > 40) {
        SetTextColor(device, kInkSoft);
        drawText(device, layout.toolbar.right - 14, kToolbarHeight / 2, elide(device, baseName(runner.board_path), room),
                 kAlignRightX | kAlignMiddleY);
    }

    SelectObject(device, old_font);
    DeleteObject(font);
}

void paintCanvas(HDC device, const Runner& runner) {
    const Layout& layout = g_ui.layout;
    const View& view = runner.view;
    fillRect(device, layout.canvas, kCanvas);
    const int saved = SaveDC(device);
    IntersectClipRect(device, layout.canvas.left, layout.canvas.top, layout.canvas.right, layout.canvas.bottom);

    const auto pen_for = [&](const simcore_view::Rgb& colour, int thickness) {
        return CreatePen(PS_SOLID, std::max(1, thickness), RGB(colour.r, colour.g, colour.b));
    };
    const auto brush_for = [&](const simcore_view::Rgb& colour) {
        return CreateSolidBrush(RGB(colour.r, colour.g, colour.b));
    };
    for (const simcore_view::SceneLine& line : runner.scene.lines) {
        const int width = std::max(1, static_cast<int>(line.width * view.zoom + 0.5f));
        HPEN pen = pen_for(line.color, width);
        HGDIOBJ old_pen = SelectObject(device, pen);
        MoveToEx(device, static_cast<int>(toCanvasX(layout, view, line.x1)),
                 static_cast<int>(toCanvasY(layout, view, line.y1)), nullptr);
        LineTo(device, static_cast<int>(toCanvasX(layout, view, line.x2)),
               static_cast<int>(toCanvasY(layout, view, line.y2)));
        SelectObject(device, old_pen);
        DeleteObject(pen);
    }

    for (const simcore_view::SceneRect& rect : runner.scene.rects) {
        const int left = static_cast<int>(toCanvasX(layout, view, rect.x));
        const int top = static_cast<int>(toCanvasY(layout, view, rect.y));
        const int right = static_cast<int>(toCanvasX(layout, view, rect.x + rect.w));
        const int bottom = static_cast<int>(toCanvasY(layout, view, rect.y + rect.h));
        if (right - left < 3 || bottom - top < 3) continue;
        HBRUSH brush = brush_for(rect.fill);
        HPEN pen = pen_for(rect.stroke, std::max(1, static_cast<int>(view.zoom)));
        HGDIOBJ old_brush = SelectObject(device, brush);
        HGDIOBJ old_pen = SelectObject(device, pen);
        RoundRect(device, left, top, right, bottom, 8, 8);
        SelectObject(device, old_pen);
        SelectObject(device, old_brush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    /* Selection and hover go *under* the markers, not over them.

       A wire's connection dot is what says "this end is attached"; drawing a
       5 px highlight on top of it leaves the two rounded ends of the dot
       sticking out either side, which reads as an arrowhead and makes the
       endpoint less clear than no marker at all. */
    const auto outline_component = [&](size_t index, COLORREF colour, int thickness) {
        for (const simcore_view::SceneRect& rect : runner.scene.rects) {
            if (!rect.has_component || rect.component_index != index) continue;
            HPEN pen = CreatePen(PS_SOLID, thickness, colour);
            HGDIOBJ old_pen = SelectObject(device, pen);
            HGDIOBJ old_brush = SelectObject(device, GetStockObject(NULL_BRUSH));
            RoundRect(device, static_cast<int>(toCanvasX(layout, view, rect.x)) - 2,
                      static_cast<int>(toCanvasY(layout, view, rect.y)) - 2,
                      static_cast<int>(toCanvasX(layout, view, rect.x + rect.w)) + 2,
                      static_cast<int>(toCanvasY(layout, view, rect.y + rect.h)) + 2, 8, 8);
            SelectObject(device, old_brush);
            SelectObject(device, old_pen);
            DeleteObject(pen);
            return true;
        }
        return false;
    };
    const auto outline_wire = [&](size_t index, COLORREF colour, int thickness) {
        /* A wire is a polyline, so every segment with this index has to be
           drawn - stopping at the first one would highlight half a bend. */
        bool drew = false;
        for (const simcore_view::SceneLine& line : runner.scene.lines) {
            if (!line.has_wire || line.halo || line.wire_index != index) continue;
            HPEN pen = CreatePen(PS_SOLID, thickness, colour);
            HGDIOBJ old_pen = SelectObject(device, pen);
            MoveToEx(device, static_cast<int>(toCanvasX(layout, view, line.x1)),
                     static_cast<int>(toCanvasY(layout, view, line.y1)), nullptr);
            LineTo(device, static_cast<int>(toCanvasX(layout, view, line.x2)),
                   static_cast<int>(toCanvasY(layout, view, line.y2)));
            SelectObject(device, old_pen);
            DeleteObject(pen);
            drew = true;
        }
        return drew;
    };
    if (g_ui.hover_pick.valid && !runner.selected.valid) {
        if (g_ui.hover_pick.on_component) {
            outline_component(g_ui.hover_pick.component, RGB(150, 180, 230), 2);
        } else if (g_ui.hover_pick.on_wire) {
            outline_wire(g_ui.hover_pick.wire, RGB(150, 180, 230), 4);
        }
    }
    if (runner.selected.valid) {
        if (runner.selected.on_component) {
            outline_component(runner.selected.component, kAccent, 3);
        } else if (runner.selected.on_wire) {
            outline_wire(runner.selected.wire, kAccent, 5);
        }
    }

    for (const simcore_view::SceneDot& dot : runner.scene.dots) {
        const int dot_radius = std::max(2, static_cast<int>(dot.r * view.zoom + 0.5f));
        const int centre_x = static_cast<int>(toCanvasX(layout, view, dot.x));
        const int centre_y = static_cast<int>(toCanvasY(layout, view, dot.y));
        HBRUSH brush = brush_for(dot.fill);
        HPEN pen = pen_for(dot.stroke, 1);
        HGDIOBJ old_brush = SelectObject(device, brush);
        HGDIOBJ old_pen = SelectObject(device, pen);
        Ellipse(device, centre_x - dot_radius, centre_y - dot_radius, centre_x + dot_radius, centre_y + dot_radius);
        SelectObject(device, old_pen);
        SelectObject(device, old_brush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    /* Labels keep a fixed screen size instead of scaling: readable at every
       zoom, and they stop being drawn altogether when the boxes are too small
       to hold them. */
    if (view.zoom > 0.45f) {
        HFONT font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
        HGDIOBJ old_font = SelectObject(device, font);
        SetBkMode(device, TRANSPARENT);
        SetTextColor(device, RGB(simcore_view::kLabel.r, simcore_view::kLabel.g, simcore_view::kLabel.b));
        for (const simcore_view::SceneRect& rect : runner.scene.rects) {
            if (rect.label.empty()) continue;
            drawText(device, static_cast<int>(toCanvasX(layout, view, rect.x + rect.w * 0.5f)),
                     static_cast<int>(toCanvasY(layout, view, rect.y + rect.h * 0.5f)), rect.label,
                     kAlignCenterX | kAlignMiddleY);
        }
        SelectObject(device, old_font);
        DeleteObject(font);
    }

    RestoreDC(device, saved);
}

void paintPanel(HDC device, const Runner& runner) {
    const Layout& layout = g_ui.layout;
    fillRect(device, layout.panel, kChrome);
    fillRect(device, RECT{layout.panel.left, layout.panel.top, layout.panel.left + 1, layout.panel.bottom}, kChromeEdge);

    HFONT ui = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HFONT bold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HFONT mono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
    HGDIOBJ old_font = SelectObject(device, bold);
    SetBkMode(device, TRANSPARENT);
    SetTextAlign(device, TA_LEFT | TA_TOP);
    const int left = layout.panel.left + 16;
    int y = layout.panel.top + 14;

    const auto heading = [&](const char* text) {
        SelectObject(device, bold);
        SetTextColor(device, kInk);
        drawText(device, left, y, text);
        y += 20;
        fillRect(device, RECT{left, y, layout.panel.right - 16, y + 1}, kChromeEdge);
        y += 8;
        SelectObject(device, mono);
    };

    heading("选中");
    const std::vector<std::string> selection = selectionLines(runner);
    if (selection.empty()) {
        SelectObject(device, ui);
        SetTextColor(device, kInkSoft);
        drawText(device, left, y, "点一个元件或一根导线，这里会列出它的");
        y += 22;
        drawText(device, left, y, "net、当前值和写回的状态字节。");
        y += 22;
    } else {
        SetTextColor(device, kAccent);
        const size_t shown = std::min(selection.size(), kMaxSelectionLines);
        for (size_t index = 0; index < shown; ++index) {
            drawText(device, left, y, selection[index]);
            y += kLineHeight;
        }
        if (shown < selection.size()) {
            SetTextColor(device, kInkSoft);
            drawText(device, left, y, "… 还有 " + std::to_string(selection.size() - shown) + " 行");
            y += kLineHeight;
        }
        y += 10;
    }

    /* The value row is laid out from its own top edge rather than from a
       baseline: measured from a baseline its swatches reach 11 px upwards, and
       that is exactly enough to land on top of the block above it. */
    const int legend_top = y;
    SelectObject(device, bold);
    SetTextColor(device, kInk);
    drawText(device, left, legend_top + 7, "值");
    SelectObject(device, ui);
    int swatch_x = left + 34;
    const char* legend[] = {"0", "1", "x", "z"};
    const tcsim::Logic legend_values[] = {tcsim::Logic::kZero, tcsim::Logic::kOne, tcsim::Logic::kX,
                                          tcsim::Logic::kZ};
    for (int index = 0; index < 4; ++index) {
        const simcore_view::Rgb colour = simcore_view::valueColor(legend_values[index]);
        fillRect(device, RECT{swatch_x, legend_top + 2, swatch_x + 16, legend_top + 18},
                 RGB(colour.r, colour.g, colour.b));
        SetTextColor(device, kInk);
        drawText(device, swatch_x + 21, legend_top + 7, legend[index]);
        swatch_x += 52;
    }
    y = legend_top + 34;

    heading("备注");
    SelectObject(device, mono);
    if (runner.scene.notes.empty()) {
        SetTextColor(device, kGoodInk);
        drawText(device, left, y, runner.fatal ? "（没有备注）" : "构建器没有任何异议");
        y += 17;
    } else {
        const size_t shown = visibleNotes(runner);
        for (size_t index = 0; index < shown; ++index) {
            SetTextColor(device, kNoteInk);
            drawText(device, left, y, runner.scene.notes[index]);
            y += kLineHeight;
        }
        if (shown < runner.scene.notes.size()) {
            SetTextColor(device, kInkSoft);
            drawText(device, left, y, "… 还有 " + std::to_string(runner.scene.notes.size() - shown) + " 条");
        }
    }

    SelectObject(device, ui);
    SetTextColor(device, kInkSoft);
    drawText(device, left, layout.panel.bottom - 56, "滚轮缩放 · 按住拖动平移 · 空格换输入");
    drawText(device, left, layout.panel.bottom - 34, "O 打开板子 · S 存 SVG · B 存 BMP · Esc 退出");

    SelectObject(device, old_font);
    DeleteObject(mono);
    DeleteObject(bold);
    DeleteObject(ui);
}

void paintStatus(HDC device, const Runner& runner) {
    const Layout& layout = g_ui.layout;
    fillRect(device, layout.status, RGB(242, 242, 246));
    fillRect(device, RECT{layout.status.left, layout.status.top, layout.status.right, layout.status.top + 1},
             kChromeEdge);

    HFONT font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HGDIOBJ old_font = SelectObject(device, font);
    SetBkMode(device, TRANSPARENT);
    const int middle = layout.status.top + kStatusHeight / 2;
    int x = 14;
    const auto cell = [&](const std::string& text, COLORREF colour) {
        SetTextColor(device, colour);
        drawText(device, x, middle, text, kAlignMiddleY);
        x += textWidth(device, text) + 18;
    };
    cell("缩放 " + std::to_string(static_cast<int>(runner.view.zoom * 100.0f + 0.5f)) + "%", kInk);
    cell(runner.tidy ? "排布 整理（非板面真实位置）" : "排布 原始", runner.tidy ? kActiveInk : kInkSoft);
    if (g_ui.cursor_on_canvas) {
        cell("光标 " + std::to_string(static_cast<int>(std::floor(g_ui.cursor_x))) + "," +
                 std::to_string(static_cast<int>(std::floor(g_ui.cursor_y))),
             kInkSoft);
        if (g_ui.hover_pick.valid) {
            if (g_ui.hover_pick.on_component) {
                cell("元件 " + std::to_string(g_ui.hover_pick.component), kInkSoft);
            } else {
                cell("导线 " + std::to_string(g_ui.hover_pick.wire), kInkSoft);
            }
        }
    }
    SetTextColor(device, runner.fatal ? kNoteInk : kGoodInk);
    drawText(device, layout.status.right - 14, middle, runner.status, kAlignRightX | kAlignMiddleY);
    SelectObject(device, old_font);
    DeleteObject(font);
}

void paintAll(HDC device, const Runner& runner) {
    paintCanvas(device, runner);
    paintPanel(device, runner);
    paintToolbar(device, runner);
    paintStatus(device, runner);
}

/* ------------------------------------------------------------------ actions */

void loadBoard(Runner& runner, const std::string& path) {
    runner.board_path = path;
    runner.file = simcore_view::readBoardFile(path);
    runner.selected = simcore_view::ScenePick{};
    g_ui.hover_pick = simcore_view::ScenePick{};
    runner.pattern = 0;
    rebuild(runner);
    g_svg_path = path + ".svg";
    g_bmp_path = path + ".bmp";
    std::printf("board %s: %zu components, %zu wires\n", path.c_str(), runner.file.board.components.size(),
                runner.file.board.wires.size());
    std::printf("%s\n", runner.status.c_str());
    for (const std::string& note : runner.scene.notes) std::printf("  note: %s\n", note.c_str());
}

bool action(HDC device, HWND window, int action_id) {
    Runner& runner = *g_runner;
    switch (action_id) {
        case kActionOpen: {
            const std::string picked = chooseBoardFile(defaultFixtureDirectory());
            if (picked.empty()) return false;
            loadBoard(runner, picked);
            fitView(runner, g_ui.layout);
            SetWindowTextW(window, widen("simcore-view  -  " + picked).c_str());
            return true;
        }
        case kActionReload:
            loadBoard(runner, runner.board_path);
            return true;
        case kActionPrevInput:
            if (runner.pattern > 0) --runner.pattern;
            rebuild(runner);
            return true;
        case kActionNextInput:
            ++runner.pattern;
            rebuild(runner);
            return true;
        case kActionResetInput:
            runner.pattern = 0;
            rebuild(runner);
            return true;
        case kActionZoomOut:
            zoomAt(runner, g_ui.layout, static_cast<float>(g_ui.layout.canvas.right + g_ui.layout.canvas.left) * 0.5f,
                   static_cast<float>(g_ui.layout.canvas.bottom + g_ui.layout.canvas.top) * 0.5f, 0.8f);
            return true;
        case kActionZoomIn:
            zoomAt(runner, g_ui.layout, static_cast<float>(g_ui.layout.canvas.right + g_ui.layout.canvas.left) * 0.5f,
                   static_cast<float>(g_ui.layout.canvas.bottom + g_ui.layout.canvas.top) * 0.5f, 1.25f);
            return true;
        case kActionFit:
            fitView(runner, g_ui.layout);
            return true;
        case kActionTidy:
            runner.tidy = !runner.tidy;
            rebuild(runner);
            return true;
        case kActionSaveSvg: {
            const bool written = simcore_view::writeSvg(g_svg_path, runner.scene, static_cast<float>(panelWidthFor(runner)));
            std::printf("%s %s\n", written ? "wrote" : "could not write", g_svg_path.c_str());
            return false;
        }
        case kActionSaveBmp: {
            BITMAPINFO info{};
            const int width = g_ui.layout.canvas.right - g_ui.layout.canvas.left;
            const int height = g_ui.layout.status.bottom;
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            HDC screen = GetDC(nullptr);
            HDC memory = CreateCompatibleDC(screen);
            HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            ReleaseDC(nullptr, screen);
            bool ok = false;
            if (bitmap && pixels) {
                HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
                paintAll(memory, runner);
                GdiFlush();
                SelectObject(memory, old_bitmap);
                const uint32_t image_size = static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * 4u;
                BITMAPFILEHEADER header{};
                header.bfType = 0x4D42;
                header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
                header.bfSize = header.bfOffBits + image_size;
                std::FILE* file = std::fopen(g_bmp_path.c_str(), "wb");
                if (file) {
                    ok = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
                         std::fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, file) == 1 &&
                         std::fwrite(pixels, image_size, 1, file) == 1;
                    std::fclose(file);
                }
            }
            if (bitmap) DeleteObject(bitmap);
            DeleteDC(memory);
            std::printf("%s %s\n", ok ? "wrote" : "could not write", g_bmp_path.c_str());
            return false;
        }
        default:
            break;
    }
    (void)device;
    return false;
}

/* ------------------------------------------------------------- window proc */

void relayout(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    g_ui.layout = layoutFor(*g_runner, client.right, client.bottom);
    /* Layout is what the toolbar's button rectangles are measured from, so it
       is rebuilt whenever the window changes shape. */
    const int saved_hover = g_ui.hover_button;
    HDC device = GetDC(window);
    buildToolbar(device, *g_runner);
    ReleaseDC(window, device);
    g_ui.hover_button = saved_hover;
}

int buttonAt(POINT point) {
    for (size_t index = 0; index < g_ui.buttons.size(); ++index) {
        const RECT& rect = g_ui.buttons[index].rect;
        if (point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool inCanvas(POINT point) {
    const RECT& canvas = g_ui.layout.canvas;
    return point.x >= canvas.left && point.x < canvas.right && point.y >= canvas.top && point.y < canvas.bottom;
}

void paintWindow(HWND window, HDC target) {
    RECT client{};
    GetClientRect(window, &client);
    /* Double buffered: without it every pan frame flickers, and a picture whose
       whole purpose is to be compared with another one has to hold still. */
    HDC memory = CreateCompatibleDC(target);
    HBITMAP buffer = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ old_bitmap = SelectObject(memory, buffer);
    paintAll(memory, *g_runner);
    BitBlt(target, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_bitmap);
    DeleteObject(buffer);
    DeleteDC(memory);
}

/* --watch: what the board file looked like when it was last read, so a change
   can be spotted with one stat call per timer tick. */
bool g_watch = false;
std::filesystem::file_time_type g_watch_time{};
std::uintmax_t g_watch_size = 0;

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT state{};
            HDC device = BeginPaint(window, &state);
            paintWindow(window, device);
            EndPaint(window, &state);
            return 0;
        }
        case WM_SIZE:
            relayout(window);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = 720;
            info->ptMinTrackSize.y = 420;
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                PostQuitMessage(0);
                return 0;
            }
            if (wparam == VK_SPACE) {
                HDC device = GetDC(window);
                action(device, window, kActionNextInput);
                ReleaseDC(window, device);
                InvalidateRect(window, nullptr, TRUE);
                return 0;
            }
            {
                HDC device = GetDC(window);
                int id = 0;
                if (wparam == 'R') id = kActionResetInput;
                if (wparam == 'O') id = kActionOpen;
                if (wparam == 'S') id = kActionSaveSvg;
                if (wparam == 'B') id = kActionSaveBmp;
                if (wparam == 'F') id = kActionFit;
                if (wparam == 'L') id = kActionTidy;
                if (wparam == VK_OEM_PLUS || wparam == VK_ADD) id = kActionZoomIn;
                if (wparam == VK_OEM_MINUS || wparam == VK_SUBTRACT) id = kActionZoomOut;
                const bool redraw = id ? action(device, window, id) : false;
                ReleaseDC(window, device);
                if (id) InvalidateRect(window, nullptr, redraw ? TRUE : FALSE);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            ScreenToClient(window, &point);
            if (inCanvas(point)) {
                const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
                zoomAt(*g_runner, g_ui.layout, static_cast<float>(point.x), static_cast<float>(point.y),
                       delta > 0 ? 1.15f : 1.0f / 1.15f);
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            if (g_ui.panning) {
                const int dx = point.x - g_ui.pan_from.x;
                const int dy = point.y - g_ui.pan_from.y;
                if (std::abs(dx) > 3 || std::abs(dy) > 3) g_ui.pan_moved = true;
                Runner& runner = *g_runner;
                runner.view.offset_x = g_ui.pan_start.offset_x - static_cast<float>(dx) / runner.view.zoom;
                runner.view.offset_y = g_ui.pan_start.offset_y - static_cast<float>(dy) / runner.view.zoom;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            const int hover = buttonAt(point);
            const bool on_canvas = inCanvas(point);
            simcore_view::ScenePick pick;
            if (on_canvas) {
                g_ui.cursor_x = toWorldX(g_ui.layout, g_runner->view, static_cast<float>(point.x));
                g_ui.cursor_y = toWorldY(g_ui.layout, g_runner->view, static_cast<float>(point.y));
                pick = simcore_view::pickScene(g_runner->scene, g_ui.cursor_x, g_ui.cursor_y);
            }
            const bool changed = hover != g_ui.hover_button || on_canvas != g_ui.cursor_on_canvas ||
                                 pick.valid != g_ui.hover_pick.valid ||
                                 (pick.valid && (pick.component != g_ui.hover_pick.component ||
                                                 pick.wire != g_ui.hover_pick.wire));
            g_ui.hover_button = hover;
            g_ui.cursor_on_canvas = on_canvas;
            g_ui.hover_pick = pick;
            if (changed) InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            SetCapture(window);
            if (inCanvas(point)) {
                /* Press and release without moving is a click; press and move is
                   a pan.  Deciding on release is what makes both gestures live
                   on the same button without a mode. */
                g_ui.panning = true;
                g_ui.pan_moved = false;
                g_ui.pan_from = point;
                g_ui.pan_start = g_runner->view;
                return 0;
            }
            g_ui.pressed_button = buttonAt(point);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            ReleaseCapture();
            if (g_ui.panning) {
                g_ui.panning = false;
                if (!g_ui.pan_moved) {
                    const float world_x = toWorldX(g_ui.layout, g_runner->view, static_cast<float>(point.x));
                    const float world_y = toWorldY(g_ui.layout, g_runner->view, static_cast<float>(point.y));
                    g_runner->selected = simcore_view::pickScene(g_runner->scene, world_x, world_y);
                }
                InvalidateRect(window, nullptr, TRUE);
                return 0;
            }
            const int released = buttonAt(point);
            const int pressed = g_ui.pressed_button;
            g_ui.pressed_button = -1;
            if (pressed >= 0 && pressed == released) {
                HDC device = GetDC(window);
                const int id = g_ui.buttons[static_cast<size_t>(pressed)].action;
                const bool redraw = action(device, window, id);
                ReleaseDC(window, device);
                InvalidateRect(window, nullptr, redraw ? TRUE : FALSE);
                return 0;
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && g_ui.panning) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            break;
        case WM_TIMER: {
            if (!g_watch || !g_runner) return 0;
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(g_runner->board_path, error);
            const auto when = std::filesystem::last_write_time(g_runner->board_path, error);
            if (error) return 0;
            if (size == g_watch_size && when == g_watch_time) return 0;
            g_watch_size = size;
            g_watch_time = when;
            const simcore_view::ScenePick keep = g_runner->selected;
            loadBoard(*g_runner, g_runner->board_path);
            g_runner->selected = keep;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

/* --------------------------------------------------------------- headless */

/* A frame that carries the whole window, not just the drawing: the toolbar and
   the panel are half of what a person is looking at when they say "this looks
   wrong".  The canvas is sized to the scene so the picture is 1:1 and does not
   depend on whatever size the window happened to be. */
bool writeFrameBmp(const std::string& path, Runner& runner) {
    const int scene_w = static_cast<int>(std::ceil(runner.scene.width));
    const int scene_h = static_cast<int>(std::ceil(runner.scene.height));
    const int panel_w = panelWidthFor(runner);
    const int content_h = std::max(scene_h, panelContentHeight(runner));
    const int width = std::max(scene_w + panel_w, toolbarNaturalWidth());
    const int canvas_w = width - panel_w;
    const int height = content_h + kToolbarHeight + kStatusHeight;

    g_ui.layout = Layout{RECT{0, 0, width, kToolbarHeight},
                         RECT{0, kToolbarHeight, canvas_w, kToolbarHeight + content_h},
                         RECT{canvas_w, kToolbarHeight, width, kToolbarHeight + content_h},
                         RECT{0, height - kStatusHeight, width, height}};
    const View saved_view = runner.view;
    /* 1:1 and centred: a frame is evidence, so it must not depend on whatever
       zoom and pan the window happened to be at when the button was pressed. */
    runner.view = View{1.0f, (static_cast<float>(scene_w) - static_cast<float>(canvas_w)) * 0.5f,
                       (static_cast<float>(scene_h) - static_cast<float>(content_h)) * 0.5f};

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height; /* bottom-up, so the DIB is already file order */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, screen);
    bool ok = false;
    if (bitmap && pixels) {
        HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
        HDC toolbar_dc = GetDC(nullptr);
        buildToolbar(toolbar_dc, runner);
        ReleaseDC(nullptr, toolbar_dc);
        paintAll(memory, runner);
        GdiFlush();
        SelectObject(memory, old_bitmap);

        const uint32_t image_size = static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * 4u;
        BITMAPFILEHEADER header{};
        header.bfType = 0x4D42; /* "BM" */
        header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + image_size;
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (file) {
            ok = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
                 std::fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, file) == 1 &&
                 std::fwrite(pixels, image_size, 1, file) == 1;
            std::fclose(file);
        }
    }
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(memory);
    runner.view = saved_view;
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    Options options = parseArguments(argc, argv);
    if (options.board.empty()) {
        /* Started with no arguments - by a double-click, say.  Ask for a board
           rather than printing usage and closing again. */
        options.board = chooseBoardFile(defaultFixtureDirectory());
        if (options.board.empty()) {
            std::fprintf(stderr,
                         "usage: simcore-view [--board FILE] [--svg OUT] [--bmp OUT] [--ticks N] [--size WxH]\n"
                         "                     [--select-component N] [--select-wire N] [--no-window]\n");
            return 2;
        }
    }
    const std::string resolved = resolveBoardPath(options.board);
    if (resolved.empty()) {
        std::fprintf(stderr, "cannot find a board at %s\n", options.board.c_str());
        return 1;
    }
    options.board = resolved;

    Runner runner;
    runner.ticks = options.ticks;
    runner.tidy = options.tidy;
    g_runner = &runner;
    loadBoard(runner, options.board);

    /* Selecting from the command line lets a frame be kept as evidence: the
       BMP then carries the same net/slot block a click would have shown. */
    if (options.select_component >= 0) {
        runner.selected.valid = true;
        runner.selected.on_component = true;
        runner.selected.component = static_cast<size_t>(options.select_component);
    } else if (options.select_wire >= 0) {
        runner.selected.valid = true;
        runner.selected.on_wire = true;
        runner.selected.wire = static_cast<size_t>(options.select_wire);
    }

    if (!options.svg.empty()) {
        if (!simcore_view::writeSvg(options.svg, runner.scene, static_cast<float>(panelWidthFor(runner)))) {
            std::fprintf(stderr, "could not write %s\n", options.svg.c_str());
            return 1;
        }
        std::printf("wrote %s\n", options.svg.c_str());
    }
    if (!options.bmp.empty()) {
        if (!writeFrameBmp(options.bmp, runner)) {
            std::fprintf(stderr, "could not write %s\n", options.bmp.c_str());
            return 1;
        }
        std::printf("wrote %s\n", options.bmp.c_str());
    }
    if (!options.window) return 0;

    g_svg_path = options.svg.empty() ? options.board + ".svg" : options.svg;
    g_bmp_path = options.bmp.empty() ? options.board + ".bmp" : options.bmp;

    HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSW window_class{};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = windowProc;
    window_class.hInstance = instance;
    /* A cursor name is a resource ordinal, not text, so the A form is right
       here even though every string in this program goes through W. */
    window_class.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = L"simcore-view";
    RegisterClassW(&window_class);

    const int scene_w = std::max(420, static_cast<int>(runner.scene.width));
    const int scene_h = std::max(260, static_cast<int>(runner.scene.height));
    const int content_h = std::max(scene_h, panelContentHeight(runner));
    const int width =
        options.width > 0 ? options.width : std::max(scene_w + panelWidthFor(runner), toolbarNaturalWidth());
    const int height = options.height > 0 ? options.height : content_h + kToolbarHeight + kStatusHeight;
    RECT frame{0, 0, width, height};
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);

    HWND window = CreateWindowExW(0, L"simcore-view", L"simcore-view", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr,
                                  instance, nullptr);
    if (!window) {
        std::fprintf(stderr, "could not create a window\n");
        return 1;
    }
    SetWindowTextW(window, widen("simcore-view  -  " + runner.board_path).c_str());
    relayout(window);
    fitView(runner, g_ui.layout);
    ShowWindow(window, SW_SHOW);

    if (options.watch) {
        g_watch = true;
        std::error_code error;
        g_watch_size = std::filesystem::file_size(runner.board_path, error);
        g_watch_time = std::filesystem::last_write_time(runner.board_path, error);
        SetTimer(window, 1, 200, nullptr);
        SetWindowTextW(window,
                       widen("simcore-view  -  " + runner.board_path + "  (watch)").c_str());
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
