#include "../sdk/tc_ui_draw.h"
#include <cassert>
#include <limits>
#include <iostream>
using namespace tc::ui;
static int pushes, pops, items, lines;
static Vec2 lastA, lastB;
static std::vector<Vec2> vertices;
static void* list() { return reinterpret_cast<void*>(1); }
static void origin(Vec2* p) { *p={100,200}; }
static void push(void* p, Vec2 a, Vec2 b, bool intersect) {
    assert(p==list() && intersect); ++pushes; lastA=a; lastB=b;
}
static void pop(void*) { ++pops; }
static bool item(const char*,Vec2,int) { ++items; return true; }
static bool yes() { return true; }
static bool hovered(int) { return true; }
static void line(void*,Vec2 a,Vec2 b,Color,float) { ++lines; lastA=a; lastB=b; }
static void poly(void*,const Vec2* p,int n,Color,int flags,float) {
    assert(flags==1); vertices.assign(p,p+n);
}
static void dummy() {}
static void* resolve(void*, const char* name) {
    if (std::strcmp(name,"ImDrawList_AddCircle")==0) return nullptr;
    auto fn = &dummy; void* p; std::memcpy(&p,&fn,sizeof(p)); return p;
}
int main() {
    static_assert(rgba(1,2,3,4)==0x04030201);
    { Canvas canvas("unloaded",{100,100}); assert(!canvas); canvas.line({0,0},{1,1},0); }
    assert(!loadDrawing(nullptr) && !drawingReady());
    TCHost host{}; host.context=&host; host.engine_proc=resolve;
    assert(!loadDrawing(&host) && !drawingReady());
    assert(drawingMissing()=="ImDrawList_AddCircle");
    auto& api=drawing_detail::table();
    api.windowList=list; api.cursorScreen=origin; api.pushClip=push; api.popClip=pop;
    api.itemActive=yes; api.line=line; api.polyline=poly;
    table().invisibleButton=item; table().isItemHovered=hovered; table().getMousePos=origin;
    { Canvas invalid("bad",{-1,20}); assert(!invalid); }
    { Canvas invalid(nullptr,{20,20}); assert(!invalid); }
    assert(items==0 && pushes==0);
    try {
        Canvas canvas("canvas",{300,200});
        assert(canvas && canvas.hovered() && canvas.active() && canvas.clicked());
        assert(lastA.x==100 && lastA.y==200 && lastB.x==400 && lastB.y==400);
        assert(canvas.mousePosition().x==0 && canvas.mousePosition().y==0);
        canvas.line({5,6},{7,8},rgba(1,2,3),2);
        assert(lines==1 && lastA.x==105 && lastA.y==206 && lastB.x==107 && lastB.y==208);
        canvas.line({0,0},{1,1},0,-1);
        canvas.line({std::numeric_limits<float>::quiet_NaN(),0},{1,1},0);
        assert(lines==1);
        const Vec2 input[]{{0,0},{10,0},{10,10}};
        canvas.polyline(input,3,0,true);
        assert(vertices.size()==3 && vertices[2].x==110 && vertices[2].y==210);
        vertices.clear();
        canvas.polyline(nullptr,3,0,true);
        canvas.polyline(input,20000,0,true);
        assert(vertices.empty());
        { Canvas::Clip clip(canvas,{10,20},{30,40}); assert(pushes==2 && lastA.x==110 && lastB.y==240); }
        assert(pops==1);
        throw 1;
    } catch(int) {}
    assert(pushes==2 && pops==2 && items==1);
    assert(!loadDrawing(&host) && !drawingReady()); // failed reload clears stale bindings
    std::cout << "PASS drawing capability failure, coordinates, input snapshot, validation and clip cleanup\n";
}
