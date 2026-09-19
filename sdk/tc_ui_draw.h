#ifndef TC_UI_DRAW_H
#define TC_UI_DRAW_H
/* Optional drawing extension for the pinned engine's cimgui ABI.
   Call loadDrawing(host) after ui::load(host). Construct Canvas only inside a
   visible window/child or a host page callback, on the render thread. All
   coordinates are pixels relative to the canvas, including mousePosition().
   The canvas reserves a layout item and intersects its clip with the window.
   Do not retain a canvas across frames or end its window before destruction.
   No private ImGui structs, second context, or GPU resources are needed. */
#include "tc_ui.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace tc { namespace ui {
using Color = std::uint32_t;
inline constexpr Color rgba(unsigned char r, unsigned char g, unsigned char b,
                            unsigned char a = 255) {
    return Color(r) | (Color(g) << 8) | (Color(b) << 16) | (Color(a) << 24);
}

namespace drawing_detail {
struct Table {
    void* (*windowList)();
    void (*cursorScreen)(Vec2*);
    void (*contentAvailable)(Vec2*);
    void (*pushClip)(void*, Vec2, Vec2, bool);
    void (*popClip)(void*);
    void (*line)(void*, Vec2, Vec2, Color, float);
    void (*rect)(void*, Vec2, Vec2, Color, float, int, float);
    void (*rectFilled)(void*, Vec2, Vec2, Color, float, int);
    void (*gradient)(void*, Vec2, Vec2, Color, Color, Color, Color);
    void (*circle)(void*, Vec2, float, Color, int, float);
    void (*circleFilled)(void*, Vec2, float, Color, int);
    void (*triangle)(void*, Vec2, Vec2, Vec2, Color, float);
    void (*triangleFilled)(void*, Vec2, Vec2, Vec2, Color);
    void (*bezier)(void*, Vec2, Vec2, Vec2, Vec2, Color, float, int);
    void (*polyline)(void*, const Vec2*, int, Color, int, float);
    void (*convex)(void*, const Vec2*, int, Color);
    void (*concave)(void*, const Vec2*, int, Color);
    void (*text)(void*, Vec2, Color, const char*, const char*);
    bool (*itemActive)();
};
inline Table& table() { static Table value{}; return value; }
inline std::string& missing() { static std::string value; return value; }
inline bool finite(float v) { return std::isfinite(v); }
inline bool finite(Vec2 v) { return finite(v.x) && finite(v.y); }
inline bool positive(float v) { return finite(v) && v > 0; }
inline bool segments(int n) { return n == 0 || (n >= 3 && n <= 4096); }
}

inline bool drawingReady() { return drawing_detail::table().windowList != nullptr; }
inline const std::string& drawingMissing() { return drawing_detail::missing(); }

/* Additive capability: failure leaves drawing disabled but ordinary UI usable.
   Resolve into a temporary table, publishing only when ALL exports exist. */
inline bool loadDrawing(const TCHost* host) {
    using namespace drawing_detail;
    drawing_detail::table() = {};
    auto& missingNames = drawing_detail::missing();
    missingNames.clear();
    if (!host || !host->context || !host->engine_proc) {
        missingNames = "engine_proc unavailable";
        return false;
    }
    drawing_detail::Table resolved{};
#define TC_DRAW_BIND(field, name) do { \
    void* entry = host->engine_proc(host->context, name); \
    if (!entry) { if (!missingNames.empty()) missingNames += ", "; missingNames += name; } \
    static_assert(sizeof(resolved.field) == sizeof(entry), "function pointer ABI"); \
    std::memcpy(&resolved.field, &entry, sizeof(entry)); \
} while (0)
    TC_DRAW_BIND(windowList, "igGetWindowDrawList");
    TC_DRAW_BIND(cursorScreen, "igGetCursorScreenPos");
    TC_DRAW_BIND(contentAvailable, "igGetContentRegionAvail");
    TC_DRAW_BIND(pushClip, "ImDrawList_PushClipRect");
    TC_DRAW_BIND(popClip, "ImDrawList_PopClipRect");
    TC_DRAW_BIND(line, "ImDrawList_AddLine");
    TC_DRAW_BIND(rect, "ImDrawList_AddRect");
    TC_DRAW_BIND(rectFilled, "ImDrawList_AddRectFilled");
    TC_DRAW_BIND(gradient, "ImDrawList_AddRectFilledMultiColor");
    TC_DRAW_BIND(circle, "ImDrawList_AddCircle");
    TC_DRAW_BIND(circleFilled, "ImDrawList_AddCircleFilled");
    TC_DRAW_BIND(triangle, "ImDrawList_AddTriangle");
    TC_DRAW_BIND(triangleFilled, "ImDrawList_AddTriangleFilled");
    TC_DRAW_BIND(bezier, "ImDrawList_AddBezierCubic");
    TC_DRAW_BIND(polyline, "ImDrawList_AddPolyline");
    TC_DRAW_BIND(convex, "ImDrawList_AddConvexPolyFilled");
    TC_DRAW_BIND(concave, "ImDrawList_AddConcavePolyFilled");
    TC_DRAW_BIND(text, "ImDrawList_AddText_Vec2");
    TC_DRAW_BIND(itemActive, "igIsItemActive");
#undef TC_DRAW_BIND
    if (!missingNames.empty()) {
        if (host->log) host->log(host->context, ("UI drawing: missing exports: " + missingNames).c_str());
        return false;
    }
    drawing_detail::table() = resolved;
    return true;
}

/* Remaining space in the current window/child, not the whole viewport. */
inline Vec2 contentAvailable() {
    Vec2 result{};
    if (drawingReady()) drawing_detail::table().contentAvailable(&result);
    return result;
}

class Canvas {
    friend class Texture;
    void* list_ = nullptr;
    Vec2 origin_{}, size_{};
    bool hovered_ = false, active_ = false, clicked_ = false;
    void (*pop_)(void*) = nullptr;
    bool point(Vec2 p) const {
        return list_ && drawing_detail::finite(p) && drawing_detail::finite(toScreen(p));
    }
    bool box(Vec2 a, Vec2 b) const { return point(a) && point(b) && b.x > a.x && b.y > a.y; }
    bool stroke(float width) const { return drawing_detail::positive(width); }
    bool points(const Vec2* input, int count, int minimum, std::vector<Vec2>& output) const {
        if (!list_ || !input || count < minimum || count > 16384) return false;
        output.reserve(count);
        for (int i = 0; i < count; ++i) {
            if (!point(input[i])) return false;
            output.push_back(toScreen(input[i]));
        }
        return true;
    }
public:
    /* Positive dimensions required. Each canvas needs a unique ImGui item ID.
       A left-button InvisibleButton supplies standard hover/capture/click
       semantics. Query the snapshot below even after drawing other widgets. */
    Canvas(const char* id, Vec2 size) {
        auto& api = drawing_detail::table();
        if (!drawingReady() || !id || !*id || !ui::table().invisibleButton ||
            !drawing_detail::positive(size.x) || !drawing_detail::positive(size.y)) return;
        api.cursorScreen(&origin_);
        size_ = size;
        if (!drawing_detail::finite(origin_) || !drawing_detail::finite(toScreen(size))) return;
        list_ = api.windowList();
        if (!list_) return;
        clicked_ = invisibleButton(id, size);
        hovered_ = isItemHovered();
        active_ = api.itemActive();
        api.pushClip(list_, origin_, toScreen(size), true);
        pop_ = api.popClip;
    }
    ~Canvas() { if (pop_) pop_(list_); }
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    Canvas(Canvas&&) = delete;
    Canvas& operator=(Canvas&&) = delete;
    explicit operator bool() const { return list_ != nullptr; }
    Vec2 size() const { return size_; }
    Vec2 origin() const { return origin_; }
    Vec2 toScreen(Vec2 local) const { return {origin_.x + local.x, origin_.y + local.y}; }
    Vec2 toLocal(Vec2 screen) const { return {screen.x - origin_.x, screen.y - origin_.y}; }
    Vec2 mousePosition() const { return toLocal(mousePos()); }
    bool hovered() const { return hovered_; }
    bool active() const { return active_; }
    bool clicked() const { return clicked_; }
    bool dragging() const { return active_ && isMouseDown(Mouse_Left); }

    void line(Vec2 a, Vec2 b, Color color, float width = 1) const {
        if (point(a) && point(b) && stroke(width))
            drawing_detail::table().line(list_, toScreen(a), toScreen(b), color, width);
    }
    void rect(Vec2 a, Vec2 b, Color color, float rounding = 0, float width = 1) const {
        if (box(a,b) && stroke(width) && drawing_detail::finite(rounding) && rounding >= 0)
            drawing_detail::table().rect(list_, toScreen(a), toScreen(b), color, rounding, 0, width);
    }
    void rectFilled(Vec2 a, Vec2 b, Color color, float rounding = 0) const {
        if (box(a,b) && drawing_detail::finite(rounding) && rounding >= 0)
            drawing_detail::table().rectFilled(list_, toScreen(a), toScreen(b), color, rounding, 0);
    }
    void gradient(Vec2 a, Vec2 b, Color topLeft, Color topRight, Color bottomRight, Color bottomLeft) const {
        if (box(a,b)) drawing_detail::table().gradient(list_, toScreen(a), toScreen(b),
                                                     topLeft, topRight, bottomRight, bottomLeft);
    }
    void circle(Vec2 center, float radius, Color color, float width = 1, int segments = 0) const {
        if (point(center) && drawing_detail::positive(radius) && stroke(width) && drawing_detail::segments(segments))
            drawing_detail::table().circle(list_, toScreen(center), radius, color, segments, width);
    }
    void circleFilled(Vec2 center, float radius, Color color, int segments = 0) const {
        if (point(center) && drawing_detail::positive(radius) && drawing_detail::segments(segments))
            drawing_detail::table().circleFilled(list_, toScreen(center), radius, color, segments);
    }
    void triangle(Vec2 a, Vec2 b, Vec2 c, Color color, float width = 1) const {
        if (point(a) && point(b) && point(c) && stroke(width))
            drawing_detail::table().triangle(list_, toScreen(a), toScreen(b), toScreen(c), color, width);
    }
    void triangleFilled(Vec2 a, Vec2 b, Vec2 c, Color color) const {
        if (point(a) && point(b) && point(c))
            drawing_detail::table().triangleFilled(list_, toScreen(a), toScreen(b), toScreen(c), color);
    }
    void bezier(Vec2 a, Vec2 control1, Vec2 control2, Vec2 b, Color color,
                float width = 1, int segments = 0) const {
        if (point(a) && point(b) && point(control1) && point(control2) && stroke(width) &&
            segments >= 0 && segments <= 4096)
            drawing_detail::table().bezier(list_, toScreen(a), toScreen(control1), toScreen(control2),
                                          toScreen(b), color, width, segments);
    }
    void polyline(const Vec2* vertices, int count, Color color, bool closed = false, float width = 1) const {
        std::vector<Vec2> translated;
        if (stroke(width) && points(vertices, count, closed ? 3 : 2, translated))
            drawing_detail::table().polyline(list_, translated.data(), count, color, closed ? 1 : 0, width);
    }
    /* Filled contours must be simple, clockwise in screen space, without a
       repeated closing vertex or holes. convexFilled additionally requires
       convex input; use concaveFilled for concave contours. */
    void convexFilled(const Vec2* vertices, int count, Color color) const {
        std::vector<Vec2> translated;
        if (points(vertices, count, 3, translated))
            drawing_detail::table().convex(list_, translated.data(), count, color);
    }
    void concaveFilled(const Vec2* vertices, int count, Color color) const {
        std::vector<Vec2> translated;
        if (points(vertices, count, 3, translated))
            drawing_detail::table().concave(list_, translated.data(), count, color);
    }
    void text(Vec2 position, Color color, const char* utf8) const {
        if (point(position) && utf8)
            drawing_detail::table().text(list_, toScreen(position), color, utf8, nullptr);
    }

    /* Optional nested clip, still in canvas coordinates. Always intersects
       existing clips and restores the previous clip on scope exit. */
    class Clip {
        void* list_ = nullptr;
        void (*pop_)(void*) = nullptr;
    public:
        Clip(const Canvas& canvas, Vec2 minimum, Vec2 maximum) {
            if (!canvas.point(minimum) || !canvas.point(maximum) ||
                maximum.x < minimum.x || maximum.y < minimum.y) return;
            list_ = canvas.list_;
            auto& api = drawing_detail::table();
            api.pushClip(list_, canvas.toScreen(minimum), canvas.toScreen(maximum), true);
            pop_ = api.popClip;
        }
        ~Clip() { if (pop_) pop_(list_); }
        Clip(const Clip&) = delete;
        Clip& operator=(const Clip&) = delete;
    };
};
}}
#endif
