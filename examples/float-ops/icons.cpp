/* Build-time generator: the picture each Float Ops type is shown with in the
   game's own component column, in the bottom drawer and as the ghost that
   follows the cursor while it is placed (tc.component.render V5).

   The picture is not drawn by a second copy of the layout: this tool *includes
   the Mod's own components.cpp* - the same table, the same drawing code, the
   same constants - and hands the draw callback a rasterising draw table instead
   of the game's ImGui one.  What ships as a PNG is therefore what the part looks
   like on the board: the purple body, the "32" badge, the name in the top right,
   the symbol or value in the middle and the pins on the 3.0 lane.

   Usage: icons.exe <output directory>   (writes <decimal custom id>.png per type;
   a missing or half-written file is what the loader turns back into the game's
   own picture, so a failed run degrades instead of breaking the package). */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>

#include "components.cpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <vector>

namespace {

std::unique_ptr<Gdiplus::FontFamily> makeFamily() {
    auto candidate = std::make_unique<Gdiplus::FontFamily>(L"Segoe UI");
    if (!candidate->IsAvailable()) candidate = std::make_unique<Gdiplus::FontFamily>(L"Arial");
    if (!candidate->IsAvailable()) return nullptr;
    return candidate;
}

/* One board cell is this many pixels in the generated picture.  The player's own
   board runs at about 25.6-30 px/cell (docs/verification.md), so the picture is
   the board's own scale; the game then draws it into its 60x60 palette item. */
constexpr float kCellPixels = 26.f;
/* The canvas is square because the game draws it into a square item box
   (ITEM_SIZE = 60x60 in build_component__...Zflat95list_u119, and the game's own
   snapshot texture is 160x160), so a square picture keeps the part's aspect.  It
   is wide enough for the picture's own extent: the pin dots sit on the +-3.0-cell
   lane and stick out by their radius, so the part reaches +-3.33 cells = +-86.6
   px, and the body's own 4.92 cells are 128 px. */
constexpr int kCanvasSize = 192;

using Gdiplus::Color;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::PointF;
using Gdiplus::RectF;
using Gdiplus::SolidBrush;

Color colourOf(uint32_t rgba) {
    return Color(static_cast<BYTE>((rgba >> 24) & 0xff), static_cast<BYTE>(rgba & 0xff),
                 static_cast<BYTE>((rgba >> 8) & 0xff), static_cast<BYTE>((rgba >> 16) & 0xff));
}

std::wstring widen(const char* utf8) {
    if (!utf8 || !*utf8) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (length <= 1) return std::wstring();
    std::wstring text(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, text.data(), length);
    return text;
}

struct Raster {
    Graphics* graphics = nullptr;
    Gdiplus::FontFamily* family = nullptr;
    float emPainted = 0.f;
    float capTop = 0.f, capHeight = 0.f;   /* ink of "H" at emPainted, from the origin */
};

Raster* rasterOf(void* context) { return static_cast<Raster*>(context); }

/* GDI+ has no "draw this text with its ink here" call, so every run goes through
   an outline: the ink box is measured and the outline is moved so that the run's
   ink starts at x and the *cap* middle sits where the Mod's own layout expects it
   (kCapPerAsked of the size below the origin - the same relation ImGui's AddText
   has, which is what the Mod's constants were measured against). */
RectF inkBox(Gdiplus::FontFamily* family, const std::wstring& text, float em, bool bold) {
    RectF none{};
    if (!family || text.empty()) return none;
    GraphicsPath path;
    if (path.AddString(text.c_str(), static_cast<INT>(text.size()), family,
                       bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, em,
                       PointF(0.f, 0.f), nullptr) != Gdiplus::Ok)
        return none;
    RectF bounds{};
    path.GetBounds(&bounds);
    return bounds;
}

bool drawRun(Raster* raster, const std::wstring& text, float x, float y, float size,
             bool bold, Color color) {
    if (!raster || !raster->graphics || !raster->family || text.empty() || !(size > 0.f))
        return false;
    const float em = size * floatops::internals::kPaintedPerAsked;
    const RectF cap = inkBox(raster->family, L"H", em, bold);
    const RectF ink = inkBox(raster->family, text, em, bold);
    if (cap.Height <= 0.f || ink.Width <= 0.f) return false;
    /* The Mod asks for a size whose painted digit height is kCapPerAsked of it;
       the face's own cap height is measured rather than assumed, so the picture
       keeps that relation whatever font this machine renders with. */
    const float wanted = floatops::internals::kCapPerAsked * size;
    const float scale = wanted / cap.Height;
    GraphicsPath path;
    if (path.AddString(text.c_str(), static_cast<INT>(text.size()), raster->family,
                       bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                       em * scale, PointF(0.f, 0.f), nullptr) != Gdiplus::Ok)
        return false;
    const float capMiddle = (cap.Y + cap.Height * 0.5f) * scale;
    Gdiplus::Matrix move(1.f, 0.f, 0.f, 1.f, x - ink.X * scale, y + wanted - capMiddle);
    path.Transform(&move);
    SolidBrush brush(color);
    return raster->graphics->FillPath(&brush, &path) == Gdiplus::Ok;
}

/* GraphicsPath is not copyable, so it is built in place. */
void buildRoundedRect(GraphicsPath* path, float x0, float y0, float x1, float y1,
                      float rounding) {
    const float radius = std::min(rounding, std::min(x1 - x0, y1 - y0) * 0.5f);
    if (radius <= 0.01f) {
        path->AddRectangle(RectF(x0, y0, x1 - x0, y1 - y0));
        return;
    }
    const float d = radius * 2.f;
    path->AddArc(x0, y0, d, d, 180.f, 90.f);
    path->AddArc(x1 - d, y0, d, d, 270.f, 90.f);
    path->AddArc(x1 - d, y1 - d, d, d, 0.f, 90.f);
    path->AddArc(x0, y1 - d, d, d, 90.f, 90.f);
    path->CloseFigure();
}

int rasterLine(void* context, float x1, float y1, float x2, float y2, uint32_t color,
               float thickness) {
    Raster* raster = rasterOf(context);
    if (!raster || !raster->graphics) return TC_COMPONENT_RENDER_ERR_ARGUMENT;
    Gdiplus::Pen pen(colourOf(color), thickness);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    raster->graphics->DrawLine(&pen, x1, y1, x2, y2);
    return TC_COMPONENT_RENDER_OK;
}

int rasterRect(void* context, float minX, float minY, float maxX, float maxY, uint32_t color,
               float rounding, float thickness) {
    Raster* raster = rasterOf(context);
    if (!raster || !raster->graphics) return TC_COMPONENT_RENDER_ERR_ARGUMENT;
    GraphicsPath path;
    buildRoundedRect(&path, minX, minY, maxX, maxY, rounding);
    Gdiplus::Pen pen(colourOf(color), thickness);
    raster->graphics->DrawPath(&pen, &path);
    return TC_COMPONENT_RENDER_OK;
}

int rasterRectFilled(void* context, float minX, float minY, float maxX, float maxY,
                     uint32_t color, float rounding) {
    Raster* raster = rasterOf(context);
    if (!raster || !raster->graphics) return TC_COMPONENT_RENDER_ERR_ARGUMENT;
    GraphicsPath path;
    buildRoundedRect(&path, minX, minY, maxX, maxY, rounding);
    SolidBrush brush(colourOf(color));
    raster->graphics->FillPath(&brush, &path);
    return TC_COMPONENT_RENDER_OK;
}

int rasterCircle(void* context, float x, float y, float radius, uint32_t color,
                 float thickness) {
    Raster* raster = rasterOf(context);
    if (!raster || !raster->graphics) return TC_COMPONENT_RENDER_ERR_ARGUMENT;
    Gdiplus::Pen pen(colourOf(color), thickness);
    raster->graphics->DrawEllipse(&pen, x - radius, y - radius, radius * 2.f, radius * 2.f);
    return TC_COMPONENT_RENDER_OK;
}

int rasterCircleFilled(void* context, float x, float y, float radius, uint32_t color) {
    Raster* raster = rasterOf(context);
    if (!raster || !raster->graphics) return TC_COMPONENT_RENDER_ERR_ARGUMENT;
    SolidBrush brush(colourOf(color));
    raster->graphics->FillEllipse(&brush, x - radius, y - radius, radius * 2.f, radius * 2.f);
    return TC_COMPONENT_RENDER_OK;
}

int rasterText(void* context, float x, float y, uint32_t color, const char* utf8) {
    Raster* raster = rasterOf(context);
    if (!raster) return TC_COMPONENT_RENDER_ERR_ARGUMENT;
    /* The size-less entry point is what an older loader draws with; the Mod uses
       the sized one, so any size that keeps the run inside the body is fine. */
    return drawRun(raster, widen(utf8), x, y, 12.f, false, colourOf(color))
               ? TC_COMPONENT_RENDER_OK
               : TC_COMPONENT_RENDER_ERR_ARGUMENT;
}

int rasterTextSized(void* context, float x, float y, float size, uint32_t color, int bold,
                    const char* utf8) {
    Raster* raster = rasterOf(context);
    if (!raster) return TC_COMPONENT_RENDER_ERR_ARGUMENT;
    return drawRun(raster, widen(utf8), x, y, size, bold != 0, colourOf(color))
               ? TC_COMPONENT_RENDER_OK
               : TC_COMPONENT_RENDER_ERR_ARGUMENT;
}

/* What the game's own CalcTextSizeA would answer for this run: the ink the run
   will actually occupy at `size`, which is what the Mod's alignment divides by
   (the same numbers the loader's measure_text returns, since the loader passes
   the size straight to CalcTextSizeA). */
int rasterMeasureText(void* context, float size, int bold, const char* utf8, float* width,
                      float* height) {
    Raster* raster = rasterOf(context);
    if (!raster || !raster->family || !width || !height) return TC_COMPONENT_RENDER_ERR_ARGUMENT;
    const RectF ink = inkBox(raster->family, widen(utf8), size, bold != 0);
    if (ink.Width <= 0.f || ink.Height <= 0.f) return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
    *width = ink.Width;
    *height = ink.Height;
    return TC_COMPONENT_RENDER_OK;
}

/* The table the Mod draws through.  Its `context` is the raster the calls land
   in, exactly as the loader's own table carries its draw context. */
TCComponentRenderDrawV2 drawTable(Raster* raster) {
    return TCComponentRenderDrawV2{
        sizeof(TCComponentRenderDrawV2), TC_COMPONENT_RENDER_DRAW_VERSION_2, raster,
        &rasterLine, &rasterRect, &rasterRectFilled, &rasterCircle, &rasterCircleFilled,
        &rasterText, &rasterTextSized, &rasterMeasureText};
}

bool writePng(Gdiplus::Bitmap& bitmap, const std::wstring& path) {
    CLSID png{};
    UINT count = 0, bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok) return false;
    std::vector<unsigned char> buffer(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return false;
    for (UINT index = 0; index < count; ++index)
        if (std::wcscmp(encoders[index].MimeType, L"image/png") == 0) {
            png = encoders[index].Clsid;
            break;
        }
    return png.Data1 && bitmap.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
}

/* The frame the Mod's own callback expects: the board's own local-to-screen
   affine (origin at the canvas centre, one cell per kCellPixels), no rotation,
   the default configuration (a null blob decodes to the type's defaults). */
bool renderIcon(Gdiplus::FontFamily& family, uint64_t customId, const std::string& out) {
    Gdiplus::Bitmap bitmap(kCanvasSize, kCanvasSize, PixelFormat32bppARGB);
    Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.Clear(Color(0, 0, 0, 0));
    Raster raster;
    raster.graphics = &graphics;
    raster.family = &family;
    const TCComponentRenderDrawV2 table = drawTable(&raster);

    TCComponentRenderFrameV1 frame{};
    frame.size = sizeof(frame);
    frame.version = TC_COMPONENT_RENDER_FRAME_VERSION_1;
    frame.custom_id = customId;
    frame.instance_id = 1;
    frame.origin_x = kCanvasSize * 0.5f;
    frame.origin_y = kCanvasSize * 0.5f;
    frame.axis_x_x = kCellPixels;
    frame.axis_x_y = 0.f;
    frame.axis_y_x = 0.f;
    frame.axis_y_y = kCellPixels;
    frame.clip_min_x = 0.f;
    frame.clip_min_y = 0.f;
    frame.clip_max_x = float(kCanvasSize);
    frame.clip_max_y = float(kCanvasSize);
    frame.config = nullptr;
    frame.config_size = 0;
    frame.config_schema = 0;
    /* Handed over through a void* step, exactly like the loader does it: the
       field is typed as V1 because that is what an old Mod reads, while the
       record behind it is the V2 table above (V2 repeats the V1 prefix). */
    frame.draw = static_cast<const TCComponentRenderDrawV1*>(
        static_cast<const void*>(&table));

    floatops::internals::renderCallback(nullptr, &frame);

    const std::wstring wide = widen(out.c_str());
    if (!writePng(bitmap, wide)) {
        std::fprintf(stderr, "icons: cannot write %s\n", out.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: icons <output directory>\n");
        return 2;
    }
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        std::fprintf(stderr, "icons: GDI+ is unavailable\n");
        return 1;
    }
    int status = 0;
    {
        const std::unique_ptr<Gdiplus::FontFamily> family = makeFamily();
        if (!family) {
            std::fprintf(stderr, "icons: no usable UI font\n");
            Gdiplus::GdiplusShutdown(token);
            return 1;
        }
        const std::string directory(argv[1]);
        auto emit = [&](uint64_t id) {
            char path[1024] = {};
            std::snprintf(path, sizeof(path), "%s/%llu.png", directory.c_str(),
                          static_cast<unsigned long long>(id));
            if (!renderIcon(*family, id, path)) status = 1;
            else std::printf("icon %s\n", path);
        };
        emit(floatops::kConstantId);
        emit(floatops::kAddId);
        emit(floatops::kDisplayId);
        const floatops::internals::CatalogueType* catalogue =
            floatops::internals::kCatalogue;
        const size_t count = sizeof(floatops::internals::kCatalogue) /
                             sizeof(floatops::internals::kCatalogue[0]);
        for (size_t index = 0; index < count; ++index) emit(catalogue[index].info.id);
    }
    Gdiplus::GdiplusShutdown(token);
    return status;
}
