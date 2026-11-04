// Run the shipped note implementation and photograph its completed interaction
// sequence, so pixel assertions use the same geometry as the final log entries.
#include "isolated-cursor.hpp"
// Confine synthetic pointer positions to this game's process. An off-screen
// window cannot place the desktop cursor at its client coordinates reliably.
#define SetCursorPos tc_test_cursor::set
#define tc_mod_load text_box_base_load
#include "../examples/text-box/plugin.cpp"
#undef tc_mod_load
#undef SetCursorPos
#include <GL/gl.h>

namespace {
void (*textBoxBaseFrame)(void*, const TCFrame*) = nullptr;
unsigned settledFrames = 0;
bool photographed = false;

void putBmp32(unsigned char* out, unsigned word) {
    for(unsigned byte = 0; byte < 4; ++byte) out[byte] = static_cast<unsigned char>(word >> (byte * 8));
}

bool photograph(const wchar_t* path) {
    GLint viewport[4]{};
    glGetIntegerv(GL_VIEWPORT, viewport);
    const int width = viewport[2], height = viewport[3];
    if(width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;
    std::vector<unsigned char> pixels(size_t(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(viewport[0], viewport[1], width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    std::FILE* file = _wfopen(path, L"wb");
    if(!file) return false;
    unsigned char header[54]{};
    header[0] = 'B'; header[1] = 'M'; header[26] = 1; header[28] = 32;
    putBmp32(header + 2, 54 + unsigned(pixels.size()));
    putBmp32(header + 10, 54); putBmp32(header + 14, 40);
    putBmp32(header + 18, unsigned(width)); putBmp32(header + 22, unsigned(-height));
    putBmp32(header + 34, unsigned(pixels.size()));
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<unsigned char> row(size_t(width) * 4);
    for(int y = 0; y < height; ++y) {
        const auto* source = pixels.data() + size_t(height - y - 1) * width * 4;
        for(int x = 0; x < width; ++x) {
            row[x * 4] = source[x * 4 + 2]; row[x * 4 + 1] = source[x * 4 + 1];
            row[x * 4 + 2] = source[x * 4]; row[x * 4 + 3] = 255;
        }
        ok = std::fwrite(row.data(), 1, row.size(), file) == row.size() && ok;
    }
    return std::fclose(file) == 0 && ok;
}

void textBoxFrame(void* user, const TCFrame* value) {
    if(textBoxBaseFrame) textBoxBaseFrame(user, value);
    if(photographed || autotestStage != 16 || ++settledFrames < 6) return;
    wchar_t path[32768]{};
    if(!GetEnvironmentVariableW(L"TC_TEST_TEXTBOX_SHOT", path, 32768)) return;
    photographed = true;
    report(photograph(path) ? "Captured frame screenshot (text-box test completion)" : "Text-box test screenshot failed");
}
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    const int result = text_box_base_load(h, plugin);
    if(result) return result;
    if(!tc_test_cursor::install()) return 20;
    textBoxBaseFrame = plugin->on_frame;
    plugin->on_frame = textBoxFrame;
    return 0;
}
