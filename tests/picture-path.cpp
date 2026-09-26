/* The loader's rule for recognising a Mod component's own picture request
   (src/picture_path.hpp): the game asks every frame for the texture
   "?snapshot_cc/com_custom_<decimal id>.png" for a custom prototype, and that
   is the only request V5 answers with a Mod's PNG.  Everything else - the
   built-in "?snapshot/<kind>.png" branch, sprites, menus, fonts and the game's
   own capture cache - has to be forwarded untouched, so both the positive and
   the near-miss forms are checked here. */

#include "../src/picture_path.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

static void require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}

static bool matches(const std::string& text, uint64_t* id = nullptr) {
    uint64_t parsed = 0;
    const bool found = tc::pictures::parseCustomSnapshot(text, id ? id : &parsed);
    return found;
}

static void accepted(const std::string& text, uint64_t expected, const char* what) {
    uint64_t id = 0;
    if (!matches(text, &id)) {
        std::fprintf(stderr, "FAIL %s: '%s' was not recognised\n", what, text.c_str());
        std::exit(1);
    }
    if (id != expected) {
        std::fprintf(stderr, "FAIL %s: '%s' gave id %llu, wanted %llu\n", what, text.c_str(),
                     static_cast<unsigned long long>(id),
                     static_cast<unsigned long long>(expected));
        std::exit(1);
    }
}

static void refused(const std::string& text, const char* what) {
    if (matches(text)) {
        std::fprintf(stderr, "FAIL %s: '%s' was recognised\n", what, text.c_str());
        std::exit(1);
    }
}

int main() {
    /* The form the palette asks for, as measured on a real board: the resolved
       path keeps the virtual "?" segment and mixes separators. */
    accepted("D:\\p\\game/asset/?snapshot_cc/com_custom_5058442071141929777.png",
             UINT64_C(5058442071141929777), "resolved request");
    accepted("D:\\p\\game\\asset\\?snapshot_cc/com_custom_5058442071141929777.png",
             UINT64_C(5058442071141929777), "resolved request, backslashes");
    accepted("?snapshot_cc/com_custom_7.png", 7, "virtual request");
    accepted("?snapshot_cc\\com_custom_0.png", 0, "zero id");
    accepted("snapshot_cc/com_custom_18446744073709551615.png", UINT64_MAX,
             "largest 64-bit id");

    /* Near misses: each one is a request the loader must forward unchanged. */
    refused("?snapshot/com_custom_7.png", "built-in snapshot branch");
    refused("?snapshot_cc/com_custom_.png", "no digits");
    refused("?snapshot_cc/com_custom_7.png.recode", "trailing bytes after .png");
    refused("?snapshot_cc/com_custom_7.PNG", "upper-case extension");
    refused("?snapshot_ccx/com_custom_7.png", "marker without a separator");
    refused("xsnapshot_cc/com_custom_7.png", "marker inside another word");
    refused("?snapshot_cc/com_customx7.png", "tag without its separator");
    refused("component_sprites/com_custom_7.png", "no snapshot marker at all");
    refused("?snapshot_cc/com_custom_18446744073709551616.png", "id wider than 64 bits");
    refused("?snapshot_cc/com_custom_7.png ", "trailing space");
    refused("", "empty");

    /* A request is a borrowed Nim string; the parser must not need a NUL. */
    {
        const std::string padded = "?snapshot_cc/com_custom_42.pngXXXX";
        /* "?snapshot_cc/com_custom_42.png" is 30 bytes exactly. */
        uint64_t id = 0;
        require(tc::pictures::parseCustomSnapshot(padded.data(), 30, &id) && id == 42,
                "length-bounded parse");
        require(!tc::pictures::parseCustomSnapshot(padded.data(), padded.size(), &id),
                "bytes past the length are not part of the path");
    }

    std::printf("PASS picture path rule (custom snapshot requests and their near misses)\n");
    return 0;
}
