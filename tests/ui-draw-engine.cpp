/* Real pinned-engine integration test. Private draw-buffer layout is used only
   here for readback (never in the SDK). Three ImVector headers: cmd/idx/vtx;
   disassembly confirms shared data at +56 and clip stack at +160. */
#include "../sdk/tc_ui_draw.h"
#include <algorithm>
#include <stdexcept>
using namespace tc::ui;
static const TCHost* host;
static void (*clipMin)(Vec2*,void*);
static void (*clipMax)(Vec2*,void*);
static void (*setScroll)(float);
static float (*getScroll)();
static int frames;
static bool failed;
template<class T> static T read(void* p, std::size_t offset) {
    T value; std::memcpy(&value,static_cast<char*>(p)+offset,sizeof(value)); return value;
}
static void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
static bool almostEqual(float a,float b) { return std::fabs(a-b)<0.05f; }
static bool equal(Vec2 a,Vec2 b) { return almostEqual(a.x,b.x)&&almostEqual(a.y,b.y); }
static void log(const std::string& s) { host->log(host->context,s.c_str()); }
template<class F> static void emits(void* list,const char* name,F fn) {
    const int before=read<int>(list,32); fn();
    require(read<int>(list,32)>before,name);
}
static void draw() {
    auto& api=drawing_detail::table();
    void* list=api.windowList();
    Vec2 beforeMin{},beforeMax{}; clipMin(&beforeMin,list); clipMax(&beforeMax,list);
    const Vec2 available=contentAvailable();
    require(available.x>100 && available.x<2000,"contentAvailable ABI");
    {
        Canvas canvas("probe",{500,360}); require(bool(canvas),"canvas unavailable");
        const Vec2 origin=canvas.origin();
        require(drawing_detail::finite(origin)&&std::fabs(origin.x)<2000&&std::fabs(origin.y)<2000,"origin ABI");
        const Vec2 expectedMin{std::max(beforeMin.x,origin.x),std::max(beforeMin.y,origin.y)};
        const Vec2 expectedMax{std::min(beforeMax.x,origin.x+500),std::min(beforeMax.y,origin.y+360)};
        Vec2 actualMin{},actualMax{}; clipMin(&actualMin,list); clipMax(&actualMax,list);
        require(equal(expectedMin,actualMin)&&equal(expectedMax,actualMax),"canvas/window clip intersection");
        const Color color=rgba(73,167,223);
        const int first=read<int>(list,32);
        emits(list,"rectFilled",[&]{canvas.rectFilled({10,10},{70,50},color);});
        void* buffer=read<void*>(list,40);
        const Vec2 vertex=read<Vec2>(buffer,first*20);
        const Color vertexColor=read<Color>(buffer,first*20+16);
        require(equal(vertex,canvas.toScreen({10,10}))&&vertexColor==color,"vertex coordinate/color ABI");
        emits(list,"line",[&]{canvas.line({10,60},{90,60},color,3);});
        emits(list,"rect",[&]{canvas.rect({100,10},{170,60},color,8,3);});
        emits(list,"rounded fill",[&]{canvas.rectFilled({180,10},{240,60},color,10);});
        emits(list,"gradient",[&]{canvas.gradient({250,10},{320,60},color,rgba(255,0,0),color,rgba(0,255,0));});
        emits(list,"circle",[&]{canvas.circle({40,110},20,color,2);});
        emits(list,"circleFilled",[&]{canvas.circleFilled({100,110},20,color,24);});
        emits(list,"triangle",[&]{canvas.triangle({140,85},{170,130},{125,130},color,2);});
        emits(list,"triangleFilled",[&]{canvas.triangleFilled({200,85},{230,130},{185,130},color);});
        emits(list,"bezier",[&]{canvas.bezier({10,170},{100,130},{150,210},{240,170},color,3);});
        const Vec2 polygon[]{{280,100},{340,100},{350,130},{270,130}};
        emits(list,"polyline",[&]{canvas.polyline(polygon,4,color,true,2);});
        emits(list,"convex",[&]{canvas.convexFilled(polygon,4,color);});
        const Vec2 concave[]{{370,85},{450,85},{410,110},{450,140},{370,140}};
        emits(list,"concave",[&]{canvas.concaveFilled(concave,5,color);});
        // Keep text inside the visible part even when the child is scrolled.
        emits(list,"text",[&]{canvas.text({10,240},color,"UTF-8 / 100% text");});
        {
            Canvas::Clip clip(canvas,{20,200},{100,280});
            clipMin(&actualMin,list); clipMax(&actualMax,list);
            const Vec2 min=canvas.toScreen({20,200}),max=canvas.toScreen({100,280});
            require(equal(actualMin,{std::max(min.x,expectedMin.x),std::max(min.y,expectedMin.y)})&&
                    equal(actualMax,{std::min(max.x,expectedMax.x),std::min(max.y,expectedMax.y)}),"nested clip");
            canvas.circleFilled({20,240},60,color);
        }
        clipMin(&actualMin,list); clipMax(&actualMax,list);
        require(equal(actualMin,expectedMin)&&equal(actualMax,expectedMax),"nested clip restore");
    }
    Vec2 afterMin{},afterMax{}; clipMin(&afterMin,list); clipMax(&afterMax,list);
    require(equal(beforeMin,afterMin)&&equal(beforeMax,afterMax),"canvas clip restore");
    text("Regular UI after canvas"); button("Still usable");
}
static void frame(void*,const TCFrame*) {
    if (failed || frames>=180) return;
    try {
        const bool moved=frames>=60;
        auto window=panel("Drawing engine test",nullptr,moved?Vec2{640,540}:Vec2{600,510},
                          moved?Vec2{160,100}:Vec2{80,60},0.62f,Window_None,Cond_Always);
        if (!window) return;
        if (auto child=Child("scroll test",{0,390})) {
            if (frames==90) setScroll(70);
            draw();
            text("Scroll tail");
            invisibleButton("tail",{100,200});
            if (frames==110) require(getScroll()>30,"child did not scroll");
        }
        ++frames;
        if (frames==1 || frames==60 || frames==120 || frames==180)
            log("DRAW PASS frames="+std::to_string(frames)+" all primitives, vertices, clip restoration");
    } catch(const std::exception& e) { failed=true; log(std::string("DRAW FAIL: ")+e.what()); }
}
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* out) {
    host=h;
    if(!load(h)||!loadDrawing(h)) return 1;
    clipMin=reinterpret_cast<decltype(clipMin)>(h->engine_proc(h->context,"ImDrawList_GetClipRectMin"));
    clipMax=reinterpret_cast<decltype(clipMax)>(h->engine_proc(h->context,"ImDrawList_GetClipRectMax"));
    setScroll=reinterpret_cast<decltype(setScroll)>(h->engine_proc(h->context,"igSetScrollY_Float"));
    getScroll=reinterpret_cast<decltype(getScroll)>(h->engine_proc(h->context,"igGetScrollY"));
    if (!clipMin||!clipMax||!setScroll||!getScroll) return 2;
    out->on_frame=frame; return 0;
}
