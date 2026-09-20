#include "../../sdk/tc_ui.h"
#include "../../sdk/tc_hook.h"
#include <windows.h>
#include <stdint.h>
#include <limits>
#include <string>
#include <cstring>
#include <atomic>
static const TCHost* host;
#ifdef TC_GUARD_SELFTEST
#include <GL/gl.h>
#include <GL/glext.h>
#include <vector>
#include <fstream>
#include <filesystem>
static void capture(){GLint v[4];glGetIntegerv(GL_VIEWPORT,v);int w=v[2],h=v[3];if(w<1||h<1)return;int stride=(w*3+3)&~3;std::vector<unsigned char> pixels(stride*h);GLint buffer,packing;glGetIntegerv(GL_READ_BUFFER,&buffer);glGetIntegerv(GL_PACK_ALIGNMENT,&packing);glReadBuffer(GL_FRONT);glPixelStorei(GL_PACK_ALIGNMENT,4);glReadPixels(0,0,w,h,GL_BGR,GL_UNSIGNED_BYTE,pixels.data());glReadBuffer(buffer);glPixelStorei(GL_PACK_ALIGNMENT,packing);BITMAPFILEHEADER f{};f.bfType=0x4d42;f.bfOffBits=54;f.bfSize=54+pixels.size();BITMAPINFOHEADER i{};i.biSize=40;i.biWidth=w;i.biHeight=h;i.biPlanes=1;i.biBitCount=24;std::ofstream out(std::filesystem::u8path(host->data_directory_utf8)/"selftest-frame.bmp",std::ios::binary);out.write((char*)&f,sizeof(f));out.write((char*)&i,sizeof(i));out.write((char*)pixels.data(),pixels.size());}
#endif
using V2 = tc::ui::Vec2;
using SimDo=void(*)(void*,uint8_t,int64_t);
/* The game's own sim_do, as resolved from the symbol profile: calling it goes
   through the hook chain like any other caller (used by the toolbar buttons to
   request a run). */
static SimDo gameSim;
static int64_t (*getCycle)();
static void** settings;
static std::atomic<int64_t> budget{100},hits{0},lastRequested{0},lastEffective{0};
static std::atomic<bool> guard{true};
static void* model;
static bool show=false;
#ifdef TC_GUARD_SELFTEST
static bool (*originalButton)(const char*,V2,int);
static uint64_t testStartTime;
static bool startedLevel=false;
static bool testButton(const char* id,V2 size,int flags){bool result=originalButton(id,size,flags);auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);if(!startedLevel&&GetTickCount64()-testStartTime>3000&&rva>=0x449df0&&rva<0x44b610){startedLevel=true;host->log(host->context,"SELFTEST activated native Start Game menu action");return true;}return result;}
#endif
template<class T> static T symbol(const char*n){void* p=host->resolve_symbol(host->context,n);T f{};memcpy(&f,&p,sizeof(f));return f;}
/* A link in the loader's sim.do chain: several mods may clamp or watch run
   requests at the same time, in priority order, and the game's own function
   still runs afterwards because this returns 0. */
static int intercepted(TCHookCall* call){
 auto* args=tc::hook::simDoArgs(call);if(!args)return 0;
 model=args->model;
 if(args->command==0&&guard.load()&&settings&&*settings){auto now=getCycle();auto n=budget.load();auto limit=now>INT64_MAX-n?INT64_MAX:now+n;
  if(args->target<0||args->target>limit){lastRequested=args->target;lastEffective=limit;++hits;args->target=limit;std::string msg="Intercepted run: target="+std::to_string(lastRequested.load())+" -> "+std::to_string(limit)+", current="+std::to_string(now);host->log(host->context,msg.c_str());}}
 return 0;
}
static void drawToolbar(int64_t now,bool ready){
 auto toolbar=tc::ui::panel("周期运行守卫 · 工具条###TCCycleGuardToolbar",nullptr,{0,0},{12,12},
                            0.62f,tc::ui::kToolbarFlags,tc::ui::Cond_Always);
 if(toolbar){
  bool on=guard.load();if(tc::ui::checkbox("拦截",&on))guard=on;
  tc::ui::sameLine();if(tc::ui::button(show?"隐藏面板":"守卫面板"))show=!show;
  tc::ui::sameLine();{tc::ui::Disabled disabled(!ready||!model);
   if(tc::ui::button("运行 N")){auto n=budget.load();gameSim(model,0,now>INT64_MAX-n?INT64_MAX:now+n);}
   tc::ui::sameLine();if(tc::ui::button("连续"))gameSim(model,0,INT64_MAX);}
  tc::ui::text("周期 "+std::to_string(now)+"  |  已拦截 "+std::to_string(hits.load())+" 次");
 }
}
static void frame(void*,const TCFrame* frameInfo){
#ifdef TC_GUARD_SELFTEST
 static int phase=0;static double start=0,stable=0;static int64_t expected=0,delay=0;
 if(phase==0){start=frameInfo->time_seconds;phase=1;}
 if(phase==1&&frameInfo->time_seconds-start>5&&settings&&*settings&&model){auto getter=tc::resolveAliasAs<int64_t(*)(uint8_t)>(host,"sim.setting.get");auto setter=tc::resolveAliasAs<void(*)(uint8_t,int64_t)>(host,"sim.setting.set");delay=getter(2);setter(2,-1);expected=getCycle()+100;gameSim(model,0,INT64_MAX);phase=2;host->log(host->context,("SELFTEST requested continuous run; expected stop="+std::to_string(expected)).c_str());}
 if(phase==2){auto cycle=getCycle();if(cycle==expected){if(stable==0)stable=frameInfo->time_seconds;if(frameInfo->time_seconds-stable>1){host->log(host->context,("SELFTEST PASS actual simulator stopped and remained at cycle "+std::to_string(cycle)).c_str());phase=3;}}else if(cycle>expected||frameInfo->time_seconds-start>25){host->log(host->context,("SELFTEST FAIL actual="+std::to_string(cycle)+" expected="+std::to_string(expected)).c_str());phase=3;}if(phase==3){tc::resolveAliasAs<void(*)(uint8_t,int64_t)>(host,"sim.setting.set")(2,delay);capture();}}
#else
 (void)frameInfo;
#endif
 tc::ui::toggleHotkey(VK_F6,&show);
 bool ready=settings&&*settings;auto now=ready?getCycle():0;
 if(ready&&model)drawToolbar(now,ready);
 if(!show)return;
 if(auto panel=tc::ui::panel("周期运行守卫 · Native Mod###TCCycleGuard",&show,{620,430},{24,110},
                            0.62f,0,tc::ui::Cond_Once)){
  tc::ui::text("工具条上的“守卫面板”按钮或 F6 打开 / 隐藏此面板。");
  bool on=guard.load();if(tc::ui::checkbox("拦截游戏运行命令",&on))guard=on;
  tc::ui::text("每次最多运行："+std::to_string(budget.load())+" 个周期");
  for(auto n:{1,10,100,1000}){if(tc::ui::button(std::to_string(n).c_str()))budget=n;if(n!=1000)tc::ui::sameLine();}
  tc::ui::text("当前周期："+std::to_string(now)+"  |  已拦截："+std::to_string(hits.load())+" 次");
  {tc::ui::Disabled disabled(!ready||!model);
   if(tc::ui::button("运行 N 个周期")){auto n=budget.load();gameSim(model,0,now>INT64_MAX-n?INT64_MAX:now+n);}
   tc::ui::sameLine();if(tc::ui::button("请求连续运行（验证拦截）"))gameSim(model,0,INT64_MAX);}
  if(hits.load())tc::ui::text("最近目标："+std::to_string(lastRequested.load())+" → "+std::to_string(lastEffective.load()));
  {tc::ui::TextWrap wrap(0);
   tc::ui::text("进入关卡后使用。守卫限制运行命令的目标周期，不修改电路或存档；停用守卫后恢复原运行行为。");}
 }
}
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* out){
 if(!h||h->api_version!=1||h->size<TC_HOST_BASE_SIZE||!out||out->size<sizeof(TCPlugin))return 1;host=h;
 gameSim=tc::resolveAliasAs<SimDo>(h,"sim.do");getCycle=tc::resolveAliasAs<int64_t(*)()>(h,"sim.cycle");settings=tc::resolveAliasAs<void**>(h,"sim.settings");if(!gameSim||!getCycle||!settings)return 2;
 if(!tc::ui::load(h))return 3;
 const int chain=tc::hook::addSimDo(h,0,&intercepted,nullptr);
 if(chain!=TC_HOOK_OK){h->log(h->context,(std::string("Cycle Guard: cannot join the sim.do chain: ")+tc::hook::errorText(chain)).c_str());return 4;}
#ifdef TC_GUARD_SELFTEST
 testStartTime=GetTickCount64();auto buttonTarget=h->resolve_symbol(h->context,"igInvisibleButton");if(h->create_hook(h->context,buttonTarget,(void*)testButton,(void**)&originalButton))return 5;
#endif
 out->on_frame=frame;h->log(h->context,"Cycle Guard initialized; joined the sim.do hook chain");return 0;
}
