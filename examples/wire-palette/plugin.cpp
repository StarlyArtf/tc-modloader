#include "../../sdk/tc_ui.h"
#include "../../sdk/tc_game_ui.h"
#include "../../sdk/tc_ui_draw.h"
#include "../../sdk/tc_ui_tool.h"
#include "palette.hpp"
#include <GL/gl.h>
#include <GL/glext.h>
#include <cmath>
struct V4{float r,g,b,a;};
static const TCHost* host;
template<class T>T sym(const char*n){return reinterpret_cast<T>(host->resolve_symbol(host->context,n));}
/* Engine entry points the popup needs but the SDK does not wrap. */
template<class T>T engineFn(const char*n){return reinterpret_cast<T>(host->engine_proc(host->context,n));}
static void closePopup(){auto close=engineFn<void(*)()>("igCloseCurrentPopup");if(close)close();}
static bool windowHovered(){auto hovered=engineFn<bool(*)(int)>("igIsWindowHovered");return hovered?hovered(0):false;}
// Palette tables keep the game's r,g,b,a naming; the UI layer uses ImVec4 order.
static tc::ui::Vec4 uiColor(const V4& c){return {c.r,c.g,c.b,c.a};}
static Palette palette;static std::filesystem::path file;static V4 table[256];static float gpu[256*3];static float edit[3]={0.85f,0.3f,0.65f};static std::string status;
static unsigned char* context;static int seen=-100,selected=0,hoverId=-1;static bool visible=false,picking=false,dirty=true,pending=false,editing=false;
using Update=bool(*)(void*,void*,void*,uint32_t,uint8_t);static Update originalUpdate;
using AddWire=void(*)(void*,uint32_t,uint8_t);static AddWire originalAddWire;
using PlaceWire=void(*)(void*,void*,void*,void*,void*,void*);static PlaceWire originalPlaceWire;


static uint8_t(*pipette)(void*,uint32_t);static int64_t(*wireAt)(void*,uint32_t);static int64_t invalidWire;
static PFNGLISPROGRAMPROC isProgram;
static PFNGLGETUNIFORMLOCATIONPROC location;
static PFNGLUSEPROGRAMPROC useProgram;
static PFNGLUNIFORM3FVPROC uniform;
// Bind-and-upload is required on the supported Intel driver: direct-state
// glProgramUniform3fv reads back correctly but the draw can still use zeros.
static void uploadUniform(GLuint program,GLint loc,GLsizei count,const float* values){
 GLint previous=0;glGetIntegerv(GL_CURRENT_PROGRAM,&previous);
 useProgram(program);uniform(loc,count,values);useProgram((GLuint)previous);
}
static std::vector<std::pair<GLuint,GLint>> programs;
static void rememberProgram(GLuint p,GLint l){for(auto&e:programs)if(e.first==p){e.second=l;dirty=true;return;}programs.emplace_back(p,l);dirty=true;host->log(host->context,("Custom wire shader program found: id="+std::to_string(p)+" location="+std::to_string(l)).c_str());}
#ifdef TC_WIRE_SELFTEST
static void* testModel;
static bool testClick=false;
#endif
static void refresh(){for(int i=0;i<256;++i){if(i>=11)table[i]={1,0,1,1};if(i>=11&&i<11+(int)palette.colors.size()){auto c=palette.colors[i-11];table[i]={c[0]/255.f,c[1]/255.f,c[2]/255.f,1};}gpu[i*3]=table[i].r;gpu[i*3+1]=table[i].g;gpu[i*3+2]=table[i].b;}dirty=true;}
static void choose(int id){selected=id;dirty=true;edit[0]=table[id].r;edit[1]=table[id].g;edit[2]=table[id].b;pending=true;if(context&&tc::ui::frameCount()-seen<=2){context[0x2a]=(uint8_t)id;pending=false;}}
static void commit(){try{RGB c;for(int i=0;i<3;++i)c[i]=(int)std::lround(std::clamp(edit[i],0.f,1.f)*255);auto next=palette;int id=next.add(c);next.save(file);palette=std::move(next);refresh();choose(id);status="颜色已使用，最近 5 色已自动保存。";}catch(const std::exception&e){status=e.what();}}
static void addWire(void* board,uint32_t point,uint8_t color){originalAddWire(board,point,(uint8_t)selected);}
static void placeWire(void* a,void* b,void* ctx,void* d,void* e,void* f){if(ctx)((unsigned char*)ctx)[0x2a]=(uint8_t)selected;originalPlaceWire(a,b,ctx,d,e,f);}
static bool update(void* model,void* ctx,void* input,uint32_t point,uint8_t fifth){context=(unsigned char*)ctx;seen=tc::ui::frameCount();
 hoverId=wireAt(model,point)!=invalidWire?(int)pipette(model,point):-1;
#ifdef TC_WIRE_SELFTEST
 testModel=model;
#endif
 if(pending){context[0x2a]=(uint8_t)selected;pending=false;}
 bool click=tc::ui::isMouseClicked();
#ifdef TC_WIRE_SELFTEST
 click=click||testClick;testClick=false;
#endif
 if(picking){if(click){
  if(wireAt(model,point)!=invalidWire){int id=pipette(model,point);if(id==255)id=0;choose(id);status="已从导线读回原始颜色。";picking=false;}else status="这里没有导线，请点击导线中心。";
 }return true;}
 bool result=originalUpdate(model,ctx,input,point,fifth);int id=context[0x2a];if(id==255)id=0;if(id!=selected){if(selected>=11&&id<=10){context[0x2a]=(uint8_t)selected;status="已保持调色盘自定义颜色。";}else{choose(id);status="已同步游戏选色／取色。";}}return result;
}
/* The palette is a tool in the game's own tool column: the loader calls this
   from the end of that column's child window, so the chip sits right after the
   game's own tools (rotate, delete, bit width ...).  Hovering it expands the
   palette in a popup, the way the game's own tools expand - nothing floats on
   screen until the mouse is on the tool. */
static void drawTool(void*,const TCFrame* f,float,float){
 const tc::ui::Vec4 current=uiColor(table[selected]);
 /* The tile and its hover popup come from the SDK (sdk/tc_ui_tool.h): the
    game's own tool metrics (80x80, radius 10, colours 53/50/68), geometric
    hover that ignores what is drawn on top, and a popup that opens on hover,
    sits beside the tile, survives the gap between them and closes only once
    the mouse has been outside both for a moment. */
 tc::ui::ToolTile tile("##wirecolortile");
 tile.colourIcon(current);
 if(tile.clicked())choose(selected);
 if(picking)tc::ui::textDisabled("取色中");
 /* The expanded palette is a *window* (tc::ui::panel), not an ImGui popup.  A
    popup inherits the tool column's tiny window font scale - which is why its
    text never appeared - and rounding it needed a style-var push that crashed
    the game.  A plain window draws text exactly like the board side panels do
    and takes the game's own window style, rounding included.  It opens on hover
    and closes once the mouse has been outside both it and the tile for a
    moment, so crossing the gap between them does not close it. */
 static bool paletteOpen=false;
 static double lastInside=0.0;
 const double now=f?f->time_seconds:0.0;
 const tc::ui::Vec2 origin=tile.origin();
 if(tile.hovered()){paletteOpen=true;lastInside=now;}
 bool insidePanel=false;
 if(paletteOpen){
  if(auto panel=tc::ui::panel("导线调色盘###TCWirePalette",nullptr,{430.f,620.f},
                              {origin.x+tile.size().x+8.f,origin.y-8.f},1.f,0,tc::ui::Cond_Always)){
   insidePanel=tc::ui::isWindowHovered(0);
   if(insidePanel)lastInside=now;
 /* Actions first: they are what a player reaches for while drawing wires. */
 if(tc::ui::button(picking?"取消取色":"取色器",{112.f,0.f}))picking=!picking;
 tc::ui::sameLine();
 if(tc::ui::button("使用此颜色",{168.f,0.f}))choose(selected);
 tc::ui::sameLine();
 tc::ui::textDisabled(("当前 ID "+std::to_string(selected)).c_str());
 tc::ui::separator();
 /* A compact preview strip: game colours, then the player's own. */
 tc::ui::text("当前");
 tc::ui::sameLine();
 tc::ui::colorButton("##current",current,0,{64.f,24.f});
 tc::ui::sameLine();
 tc::ui::textDisabled(picking?"取色中：点击已放置的导线，Esc 取消":(hoverId>=0?("光标下导线颜色 ID："+std::to_string(hoverId)).c_str():"把鼠标移到电路区即可取色"));
 tc::ui::separator();
 /* Mixing a new colour.  The visual picker is the point of this panel - a square
   you drag through hue and saturation, with the hue and alpha bars beside it and
   ImGui's own R/G/B, H/S/V and hex inputs under it - so it comes before the
   colour lists and is capped in size to leave them room.  Releasing the mouse
   saves, like the old floating panel did. */
 tc::ui::text("新建颜色（拖动取色，松手即保存）");
 /* Inside the popup the panel's own width is what matters, not the tool
    column's: fix it so the picker always has room. */
 const float pickerWidth=260.f;
 tc::ui::setNextItemWidth(pickerWidth);
 if(tc::ui::colorPicker3("##picker",edit,0))editing=true;
 if(editing&&(tc::ui::isItemDeactivatedAfterEdit()||(!tc::ui::isAnyItemActive()&&!tc::ui::isMouseDown(0)))){commit();editing=false;}
 const tc::ui::Vec4 preview{edit[0],edit[1],edit[2],1.f};
 tc::ui::colorButton("##preview",preview,0,{64.f,24.f});
 tc::ui::sameLine();
 if(tc::ui::button("保存并选中",{168.f,0.f}))commit();
 tc::ui::separator();
 tc::ui::text("游戏自带色");
 for(int id=0;id<11;++id){
  const std::string label="##game"+std::to_string(id);
  if(tc::ui::colorButton(label.c_str(),uiColor(table[id]),0,{34.f,22.f}))choose(id);
  if(id%8!=7)tc::ui::sameLine();
 }
 tc::ui::newLine();
 tc::ui::separator();
 tc::ui::text("我的颜色（点击复用）");
 const int saved=(int)palette.colors.size();
 if(saved==0)tc::ui::textDisabled("还没有自定义颜色：调一个 RGB，然后按「保存并选中」。");
 for(int index=0;index<saved;++index){
  const int id=11+index;
  const std::string label="##saved"+std::to_string(id);
  if(tc::ui::colorButton(label.c_str(),uiColor(table[id]),0,{34.f,22.f}))choose(id);
  if(index%8!=7)tc::ui::sameLine();
 }
 if(saved>0)tc::ui::newLine();
 if(!palette.recent.empty()){
  tc::ui::textDisabled("最近用过");
  for(size_t index=0;index<palette.recent.size();++index){
   const int id=palette.recent[index];
   const std::string label="##recent"+std::to_string(id);
   if(tc::ui::colorButton(label.c_str(),uiColor(table[id]),0,{34.f,22.f}))choose(id);
   if(index%8!=7)tc::ui::sameLine();
  }
  tc::ui::newLine();
 }
 tc::ui::separator();
 if(!status.empty())tc::ui::textDisabled(status.c_str());
 if(programs.empty())tc::ui::textDisabled("等待导线着色器；请确认 Mod 已应用并重启游戏。");
 if(f&&f->frame_number-seen>5&&!context)tc::ui::textDisabled("请把鼠标移到电路区一次，以连接绘图工具。");
  }   /* end of the palette window */
 if(!tile.hovered()&&!insidePanel&&!tc::ui::isAnyItemActive()&&!tc::ui::isMouseDown(0)&&
     lastInside>0.0&&now-lastInside>0.25){
  paletteOpen=false;
  /* One line per transition, so a log-only check can see the expanded palette
     closing without needing to watch the screen. */
  static double lastClosedLog=0.0;
  if(now-lastClosedLog>5.0){lastClosedLog=now;host->log(host->context,"Wire Palette: the expanded palette closed after the mouse left");}
 }
 }
}
#ifdef TC_WIRE_SELFTEST
#include "../../tests/wire-palette-playtest.hpp"
#endif
static void frame(void*,const TCFrame*f){
#ifdef TC_WIRE_SELFTEST
 testTick(f);
#ifdef TC_WIRE_RENDER_SELFTEST
 testRenderTick();
#endif
#endif

 if(!isProgram){isProgram=(PFNGLISPROGRAMPROC)wglGetProcAddress("glIsProgram");location=(PFNGLGETUNIFORMLOCATIONPROC)wglGetProcAddress("glGetUniformLocation");useProgram=(PFNGLUSEPROGRAMPROC)wglGetProcAddress("glUseProgram");uniform=(PFNGLUNIFORM3FVPROC)wglGetProcAddress("glUniform3fv");}
 static GLuint scanId=1;static int nextUpload=0;
 if(isProgram&&location&&useProgram&&uniform){int budget=programs.empty()?8192:1024;bool wrapped=false;while(budget-->0){if(scanId>65535u){scanId=1;wrapped=true;}GLuint p=scanId++;if(isProgram(p)){int l=location(p,"tc_custom_wire_colors");if(l>=0)rememberProgram(p,l);}}if(wrapped){programs.erase(std::remove_if(programs.begin(),programs.end(),[](const auto&e){return !isProgram(e.first);}),programs.end());}if(dirty||f->frame_number>=nextUpload){for(auto [p,l]:programs)if(isProgram(p))uploadUniform(p,l,256,gpu);dirty=false;nextUpload=f->frame_number+60;}}
 DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);if(pid==GetCurrentProcessId()&&(GetAsyncKeyState(VK_ESCAPE)&0x8000))picking=false;
}
static void writeMemory(void*p,const void*v,size_t n){DWORD old;if(!VirtualProtect(p,n,PAGE_READWRITE,&old))throw std::runtime_error("Memory patch failed");memcpy(p,v,n);DWORD ignored;VirtualProtect(p,n,old,&ignored);FlushInstructionCache(GetCurrentProcess(),p,n);}
static V4** patchedRef;static V4* oldTable;static unsigned char* patchedLimit;
static void cleanup(void*){try{if(patchedRef)writeMemory(patchedRef,&oldTable,sizeof(oldTable));if(patchedLimit){unsigned char n=10;writeMemory(patchedLimit,&n,1);}}catch(...) {}}
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost*h,TCPlugin*out){try{
 if(!h||h->api_version!=1||h->size<TC_HOST_BASE_SIZE||!out||out->size<sizeof(TCPlugin))return 1;host=h;
 auto stock=sym<V4*>("WIRE_COLORS__presenterZcontext_u3002");auto ref=sym<V4**>(".refptr.WIRE_COLORS__presenterZcontext_u3002");
 pipette=sym<decltype(pipette)>("pipette_wire__modelZutilities_u2289");wireAt=sym<decltype(wireAt)>("get_wire__presenterZutilitiesZhelper95functions_u1916");auto invalid=sym<int64_t*>("INVALID_WIRE_ID__modelZsave95mongerZcommon_u3578");auto target=sym<void*>("handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
 auto addTarget=sym<void*>("add_wire_from_pos__modelZboardZboard_u28435");
 auto placeTarget=sym<void*>("handle_place_wire__presenterZuser95inputZboard95ioZactionZplace95wire_u2");
 auto limit=(unsigned char*)GetModuleHandleW(nullptr)+0x467c98;
 if(!stock||!ref||*ref!=stock||!pipette||!wireAt||!invalid||!target||!addTarget||!placeTarget||limit[0]!=0x3c||limit[1]!=10)return 2;invalidWire=*invalid;
 if(!tc::ui::load(h))return 3;
 if(!tc::ui::loadDrawing(h))h->log(h->context,"Wire Palette: drawing capability unavailable; the tool falls back to a plain colour button");
 memcpy(table,stock,11*sizeof(V4));file=std::filesystem::u8path(h->data_directory_utf8)/"palette.json";palette.load(file);refresh();
 if(h->create_hook(h->context,target,(void*)update,(void**)&originalUpdate))return 4;
 if(h->create_hook(h->context,addTarget,(void*)addWire,(void**)&originalAddWire))return 7;
 if(h->create_hook(h->context,placeTarget,(void*)placeWire,(void**)&originalPlaceWire))return 8;
#ifdef TC_WIRE_SELFTEST
 if(h->create_hook(h->context,h->resolve_symbol(h->context,"igInvisibleButton"),(void*)testInvisible,(void**)&testInvisibleOriginal))return 6;
#endif
 out->on_unload=cleanup;patchedRef=ref;oldTable=stock;patchedLimit=limit+1;
 unsigned char max=254;V4* p=table;writeMemory(ref,&p,sizeof(p));writeMemory(limit+1,&max,1);
 if(!tc::game_ui::load(h))h->log(h->context,"Wire Palette: game widget style unavailable; panel uses plain buttons");
 const int slot=tc::ui::registerBoardToolbar("palette",drawTool,nullptr,h);
 if(slot!=0)h->log(h->context,("Wire Palette: board tool registration failed ("+std::to_string(slot)+")").c_str());
 out->on_frame=frame;h->log(h->context,"Wire Palette loaded: RGB palette, wire pipette and persistent recent 5");return 0;
 }catch(const std::exception&e){if(host)host->log(host->context,e.what());return 5;}}
