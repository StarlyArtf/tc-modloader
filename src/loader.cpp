#include "core.hpp"
#include "compat.hpp"
#include "native.hpp"
#include "saves.hpp"
#include <memory>
#include <shellapi.h>
struct V2 {float x,y;};
static HMODULE engine;
template<class T> T api(const char* name){auto p=GetProcAddress(engine,name);if(!p)throw std::runtime_error(std::string("Missing engine API: ")+name);T result;static_assert(sizeof(result)==sizeof(p));memcpy(&result,&p,sizeof(p));return result;}
static std::unique_ptr<tc::Core> core;
static std::unique_ptr<tc::NativeRuntime> nativeRuntime;
static bool attempted=false, compatible=false, homeSeen=false, firstDraw=true;
static bool managerOpen=false;
static int debugFrames=0;
static std::string error;
static std::set<std::string> selected;
static tc::fs::path gameRoot;
static std::unique_ptr<tc::SaveProfiles> saves;
static void log(const std::string& msg){try{std::ofstream f(gameRoot/L"tc-modloader-data"/L"loader.log",std::ios::app);f<<msg<<"\n";}catch(...) {}}
static void init(){if(attempted)return;attempted=true;wchar_t buf[32768];GetModuleFileNameW(nullptr,buf,32768);gameRoot=tc::fs::path(buf).parent_path();
 try{engine=GetModuleHandleW(L"tc_game_engine.dll");if(!engine)throw std::runtime_error("Original engine is missing");
 compatible=tc::hash(tc::read(buf))==TC_EXE_SHA && tc::hash(tc::read(gameRoot/L"tc_game_engine.dll"))==TC_ENGINE_SHA;
 if(!compatible)throw std::runtime_error("Unsupported game build. Reinstall a compatible loader.");
 saves=std::make_unique<tc::SaveProfiles>(gameRoot);
 core=std::make_unique<tc::Core>(gameRoot);core->scan();selected=core->enabled_set();nativeRuntime=std::make_unique<tc::NativeRuntime>(*core,engine,[](const std::string& s){log(s);});log(std::string("TC Mod Loader 0.3.0; ImGui ")+api<const char*(*)()>("igGetVersion")());log("Isolated save directory: "+saves->path(tc_save_boot::profile).u8string());
 }catch(const std::exception& e){error=e.what();log(error);}}
static void text(const std::string& s){api<void(*)(const char*,const char*)>("igTextUnformatted")(s.c_str(),nullptr);}
static bool button(const char* label,V2 size={0,0}){return api<bool(*)(const char*,V2)>("igButton")(label,size);}
static void line(){api<void(*)()>("igSeparator")();}
static void same(){api<void(*)(float,float)>("igSameLine")(0,-1);}
static void draw(){
 auto setpos=api<void(*)(V2)>("igSetCursorPos");
 float w=api<float(*)()>("igGetWindowWidth")(), h=api<float(*)()>("igGetWindowHeight")();
 float scale=std::max(0.65f,std::min(w/1600.f,h/900.f));
 api<void(*)(float)>("igSetWindowFontScale")(scale);
 setpos({w-180*scale,24*scale});
 if(button("Mods",{150*scale,46*scale})) {if(core){core->scan();selected=core->enabled_set();}managerOpen=true;debugFrames=5;api<void(*)(const char*,int)>("igOpenPopup_Str")("Mod 管理###TCMods",0);log("Manager opened");}
 api<void(*)(float)>("igSetWindowFontScale")(1.f);
 api<void(*)(V2,int)>("igSetNextWindowSize")({std::min(w-30.f,1000*scale),std::min(h-40.f,740*scale)},1);
 api<void(*)(V2,int,V2)>("igSetNextWindowPos")({w/2,h/2},1,{0.5f,0.5f});
 api<void(*)(float)>("igSetNextWindowBgAlpha")(1.f);
 bool open=true;
 bool visible=api<bool(*)(const char*,bool*,int)>("igBeginPopupModal")("Mod 管理###TCMods",&open,2|32);
 if(debugFrames>0){--debugFrames;log(std::string("Popup frame: home=")+(homeSeen?"1":"0")+" visible="+(visible?"1":"0"));}
 if(!open)managerOpen=false;
 if(visible) {
  api<void(*)(float)>("igSetWindowFontScale")(0.64f*scale);
  text("TC MOD LOADER  /  0.3.0  /  NATIVE API 1");
  text("将 .mod 文件放入游戏目录的 mods 文件夹，重新打开此页即可识别。");
  text("支持原生代码 Mod：函数 Hook、游戏接口调用、每帧逻辑与自定义面板。");
  text("勾选后点击“应用更改”，重启游戏后完整生效。同一文件的冲突会被拦截。");
  line();
  if(button("刷新列表")){try{if(core){core->scan();selected=core->enabled_set();}error.clear();}catch(const std::exception&e){error=e.what();}}
  same();if(button("打开 mods 文件夹"))ShellExecuteW(nullptr,L"open",(gameRoot/L"mods").c_str(),nullptr,nullptr,SW_SHOWNORMAL);
  same();if(button("全部停用"))selected.clear();
  if(saves){
   line();text("存档已隔离（停用全部 Mod 后仍使用独立存档）");
   api<void(*)(float)>("igPushTextWrapPos")(0);text("当前："+saves->path(tc_save_boot::profile).u8string());api<void(*)()>("igPopTextWrapPos")();
   if(button("导入原版存档（新副本）")){try{auto id=saves->import_original();core->notice="导入并校验成功。重启游戏后使用新副本；当前 Mod 存档和原版存档均保留。";error.clear();log("Imported original saves into "+saves->path(id).u8string());}catch(const std::exception&e){error=e.what();}}
   same();if(button("打开当前存档"))ShellExecuteW(nullptr,L"open",saves->path(tc_save_boot::profile).c_str(),nullptr,nullptr,SW_SHOWNORMAL);
   text("导入前请关闭原版游戏。仅复制本机存档，不合并、不覆盖。导入后重启生效。");
   try{auto next=saves->next();if(next!=tc_save_boot::profile)text("待重启切换："+tc::fs::path(next).u8string());}catch(const std::exception&e){error=e.what();}
  }
  line();
  if(core){text("找到 "+std::to_string(core->mods.size())+" 个 Mod  |  已勾选 "+std::to_string(selected.size())+" 个");
   api<bool(*)(const char*,V2,int,int)>("igBeginChild_Str")("TCModList",{0,-150*scale},0,0);
   if(core->mods.empty())text("还没有 Mod。将 .mod 文件放入 mods 文件夹后刷新。");
   for(auto& m:core->mods){api<void(*)(const char*)>("igPushID_Str")(m.source.u8string().c_str());bool on=selected.count(m.id)>0;
    api<void(*)(bool)>("igBeginDisabled")(!m.error.empty());
    if(api<bool(*)(const char*,bool*)>("igCheckbox")("##enabled",&on)){if(on)selected.insert(m.id);else selected.erase(m.id);}
    api<void(*)()>("igEndDisabled")();same();text(m.name+"  "+m.version);text(m.id+(m.author.empty()?"":"  /  "+m.author));
    api<void(*)(float)>("igPushTextWrapPos")(0);text(m.error.empty()?m.description:"ERROR: "+m.error);api<void(*)()>("igPopTextWrapPos")();
    text(std::string(m.entry.empty()?"资源 Mod":"原生代码 Mod")+"  |  "+std::to_string(m.files.size())+" 个资源文件"+(core->enabled(m.id)?"  |  下次启动启用":"  |  下次启动停用"));
    if(!m.entry.empty()){auto it=nativeRuntime->statuses.find(m.id);text("本次运行："+(it==nativeRuntime->statuses.end()?std::string("未加载"):it->second));}line();api<void(*)()>("igPopID")();}
   for(auto& id:core->enabled_set()){bool found=false;for(auto& m:core->mods)if(m.id==id)found=true;if(!found){bool on=selected.count(id);if(api<bool(*)(const char*,bool*)>("igCheckbox")(("Missing package: "+id).c_str(),&on)){if(on)selected.insert(id);else selected.erase(id);}text("Uncheck and Apply to restore its original files.");}}
   api<void(*)()>("igEndChild")();
  }
  line();
  if(button("应用更改")){try {if(!core)throw std::runtime_error("Loader backend unavailable");core->apply(selected);selected=core->enabled_set();core->notice="更改已保存。请关闭并重新启动游戏，让所有修改完整生效。";error.clear();log("Applied selected Mods");}catch(const std::exception&e){error=e.what();log(error);}}
  same();if(button("关闭")){managerOpen=false;api<void(*)()>("igCloseCurrentPopup")();}
  api<void(*)(float)>("igPushTextWrapPos")(0);if(!error.empty())text("操作未完成："+error);else if(core)text(core->notice);api<void(*)()>("igPopTextWrapPos")();
  api<void(*)()>("igEndPopup")();
 }
 if(firstDraw){firstDraw=false;log("Main menu Mods button rendered");}
}
extern "C" __declspec(dllexport) bool igInvisibleButton(const char* id,V2 size,int flags){
 auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);init();
 if(compatible&&rva>=TC_HOME_START_RVA&&rva<TC_HOME_END_RVA)homeSeen=true;
 return api<bool(*)(const char*,V2,int)>("igInvisibleButton")(id,size,flags);
}
extern "C" __declspec(dllexport) void igEnd(){
 auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);init();
 if(compatible&&rva==TC_MENU_END_RVA){if(homeSeen||managerOpen){try{draw();}catch(const std::exception&e){log(e.what());}}homeSeen=false;}
 api<void(*)()>("igEnd")();
 if(compatible&&nativeRuntime)nativeRuntime->frame();
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(h);return tc_save_boot::attach()?TRUE:FALSE;}return TRUE;}
