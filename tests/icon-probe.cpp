/* Development probe: what the game draws as a component's *icon*.

   The right-hand component column, the drawer's preview picture and the foundry
   all draw a component from one texture, and the path of that texture is built
   by the game itself:

     * `get_captured_path__presenterZio_u28(out, kind, value, flag)` is the
       dispatcher the UI asks: a custom kind (0x4e) takes the custom branch, every
       other kind the built-in one;
     * `create_texture_unsafe__presenterZrendererZtextureZtexture_u364` (and the
       checked wrapper `create_texture__u1978`) receives the *resolved* file path.

   This probe hooks both, read-only: every call is forwarded unchanged and only
   logged, so the log says exactly which file the game would show for which
   component.  It also clicks the palette's own `自定义` tab once the board is up,
   because that is the page the custom components live on (the icon is only asked
   for while the page is drawn), and writes everything to `icons.txt` in the
   plugin's data directory.

   Environment:
     TC_ICON_PROBE_TAB   "0" disables the palette tab click (default: click)
     TC_ICON_PROBE_X/Y   the tab's client coordinates (default: 0.9625/0.3225 of
                         the client size, measured on the 1200x800 sandbox)
     TC_ICON_PROBE_HOLD  seconds to keep probing after the click (default 6)
*/
#include "../sdk/tc_mod.h"
#include "../sdk/tc_game_model.h"
#include "../sdk/tc_component_model.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace {

const TCHost* host = nullptr;
bool started = false, clicked = false, finished = false;
int captured = 0;
bool body_clicked = false, body_clicked_ok = false;
double body_retry = 0.0, body_time = 0.0;
double start_time = 0.0, click_time = 0.0;
double hold = 6.0;
bool click_tab = true;
float tab_fx = 0.9625f, tab_fy = 0.3225f;
/* The custom page lists the Mod's folder first; a second click on the row under
   the tab strip expands it, which is what makes the items (and their icons) draw. */
float row_fy = 0.40f;
/* The palette hangs off the right edge, so the click is measured from there:
   `-45` px from the right is the middle of a tab at the sizes the game is run at
   here, and the rows are at fixed distances below the top.  Set
   TC_ICON_PROBE_X/Y (and _R) to override in absolute and right-relative pixels. */
int tab_y = 258, row_y = 320, right_offset = 45;
bool absolute_click = false;
int palette_y = 470;
int palette_step = 0;
bool paint_design = false, painted = false;
uint64_t paint_id = UINT64_C(0x463332434f4e5331);
uint8_t paint_cell = 0x51;
/* TC_ICON_PROBE_SUBSTITUTE=<png path> + TC_ICON_PROBE_MATCH=<substring of the
   requested path>: the takeover spike described in hookCreateTextureUnsafe. */
std::string substitute, substitute_match = "snapshot_cc";
/* TC_ICON_PROBE_STAGES=1 + TC_ICON_PROBE_TAG=<name>: the three-surface flow the
   component-picture case drives (tests/picture-playtest.ps1) - it photographs the
   picture where a player sees it: the component column's item, the bottom
   drawer's preview and the ghost that follows the cursor while a component is
   being placed.  The research flow above stays untouched when this is off. */
bool stages = false;
std::string stage_tag = "run";
int stage_capture = 0;
double stage_start = -1.0;
int stage_step = 0;
int stage_placed = 0;
int stage_item_x = -1, stage_item_y = -1;
int stage_drag_x = 0, stage_drag_y = 0;   /* 0 = the flow's own default */
int logged = 0;
std::vector<std::string> lines;
std::string dump_path;
/* The game's own catalog thumbnails come from the prototype's shape string
   (docs/PLAN-float-components.md: "shape_svg = 元件目录缩略图"), so the probe
   dumps a few built-in ones to read the format off a working example. */
tc::TCGameModel game;

void log(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
}

/* Everything goes to icons.txt; loader.log only carries the lines that are about
   component pictures, because the game asks for a texture for every menu logo,
   glyph atlas and board sprite as well. */
void record(const std::string& message) {
    if (message.find("com_") != std::string::npos ||
        message.find("capture") != std::string::npos ||
        message.find("snapshot") != std::string::npos ||
        message.find("ICON-PROBE") != std::string::npos) {
        log(message);
    }
    if (lines.size() < 20000) lines.push_back(message);
}

/* Nim passes a `string` parameter as a pointer to {int64 length; char* data} and
   the characters start eight bytes into the payload (the capacity word comes
   first), which is the same convention the loader's own dumps use. */
std::string nimString(const void* pointer) {
    if (!pointer) return std::string();
    int64_t length = 0;
    char* data = nullptr;
    std::memcpy(&length, pointer, sizeof(length));
    std::memcpy(&data, static_cast<const unsigned char*>(pointer) + 8, sizeof(data));
    if (!data || length <= 0 || length > 4096) return std::string();
    return std::string(data + 8, static_cast<size_t>(length));
}

/* ---- the two hooks ------------------------------------------------------- */

/* out: var string, kind: uint8 (0x4e = a custom prototype), value: the id the
   string is built from, flag: the "second form" the built-in branch takes. */
using CapturedPathFn = void (*)(void* out, uint8_t kind, uint64_t value, uint8_t flag);
CapturedPathFn capturedPathOriginal = nullptr;

void hookCapturedPath(void* out, uint8_t kind, uint64_t value, uint8_t flag) {
    if (capturedPathOriginal) capturedPathOriginal(out, kind, value, flag);
    if (logged > 20000) return;
    ++logged;
    char line[256] = {};
    std::snprintf(line, sizeof(line),
                  "ICON-PROBE: captured path asked kind=0x%02x value=%llu (0x%llx) flag=%u",
                  kind, static_cast<unsigned long long>(value),
                  static_cast<unsigned long long>(value), static_cast<unsigned>(flag));
    record(line);
}

/* The texture factory: rcx = out texture, rdx = Nim string (by pointer).  The
   remaining arguments are forwarded untouched, which is what keeps the hook
   read-only. */
using CreateTextureUnsafeFn = uint64_t (*)(void* out, const void* path, void* a3, void* a4,
                                          uint64_t a5);
CreateTextureUnsafeFn createTextureUnsafeOriginal = nullptr;

uint64_t hookCreateTextureUnsafe(void* out, const void* path, void* a3, void* a4, uint64_t a5) {
    const std::string text = nimString(path);
    if (!text.empty() && logged <= 20000) {
        ++logged;
        char line[512] = {};
        std::snprintf(line, sizeof(line), "ICON-PROBE: texture request (unsafe) \"%s\"",
                      text.c_str());
        record(line);
    }
    /* Takeover spike: when the game asks for one of our components' snapshot,
       hand the *original* factory a path of our own instead of synthesising its
       texture struct.  The game then creates/owns the texture exactly as it does
       at startup - it simply reads a different file. */
    if (!substitute.empty() && text.find(substitute_match) != std::string::npos &&
        createTextureUnsafeOriginal) {
        std::vector<char> payload(8 + substitute.size() + 1, 0);
        const uint64_t capacity = uint64_t(substitute.size()) | (uint64_t(1) << 62);
        std::memcpy(payload.data(), &capacity, sizeof(capacity));
        std::memcpy(payload.data() + 8, substitute.data(), substitute.size());
        struct NimString { int64_t length; char* data; } substituted{
            static_cast<int64_t>(substitute.size()), payload.data()};
        static bool told = false;
        if (!told) {
            told = true;
            record("ICON-PROBE: substituting the picture for \"" + text + "\" with \"" +
                   substitute + "\"");
        }
        return createTextureUnsafeOriginal(out, &substituted, a3, a4, a5);
    }
    if (!text.empty() && logged <= 20000) {
        ++logged;
        char line[512] = {};
        std::snprintf(line, sizeof(line), "ICON-PROBE: texture request (unsafe) \"%s\"",
                      text.c_str());
        record(line);
    }
    return createTextureUnsafeOriginal
               ? createTextureUnsafeOriginal(out, path, a3, a4, a5)
               : 0;
}

/* The checked wrapper resolves the virtual path ("?snapshot_cc/…") into the real
   file path and then calls the factory above, so logging both shows the rule end
   to end: virtual name in, absolute file out. */
using CreateTextureFn = uint64_t (*)(void* out, const void* path, void* a3, void* a4);
CreateTextureFn createTextureOriginal = nullptr;

uint64_t hookCreateTexture(void* out, const void* path, void* a3, void* a4) {
    const std::string text = nimString(path);
    if (!text.empty() && logged <= 20000) {
        ++logged;
        char line[512] = {};
        std::snprintf(line, sizeof(line), "ICON-PROBE: texture request \"%s\"", text.c_str());
        record(line);
    }
    return createTextureOriginal ? createTextureOriginal(out, path, a3, a4) : 0;
}

/* ---- the palette tab click ---------------------------------------------- */

BOOL CALLBACK findWindowCallback(HWND candidate, LPARAM data) {
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

HWND findWindow() {
    HWND found = nullptr;
    EnumWindows(findWindowCallback, reinterpret_cast<LPARAM>(&found));
    return found;
}

/* The game only consumes synthetic mouse input while its window is the
   foreground one.  Measured: a run whose window did not come forward logged every
   pointer move and then released the button at the *previous* position -
   SetCursorPos had silently done nothing, and the placement never started (the
   stage flow still photographed its frames, so the case came back red for what
   looked like a loader fault).  Windows throttles SetForegroundWindow for a
   background process, so ask, then nudge with a restore + top-most re-show. */
void focusWindow(HWND window) {
    if (!window) return;
    if (GetForegroundWindow() == window) return;
    SetForegroundWindow(window);
    if (GetForegroundWindow() == window) return;
    ShowWindow(window, SW_RESTORE);
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window);
}

/* Put the system cursor at a client position, and check that it moved.
   SetCursorPos silently does nothing while another window holds the foreground,
   and every later step then acts on stale coordinates: measured, the placement
   stage logged its moves and then released the button at the previous position,
   which read in the log like the loader had lost a placement.  Retry with the
   focus forced, and say so in the log when even that fails, so a probe problem
   can never be mistaken for a takeover problem again. */
bool placeCursor(HWND window, int x, int y) {
    POINT screen{x, y};
    ClientToScreen(window, &screen);
    for (int attempt = 0; attempt < 3; ++attempt) {
        SetCursorPos(screen.x, screen.y);
        POINT now{};
        if (GetCursorPos(&now) && now.x == screen.x && now.y == screen.y) return true;
        focusWindow(window);
        Sleep(60);
    }
    record("ICON-PROBE: could not put the cursor at client " + std::to_string(x) + "," +
           std::to_string(y) + " (the game window would not take focus)");
    return false;
}

void clickClient(float fx, float fy, const char* what) {
    HWND window = findWindow();
    if (!window) {
        record(std::string("ICON-PROBE: no game window to click (") + what + ")");
        return;
    }
    RECT client{};
    GetClientRect(window, &client);
    const int x = absolute_click ? client.right - right_offset
                                 : static_cast<int>(client.right * fx);
    const int y = absolute_click ? static_cast<int>(fy) : static_cast<int>(client.bottom * fy);
    POINT screen{x, y};
    ClientToScreen(window, &screen);
    if (!placeCursor(window, x, y)) return;
    const LPARAM point = MAKELPARAM(x, y);
    PostMessageW(window, WM_MOUSEMOVE, 0, point);
    Sleep(40);
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
    Sleep(60);
    PostMessageW(window, WM_LBUTTONUP, 0, point);
    char line[160] = {};
    std::snprintf(line, sizeof(line),
                  "ICON-PROBE: clicked %s at client %d,%d (client %ldx%ld)", what, x, y,
                  client.right, client.bottom);
    record(line);
}

/* Hover only: the palette's levels (category tab, sub-category flyout, then the
   item list) open on hover, so a move with no button is what reveals them. */
void hoverClient(int right, int y, const char* what) {
    HWND window = findWindow();
    if (!window) return;
    RECT client{};
    GetClientRect(window, &client);
    const int x = client.right - right;
    if (!placeCursor(window, x, y)) return;
    const LPARAM point = MAKELPARAM(x, y);
    PostMessageW(window, WM_MOUSEMOVE, 0, point);
    char line[192] = {};
    std::snprintf(line, sizeof(line), "ICON-PROBE: hovering %s at client %d,%d (right-%d)",
                  what, x, y, right);
    record(line);
}

/* The drawer's preview picture is the same component picture the palette item
   draws, and opening the drawer only needs a click on the component's body.  The
   Float Ops Mod publishes every body's rectangle (TC_FLOATOPS_BODIES=1), which is
   what the panel playtest clicks; the probe reads the same file. */
bool clickFirstBody() {
    if (!host || !host->data_directory_utf8) return false;
    std::string directory = host->data_directory_utf8;
    const size_t cut = directory.find_last_of("\\/");
    if (cut == std::string::npos) return false;
    directory.resize(cut + 1);
    const std::string path = directory + "local.float-ops\\bodies.txt";
    std::ifstream file(path);
    if (!file) {
        record("ICON-PROBE: no bodies.txt yet (" + path + ")");
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        unsigned long long id = 0;
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        if (std::sscanf(line.c_str(), "body 0x%llx %f %f %f %f", &id, &x0, &y0, &x1, &y1) != 5)
            continue;
        if (!(x1 > x0 && y1 > y0)) continue;
        HWND window = findWindow();
        if (!window) return false;
        const int x = static_cast<int>((x0 + x1) * 0.5f);
        const int y = static_cast<int>((y0 + y1) * 0.5f);
        if (!placeCursor(window, x, y)) return false;
        const LPARAM point = MAKELPARAM(x, y);
        PostMessageW(window, WM_MOUSEMOVE, 0, point);
        Sleep(40);
        PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
        Sleep(60);
        PostMessageW(window, WM_LBUTTONUP, 0, point);
        char text[192] = {};
        std::snprintf(text, sizeof(text),
                      "ICON-PROBE: clicked the component body 0x%llx at %d,%d", id, x, y);
        record(text);
        return true;
    }
    record("ICON-PROBE: bodies.txt carried no usable rectangle");
    return false;
}

void writeDump() {
    if (!host || !host->data_directory_utf8) return;
    char path[512] = {};
    std::snprintf(path, sizeof(path), "%s\\icons.txt", host->data_directory_utf8);
    std::FILE* file = std::fopen(path, "wb");
    if (!file) return;
    for (const std::string& line : lines) {
        std::fprintf(file, "%s\n", line.c_str());
    }
    std::fclose(file);
}

void dumpBuiltinShapes() {
    struct Sample {
        uint8_t kind;
        const char* name;
    };
    const Sample samples[] = {
        {0x03, "NOT bit"},   {0x0b, "ADD byte?"}, {0x5a, "Static Value"},
        {0x0f, "AND bit?"},  {0x4f, "Input Pin"}, {0x51, "Output Pin"},
    };
    for (const Sample& sample : samples) {
        const char* shape = game.builtinPrototypeShapeSvg(sample.kind);
        char line[600] = {};
        std::snprintf(line, sizeof(line), "ICON-PROBE: builtin kind=0x%02x (%s) shape=\"%s\"",
                      sample.kind, sample.name, shape ? shape : "(null)");
        record(line);
    }
}

/* ---- route B spike: replace the picture the game computed for a type -------

   A custom prototype's "design picture" is the 32x32, four-bit-per-cell image the
   game computes from the prototype's own circuit (`update_custom_design`).  It is
   what the palette card, the drawer's preview picture and the placement ghost all
   draw.  The buffer is a Nim seq: 64 64-bit words (each holding 16 cells, two rows
   per word) behind an eight-byte header - the header is exactly what the earlier
   raw-offset surgery destroyed, which is why that crashed.  This spike writes only
   the cells and re-reads them, to see the picture change without touching the
   header.  Enabled with TC_ICON_PROBE_DESIGN=<hex kind...>. */
bool paintDesign(uint64_t id, uint8_t cell) {
    tc::TCPrototype prototype{};
    if (!game.getCustomPrototype(id, prototype)) {
        record("ICON-PROBE: design: no such custom prototype");
        return false;
    }
    void* buffer = nullptr;
    std::memcpy(&buffer, prototype.bytes + 0x570, sizeof(buffer));
    if (!buffer) {
        record("ICON-PROBE: design: the prototype has no design buffer yet");
        return false;
    }
    uint64_t header = 0;
    std::memcpy(&header, buffer, sizeof(header));
    uint64_t word = 0;
    for (int cell_index = 0; cell_index < 16; ++cell_index) word = (word << 4) | (cell & 0xf);
    for (int index = 0; index < 64; ++index)
        std::memcpy(static_cast<unsigned char*>(buffer) + 8 + index * 8, &word, 8);
    char line[192] = {};
    std::snprintf(line, sizeof(line),
                  "ICON-PROBE: design: id=0x%llx buffer=%p header=%llu cell=0x%02x cells written",
                  static_cast<unsigned long long>(id), buffer,
                  static_cast<unsigned long long>(header), static_cast<unsigned>(cell));
    record(line);
    /* The clone is a real copy: nothing changes until it goes back through the
       game's own setter, which is the round trip the loader's importer uses.  The
       header above is left untouched - that is what the earlier raw surgery
       destroyed. */
    tc::TCComponentModel components;
    const bool model = components.load(host);
    const bool stored = game.setCustomPrototype(id, prototype);
    const bool released = model
                              ? components.releasePrototype(prototype) ==
                                    tc::TCComponentStatus::Ok
                                : false;
    char result[192] = {};
    std::snprintf(result, sizeof(result),
                  "ICON-PROBE: design: set=%s release=%s (model=%s)", stored ? "ok" : "refused",
                  released ? "ok" : "skipped", model ? "ok" : "missing");
    record(result);
    return stored;
}

/* ---- the picture -------------------------------------------------------- */

/* The loader's own TC_MODLOADER_SHOT did not fire in this configuration (an
   outside capture of a fullscreen independent-flip window is black anyway), so
   the probe reads the GL backbuffer itself: the frame callback already runs on
   the render thread.  A 32-bit bottom-up BMP, the same shape the loader writes. */
using GetIntegervFn = void (*)(unsigned, int*);
using ReadPixelsFn = void (*)(int, int, int, int, unsigned, unsigned, void*);
using PixelStoreFn = void (*)(unsigned, int);
GetIntegervFn getIntegerv = nullptr;
ReadPixelsFn readPixels = nullptr;
PixelStoreFn pixelStore = nullptr;

void loadGl() {
    HMODULE module = LoadLibraryA("opengl32.dll");
    if (!module) return;
    /* GetProcAddress returns FARPROC; copying through a void* keeps the
       -Wcast-function-type build honest (the loader resolves its engine exports
       the same way). */
    void* address = reinterpret_cast<void*>(GetProcAddress(module, "glGetIntegerv"));
    std::memcpy(&getIntegerv, &address, sizeof(getIntegerv));
    address = reinterpret_cast<void*>(GetProcAddress(module, "glReadPixels"));
    std::memcpy(&readPixels, &address, sizeof(readPixels));
    address = reinterpret_cast<void*>(GetProcAddress(module, "glPixelStorei"));
    std::memcpy(&pixelStore, &address, sizeof(pixelStore));
}

void writeLe32(unsigned char* out, unsigned value) {
    out[0] = static_cast<unsigned char>(value & 0xff);
    out[1] = static_cast<unsigned char>((value >> 8) & 0xff);
    out[2] = static_cast<unsigned char>((value >> 16) & 0xff);
    out[3] = static_cast<unsigned char>((value >> 24) & 0xff);
}

/* Where the component column drew the last item: the strip of items is drawn
   left of the palette, and the *pictures* in it are the Mod's (for this case they
   are deliberately garish), so the right-most magenta run in the frame is the item
   the placement stage has to pick up.  Returning it from the frame itself keeps
   the case free of a guessed palette geometry - the game's own layout decides
   where the items are, whatever the window size. */
void analyseItem(const std::vector<unsigned char>& pixels, int width, int height,
                 int* itemX, int* itemY) {
    const unsigned char* data = pixels.data();
    int maxX = -1, sumX = 0, sumY = 0, count = 0;
    for (int y = 0; y < height; ++y) {
        const unsigned char* row = data + static_cast<size_t>(y) * width * 4u;
        for (int x = 0; x < width; ++x) {
            const unsigned char* pixel = row + static_cast<size_t>(x) * 4u;
            if (pixel[0] > 200 && pixel[1] < 60 && pixel[2] > 200 && maxX < x) maxX = x;
        }
    }
    if (maxX < 0) return;
    for (int y = 0; y < height; ++y) {
        const unsigned char* row = data + static_cast<size_t>(y) * width * 4u;
        for (int x = maxX - 120 > 0 ? maxX - 120 : 0; x <= maxX; ++x) {
            const unsigned char* pixel = row + static_cast<size_t>(x) * 4u;
            if (!(pixel[0] > 200 && pixel[1] < 60 && pixel[2] > 200)) continue;
            sumX += x;
            /* The GL rows come back bottom-up; client coordinates are top-down. */
            sumY += height - 1 - y;
            ++count;
        }
    }
    if (!count) return;
    *itemX = sumX / count;
    *itemY = sumY / count;
}

bool captureFrame(const char* path, int* itemX, int* itemY) {
    if (!getIntegerv || !readPixels || !pixelStore || !path) return false;
    int viewport[4] = {};
    getIntegerv(0x0ba2 /* GL_VIEWPORT */, viewport);
    const int width = viewport[2], height = viewport[3];
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    pixelStore(0x0d05 /* GL_PACK_ALIGNMENT */, 1);
    readPixels(viewport[0], viewport[1], width, height, 0x1908 /* GL_RGBA */,
               0x1401 /* GL_UNSIGNED_BYTE */, pixels.data());
    if (itemX && itemY) {
        *itemX = -1;
        *itemY = -1;
        analyseItem(pixels, width, height, itemX, itemY);
    }
    std::FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    const unsigned pixelBytes = static_cast<unsigned>(width) * height * 4u;
    unsigned char header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    writeLe32(header + 2, 54u + pixelBytes);
    writeLe32(header + 10, 54u);
    writeLe32(header + 14, 40u);
    writeLe32(header + 18, static_cast<unsigned>(width));
    writeLe32(header + 22, static_cast<unsigned>(-height));
    header[26] = 1; header[28] = 32;
    writeLe32(header + 34, pixelBytes);
    std::fwrite(header, 1, sizeof(header), file);
    std::vector<unsigned char> row(static_cast<size_t>(width) * 4u);
    for (int y = 0; y < height; ++y) {
        const unsigned char* source =
            pixels.data() + static_cast<size_t>(height - 1 - y) * width * 4u;
        for (int x = 0; x < width; ++x) {
            row[static_cast<size_t>(x) * 4 + 0] = source[static_cast<size_t>(x) * 4 + 2];
            row[static_cast<size_t>(x) * 4 + 1] = source[static_cast<size_t>(x) * 4 + 1];
            row[static_cast<size_t>(x) * 4 + 2] = source[static_cast<size_t>(x) * 4 + 0];
            row[static_cast<size_t>(x) * 4 + 3] = 255;
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
    return true;
}

bool captureFrame(const char* path) {
    return captureFrame(path, nullptr, nullptr);
}

void capturePicture(int index) {
    if (!host || !host->data_directory_utf8) return;
    char path[512] = {};
    std::snprintf(path, sizeof(path), "%s\\palette-%d.bmp", host->data_directory_utf8, index);
    char line[256] = {};
    std::snprintf(line, sizeof(line), "ICON-PROBE: %s frame #%d",
                  captureFrame(path) ? "captured" : "could not capture", index);
    record(line);
}

/* ---- the three-surface flow (tests/picture-playtest.ps1) ---------------- */

void moveClient(int x, int y, const char* what) {
    HWND window = findWindow();
    if (!window) return;
    RECT client{};
    GetClientRect(window, &client);
    if (!placeCursor(window, x, y)) return;
    PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    char line[224] = {};
    std::snprintf(line, sizeof(line),
                  "ICON-PROBE: stage %s: pointer at client %d,%d (client %ldx%ld)",
                  what, x, y, client.right, client.bottom);
    record(line);
}

/* Press and stay down: what the ghost stage needs, because the component is
   dragged out of the list. */
void holdClient(int x, int y, const char* what) {
    HWND window = findWindow();
    if (!window) return;
    if (!placeCursor(window, x, y)) return;
    PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    Sleep(40);
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    char line[224] = {};
    std::snprintf(line, sizeof(line), "ICON-PROBE: stage %s: pressed and held at %d,%d",
                  what, x, y);
    record(line);
}

void releaseClient() {
    HWND window = findWindow();
    if (!window) return;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    ScreenToClient(window, &cursor);
    if (!placeCursor(window, cursor.x, cursor.y)) return;
    PostMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(cursor.x, cursor.y));
    Sleep(30);
    PostMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(cursor.x, cursor.y));
    char line[192] = {};
    std::snprintf(line, sizeof(line), "ICON-PROBE: stage released the drag at client %ld,%ld",
                  cursor.x, cursor.y);
    record(line);
}

/* A click at an explicit client position - the flow's own "empty board space"
   step, which must not go through the palette-relative placement clickClient
   applies (that one lands on the category strip at this height). */
/* Letting go at an explicit client position.  releaseClient() reads the system
   cursor back, which is wrong for the placement stage twice over: the drop cell
   has to be a place the case can measure (a cell of the case's own choosing, not
   wherever the cursor happens to have been left), and SetCursorPos is the first
   thing to go missing when another window steals the foreground - measured, that
   put a release at the previous move's coordinates instead. */
void releaseAt(int x, int y, const char* what) {
    HWND window = findWindow();
    if (!window) return;
    if (!placeCursor(window, x, y)) return;
    PostMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
    Sleep(30);
    PostMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    char line[224] = {};
    std::snprintf(line, sizeof(line), "ICON-PROBE: stage %s: released at client %d,%d",
                  what, x, y);
    record(line);
}

void clickAt(int x, int y, const char* what) {
    HWND window = findWindow();
    if (!window) return;
    if (!placeCursor(window, x, y)) return;
    PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    Sleep(40);
    PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    Sleep(60);
    PostMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    char line[224] = {};
    std::snprintf(line, sizeof(line), "ICON-PROBE: stage %s: clicked client %d,%d", what, x, y);
    record(line);
}

void captureStage(const char* stage) {
    if (!host || !host->data_directory_utf8) return;
    char path[512] = {};
    std::snprintf(path, sizeof(path), "%s\\stage-%s-%s-%d.bmp",
                  host->data_directory_utf8, stage_tag.c_str(), stage, ++stage_capture);
    /* Every picture of the component column also says where the items are, so the
       placement stage can pick one up without a guessed palette geometry. */
    int itemX = -1, itemY = -1;
    const bool ok = captureFrame(path, &itemX, &itemY);
    /* Drawer previews use the same marker picture, but they are not palette
       items.  A late drawer capture must not replace the component-column
       coordinate (or its deliberate fallback) with a point at the bottom of
       the window. */
    if (std::strcmp(stage, "card") == 0 && itemX > 0 && itemY > 0) {
        stage_item_x = itemX;
        stage_item_y = itemY;
    }
    char line[512] = {};
    std::snprintf(line, sizeof(line),
                  "ICON-PROBE: stage %s frame %d %s \"%s\"%s", stage, stage_capture,
                  ok ? "saved" : "FAILED", path,
                  (itemX > 0 && itemY > 0)
                      ? (std::string(" item=") + std::to_string(itemX) + "," +
                         std::to_string(itemY)).c_str()
                      : "");
    record(line);
}

/* One step per frame, keyed on the elapsed time since the flow armed.  Each step
   runs *between* two of the game's own frames - the palette's flyouts open over
   frames and their hover state is consumed by the game's frame loop, so a step
   that slept inside one callback would leave the list undrawn (measured: the
   tooltip naming the item appeared, the list itself never did).

   The palette hangs off the right edge: the category tab is right-45 px in (the
   player's own window size, measured), and every level below it is a *hover* that
   opens one flyout further left, so the component list opens while the pointer
   rests at right-420. */
void stagedFlow(double elapsed) {
    if (stage_start < 0.0) {
        stage_start = elapsed;
        record("ICON-PROBE: stage flow armed at " + std::to_string(elapsed) + "s (tag \"" +
               stage_tag + "\")");
        return;
    }
    const double at = elapsed - stage_start;
    struct Step {
        double when;
        const char* what;
    };
    /* Nothing here presses Escape: in this build Escape leaves the level (measured:
       the stage after it photographed the main menu), and a click on empty board
       space is what clears the selection and closes the drawer.  The drag also
       ends back over the palette, so the component is dropped nowhere and the next
       run photographs the same board (a placement here would sit under the next
       run's ghost and be mistaken for it - measured, and the reason this comment
       exists). */
    static const Step steps[] = {
        /* The component column: the tab, then one hover per flyout level. */
        {0.0, "tab"}, {2.0, "card-1"}, {4.0, "card-2"}, {6.0, "card-3"}, {8.0, "card-4"},
        {10.0, "card-5"},
        /* The drawer: the pointer leaves the palette and clicks a component body. */
        {12.0, "away"}, {13.5, "body"}, {15.0, "drawer-1"}, {16.5, "drawer-2"},
        {18.0, "drawer-3"}, {19.5, "clear"},
        /* The placement ghost: the item is taken out of the list again and dragged
           over the board, where the component follows the pointer. */
        {21.0, "tab"}, {23.0, "list-1"}, {25.0, "list-2"}, {27.0, "list-3"},
        {28.6, "item-hover"}, {29.0, "item"}, {30.5, "move-1"}, {31.2, "move-2"},
        {32.0, "ghost-1"}, {33.0, "ghost-2"}, {34.0, "ghost-3"}, {35.0, "ghost-4"},
        {36.0, "release"}, {37.0, "after"},
        /* A real placement: the item is dragged out again and this time dropped
           ON the board.  That is the path the player reported ("the ghost stays
           after I place a part"): the game consumes the record without calling
           hide_clipboard, so the preview has to end off the record itself.  The
           drop cell is far from both the cancelled drag's cell and the fixture,
           so neither this run's earlier boxes nor (the sandbox profile is shared)
           the next run's can pick the placed part up. */
        {39.0, "list-4"}, {41.0, "list-5"}, {43.0, "list-6"}, {44.1, "item-2-hover"},
        {44.5, "item-2"},
        {46.0, "lift-2"}, {47.5, "over-2"}, {49.0, "place-2"}, {50.0, "placed"},
        {51.5, "park-2"}, {53.0, "settled"}, {54.0, "done"},
    };
    const int stepCount = static_cast<int>(sizeof(steps) / sizeof(steps[0]));
    if (stage_step >= stepCount || at < steps[stage_step].when) return;
    const std::string what = steps[stage_step].what;
    ++stage_step;
    HWND window = findWindow();
    RECT client{};
    if (window) GetClientRect(window, &client);
    const int width = client.right, height = client.bottom;
    if (what == "tab") {
        clickClient(tab_fx, static_cast<float>(palette_y), "the palette category tab");
    } else if (what == "card-1") {
        /* The Mod's 浮点 folder is the button immediately left of 自定义.
           Click its centre: hovering the old border coordinate was timing- and
           DPI-sensitive in the real game. */
        clickAt(width - right_offset - 90, palette_y, "the Float Ops folder");
    } else if (what == "card-2") {
        hoverClient(right_offset + 195, palette_y + 86, "the component list");
    } else if (what == "card-3") {
        hoverClient(right_offset + 195, palette_y + 86, "the component list");
    } else if (what == "card-4" || what == "card-5") {
        captureStage("card");
    } else if (what == "away") {
        moveClient(width / 2, height / 2, "close-palette");
    } else if (what == "body") {
        clickFirstBody();
    } else if (what == "drawer-1" || what == "drawer-2" || what == "drawer-3") {
        captureStage("drawer");
    } else if (what == "clear") {
        /* Well away from the board's parts and from the category strip (a click on
           the strip's own M0 tab loads the foundry, measured). */
        clickAt(width / 8, height * 3 / 4, "empty board space");
    } else if (what == "list-1") {
        clickAt(width - right_offset - 90, palette_y, "the Float Ops folder");
    } else if (what == "list-2") {
        hoverClient(right_offset + 195, palette_y + 86, "the component list");
    } else if (what == "list-3") {
        hoverClient(right_offset + 195, palette_y + 86, "the component list");
    } else if (what == "item") {
        /* Taking the item out of the list is what starts the placement - pressed
           and held, so the drag below is a drag for either of the game's two
           placement styles (pick-then-click or press-and-drag).  The point comes
           from the picture the component column just drew (see analyseItem); the
           palette-relative guess is only the fallback. */
        const int x = stage_item_x > 0 ? stage_item_x : width - right_offset - 194;
        const int y = stage_item_y > 0 ? stage_item_y : palette_y + 86;
        holdClient(x, y, "the component list item");
        stage_placed = 1;
    } else if (what == "item-hover" || what == "item-2-hover") {
        /* Rest on the item on the frame before pressing it.  The palette's
           flyouts are hover-driven and the game closes them again once the
           pointer has been still for a while: measured, a press that came two
           seconds after the last hover sometimes landed on a closed list and
           started no placement at all (the case then went red for what looked
           like a loader fault). */
        const int x = stage_item_x > 0 ? stage_item_x : width - right_offset - 194;
        const int y = stage_item_y > 0 ? stage_item_y : palette_y + 86;
        moveClient(x, y, "resting on the component list item");
    } else if (what == "move-1") {
        moveClient(stage_drag_x ? stage_drag_x - 80 : width / 2 + 120,
                   stage_drag_y ? stage_drag_y - 90 : height / 2 + 60, "drag onto the board");
    } else if (what == "move-2") {
        moveClient(stage_drag_x ? stage_drag_x : width / 2 + 40,
                   stage_drag_y ? stage_drag_y : height / 2 + 150, "drag onto the board");
    } else if (what.find("ghost-") == 0) {
        captureStage("ghost");
    } else if (what == "after") {
        captureStage("after");
    } else if (what == "release") {
        /* Back over the palette before letting go: the component is dropped
           nowhere, so the board is left exactly as the flow found it and a second
           run photographs the same scene (a placement here would sit under the
           next run's ghost and be mistaken for it). */
        moveClient(width - 120, palette_y, "back to the palette");
        releaseClient();
    } else if (what == "done") {
        finished = true;
        writeDump();
        record("ICON-PROBE: stage flow done (tag \"" + stage_tag + "\", " +
               std::to_string(stage_capture) + " picture(s), placement started=" +
               std::to_string(stage_placed) + ")");
    } else if (what == "list-4") {
        clickAt(width - right_offset - 90, palette_y, "the Float Ops folder (second drag)");
    } else if (what == "list-5" || what == "list-6") {
        hoverClient(right_offset + 195, palette_y + 86, "the component list (second drag)");
    } else if (what == "item-2") {
        const int x = stage_item_x > 0 ? stage_item_x : width - right_offset - 194;
        const int y = stage_item_y > 0 ? stage_item_y : palette_y + 86;
        holdClient(x, y, "the component list item (second drag)");
        stage_placed = 2;
    } else if (what == "lift-2") {
        /* The same drag shape as the first pass: onto the board, then to the
           drop cell, then let go *over the board* - a real placement. */
        moveClient(width / 2, height / 2, "second component onto the board");
    } else if (what == "over-2") {
        moveClient(width / 4, height / 5, "second component onto the board");
    } else if (what == "place-2") {
        releaseAt(width / 4, height / 5, "the placement");
    } else if (what == "placed") {
        captureStage("placed");
    } else if (what == "park-2") {
        moveClient(width / 8, height / 4, "parked after the placement");
    } else if (what == "settled") {
        captureStage("settled");
    }
}

void frame(void*, const TCFrame* tick) {
    if (!tick) return;
    if (!started) {
        started = true;
        start_time = tick->time_seconds;
        loadGl();
        if (game.load(host)) dumpBuiltinShapes();
        if (absolute_click) tab_fy = static_cast<float>(tab_y);
        if (absolute_click && row_y) row_fy = static_cast<float>(row_y);
        char shot[512] = {};
        char delay[64] = {};
        GetEnvironmentVariableA("TC_MODLOADER_SHOT", shot, sizeof(shot));
        GetEnvironmentVariableA("TC_MODLOADER_SHOT_DELAY", delay, sizeof(delay));
        record(std::string("ICON-PROBE: armed (click=") + (click_tab ? "1" : "0") +
               ", shot=\"" + shot + "\", shot-delay=" + delay + ")");
    }
    const double elapsed = tick->time_seconds - start_time;
    /* The three-surface flow replaces the research flow completely when the case
       asks for it; it arms itself once the board has had time to come up. */
    if (stages) {
        if (elapsed > 20.0) stagedFlow(elapsed);
        return;
    }
    /* The board has to be up before the palette is drawn, and the sandbox takes
       a few seconds to settle: the click and the report are both timed. */
    /* First open the drawer on the board's own component: its preview picture is
       asked for the same custom-component texture as the palette item. */
    if (!body_clicked && elapsed > 12.0) {
        body_clicked = true;
        if (clickFirstBody()) {
            body_clicked_ok = true;
            body_time = elapsed;
        } else {
            body_retry = elapsed + 4.0;
        }
    } else if (body_clicked && body_retry > 0.0 && !body_clicked_ok && elapsed > body_retry) {
        if (clickFirstBody()) {
            body_clicked_ok = true;
            body_time = elapsed;
        } else {
            body_retry = elapsed + 4.0;
        }
    }
    /* Pictures while the drawer is open: its preview picture is the same
       custom-component texture, and it is on screen without touching the palette
       (which would take the selection away and close the drawer).  Several frames
       are written because a GL readback from this callback can lag the frame the
       game is presenting. */
    if (body_clicked_ok && captured < 8 && elapsed > body_time + 2.0 + captured * 1.5) {
        ++captured;
        capturePicture(captured);
    }
    /* The spike paints once, shortly after the board is up, so the next pictures
       show whether the game's own pictures followed it. */
    /* Painted before the first picture, so every frame below shows the result. */
    if (paint_design && !painted && body_clicked_ok && elapsed > body_time + 1.0) {
        painted = true;
        paintDesign(paint_id, paint_cell);
    }
    /* The palette: select the custom category, then walk its flyouts with the
       pointer, one level per step, photographing each level.  Every level below
       the tab is a *hover* - measured, a click on a category only selects it,
       while the sub-category and item lists appear while the pointer rests on
       them - so each step moves the cursor and waits for the next picture. */
    if (click_tab && palette_step < 4 && body_clicked_ok &&
        elapsed > body_time + 8.0 + palette_step * 2.0) {
        switch (palette_step) {
            case 0:
                clickClient(tab_fx, static_cast<float>(palette_y),
                            "the palette's custom category tab");
                break;
            case 1:
                hoverClient(right_offset + 125, palette_y, "the category's flyout row");
                break;
            case 2:
                hoverClient(right_offset + 250, palette_y, "the flyout's own flyout");
                break;
            default:
                hoverClient(right_offset + 375, palette_y, "one more level to the left");
                break;
        }
        ++palette_step;
        if (!clicked) {
            clicked = true;
            click_time = elapsed;
        }
        /* A picture of every level, taken just before the next step. */
        capturePicture(++captured);
    }
    if (finished) return;
    const double since = clicked ? elapsed - click_time : elapsed;
    if (clicked && since > hold) {
        finished = true;
        writeDump();
        record("ICON-PROBE: done, " + std::to_string(lines.size()) + " line(s) in icons.txt");
    } else if (!click_tab && elapsed > 30.0) {
        finished = true;
        writeDump();
        record("ICON-PROBE: done, " + std::to_string(lines.size()) + " line(s) in icons.txt");
    }
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!h->create_hook || !h->resolve_symbol) {
        log("ICON-PROBE: this loader cannot create hooks");
        return 2;
    }
    {
        char buffer[32] = {};
        if (GetEnvironmentVariableA("TC_ICON_PROBE_TAB", buffer, sizeof(buffer)) > 0 &&
            buffer[0] == '0')
            click_tab = false;
    }
    {
        char buffer[64] = {};
        if (GetEnvironmentVariableA("TC_ICON_PROBE_STAGES", buffer, sizeof(buffer)) > 0 &&
            buffer[0] != '0' && buffer[0] != '\0')
            stages = true;
        if (GetEnvironmentVariableA("TC_ICON_PROBE_TAG", buffer, sizeof(buffer)) > 0 &&
            buffer[0] != '\0')
            stage_tag = buffer;
    }
    {
        char buffer[32] = {};
        if (GetEnvironmentVariableA("TC_ICON_PROBE_X", buffer, sizeof(buffer)) > 0)
            tab_fx = static_cast<float>(std::atof(buffer));
        if (GetEnvironmentVariableA("TC_ICON_PROBE_Y", buffer, sizeof(buffer)) > 0)
            tab_fy = static_cast<float>(std::atof(buffer));
        if (GetEnvironmentVariableA("TC_ICON_PROBE_ABSOLUTE", buffer, sizeof(buffer)) > 0)
            absolute_click = buffer[0] != '0';
        if (GetEnvironmentVariableA("TC_ICON_PROBE_RIGHT", buffer, sizeof(buffer)) > 0)
            right_offset = std::atoi(buffer);
        if (GetEnvironmentVariableA("TC_ICON_PROBE_ROW_Y", buffer, sizeof(buffer)) > 0)
            row_y = std::atoi(buffer);
        if (GetEnvironmentVariableA("TC_ICON_PROBE_PALETTE_Y", buffer, sizeof(buffer)) > 0)
            palette_y = std::atoi(buffer);
        if (GetEnvironmentVariableA("TC_ICON_PROBE_DESIGN", buffer, sizeof(buffer)) > 0) {
            const unsigned long long id = std::strtoull(buffer, nullptr, 16);
            if (id) {
                paint_design = true;
                paint_id = id;
            }
        }
        if (GetEnvironmentVariableA("TC_ICON_PROBE_DESIGN_CELL", buffer, sizeof(buffer)) > 0)
            paint_cell = static_cast<uint8_t>(std::strtoul(buffer, nullptr, 16));
        {
            /* A full Windows path does not fit the 32-byte buffers above. */
            char path_buffer[1024] = {};
            if (GetEnvironmentVariableA("TC_ICON_PROBE_SUBSTITUTE", path_buffer,
                                        sizeof(path_buffer)) > 0)
                substitute = path_buffer;
        }
        if (GetEnvironmentVariableA("TC_ICON_PROBE_MATCH", buffer, sizeof(buffer)) > 0)
            substitute_match = buffer;
        if (GetEnvironmentVariableA("TC_ICON_PROBE_HOLD", buffer, sizeof(buffer)) > 0) {
            const double value = std::atof(buffer);
            if (value > 0.0 && value < 120.0) hold = value;
        }
        /* The placement stage normally takes the item's position from the picture
           the component column drew; these pin it to one tile, which is how a case
           (or a person) drags the same component twice. */
        if (GetEnvironmentVariableA("TC_ICON_PROBE_ITEM_X", buffer, sizeof(buffer)) > 0)
            stage_item_x = std::atoi(buffer);
        if (GetEnvironmentVariableA("TC_ICON_PROBE_ITEM_Y", buffer, sizeof(buffer)) > 0)
            stage_item_y = std::atoi(buffer);
        if (GetEnvironmentVariableA("TC_ICON_PROBE_DRAG_X", buffer, sizeof(buffer)) > 0)
            stage_drag_x = std::atoi(buffer);
        if (GetEnvironmentVariableA("TC_ICON_PROBE_DRAG_Y", buffer, sizeof(buffer)) > 0)
            stage_drag_y = std::atoi(buffer);
    }

    void* captured = h->resolve_symbol(h->context, "get_captured_path__presenterZio_u28");
    if (captured &&
        h->create_hook(h->context, captured, reinterpret_cast<void*>(hookCapturedPath),
                       reinterpret_cast<void**>(&capturedPathOriginal)) == 0) {
        record("ICON-PROBE: get_captured_path dispatcher hooked");
    } else {
        record("ICON-PROBE: get_captured_path is not hookable; icon paths stay unlogged");
    }

    void* create = h->resolve_symbol(
        h->context, "create_texture_unsafe__presenterZrendererZtextureZtexture_u364");
    if (create &&
        h->create_hook(h->context, create, reinterpret_cast<void*>(hookCreateTextureUnsafe),
                       reinterpret_cast<void**>(&createTextureUnsafeOriginal)) == 0) {
        record("ICON-PROBE: texture factory hooked");
    } else {
        record("ICON-PROBE: the texture factory is not hookable; paths stay unlogged");
    }

    void* checked = h->resolve_symbol(
        h->context, "create_texture__presenterZrendererZtextureZtexture_u1978");
    if (checked &&
        h->create_hook(h->context, checked, reinterpret_cast<void*>(hookCreateTexture),
                       reinterpret_cast<void**>(&createTextureOriginal)) == 0) {
        record("ICON-PROBE: the checked texture wrapper is hooked");
    } else {
        record("ICON-PROBE: the checked texture wrapper is not hookable");
    }

    plugin->user = nullptr;
    plugin->on_frame = frame;
    plugin->on_unload = nullptr;
    return 0;
}
