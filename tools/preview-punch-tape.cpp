/* Draws the punch tape's geometry on a fake drawer, with the game's own chip
   sprite, so the layout can be looked at without opening the game.

   It runs the same tape_layout.hpp the plugin runs, at the drawer size you give
   it, and writes a BMP:

     preview-punch-tape <out.bmp> <panelWidth> <panelHeight> <bits>[,<bits>...]

   Example (the window the stall was measured in, three values):

     preview-punch-tape preview.bmp 2555 345 18,32,64

   This is a preview of the *maths*, not a screenshot: the drawer, the heading
   and the native label/value fields are drawn as flat placeholders at the
   positions the game uses (heading y=19, value input y=73, controls up to
   x=680). */
#include "../examples/punch-tape/tape_layout.hpp"

#include <windows.h>
#include <wincodec.h>
#include <objbase.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr float kNativeControlsRight = 680.f;
constexpr float kControlsTapeGap = 32.f;
constexpr float kHorizontalMargin = 16.f;
constexpr float kVerticalMargin = 10.f;
constexpr float kTapeTop = 119.f;

struct Pixel {
    unsigned char b = 0, g = 0, r = 0;
};

struct Image {
    int width = 0, height = 0;
    std::vector<Pixel> pixels;
    Image(int w, int h) : width(w), height(h), pixels(static_cast<size_t>(w) * h) {}
    Pixel& at(int x, int y) { return pixels[static_cast<size_t>(y) * width + x]; }
    void fill(Pixel colour) {
        for (auto& pixel : pixels) pixel = colour;
    }
    void rect(float x0, float y0, float x1, float y1, Pixel colour) {
        if (x1 < x0) std::swap(x0, x1);
        if (y1 < y0) std::swap(y0, y1);
        const int left = static_cast<int>(x0 < 0 ? 0 : x0);
        const int top = static_cast<int>(y0 < 0 ? 0 : y0);
        const int right = static_cast<int>(x1 > width ? width : x1);
        const int bottom = static_cast<int>(y1 > height ? height : y1);
        for (int y = top; y < bottom; ++y)
            for (int x = left; x < right; ++x) at(x, y) = colour;
    }
    bool writeBmp(const char* path) const {
        const int rowBytes = ((width * 3) + 3) & ~3;
        const int dataBytes = rowBytes * height;
        const int fileBytes = 54 + dataBytes;
        std::vector<unsigned char> out(static_cast<size_t>(fileBytes), 0);
        out[0] = 'B'; out[1] = 'M';
        memcpy(&out[2], &fileBytes, 4);
        const int offset = 54;
        memcpy(&out[10], &offset, 4);
        const int header = 40;
        memcpy(&out[14], &header, 4);
        memcpy(&out[18], &width, 4);
        memcpy(&out[22], &height, 4);
        const short planes = 1, bits = 24;
        memcpy(&out[26], &planes, 2);
        memcpy(&out[28], &bits, 2);
        memcpy(&out[34], &dataBytes, 4);
        for (int y = 0; y < height; ++y) {
            unsigned char* row = out.data() + 54 + static_cast<size_t>(height - 1 - y) * rowBytes;
            for (int x = 0; x < width; ++x) {
                const Pixel pixel = pixels[static_cast<size_t>(y) * width + x];
                row[x * 3 + 0] = pixel.b;
                row[x * 3 + 1] = pixel.g;
                row[x * 3 + 2] = pixel.r;
            }
        }
        FILE* file = std::fopen(path, "wb");
        if (!file) return false;
        const size_t written = std::fwrite(out.data(), 1, out.size(), file);
        std::fclose(file);
        return written == out.size();
    }
};

struct Sprite {
    int width = 0, height = 0;
    std::vector<unsigned char> rgba;
};

bool loadSprite(const wchar_t* path, Sprite* sprite) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return false;
    bool ok = false;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    do {
        if (FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &decoder)))
            break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
            break;
        UINT w = 0, h = 0;
        if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0) break;
        sprite->width = static_cast<int>(w);
        sprite->height = static_cast<int>(h);
        sprite->rgba.assign(static_cast<size_t>(w) * h * 4, 0);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(sprite->rgba.size()),
                                         sprite->rgba.data())))
            break;
        ok = true;
    } while (false);
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    factory->Release();
    return ok;
}

/* Game sprite atlas: 96x96 chips, pink at x=2, green at x=102, and the first
   three rows are the normal, hovered and pressed frames. */
void drawChip(Image& image, const Sprite& sprite, float x, float y, float cell, bool on) {
    if (cell < 1.f) return;
    const float u0 = on ? 102.f : 2.f;
    const float v0 = 2.f;
    const float span = 96.f;
    for (int py = 0; py < static_cast<int>(cell); ++py) {
        for (int px = 0; px < static_cast<int>(cell); ++px) {
            const int sx = static_cast<int>(u0 + (px + 0.5f) * span / cell);
            const int sy = static_cast<int>(v0 + (py + 0.5f) * span / cell);
            if (sx < 0 || sy < 0 || sx >= sprite.width || sy >= sprite.height) continue;
            const size_t index = (static_cast<size_t>(sy) * sprite.width + sx) * 4;
            const unsigned char* texel = &sprite.rgba[index];
            const int alpha = texel[3];
            if (alpha < 8) continue;
            const int x0 = static_cast<int>(x) + px;
            const int y0 = static_cast<int>(y) + py;
            if (x0 < 0 || y0 < 0 || x0 >= image.width || y0 >= image.height) continue;
            Pixel& target = image.at(x0, y0);
            const float a = alpha / 255.f;
            target.r = static_cast<unsigned char>(texel[0] * a + target.r * (1.f - a));
            target.g = static_cast<unsigned char>(texel[1] * a + target.g * (1.f - a));
            target.b = static_cast<unsigned char>(texel[2] * a + target.b * (1.f - a));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::printf("usage: preview-punch-tape <out.bmp> <panelWidth> <panelHeight> <bits>[,<bits>...]\n");
        return 2;
    }
    const int panelWidth = std::atoi(argv[2]);
    const int panelHeight = std::atoi(argv[3]);
    std::vector<int> widths;
    for (const char* cursor = argv[4]; *cursor;) {
        const int value = std::atoi(cursor);
        if (value > 0) widths.push_back(value);
        const char* comma = std::strchr(cursor, ',');
        if (!comma) break;
        cursor = comma + 1;
    }
    if (panelWidth < 64 || panelHeight < 64 || widths.empty()) return 2;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Sprite sprite;
    const bool haveSprite = loadSprite(L"asset\\io_state\\io_state.png", &sprite)
                                || loadSprite(L"D:\\p\\asset\\io_state\\io_state.png", &sprite);
    Image image(panelWidth, panelHeight);
    image.fill(Pixel{60, 43, 46}); /* the drawer's own dark purple, BGR */
    /* Placeholders for what the game itself draws in that panel. */
    image.rect(170.f, 73.f, 490.f, 99.f, Pixel{42, 37, 37});
    image.rect(170.f, 121.f, 490.f, 147.f, Pixel{42, 37, 37});

    float top = kTapeTop;
    for (const int width : widths) {
        float tapeLeft = kNativeControlsRight + kControlsTapeGap;
        if (tapeLeft > panelWidth * 0.45f) tapeLeft = panelWidth * 0.45f;
        const float availableWidth = panelWidth - tapeLeft - kHorizontalMargin;
        const float availableHeight =
            panelHeight > 0 ? panelHeight - kTapeTop - kVerticalMargin : -1.f;
        const tc_tape::Layout layout = tc_tape::makeLayout(width, availableWidth, availableHeight);
        const float gridW = tc_tape::gridWidth(width, layout);
        const float gridH = tc_tape::gridHeight(width, layout);
        float x = (panelWidth - gridW) * 0.5f;
        if (x < tapeLeft) x = tapeLeft;
        const float rightmost = panelWidth - kHorizontalMargin - gridW;
        if (x > rightmost) x = rightmost;
        if (x < 8.f) x = 8.f;
        std::printf("bits=%d cell=%.1f groupsPerRow=%d grid=%.0fx%.0f at %.0f,%.0f\n", width,
                    layout.cell, layout.groupsPerRow, gridW, gridH, x, top);
        for (int bit = 0; bit < width; ++bit) {
            tc_tape::Point point{};
            if (!tc_tape::cellPositionForBit(bit, width, layout, &point)) continue;
            /* A recognisable pattern: every third bit set. */
            drawChip(image, sprite, x + point.x, top + point.y, layout.cell, (bit % 3) == 0);
        }
        top += gridH + 24.f;
        if (top > panelHeight) break;
    }
    if (!image.writeBmp(argv[1])) {
        std::printf("could not write %s\n", argv[1]);
        return 1;
    }
    std::printf("wrote %s (%dx%d, sprite %s)\n", argv[1], panelWidth, panelHeight,
                haveSprite ? "loaded" : "missing");
    return 0;
}
