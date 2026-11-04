#pragma once
/* The game's Screen component (kind 0x5b): the settings its three pins latch,
   and the picture those settings describe.

   None of this is guessed - every piece comes from the game's own files:

     * the pins - `build/kinds.txt` (re-dumped 2026-09-29):
       `0x5b name="Screen" inputs=3 outputs=0 in0=(-26,-19,w1) in1=(-26,-18,w8)
        in2=(-26,-17,w64)`.  The level that introduces it names them the same way
       (`campaign/symphony_8_io_devices/meta.txt`: "The screen has three inputs,
       Enable, Setting index and Value ... Enable pin should be [T]"), and a real
       board wires exactly that shape: the 1-bit wire lands on in0 while the
       other two come from 32-bit CPU wires;
     * what a write means - the level's ISA (`default.isa`): "screen %a, %b /
       Stores the value of %b in screen setting %a", which the level's own
       emulator applies verbatim (`test.si`: `screen_setting[a_value] = b_value`);
     * what the settings do, and how the pixels are packed - the game's manual,
       `asset/manual/Circuits/The screen/doc.txt`: mode 0 ASCII 8 (a 96x40
       character screen), mode 1 ASCII 32 (four bytes per cell: character,
       foreground, background, padding), mode 2 Pixel 8 (one byte per pixel,
       RRRGGGBB) and mode 3 Pixel 32 (four bytes per pixel: red, green, blue,
       padding).  A pixel mode's resolution is `4*(setting+1) x 3*(setting+1)`;
     * and a program that uses it that way - `symphony_8_io_devices/new_program.asm`
       writes mode 2, the data offset, then resolution 19, and its drawing code
       steps 80 bytes per pixel row, which is exactly `4*(19+1)`.

   A screen shows no value on a wire: the game links the component to a RAM (the
   little red button in its corner) and draws the bytes that RAM holds.  This
   header models the half a shadow simulator can own without the game - the
   settings table and the pixels those settings describe - so "what is the game
   showing" can be answered offline from a RAM image.  Which RAM that is, and
   where its bytes come from, is the data bridge that still needs a real session
   (see docs/PLAN-sandbox-simulator.md section 17). */

#include "memory.hpp"
#include "netlist.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tcsim {

/* The four modes the manual lists, in its own numbering (setting 0). */
enum class ScreenMode : uint8_t { kAscii8 = 0, kAscii32 = 1, kPixel8 = 2, kPixel32 = 3 };

inline const char* screenModeName(ScreenMode mode) {
    switch (mode) {
        case ScreenMode::kAscii8: return "ASCII 8";
        case ScreenMode::kAscii32: return "ASCII 32";
        case ScreenMode::kPixel8: return "Pixel 8";
        case ScreenMode::kPixel32: return "Pixel 32";
    }
    return "unknown";
}

/* What the three pins have latched.  `ScreenSettings::kCount` is the number of
   settings the game keeps: the level checks eight of them
   (`campaign/symphony_8_io_devices/test.si`: `array(<Int>, 8)`), the manual only
   ever names 0..4.  A write to an index outside the table is *recorded* rather
   than invented - the game's behaviour there has not been measured. */
struct ScreenSettings {
    static constexpr size_t kCount = 8;
    static constexpr size_t kNoUnlisted = static_cast<size_t>(-1);
    std::array<uint64_t, kCount> setting{};
    size_t unlisted_index = kNoUnlisted;

    uint64_t at(size_t index) const { return index < kCount ? setting[index] : 0; }
    ScreenMode mode() const { return static_cast<ScreenMode>(at(0) & 0xffu); }
    uint64_t data_offset() const { return at(1); }
    int resolution() const { return static_cast<int>(at(2) & 0xffu); }
    /* ASCII 8 only: foreground, background and font. */
    uint64_t ascii_foreground() const { return at(2); }
    uint64_t ascii_background() const { return at(3); }
    uint64_t ascii_font() const { return at(4); }
};

inline int screenPixelWidth(int resolution_setting) {
    return 4 * (resolution_setting + 1);
}
inline int screenPixelHeight(int resolution_setting) {
    return 3 * (resolution_setting + 1);
}

/* A rendered screen: one 0xRRGGBB word per pixel, row-major from the top left,
   exactly the layout the pixels sit in the RAM. */
struct ScreenImage {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
    bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
};

namespace screen_detail {

/* 3 bits of red, 3 of green, 2 of blue, spread back over 8 bits by repeating
   the bits (RRR -> RRRRRRRR).  The manual draws the split but not the
   expansion; this is the arithmetic that keeps 0 -> 0 and 7 -> 255. */
inline uint32_t expand332(uint8_t value) {
    const uint32_t red = (value >> 5) & 0x7u;
    const uint32_t green = (value >> 2) & 0x7u;
    const uint32_t blue = value & 0x3u;
    const uint32_t red8 = (red << 5) | (red << 2) | (red >> 1);
    const uint32_t green8 = (green << 5) | (green << 2) | (green >> 1);
    const uint32_t blue8 = (blue << 6) | (blue << 4) | (blue << 2) | blue;
    return (red8 << 16) | (green8 << 8) | blue8;
}

}  // namespace screen_detail

/* The picture the settings describe, read out of the RAM the screen is linked
   to.  Returns false and fills `note` for a mode this renderer cannot draw yet,
   so a caller reports it instead of showing a made-up picture:

     * the two ASCII modes need the game's character fonts
       (`asset/manual/Circuits/The screen/TC_cp437.png` and friends) and are not
       implemented;
     * a mode number outside 0..3 is named as-is. */
inline bool renderScreen(const ScreenSettings& settings, const MemoryImage& ram, ScreenImage& out,
                         std::string& note) {
    note.clear();
    const ScreenMode mode = settings.mode();
    if (mode == ScreenMode::kAscii8 || mode == ScreenMode::kAscii32) {
        note = std::string("the screen is in ") + screenModeName(mode) +
               " mode, which needs the game's character font; not rendered";
        return false;
    }
    if (static_cast<uint8_t>(mode) > static_cast<uint8_t>(ScreenMode::kPixel32)) {
        note = "the screen's mode setting is " + std::to_string(static_cast<int>(settings.at(0))) +
               ", which is not one of the four documented modes";
        return false;
    }
    const int resolution = settings.resolution();
    out.width = screenPixelWidth(resolution);
    out.height = screenPixelHeight(resolution);
    out.pixels.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height), 0);
    const uint64_t base = settings.data_offset();
    const uint64_t bytes_per_pixel = mode == ScreenMode::kPixel8 ? 1u : 4u;
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const uint64_t at = base + (static_cast<uint64_t>(y) * static_cast<uint64_t>(out.width) +
                                        static_cast<uint64_t>(x)) * bytes_per_pixel;
            if (mode == ScreenMode::kPixel8) {
                out.pixels[static_cast<size_t>(y) * static_cast<size_t>(out.width) + static_cast<size_t>(x)] =
                    screen_detail::expand332(ram.readByte(at));
                continue;
            }
            const uint32_t red = ram.readByte(at);
            const uint32_t green = ram.readByte(at + 1);
            const uint32_t blue = ram.readByte(at + 2);
            /* at + 3 is the padding byte the manual documents. */
            out.pixels[static_cast<size_t>(y) * static_cast<size_t>(out.width) + static_cast<size_t>(x)] =
                (red << 16) | (green << 8) | blue;
        }
    }
    return true;
}

/* The three pins as an engine device: while Enable is 1 the screen stores Value
   in Setting index.  Nothing is driven back - a screen has no outputs - so this
   device exists to own the settings table, which the caller keeps a pointer to
   (the board builder does) so a report or a viewer can read what the screen
   would be showing.  An unknown Enable or an unknown setting index leaves the
   table alone, and an index outside it is counted. */
inline std::unique_ptr<DeviceSpec> screenSpec(const std::shared_ptr<ScreenSettings>& settings,
                                              ArcDelay delay = ArcDelay{},
                                              const std::string& name = "screen") {
    auto spec = std::make_unique<DeviceSpec>();
    spec->name = name;
    spec->kind = DeviceKind::kCombinational;
    spec->inputs = {PinSpec{"Enable", 1}, PinSpec{"Setting", 8}, PinSpec{"Value", 64}};
    spec->default_delay = delay;
    spec->behavior = [settings](const EvalContext&, const std::vector<BitVector>& inputs,
                                std::vector<BitVector>& outputs) {
        outputs.clear();
        if (inputs.size() < 3 || memoryEnable(inputs[0]) != Logic::kOne) return;
        if (!inputs[1].isKnown() || !inputs[2].isKnown()) return;
        const uint64_t index = inputs[1].toUint64();
        if (index < ScreenSettings::kCount) settings->setting[index] = inputs[2].toUint64();
        else if (settings->unlisted_index == ScreenSettings::kNoUnlisted) settings->unlisted_index = index;
    };
    return spec;
}

}  // namespace tcsim
