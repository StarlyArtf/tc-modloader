/* Unit tests for sdk/tc_code_editor.h, the source editor the TC-Verilog page
   draws.  Two things are worth testing without the game:

     * the Enter behaviour (keep the line's indentation, open a level after
       `begin`/`case`/..., close one before `end`/...), and
     * the guard that keeps the input callback away from a buffer whose layout
       does not match the ImGuiInputTextCallbackData it was handed.

   The widget reaches ImGui through one function pointer, so the test installs
   its own that behaves like ImGuiInputTextCallbackData::InsertChars.  No
   engine, no window, no frame. */

#include "../sdk/tc_code_editor.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

namespace {

using tc::ui::code_editor_detail::CallbackData;
using tc::ui::code_editor_detail::CallbackState;
using tc::ui::code_editor_detail::codeEditorCallback;
using tc::ui::code_editor_detail::drawApi;
using tc::ui::code_editor_detail::kFlagCallbackAlways;
using tc::ui::code_editor_detail::kFlagCallbackEdit;

int insertCalls = 0;

void stubInsertChars(void* raw, int position, const char* text, const char* textEnd) {
    auto* data = static_cast<CallbackData*>(raw);
    const std::string piece = textEnd ? std::string(text, textEnd) : std::string(text);
    std::string body(data->buf, static_cast<std::size_t>(data->bufTextLen));
    body.insert(static_cast<std::size_t>(position), piece);
    std::memcpy(data->buf, body.c_str(), body.size() + 1);
    data->bufTextLen = static_cast<int>(body.size());
    data->cursorPos = position + static_cast<int>(piece.size());
    data->selectionStart = data->cursorPos;
    data->selectionEnd = data->cursorPos;
    data->bufDirty = true;
    ++insertCalls;
}

/* Runs one Enter at the end of `text` and reports what the buffer holds then.
   `previousLength` is the length before the newline was typed, which is what
   the widget compares against. */
std::string typeEnter(const std::string& text, int eventFlag = kFlagCallbackEdit,
                      std::size_t capacity = 4096, int previousDelta = 1) {
    std::string storage = text;
    storage.resize(capacity, '\0');
    CallbackData data{};
    data.eventFlag = eventFlag;
    data.flags = kFlagCallbackAlways | kFlagCallbackEdit;
    data.buf = storage.data();
    data.bufTextLen = static_cast<int>(text.size());
    data.bufSize = static_cast<int>(capacity);
    data.cursorPos = static_cast<int>(text.size());
    data.selectionStart = data.cursorPos;
    data.selectionEnd = data.cursorPos;
    CallbackState state;
    state.autoIndent = true;
    state.indentWidth = 4;
    state.previousLength = static_cast<int>(text.size()) - previousDelta;
    data.userData = &state;
    codeEditorCallback(&data);
    return std::string(data.buf);
}

void expect(const char* name, const std::string& got, const std::string& want) {
    if (got != want) {
        std::cerr << "FAIL " << name << "\n  got  " << got << "\n  want " << want << "\n";
        std::exit(1);
    }
    std::cout << "ok " << name << "\n";
}

}  // namespace

int main() {
    drawApi() = tc::ui::code_editor_detail::DrawApi{};
    drawApi().insertChars = &stubInsertChars;

    /* A line with no indentation and no block around it stays without one. */
    insertCalls = 0;
    expect("plain line keeps column zero", typeEnter("module m;\n"), "module m;\n");
    assert(insertCalls == 0);

    /* Enter after `begin` opens a level; Enter after `end` closes one. */
    expect("begin opens a level", typeEnter("begin\n"), "begin\n    ");
    expect("clk begin opens a level", typeEnter("always @(posedge clk) begin\n"),
           "always @(posedge clk) begin\n    ");
    expect("case opens a level", typeEnter("case (sel)\n"), "case (sel)\n    ");
    expect("end at column zero stays there", typeEnter("end\n"), "end\n");
    expect("end closes one level", typeEnter("        end\n"), "        end\n    ");
    expect("endassign keeps the indent", typeEnter("    x <= y;\n"), "    x <= y;\n    ");

    /* An assignment that happens to end in `end`-ish text is not a closer. */
    expect("identifier ending in end is not a closer", typeEnter("    value = blend;\n"),
           "    value = blend;\n    ");

    /* Tabs are preserved instead of being rewritten to spaces. */
    expect("tab indent is copied", typeEnter("\tif (x)\n"), "\tif (x)\n\t");
    expect("tab indent plus a level", typeEnter("\tbegin\n"), "\tbegin\n\t\t");

    /* Only a single newline typed at the caret indents: a line that opens
       nothing stays as it is, and an edit that changed more than one character
       (a paste, a replacement) is left to ImGui. */
    insertCalls = 0;
    expect("a line that opens nothing keeps column zero", typeEnter("value = 1;\n"),
           "value = 1;\n");
    assert(insertCalls == 0);
    insertCalls = 0;
    expect("no indent for a multi-character edit",
           typeEnter("begin\n", kFlagCallbackEdit, 4096, 2), "begin\n");
    assert(insertCalls == 0);

    /* The callback only acts on the events the widget itself asked for, and
       only on a buffer that passes the layout check. */
    insertCalls = 0;
    expect("foreign event flag is ignored", typeEnter("begin\n", 0), "begin\n");
    assert(insertCalls == 0);

    {
        std::string storage = "begin\n";
        storage.resize(64, '\0');
        CallbackData data{};
        data.eventFlag = kFlagCallbackEdit;
        data.buf = storage.data();
        data.bufTextLen = 6;
        data.bufSize = 6; /* one byte short of NUL: rejected before any write */
        data.cursorPos = 6;
        CallbackState state;
        state.previousLength = 5;
        data.userData = &state;
        insertCalls = 0;
        codeEditorCallback(&data);
        assert(insertCalls == 0);
        assert(state.rejected);
        assert(!state.trusted);
        assert(std::string(data.buf) == "begin\n");
        std::cout << "ok a short buffer is rejected\n";
    }

    /* A good call records the caret so the gutter can mark the line. */
    {
        std::string storage = "aa\nbb\ncc\n";
        storage.resize(64, '\0');
        CallbackData data{};
        data.eventFlag = kFlagCallbackAlways;
        data.buf = storage.data();
        data.bufTextLen = 9;
        data.bufSize = 64;
        data.cursorPos = 8;
        CallbackState state;
        data.userData = &state;
        codeEditorCallback(&data);
        assert(state.trusted);
        assert(!state.rejected);
        assert(state.caretLine == 2);
        std::cout << "ok the caret line is tracked\n";
    }

    /* The table the widget draws through stays optional: with no engine the
       widget has to say so instead of dereferencing a null pointer. */
    drawApi() = tc::ui::code_editor_detail::DrawApi{};
    assert(!tc::ui::codeEditorAvailable());
    std::cout << "ok the overlay reports itself unavailable without an engine\n";

    std::cout << "tc-code-editor: all checks passed\n";
    return 0;
}
