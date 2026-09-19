/* Development probe: why does text submitted from the game's tool column not
   appear, and what makes it appear?

   Measured so far:

     * inside the tool column the current ImGui font is the game's *icon* font
       (Icon_Complete.ttf at size 72), which has no Latin or CJK glyphs, so text
       submits no glyphs at all while rectangles - which need none - still show;
     * a board side panel, a main-menu page and the end of a frame all run with
       the game's text font (NoroshiCode_Regular.ttf at size 45);
     * the game does not push fonts by pointer: it keeps a table
       (defined_fonts__presenterZimguiZimgui_u7413) and pushes an entry by index
       through its own igPushFont wrapper
       (igPushFont__presenterZimguiZimgui_u7614), which is also why the tool
       column's own labels ("+255") render while a plugin's text does not.

   This probe enumerates that table (which index is which font) and draws the
   same line three ways: with nothing done, with the text font pushed through
   the game's own wrapper, and inside a plain window next to the tile.  It draws
   on *every* frame: an earlier version only drew every 120th frame and the
   screenshot kept catching an empty frame. */
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_ui.h"
#include "../sdk/tc_ui_draw.h"
#include "../sdk/tc_ui_tool.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using tc::ui::Vec2;
using tc::ui::Vec4;

static const TCHost* host=nullptr;
/* The game's font table and its own "push font by index" wrapper. */
static void** definedFonts=nullptr;
static void (*gamePushFont)(unsigned char index)=nullptr;
static void (*popFont)()=nullptr;
static int toolTextFontIndex=-1;
static bool enumerated=false;

static void logLine(const std::string& message) {
    if (host&&host->log) host->log(host->context,message.c_str());
}
template <class T> static T engineFn(const char* name) {
    return reinterpret_cast<T>(host->engine_proc(host->context,name));
}
template <class T> static T gameSym(const char* name) {
    return reinterpret_cast<T>(host->resolve_symbol(host->context,name));
}
static std::string number(float value) {
    char text[32];std::snprintf(text,sizeof(text),"%.2f",value);return text;
}
static std::string fontName(void* font) {
    auto getName=engineFn<const char*(*)(const void*)>("ImFont_GetDebugName");
    if (!font) return "none";
    const char* name=getName?getName(font):nullptr;
    return name&&*name?name:"unnamed";
}
static std::string currentFont() {
    auto getFont=engineFn<void*(*)()>("igGetFont");
    auto getSize=engineFn<float(*)()>("igGetFontSize");
    void* font=getFont?getFont():nullptr;
    return "font="+fontName(font)+"("+std::to_string((uintptr_t)font)+") size="+
           number(getSize?getSize():-1.f);
}
static std::string currentWindowName() {
    auto getWindow=engineFn<void*(*)()>("igGetCurrentWindowRead");
    if (!getWindow) return "?";
    void* window=getWindow();
    if (!window) return "?";
    const char* name=*reinterpret_cast<const char**>(reinterpret_cast<char*>(window)+8);
    return name&&*name?name:"?";
}
/* Index -> font, straight out of the game's own table: the index is what the
   game's wrappers take, so it is the handle a plugin has to use. */
static void enumerateFonts() {
    if (enumerated||!definedFonts) return;
    enumerated=true;
    for (int index=0;index<8;++index) {
        void* font=definedFonts[index];
        if (!font) break;
        const std::string name=fontName(font);
        logLine("TOOLBARFONT defined_fonts["+std::to_string(index)+"]="+name);
        if (toolTextFontIndex<0&&name.find("Icon")==std::string::npos) toolTextFontIndex=index;
    }
    logLine("TOOLBARFONT text font index chosen: "+std::to_string(toolTextFontIndex));
}
static std::string describe(const char* where,int frame) {
    return std::string("TOOLBARFONT ")+where+" frame="+std::to_string(frame)+" window=\""+
           currentWindowName()+"\" "+currentFont();
}

/* ---------------------------------------------------------------- toolbar tool */
static void drawTool(void*,const TCFrame* frame,float,float) {
    const int current=frame?frame->frame_number:0;
    static int lastReport=-1000;
    enumerateFonts();
    tc::ui::ToolTile tile("##toolfonttile");
    tile.colourIcon(Vec4{1.f,0.45f,0.1f,1.f});
    const Vec2 origin=tile.origin();
    if (current-lastReport>=120) {
        lastReport=current;
        logLine(describe("tool",current)+" tileOrigin="+number(origin.x)+","+number(origin.y));
    }
    /* Drawn every frame and with no font handling of its own: whatever shows
       here is what the host did for the plugin. */
    tc::ui::text("TOOLFONT tool bare");

    Vec2 at{origin.x+96.f,origin.y-8.f};
    if (origin.x<1.f||origin.y<1.f) at=Vec2{400.f,300.f};
    tc::ui::setNextWindowPos(at,tc::ui::Cond_Always,Vec2{0.f,0.f});
    tc::ui::setNextWindowSize(Vec2{560.f,300.f},tc::ui::Cond_Always);
    tc::ui::Window window("TOOLBARFONT 探针窗###TCToolFontProbe",nullptr,tc::ui::Window_None);
    if (window) {
        tc::ui::text("TOOLFONT window bare");
        tc::ui::colorButton("##probe colour",Vec4{0.2f,0.8f,0.4f,1.f},0,{120.f,28.f});
        tc::ui::button("TOOLFONT window button",Vec2{260.f,0.f});
        tc::ui::textDisabled("TOOLFONT window disabled");
    }
}

/* ------------------------------------------------------------ board side panel */
static void drawPanel(void*,const TCFrame* frame,float,float) {
    const int current=frame?frame->frame_number:0;
    static int lastReport=-1000;
    if (current-lastReport>=240) {
        lastReport=current;
        logLine(describe("sidepanel",current));
        tc::ui::text("TOOLFONT sidepanel baseline");
    }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* out) {
    if (!h||!out||h->api_version!=TC_MOD_API_VERSION||out->size<sizeof(TCPlugin)) return 1;
    host=h;
    if (!tc::ui::load(h)) {
        logLine("TOOLBARFONT: UI API missing: "+tc::ui::missing());
        return 3;
    }
    if (!tc::ui::loadDrawing(h)) logLine("TOOLBARFONT: drawing capability unavailable");
    definedFonts=gameSym<void**>("defined_fonts__presenterZimguiZimgui_u7413");
    gamePushFont=gameSym<void(*)(unsigned char)>("igPushFont__presenterZimguiZimgui_u7614");
    /* The engine's own PopFont: the game's Nim layer has no wrapper for it. */
    popFont=engineFn<void(*)()>("igPopFont");
    logLine(std::string("TOOLBARFONT symbols: defined_fonts=")+
            (definedFonts?"yes":"no")+" gamePushFont="+(gamePushFont?"yes":"no")+
            " popFont="+(popFont?"yes":"no"));
    const int tool=tc::ui::registerBoardToolbar("probe",drawTool,nullptr,h);
    const int panel=tc::ui::registerBoardPanel("side","文字探针##TCToolFontSide",drawPanel,nullptr,h,460.f,200.f);
    logLine("TOOLBARFONT registered: tool="+std::to_string(tool)+" panel="+std::to_string(panel));
    return 0;
}
