#pragma once
/* The rule that turns one of the game's own texture requests into a Mod
   component's custom id.

   The game draws a component's picture from a texture whose path it builds
   itself (docs/research/component-icons.md): the dispatcher
   get_captured_path__presenterZio_u28 sends a custom prototype (kind 0x4e) to
   "?snapshot_cc/com_custom_<decimal id>.png", and the renderer resolves that
   with get_asset_path into "<game>/asset/?snapshot_cc/com_custom_<id>.png".
   The same text therefore arrives either way round - virtual ("?…") or
   resolved ("…/asset/?…") - and with either separator, which is exactly what
   this parser accepts.

   Everything else must be forwarded untouched, so the match is deliberately
   narrow: the marker has to be preceded by a separator (or start the string),
   followed by a separator, then the literal "com_custom_", then at least one
   decimal digit, then ".png" at the very end.  A built-in component asks for
   "?snapshot/<kind name>.png" and never reaches this rule; the game's own
   sprite, menu, font and capture textures (component_sprites/com_off.png,
   the capture cache, …) do not contain the marker at all. */

#include <cstddef>
#include <cstdint>
#include <string>

namespace tc::pictures {

inline bool isSeparator(char value) {
    return value == '/' || value == '\\' || value == '?';
}

/* Parses `text` (exactly `length` bytes) and, on a match, stores the decimal id.
   Returns false for every other path, including one whose digits would not fit
   a 64-bit id - a malformed request is forwarded like any other texture. */
inline bool parseCustomSnapshot(const char* text, size_t length, uint64_t* customId) {
    if (!text || !customId || length < 20) return false;
    static constexpr char kMarker[] = "snapshot_cc";
    static constexpr size_t kMarkerLength = sizeof(kMarker) - 1;
    static constexpr char kTag[] = "com_custom_";
    static constexpr size_t kTagLength = sizeof(kTag) - 1;
    for (size_t at = 0; at + kMarkerLength <= length; ++at) {
        if (text[at] != 's') continue;
        if (length - at < kMarkerLength) break;
        bool marker = true;
        for (size_t index = 0; index < kMarkerLength; ++index)
            if (text[at + index] != kMarker[index]) { marker = false; break; }
        if (!marker) continue;
        /* "xsnapshot_cc/…" is not a request this rule knows: the marker always
           starts a path element (the resolved path has "?snapshot_cc/…"). */
        if (at != 0 && !isSeparator(text[at - 1])) continue;
        size_t index = at + kMarkerLength;
        if (index >= length || !isSeparator(text[index])) continue;
        ++index;
        if (length - index < kTagLength || std::string(text + index, kTagLength) != kTag)
            continue;
        index += kTagLength;
        const size_t digits = index;
        uint64_t value = 0;
        while (index < length && text[index] >= '0' && text[index] <= '9') {
            const uint64_t digit = static_cast<uint64_t>(text[index] - '0');
            if (value > (UINT64_MAX - digit) / 10) return false;
            value = value * 10 + digit;
            ++index;
        }
        if (index == digits) return false;
        if (length - index != 4 || text[index] != '.' || text[index + 1] != 'p' ||
            text[index + 2] != 'n' || text[index + 3] != 'g')
            return false;
        *customId = value;
        return true;
    }
    return false;
}

inline bool parseCustomSnapshot(const std::string& text, uint64_t* customId) {
    return parseCustomSnapshot(text.data(), text.size(), customId);
}

}  // namespace tc::pictures
