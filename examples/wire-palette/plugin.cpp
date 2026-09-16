#include "../../sdk/tc_mod_api.h"
#include "palette.hpp"
#include <GL/gl.h>
#include <GL/glext.h>
#include <cmath>
struct V2{float x,y;};struct V4{float r,g,b,a;};
static const TCHost* host;
template<class T>T api(const char*n){return reinterpret_cast<T>(host->engine_proc(host->context,n));}
template<class T>T sym(const char*n){return reinterpret_cast<T>(host->resolve_symbol(host->context,n));}
static Palette palette;static std::filesystem::path file;static V4 table[256];static float gpu[256*3];static float edit[3]={0.85f,0.3f,0.65f};static std::string status;
static unsigned char* context;static int seen=-100,selected=0,hoverId=-1;static bool visible=true,picking=false,dirty=true,pending=false,editing=false;
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
static void choose(int id){selected=id;dirty=true;edit[0]=table[id].r;edit[1]=table[id].g;edit[2]=table[id].b;pending=true;if(context&&api<int(*)()>("igGetFrameCount")()-seen<=2){context[0x2a]=(uint8_t)id;pending=false;}}
static void commit(){try{RGB c;for(int i=0;i<3;++i)c[i]=(int)std::lround(std::clamp(edit[i],0.f,1.f)*255);auto next=palette;int id=next.add(c);next.save(file);palette=std::move(next);refresh();choose(id);status="颜色已使用，最近 5 色已自动保存。";}catch(const std::exception&e){status=e.what();}}
static void addWire(void* board,uint32_t point,uint8_t color){originalAddWire(board,point,(uint8_t)selected);}
static void placeWire(void* a,void* b,void* ctx,void* d,void* e,void* f){if(ctx)((unsigned char*)ctx)[0x2a]=(uint8_t)selected;originalPlaceWire(a,b,ctx,d,e,f);}
static bool update(void* model,void* ctx,void* input,uint32_t point,uint8_t fifth){context=(unsigned char*)ctx;seen=api<int(*)()>("igGetFrameCount")();
 hoverId=wireAt(model,point)!=invalidWire?(int)pipette(model,point):-1;
#ifdef TC_WIRE_SELFTEST
 testModel=model;
#endif
 if(pending){context[0x2a]=(uint8_t)selected;pending=false;}
 bool click=api<bool(*)(int,bool)>("igIsMouseClicked_Bool")(0,false);
#ifdef TC_WIRE_SELFTEST
 click=click||testClick;testClick=false;
#endif
 if(picking){if(click){
  if(wireAt(model,point)!=invalidWire){int id=pipette(model,point);if(id==255)id=0;choose(id);status="已从导线读回原始颜色。";picking=false;}else status="这里没有导线，请点击导线中心。";
 }return true;}
 bool result=originalUpdate(model,ctx,input,point,fifth);int id=context[0x2a];if(id==255)id=0;if(id!=selected){if(selected>=11&&id<=10){context[0x2a]=(uint8_t)selected;status="已保持调色盘自定义颜色。";}else{choose(id);status="已同步游戏选色／取色。";}}return result;
}
static void text(const char*s){api<void(*)(const char*,const char*)>("igTextUnformatted")(s,nullptr);}
static bool button(const char*s){return api<bool(*)(const char*,V2)>("igButton")(s,{0,0});}
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
 DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);static bool last=false;bool down=pid==GetCurrentProcessId()&&(GetAsyncKeyState(VK_F7)&0x8000);if(down&&!last)visible=!visible;last=down;if(pid==GetCurrentProcessId()&&(GetAsyncKeyState(VK_ESCAPE)&0x8000))picking=false;
 if(!visible)return;api<void(*)(V2,int)>("igSetNextWindowSize")({510,800},2);api<void(*)(V2,int)>("igSetNextWindowPos")({660,110},2);
 if(api<bool(*)(const char*,bool*,int)>("igBegin")("导线调色盘###TCWirePalette",&visible,0)){
 api<void(*)(float)>("igSetWindowFontScale")(0.6f);text("F7 显示 / 隐藏 · RGB 精确取色");
 api<void(*)(float)>("igSetNextItemWidth")(360);
 if(api<bool(*)(const char*,float*,int)>("igColorPicker3")("##picker",edit,0))editing=true;
 if(editing&&(api<bool(*)()>("igIsItemDeactivatedAfterEdit")()||(!api<bool(*)()>("igIsAnyItemActive")()&&!api<bool(*)(int)>("igIsMouseDown_Nil")(0)))){commit();editing=false;}
 if(button("使用当前颜色"))commit();api<void(*)(float,float)>("igSameLine")(0,-1);
 if(button(picking?"取消取色":"取色器"))picking=!picking;
 text("最近 5 种自定义颜色（点击复用）");
 for(int id:palette.recent){auto label="##recent"+std::to_string(id);if(api<bool(*)(const char*,V4,int,V2)>("igColorButton")(label.c_str(),table[id],0,{48,32}))choose(id);api<void(*)(float,float)>("igSameLine")(0,-1);}api<void(*)()>("igNewLine")();
 api<void(*)(float)>("igPushTextWrapPos")(0);
 if(picking)text("取色中：点击已放置的导线；Esc 取消。该次点击不会放置或改色。");
 text("调好颜色后松开鼠标即自动保存。新导线使用当前颜色；已有导线可用游戏原有改色快捷键。");
 if(f->frame_number-seen>5)text("请进入关卡并将鼠标移到电路区，以连接绘图工具。");
 text(hoverId>=0?("光标下导线颜色 ID："+std::to_string(hoverId)).c_str():"光标下导线颜色 ID：无");
 if(programs.empty())text("等待导线着色器；请确认 Mod 已应用并重启游戏。");
 if(!status.empty())text(status.c_str());api<void(*)()>("igPopTextWrapPos")();
 }api<void(*)()>("igEnd")();
}
static void writeMemory(void*p,const void*v,size_t n){DWORD old;if(!VirtualProtect(p,n,PAGE_READWRITE,&old))throw std::runtime_error("Memory patch failed");memcpy(p,v,n);DWORD ignored;VirtualProtect(p,n,old,&ignored);FlushInstructionCache(GetCurrentProcess(),p,n);}
static V4** patchedRef;static V4* oldTable;static unsigned char* patchedLimit;
static void cleanup(void*){try{if(patchedRef)writeMemory(patchedRef,&oldTable,sizeof(oldTable));if(patchedLimit){unsigned char n=10;writeMemory(patchedLimit,&n,1);}}catch(...) {}}
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost*h,TCPlugin*out){try{
 if(!h||h->api_version!=1||h->size<sizeof(TCHost)||!out||out->size<sizeof(TCPlugin))return 1;host=h;
 auto stock=sym<V4*>("WIRE_COLORS__presenterZcontext_u3002");auto ref=sym<V4**>(".refptr.WIRE_COLORS__presenterZcontext_u3002");
 pipette=sym<decltype(pipette)>("pipette_wire__modelZutilities_u2289");wireAt=sym<decltype(wireAt)>("get_wire__presenterZutilitiesZhelper95functions_u1916");auto invalid=sym<int64_t*>("INVALID_WIRE_ID__modelZsave95mongerZcommon_u3578");auto target=sym<void*>("handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
 auto addTarget=sym<void*>("add_wire_from_pos__modelZboardZboard_u28435");
 auto placeTarget=sym<void*>("handle_place_wire__presenterZuser95inputZboard95ioZactionZplace95wire_u2");
 auto limit=(unsigned char*)GetModuleHandleW(nullptr)+0x467c98;
 if(!stock||!ref||*ref!=stock||!pipette||!wireAt||!invalid||!target||!addTarget||!placeTarget||limit[0]!=0x3c||limit[1]!=10)return 2;invalidWire=*invalid;
 for(auto n:{"igGetFrameCount","igIsMouseClicked_Bool","igBegin","igEnd","igSetNextWindowSize","igSetNextWindowPos","igSetWindowFontScale","igTextUnformatted","igButton","igSameLine","igNewLine","igColorPicker3","igColorButton","igSetNextItemWidth","igIsItemDeactivatedAfterEdit","igIsAnyItemActive","igIsMouseDown_Nil","igPushTextWrapPos","igPopTextWrapPos"})if(!h->engine_proc(h->context,n))return 3;
 memcpy(table,stock,11*sizeof(V4));file=std::filesystem::u8path(h->data_directory_utf8)/"palette.json";palette.load(file);refresh();
 if(h->create_hook(h->context,target,(void*)update,(void**)&originalUpdate))return 4;
 if(h->create_hook(h->context,addTarget,(void*)addWire,(void**)&originalAddWire))return 7;
 if(h->create_hook(h->context,placeTarget,(void*)placeWire,(void**)&originalPlaceWire))return 8;
#ifdef TC_WIRE_SELFTEST
 if(h->create_hook(h->context,h->resolve_symbol(h->context,"igInvisibleButton"),(void*)testInvisible,(void**)&testInvisibleOriginal))return 6;
#endif
 out->on_unload=cleanup;patchedRef=ref;oldTable=stock;patchedLimit=limit+1;
 unsigned char max=254;V4* p=table;writeMemory(ref,&p,sizeof(p));writeMemory(limit+1,&max,1);
 out->on_frame=frame;h->log(h->context,"Wire Palette loaded: RGB palette, wire pipette and persistent recent 5");return 0;
 }catch(const std::exception&e){if(host)host->log(host->context,e.what());return 5;}}
