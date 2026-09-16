#pragma once
#include "core.hpp"
#include "../sdk/tc_mod_api.h"
#include "MinHook.h"
#include <memory>
#include <functional>
#include "component_timing.hpp"
namespace tc {
struct Symbols {
 std::map<std::string,void*> values;std::set<void*> functions;
 explicit Symbols(const fs::path& exe){auto b=read(exe);auto dos=(const IMAGE_DOS_HEADER*)b.data();auto nt=(const IMAGE_NT_HEADERS64*)(b.data()+dos->e_lfanew);auto fh=nt->FileHeader;auto sections=(const IMAGE_SECTION_HEADER*)((const char*)&nt->OptionalHeader+fh.SizeOfOptionalHeader);
  size_t start=fh.PointerToSymbolTable,end=start+(uint64_t)fh.NumberOfSymbols*sizeof(IMAGE_SYMBOL);if(!start||end+4>b.size())throw std::runtime_error("Game COFF symbol table is missing");uint32_t strings{};memcpy(&strings,b.data()+end,4);if(strings<4||end+strings>b.size())throw std::runtime_error("Invalid COFF strings");
  for(uint32_t i=0;i<fh.NumberOfSymbols;){IMAGE_SYMBOL s;memcpy(&s,b.data()+start+(uint64_t)i*sizeof(s),sizeof(s));i+=1+s.NumberOfAuxSymbols;if(s.SectionNumber<=0||s.SectionNumber>fh.NumberOfSections)continue;std::string name;
   if(s.N.Name.Short){char n[9]{};memcpy(n,s.N.ShortName,8);name=n;}else {auto off=s.N.Name.Long;if(off<4||off>=strings)continue;auto p=b.data()+end+off;auto z=(const char*)memchr(p,0,strings-off);if(!z)continue;name.assign(p,static_cast<size_t>(z-p));}
   auto& sec=sections[s.SectionNumber-1];if(s.Value>=sec.Misc.VirtualSize)continue;auto ptr=(char*)GetModuleHandleW(nullptr)+sec.VirtualAddress+s.Value;values.emplace(name,ptr);if((sec.Characteristics&IMAGE_SCN_MEM_EXECUTE)&&(s.Type&0x20))functions.insert(ptr);
  }
 }
};
class NativeRuntime {
 struct Loaded {NativeRuntime* owner;std::string id,folder,status;TCHost host{};TCPlugin plugin{};HMODULE dll{};std::vector<void*> hooks;bool accepting=true,active=false;};
 Core& core;HMODULE engine;std::unique_ptr<Symbols> symbols;std::vector<std::unique_ptr<Loaded>> loaded;std::set<void*> ownedHooks;std::function<void(const std::string&)> logger;int lastFrame=-1;bool inside=false,started=false;
 static void log_api(void* c,const char* msg){auto& p=*(Loaded*)c;p.owner->logger("["+p.id+"] "+(msg?msg:""));}
 static void* resolve(void* c,const char* name){auto& p=*(Loaded*)c;auto& m=p.owner->symbols->values;auto i=m.find(name?name:"");return i==m.end()?nullptr:i->second;}
 static void* engine_api(void* c,const char* name){auto& p=*(Loaded*)c;return name?(void*)GetProcAddress(p.owner->engine,name):nullptr;}
 static int hook_api(void* c,void* target,void* detour,void** original){auto& p=*(Loaded*)c;auto& r=*p.owner;if(!p.accepting||!target||!detour||!original||!r.symbols->functions.count(target)||r.ownedHooks.count(target)){log_api(c,"Hook rejected: phase, target or conflict");return -1;}auto status=MH_CreateHook(target,detour,original);if(status!=MH_OK){log_api(c,MH_StatusToString(status));return (int)status+1;}p.hooks.push_back(target);r.ownedHooks.insert(target);return 0;}
 void reject(Loaded& p){for(auto target:p.hooks){MH_DisableHook(target);MH_RemoveHook(target);ownedHooks.erase(target);}p.hooks.clear();if(p.plugin.on_unload){try{p.plugin.on_unload(p.plugin.user);}catch(...) {}}p.plugin={};p.accepting=false;p.active=false;/* Keep rejected DLL mapped: it may have static destructors/threads. */}
public:
 std::map<std::string,std::string> statuses;
 NativeRuntime(Core& c,HMODULE e,std::function<void(const std::string&)> log):core(c),engine(e),logger(std::move(log)){}
 void boot(){if(started)return;started=true;try{
  if(GetAsyncKeyState(VK_SHIFT)&0x8000){logger("Native safe mode: Shift held; plugins skipped");for(auto& id:core.enabled_set())statuses[id]="安全模式：本次未加载";return;}
  symbols=std::make_unique<Symbols>(core.root/L"Turing Complete.exe");auto mh=MH_Initialize();if(mh!=MH_OK&&mh!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("MinHook initialization failed");
  timing::install(symbols->values,ownedHooks,logger);
  std::map<std::string,Mod*> mods;for(auto& m:core.mods)if(m.error.empty())mods[m.id]=&m;
  std::set<std::string> visiting,done,failed;std::function<void(const std::string&)> load=[&](const std::string& id){if(done.count(id))return;if(!visiting.insert(id).second){failed.insert(id);statuses[id]="依赖循环";return;}if(!mods.count(id)){failed.insert(id);statuses[id]="Mod 缺失或无效";done.insert(id);return;}auto& m=*mods.at(id);
   for(auto& dep:m.dependencies){if(!core.enabled(dep)){failed.insert(dep);}else load(dep);if(failed.count(dep))failed.insert(id);}
   if(failed.count(id)){statuses[id]="依赖未成功加载";done.insert(id);return;}
   if(!m.entry.empty()) {auto p=std::make_unique<Loaded>();p->owner=this;p->id=id;
    try {if(!core.state.contains("native")||core.state["native"].value(id,"")!=m.digest)throw std::runtime_error("包内容已改变，请在 Mods 页面重新应用后重启");
     auto cache=core.dir/L"plugins"/fs::u8path(id)/m.digest;no_links(core.root,cache);fs::create_directories(cache);for(auto& [rel,bytes]:m.native){auto out=cache/fs::u8path(rel);no_links(core.root,out);auto tmp=out;tmp+=L".tc-tmp";no_links(core.root,tmp);if(!fs::exists(out)||hash(read(out))!=hash(bytes))atomic(out,bytes);}
     auto data=core.dir/L"plugin-data"/fs::u8path(id);no_links(core.root,data);fs::create_directories(data);p->folder=data.u8string();p->host={sizeof(TCHost),TC_MOD_API_VERSION,p.get(),"2.1.334 / native-api-1",p->id.c_str(),p->folder.c_str(),log_api,resolve,engine_api,hook_api};p->plugin.size=sizeof(TCPlugin);
     p->dll=LoadLibraryExW((cache/fs::u8path(m.entry)).c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);if(!p->dll)throw std::runtime_error("LoadLibrary failed: "+std::to_string(GetLastError()));auto entry=(TCModLoad)GetProcAddress(p->dll,"tc_mod_load");if(!entry)throw std::runtime_error("Missing tc_mod_load export");timing::Registration registration;if(entry(&p->host,&p->plugin)!=0||p->plugin.size!=sizeof(TCPlugin))throw std::runtime_error("Plugin rejected API / initialization failed");
     p->accepting=false;for(auto h:p->hooks)if(MH_EnableHook(h)!=MH_OK)throw std::runtime_error("Cannot enable hook");timing::commit(registration.ids);p->active=true;statuses[id]="运行中（原生代码）";logger("Native loaded: "+id+"; hooks="+std::to_string(p->hooks.size()));
    }catch(const std::exception&e){statuses[id]=e.what();failed.insert(id);logger("Native failed: "+id+": "+e.what());reject(*p);}loaded.push_back(std::move(p));
   }
   visiting.erase(id);done.insert(id);
  };for(auto& id:core.enabled_set())load(id);
 }catch(const std::exception&e){logger(std::string("Native runtime unavailable: ")+e.what());}}
 void frame(){if(inside)return;inside=true;boot();auto getFrame=(int(*)())GetProcAddress(engine,"igGetFrameCount");auto getTime=(double(*)())GetProcAddress(engine,"igGetTime");int n=getFrame();if(n!=lastFrame){lastFrame=n;TCFrame f{sizeof(TCFrame),n,getTime()};for(auto& p:loaded)if(p->active&&p->plugin.on_frame){try{p->plugin.on_frame(p->plugin.user,&f);}catch(...){logger("Plugin callback threw: "+p->id);statuses[p->id]="回调异常：请停用后重启";p->plugin.on_frame=nullptr;}}}inside=false;}
};
}
