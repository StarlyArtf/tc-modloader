#include "setup_window.hpp"

#include "compat.hpp"
#include "setup_core.hpp"
#include "setup_discovery.hpp"
#include "setup_state.hpp"
#include "version.hpp"

#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace tc::setup {
namespace {

constexpr int kAddButton = 1001;
constexpr int kPrimaryButton = 1002;
constexpr int kSecondaryButton = 1003;
constexpr int kMoreButton = 1004;
constexpr int kAppIcon = 100;

constexpr UINT kScanDone = WM_APP + 1;
constexpr UINT kWorkPhase = WM_APP + 2;
constexpr UINT kWorkDone = WM_APP + 3;

struct Palette {
    COLORREF window;
    COLORREF header;
    COLORREF panel;
    COLORREF panelRaised;
    COLORREF border;
    COLORREF text;
    COLORREF muted;
    COLORREF accent;
    COLORREF good;
    COLORREF warn;
    COLORREF onAccent;
    bool system;  /* high contrast: draw with the system's own colors */
};

struct ScanRequest {
    HWND window = nullptr;
    Candidate initial;
    std::vector<Candidate> manual;
};

struct WorkRequest {
    HWND window = nullptr;
    Action action = Action::install;
    tc::fs::path root;
    InstallationSnapshot before;
};

struct WorkOutcome {
    Action action = Action::install;
    SetupResult result;
    InstallationSnapshot snapshot;
    LoaderState before_state = LoaderState::not_installed;
    std::string before_engine;
    bool cancelled = false;
};

struct Ui {
    HWND window = nullptr;
    HWND addButton = nullptr;
    HWND primaryButton = nullptr;
    HWND secondaryButton = nullptr;
    HWND moreButton = nullptr;
    HFONT titleFont = nullptr;
    HFONT headingFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT smallFont = nullptr;
    std::wstring bodyFace;
    Palette palette{};
    std::vector<InstallationSnapshot> installs;
    std::vector<Candidate> manual;
    Candidate initial;
    std::wstring focus_root;  /* select this location when the scan lands */
    size_t selected = 0;
    bool scanning = false;
    bool working = false;
    std::wstring phase;
    std::wstring close_note;  /* shown when a close request arrives mid-write */
    bool confirming_uninstall = false;
    ResultKind result = ResultKind::none;
    std::wstring result_title;
    std::wstring result_body;
    std::wstring evidence;
    bool technical_open = false;
    std::wstring result_technical;  /* the core's detail for the banner above */
    std::vector<MenuEntry> menu;
    UINT dpi = 96;
};

Ui ui;

int scale(int value) { return MulDiv(value, static_cast<int>(ui.dpi), 96); }

UINT dpi_of(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return get_dpi ? get_dpi(window) : 96;
}

std::wstring lowered(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return value;
}

/* ---------------------------------------------------------------- theme --- */

bool high_contrast() {
    HIGHCONTRASTW scheme{};
    scheme.cbSize = sizeof(scheme);
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(scheme), &scheme, 0)) return false;
    return (scheme.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

/* Windows 10/11 expose the "choose your default app mode" switch here. */
bool apps_use_light_theme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) !=
        ERROR_SUCCESS)
        return true;
    return value != 0;
}

Palette make_palette() {
    Palette palette{};
    if (high_contrast()) {
        palette.window = GetSysColor(COLOR_WINDOW);
        palette.header = GetSysColor(COLOR_BTNFACE);
        palette.panel = GetSysColor(COLOR_WINDOW);
        palette.panelRaised = GetSysColor(COLOR_BTNFACE);
        palette.border = GetSysColor(COLOR_WINDOWFRAME);
        palette.text = GetSysColor(COLOR_WINDOWTEXT);
        palette.muted = GetSysColor(COLOR_GRAYTEXT);
        palette.accent = GetSysColor(COLOR_HIGHLIGHT);
        palette.good = GetSysColor(COLOR_WINDOWTEXT);
        palette.warn = GetSysColor(COLOR_WINDOWTEXT);
        palette.onAccent = GetSysColor(COLOR_HIGHLIGHTTEXT);
        palette.system = true;
        return palette;
    }
    palette.system = false;
    palette.onAccent = RGB(8, 26, 36);
    if (apps_use_light_theme()) {
        palette.window = RGB(246, 248, 250);
        palette.header = RGB(236, 240, 245);
        palette.panel = RGB(255, 255, 255);
        palette.panelRaised = RGB(241, 245, 249);
        palette.border = RGB(208, 216, 226);
        palette.text = RGB(24, 32, 42);
        palette.muted = RGB(94, 108, 124);
        palette.accent = RGB(0, 110, 175);
        palette.good = RGB(21, 128, 61);
        palette.warn = RGB(176, 98, 0);
        palette.onAccent = RGB(255, 255, 255);
        return palette;
    }
    palette.window = RGB(17, 24, 33);
    palette.header = RGB(20, 28, 38);
    palette.panel = RGB(23, 32, 43);
    palette.panelRaised = RGB(29, 41, 54);
    palette.border = RGB(49, 64, 79);
    palette.text = RGB(242, 246, 250);
    palette.muted = RGB(157, 174, 192);
    palette.accent = RGB(50, 190, 239);
    palette.good = RGB(84, 214, 112);
    palette.warn = RGB(255, 184, 47);
    return palette;
}

int CALLBACK family_exists_callback(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM data) {
    *reinterpret_cast<bool*>(data) = true;
    return 0;
}

bool family_exists(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW request{};
    request.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(request.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    EnumFontFamiliesExW(dc, &request, family_exists_callback,
                        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

HFONT make_font(int size, int weight) {
    return CreateFontW(-MulDiv(size, static_cast<int>(ui.dpi), 96), 0, 0, 0, weight, FALSE,
                       FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                       ui.bodyFace.c_str());
}

void rebuild_palette() {
    ui.palette = make_palette();
    if (!ui.window) return;
    BOOL dark = ui.palette.system || apps_use_light_theme() ? FALSE : TRUE;
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (auto set_attribute = reinterpret_cast<DwmSetWindowAttributeFn>(
                GetProcAddress(dwm, "DwmSetWindowAttribute")))
            set_attribute(ui.window, 20, &dark, sizeof(dark));
        FreeLibrary(dwm);
    }
    InvalidateRect(ui.window, nullptr, TRUE);
}

void rebuild_fonts() {
    for (HFONT font : {ui.titleFont, ui.headingFont, ui.bodyFont, ui.smallFont})
        if (font) DeleteObject(font);
    ui.titleFont = make_font(27, FW_SEMIBOLD);
    ui.headingFont = make_font(18, FW_SEMIBOLD);
    ui.bodyFont = make_font(15, FW_NORMAL);
    ui.smallFont = make_font(13, FW_NORMAL);
}

/* The installer's own icon, embedded as resource 100; IDI_APPLICATION is only
   the fallback for a build compiled without the resource script. */
HICON app_icon(int size) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (HICON icon = reinterpret_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(kAppIcon), IMAGE_ICON, size, size, LR_SHARED)))
        return icon;
    return LoadIconW(nullptr, IDI_APPLICATION);
}

/* ------------------------------------------------------------- geometry --- */

struct Geometry {
    RECT client{};
    RECT header{};
    RECT leftPanel{};
    RECT rightPanel{};
    RECT content{};   /* text area inside the right panel */
    RECT buttons{};   /* the row the three right-hand buttons sit on */
};

Geometry geometry() {
    Geometry out;
    GetClientRect(ui.window, &out.client);
    out.header = RECT{0, 0, out.client.right, scale(64)};
    const int panelTop = scale(148);
    const int panelBottom = out.client.bottom - scale(24);
    const int split = std::max(scale(355), static_cast<int>(out.client.right * 37 / 100));
    out.leftPanel = RECT{scale(20), panelTop, split - scale(8), panelBottom};
    out.rightPanel = RECT{split + scale(8), panelTop, out.client.right - scale(20), panelBottom};
    out.content = RECT{out.rightPanel.left + scale(34), out.rightPanel.top + scale(22),
                       out.rightPanel.right - scale(34), out.rightPanel.bottom};
    const int top = out.client.bottom - scale(106);
    out.buttons = RECT{out.content.left, top, out.content.right, top + scale(42)};
    return out;
}

RECT list_item_rect(size_t index) {
    const Geometry layout = geometry();
    const int left = layout.leftPanel.left + scale(14);
    const int top = layout.leftPanel.top + scale(12) +
                    static_cast<int>(index) * scale(98);
    const int width = layout.leftPanel.right - layout.leftPanel.left - scale(28);
    return RECT{left, top, left + width, top + scale(86)};
}

/* The right panel keeps one fixed order - name, version rows, safety checks,
   banner, trust line, buttons - so both the painter and the banner share these
   row tops instead of each computing its own. */
std::wstring technical_text();

struct RightRows {
    int title = 0;
    int path = 0;
    int divider = 0;
    int version_label = 0;
    int loader_label = 0;
    int divider2 = 0;
    int safety_label = 0;
    int lines = 0;
};

RightRows right_rows(const Geometry& layout) {
    RightRows rows;
    rows.title = layout.content.top;
    rows.path = rows.title + scale(30);
    rows.divider = rows.path + scale(32);
    rows.version_label = rows.divider + scale(13);
    rows.loader_label = rows.version_label + scale(26);
    rows.divider2 = rows.loader_label + scale(24) + scale(12);
    rows.safety_label = rows.divider2 + scale(8);
    rows.lines = rows.safety_label + scale(28);
    return rows;
}

/* A banner - working, confirming or a result - takes the place of the safety
   block, which is what "右侧详情原地变成成功状态" means: the player reads the
   outcome where the checks were, in the same fixed hierarchy.  It stops above
   the trust line so the "nothing is deleted" promise stays visible. */
RECT banner_rect(const Geometry& layout) {
    const RightRows rows = right_rows(layout);
    const int bottom = layout.buttons.top - scale(52);
    return RECT{layout.content.left, rows.safety_label, layout.content.right, bottom};
}

/* --------------------------------------------------------------- drawing -- */

void draw_text(HDC dc, const std::wstring& value, RECT rect, COLORREF color, HFONT font,
               UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
    if (value.empty()) return;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ previous = SelectObject(dc, font);
    DrawTextW(dc, value.c_str(), -1, &rect, format);
    SelectObject(dc, previous);
}

void fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void frame(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}

void status_dot(HDC dc, int x, int y, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_NULL, 0, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    const int size = scale(10);
    Ellipse(dc, x, y, x + size, y + size);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

COLORREF tone_color(LineTone tone) {
    switch (tone) {
        case LineTone::good: return ui.palette.good;
        case LineTone::warn: return ui.palette.warn;
        default: return ui.palette.muted;
    }
}

void paint_list_item(HDC dc, size_t index, const RECT& row) {
    const auto& item = ui.installs[index];
    fill(dc, row, index == ui.selected ? ui.palette.panelRaised : ui.palette.panel);
    frame(dc, row, index == ui.selected ? ui.palette.accent : ui.palette.border);
    RECT source{row.left + scale(18), row.top + scale(10), row.right - scale(14),
                row.top + scale(38)};
    draw_text(dc, item.source_label, source, ui.palette.text, ui.headingFont);
    RECT path{source.left, source.bottom, source.right, source.bottom + scale(25)};
    draw_text(dc, item.root.wstring(), path, ui.palette.muted, ui.smallFont);
    const COLORREF color = item.compatibility == Compatibility::compatible
                              ? ui.palette.good
                              : (item.compatibility == Compatibility::checking
                                     ? ui.palette.muted
                                     : ui.palette.warn);
    status_dot(dc, source.left, row.bottom - scale(23), color);
    RECT status{source.left + scale(20), row.bottom - scale(31), row.right - scale(10),
                row.bottom - scale(7)};
    draw_text(dc, compatibility_text(item), status, color, ui.smallFont);
}

bool banner_active() {
    return ui.working || ui.confirming_uninstall || ui.result != ResultKind::none;
}

void paint_banner(HDC dc, const Geometry& layout) {
    const RECT banner = banner_rect(layout);
    std::wstring title;
    std::wstring body;
    COLORREF tone = ui.palette.accent;
    if (ui.working) {
        title = L"正在安全写入";
        body = ui.phase;
        if (!ui.close_note.empty()) body += L"\n" + ui.close_note;
        tone = ui.palette.accent;
    } else if (ui.confirming_uninstall) {
        title = L"卸载 Mod Loader？";
        body = L"恢复原始引擎，但保留 Mod、配置和存档。\n确认后立即开始，中途不能取消。";
        tone = ui.palette.warn;
    } else {
        title = ui.result_title;
        body = ui.result_body;
        if (!ui.evidence.empty()) body += L"\n" + ui.evidence;
        tone = result_is_success(ui.result) ? ui.palette.good : ui.palette.warn;
    }
    {
        fill(dc, banner, ui.palette.panelRaised);
        frame(dc, banner, tone);
        RECT heading{banner.left + scale(16), banner.top + scale(10),
                     banner.right - scale(16), banner.top + scale(38)};
        draw_text(dc, title, heading, ui.palette.text, ui.headingFont);
        RECT text{heading.left, heading.bottom, heading.right, banner.bottom - scale(8)};
        draw_text(dc, body, text, ui.palette.muted, ui.bodyFont,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }
}

void paint_right(HDC dc, const Geometry& layout) {
    const auto* item =
        ui.selected < ui.installs.size() ? &ui.installs[ui.selected] : nullptr;
    if (!item) {
        RECT empty{layout.content.left, layout.content.top + scale(60),
                   layout.content.right, layout.content.top + scale(190)};
        draw_text(dc, L"添加一个游戏位置后，这里会显示兼容性和安装状态。", empty,
                  ui.palette.muted, ui.headingFont,
                  DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        return;
    }
    const RightRows rows = right_rows(layout);
    const int left = layout.content.left;
    const int right = layout.content.right;
    RECT game{left, rows.title, right, rows.title + scale(30)};
    draw_text(dc, L"Turing Complete · " + item->source_label, game, ui.palette.text,
              ui.titleFont);
    RECT path{left, rows.path, right, rows.path + scale(24)};
    draw_text(dc, item->root.wstring(), path, ui.palette.muted, ui.bodyFont);
    if (ui.technical_open) {
        /* 查看技术信息 replaces the whole detail block - the version rows, the
           dividers and the safety checks - because the raw facts need the room
           and they already say which build and which engine are in there. */
        RECT block{left, path.bottom + scale(10), right, layout.buttons.top - scale(52)};
        fill(dc, block, ui.palette.panel);
        frame(dc, block, ui.palette.border);
        RECT text{block.left + scale(14), block.top + scale(10), block.right - scale(14),
                  block.bottom - scale(8)};
        draw_text(dc, technical_text(), text, ui.palette.muted, ui.smallFont,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        RECT trust{left, layout.buttons.top - scale(44), right, layout.buttons.top - scale(20)};
        draw_text(dc, L"安装不会删除 Mod、配置或存档。", trust, ui.palette.muted,
                  ui.smallFont);
        return;
    }
    RECT divider{left, rows.divider, right, rows.divider + scale(1)};
    fill(dc, divider, ui.palette.border);

    RECT version_label{left, rows.version_label, left + scale(150),
                       rows.version_label + scale(24)};
    draw_text(dc, L"游戏版本", version_label, ui.palette.muted, ui.bodyFont);
    RECT version_value{version_label.right, version_label.top, right, version_label.bottom};
    draw_text(dc,
              item->compatibility == Compatibility::compatible
                  ? item->game_version + L"（受支持的构建）"
                  : compatibility_text(*item),
              version_value,
              item->compatibility == Compatibility::compatible ? ui.palette.text
                                                               : ui.palette.warn,
              ui.bodyFont);
    RECT loader_label{left, rows.loader_label, version_label.right,
                      rows.loader_label + scale(24)};
    draw_text(dc, L"Mod Loader", loader_label, ui.palette.muted, ui.bodyFont);
    RECT loader_value{loader_label.right, loader_label.top, right, loader_label.bottom};
    draw_text(dc, loader_text(*item), loader_value, ui.palette.text, ui.bodyFont);

    RECT divider2{left, rows.divider2, right, rows.divider2 + scale(1)};
    fill(dc, divider2, ui.palette.border);
    if (banner_active()) {
        paint_banner(dc, layout);
    } else {
        RECT safety{left, rows.safety_label, right, rows.safety_label + scale(24)};
        draw_text(dc, L"安全检查", safety, ui.palette.text, ui.headingFont);
        const auto lines = safety_lines(*item);
        const auto tones = safety_tones(*item);
        int y = rows.lines;
        for (size_t index = 0; index < lines.size(); ++index) {
            const COLORREF color =
                tone_color(index < tones.size() ? tones[index] : LineTone::normal);
            status_dot(dc, left, y + scale(5), color);
            RECT line{left + scale(20), y, right, y + scale(26)};
            draw_text(dc, lines[index], line, color, ui.bodyFont);
            y += scale(26);
        }
    }
    RECT trust{left, layout.buttons.top - scale(44), right, layout.buttons.top - scale(20)};
    draw_text(dc, L"安装不会删除 Mod、配置或存档。", trust, ui.palette.muted, ui.smallFont);
}

void paint() {
    PAINTSTRUCT paint_state{};
    HDC dc = BeginPaint(ui.window, &paint_state);
    const Geometry layout = geometry();
    fill(dc, layout.client, ui.palette.window);
    fill(dc, layout.header, ui.palette.header);
    RECT product{scale(24), 0, layout.client.right - scale(24), layout.header.bottom};
    draw_text(dc, L"TC Mod Loader", product, ui.palette.text, ui.headingFont);
    RECT version{layout.client.right - scale(240), 0, layout.client.right - scale(24),
                 layout.header.bottom};
    draw_text(dc, L"安装器 " TC_MODLOADER_VERSION_WTEXT, version, ui.palette.muted,
              ui.smallFont, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    RECT title{scale(28), scale(72), layout.client.right - scale(28), scale(108)};
    draw_text(dc, L"选择要管理的游戏", title, ui.palette.text, ui.titleFont);
    const std::wstring count =
        ui.scanning ? L"正在检查 Turing Complete 安装位置…"
                    : (ui.installs.empty()
                           ? L"尚未检测到 Turing Complete 安装位置"
                           : L"检测到 " + std::to_wstring(ui.installs.size()) +
                                 L" 个 Turing Complete 安装位置");
    RECT subtitle{title.left, title.bottom, title.right, scale(140)};
    draw_text(dc, count, subtitle, ui.palette.muted, ui.bodyFont);

    fill(dc, layout.leftPanel, ui.palette.panel);
    frame(dc, layout.leftPanel, ui.palette.border);
    fill(dc, layout.rightPanel, ui.palette.panel);
    frame(dc, layout.rightPanel, ui.palette.border);

    /* A list longer than the column scrolls off the bottom edge; clipping keeps
       the rows inside their own panel instead of drawing over the frame. */
    const int clip = SaveDC(dc);
    IntersectClipRect(dc, layout.leftPanel.left + 1, layout.leftPanel.top + 1,
                      layout.leftPanel.right - 1, layout.leftPanel.bottom - 1);
    for (size_t index = 0; index < ui.installs.size(); ++index)
        paint_list_item(dc, index, list_item_rect(index));
    RestoreDC(dc, clip);

    if (ui.installs.empty() && !ui.scanning) {
        RECT empty{layout.leftPanel.left + scale(20), layout.leftPanel.top + scale(28),
                   layout.leftPanel.right - scale(20), layout.leftPanel.top + scale(110)};
        draw_text(dc, L"没有自动找到游戏\n选择 Turing Complete.exe 所在位置", empty,
                  ui.palette.muted, ui.bodyFont,
                  DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }
    paint_right(dc, layout);
    EndPaint(ui.window, &paint_state);
}

void draw_button(const DRAWITEMSTRUCT& item) {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool primary = item.CtlID == kPrimaryButton;
    COLORREF background = ui.palette.panelRaised;
    COLORREF border = ui.palette.border;
    COLORREF foreground = disabled ? ui.palette.muted : ui.palette.text;
    if (primary && !disabled) {
        background = pressed ? ui.palette.border : ui.palette.accent;
        border = background;
        foreground = ui.palette.onAccent;
    } else if (pressed && !disabled) {
        background = ui.palette.border;
    }
    if (disabled) background = ui.palette.panel;
    fill(item.hDC, item.rcItem, background);
    frame(item.hDC, item.rcItem, border);
    wchar_t label[256]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    RECT text_rect = item.rcItem;
    draw_text(item.hDC, label, text_rect, foreground, ui.bodyFont,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (item.itemState & ODS_FOCUS) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(item.hDC, &focus);
    }
}

/* ----------------------------------------------------------- operations --- */

void rescan();
void update_controls();

InstallationSnapshot* selected_snapshot() {
    return ui.selected < ui.installs.size() ? &ui.installs[ui.selected] : nullptr;
}

void replace_snapshot(const InstallationSnapshot& snapshot) {
    const std::wstring key = lowered(snapshot.root.wstring());
    for (auto& item : ui.installs) {
        if (lowered(item.root.wstring()) == key) {
            auto label = item.source_label;
            item = snapshot;
            if (item.source_label.empty()) item.source_label = label;
            return;
        }
    }
    ui.installs.push_back(snapshot);
    ui.selected = ui.installs.size() - 1;
}

void open_path(const tc::fs::path& path) {
    ShellExecuteW(ui.window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

/* What 查看技术信息 shows: the failure's own detail when there is one, the
   current read-only inspection otherwise. */
std::wstring technical_text() {
    if (ui.result != ResultKind::none && !ui.result_technical.empty())
        return ui.result_technical;
    const auto* item = selected_snapshot();
    return item ? item->technical_detail : std::wstring{};
}

void copy_to_clipboard(const std::wstring& text) {
    if (text.empty()) return;
    if (!OpenClipboard(ui.window)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* target = GlobalLock(memory)) {
            memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
        }
    }
    CloseClipboard();
}

void choose_location() {
    if (ui.working) return;
    wchar_t file[32768] = L"Turing Complete.exe";
    OPENFILENAMEW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = ui.window;
    picker.lpstrFilter = L"Turing Complete.exe\0Turing Complete.exe\0\0";
    picker.lpstrFile = file;
    picker.nMaxFile = static_cast<DWORD>(std::size(file));
    picker.lpstrTitle = L"选择游戏目录中的 Turing Complete.exe";
    picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&picker)) return;
    const auto root = tc::fs::path(file).parent_path();
    ui.manual.push_back({root, L"手动添加"});
    ui.focus_root = tc::fs::absolute(root).lexically_normal().wstring();
    rescan();
}

void start_work(Action action, const tc::fs::path& root);

void run_action(Action action) {
    const auto* item = selected_snapshot();
    switch (action) {
        case Action::none: return;
        case Action::choose_location: choose_location(); return;
        case Action::install:
        case Action::repair:
        case Action::upgrade:
            if (item) start_work(Action::install, item->root);
            return;
        case Action::uninstall:
            ui.confirming_uninstall = true;
            update_controls();
            return;
        case Action::confirm_uninstall:
            if (item) start_work(Action::uninstall, item->root);
            return;
        case Action::cancel:
            ui.confirming_uninstall = false;
            update_controls();
            return;
        case Action::retry:
        case Action::rescan:
            rescan();
            return;
        case Action::elevate:
            if (item) start_work(Action::elevate, item->root);
            return;
        case Action::dismiss:
            ui.result = ResultKind::none;
            ui.result_title.clear();
            ui.result_body.clear();
            ui.result_technical.clear();
            ui.evidence.clear();
            ui.technical_open = false;
            ui.confirming_uninstall = false;
            update_controls();
            return;
        case Action::open_directory:
            if (item) open_path(item->root);
            return;
        case Action::open_mods:
            if (item) open_path(item->root / L"mods");
            return;
        case Action::open_log:
            if (item) open_path(item->root / L"tc-modloader-data" / L"loader.log");
            return;
        case Action::technical:
            ui.technical_open = !ui.technical_open;
            update_controls();
            return;
        case Action::copy_technical:
            copy_to_clipboard(technical_text());
            return;
    }
}

void show_more() {
    if (ui.working || ui.menu.empty()) return;
    HMENU menu = CreatePopupMenu();
    int id = 1;
    for (const auto& entry : ui.menu) {
        if (entry.label.empty()) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        AppendMenuW(menu, MF_STRING | (entry.enabled ? 0 : MF_GRAYED), id++, entry.label.c_str());
    }
    RECT anchor{};
    GetWindowRect(ui.moreButton, &anchor);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN,
                                       anchor.right, anchor.bottom, 0, ui.window, nullptr);
    DestroyMenu(menu);
    if (command <= 0) return;
    size_t index = 0;
    for (const auto& entry : ui.menu) {
        if (entry.label.empty()) continue;
        if (++index == static_cast<size_t>(command)) {
            run_action(entry.action);
            return;
        }
    }
}

ActionSet result_actions() {
    ActionSet set;
    if (ui.confirming_uninstall) {
        set.primary = Action::confirm_uninstall;
        set.primary_label = L"确认卸载并恢复";
        set.primary_enabled = true;
        set.secondary = Action::cancel;
        set.secondary_label = L"取消";
        return set;
    }
    if (ui.result != ResultKind::none) {
        set.primary = Action::dismiss;
        set.primary_label = L"完成";
        set.primary_enabled = true;
        if (result_is_success(ui.result)) {
            set.secondary = Action::open_mods;
            set.secondary_label = L"打开 Mods 文件夹";
            set.menu = {{Action::open_log, L"查看安装日志", true},
                        {Action::open_directory, L"打开游戏目录", true},
                        {Action::rescan, L"重新检查", true},
                        {Action::technical, L"查看技术信息", true}};
        } else {
            set.secondary = Action::technical;
            set.secondary_label = ui.technical_open ? L"隐藏技术信息" : L"查看技术信息";
            set.menu = {{Action::copy_technical, L"复制技术信息", true},
                        {Action::rescan, L"重新检查", true},
                        {Action::open_directory, L"打开游戏目录", true},
                        {Action::choose_location, L"选择其他位置", true}};
        }
        return set;
    }
    if (const auto* item = selected_snapshot()) return actions_for(*item);
    set.primary = Action::choose_location;
    set.primary_label = L"选择游戏位置";
    set.primary_enabled = true;
    set.menu = {{Action::choose_location, L"添加其他位置", true},
                {Action::rescan, L"重新检查", true}};
    return set;
}

void update_controls() {
    ActionSet set = result_actions();
    if (ui.working) {
        set.primary = Action::none;
        set.primary_label = ui.phase.empty() ? L"正在安全写入…" : ui.phase;
        set.primary_enabled = false;
        set.secondary = Action::none;
        set.menu.clear();
    }
    SetWindowTextW(ui.primaryButton, set.primary_label.c_str());
    EnableWindow(ui.primaryButton, set.primary_enabled && !ui.working);
    SetWindowTextW(ui.secondaryButton, set.secondary_label.c_str());
    const bool secondary = set.secondary != Action::none && !ui.working;
    ShowWindow(ui.secondaryButton, secondary ? SW_SHOW : SW_HIDE);
    EnableWindow(ui.secondaryButton, secondary);
    EnableWindow(ui.moreButton, !ui.menu.empty() && !ui.working);
    EnableWindow(ui.addButton, !ui.working && !ui.scanning);
    ui.menu = set.menu;
    InvalidateRect(ui.window, nullptr, TRUE);
}

/* Every write goes through the worker: hashing a 10 MB executable on the UI
   thread is what made the old installer look frozen. */
DWORD WINAPI work_thread(void* parameter) {
    std::unique_ptr<WorkRequest> request(static_cast<WorkRequest*>(parameter));
    const auto phase = [window = request->window](SetupPhase stage) {
        const wchar_t* text = L"正在再次检查游戏…";
        switch (stage) {
            case SetupPhase::rechecking: text = L"正在再次检查游戏…"; break;
            case SetupPhase::backing_up: text = L"正在备份原始引擎…"; break;
            case SetupPhase::deploying: text = L"正在安装 Mod Loader…"; break;
            case SetupPhase::verifying: text = L"正在验证安装结果…"; break;
        }
        PostMessageW(window, kWorkPhase, 0,
                     reinterpret_cast<LPARAM>(new std::wstring(text)));
    };
    auto* outcome = new WorkOutcome();
    outcome->action = request->action;
    outcome->before_state = request->before.loader_state;
    outcome->before_engine = request->before.engine_sha256;
    if (request->action == Action::elevate) {
        PostMessageW(request->window, kWorkPhase, 0,
                     reinterpret_cast<LPARAM>(new std::wstring(L"正在等待管理员授权…")));
        wchar_t module[32768]{};
        GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
        std::wstring parameters = L"--install \"" + request->root.wstring() + L"\"";
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpVerb = L"runas";
        info.lpFile = module;
        info.lpParameters = parameters.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) {
            const DWORD error = GetLastError();
            if (error == ERROR_CANCELLED) {
                outcome->cancelled = true;
                outcome->result.user_message = L"未进行任何更改。";
            } else {
                outcome->result.error = SetupError::permission_denied;
                outcome->result.user_message = describe(SetupError::permission_denied);
                outcome->result.technical_detail =
                    L"无法以管理员身份重新启动安装器（错误 " +
                    std::to_wstring(static_cast<unsigned long>(error)) + L"）。\n";
            }
        } else {
            WaitForSingleObject(info.hProcess, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(info.hProcess, &code);
            CloseHandle(info.hProcess);
            outcome->snapshot = inspect(request->root, request->before.source_label);
            const bool installed =
                outcome->snapshot.loader_state == LoaderState::installed_current;
            if (installed) {
                outcome->result.user_message =
                    L"Mod Loader " TC_MODLOADER_VERSION_WTEXT
                    L" 已安装\n原始引擎已安全备份\n\n把 .mod 文件放入 Mods 文件夹，然后正常启动"
                    L"游戏，在主菜单 Mods 页面启用。";
            } else {
                outcome->result.error =
                    code == 0 ? SetupError::verification_failed : SetupError::io_error;
                outcome->result.user_message =
                    L"管理员进程没有完成安装，游戏保持不变。";
                outcome->result.technical_detail = outcome->snapshot.technical_detail +
                                                   L"提升进程退出码：" +
                                                   std::to_wstring(code) + L"\n";
            }
        }
    } else {
        outcome->result = request->action == Action::uninstall
                              ? uninstall(request->root, phase)
                              : install(request->root, phase);
        outcome->snapshot = inspect(request->root, request->before.source_label);
    }
    if (outcome->snapshot.root.empty()) outcome->snapshot = request->before;
    PostMessageW(request->window, kWorkDone, 0,
                 reinterpret_cast<LPARAM>(outcome));
    return 0;
}

void start_work(Action action, const tc::fs::path& root) {
    if (ui.working || ui.scanning) return;
    auto* request = new WorkRequest();
    request->window = ui.window;
    request->action = action;
    request->root = root;
    if (const auto* item = selected_snapshot()) request->before = *item;
    ui.working = true;
    ui.phase = L"正在再次检查游戏…";
    ui.close_note.clear();
    ui.confirming_uninstall = false;
    update_controls();
    HANDLE thread = CreateThread(nullptr, 0, work_thread, request, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
        delete request;
}

DWORD WINAPI scan_thread(void* parameter) {
    std::unique_ptr<ScanRequest> request(static_cast<ScanRequest*>(parameter));
    std::vector<Candidate> candidates;
    if (!request->initial.root.empty()) candidates.push_back(request->initial);
    const std::wstring steam = registry_steam_path();
    if (!steam.empty()) {
        std::wstring text;
        std::ifstream input(tc::fs::path(steam) / L"steamapps" / L"libraryfolders.vdf",
                            std::ios::binary);
        if (input) {
            const std::string bytes((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
            text = widen(bytes);
        }
        for (const auto& candidate : steam_candidates(steam, text))
            if (looks_like_game(candidate.root)) candidates.push_back(candidate);
    }
    for (const auto& candidate : request->manual) candidates.push_back(candidate);
    std::error_code error;
    const auto base = tc::fs::current_path(error);
    auto* snapshots = new std::vector<InstallationSnapshot>();
    for (const auto& candidate :
         merge_candidates(candidates, error ? tc::fs::path{} : base))
        snapshots->push_back(inspect(candidate.root, candidate.source));
    PostMessageW(request->window, kScanDone, 0, reinterpret_cast<LPARAM>(snapshots));
    return 0;
}

void rescan() {
    if (ui.working) return;
    if (ui.scanning) return;
    auto* request = new ScanRequest();
    request->window = ui.window;
    request->initial = ui.initial;
    request->manual = ui.manual;
    ui.scanning = true;
    update_controls();
    HANDLE thread = CreateThread(nullptr, 0, scan_thread, request, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
        delete request;
}

void finish_scan(std::vector<InstallationSnapshot>* snapshots) {
    std::unique_ptr<std::vector<InstallationSnapshot>> owned(snapshots);
    const std::wstring previous = ui.focus_root.empty()
                                      ? (selected_snapshot() ? selected_snapshot()->root.wstring()
                                                             : std::wstring{})
                                      : ui.focus_root;
    ui.focus_root.clear();
    ui.installs = *owned;
    ui.selected = 0;
    for (size_t index = 0; index < ui.installs.size(); ++index) {
        if (lowered(ui.installs[index].root.wstring()) == lowered(previous))
            ui.selected = index;
    }
    ui.scanning = false;
    update_controls();
}

void finish_work(WorkOutcome* outcome) {
    std::unique_ptr<WorkOutcome> owned(outcome);
    ui.working = false;
    ui.phase.clear();
    ui.close_note.clear();
    replace_snapshot(outcome->snapshot);
    if (outcome->cancelled) {
        ui.result = ResultKind::nothing_changed;
    } else if (outcome->result.error == SetupError::none) {
        if (outcome->action == Action::uninstall)
            ui.result = ResultKind::uninstalled;
        else if (outcome->before_state == LoaderState::installed_current)
            ui.result = ResultKind::repaired;
        else if (outcome->before_state == LoaderState::installed_previous)
            ui.result = ResultKind::upgraded;
        else
            ui.result = ResultKind::installed;
    } else {
        ui.result = ResultKind::failed;
    }
    if (ui.result == ResultKind::failed) {
        ui.result_body = outcome->result.user_message;
        ui.result_technical = outcome->result.technical_detail;
        const bool untouched = !outcome->snapshot.engine_sha256.empty() &&
                               outcome->snapshot.engine_sha256 == outcome->before_engine;
        if (untouched)
            ui.evidence = L"游戏文件没有改变：引擎哈希与操作前一致。";
        else if (outcome->snapshot.loader_state == LoaderState::not_installed)
            ui.evidence = L"已恢复到操作前的状态。";
        else
            ui.evidence = L"引擎哈希已改变，请查看技术信息。";
    } else {
        ui.result_body = outcome->result.user_message;
        ui.result_technical.clear();
        ui.evidence.clear();
    }
    ui.technical_open = false;
    ui.result_title = result_title(ui.result);
    update_controls();
}

/* ------------------------------------------------------------------ window */

void layout_window() {
    const Geometry layout = geometry();
    const int gap = scale(10);
    const int more_width = scale(132);
    const int secondary_width = scale(172);
    const int right = layout.content.right;
    int x = right - more_width;
    MoveWindow(ui.moreButton, x, layout.buttons.top, more_width, scale(42), TRUE);
    x -= gap + secondary_width;
    MoveWindow(ui.secondaryButton, x, layout.buttons.top, secondary_width, scale(42), TRUE);
    const int primary_left = layout.content.left;
    const int primary_width = std::max(scale(190), x - gap - primary_left);
    MoveWindow(ui.primaryButton, primary_left, layout.buttons.top, primary_width, scale(42),
               TRUE);
    MoveWindow(ui.addButton, layout.leftPanel.left + scale(14),
               layout.leftPanel.bottom - scale(54),
               layout.leftPanel.right - layout.leftPanel.left - scale(28), scale(40), TRUE);
}

bool handle_key(WPARAM key) {
    switch (key) {
        case VK_ESCAPE:
            if (ui.confirming_uninstall) {
                run_action(Action::cancel);
                return true;
            }
            if (ui.result != ResultKind::none && !ui.working) {
                run_action(Action::dismiss);
                return true;
            }
            return false;
        case VK_UP:
        case VK_DOWN: {
            if (ui.installs.empty() || ui.working) return false;
            if (key == VK_UP && ui.selected > 0) --ui.selected;
            if (key == VK_DOWN && ui.selected + 1 < ui.installs.size()) ++ui.selected;
            update_controls();
            return true;
        }
        case VK_RETURN: {
            /* Enter is the main action, as long as a button is not focused -
               there, Enter belongs to that button. */
            if (GetFocus() != ui.window) return false;
            if (ui.working) return true;
            ActionSet set = result_actions();
            run_action(set.primary);
            return true;
        }
        default: return false;
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            ui.window = window;
            ui.dpi = dpi_of(window);
            ui.bodyFace = family_exists(L"Segoe UI Variable Text")
                              ? L"Segoe UI Variable Text"
                              : L"Segoe UI";
            rebuild_fonts();
            rebuild_palette();
            const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
            ui.addButton = CreateWindowW(L"BUTTON", L"＋  添加其他位置", style, 0, 0, 0, 0,
                                         window, reinterpret_cast<HMENU>(kAddButton), nullptr,
                                         nullptr);
            ui.primaryButton = CreateWindowW(L"BUTTON", L"安装到此位置",
                                             style | BS_DEFPUSHBUTTON, 0, 0, 0, 0, window,
                                             reinterpret_cast<HMENU>(kPrimaryButton), nullptr,
                                             nullptr);
            ui.secondaryButton = CreateWindowW(L"BUTTON", L"打开游戏目录", style, 0, 0, 0, 0,
                                               window,
                                               reinterpret_cast<HMENU>(kSecondaryButton),
                                               nullptr, nullptr);
            ui.moreButton = CreateWindowW(L"BUTTON", L"更多操作  ▾", style, 0, 0, 0, 0, window,
                                          reinterpret_cast<HMENU>(kMoreButton), nullptr,
                                          nullptr);
            layout_window();
            rescan();
            return 0;
        }
        case WM_SIZE:
            layout_window();
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        case WM_GETMINMAXINFO: {
            /* The plan's floor: 820 x 560 DIP, including the frame.  Below this
               the two columns and the result banner stop fitting. */
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            RECT frame{0, 0, scale(820), scale(560)};
            AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
            info->ptMinTrackSize.x = frame.right - frame.left;
            info->ptMinTrackSize.y = frame.bottom - frame.top;
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint();
            return 0;
        case WM_DRAWITEM:
            if (lParam) draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_DPICHANGED: {
            ui.dpi = LOWORD(wParam);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            rebuild_fonts();
            rebuild_palette();
            layout_window();
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            rebuild_palette();
            return 0;
        case WM_LBUTTONDOWN: {
            if (ui.working) return 0;
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            for (size_t index = 0; index < ui.installs.size(); ++index) {
                RECT row = list_item_rect(index);
                if (!PtInRect(&row, point)) continue;
                if (ui.selected != index) {
                    ui.selected = index;
                    /* A banner belongs to the location it was produced for. */
                    ui.result = ResultKind::none;
                    ui.result_title.clear();
                    ui.result_body.clear();
                    ui.result_technical.clear();
                    ui.evidence.clear();
                    ui.technical_open = false;
                    ui.confirming_uninstall = false;
                    update_controls();
                }
                return 0;
            }
            break;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kAddButton: run_action(Action::choose_location); return 0;
                case kPrimaryButton: {
                    ActionSet set = result_actions();
                    if (ui.working) return 0;
                    run_action(set.primary);
                    return 0;
                }
                case kSecondaryButton: {
                    ActionSet set = result_actions();
                    if (ui.working) return 0;
                    run_action(set.secondary);
                    return 0;
                }
                case kMoreButton: show_more(); return 0;
                default: break;
            }
            break;
        case kScanDone:
            finish_scan(reinterpret_cast<std::vector<InstallationSnapshot>*>(lParam));
            return 0;
        case kWorkPhase: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
            ui.phase = *text;
            update_controls();
            return 0;
        }
        case kWorkDone:
            finish_work(reinterpret_cast<WorkOutcome*>(lParam));
            return 0;
        case WM_CLOSE:
            if (ui.working) {
                /* A write is in flight: closing now would leave a half-written
                   engine.  Say so instead of vanishing. */
                ui.close_note = L"正在完成安全写入，请稍候。";
                InvalidateRect(window, nullptr, TRUE);
                MessageBeep(MB_ICONINFORMATION);
                return 0;
            }
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            for (HFONT font : {ui.titleFont, ui.headingFont, ui.bodyFont, ui.smallFont})
                if (font) DeleteObject(font);
            ui.titleFont = ui.headingFont = ui.bodyFont = ui.smallFont = nullptr;
            ui.window = nullptr;
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int run_window(HINSTANCE instance, const std::wstring& initial) {
    if (auto set_dpi = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"),
                           "SetProcessDpiAwarenessContext")))
        set_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ui.initial = initial.empty() ? Candidate{} : Candidate{tc::fs::path(initial), L"命令行"};

    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.style = CS_HREDRAW | CS_VREDRAW;
    klass.lpfnWndProc = window_proc;
    klass.hInstance = instance;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = app_icon(GetSystemMetrics(SM_CXICON));
    klass.hIconSm = app_icon(GetSystemMetrics(SM_CXSMICON));
    klass.lpszClassName = L"TCModLoaderSetupWindow";
    RegisterClassExW(&klass);
    HWND window = CreateWindowExW(0, klass.lpszClassName,
                                  L"TC Mod Loader " TC_MODLOADER_VERSION_WTEXT,
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                  CW_USEDEFAULT, 1000, 620, nullptr, nullptr, instance,
                                  nullptr);
    if (!window) return 1;
    SendMessageW(window, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(app_icon(GetSystemMetrics(SM_CXICON))));
    SendMessageW(window, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(app_icon(GetSystemMetrics(SM_CXSMICON))));
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = std::min(scale(980), static_cast<int>((work.right - work.left) * 94 / 100));
    const int height = std::min(scale(660), static_cast<int>((work.bottom - work.top) * 94 / 100));
    SetWindowPos(window, nullptr, work.left + (work.right - work.left - width) / 2,
                 work.top + (work.bottom - work.top - height) / 2, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && handle_key(message.wParam)) continue;
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}

}  // namespace tc::setup
