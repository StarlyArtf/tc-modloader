#pragma once
/* Reads the board fixture format simcore/tests/simcore-tests.cpp already
   parses, so a board captured once can be replayed by the tests and drawn by
   the viewer without a second format:

     component index=0 kind=0x3f x=-5 y=0 rotation=0 word30=0 word40=2
     wire index=0 from=-5,1 to=0,1 width=1 slot=256

   Lines starting with '#' and blank lines are ignored.  `index=` is accepted
   and ignored (the position in the file is the index).  `custom=` is optional
   and only used for the label of a custom component (kind 0x4e).

   Deliberately no game dependency and no binary format: a fixture is text so
   it can be reviewed in a diff.  Reading a real save file (a snappy-compressed
   circuit.data) belongs to a later step and needs python-snappy, which
   simcore must not depend on. */

#include "../include/tcsim/board.hpp"
#include "../include/tcsim/custom.hpp"
#include "kind_pins.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace simcore_view {

struct BoardFile {
    tcsim::BoardView board;
    /* One entry per wire: the route the game itself draws, when the record
       carries it (`path=x,y;x,y;...`).  Empty for a wire that has none, which is
       when the viewer falls back to its own routing. */
    std::vector<std::vector<std::pair<int, int>>> wire_paths;
    /* One entry per component: the level an interactive source is holding, or
       255 when the record did not carry one (`level=`). */
    std::vector<uint8_t> component_levels;
    /* One entry per wire: the value the board is showing, or 255 when the record
       did not carry one (`value=`).  A latch's state lives in its feedback nets,
       so seeding those from the record is what makes "the switch was set
       earlier" survive into this window. */
    std::vector<uint8_t> wire_values;
    /* Pins the record carried for a component (`inpins=` / `outpins=`), one entry
       per component, plus their widths (`inbits=` / `outbits=`, one width per
       pin).  The live Mod writes both for *every* component, from the game's own
       prototype table - a Mod type's pins are whatever its Mod declared, and a
       built-in's are whatever that build of the game says they are, so they
       travel with the record.  A component without them falls back to the
       frozen table. */
    std::vector<std::vector<std::pair<int, int>>> component_in_pins;
    std::vector<std::vector<std::pair<int, int>>> component_out_pins;
    std::vector<std::vector<uint16_t>> component_in_bits;
    std::vector<std::vector<uint16_t>> component_out_bits;
    /* The circuits behind the board's custom components (kind 0x4e), keyed by
       the prototype id the component names.  The live Mod writes them into the
       same file (`defcustom` / `defcomponent` / `defwire`); without them the
       viewer could not flatten a custom component and would refuse a board the
       Mod simulates. */
    std::map<uint64_t, tcsim::CustomDefinition> definitions;
    /* Components that only carry pins: the definition components the record
       described, in file order.  A definition may place another custom
       component, and flattening that nested instance needs *its* pins - only
       the game knew them, so they travel with the record like the board's own
       do. */
    std::vector<tcsim::BoardComponent> extra_components;
    std::vector<std::vector<std::pair<int, int>>> extra_in_pins;
    std::vector<std::vector<std::pair<int, int>>> extra_out_pins;
    std::vector<std::vector<uint16_t>> extra_in_bits;
    std::vector<std::vector<uint16_t>> extra_out_bits;
    bool ok = false;
    std::string error;
};

inline long long field(const std::string& line, const char* key, size_t key_length, bool* found) {
    const size_t at = line.find(key);
    if (at == std::string::npos) {
        if (found) *found = false;
        return 0;
    }
    if (found) *found = true;
    return std::strtoll(line.c_str() + at + key_length, nullptr, 10);
}

/* The same, for the fields that are written in hexadecimal (`kind=0x..`,
   `def=0x..`, a custom prototype id). */
inline uint64_t hexField(const std::string& line, const char* key, size_t key_length, bool* found) {
    const size_t at = line.find(key);
    if (at == std::string::npos) {
        if (found) *found = false;
        return 0;
    }
    if (found) *found = true;
    return std::strtoull(line.c_str() + at + key_length, nullptr, 16);
}

/* "x,y;x,y;..." into points, for the two list-shaped fields. */
inline std::vector<std::pair<int, int>> parsePoints(const std::string& text) {
    std::vector<std::pair<int, int>> points;
    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t semi = text.find(';', cursor);
        const std::string item =
            text.substr(cursor, semi == std::string::npos ? std::string::npos : semi - cursor);
        const size_t comma = item.find(',');
        if (comma != std::string::npos)
            points.push_back({static_cast<int>(std::strtol(item.c_str(), nullptr, 10)),
                              static_cast<int>(std::strtol(item.c_str() + comma + 1, nullptr, 10))});
        if (semi == std::string::npos) break;
        cursor = semi + 1;
    }
    return points;
}

/* "1;8;8" into per-pin widths.  A missing or zero entry means "the record did
   not carry one", which is what every board written before this field existed
   says - the caller then falls back to the frozen table's own number rather
   than treating the pin as one bit wide. */
inline std::vector<uint16_t> parseWidths(const std::string& text) {
    std::vector<uint16_t> widths;
    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t semi = text.find(';', cursor);
        const std::string item =
            text.substr(cursor, semi == std::string::npos ? std::string::npos : semi - cursor);
        const long value = std::strtol(item.c_str(), nullptr, 10);
        widths.push_back(value > 0 ? static_cast<uint16_t>(value < 64 ? value : 64) : 0);
        if (semi == std::string::npos) break;
        cursor = semi + 1;
    }
    return widths;
}

inline BoardFile readBoardFile(const std::string& path) {
    BoardFile result;
    std::ifstream file(path);
    if (!file) {
        result.error = "cannot open " + path;
        return result;
    }
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("defcustom", 0) == 0) continue; /* a definition's header line */

        /* A definition line is one of the board's own lines with a three-letter
           prefix in front and `def=0x<id>` at the end: the same field extraction
           reads both, and the id says which definition the record belongs to. */
        bool in_definition = false;
        uint64_t definition_id = 0;
        if (line.rfind("defcomponent ", 0) == 0 || line.rfind("defwire ", 0) == 0) {
            bool has_definition = false;
            definition_id = hexField(line, " def=0x", 7, &has_definition);
            if (!has_definition) {
                result.error = "definition line without def= : " + line;
                return result;
            }
            in_definition = true;
            line = line.substr(3);
        }

        if (line.rfind("component", 0) == 0) {
            tcsim::BoardComponent component;
            unsigned kind = 0;
            long long x = 0;
            long long y = 0;
            long long rotation = 0;
            if (std::sscanf(line.c_str(), "component index=%*d kind=0x%x x=%lld y=%lld rotation=%lld", &kind, &x,
                            &y, &rotation) != 4) {
                result.error = "bad component line: " + line;
                return result;
            }
            component.kind = static_cast<uint8_t>(kind);
            component.x = static_cast<int16_t>(x);
            component.y = static_cast<int16_t>(y);
            component.rotation = static_cast<uint8_t>(rotation);
            bool found = false;
            const long long inputs = field(line, " word30=", 8, &found);
            if (found) component.input_count = static_cast<uint8_t>(inputs);
            const long long outputs = field(line, " word40=", 8, &found);
            if (found) component.output_count = static_cast<uint8_t>(outputs);
            const long long id = field(line, " id=", 4, &found);
            if (found) component.id = static_cast<uint64_t>(id);
            const long long custom = field(line, " custom=", 8, &found);
            if (found) component.custom_id = static_cast<uint64_t>(custom);
            const long long memory_bits = field(line, " memory_bits=", 13, &found);
            if (found) component.memory_bits = static_cast<uint16_t>(memory_bits);
            const long long memory_extra = field(line, " memory_extra=", 14, &found);
            if (found) component.memory_extra_address_bits = static_cast<uint16_t>(memory_extra);
            const long long little_endian = field(line, " little_endian=", 15, &found);
            if (found) component.memory_little_endian = little_endian != 0;
            /* A Static Indexer's field width lives in the record's +0x70, which
               is the one setting its device cannot be built without. */
            const long long field_bits = field(line, " field_bits=", 12, &found);
            if (found) component.field_bits = static_cast<uint16_t>(field_bits);
            uint8_t level = 255;
            const long long level_value = field(line, " level=", 7, &found);
            if (found && level_value >= 0 && level_value <= 1)
                level = static_cast<uint8_t>(level_value);
            /* The width lists are written before the pin lists so that each
               field ends at the next key and the parser needs no quoting. */
            std::string in_text, out_text, in_bits_text, out_bits_text;
            const size_t inpins_at = line.find(" inpins=");
            const size_t outpins_at = line.find(" outpins=");
            const size_t inbits_at = line.find(" inbits=");
            const size_t outbits_at = line.find(" outbits=");
            /* The next key's own leading space is where this value stops.  When
               the value is empty those two positions coincide (a component with
               no inputs at all writes `inpins= outpins=...`), and slicing to the
               end of the line would then hand the *next* field to this one. */
            const auto slice = [&line](size_t key_at, size_t key_length, size_t stop_at) {
                const size_t start = key_at + key_length;
                if (stop_at == std::string::npos) return line.substr(start);
                if (stop_at <= start) return std::string();
                return line.substr(start, stop_at - start);
            };
            if (inbits_at != std::string::npos) in_bits_text = slice(inbits_at, 8, outbits_at);
            if (outbits_at != std::string::npos) out_bits_text = slice(outbits_at, 9, inpins_at);
            if (inpins_at != std::string::npos) {
                in_text = slice(inpins_at, 8, outpins_at);
            }
            if (outpins_at != std::string::npos) out_text = slice(outpins_at, 9, std::string::npos);
            if (in_definition) {
                result.definitions[definition_id].components.push_back(component);
                result.extra_components.push_back(component);
                result.extra_in_pins.push_back(parsePoints(in_text));
                result.extra_out_pins.push_back(parsePoints(out_text));
                result.extra_in_bits.push_back(parseWidths(in_bits_text));
                result.extra_out_bits.push_back(parseWidths(out_bits_text));
            } else {
                result.component_levels.push_back(level);
                result.component_in_pins.push_back(parsePoints(in_text));
                result.component_out_pins.push_back(parsePoints(out_text));
                result.component_in_bits.push_back(parseWidths(in_bits_text));
                result.component_out_bits.push_back(parseWidths(out_bits_text));
                result.board.components.push_back(component);
            }
            continue;
        }

        if (line.rfind("wire", 0) == 0) {
            tcsim::BoardWire wire;
            long long x1 = 0;
            long long y1 = 0;
            long long x2 = 0;
            long long y2 = 0;
            long long width = 0;
            if (std::sscanf(line.c_str(), "wire index=%*d from=%lld,%lld to=%lld,%lld width=%lld", &x1, &y1, &x2,
                            &y2, &width) != 5) {
                result.error = "bad wire line: " + line;
                return result;
            }
            /* A definition's wires carry no state slot - there is nothing of the
               game's to publish to inside a definition - so the field is
               optional here and the sentinel is what the flatten uses. */
            bool has_slot = false;
            const uint64_t slot = static_cast<uint64_t>(field(line, " slot=", 6, &has_slot));
            wire.x1 = static_cast<int16_t>(x1);
            wire.y1 = static_cast<int16_t>(y1);
            wire.x2 = static_cast<int16_t>(x2);
            wire.y2 = static_cast<int16_t>(y2);
            wire.width = static_cast<uint16_t>(width);
            wire.slot = has_slot ? slot : tcsim::kNoStateSlot;
            uint8_t value = 255;
            bool found = false;
            const long long wire_value = field(line, " value=", 7, &found);
            if (found && wire_value >= 0 && wire_value <= 1)
                value = static_cast<uint8_t>(wire_value);
            result.wire_values.push_back(value);
            std::vector<std::pair<int, int>> route;
            const size_t at = line.find(" path=");
            if (at != std::string::npos) {
                const std::string rest = line.substr(at + 6);
                size_t cursor = 0;
                while (cursor < rest.size()) {
                    const size_t semi = rest.find(';', cursor);
                    const std::string item = rest.substr(
                        cursor, semi == std::string::npos ? std::string::npos : semi - cursor);
                    const size_t comma = item.find(',');
                    if (comma != std::string::npos)
                        route.push_back(
                            {static_cast<int>(std::strtol(item.c_str(), nullptr, 10)),
                             static_cast<int>(std::strtol(item.c_str() + comma + 1, nullptr, 10))});
                    if (semi == std::string::npos) break;
                    cursor = semi + 1;
                }
            }
            if (in_definition) {
                wire.slot = tcsim::kNoStateSlot;
                result.definitions[definition_id].wires.push_back(wire);
            } else {
                result.wire_paths.push_back(std::move(route));
                result.board.wires.push_back(wire);
            }
            continue;
        }
    }
    result.ok = true;
    return result;
}

/* The pins that travelled with the record, registered so every lookup - the
   drawing's and the netlist's alike - finds them.  The live Mod writes them for
   *every* component out of the game's own prototype table, which is what makes
   the viewer agree with the running sandbox; the frozen table in kind_pins.hpp
   is only the fallback for a fixture written before those fields existed.
   Called from netlistView so the window and the tests cannot disagree. */
inline void registerRecordGeometry(const BoardFile& file) {
    recordGeometry().clear();
    /* One row per component whose pins the record carried: the board's own
       first, then every definition component (a nested custom instance has to
       be placeable too, or flattening it would leave a component the netlist
       cannot read). */
    const auto register_one = [](const tcsim::BoardComponent& component,
                                 const std::vector<std::pair<int, int>>& in_pins,
                                 const std::vector<std::pair<int, int>>& out_pins,
                                 const std::vector<uint16_t>& in_bits, const std::vector<uint16_t>& out_bits) {
        if (in_pins.empty() && out_pins.empty()) return;
        /* The row is keyed by instance id, so a component without one (the
           board's own record) has no place in the map. */
        if (!component.id) return;
        KindGeometry row;
        row.kind = component.kind;
        /* The prototype's *name* is the one thing a record cannot carry: it is
           the game's own text.  Taking it from the frozen table by kind keeps
           the labels readable; a kind that table never named falls back to its
           kind number, which is what an unidentified component should say. */
        const KindGeometry* known = geometryForKind(component.kind);
        row.name = known ? known->name : "";
        row.input_count = static_cast<uint8_t>(std::min<size_t>(in_pins.size(), tcsim::KindPins::kMaxPins));
        for (uint8_t pin = 0; pin < row.input_count; ++pin) {
            row.in_x[pin] = static_cast<int8_t>(in_pins[pin].first);
            row.in_y[pin] = static_cast<int8_t>(in_pins[pin].second);
            if (pin < in_bits.size()) row.in_bits[pin] = in_bits[pin];
        }
        row.output_count = static_cast<uint8_t>(std::min<size_t>(out_pins.size(), tcsim::KindPins::kMaxPins));
        for (uint8_t pin = 0; pin < row.output_count; ++pin) {
            row.out_x[pin] = static_cast<int8_t>(out_pins[pin].first);
            row.out_y[pin] = static_cast<int8_t>(out_pins[pin].second);
            if (pin < out_bits.size()) row.out_bits[pin] = out_bits[pin];
        }
        recordGeometry()[component.id] = row;
    };
    const auto none = std::vector<std::pair<int, int>>{};
    const auto no_bits = std::vector<uint16_t>{};
    for (size_t index = 0; index < file.board.components.size(); ++index) {
        register_one(file.board.components[index], index < file.component_in_pins.size() ? file.component_in_pins[index] : none,
                     index < file.component_out_pins.size() ? file.component_out_pins[index] : none,
                     index < file.component_in_bits.size() ? file.component_in_bits[index] : no_bits,
                     index < file.component_out_bits.size() ? file.component_out_bits[index] : no_bits);
    }
    for (size_t index = 0; index < file.extra_components.size(); ++index) {
        register_one(file.extra_components[index], index < file.extra_in_pins.size() ? file.extra_in_pins[index] : none,
                     index < file.extra_out_pins.size() ? file.extra_out_pins[index] : none,
                     index < file.extra_in_bits.size() ? file.extra_in_bits[index] : no_bits,
                     index < file.extra_out_bits.size() ? file.extra_out_bits[index] : no_bits);
    }
}

/* The view the netlist is built from: a record that carries custom definitions
   is flattened first, exactly the way the live Mod flattens the running board.
   Shared here because the window and the tests have to run the same thing - a
   viewer that skipped this could only refuse a board with a foundry component
   on it, which is what it used to do. */
inline tcsim::BoardView netlistView(const BoardFile& file) {
    registerRecordGeometry(file);
    if (file.definitions.empty()) return file.board;
    return tcsim::flattenCustomComponents(
        file.board,
        [&file](uint64_t id, tcsim::CustomDefinition& out) {
            const auto found = file.definitions.find(id);
            if (found == file.definitions.end()) return false;
            out = found->second;
            return true;
        },
        [](const tcsim::BoardComponent& component, tcsim::KindPins& pins) {
            return pinsForKind(component, pins);
        });
}

}  // namespace simcore_view
