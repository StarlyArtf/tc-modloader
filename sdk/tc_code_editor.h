#ifndef TC_CODE_EDITOR_H
#define TC_CODE_EDITOR_H
/* A code editor widget for mod panels.

   Why it is built this way
   ------------------------
   The engine does not export a usable text editor to plugins (it has one for
   its own IDE window, but that widget keeps a single process-wide state and we
   must not fight the game for it).  What the engine *does* export is everything
   ImGui itself is made of, so this header keeps the engine's own
   `igInputTextMultiline` for editing - caret, selection, mouse drag, undo/redo,
   IME and the game's text-input plumbing all keep working - and paints the
   editor chrome on top of it with the window draw list:

     * a line-number gutter with its own background, right aligned digits, and
       the caret's line picked out;
     * Verilog colouring (keywords, system tasks, compiler directives, numbers
       with a width/radix base, strings, line and block comments) drawn exactly
       over the characters the input already drew;
     * Enter keeps the line's indentation, opens a level after `begin`/`case`/…
       and closes one before `end`/`endcase`/…  (implemented in the input
       callback, which is where ImGui expects a widget to do it).

   Everything is additive.  `loadCodeEditor(host)` resolves the optional entry
   points once; if this build does not export one of them the widget degrades to
   the plain multiline input instead of failing, and `codeEditorAvailable()`
   says which mode is in use.

   Verified ABI (this pinned build, `tc_game_engine.dll`)
   ------------------------------------------------------
   - `ImGuiInputTextCallbackData` offsets were read out of the engine's own
     `ImGuiInputTextCallbackData_InsertChars` / `_DeleteChars` / `_SelectAll`
     machine code and match the 1.92.6 layout exactly (72 bytes):
     EventFlag@8 Flags@12 UserData@16 Buf@40 BufTextLen@48 BufSize@52
     BufDirty@37 CursorPos@56 SelectionStart@60 SelectionEnd@64.
   - `ImDrawList_GetClipRectMin/Max` take the out pointer first, like the other
     ImVec2-returning entry points of this build.
   - `ImGuiInputTextCallbackData_InsertChars(self, pos, text, text_end)`.

   The callback refuses to touch anything until the event flag matches one of
   the flags this widget itself requested, and until the fields pass a range
   check plus `strlen(Buf) == BufTextLen`; a build with a different layout
   therefore keeps the plain editor instead of corrupting its buffer. */

#include "tc_ui.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tc {
namespace ui {

/* ImGui packed colour (ABGR order, as the draw list expects). */
using CodeColor = std::uint32_t;
inline constexpr CodeColor codeRgba(unsigned char r, unsigned char g, unsigned char b,
                                    unsigned char a = 255) {
    return CodeColor(r) | (CodeColor(g) << 8) | (CodeColor(b) << 16) | (CodeColor(a) << 24);
}

struct CodeEditorOptions {
    bool lineNumbers = true;
    bool highlight = true;
    bool autoIndent = true;
    bool currentLine = true;
    bool indentGuides = true;
    /* Columns added after an opening keyword and removed before a closing one. */
    int indentWidth = 4;
    /* Room kept below the last line so the caret is not glued to the bottom. */
    float extraBottom = 18.f;
    /* Gap between the gutter digits and the first text column. */
    float gutterPadding = 10.f;
};

namespace code_editor_detail {

/* Entry points the overlay adds on top of tc_ui.h's table.  Optional: a build
   missing one simply keeps the plain multiline input.  The names are prefixed
   because a `using namespace` of this detail block sits next to tc_ui.h's own
   Table/table()/load() helper. */
struct DrawApi {
    void* (*windowList)();
    void (*addText)(void*, Vec2, CodeColor, const char*, const char*);
    void (*addRectFilled)(void*, Vec2, Vec2, CodeColor, float, int);
    void (*calcTextSize)(Vec2*, const char*, const char*, bool, float);
    void (*clipRectMin)(Vec2*, void*);
    void (*clipRectMax)(Vec2*, void*);
    float (*fontSize)();
    void (*insertChars)(void*, int, const char*, const char*);
    void (*pushStyleVarVec2)(int, Vec2);
    void (*popStyleVar)(int);
    void (*pushStyleColor)(int, Vec4);
    void (*popStyleColor)(int);
    std::uint32_t (*colorU32)(int, float);
    float (*getScrollY)();
    void (*setScrollY)(float);
};

inline DrawApi& drawApi() { static DrawApi value{}; return value; }
inline bool& resolveAttempted() { static bool value = false; return value; }
inline std::string& unresolved() { static std::string value; return value; }

inline bool resolve(const TCHost* host) {
    resolveAttempted() = true;
    drawApi() = DrawApi{};
    unresolved().clear();
    if (!host || !host->engine_proc || !host->context) {
        unresolved() = "engine_proc unavailable";
        return false;
    }
    DrawApi resolved{};
#define TC_CODE_BIND(field, name)                                                     \
    do {                                                                              \
        void* entry = host->engine_proc(host->context, name);                          \
        if (!entry) {                                                                 \
            if (!unresolved().empty()) unresolved() += ", ";                           \
            unresolved() += name;                                                      \
        }                                                                             \
        std::memcpy(&resolved.field, &entry, sizeof(entry));                           \
    } while (0)
    TC_CODE_BIND(windowList, "igGetWindowDrawList");
    TC_CODE_BIND(addText, "ImDrawList_AddText_Vec2");
    TC_CODE_BIND(addRectFilled, "ImDrawList_AddRectFilled");
    TC_CODE_BIND(calcTextSize, "igCalcTextSize");
    TC_CODE_BIND(clipRectMin, "ImDrawList_GetClipRectMin");
    TC_CODE_BIND(clipRectMax, "ImDrawList_GetClipRectMax");
    TC_CODE_BIND(fontSize, "igGetFontSize");
    TC_CODE_BIND(insertChars, "ImGuiInputTextCallbackData_InsertChars");
    TC_CODE_BIND(pushStyleVarVec2, "igPushStyleVar_Vec2");
    TC_CODE_BIND(popStyleVar, "igPopStyleVar");
    TC_CODE_BIND(pushStyleColor, "igPushStyleColor_Vec4");
    TC_CODE_BIND(popStyleColor, "igPopStyleColor");
    TC_CODE_BIND(colorU32, "igGetColorU32_Col");
    TC_CODE_BIND(getScrollY, "igGetScrollY");
    TC_CODE_BIND(setScrollY, "igSetScrollY_Float");
#undef TC_CODE_BIND
    if (!unresolved().empty()) {
        if (host->log)
            host->log(host->context,
                      ("Code editor: missing engine exports: " + unresolved()).c_str());
        return false;
    }
    drawApi() = resolved;
    return true;
}

/* ---- ImGui constants of this pinned build (1.92.6) --------------------- */

/* ImGuiInputTextFlags_ */
inline constexpr int kFlagAllowTabInput = 1 << 5;
inline constexpr int kFlagCallbackAlways = 1 << 20;
inline constexpr int kFlagCallbackEdit = 1 << 23;
/* ImGuiStyleVar_FramePadding */
inline constexpr int kVarFramePadding = 11;
/* ImGuiCol_Text - the colour the input draws its characters in. */
inline constexpr int kColText = 0;
/* Frame padding the editor asks for, so the overlay can place its text. */
inline constexpr float kPadX = 4.f;
inline constexpr float kPadY = 4.f;

/* Colours: a dark palette that reads well over the game's panels.  The base
   text colour is the input's own, so plain identifiers keep the theme. */
inline constexpr CodeColor kKeyword = codeRgba(0x7d, 0xa2, 0xff);
inline constexpr CodeColor kDirective = codeRgba(0xc5, 0x8a, 0xd8);
inline constexpr CodeColor kSystemTask = codeRgba(0x4e, 0xc9, 0xb0);
inline constexpr CodeColor kComment = codeRgba(0x66, 0x8c, 0x6a);
inline constexpr CodeColor kString = codeRgba(0xdc, 0xa8, 0x5c);
inline constexpr CodeColor kNumber = codeRgba(0xb0, 0xd0, 0x86);
inline constexpr CodeColor kGutterText = codeRgba(0x60, 0x68, 0x74);
inline constexpr CodeColor kGutterActive = codeRgba(0xd2, 0xda, 0xe4);
inline constexpr CodeColor kGutterBackground = codeRgba(0x18, 0x1a, 0x1f, 0xff);
inline constexpr CodeColor kGutterEdge = codeRgba(0x34, 0x36, 0x3d, 0xff);
inline constexpr CodeColor kCurrentLine = codeRgba(0x28, 0x35, 0x46, 0x88);
inline constexpr CodeColor kIndentGuide = codeRgba(0x4a, 0x4e, 0x58, 0x58);

/* ---- lexer ------------------------------------------------------------- */

inline bool wordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

/* Verilog words that get the keyword colour.  Identifiers keep the input's own
   text colour, which is what makes a keyword stand out. */
inline bool keyword(const char* text, std::size_t length) {
    static const char* words[] = {
        "module", "endmodule", "macromodule", "input", "output", "inout", "wire", "reg",
        "logic", "bit", "byte", "int", "integer", "shortint", "longint", "real", "time",
        "realtime", "signed", "unsigned", "assign", "always", "always_ff", "always_comb",
        "always_latch", "initial", "final", "if", "else", "case", "casez", "casex",
        "endcase", "default", "begin", "end", "for", "while", "repeat", "forever", "do",
        "genvar", "generate", "endgenerate", "parameter", "localparam", "defparam",
        "posedge", "negedge", "edge", "or", "and", "nand", "nor", "xor", "xnor", "not",
        "buf", "bufif0", "bufif1", "notif0", "notif1", "nmos", "pmos", "cmos", "rnmos",
        "rpmos", "rcmos", "tran", "tranif0", "tranif1", "rtran", "rtranif0", "rtranif1",
        "supply0", "supply1", "tri", "tri0", "tri1", "triand", "trior", "trireg", "wand",
        "wor", "uwire", "pullup", "pulldown", "function", "endfunction", "task", "endtask",
        "automatic", "static", "return", "break", "continue", "package", "endpackage",
        "typedef", "enum", "struct", "union", "packed", "interface", "endinterface",
        "modport", "endmodport", "program", "endprogram", "class", "endclass", "new",
        "this", "super", "extern", "virtual", "local", "protected", "const", "var", "void",
        "chandle", "string", "event", "scalared", "vectored", "small", "medium", "large",
        "table", "endtable", "primitive", "endprimitive", "specify", "endspecify",
        "specparam", "config", "endconfig", "design", "instance", "liblist", "cell", "use",
        "wait", "fork", "join", "join_any", "join_none", "disable", "force", "release",
        "deassign", "repeat", "unique", "unique0", "priority", "inside", "with",
    };
    for (const char* candidate : words) {
        if (std::strlen(candidate) == length && std::memcmp(candidate, text, length) == 0)
            return true;
    }
    return false;
}

struct Span {
    CodeColor color = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
};

/* One line's coloured spans.  `state` carries the block-comment flag across
   lines, so the widget only scans the buffer once from the top. */
struct LineState {
    bool blockComment = false;
};

inline void scanLine(const char* line, std::size_t length, LineState* state,
                     std::vector<Span>* spans) {
    std::size_t index = 0;
    while (index < length) {
        if (state->blockComment) {
            const std::size_t begin = index;
            while (index + 1 < length && !(line[index] == '*' && line[index + 1] == '/')) ++index;
            if (index + 1 < length) {
                index += 2;
                state->blockComment = false;
            } else {
                index = length;
            }
            spans->push_back({kComment, begin, index});
            continue;
        }
        const char c = line[index];
        if (c == '/' && index + 1 < length && line[index + 1] == '/') {
            spans->push_back({kComment, index, length});
            return;
        }
        if (c == '/' && index + 1 < length && line[index + 1] == '*') {
            state->blockComment = true;
            continue;
        }
        if (c == '"') {
            const std::size_t begin = index++;
            while (index < length) {
                if (line[index] == '\\' && index + 1 < length) {
                    index += 2;
                    continue;
                }
                if (line[index] == '"') {
                    ++index;
                    break;
                }
                ++index;
            }
            spans->push_back({kString, begin, index});
            continue;
        }
        if (c == '`') {
            const std::size_t begin = index++;
            while (index < length && wordChar(line[index])) ++index;
            spans->push_back({kDirective, begin, index});
            continue;
        }
        if (c == '$') {
            const std::size_t begin = index++;
            while (index < length && wordChar(line[index])) ++index;
            if (index - begin > 1)
                spans->push_back({kSystemTask, begin, index});
            continue;
        }
        if (c >= '0' && c <= '9') {
            const std::size_t begin = index;
            while (index < length && (wordChar(line[index]) || line[index] == '\'' ||
                                      line[index] == '.' ||
                                      ((line[index] == 'x' || line[index] == 'z' ||
                                        line[index] == 'X' || line[index] == 'Z' ||
                                        line[index] == '?') &&
                                       index > begin)))
                ++index;
            spans->push_back({kNumber, begin, index});
            continue;
        }
        if (c == '\'' && index + 1 < length &&
            std::strchr("bBoOdDhHsS", line[index + 1]) != nullptr) {
            const std::size_t begin = index;
            index += 2;
            while (index < length &&
                   (wordChar(line[index]) || line[index] == '?'))
                ++index;
            spans->push_back({kNumber, begin, index});
            continue;
        }
        if (wordChar(c)) {
            const std::size_t begin = index;
            while (index < length && wordChar(line[index])) ++index;
            if (keyword(line + begin, index - begin))
                spans->push_back({kKeyword, begin, index});
            continue;
        }
        ++index;
    }
}

/* ---- the input callback ------------------------------------------------ */

/* `ImGuiInputTextCallbackData` of ImGui 1.92.6, field for field.  The offsets
   below were read out of this build's own InsertChars/DeleteChars/SelectAll and
   are asserted, so a mismatched build fails at compile time instead of writing
   into the wrong bytes at run time. */
struct CallbackData {
    void* context;                     /*  0 ImGuiContext*            */
    int eventFlag;                     /*  8 ImGuiInputTextFlags      */
    int flags;                         /* 12                          */
    void* userData;                    /* 16                          */
    std::uint32_t id;                  /* 24 ImGuiID                  */
    int eventKey;                      /* 28 ImGuiKey                 */
    std::uint32_t eventChar;           /* 32 ImWchar                  */
    bool eventActivated;               /* 36                          */
    bool bufDirty;                     /* 37                          */
    char* buf;                         /* 40                          */
    int bufTextLen;                    /* 48                          */
    int bufSize;                       /* 52                          */
    int cursorPos;                     /* 56                          */
    int selectionStart;                /* 60                          */
    int selectionEnd;                  /* 64                          */
};
static_assert(sizeof(CallbackData) == 72, "ImGuiInputTextCallbackData size");
static_assert(offsetof(CallbackData, eventFlag) == 8, "callback ABI");
static_assert(offsetof(CallbackData, flags) == 12, "callback ABI");
static_assert(offsetof(CallbackData, userData) == 16, "callback ABI");
static_assert(offsetof(CallbackData, bufDirty) == 37, "callback ABI");
static_assert(offsetof(CallbackData, buf) == 40, "callback ABI");
static_assert(offsetof(CallbackData, bufTextLen) == 48, "callback ABI");
static_assert(offsetof(CallbackData, bufSize) == 52, "callback ABI");
static_assert(offsetof(CallbackData, cursorPos) == 56, "callback ABI");
static_assert(offsetof(CallbackData, selectionEnd) == 64, "callback ABI");

struct CallbackState {
    bool autoIndent = true;
    int indentWidth = 4;
    bool trusted = false;      /* the layout check passed at least once   */
    bool rejected = false;     /* it failed: leave the buffer alone       */
    int previousLength = 0;
    int caretLine = 0;
    int caretColumn = 0;
    int lastEvent = 0;
};

inline std::size_t lineStartOf(const char* text, int position) {
    int start = position;
    while (start > 0 && text[start - 1] != '\n') --start;
    return static_cast<std::size_t>(start);
}

inline std::size_t indentationOf(const char* text, std::size_t lineStart) {
    std::size_t count = 0;
    while (text[lineStart + count] == ' ' || text[lineStart + count] == '\t') ++count;
    return count;
}

inline std::size_t trimmedEnd(const char* text, std::size_t from, std::size_t to) {
    while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\t' ||
                         text[to - 1] == '\r'))
        --to;
    return to;
}

inline std::size_t trimmedBegin(const char* text, std::size_t from, std::size_t to) {
    while (from < to && (text[from] == ' ' || text[from] == '\t')) ++from;
    return from;
}

/* Last word of `[from,to)`; empty when the line does not end in one. */
inline bool endsWithWord(const char* text, std::size_t from, std::size_t to,
                         const char* word) {
    const std::size_t length = std::strlen(word);
    if (to - from < length) return false;
    for (std::size_t index = 0; index < length; ++index)
        if (text[to - length + index] != word[index]) return false;
    return to == from + length || !wordChar(text[to - length - 1]);
}

inline bool startsWithWord(const char* text, std::size_t from, std::size_t to,
                           const char* word) {
    const std::size_t length = std::strlen(word);
    if (to - from < length) return false;
    for (std::size_t index = 0; index < length; ++index)
        if (text[from + index] != word[index]) return false;
    return from + length == to || !wordChar(text[from + length]);
}

/* True when the line opens a block: `begin`, `case`, `else`, `generate`, and so
   on.  Only the shapes a Verilog writer actually types are covered; anything
   else keeps the indentation of the line it was written under. */
inline bool opensBlock(const char* text, std::size_t from, std::size_t to) {
    static const char* const openers[] = {"begin", "case", "casez", "casex", "else",
                                          "generate", "fork", "do"};
    if (to <= from) return false;
    for (const char* opener : openers)
        if (endsWithWord(text, from, to, opener)) return true;
    /* `case (sel)` ends in a ')' rather than in the keyword, so the case forms
       are recognised by their first word instead.  `always @(...)` and `if (x)`
       end in ')' too and are left neutral: the body that follows them is a
       single statement unless the writer typed `begin`. */
    static const char* const cases[] = {"case", "casez", "casex"};
    for (const char* form : cases)
        if (startsWithWord(text, from, to, form)) return true;
    return false;
}

/* True when the line closes a block. */
inline bool closesBlock(const char* text, std::size_t from, std::size_t to) {
    static const char* const closers[] = {"end", "endcase", "endgenerate", "endmodule",
                                          "endfunction", "endtask", "endpackage",
                                          "endinterface", "endprimitive", "join",
                                          "join_any", "join_none"};
    for (const char* closer : closers)
        if (startsWithWord(text, from, to, closer)) return true;
    return false;
}

inline bool layoutLooksSane(CallbackData* data) {
    if (!data->buf || data->bufTextLen < 0 || data->bufSize < data->bufTextLen + 1)
        return false;
    if (data->bufSize > (1 << 26)) return false;
    if (data->cursorPos < 0 || data->cursorPos > data->bufTextLen) return false;
    return std::strlen(data->buf) == static_cast<std::size_t>(data->bufTextLen);
}

inline int codeEditorCallback(void* raw) {
    auto* data = static_cast<CallbackData*>(raw);
    auto* state = static_cast<CallbackState*>(data->userData);
    if (!state || state->rejected) return 0;
    /* The widget asked for exactly these two events; anything else means the
       structure this build handed us is not the one the offsets describe. */
    if (data->eventFlag != kFlagCallbackAlways && data->eventFlag != kFlagCallbackEdit) {
        state->rejected = true;
        return 0;
    }
    if (!layoutLooksSane(data)) {
        state->rejected = true;
        return 0;
    }
    state->trusted = true;
    state->lastEvent = data->eventFlag;

    const std::size_t lineStart = lineStartOf(data->buf, data->cursorPos);
    state->caretLine = static_cast<int>(
        std::count(data->buf, data->buf + lineStart, '\n'));
    state->caretColumn = data->cursorPos - static_cast<int>(lineStart);

    if (data->eventFlag != kFlagCallbackEdit) return 0;
    const int length = data->bufTextLen;
    const int previous = state->previousLength;
    state->previousLength = length;
    if (!state->autoIndent || length != previous + 1) return 0;
    if (data->cursorPos <= 0 || data->buf[data->cursorPos - 1] != '\n') return 0;

    /* The caret is on a fresh, empty line: copy what the line above it indented
       to, then step in or out of the block it is part of. */
    const int textLength = data->cursorPos - 1;
    const std::size_t aboveStart = lineStartOf(data->buf, textLength);
    std::size_t aboveEnd = trimmedEnd(data->buf, aboveStart, static_cast<std::size_t>(textLength));
    const std::size_t aboveText = trimmedBegin(data->buf, aboveStart, aboveEnd);
    std::size_t indent = indentationOf(data->buf, aboveStart);
    if (closesBlock(data->buf, aboveText, aboveEnd))
        indent = indent >= static_cast<std::size_t>(state->indentWidth)
                     ? indent - static_cast<std::size_t>(state->indentWidth)
                     : 0;
    const bool opens = opensBlock(data->buf, aboveText, aboveEnd);
    if (indent == 0 && !opens) return 0;

    std::string pad;
    if (aboveEnd > aboveStart && data->buf[aboveStart] == '\t') {
        pad.assign(indent, '\t');
    } else {
        pad.assign(indent, ' ');
    }
    if (opens) {
        if (!pad.empty() && pad[0] == '\t')
            pad.push_back('\t');
        else
            pad.append(static_cast<std::size_t>(state->indentWidth), ' ');
    }
    if (pad.empty()) return 0;
    drawApi().insertChars(data, data->cursorPos, pad.c_str(), nullptr);
    return 0;
}

}  // namespace code_editor_detail

/* Resolves the optional entry points.  Safe to call more than once; returns
   false when this build cannot support the overlay. */
inline bool loadCodeEditor(const TCHost* host) { return code_editor_detail::resolve(host); }

/* True when the highlighted editor can run; resolves on the first call that is
   given a host, so a plugin may simply say `codeEditorAvailable(host)`. */
inline bool codeEditorAvailable(const TCHost* host = nullptr) {
    if (!code_editor_detail::resolveAttempted() && host) code_editor_detail::resolve(host);
    return code_editor_detail::drawApi().addText != nullptr &&
           code_editor_detail::drawApi().windowList != nullptr &&
           code_editor_detail::drawApi().calcTextSize != nullptr &&
           code_editor_detail::drawApi().insertChars != nullptr;
}

/* Comma separated list of the entry points this build does not export. */
inline const std::string& codeEditorMissing() { return code_editor_detail::unresolved(); }

/* Per-window state.  Keep one per editor for as long as the panel exists; it
   carries the caret line for the gutter and the auto-indent bookkeeping.

   `layoutVerified` turns on the coloured text: a multi-line input draws its
   characters inside a child window of its own, and ImGui renders child windows
   after their parent, so anything the widget paints itself would be covered by
   the input's own (plain) text.  Once a callback has proved that this build's
   `ImGuiInputTextCallbackData` is the one the offsets below describe, the input
   is told to draw its text with a transparent colour and the widget paints the
   whole buffer itself, in colour.  Until then the plain text stays visible and
   only the accents are added, so a build with a different layout keeps working
   (with a slightly muted highlighting) instead of going blank. */
struct CodeEditorState {
    bool initialised = false;
    int caretLine = 0;
    int caretColumn = 0;
    bool autoIndentActive = false;
    bool layoutVerified = false;
};

/* Draws `buffer` (NUL terminated, `capacity` bytes) as a code editor inside the
   current window or child.  The item is `size` tall and `size.x` wide (0 means
   "as wide as the available region"); the caller owns the buffer.  Returns true
   when the text changed in this frame. */
inline bool codeEditor(const char* id, char* buffer, std::size_t capacity, Vec2 size,
                       CodeEditorState* state, const CodeEditorOptions& options = {}) {
    using namespace code_editor_detail;
    if (!buffer || capacity < 2) return false;
    if (!codeEditorAvailable() || !state) {
        return inputTextMultiline(id, buffer, static_cast<unsigned long long>(capacity),
                                  size, 0);
    }
    auto& draw = drawApi();
    if (!state->initialised) {
        state->initialised = true;
    }

    /* Line table of the current text (only the visible ones are drawn). */
    std::vector<std::size_t> lines;
    lines.push_back(0);
    const std::size_t length = std::strlen(buffer);
    for (std::size_t index = 0; index < length; ++index)
        if (buffer[index] == '\n') lines.push_back(index + 1);
    const std::size_t lineCount = lines.size();

    const float lineHeight = draw.fontSize ? draw.fontSize() : 16.f;
    const Vec2 available = contentRegionAvailable();
    float width = size.x > 0.f ? size.x : available.x;
    if (width <= 0.f) width = 320.f;
    float height = size.y > 0.f ? size.y : 240.f;
    if (height <= 0.f) height = 240.f;

    /* Gutter: as wide as the longest line number plus a gap. */
    const std::string digits = std::to_string(lineCount);
    Vec2 digitSize{0.f, 0.f};
    draw.calcTextSize(&digitSize, digits.c_str(), nullptr, false, 0.f);
    const float gutter = options.lineNumbers ? digitSize.x + options.gutterPadding : 0.f;
    const float textWidth = std::max(64.f, width - gutter);

    /* The input is as tall as its text so the enclosing child scrolls it; that
       keeps the caret and the overlay on the same, single scroll offset. */
    const float contentHeight =
        lineHeight * static_cast<float>(lineCount) + std::max(0.f, options.extraBottom);
    const float inputHeight = std::max(height, contentHeight);

    Vec2 origin{0.f, 0.f};
    if (table().getCursorScreenPos) table().getCursorScreenPos(&origin);
    const float inputX = origin.x + gutter + kPadX;
    const float inputY = origin.y + kPadY;

    if (gutter > 0.f) setCursorPos({cursorPosX() + gutter, cursorPosY()});

    CallbackState callbackState;
    callbackState.autoIndent = options.autoIndent;
    callbackState.indentWidth = std::max(1, options.indentWidth);
    callbackState.previousLength = static_cast<int>(length);
    callbackState.caretLine = state->caretLine;

    if (draw.pushStyleVarVec2 && draw.popStyleVar)
        draw.pushStyleVarVec2(kVarFramePadding, {kPadX, kPadY});
    /* Once the callback layout is known, the input's own characters are turned
       transparent and every line is painted below instead.  The caret uses
       ImGuiCol_InputTextCursor and the selection its own colour, so both stay
       exactly as ImGui draws them. */
    const bool ownText = state->layoutVerified && options.highlight &&
                         draw.pushStyleColor && draw.popStyleColor && draw.colorU32;
    if (ownText) draw.pushStyleColor(kColText, {0.f, 0.f, 0.f, 0.f});
    const bool changed = table().inputTextMultiline
                             ? table().inputTextMultiline(
                                   id, buffer, static_cast<unsigned long long>(capacity),
                                   {textWidth, inputHeight},
                                   kFlagAllowTabInput | kFlagCallbackAlways |
                                       kFlagCallbackEdit,
                                   reinterpret_cast<void*>(&codeEditorCallback),
                                   &callbackState)
                             : false;
    if (ownText) draw.popStyleColor(1);
    if (draw.pushStyleVarVec2 && draw.popStyleVar) draw.popStyleVar(1);

    state->caretLine = callbackState.caretLine;
    state->caretColumn = callbackState.caretColumn;
    state->autoIndentActive = callbackState.autoIndent && !callbackState.rejected;
    /* Sticky: the callback only runs while the field is active, but the colours
       it unlocks have to stay on after the player clicks away. */
    if (callbackState.trusted) state->layoutVerified = true;

    void* list = draw.windowList ? draw.windowList() : nullptr;
    if (!list) return changed;
    Vec2 clipMin{origin.x, origin.y};
    Vec2 clipMax{origin.x + textWidth + gutter, origin.y + inputHeight};
    if (draw.clipRectMin) draw.clipRectMin(&clipMin, list);
    if (draw.clipRectMax) draw.clipRectMax(&clipMax, list);

    const float itemTop = origin.y;
    const float itemBottom = origin.y + inputHeight;
    const float visibleTop = std::max(itemTop, clipMin.y);
    const float visibleBottom = std::min(itemBottom, clipMax.y);
    if (visibleBottom <= visibleTop) return changed;

    /* First and last line whose band can be seen. */
    int firstLine = static_cast<int>((visibleTop - inputY) / lineHeight) - 1;
    int lastLine = static_cast<int>((visibleBottom - inputY) / lineHeight) + 2;
    firstLine = std::max(0, firstLine);
    lastLine = std::min(static_cast<int>(lineCount), lastLine);

    if (gutter > 0.f) {
        draw.addRectFilled(list, {origin.x, visibleTop}, {origin.x + gutter, visibleBottom},
                           kGutterBackground, 0.f, 0);
        draw.addRectFilled(list, {origin.x + gutter - 1.f, visibleTop},
                           {origin.x + gutter, visibleBottom}, kGutterEdge, 0.f, 0);
    }

    LineState state0;
    std::vector<Span> spans;
    std::vector<Span> scratch;
    const CodeColor plainColor =
        (ownText && draw.colorU32) ? draw.colorU32(kColText, 1.f) : 0u;
    for (std::size_t line = 0; line < lineCount; ++line) {
        const std::size_t begin = lines[line];
        const std::size_t end = (line + 1 < lineCount) ? lines[line + 1] - 1 : length;
        const bool visible = static_cast<int>(line) >= firstLine &&
                             static_cast<int>(line) < lastLine;
        if (!visible) {
            /* Lines above the window still decide whether the window starts
               inside a block comment; nothing below it can affect a line that
               was already painted. */
            if (end > begin && line < static_cast<std::size_t>(firstLine)) {
                scratch.clear();
                scanLine(buffer + begin, end - begin, &state0, &scratch);
            }
            continue;
        }
        if (end <= begin) continue;
        const float lineY = inputY + lineHeight * static_cast<float>(line);
        const bool active = static_cast<int>(line) == state->caretLine;
        if (active && options.currentLine) {
            draw.addRectFilled(list, {origin.x + gutter, lineY},
                               {origin.x + gutter + textWidth, lineY + lineHeight},
                               kCurrentLine, 0.f, 0);
        }
        if (options.lineNumbers && gutter > 0.f) {
            const std::string number = std::to_string(line + 1);
            Vec2 size2{0.f, 0.f};
            draw.calcTextSize(&size2, number.c_str(), nullptr, false, 0.f);
            draw.addText(list,
                         {origin.x + gutter - options.gutterPadding - size2.x, lineY},
                         active ? kGutterActive : kGutterText, number.c_str(), nullptr);
        }
        if (options.indentGuides) {
            std::size_t spaces = 0;
            while (begin + spaces < end && buffer[begin + spaces] == ' ') ++spaces;
            for (std::size_t column = static_cast<std::size_t>(options.indentWidth);
                 column <= spaces; column += static_cast<std::size_t>(options.indentWidth)) {
                const std::string pad(column, ' ');
                Vec2 padSize{0.f, 0.f};
                draw.calcTextSize(&padSize, pad.c_str(), nullptr, false, 0.f);
                const float guideX = inputX + padSize.x - 1.f;
                draw.addRectFilled(list, {guideX, lineY},
                                   {guideX + 1.f, lineY + lineHeight}, kIndentGuide, 0.f, 0);
            }
        }
        if (!options.highlight) continue;
        spans.clear();
        scanLine(buffer + begin, end - begin, &state0, &spans);
        float x = 0.f;
        /* Span offsets are relative to the start of this line, so the running
           cursor is relative too. */
        std::size_t cursor = 0;
        for (const Span& span : spans) {
            const std::size_t gapEnd = span.begin;
            if (gapEnd > cursor) {
                const std::string gap(buffer + begin + cursor, gapEnd - cursor);
                Vec2 size2{0.f, 0.f};
                draw.calcTextSize(&size2, gap.c_str(), nullptr, false, 0.f);
                if (ownText)
                    draw.addText(list, {inputX + x, lineY}, plainColor, gap.c_str(),
                                 nullptr);
                x += size2.x;
            }
            const std::string piece(buffer + begin + span.begin, span.end - span.begin);
            draw.addText(list, {inputX + x, lineY}, span.color, piece.c_str(), nullptr);
            Vec2 size2{0.f, 0.f};
            draw.calcTextSize(&size2, piece.c_str(), nullptr, false, 0.f);
            x += size2.x;
            cursor = span.end;
        }
        if (ownText && cursor < end - begin) {
            const std::string tail(buffer + begin + cursor, end - begin - cursor);
            draw.addText(list, {inputX + x, lineY}, plainColor, tail.c_str(), nullptr);
        }
    }

    /* Keep the caret in view.  The field is as tall as its text and it is the
       enclosing child that scrolls, so the caret would otherwise walk off the
       bottom edge while typing.  This runs one frame behind the scroll it asks
       for, which is how ImGui's own scroll requests behave too. */
    if (draw.getScrollY && draw.setScrollY && callbackState.trusted &&
        !callbackState.rejected && isItemFocused()) {
        const float caretTop = inputY + lineHeight * static_cast<float>(state->caretLine);
        const float caretBottom = caretTop + lineHeight;
        float delta = 0.f;
        if (caretTop < clipMin.y) {
            delta = caretTop - clipMin.y - lineHeight;
        } else if (caretBottom > clipMax.y) {
            delta = caretBottom - clipMax.y + lineHeight;
        }
        if (delta != 0.f) draw.setScrollY(draw.getScrollY() + delta);
    }
    return changed;
}

}  // namespace ui
}  // namespace tc

#endif  // TC_CODE_EDITOR_H
