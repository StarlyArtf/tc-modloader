#pragma once
/* The platform-independent half of the viewer: a board, a netlist build and
   (optionally) a set of net values become a flat list of rectangles, lines,
   dots and labels.

   Keeping this free of any windowing API is deliberate.  The list is plain
   data, so it can be asserted in an offline test, written as SVG, or handed to
   Win32 GDI - and the whole point of this tool is that its picture comes from
   a different renderer than the game's, so the less it shares the better.

   Geometry is schematic, not a replica: cells map to pixels linearly, a wire
   is the straight segment its two endpoints describe, and no camera, zoom or
   artwork is involved.  That is the feature, not a shortcut. */

#include "kind_pins.hpp"

#include "../include/tcsim/board.hpp"
#include "../include/tcsim/netlist.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace simcore_view {

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

inline const Rgb kBodyFill{238, 238, 244};
inline const Rgb kBodyStroke{86, 86, 100};
inline const Rgb kUnknownKindFill{248, 242, 220};
inline const Rgb kUnknownKindStroke{198, 138, 20};
inline const Rgb kNoGeometryFill{252, 228, 228};
inline const Rgb kNoGeometryStroke{200, 45, 45};
inline const Rgb kPinFill{52, 52, 62};
inline const Rgb kWireDefault{140, 140, 152};
inline const Rgb kLabel{20, 20, 28};
inline const Rgb kNote{120, 30, 30};

/* The canvas colour, needed here because a wire is drawn with a casing in it.
   The caller fills the canvas with this exact colour, so the casing is
   invisible except where it lies over another wire. */
inline const Rgb kCanvasFill{253, 253, 255};

/* Logic -> colour, the convention every waveform tool uses: 0 and 1 are dull
   and bright, an unknown is alarming, an undriven net is grey. */
inline Rgb valueColor(tcsim::Logic value) {
    switch (value) {
        case tcsim::Logic::kZero: return Rgb{72, 112, 200};
        case tcsim::Logic::kOne: return Rgb{30, 158, 62};
        case tcsim::Logic::kX: return Rgb{205, 45, 45};
        case tcsim::Logic::kZ: return Rgb{150, 150, 155};
    }
    return Rgb{150, 150, 155};
}

struct SceneRect {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
    Rgb fill;
    Rgb stroke;
    std::string label;
    std::string sub_label;
    size_t component_index = 0;
    bool has_component = false;
};

struct SceneLine {
    float x1 = 0;
    float y1 = 0;
    float x2 = 0;
    float y2 = 0;
    Rgb color;
    float width = 2.0f;
    /* Which entry of the board's wire list this segment came from, so a click
       on it can be traced back to a net and therefore to the state bytes the
       value is written back to. */
    size_t wire_index = 0;
    bool has_wire = false;
    /* A casing: the same geometry as the wire it belongs to, drawn wider and in
       the canvas colour just before it.  It is not part of the drawing anyone
       picks or counts. */
    bool halo = false;
};

struct SceneDot {
    float x = 0;
    float y = 0;
    float r = 4.0f;
    Rgb fill;
    Rgb stroke;
    /* A wire landing on a pin, rather than the pin itself.  The two are drawn
       in different colours and sizes, and a reader (or a test) needs to tell
       "attached" from "there is a pin here". */
    bool connection = false;
};

struct SceneText {
    float x = 0;
    float y = 0;
    std::string text;
    Rgb color;
    bool centred = true;
    float size = 11.0f;
};

struct Scene {
    float width = 0;
    float height = 0;
    std::vector<SceneRect> rects;
    std::vector<SceneLine> lines;
    std::vector<SceneDot> dots;
    std::vector<SceneText> texts;
    std::vector<std::string> notes;
};

struct SceneOptions {
    float cell = 30.0f;
    float margin = 56.0f;
    /* Redraw the components on a dataflow grid instead of where the board puts
       them.  Off by default: the whole point of this view is that it shows the
       same board the game does, so a tidier picture that is not the board has
       to be something the reader asked for. */
    bool tidy_layout = false;
};

/* What the pointer is over.  A component wins over a wire when both match, and
   a later component wins over an earlier one because later bodies are drawn on
   top of earlier ones. */
struct ScenePick {
    bool valid = false;
    size_t component = 0;
    size_t wire = 0;
    bool on_component = false;
    bool on_wire = false;
};

struct SceneInput {
    const tcsim::BoardView* board = nullptr;
    /* Optional: when present, wires and pins are coloured by net value and the
       build's notes are carried into the legend. */
    const tcsim::BuildResult* built = nullptr;
    const tcsim::NetList* values = nullptr;
    /* Optional: the game's own route per wire (parallel to board->wires).  A
       wire that has one is drawn exactly like that, because the point of this
       window is to show the board, not this window's idea of a tidy schematic. */
    const std::vector<std::vector<std::pair<int, int>>>* wire_paths = nullptr;
    /* Optional: pins the record carried for a component, indexed like
       board->components.  A Mod type's pins are whatever its Mod declared, so
       they travel with the record instead of being guessed here. */
    const std::vector<std::vector<std::pair<int, int>>>* component_in_pins = nullptr;
    const std::vector<std::vector<std::pair<int, int>>>* component_out_pins = nullptr;
    bool show_pins = true;
    bool show_labels = true;
};

namespace scene_detail {

/* One component's pins: the record's own when it carried them (a Mod type the
   frozen table cannot know), else the frozen table.  Shared by the drawing and
   by the netlist the viewer builds, so the two cannot disagree about where a
   Mod component's pins are. */
inline bool pinsForComponent(
    const tcsim::BoardView& board, const tcsim::BoardComponent& component,
    const std::vector<std::vector<std::pair<int, int>>>* in_pins_by_component,
    const std::vector<std::vector<std::pair<int, int>>>* out_pins_by_component,
    tcsim::KindPins& out) {
    if (component.id && in_pins_by_component && out_pins_by_component) {
        const size_t count = std::min(board.components.size(), in_pins_by_component->size());
        for (size_t index = 0; index < count; ++index) {
            if (board.components[index].id != component.id) continue;
            const std::vector<std::pair<int, int>>& in_pins = (*in_pins_by_component)[index];
            const std::vector<std::pair<int, int>>& out_pins =
                index < out_pins_by_component->size() ? (*out_pins_by_component)[index]
                                                      : std::vector<std::pair<int, int>>{};
            if (in_pins.empty() && out_pins.empty()) break;
            out = tcsim::KindPins{};
            out.kind = component.kind;
            out.name = "Mod";
            out.input_count = static_cast<uint8_t>(std::min<size_t>(in_pins.size(), 8));
            for (uint8_t pin = 0; pin < out.input_count; ++pin) {
                out.in_x[pin] = static_cast<int8_t>(in_pins[pin].first);
                out.in_y[pin] = static_cast<int8_t>(in_pins[pin].second);
                out.in_bits[pin] = 1;
            }
            out.output_count = static_cast<uint8_t>(std::min<size_t>(out_pins.size(), 4));
            for (uint8_t pin = 0; pin < out.output_count; ++pin) {
                out.out_x[pin] = static_cast<int8_t>(out_pins[pin].first);
                out.out_y[pin] = static_cast<int8_t>(out_pins[pin].second);
                out.out_bits[pin] = 1;
            }
            return true;
        }
    }
    return simcore_view::pinsForKind(component, out);
}

inline std::string hex16(uint64_t value) {
    char text[24] = {};
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

/* The label a component gets when the frozen geometry table has no name for
   its kind.  It says the kind number rather than guessing a name, so a
   component the viewer cannot identify is obvious on screen. */
inline std::string fallbackName(const tcsim::BoardComponent& component) {
    if (component.kind == 0x4e) return "custom " + hex16(component.custom_id);
    char text[24] = {};
    std::snprintf(text, sizeof(text), "kind 0x%02X", component.kind);
    return text;
}

/* The body is derived from the pins rather than from a fixed box around the
   component's own cell.

   The prototype table puts an input at x = -1 and an output at x = +2, so a
   body of a constant two cells centred on the cell touches the inputs and
   leaves the output stub hanging a whole cell off the right edge - which is
   exactly what a picture of it shows.  A pin is the end of a stub, so the body
   stops half a cell short of every pin and the stubs stick out at both ends. */
struct BodyBox {
    float left = 0;
    float right = 0;
    float top = 0;
    float bottom = 0;
};

/* The body in cell units, relative to the component's own cell.  Kept separate
   from the pixel form because the wire router reasons in cells. */
inline BodyBox bodyCellsFor(const tcsim::KindPins& pins, uint8_t rotation) {
    bool any = false;
    float low_x = 0;
    float high_x = 0;
    float low_y = 0;
    float high_y = 0;
    const auto include = [&](int x, int y) {
        const auto offset = tcsim::rotateOffset(x, y, rotation);
        const float px = static_cast<float>(offset.first);
        const float py = static_cast<float>(offset.second);
        if (!any) {
            low_x = high_x = px;
            low_y = high_y = py;
            any = true;
            return;
        }
        low_x = std::min(low_x, px);
        high_x = std::max(high_x, px);
        low_y = std::min(low_y, py);
        high_y = std::max(high_y, py);
    };
    for (uint8_t index = 0; index < pins.input_count && index < 8; ++index) {
        include(pins.in_x[index], pins.in_y[index]);
    }
    for (uint8_t index = 0; index < pins.output_count && index < 4; ++index) {
        include(pins.out_x[index], pins.out_y[index]);
    }
    if (!any) {
        low_x = -1.0f;
        high_x = 1.0f;
        low_y = -0.6f;
        high_y = 0.6f;
    }

    constexpr float kStub = 0.5f;
    float left = low_x + kStub;
    float right = high_x - kStub;
    float top = low_y;
    float bottom = high_y;
    /* A single pin carries no extent in one direction (a NOT gate's two pins
       share y = 0), so a floor keeps a degenerate box from collapsing. */
    if (right - left < 0.9f) {
        const float middle = (left + right) * 0.5f;
        left = middle - 0.45f;
        right = middle + 0.45f;
    }
    if (bottom - top < 1.2f) {
        const float middle = (top + bottom) * 0.5f;
        top = middle - 0.6f;
        bottom = middle + 0.6f;
    }
    BodyBox box;
    box.left = left;
    box.right = right;
    box.top = top;
    box.bottom = bottom;
    return box;
}

inline BodyBox bodyFor(const tcsim::KindPins& pins, uint8_t rotation, float centre_x, float centre_y,
                       float cell) {
    const BodyBox cells = bodyCellsFor(pins, rotation);
    BodyBox box;
    box.left = centre_x + cells.left * cell;
    box.right = centre_x + cells.right * cell;
    box.top = centre_y + cells.top * cell;
    box.bottom = centre_y + cells.bottom * cell;
    return box;
}

/* The direction a pin's stub points, read off the body: the pin sits on one
   side of it.  An input like (-1,-1) is on the left; a Full Adder's (0,-2) is
   on top.  A wire that leaves along its own stub and arrives along the other
   one reads like a schematic instead of cutting across the part. */
inline std::pair<int, int> pinNormal(int pin_x, int pin_y, const BodyBox& body) {
    const float centre_x = (body.left + body.right) * 0.5f;
    const float centre_y = (body.top + body.bottom) * 0.5f;
    const float dx = static_cast<float>(pin_x) - centre_x;
    const float dy = static_cast<float>(pin_y) - centre_y;
    if (std::fabs(dx) >= std::fabs(dy)) return {dx >= 0.0f ? 1 : -1, 0};
    return {0, dy >= 0.0f ? 1 : -1};
}

/* Does an axis-aligned segment pass through a body?  Used to pick between the
   two possible elbows: with no obstacle avoidance at all, a cross-coupled gate
   pair draws its feedback wire straight through the part next door. */
inline bool segmentHitsBody(int x1, int y1, int x2, int y2, const BodyBox& body) {
    if (y1 == y2) {
        if (static_cast<float>(y1) < body.top || static_cast<float>(y1) > body.bottom) return false;
        const int low = std::min(x1, x2);
        const int high = std::max(x1, x2);
        return static_cast<float>(high) >= body.left && static_cast<float>(low) <= body.right;
    }
    if (x1 == x2) {
        if (static_cast<float>(x1) < body.left || static_cast<float>(x1) > body.right) return false;
        const int low = std::min(y1, y2);
        const int high = std::max(y1, y2);
        return static_cast<float>(high) >= body.top && static_cast<float>(low) <= body.bottom;
    }
    return false;
}

/* Which way a leg goes, as a unit vector.  Every leg this router draws is axis
   aligned; a diagonal would mean a bug, and it reports (0,0) rather than
   guessing. */
inline std::pair<int, int> legDirection(const std::pair<int, int>& from, const std::pair<int, int>& to) {
    const int dx = to.first - from.first;
    const int dy = to.second - from.second;
    if (dx == 0 && dy == 0) return {0, 0};
    if (dx == 0) return {0, dy > 0 ? 1 : -1};
    if (dy == 0) return {dx > 0 ? 1 : -1, 0};
    return {0, 0};
}

/* How many times a path doubles back on itself.

   This is the number that matters for readability, and it is the one the first
   router could not see: leaving an output along its own stub and then running
   the other way draws a wire that looks cut in half.  Counting reversals and
   pricing them far above length lets a longer way round win a tie it should. */
inline int reversalsIn(const std::vector<std::pair<int, int>>& points) {
    int reversals = 0;
    std::pair<int, int> previous{0, 0};
    for (size_t index = 1; index < points.size(); ++index) {
        std::pair<int, int> direction = legDirection(points[index - 1], points[index]);
        if (direction.first == 0 && direction.second == 0) continue;
        if ((previous.first != 0 || previous.second != 0) && direction.first == -previous.first &&
            direction.second == -previous.second) {
            ++reversals;
        }
        previous = direction;
    }
    return reversals;
}

/* Drop repeated points and points that sit in the middle of a straight run, so
   a path is described by its actual corners. */
inline std::vector<std::pair<int, int>> simplifyPath(std::vector<std::pair<int, int>> points) {
    std::vector<std::pair<int, int>> compact;
    for (const std::pair<int, int>& point : points) {
        if (!compact.empty() && compact.back() == point) continue;
        compact.push_back(point);
    }
    std::vector<std::pair<int, int>> result;
    for (size_t index = 0; index < compact.size(); ++index) {
        if (index > 0 && index + 1 < compact.size()) {
            const std::pair<int, int> before = legDirection(compact[index - 1], compact[index]);
            const std::pair<int, int> after = legDirection(compact[index], compact[index + 1]);
            if (before == after && (before.first != 0 || before.second != 0)) continue;
        }
        result.push_back(compact[index]);
    }
    return result;
}

/* A tidy alternative to the board's own coordinates.

   Devices are ranked by breadth-first distance from the ones nothing drives
   (level inputs, constants, switches) and placed in columns by rank, sorted
   inside a column by the row the player put them on.  Breadth-first rather
   than longest-path on purpose: it terminates on the loops this simulator
   accepts, and a loop with no source at all simply lands in column zero
   instead of inflating every rank below it.

   This is a reading aid, not a model of the board - the default view keeps the
   real coordinates, because comparing this picture with the game's is the
   reason the tool exists. */
inline std::vector<std::pair<int, int>> tidyPositions(const tcsim::BoardView& board,
                                                      const tcsim::BuildResult& built) {
    std::vector<std::pair<int, int>> slots(board.components.size(), std::make_pair(0, 0));
    const tcsim::NetList& list = built.netlist;
    const size_t devices = list.deviceCount();
    if (devices == 0) return {};

    std::vector<int> level(devices, -1);
    std::vector<tcsim::DeviceId> queue;
    /* A net the game drives from outside - a level input, a switch, a clock -
       has no driver device in the netlist.  Treating it as undriven makes every
       component downstream of it look like a root, and the whole board lands in
       one column. */
    const auto listed = [](const std::vector<tcsim::NetId>& nets, tcsim::NetId net) {
        return std::find(nets.begin(), nets.end(), net) != nets.end();
    };
    const auto net_is_driven = [&](tcsim::NetId net) {
        if (net == tcsim::kNoNet || net >= list.netCount()) return false;
        if (!list.net(net).drivers.empty()) return true;
        return listed(built.source_nets, net) || listed(built.clock_nets, net);
    };
    for (tcsim::DeviceId id = 0; id < devices; ++id) {
        bool driven = false;
        for (tcsim::NetId net : list.device(id).inputs) {
            if (net_is_driven(net)) {
                driven = true;
                break;
            }
        }
        if (!driven) {
            level[id] = 0;
            queue.push_back(id);
        }
    }
    const auto seed_from = [&](const std::vector<tcsim::NetId>& nets) {
        for (tcsim::NetId net : nets) {
            if (net == tcsim::kNoNet || net >= list.netCount()) continue;
            for (const tcsim::LoadSlot& load : list.net(net).loads) {
                if (load.device == tcsim::kNoDevice || load.device >= devices) continue;
                if (level[load.device] >= 0) continue;
                level[load.device] = 1;
                queue.push_back(load.device);
            }
        }
    };
    seed_from(built.source_nets);
    seed_from(built.clock_nets);
    for (size_t head = 0; head < queue.size(); ++head) {
        const tcsim::DeviceId id = queue[head];
        for (tcsim::NetId net : list.device(id).outputs) {
            if (net == tcsim::kNoNet || net >= list.netCount()) continue;
            for (const tcsim::LoadSlot& load : list.net(net).loads) {
                if (load.device == tcsim::kNoDevice || load.device >= devices) continue;
                if (level[load.device] >= 0) continue;
                level[load.device] = level[id] + 1;
                queue.push_back(load.device);
            }
        }
    }

    std::map<int, std::vector<size_t>> columns;
    std::vector<size_t> source_column;
    std::vector<size_t> undeviced;
    int deepest = 0;
    for (size_t index = 0; index < board.components.size(); ++index) {
        const tcsim::BoardComponent& component = board.components[index];
        if (component.kind == 0x00) continue;
        const tcsim::DeviceId device = index < built.devices_by_component.size()
                                           ? built.devices_by_component[index]
                                           : tcsim::kNoDevice;
        if (device == tcsim::kNoDevice || device >= devices || level[device] < 0) {
            /* A level input has no device either, but it is the start of the
               flow rather than a stray: it goes in its own column to the left
               of everything the devices rank in. */
            if (tcsim::kindIsSource(component.kind)) {
                source_column.push_back(index);
                continue;
            }
            /* Nothing models this one, so it has no rank to sit in. */
            undeviced.push_back(index);
            continue;
        }
        deepest = std::max(deepest, level[device]);
        columns[level[device]].push_back(index);
    }

    /* Wide enough for the widest body the table has (a Full Adder is three
       cells across) and tall enough that a body never lands on the one below. */
    constexpr int kColumnStep = 7;
    constexpr int kRowStep = 5;
    const auto place = [&](int column, size_t slot, size_t index) {
        slots[index] = std::make_pair(column * kColumnStep, static_cast<int>(slot) * kRowStep);
    };
    for (auto& entry : columns) {
        std::sort(entry.second.begin(), entry.second.end(), [&](size_t a, size_t b) {
            return board.components[a].y < board.components[b].y;
        });
        for (size_t slot = 0; slot < entry.second.size(); ++slot) place(entry.first, slot, entry.second[slot]);
    }
    std::sort(source_column.begin(), source_column.end(), [&](size_t a, size_t b) {
        return board.components[a].y < board.components[b].y;
    });
    for (size_t slot = 0; slot < source_column.size(); ++slot) place(-1, slot, source_column[slot]);
    std::sort(undeviced.begin(), undeviced.end(), [&](size_t a, size_t b) {
        return board.components[a].y < board.components[b].y;
    });
    for (size_t slot = 0; slot < undeviced.size(); ++slot) place(deepest + 1, slot, undeviced[slot]);
    return slots;
}

}  // namespace scene_detail

inline Scene buildScene(const SceneInput& input, const SceneOptions& options = SceneOptions()) {
    Scene scene;
    if (!input.board) return scene;
    const tcsim::BoardView& board = *input.board;
    const float cell = options.cell;
    const float margin = options.margin;

    /* Either the board's own coordinates or the tidy grid.  Everything below
       asks this one accessor, so the two modes cannot disagree about where a
       component is. */
    std::vector<std::pair<int, int>> tidy;
    if (options.tidy_layout && input.built) tidy = scene_detail::tidyPositions(board, *input.built);
    const auto at = [&](size_t index, const tcsim::BoardComponent& component) {
        if (!tidy.empty() && index < tidy.size()) return tidy[index];
        return std::make_pair(static_cast<int>(component.x), static_cast<int>(component.y));
    };

    /* Which component pin, if any, sits on a cell - in the board's own
       coordinates.  In tidy mode a wire has to follow its pins to the new grid,
       or every wire would still be drawn pointing at where its component used
       to be. */
    std::map<std::pair<int, int>, std::pair<size_t, std::pair<int, int>>> pin_origin;
    for (size_t index = 0; index < board.components.size(); ++index) {
        const tcsim::BoardComponent& component = board.components[index];
        if (component.kind == 0x00) continue;
        tcsim::KindPins pins;
        if (!pinsForKind(component, pins)) continue;
        for (uint8_t pin = 0; pin < pins.input_count; ++pin) {
            const auto offset = tcsim::rotateOffset(pins.in_x[pin], pins.in_y[pin], component.rotation);
            pin_origin[{component.x + offset.first, component.y + offset.second}] =
                {index, {offset.first, offset.second}};
        }
        for (uint8_t pin = 0; pin < pins.output_count; ++pin) {
            const auto offset = tcsim::rotateOffset(pins.out_x[pin], pins.out_y[pin], component.rotation);
            pin_origin[{component.x + offset.first, component.y + offset.second}] =
                {index, {offset.first, offset.second}};
        }
    }
    const auto remap = [&](int x, int y) {
        if (tidy.empty()) return std::make_pair(x, y);
        const auto found = pin_origin.find({x, y});
        if (found == pin_origin.end()) return std::make_pair(x, y);
        const std::pair<int, int> position = at(found->second.first, board.components[found->second.first]);
        return std::make_pair(position.first + found->second.second.first,
                              position.second + found->second.second.second);
    };

    /* Bodies in absolute cell units, for the router, and each pin's outward
       direction. */
    std::vector<scene_detail::BodyBox> body_cells(board.components.size());
    for (size_t index = 0; index < board.components.size(); ++index) {
        const tcsim::BoardComponent& component = board.components[index];
        if (component.kind == 0x00) continue;
        tcsim::KindPins pins;
        if (!pinsForKind(component, pins)) continue;
        if (pins.input_count != component.input_count || pins.output_count != component.output_count) continue;
        const std::pair<int, int> position = at(index, component);
        const scene_detail::BodyBox local = scene_detail::bodyCellsFor(pins, component.rotation);
        scene_detail::BodyBox box;
        box.left = local.left + static_cast<float>(position.first);
        box.right = local.right + static_cast<float>(position.first);
        box.top = local.top + static_cast<float>(position.second);
        box.bottom = local.bottom + static_cast<float>(position.second);
        body_cells[index] = box;
    }
    const auto normal_of = [&](int x, int y) {
        const auto found = pin_origin.find({x, y});
        if (found == pin_origin.end()) return std::make_pair(0, 0);
        const tcsim::BoardComponent& component = board.components[found->second.first];
        tcsim::KindPins pins;
        if (!pinsForKind(component, pins)) return std::make_pair(0, 0);
        return scene_detail::pinNormal(found->second.second.first, found->second.second.second,
                                       scene_detail::bodyCellsFor(pins, component.rotation));
    };
    const size_t kNoComponent = board.components.size();
    const auto component_at_pin = [&](int x, int y) {
        const auto found = pin_origin.find({x, y});
        return found == pin_origin.end() ? kNoComponent : found->second.first;
    };
    const auto crossings = [&](int ax, int ay, int bx, int by, size_t skip_a, size_t skip_b) {
        int count = 0;
        for (size_t index = 0; index < body_cells.size(); ++index) {
            if (index == skip_a || index == skip_b) continue;
            if (body_cells[index].right <= body_cells[index].left) continue;
            if (scene_detail::segmentHitsBody(ax, ay, bx, by, body_cells[index])) ++count;
        }
        return count;
    };

    /* Legs already claimed by earlier wires.  Two wires running along the same
       track is what makes a picture ambiguous in the first place, so a wire
       prefers to take the other way round when one is free. */
    struct PlacedLeg {
        int x1 = 0;
        int y1 = 0;
        int x2 = 0;
        int y2 = 0;
    };
    std::vector<PlacedLeg> placed_legs;
    const auto overlapCost = [&](int x1, int y1, int x2, int y2) {
        int cost = 0;
        const bool horizontal = y1 == y2;
        for (const PlacedLeg& leg : placed_legs) {
            if ((leg.y1 == leg.y2) != horizontal) continue;
            if (horizontal) {
                if (y1 != leg.y1) continue;
                if (std::min(x1, x2) < std::max(leg.x1, leg.x2) &&
                    std::min(leg.x1, leg.x2) < std::max(x1, x2)) {
                    ++cost;
                }
            } else {
                if (x1 != leg.x1) continue;
                if (std::min(y1, y2) < std::max(leg.y1, leg.y2) &&
                    std::min(leg.y1, leg.y2) < std::max(y1, y2)) {
                    ++cost;
                }
            }
        }
        return cost;
    };

    /* Routing happens in cells and entirely before any pixel is computed: a
       detour leaves the rectangle the components and the original endpoints
       describe, and anything outside the extents would be clipped away. */
    struct RoutedWire {
        size_t index = 0;
        Rgb color;
        std::vector<std::pair<int, int>> path;
        /* The endpoints as the board recorded them, before the tidy grid moved
           anything: which cell is a pin is a fact about the board. */
        std::pair<int, int> from_cell{0, 0};
        std::pair<int, int> to_cell{0, 0};
    };
    std::vector<RoutedWire> routed;
    for (size_t index = 0; index < board.wires.size(); ++index) {
        const tcsim::BoardWire& wire = board.wires[index];
        Rgb color = kWireDefault;
        if (input.built && input.values && index < input.built->net_of_wire.size()) {
            const tcsim::NetId net = input.built->net_of_wire[index];
            if (net != tcsim::kNoNet && net < input.values->netCount()) {
                const tcsim::BitVector& value = input.values->net(net).value;
                if (value.size() > 0) color = valueColor(value[0]);
            }
        }
        const std::pair<int, int> a = remap(wire.x1, wire.y1);
        const std::pair<int, int> b = remap(wire.x2, wire.y2);
        const std::pair<int, int> normal_a = normal_of(wire.x1, wire.y1);
        const std::pair<int, int> normal_b = normal_of(wire.x2, wire.y2);
        const std::pair<int, int> escape_a{a.first + normal_a.first, a.second + normal_a.second};
        const std::pair<int, int> escape_b{b.first + normal_b.first, b.second + normal_b.second};

        /* The channel a detour runs in: just outside the two end components,
           so it stays local to this wire instead of running to the edge of the
           board. */
        const auto body_of_pin = [&](int x, int y) {
            const size_t component = component_at_pin(x, y);
            if (component >= body_cells.size() || body_cells[component].right <= body_cells[component].left) {
                return scene_detail::BodyBox{static_cast<float>(x) - 1.0f, static_cast<float>(x) + 1.0f,
                                             static_cast<float>(y) - 1.0f, static_cast<float>(y) + 1.0f};
            }
            return body_cells[component];
        };
        const scene_detail::BodyBox box_a = body_of_pin(wire.x1, wire.y1);
        const scene_detail::BodyBox box_b = body_of_pin(wire.x2, wire.y2);
        const float escape_x_low = static_cast<float>(std::min(escape_a.first, escape_b.first));
        const float escape_x_high = static_cast<float>(std::max(escape_a.first, escape_b.first));
        const int channel_top = static_cast<int>(std::floor(std::min(box_a.top, box_b.top) - 1.5f));
        const int channel_bottom = static_cast<int>(std::ceil(std::max(box_a.bottom, box_b.bottom) + 1.5f));
        const int channel_left =
            static_cast<int>(std::floor(std::min(std::min(box_a.left, box_b.left), escape_x_low) - 1.5f));
        const int channel_right =
            static_cast<int>(std::ceil(std::max(std::max(box_a.right, box_b.right), escape_x_high) + 1.5f));

        /* Right angles, not diagonals, with a stub at each end that follows the
           pin out of (or into) its own part. */
        const auto build = [&](std::initializer_list<std::pair<int, int>> middle) {
            std::vector<std::pair<int, int>> path;
            path.push_back(a);
            if (normal_a.first || normal_a.second) path.push_back(escape_a);
            for (const std::pair<int, int>& point : middle) path.push_back(point);
            if (escape_b != path.back()) path.push_back(escape_b);
            if (b != path.back()) path.push_back(b);
            return scene_detail::simplifyPath(path);
        };
        const auto cost_of = [&](const std::vector<std::pair<int, int>>& path) {
            const size_t skip_a = component_at_pin(wire.x1, wire.y1);
            const size_t skip_b = component_at_pin(wire.x2, wire.y2);
            int total = 1000 * scene_detail::reversalsIn(path);
            for (size_t step = 1; step < path.size(); ++step) {
                total += std::abs(path[step].first - path[step - 1].first) +
                         std::abs(path[step].second - path[step - 1].second);
                total += 20 * crossings(path[step - 1].first, path[step - 1].second, path[step].first,
                                        path[step].second, skip_a, skip_b);
                total += 30 * overlapCost(path[step - 1].first, path[step - 1].second, path[step].first,
                                          path[step].second);
            }
            return total;
        };

        /* Every way this wire could reasonably go, then the cheapest.  A
           reversal costs far more than a detour, because a wire that doubles
           back on itself is the one that reads as broken; running through a
           component costs more than a few extra cells but less than that. */
        std::vector<std::vector<std::pair<int, int>>> candidates;
        /* The route the game itself draws wins when the record carried one. */
        const std::vector<std::vector<std::pair<int, int>>>* given_paths = input.wire_paths;
        if (given_paths && index < given_paths->size() && (*given_paths)[index].size() >= 2) {
            const std::vector<std::pair<int, int>>& given = (*given_paths)[index];
            for (size_t step = 1; step < given.size(); ++step)
                if (given[step] != given[step - 1])
                    placed_legs.push_back(PlacedLeg{given[step - 1].first, given[step - 1].second,
                                                    given[step].first, given[step].second});
            RoutedWire entry;
            entry.index = index;
            entry.color = color;
            entry.path = given;
            entry.from_cell = {wire.x1, wire.y1};
            entry.to_cell = {wire.x2, wire.y2};
            routed.push_back(entry);
            continue;
        }
        if (escape_a.first == escape_b.first || escape_a.second == escape_b.second) {
            candidates.push_back(build({}));
        }
        candidates.push_back(build({{escape_b.first, escape_a.second}}));
        candidates.push_back(build({{escape_a.first, escape_b.second}}));
        candidates.push_back(build({{escape_a.first, channel_bottom}, {escape_b.first, channel_bottom}}));
        candidates.push_back(build({{escape_a.first, channel_top}, {escape_b.first, channel_top}}));
        candidates.push_back(build({{channel_left, escape_a.second}, {channel_left, escape_b.second}}));
        candidates.push_back(build({{channel_right, escape_a.second}, {channel_right, escape_b.second}}));

        std::vector<std::pair<int, int>> best = candidates.front();
        int best_cost = cost_of(best);
        for (const std::vector<std::pair<int, int>>& candidate : candidates) {
            const int cost = cost_of(candidate);
            if (cost < best_cost) {
                best_cost = cost;
                best = candidate;
            }
        }
        for (size_t step = 1; step < best.size(); ++step) {
            placed_legs.push_back(PlacedLeg{best[step - 1].first, best[step - 1].second, best[step].first,
                                            best[step].second});
        }
        RoutedWire entry;
        entry.index = index;
        entry.color = color;
        entry.path = best;
        entry.from_cell = {wire.x1, wire.y1};
        entry.to_cell = {wire.x2, wire.y2};
        routed.push_back(entry);
    }

    /* The extents, now that the routes are known: the components (with their
       bodies, which reach past their own cell), the pins and every point a wire
       actually visits. */
    bool any = false;
    int low_x = 0;
    int high_x = 0;
    int low_y = 0;
    int high_y = 0;
    const auto include = [&](float x, float y) {
        const int left = static_cast<int>(std::floor(x));
        const int right = static_cast<int>(std::ceil(x));
        const int top = static_cast<int>(std::floor(y));
        const int bottom = static_cast<int>(std::ceil(y));
        if (!any) {
            low_x = left;
            high_x = right;
            low_y = top;
            high_y = bottom;
            any = true;
            return;
        }
        low_x = std::min(low_x, left);
        high_x = std::max(high_x, right);
        low_y = std::min(low_y, top);
        high_y = std::max(high_y, bottom);
    };
    for (size_t index = 0; index < board.components.size(); ++index) {
        const tcsim::BoardComponent& component = board.components[index];
        if (component.kind == 0x00) continue;
        const std::pair<int, int> position = at(index, component);
        include(static_cast<float>(position.first), static_cast<float>(position.second));
        if (body_cells[index].right > body_cells[index].left) {
            include(body_cells[index].left, body_cells[index].top);
            include(body_cells[index].right, body_cells[index].bottom);
        }
        tcsim::KindPins pins;
        if (pinsForKind(component, pins)) {
            for (uint8_t pin = 0; pin < pins.input_count; ++pin) {
                const auto offset = tcsim::rotateOffset(pins.in_x[pin], pins.in_y[pin], component.rotation);
                include(static_cast<float>(position.first + offset.first),
                        static_cast<float>(position.second + offset.second));
            }
            for (uint8_t pin = 0; pin < pins.output_count; ++pin) {
                const auto offset = tcsim::rotateOffset(pins.out_x[pin], pins.out_y[pin], component.rotation);
                include(static_cast<float>(position.first + offset.first),
                        static_cast<float>(position.second + offset.second));
            }
        }
    }
    for (const RoutedWire& wire : routed) {
        for (const std::pair<int, int>& point : wire.path) {
            include(static_cast<float>(point.first), static_cast<float>(point.second));
        }
    }
    if (!any) {
        scene.notes.push_back("the board has no components and no wires");
        scene.width = 320;
        scene.height = 200;
        return scene;
    }

    const float origin_x = margin + cell * 0.5f;
    const float origin_y = margin + cell * 0.5f;
    const auto px = [&](int x) { return origin_x + static_cast<float>(x - low_x) * cell; };
    const auto py = [&](int y) { return origin_y + static_cast<float>(y - low_y) * cell; };
    scene.width = margin * 2.0f + static_cast<float>(high_x - low_x) * cell;
    scene.height = margin * 2.0f + static_cast<float>(high_y - low_y) * cell;

    /* Wires first, so a body and its pins sit on top of them.  They are
       collected here and sorted into the scene at the end. */
    std::vector<SceneLine> raw_wires;
    for (const RoutedWire& wire : routed) {
        for (size_t step = 1; step < wire.path.size(); ++step) {
            const std::pair<int, int>& from = wire.path[step - 1];
            const std::pair<int, int>& to = wire.path[step];
            if (from == to) continue;
            SceneLine line;
            line.x1 = px(from.first);
            line.y1 = py(from.second);
            line.x2 = px(to.first);
            line.y2 = py(to.second);
            line.color = wire.color;
            line.width = 2.0f;
            line.wire_index = wire.index;
            line.has_wire = true;
            raw_wires.push_back(line);
        }
    }

    /* Draw order and casing.

       Two wires that share a track were indistinguishable before: whichever was
       emitted last simply painted over the other, so a wire ending exactly on
       another wire's run read as a connection.  Two changes fix that.  Sorting
       by length puts the short pin stubs above the long runs they land on, and
       a casing in the canvas colour makes whichever is on top visibly lie over
       the one below instead of merging with it. */
    std::sort(raw_wires.begin(), raw_wires.end(), [](const SceneLine& a, const SceneLine& b) {
        const float length_a = std::fabs(a.x2 - a.x1) + std::fabs(a.y2 - a.y1);
        const float length_b = std::fabs(b.x2 - b.x1) + std::fabs(b.y2 - b.y1);
        if (length_a != length_b) return length_a > length_b;
        if (a.wire_index != b.wire_index) return a.wire_index < b.wire_index;
        return a.x1 < b.x1 || (a.x1 == b.x1 && a.y1 < b.y1);
    });
    for (const SceneLine& line : raw_wires) {
        SceneLine halo = line;
        halo.halo = true;
        halo.width = line.width + 4.0f;
        halo.color = kCanvasFill;
        scene.lines.push_back(halo);
        scene.lines.push_back(line);
    }

    for (size_t index = 0; index < board.components.size(); ++index) {
        const tcsim::BoardComponent& component = board.components[index];
        if (component.kind == 0x00) continue; /* the board's own record, not a component */

        tcsim::KindPins pins;
        const bool known = pinsForKind(component, pins);
        const bool counts_match =
            known && pins.input_count == component.input_count && pins.output_count == component.output_count;
        const std::pair<int, int> position = at(index, component);

        Rgb fill = kBodyFill;
        Rgb stroke = kBodyStroke;
        if (!known) {
            fill = kUnknownKindFill;
            stroke = kUnknownKindStroke;
        } else if (!counts_match) {
            fill = kNoGeometryFill;
            stroke = kNoGeometryStroke;
        }

        const float centre_x = px(position.first);
        const float centre_y = py(position.second);
        /* A component whose pins the table does not describe still gets a
           placeholder box, drawn from the same rule so it sits where a real
           body would. */
        const tcsim::KindPins body_pins = known ? pins : tcsim::KindPins{};
        scene_detail::BodyBox box =
            scene_detail::bodyFor(body_pins, component.rotation, centre_x, centre_y, cell);
        /* A record that carried pins without a name (a Mod type: the frozen
           table has no row for it) still gets its kind spelled out rather than
           an empty label. */
        const std::string name =
            known && pins.name && *pins.name ? pins.name : scene_detail::fallbackName(component);
        /* The body also has to hold its own name.  An Input has both of its
           pins in one column, so a strictly pin-derived box is a sliver and the
           label spills out of both sides of it. */
        const float needed = (static_cast<float>(name.size()) * 0.30f + 0.5f) * cell;
        if (box.right - box.left < needed) {
            const float middle = (box.left + box.right) * 0.5f;
            box.left = middle - needed * 0.5f;
            box.right = middle + needed * 0.5f;
        }

        SceneRect rect;
        rect.x = box.left;
        rect.y = box.top;
        rect.w = box.right - box.left;
        rect.h = box.bottom - box.top;
        rect.fill = fill;
        rect.stroke = stroke;
        rect.component_index = index;
        rect.has_component = true;
        if (input.show_labels) {
            rect.label = name;
            rect.sub_label = std::to_string(position.first) + "," + std::to_string(position.second);
            if (component.rotation) rect.sub_label += " r" + std::to_string(component.rotation * 90);
        }
        scene.rects.push_back(rect);

        if (!known || !counts_match) {
            std::string note = "component " + std::to_string(index) + " (" +
                               scene_detail::fallbackName(component) + ") at " + std::to_string(position.first) + "," +
                               std::to_string(position.second);
            if (!known) {
                note += ": no pin geometry in the frozen table";
            } else {
                note += ": record says " + std::to_string(component.input_count) + "/" +
                        std::to_string(component.output_count) + " pins, the table says " +
                        std::to_string(pins.input_count) + "/" + std::to_string(pins.output_count);
            }
            scene.notes.push_back(note);
        }

        if (!input.show_pins || !known || !counts_match) continue;
        const uint8_t rotation = component.rotation;
        for (uint8_t pin = 0; pin < pins.input_count; ++pin) {
            const auto offset = tcsim::rotateOffset(pins.in_x[pin], pins.in_y[pin], rotation);
            scene.dots.push_back(SceneDot{px(position.first + offset.first), py(position.second + offset.second), 4.0f,
                                          kPinFill, kBodyStroke});
        }
        for (uint8_t pin = 0; pin < pins.output_count; ++pin) {
            const auto offset = tcsim::rotateOffset(pins.out_x[pin], pins.out_y[pin], rotation);
            Rgb fill_color = kPinFill;
            if (input.built && input.values) {
                const tcsim::DeviceId device =
                    index < input.built->devices_by_component.size() ? input.built->devices_by_component[index]
                                                                    : tcsim::kNoDevice;
                if (device != tcsim::kNoDevice && device < input.values->deviceCount()) {
                    const tcsim::DeviceInstance& instance = input.values->device(device);
                    if (pin < instance.outputs.size()) {
                        const tcsim::NetId net = instance.outputs[pin];
                        if (net != tcsim::kNoNet && net < input.values->netCount()) {
                            const tcsim::BitVector& value = input.values->net(net).value;
                            if (value.size() > 0) fill_color = valueColor(value[0]);
                        }
                    }
                }
            }
            scene.dots.push_back(SceneDot{px(position.first + offset.first), py(position.second + offset.second), 4.0f,
                                          fill_color, kBodyStroke});
        }
    }

    if (input.built) {
        for (const tcsim::BuildNote& note : input.built->notes) {
            scene.notes.push_back(std::string(note.fatal ? "FATAL: " : "") + note.text);
        }
    }

    /* Where a wire lands on a pin, put a dot in the wire's own colour on the
       joint.

       Without it the picture cannot answer the one question it exists to
       answer: a wire ending at an input looked exactly like a wire that stops
       in mid air, because the pin itself is a small dark dot whatever is
       attached to it.  A dot means connected; a wire end with no dot is a
       dangling end.  These are pushed last so they sit above the pin markers,
       and they are the reason a reader can stop squinting at an endpoint. */
    for (const RoutedWire& wire : routed) {
        if (wire.path.empty()) continue;
        const auto mark = [&](const std::pair<int, int>& original, const std::pair<int, int>& cell) {
            if (pin_origin.find(original) == pin_origin.end()) return;
            SceneDot dot;
            dot.x = px(cell.first);
            dot.y = py(cell.second);
            dot.r = 5.5f;
            dot.fill = wire.color;
            dot.stroke = kBodyStroke;
            dot.connection = true;
            scene.dots.push_back(dot);
        };
        mark(wire.from_cell, wire.path.front());
        mark(wire.to_cell, wire.path.back());
    }
    return scene;
}

namespace pick_detail {

inline float distanceToSegment(float px, float py, float x1, float y1, float x2, float y2) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float length_squared = dx * dx + dy * dy;
    if (length_squared <= 0.0f) return std::hypot(px - x1, py - y1);
    float t = ((px - x1) * dx + (py - y1) * dy) / length_squared;
    t = std::max(0.0f, std::min(1.0f, t));
    return std::hypot(px - (x1 + t * dx), py - (y1 + t * dy));
}

}  // namespace pick_detail

/* Nearest thing under the point.  Tolerance is generous because the target is
   a human aiming at a 30 px box, not a hit test on a real UI. */
inline ScenePick pickScene(const Scene& scene, float x, float y, float tolerance = 5.0f) {
    ScenePick found;
    for (size_t index = scene.rects.size(); index-- > 0;) {
        const SceneRect& rect = scene.rects[index];
        if (!rect.has_component) continue;
        if (x >= rect.x - tolerance && x <= rect.x + rect.w + tolerance && y >= rect.y - tolerance &&
            y <= rect.y + rect.h + tolerance) {
            found.valid = true;
            found.on_component = true;
            found.component = rect.component_index;
            return found;
        }
    }
    float best = tolerance;
    for (const SceneLine& line : scene.lines) {
        if (!line.has_wire || line.halo) continue;
        const float distance =
            pick_detail::distanceToSegment(x, y, line.x1, line.y1, line.x2, line.y2);
        if (distance <= best) {
            best = distance;
            found.valid = true;
            found.on_component = false;
            found.on_wire = true;
            found.wire = line.wire_index;
        }
    }
    return found;
}

}  // namespace simcore_view
