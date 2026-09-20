#pragma once
#include "core.hpp"
#include "compat.hpp"
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_hook.h"
#include "MinHook.h"
#include <memory>
#include <functional>
#include "component_timing.hpp"
#include "native_logic.hpp"
#include "native_component.hpp"
#include "ui_texture.hpp"
#include "symbol_profile.hpp"
#include "fault_guard.hpp"
#include "game_handles.hpp"
namespace tc {
struct Symbols {
 std::map<std::string,void*> values;std::set<void*> functions;
 explicit Symbols(const fs::path& exe,HMODULE module=GetModuleHandleW(nullptr)){auto b=read(exe);auto dos=(const IMAGE_DOS_HEADER*)b.data();auto nt=(const IMAGE_NT_HEADERS64*)(b.data()+dos->e_lfanew);auto fh=nt->FileHeader;auto sections=(const IMAGE_SECTION_HEADER*)((const char*)&nt->OptionalHeader+fh.SizeOfOptionalHeader);
  size_t start=fh.PointerToSymbolTable,end=start+(uint64_t)fh.NumberOfSymbols*sizeof(IMAGE_SYMBOL);if(!start||end+4>b.size())throw std::runtime_error("Game COFF symbol table is missing");uint32_t strings{};memcpy(&strings,b.data()+end,4);if(strings<4||end+strings>b.size())throw std::runtime_error("Invalid COFF strings");
  for(uint32_t i=0;i<fh.NumberOfSymbols;){IMAGE_SYMBOL s;memcpy(&s,b.data()+start+(uint64_t)i*sizeof(s),sizeof(s));i+=1+s.NumberOfAuxSymbols;if(s.SectionNumber<=0||s.SectionNumber>fh.NumberOfSections)continue;std::string name;
   if(s.N.Name.Short){char n[9]{};memcpy(n,s.N.ShortName,8);name=n;}else {auto off=s.N.Name.Long;if(off<4||off>=strings)continue;auto p=b.data()+end+off;auto z=(const char*)memchr(p,0,strings-off);if(!z)continue;name.assign(p,static_cast<size_t>(z-p));}
   auto& sec=sections[s.SectionNumber-1];if(s.Value>=sec.Misc.VirtualSize)continue;auto ptr=(char*)module+sec.VirtualAddress+s.Value;values.emplace(name,ptr);if((sec.Characteristics&IMAGE_SCN_MEM_EXECUTE)&&(s.Type&0x20))functions.insert(ptr);
  }
 }
};
class NativeRuntime {
 /* One native UI page registered by a plugin.  Owned by the loader: id/title
    are copies, and draw/user are dropped (draw set to null) as soon as the
    plugin reports a failure, so a broken page keeps its frame and back button
    without running plugin code again. */
 struct UiPage {std::string id,title;void (*draw)(void*,const TCFrame*,float,float)=nullptr;void* user=nullptr;bool failed=false;};
 /* A panel registered for one of the game's own screens (today: the circuit
    board's side area).  Unlike a page, a slot has no "open" state to track:
    the loader draws it from that screen's own per-frame UI code, so it comes
    and goes with the screen. */
 /* `collapsed` is host state (the player folded the panel), not plugin state:
    the loader owns the toggle, so a plugin cannot leave the panel folded with
    no way back. */
 struct BoardSlot {std::string id,title;uint32_t kind=TC_UI_SLOT_BOARD_SIDE;void (*draw)(void*,const TCFrame*,float,float)=nullptr;void* user=nullptr;bool failed=false,collapsed=false;float width=0.f,height=0.f;int drawn=0,skipped=0;};
 /* `reported` is set when a plugin called report_status during tc_mod_load: the
    loader then keeps that message instead of overwriting it with "running" once
    the plugin finishes loading, which is what the author asked to see. */
 struct Loaded {NativeRuntime* owner;std::string id,folder,status;fs::path packageRoot;TCHost host{};TCPlugin plugin{};HMODULE dll{};std::vector<void*> hooks;std::vector<uint64_t> logicIds,componentIds;std::vector<UiPage> pages;std::vector<BoardSlot> slots;bool accepting=true,active=false,reported=false;};
 ui_texture::Manager textures;
 Core& core;HMODULE engine;std::unique_ptr<Symbols> symbols;std::vector<std::unique_ptr<Loaded>> loaded;std::set<void*> ownedHooks;std::map<void*,std::string> loaderOwned;std::function<void(const std::string&)> logger;int lastFrame=-1,lastUiFrame=-1,lastSlotFrame=-1,lastToolFrame=-1;int64_t lastEngineFrame=-1;bool inside=false,started=false;
 /* Per content region (keyed by "<mod>/<id>"): last logged scroll offset and
    last logged scroll-strip state, so the logs are change-driven and bounded. */
 std::map<std::string,float> lastScrollY;std::map<std::string,int> lastStripState;
 /* Last message a plugin reported through report_status, so the log only grows
    when the text changes: a plugin may report every frame. */
 std::map<std::string,std::string> lastReportedStatus;
 /* The game's own font table entry and wrappers the tool column borrows from
    (see resolveToolTextFont).  -1 means "not known"; nothing is pushed then. */
 int toolTextFontIndex=-1;
 void (*pushGameFont)(unsigned char)=nullptr;
 void (*popGameFont)()=nullptr;
 /* Address ranges of the board scene's UI entry points, built once during
    boot.  The loader's igInvisibleButton proxy uses them to tell "the player
    is on a circuit board" from "the main menu is up": pages belong to the
    menu, so entering a board must close them. */
 std::vector<std::pair<uintptr_t,uintptr_t>> boardRanges;
 /* Screen rectangle of the last tool button the game drew in its own tool
    column, learned from the child window that each tool is drawn in.  Plugin
    tools are placed directly below it, so they line up with the game's own. */
 float toolColumnX=0.f,toolColumnBottom=0.f,toolColumnPitchX=96.f,toolColumnPitchY=96.f;
 bool toolColumnKnown=false;
 /* Page the user is currently inside, or "" for the main menu.  Survives
    scene changes so leaving a level returns to the page that was open. */
 std::string openPageId,openPageOwner;
 static constexpr int kMaxPagesPerPlugin=16;
 static constexpr int kMaxSlotsPerPlugin=8;
 /* Default board panel geometry, in unscaled pixels; a slot's preferred size
    wins when it asks for one, and the host shrinks a panel that would not fit
    the window it is drawn into. */
 static constexpr float kSlotDefaultWidth=360.f,kSlotDefaultHeight=300.f;
 static constexpr float kSlotMargin=18.f,kSlotTop=96.f;
 /* Width of the host-drawn scroll strip inside a panel's content region. */
 static constexpr float kSlotScrollbarWidth=12.f;
 /* Size of the host-drawn collapse toggle in the panel header. */
 static constexpr float kSlotToggleSize=22.f;
static void log_api(void* c,const char* msg){auto& p=*(Loaded*)c;p.owner->logger("["+p.id+"] "+(msg?msg:""));}
 /* report_status: the plugin's own line for the Mods page and for loader.log.
    The message is copied, capped and de-duplicated, because a plugin may call
    this from a per-frame callback without wanting to flood the log. */
 static int report_status_api(void* c,int level,const char* message){
  auto* p=(Loaded*)c;if(!p||!message||level<0||level>2)return -1;
  std::string text(message);if(text.size()>512)text.resize(512);if(text.empty())return -1;
  auto& r=*p->owner;p->reported=true;
  r.statuses[p->id]=text;r.statusLevels[p->id]=level;
  auto seen=r.lastReportedStatus.find(p->id);
  if(seen==r.lastReportedStatus.end()||seen->second!=text){r.lastReportedStatus[p->id]=text;r.logger("["+p->id+"] status: "+text);}
  return 0;
 }
 static void* resolve(void* c,const char* name){auto& p=*(Loaded*)c;auto& m=p.owner->symbols->values;auto i=m.find(name?name:"");return i==m.end()?nullptr:i->second;}
 static void* engine_api(void* c,const char* name){auto& p=*(Loaded*)c;return name?(void*)GetProcAddress(p.owner->engine,name):nullptr;}
 static int create_ui_texture_api(void* c,const TCUiTexturePixels* pixels,TCUiTexture* out){
  auto& p=*(Loaded*)c;if(!p.owner->textures.onThread()||(!p.accepting&&!p.active))return -1;
  try{return p.owner->textures.create(&p,pixels,out);}catch(...){return -4;}
 }
 static int load_ui_texture_api(void* c,const char* path,uint32_t filter,TCUiTexture* out){
  auto& p=*(Loaded*)c;if(!p.owner->textures.onThread()||(!p.accepting&&!p.active))return -1;
  if(!path||!out||out->size<sizeof(*out)||filter>1)return -2;
  try{
   std::string relative(path);if(!safe(relative)||relative.rfind("native/",0)!=0)return -2;
   auto file=p.packageRoot/fs::u8path(relative);no_links(p.owner->core.root,file);
   std::error_code error;auto length=fs::file_size(file,error);if(error||!length||length>64u*1024u*1024u)return -4;
   auto bytes=read(file);std::vector<unsigned char> pixels;uint32_t width=0,height=0;
   if(!ui_texture::decode(bytes.data(),bytes.size(),pixels,width,height))return -4;
   TCUiTexturePixels definition{sizeof(TCUiTexturePixels),width,height,filter,pixels.data(),pixels.size()};
   return p.owner->textures.create(&p,&definition,out);
  }catch(...){return -4;}
 }
 static int release_ui_texture_api(void* c,uint64_t handle){auto& p=*(Loaded*)c;return p.owner->textures.release(&p,handle);}
static int hook_api(void* c,void* target,void* detour,void** original){auto& p=*(Loaded*)c;auto& r=*p.owner;
 if(!p.accepting||!target||!detour||!original||!r.symbols->functions.count(target)){log_api(c,"Hook rejected: phase, target or conflict");return -1;}
 /* A target the loader already owns - today scene.change, watched so that Board
    handles are invalidated whatever the installed mods do - is refused with its
    reason, so an author learns what to use instead of silently losing the
    lifetime guarantee by taking the detour first. */
 auto owned=r.loaderOwned.find(target);
 if(owned!=r.loaderOwned.end()){log_api(c,("Hook rejected: the loader owns this target for "+owned->second+"; use the event instead of create_hook").c_str());return -1;}
 if(r.ownedHooks.count(target)){log_api(c,"Hook rejected: phase, target or conflict");return -1;}
 /* A hook chain point is owned by the loader, but only because several plugins
    may need it; the way in is register_hook_chain, not a private detour. */
 if(r.chainTargets.count(target)){log_api(c,"Hook rejected: this target is a loader hook chain point; use register_hook_chain instead of create_hook");return -1;}
 auto status=MH_CreateHook(target,detour,original);if(status!=MH_OK){log_api(c,MH_StatusToString(status));return (int)status+1;}p.hooks.push_back(target);r.ownedHooks.insert(target);return 0;}
 /* ---------------------------------------------------------------------
    Hook chains (loader 0.6.0)
    ------------------------------------------------------------------
    A hook point is a game function whose signature the loader knows (the
    catalogue is src/symbol_profile.hpp plus the typed thunks below).  The
    loader owns the single detour and calls the joined plugins in a defined
    order, so two mods that both want sim_do no longer fight over the target.
    A chain is installed only when at least one plugin joined it, so a setup
    without such a mod leaves the game's code untouched. */
 static constexpr size_t kMaxHookLinksPerPoint=32;
 struct HookLink {std::string mod;int32_t priority;uint64_t order;TCHookCallback callback;void* user;bool disabled=false;};
 struct HookChain {
  NativeRuntime* owner=nullptr;uint32_t id=0;const char* alias="";void* target=nullptr;
  void* trampoline=nullptr;void (*invoke_original)(TCHookCall*)=nullptr;
  std::vector<HookLink> links;bool installed=false;
 };
 /* One cursor per chain run.  `cursor` is the index of the next link, shared
    with nested runs so a link that calls run_chain() advances the same walk
    instead of restarting it, and `original_ran` is what keeps the game's own
    function from running twice. */
 struct ChainCursor {HookChain* chain;size_t* cursor;bool* original_ran;};
 inline static NativeRuntime* activeInstance=nullptr;
 std::vector<HookChain> chains;
 std::set<void*> chainTargets;
 std::map<std::string,void*> aliases;
 uint64_t hookOrder=0;
 HookChain* chainById(uint32_t id){for(auto& chain:chains)if(chain.id==id)return &chain;return nullptr;}
 int64_t cycleNow(){auto i=aliases.find("sim.cycle");if(i==aliases.end()||!i->second)return -1;using Fn=int64_t(*)();return reinterpret_cast<Fn>(i->second)();}
 /* The last engine frame the loader ran in, or -1 before the first one.  Used to
    tell "the scene switched because this frame loaded the level" from "the
    player left the level later on".

    The value is cached rather than read from igGetFrameCount here: these detours
    also run in tests/hook-chain.cpp, which plays the game with a bare engine DLL
    and no frame loop at all, and calling into an engine that was never stepped
    is a fault.  One frame of staleness is enough for the comparison below (a
    scene change is allowed to be up to one frame after its level load). */
 int64_t engineFrame() const {return lastEngineFrame;}
 static void invokeOriginalSimDo(TCHookCall* call){auto* chain=activeInstance?activeInstance->chainById(TC_HOOK_SIM_DO):nullptr;if(!chain||!chain->trampoline)return;auto* a=(TCHookSimDoArgs*)call->args;using Fn=void(*)(void*,uint8_t,int64_t);reinterpret_cast<Fn>(chain->trampoline)(a->model,(uint8_t)a->command,a->target);}
 static void invokeOriginalLevelLoad(TCHookCall* call){auto* chain=activeInstance?activeInstance->chainById(TC_HOOK_LEVEL_LOAD):nullptr;if(!chain||!chain->trampoline)return;auto* a=(TCHookLevelLoadArgs*)call->args;using Fn=void(*)(void*,const void*);reinterpret_cast<Fn>(chain->trampoline)(a->board_model,a->name);}
 /* Runs the remaining links and then the game's own function, once.  Used both
    as the entry point of a chain run and as call->run_chain(). */
 static int32_t chainRun(TCHookCall* call){
  auto* state=call?(ChainCursor*)call->loader_state:nullptr;if(!state||!state->chain)return 1;
  auto& chain=*state->chain;
  while(*state->cursor<chain.links.size()){
   const size_t index=*state->cursor;*state->cursor=index+1;
   ChainCursor inner{&chain,state->cursor,state->original_ran};
   auto* savedState=(ChainCursor*)call->loader_state;auto* savedUser=call->user;
   call->loader_state=&inner;call->user=chain.links[index].user;
   int stop=0;
   if(!chain.links[index].disabled){
    /* The mark only feeds the fault journal: containment is not offered (see
       src/fault_guard.hpp for why), so a C++ exception is what the loader can
       actually survive, and a hardware fault names the culprit in fault.log. */
    fault::Scope mark(chain.links[index].mod.c_str(),chain.alias);
    try{stop=chain.links[index].callback(call)?1:0;}
    catch(const std::exception&e){if(chain.owner)chain.owner->logger("Hook chain "+std::string(chain.alias)+": "+chain.links[index].mod+" threw: "+e.what());stop=1;}
    catch(...){if(chain.owner)chain.owner->logger("Hook chain "+std::string(chain.alias)+": "+chain.links[index].mod+" threw");stop=1;}
   }
   call->loader_state=savedState;call->user=savedUser;
   if(stop)break;
  }
  if(!*state->original_ran&&!call->skip_original&&chain.invoke_original){*state->original_ran=true;chain.invoke_original(call);}
  return 0;
 }
 static void detourSimDo(void* model,uint8_t command,int64_t target){
  auto* self=activeInstance;if(!self)return;auto* chain=self->chainById(TC_HOOK_SIM_DO);if(!chain)return;
  TCHookSimDoArgs args{sizeof(TCHookSimDoArgs),command,model,target};
  TCHookCall call{};
  call.size=sizeof(TCHookCall);call.hook_id=chain->id;call.cycle=self->cycleNow();call.args=&args;call.run_chain=&NativeRuntime::chainRun;
  size_t cursor=0;bool originalRan=false;ChainCursor state{chain,&cursor,&originalRan};
  call.loader_state=&state;
  chainRun(&call);
 }
 static void detourLevelLoad(void* model,const void* name){
  auto* self=activeInstance;if(!self)return;auto* chain=self->chainById(TC_HOOK_LEVEL_LOAD);if(!chain)return;
  TCHookLevelLoadArgs args{sizeof(TCHookLevelLoadArgs),0u,model,name};
  TCHookCall call{};
  call.size=sizeof(TCHookCall);call.hook_id=chain->id;call.cycle=self->cycleNow();call.args=&args;call.run_chain=&NativeRuntime::chainRun;
  size_t cursor=0;bool originalRan=false;ChainCursor state{chain,&cursor,&originalRan};
  call.loader_state=&state;
  chainRun(&call);
 }
 /* Resolves the whole symbol profile once, and reserves every hook point so a
    raw create_hook on the same target is refused with a pointer to the chain. */
 void buildChains(){
  chains.clear();chainTargets.clear();aliases.clear();
  for(auto& entry:symbol_profile()){
   auto found=symbols->values.find(entry.coff);
   if(found==symbols->values.end()){logger(std::string("Symbol profile: unresolved ")+entry.alias+" ("+entry.coff+")");continue;}
   aliases[entry.alias]=found->second;
  }
  logger("Symbol profile: "+std::to_string(aliases.size())+"/"+std::to_string(symbol_profile().size())+" aliases resolved");
  chains.push_back({this,TC_HOOK_SIM_DO,"sim.do",nullptr,nullptr,&NativeRuntime::invokeOriginalSimDo,{},false});
  chains.push_back({this,TC_HOOK_LEVEL_LOAD,"level.load",nullptr,nullptr,&NativeRuntime::invokeOriginalLevelLoad,{},false});
  for(auto& chain:chains){auto found=aliases.find(chain.alias);if(found==aliases.end())continue;chain.target=found->second;chainTargets.insert(found->second);}
 }
 static void* resolve_alias_api(void* c,const char* name){auto& p=*(Loaded*)c;auto found=p.owner->aliases.find(name?name:"");return found==p.owner->aliases.end()?nullptr:found->second;}
 /* ---------------------------------------------------------------------
    Event bus (loader 0.6.0)
    ------------------------------------------------------------------
    Facts about the running game that every interesting mod ends up hunting for
    ("a level was loaded", "the player asked the simulation to run", "the game
    saved") are watched here once, shared between plugins, and handed out as
    typed events.  The listener list lives with the plugins, so a rejected
    plugin is dropped from the bus like its hooks and its UI. */
 static constexpr size_t kMaxEventListeners=64;
  struct EventListener {std::string mod;uint32_t kinds;uint64_t order;TCEventCallback callback;void* user;bool disabled=false;};
 std::vector<EventListener> listeners;
 uint64_t eventOrder=0;
 GameHandles gameHandles;
 bool sceneHooked=false,saveHooked=false;
 void* saveOriginal=nullptr;
 void* sceneOriginal=nullptr;
 bool wantsEvents(uint32_t kind) const {for(auto& listener:listeners)if(listener.kinds&kind)return true;return false;}
 void dispatchEvent(uint32_t kind,uint32_t flags,void* subject,const char* name){
  if(listeners.empty())return;
  TCEvent event{};
  event.size=sizeof(TCEvent);event.kind=kind;event.flags=flags;event.cycle=cycleNow();
  event.subject=subject;event.name=name;
  for(auto& listener:listeners){
   if(!(listener.kinds&kind)||listener.disabled)continue;
   event.user=listener.user;
   fault::Scope mark(listener.mod.c_str(),"an event listener");
   try{listener.callback(&event);}
   catch(const std::exception&e){logger("Event "+std::to_string(kind)+" listener "+listener.mod+" threw: "+e.what());}
   catch(...){logger("Event "+std::to_string(kind)+" listener "+listener.mod+" threw");}
  }
 }
 static int add_event_listener_api(void* c,uint32_t kinds,TCEventCallback callback,void* user){
  auto& p=*(Loaded*)c;auto& r=*p.owner;
  if(!p.accepting)return TC_EVENT_ERR_UNAVAILABLE;
  if(!callback||kinds==0)return TC_EVENT_ERR_ARGUMENT;
  if(r.listeners.size()>=kMaxEventListeners)return TC_EVENT_ERR_CAPACITY;
  r.listeners.push_back(EventListener{p.id,kinds,r.eventOrder++,callback,user});
  log_api(c,("Event listener added for mask "+std::to_string(kinds)).c_str());
  return TC_EVENT_OK;
 }
 void dropEventListeners(const std::string& id){listeners.erase(std::remove_if(listeners.begin(),listeners.end(),[&](const EventListener&listener){return listener.mod==id;}),listeners.end());}
 /* The loader's own links.  They sit at the earliest priority so they see every
    call - including the ones another mod swallows - and they never swallow
    anything themselves: an event reports, it does not steer. */
 static constexpr int32_t kLoaderLinkPriority=-0x7fffffff;
 static int eventSimDoLink(TCHookCall* call){auto* args=hook::simDoArgs(call);auto* self=activeInstance;if(self&&args)self->dispatchEvent(TC_EVENT_SIM_COMMAND,args->command,nullptr,nullptr);return 0;}
 static int eventLevelLoadLink(TCHookCall* call){auto* args=hook::levelLoadArgs(call);auto* self=activeInstance;if(self&&args){self->gameHandles.enterBoard(args->board_model,self->engineFrame());self->dispatchEvent(TC_EVENT_LEVEL_LOAD,0,args->board_model,nullptr);}return 0;}
 static void detourSceneChange(void* context,int scene){
  auto* self=activeInstance;
  if(self){
   /* A switch in the frame that loaded the level is the entry to the board
      scene, not a leave: killing the handle there would empty the registry for
      the whole level (the pinned build loads the level and switches the scene in
      one frame).  The handle state is settled before the event is raised, so a
      listener sees the same answer a later frame would. */
   if(!self->gameHandles.leaveBoardOnSceneChange(self->engineFrame()))
    self->logger("Game handles: scene "+std::to_string(scene)+" in the level's own frame; the Board handle stays valid");
   /* subject: the context the game itself passes to change_scene.  It is what a
      mod (or a driver) has to hand back to change the scene again, and it was
      only reachable by hooking this very function before the loader took it
      over - the board-panel playtest lost its scene step to exactly that. */
   self->dispatchEvent(TC_EVENT_SCENE_CHANGE,static_cast<uint32_t>(scene),context,nullptr);
  }
  if(self&&self->sceneOriginal)reinterpret_cast<void(*)(void*,int)>(self->sceneOriginal)(context,scene);
 }
 static void detourSave(){
  auto* self=activeInstance;
  if(self){
   /* The save counter is a data symbol; read it if the profile resolved it, so a
      listener can tell "the fifth save" from the first. */
   uint32_t count=0;auto found=self->aliases.find("save.count");
   if(found!=self->aliases.end()&&found->second)count=static_cast<uint32_t>(*reinterpret_cast<const int64_t*>(found->second));
   self->dispatchEvent(TC_EVENT_SAVE,count,nullptr,nullptr);
  }
  if(self&&self->saveOriginal)reinterpret_cast<void(*)()>(self->saveOriginal)();
 }
 /* Registers a link owned by the loader itself (no plugin behind it), so a
    rejected plugin can never remove it. */
 int registerOwnChainLink(uint32_t id,int32_t priority,TCHookCallback callback){
  auto* chain=chainById(id);if(!chain)return TC_HOOK_ERR_ID;
  if(!chain->target)return TC_HOOK_ERR_TARGET;
  if(chain->links.size()>=kMaxHookLinksPerPoint)return TC_HOOK_ERR_CAPACITY;
  chain->links.push_back(HookLink{std::string(),priority,hookOrder++,callback,nullptr});
  return TC_HOOK_OK;
 }
 /* Installs what the registered listeners need; called once, after every plugin
    has had its chance to subscribe. */
  /* Why scene.change is owned from boot rather than armed with the other event
     sources: the Board handle registry is invalidated here, and a plugin that
     hooked the target first would take the detour and silently switch that - and
     the SCENE_CHANGE event - off.  create_hook therefore refuses the point (see
     hook_api) instead of letting the lifetime guarantee depend on load order. */
  void armSceneChangeTracking(){
   if(sceneHooked)return;
   auto found=aliases.find("scene.change");
   if(found==aliases.end()){logger("Event source scene.change unavailable: the symbol profile did not resolve it");return;}
   if(MH_CreateHook(found->second,reinterpret_cast<void*>(&NativeRuntime::detourSceneChange),&sceneOriginal)!=MH_OK){logger("Event source scene.change: hook failed");return;}
   if(MH_EnableHook(found->second)!=MH_OK){MH_RemoveHook(found->second);logger("Event source scene.change: hook failed");return;}
   sceneHooked=true;ownedHooks.insert(found->second);
   loaderOwned[found->second]="the SCENE_CHANGE event and Board handle invalidation";
   logger("Event source scene.change armed (Board handle tracking)");
  }
  void armEventSources(){
  /* This one link serves both the level.load event and Board handle tracking.
     The log keeps the event-source wording (users and tests read that line)
     and states the handle registry on a second, explicit line. */
  {const int status=registerOwnChainLink(TC_HOOK_LEVEL_LOAD,kLoaderLinkPriority,&eventLevelLoadLink);logger("Event source level.load: "+std::string(hook::errorText(status)));logger("Game handle tracking level.load: "+std::string(hook::errorText(status)));}
  if(wantsEvents(TC_EVENT_SIM_COMMAND))
   {const int status=registerOwnChainLink(TC_HOOK_SIM_DO,kLoaderLinkPriority,&eventSimDoLink);logger("Event source sim.do: "+std::string(hook::errorText(status)));}
  /* scene.change is not armed here: it is installed in boot(), before any
     plugin loads, because Board handle invalidation must not depend on what the
     installed mods subscribe to or hook (see armSceneChangeTracking). */
  if(wantsEvents(TC_EVENT_SAVE)&&!saveHooked){
   auto found=aliases.find("save.level");
   if(found==aliases.end()){logger("Event source save unavailable: the symbol profile did not resolve save.level");}
   else if(MH_CreateHook(found->second,reinterpret_cast<void*>(&NativeRuntime::detourSave),&saveOriginal)==MH_OK&&MH_EnableHook(found->second)==MH_OK){saveHooked=true;ownedHooks.insert(found->second);logger("Event source save armed");}
   else logger("Event source save: hook failed");
  }
 }
 static int register_hook_chain_api(void* c,uint32_t hook_id,int32_t priority,TCHookCallback callback,void* user){
  auto& p=*(Loaded*)c;auto& r=*p.owner;
  if(!p.accepting)return TC_HOOK_ERR_UNAVAILABLE;
  if(!callback)return TC_HOOK_ERR_ARGUMENT;
  auto* chain=r.chainById(hook_id);
  if(!chain)return TC_HOOK_ERR_ID;
  /* The point exists in the ABI but not in this game build: say so now, while
     the plugin can still refuse to load, instead of at the first call. */
  if(!chain->target)return TC_HOOK_ERR_TARGET;
  if(chain->links.size()>=kMaxHookLinksPerPoint)return TC_HOOK_ERR_CAPACITY;
  chain->links.push_back(HookLink{p.id,priority,r.hookOrder++,callback,user});
  log_api(c,("Hook chain "+std::string(chain->alias)+" joined (priority "+std::to_string(priority)+")").c_str());
  return TC_HOOK_OK;
 }
 /* Ordering: priority, then mod id, then registration order, so the sequence is
    the same on every run whatever order the packages were enabled in. */
 void installChains(){
  for(auto& chain:chains){
   if(chain.installed||chain.links.empty())continue;
   std::stable_sort(chain.links.begin(),chain.links.end(),[](const HookLink&a,const HookLink&b){if(a.priority!=b.priority)return a.priority<b.priority;if(a.mod!=b.mod)return a.mod<b.mod;return a.order<b.order;});
   void* detour=chain.id==TC_HOOK_SIM_DO?reinterpret_cast<void*>(&NativeRuntime::detourSimDo)
    :reinterpret_cast<void*>(&NativeRuntime::detourLevelLoad);
   auto status=MH_CreateHook(chain.target,detour,&chain.trampoline);
   if(status!=MH_OK||MH_EnableHook(chain.target)!=MH_OK){logger("Hook chain "+std::string(chain.alias)+": install failed ("+std::string(MH_StatusToString(status))+")");continue;}
   ownedHooks.insert(chain.target);chain.installed=true;
   std::string who;for(auto& link:chain.links){if(!who.empty())who+=", ";who+=link.mod+"@"+std::to_string(link.priority);}
   logger("Hook chain "+std::string(chain.alias)+" installed with "+std::to_string(chain.links.size())+" link(s): "+who);
  }
 }
 /* A rejected plugin keeps no link behind, exactly like its hooks and its UI. */
 void dropHookLinks(const std::string& id){for(auto& chain:chains)chain.links.erase(std::remove_if(chain.links.begin(),chain.links.end(),[&](const HookLink&link){return link.mod==id;}),chain.links.end());}
 static int register_logic_api(void* c,const TCLogicDefinition* d){auto& p=*(Loaded*)c;if(!p.accepting)return -1;int result=logic::add(d);if(!result)p.logicIds.push_back(d->custom_id);return result;}
 static int get_current_game_handle_api(void* c,uint32_t kind,TCGameHandle* out){auto& p=*(Loaded*)c;return p.owner->gameHandles.current(kind,out);}
 static int validate_game_handle_api(void* c,const TCGameHandle* handle){auto& p=*(Loaded*)c;return p.owner->gameHandles.valid(handle);}
 static int resolve_game_handle_api(void* c,const TCGameHandle* handle,const void** out){auto& p=*(Loaded*)c;return p.owner->gameHandles.resolve(handle,out);}
 static int register_component_api(void* c,const TCNativeComponentDefinition* d){auto& p=*(Loaded*)c;if(!p.accepting)return -1;try {int result=registerNativeComponent(&p.host,d);if(!result){p.logicIds.push_back(d->custom_id);p.componentIds.push_back(d->custom_id);}else log_api(c,("Component registration rejected: "+std::to_string(result)).c_str());return result;}catch(const std::exception& e){log_api(c,e.what());return -4;}catch(...){return -4;}}
 static int register_ui_page_api(void* c,const TCUiPageDefinition* d){auto& p=*(Loaded*)c;if(!p.accepting)return -1;if(!d||d->size<sizeof(TCUiPageDefinition)||!d->page_id||!d->page_id[0]||!d->draw)return -2;std::string id=d->page_id;if(id.size()>=64||id.find("###")!=std::string::npos)return -2;if(p.pages.size()>=kMaxPagesPerPlugin)return -4;for(auto& page:p.pages)if(page.id==id)return -3;p.pages.push_back(UiPage{id,d->title?d->title:"",d->draw,d->user,false});log_api(c,("UI page registered: "+id).c_str());return 0;}
 void reject(Loaded& p){logic::finish(p.logicIds,false);if(!p.componentIds.empty()){TCGameModel game;if(game.load(&p.host))for(auto id:p.componentIds)game.removeCustomPrototype(id);p.componentIds.clear();}for(auto target:p.hooks){MH_DisableHook(target);MH_RemoveHook(target);ownedHooks.erase(target);}p.hooks.clear();dropHookLinks(p.id);dropEventListeners(p.id);if(p.plugin.on_unload){try{p.plugin.on_unload(p.plugin.user);}catch(...) {}}textures.reject(&p);p.plugin={};p.accepting=false;p.active=false;/* Keep rejected DLL mapped: it may have static destructors/threads. */}


 /* Registers a panel for one of the game's own screens.  Same validation and
    ownership rules as a page: the host copies the strings, rejects duplicates
    inside one plugin, and a rejected plugin leaves no entry behind. */
 static int register_ui_slot_api(void* c,const TCUiSlotDefinition* d){
  auto& p=*(Loaded*)c;if(!p.accepting)return -1;
  if(!d||d->size<sizeof(TCUiSlotDefinition)||!d->slot_id||!d->slot_id[0]||!d->draw)return -2;
  if(d->kind!=TC_UI_SLOT_BOARD_SIDE&&d->kind!=TC_UI_SLOT_BOARD_TOOLBAR)return -2;
  std::string id=d->slot_id;if(id.size()>=64||id.find("###")!=std::string::npos)return -2;
  if(p.slots.size()>=kMaxSlotsPerPlugin)return -4;
  for(auto& slot:p.slots)if(slot.id==id)return -3;
  BoardSlot slot;
  slot.id=id;slot.title=d->title?d->title:"";slot.draw=d->draw;slot.user=d->user;
  slot.kind=d->kind;
  /* A non-finite or negative request would poison the layout arithmetic. */
  slot.width=(std::isfinite(d->preferred_width)&&d->preferred_width>0.f)?d->preferred_width:0.f;
  slot.height=(std::isfinite(d->preferred_height)&&d->preferred_height>0.f)?d->preferred_height:0.f;
  p.slots.push_back(std::move(slot));
  log_api(c,("UI slot registered: "+id+(d->kind==TC_UI_SLOT_BOARD_TOOLBAR?" (board tool)":" (board side panel)")).c_str());
  return 0;
 }
 /* ---------------------------------------------------------------------
    Native UI pages (loader 0.5.0)
    ------------------------------------------------------------------ */
 /* ImGui entry points the page container needs, resolved from the game's own
    engine module - never from this proxy.  Each signature below is the one the
    engine was verified to use, so a mistyped one cannot corrupt the stack
    silently.  A missing export degrades to "no page container" while the rest
    of the loader keeps working. */
 struct PageApi {
  struct Vec2{float x,y;};
  void* (*getViewport)()=nullptr;
  /* By value, not by pointer: the loader's own Mods modal calls these the same
     way and is sized correctly, while the pointer form silently produced a
     32x32 default window (ImGui's minimum) - which is why the page drew but
     nothing in it could ever be hovered or clicked. */
  void (*setNextWindowPos)(Vec2,int,Vec2)=nullptr;
  void (*setNextWindowSize)(Vec2,int)=nullptr;
  void (*setNextWindowBgAlpha)(float)=nullptr;
  bool (*begin)(const char*,bool*,int)=nullptr;
  void (*end)()=nullptr;
  void (*pushId)(const char*)=nullptr;
  void (*popId)()=nullptr;
  void (*text)(const char*,const char*)=nullptr;
  void (*separator)()=nullptr;
  bool (*button)(const char*,Vec2)=nullptr;
  void (*setWindowFontScale)(float)=nullptr;
  void (*setCursorPos)(Vec2)=nullptr;
  void (*pushTextWrapPos)(float)=nullptr;
  void (*popTextWrapPos)()=nullptr;
  /* Only for logging what a scale push did (see the page container). */
  float (*getFontSize)()=nullptr;
  void (*setNextWindowFocus)()=nullptr;
  /* Shared content-region entry points (see drawContentRegion below). */
  void (*cursorScreen)(Vec2*)=nullptr;
  void (*mouseScreen)(Vec2*)=nullptr;
  bool (*beginChild)(const char*,Vec2,int,int)=nullptr;
  void (*endChild)()=nullptr;
  void (*contentAvail)(Vec2*)=nullptr;
  float (*getScrollY)()=nullptr;
  float (*getScrollMaxY)()=nullptr;
  void (*setScrollY)(float)=nullptr;
  bool (*invisibleButton)(const char*,Vec2,int)=nullptr;
  bool (*isItemActive)()=nullptr;
  bool (*isItemHovered)(int)=nullptr;
  bool (*isWindowHovered)(int)=nullptr;
  bool available() const {
   return getViewport&&setNextWindowPos&&setNextWindowSize&&setNextWindowBgAlpha&&begin&&end&&
          pushId&&popId&&text&&separator&&button&&setWindowFontScale&&setCursorPos&&
          pushTextWrapPos&&popTextWrapPos;
  }
 };
static void* proc(HMODULE module,const char* name){return module?(void*)GetProcAddress(module,name):nullptr;}
static PageApi& pageApi(){
  static PageApi value;static bool bound=false;
  if(!bound){bound=true;
   HMODULE module=GetModuleHandleW(L"tc_game_engine.dll");if(!module)module=GetModuleHandleW(nullptr);
   value.getViewport=(decltype(value.getViewport))proc(module,"igGetMainViewport");
   value.setNextWindowPos=(decltype(value.setNextWindowPos))proc(module,"igSetNextWindowPos");
   value.setNextWindowSize=(decltype(value.setNextWindowSize))proc(module,"igSetNextWindowSize");
   value.setNextWindowBgAlpha=(decltype(value.setNextWindowBgAlpha))proc(module,"igSetNextWindowBgAlpha");
   value.begin=(decltype(value.begin))proc(module,"igBegin");
   value.end=(decltype(value.end))proc(module,"igEnd");
   value.pushId=(decltype(value.pushId))proc(module,"igPushID_Str");
   value.popId=(decltype(value.popId))proc(module,"igPopID");
   value.text=(decltype(value.text))proc(module,"igTextUnformatted");
   value.separator=(decltype(value.separator))proc(module,"igSeparator");
   value.button=(decltype(value.button))proc(module,"igButton");
    value.setCursorPos=(decltype(value.setCursorPos))proc(module,"igSetCursorPos");
   value.pushTextWrapPos=(decltype(value.pushTextWrapPos))proc(module,"igPushTextWrapPos");
   value.popTextWrapPos=(decltype(value.popTextWrapPos))proc(module,"igPopTextWrapPos");
   value.setNextWindowFocus=(decltype(value.setNextWindowFocus))proc(module,"igSetNextWindowFocus");
   value.setWindowFontScale=(decltype(value.setWindowFontScale))proc(module,"igSetWindowFontScale");
   value.getFontSize=(decltype(value.getFontSize))proc(module,"igGetFontSize");
   value.cursorScreen=(decltype(value.cursorScreen))proc(module,"igGetCursorScreenPos");
   value.mouseScreen=(decltype(value.mouseScreen))proc(module,"igGetMousePos");
   value.beginChild=(decltype(value.beginChild))proc(module,"igBeginChild_Str");
   value.endChild=(decltype(value.endChild))proc(module,"igEndChild");
   value.contentAvail=(decltype(value.contentAvail))proc(module,"igGetContentRegionAvail");
   value.getScrollY=(decltype(value.getScrollY))proc(module,"igGetScrollY");
   value.getScrollMaxY=(decltype(value.getScrollMaxY))proc(module,"igGetScrollMaxY");
   value.setScrollY=(decltype(value.setScrollY))proc(module,"igSetScrollY_Float");
   value.invisibleButton=(decltype(value.invisibleButton))proc(module,"igInvisibleButton");
   value.isItemActive=(decltype(value.isItemActive))proc(module,"igIsItemActive");
   value.isItemHovered=(decltype(value.isItemHovered))proc(module,"igIsItemHovered");
   value.isWindowHovered=(decltype(value.isWindowHovered))proc(module,"igIsWindowHovered");
  }
   return value;
 }
 /* Vertical offset of the page content: title bar plus separator. */
 float pageHeaderHeight() const {return 64.f*(core.ui_scale>0?core.ui_scale:1.f);}
 /* ---------------------------------------------------------------------
    Shared content region (menu page container and board side panels)
    ------------------------------------------------------------------
    Both host containers hand plugin content the same thing: a child window
    that clips it to the container, plus a scrollbar the host draws and drives
    itself.  This build's style gives children no usable scrollbar and the
    mouse wheel never reaches ImGui, so a wheel or a native scrollbar would be
    dead weight; the strip below is real mouse input the playtests can drive.

    The caller owns the ID scopes and decides what to do when the plugin's
    draw throws; this returns false in that case.  `origin` receives the
    region's top-left corner in screen coordinates and `hovered` whether the
    region (or its strip) owns the mouse this frame. */
 template<class Api>
 void scrollStrip(const Api& api,float available,float drawHeight,float top,float scale,
                  const std::string& who){
  if(!api.getScrollMaxY||!api.setScrollY||!api.invisibleButton||!api.isItemActive)return;
  const float maximum=api.getScrollMaxY();
  if(!(maximum>1.f))return;
  const float stripWidth=kSlotScrollbarWidth*scale;
  api.setCursorPos(typename Api::Vec2{available-stripWidth,api.getScrollY()});
  typename Api::Vec2 origin{};
  api.cursorScreen(&origin);
  api.invisibleButton("###scrollstrip",typename Api::Vec2{stripWidth,drawHeight},0);
  const bool hovered=api.isItemHovered&&api.isItemHovered(0);
  const bool active=api.isItemActive();
  /* Bounded log of the strip's own state: this is the affordance the user
     drags, so "the content did not scroll" has to be attributable. */
  const int now=(active?2:0)|(hovered?1:0);
  const auto previousStrip=lastStripState.find(who);
  if(previousStrip==lastStripState.end()?true:previousStrip->second!=now){
   lastStripState[who]=now;
   logger("Host scroll strip "+who+" hovered="+std::to_string(hovered?1:0)+" active="+
          std::to_string(active?1:0)+" max="+std::to_string((int)maximum)+" scroll="+
          std::to_string((int)api.getScrollY())+" x="+std::to_string((int)origin.x)+" y="+
          std::to_string((int)origin.y)+" w="+std::to_string((int)stripWidth)+" h="+
          std::to_string((int)drawHeight));
  }
  if(!active||!api.mouseScreen||drawHeight<=1.f)return;
  typename Api::Vec2 mouse{};
  api.mouseScreen(&mouse);
  float ratio=(mouse.y-top)/drawHeight;
  if(ratio<0.f)ratio=0.f;
  if(ratio>1.f)ratio=1.f;
  api.setScrollY(ratio*maximum);
 }
 template<class Api>
 bool drawContentRegion(const Api& api,const typename Api::Vec2& size,float scale,const char* id,
                        const std::string& who,void (*draw)(void*,const TCFrame*,float,float),
                        void* user,const TCFrame& frame,bool& hovered,typename Api::Vec2& origin){
  hovered=false;
  /* Degrade to an unclipped draw rather than drawing nothing: on a build that
     is missing part of the region API the container still works, it just
     cannot clip or scroll. */
  if(!api.beginChild||!api.endChild||!api.cursorScreen||!api.isWindowHovered){
   api.pushTextWrapPos(size.x);
   bool ok=true;
   try{draw(user,&frame,size.x,size.y);}
   catch(const std::exception& e){ok=false;logger("Plugin content threw: "+who+": "+e.what());}
   catch(...){ok=false;logger("Plugin content threw: "+who);}
   api.popTextWrapPos();
   return ok;
  }
  if(!(size.x>=32.f&&size.y>=32.f))return true;
  api.cursorScreen(&origin);
  /* NoScrollbar: this build's own child scrollbar is not interactive here (the
     host draws and drives the strip instead), and leaving it on reserves a
     second, dead column of width plus a visible bar next to the strip. */
  const bool visible=api.beginChild(id,size,0,1<<3);
  bool ok=true;
  if(visible){
   typename Api::Vec2 avail{};
   if(api.contentAvail)api.contentAvail(&avail);
   const float available=avail.x>0.f?avail.x:size.x;
   const float stripWidth=kSlotScrollbarWidth*scale;
   const float drawWidth=available>stripWidth*2.f?available-stripWidth:available;
   const float drawHeight=avail.y>0.f?avail.y:size.y;
   api.pushTextWrapPos(drawWidth);
   try{draw(user,&frame,drawWidth,drawHeight);}
   catch(const std::exception& e){ok=false;logger("Plugin content threw: "+who+": "+e.what());}
   catch(...){ok=false;logger("Plugin content threw: "+who);}
   api.popTextWrapPos();
   scrollStrip(api,available,drawHeight,origin.y,scale,who);
   /* Authoritative scroll offset of the region, logged when it moves: the
      playtests assert on this rather than on a plugin's own reading. */
   if(api.getScrollY){
    const float scroll=api.getScrollY();
    const auto previousScroll=lastScrollY.find(who);
    if(previousScroll==lastScrollY.end()?true:previousScroll->second!=scroll){
     lastScrollY[who]=scroll;
     logger("Host content scroll "+who+" y="+std::to_string((int)scroll)+" max="+
            std::to_string((int)(api.getScrollMaxY?api.getScrollMaxY():0.f)));
    }
   }
   if(api.isWindowHovered(0))hovered=true;
  }
  api.endChild();
  return ok;
 }
 /* The host container.  While a page is open this window covers the whole
    main viewport, so it *replaces* the home page instead of floating over it
    and the back button returns to the home page.  Levels are unaffected: the
    page only draws while the home page is the screen being built.

    The window and both ID scopes are opened and closed here, so a page cannot
    leave the window stack unbalanced. */
void drawPageContainer(Loaded& owner,UiPage& page,const TCFrame& frame){
  const PageApi& api=pageApi();
  const char* failure=nullptr;
  if(!api.available())failure="engine is missing part of the page container API";
  const PageApi::Vec2* viewport=reinterpret_cast<const PageApi::Vec2*>(api.getViewport());
  const PageApi::Vec2 position=viewport?viewport[0]:PageApi::Vec2{0,0};
  const PageApi::Vec2 size=viewport?viewport[2]:PageApi::Vec2{0,0};
  if(!failure&&!viewport)failure="igGetMainViewport returned null";
  if(!failure&&(size.x<64.f||size.y<64.f))failure="viewport too small";
  if(failure){
   /* A page that cannot draw must say so once, not silently do nothing: that
      is the difference between "the host refused" and "the page crashed". */
   static std::string lastFailure;static int repeats=0;
   if(lastFailure!=failure){lastFailure=failure;repeats=1;logger(std::string("Plugin UI page not drawn: ")+failure);}
   else if(++repeats%600==0)logger(std::string("Plugin UI page still not drawn: ")+failure);
   return;
  }
  const float scale=core.ui_scale>0.35f?core.ui_scale:0.35f;
  api.setNextWindowBgAlpha(1.f);
  const PageApi::Vec2 pivot{0,0};
  api.setNextWindowPos(position,1,pivot);
  api.setNextWindowSize(size,1);
  /* Bring the container to the front.  Without this the game's own menu
     window, which covers most of the same area, stays above ours: the mouse
     over that area then hovers the menu window and none of the page's widgets
     ever become hoverable (observed as "the page draws but nothing in it can
     be clicked").  igSetNextWindowFocus takes no arguments, so its signature
     cannot be wrong. */
  if(api.setNextWindowFocus)api.setNextWindowFocus();
  /* NoTitleBar|NoResize|NoMove|NoScrollbar|NoCollapse|NoSavedSettings|
     NoBringToFrontOnFocus */
  const int flags=(1<<0)|(1<<1)|(1<<2)|(1<<3)|(1<<5)|(1<<8);
  if(api.begin("TC Mod Page###TCModPage",nullptr,flags)){
   /* The header uses the same font multiplier the home page uses, so it reads as
      the game's own text instead of guessing a size.  It is set through
      igSetWindowFontScale on *our own* window rather than through the game's
      igPushFontScale/igPopFontScale stack: those two are Nim functions in the
      executable, and driving the game's stack from here tore the page apart -
      a run with them bound never reached the page body at all (and the code had
      been silently asking the engine DLL for them by name, which can never
      resolve - the executable owns those symbols).  Our window, our scale: set
      it for the header, put it back for the content. */
   const bool withFont=api.setWindowFontScale!=nullptr;
   const float headerScale=(2.f*scale)/1.6666666f;
   const float beforeFont=api.getFontSize?api.getFontSize():0.f;
   if(withFont)api.setWindowFontScale(headerScale);
   if(withFont&&api.getFontSize){
    static bool logged=false;
    if(!logged){
     logged=true;
     logger("Mod page font scale applied: window font "+std::to_string((int)beforeFont)+
            " -> "+std::to_string((int)api.getFontSize()));
    }
   }
   api.text(owner.id.c_str(),nullptr);
   if(api.button("< BACK###TCModPageBack",PageApi::Vec2{110.f*scale,40.f*scale}))closePage();
   api.separator();
   if(withFont)api.setWindowFontScale(1.f);
   api.setCursorPos(PageApi::Vec2{0.f,pageHeaderHeight()});
   api.pushId(owner.id.c_str());
   api.pushId(page.id.c_str());
   const float contentHeight=size.y>pageHeaderHeight()?size.y-pageHeaderHeight():0.f;
   /* The page content gets the same clipped, scrollable region as a board
      panel: a page that is taller than the window scrolls instead of drawing
      over the game's UI, and content that does not fit cannot be clicked
      where it is not visible. */
   bool hovered=false;
   PageApi::Vec2 region{0.f,pageHeaderHeight()};
   const std::string who=owner.id+"/"+page.id;
   const bool ok=drawContentRegion(api,PageApi::Vec2{size.x,contentHeight},scale,"###content",
                                    who,page.draw,page.user,frame,hovered,region);
   if(!ok){
    page.draw=nullptr;page.failed=true;
    statuses[owner.id]="UI page disabled after an exception";
   }
   /* One bounded line per page: the playtest needs the content origin to turn
      a widget's own coordinates into a click point. */
   if(!page.failed){
    static int logged=0;static float lastWidth=-1.f,lastHeight=-1.f;
    if(logged<3||contentHeight!=lastHeight||size.x!=lastWidth||logged%600==0){
     ++logged;lastWidth=size.x;lastHeight=contentHeight;
     logger("UI page content "+who+" x="+std::to_string((int)region.x)+" y="+
            std::to_string((int)region.y)+" w="+std::to_string((int)size.x)+" h="+
            std::to_string((int)contentHeight)+" hovered="+std::to_string(hovered?1:0));
    }
   }
   api.popId();
   api.popId();
  }
  api.end();
 }
 /* ---------------------------------------------------------------------
    Circuit-board side panels (loader 0.6.0)
    ------------------------------------------------------------------
    A slot is a panel the host draws inside one of the game's own screens.
    Today there is exactly one kind: the board's right-hand side.  The panel
    is drawn from the board's *own* per-frame UI code, so it exists only while
    the board exists and needs no open/close state of its own.

    The entry points below are resolved from the original engine module, never
    through this proxy: the proxy is what implements igIsAnyItemActive for the
    game, and a panel that called back into it would recurse. */
 struct SlotApi {
  struct Vec2{float x,y;};
  struct Vec4{float x,y,z,w;};
  int (*getFrameCount)()=nullptr;
  double (*getTime)()=nullptr;
  float (*getWindowWidth)()=nullptr;
  float (*getWindowHeight)()=nullptr;
  void (*cursorScreen)(Vec2*)=nullptr;
  void (*mouseScreen)(Vec2*)=nullptr;
  void (*setCursorPos)(Vec2)=nullptr;
  /* Absolute placement, for tools that line up under the game's own controls.
     Optional: without it a tool is drawn wherever the cursor happens to be. */
  void (*setCursorScreen)(Vec2)=nullptr;
  /* Window font scale, left as the game left it: with the game's text font
     borrowed for plugin draws (resolveToolTextFont) that size is the game's own
     UI text size (measured 45 px in the column, the same as the board panels).
     Kept bound so a caller may still adjust it; the toolbar path does not. */
  void (*setWindowFontScale)(float)=nullptr;
  bool (*beginChild)(const char*,Vec2,int,int)=nullptr;
  void (*endChild)()=nullptr;
  void (*pushId)(const char*)=nullptr;
  void (*popId)()=nullptr;
  void (*text)(const char*,const char*)=nullptr;
  void (*separator)()=nullptr;
  void (*pushTextWrapPos)(float)=nullptr;
  void (*popTextWrapPos)()=nullptr;
  void (*pushStyleColor)(int,Vec4)=nullptr;
  void (*popStyleColor)(int)=nullptr;
  bool (*isWindowHovered)(int)=nullptr;
  /* Remaining space inside the current child, scrollbar included in the
     calculation.  Optional: without it the host falls back to the geometry it
     asked for, which is only wrong by one scrollbar width. */
  void (*contentAvail)(Vec2*)=nullptr;
  /* Authoritative scroll offset of the content region, for diagnostics. */
  float (*getScrollY)()=nullptr;
  float (*getScrollMaxY)()=nullptr;
  void (*setScrollY)(float)=nullptr;
  bool (*invisibleButton)(const char*,Vec2,int)=nullptr;
  bool (*isItemActive)()=nullptr;
  bool (*isItemHovered)(int)=nullptr;
  std::string missing;
  bool available() const {return missing.empty();}
 };
 static const SlotApi& slotApi(){
  static SlotApi value;static bool bound=false;
  if(!bound){bound=true;
   HMODULE module=GetModuleHandleW(L"tc_game_engine.dll");if(!module)module=GetModuleHandleW(nullptr);
   value.getFrameCount=(decltype(value.getFrameCount))proc(module,"igGetFrameCount");
   value.getTime=(decltype(value.getTime))proc(module,"igGetTime");
   value.getWindowWidth=(decltype(value.getWindowWidth))proc(module,"igGetWindowWidth");
   value.getWindowHeight=(decltype(value.getWindowHeight))proc(module,"igGetWindowHeight");
   value.cursorScreen=(decltype(value.cursorScreen))proc(module,"igGetCursorScreenPos");
   value.mouseScreen=(decltype(value.mouseScreen))proc(module,"igGetMousePos");
   value.setCursorPos=(decltype(value.setCursorPos))proc(module,"igSetCursorPos");
   value.setCursorScreen=(decltype(value.setCursorScreen))proc(module,"igSetCursorScreenPos");
   /* Window font scale, for callers that want a different text size than the
      game's own.  Optional: guarded at the call site. */
   value.setWindowFontScale=(decltype(value.setWindowFontScale))proc(module,"igSetWindowFontScale");
   value.beginChild=(decltype(value.beginChild))proc(module,"igBeginChild_Str");
   value.endChild=(decltype(value.endChild))proc(module,"igEndChild");
   value.pushId=(decltype(value.pushId))proc(module,"igPushID_Str");
   value.popId=(decltype(value.popId))proc(module,"igPopID");
   value.text=(decltype(value.text))proc(module,"igTextUnformatted");
   value.separator=(decltype(value.separator))proc(module,"igSeparator");
   value.pushTextWrapPos=(decltype(value.pushTextWrapPos))proc(module,"igPushTextWrapPos");
   value.popTextWrapPos=(decltype(value.popTextWrapPos))proc(module,"igPopTextWrapPos");
   value.pushStyleColor=(decltype(value.pushStyleColor))proc(module,"igPushStyleColor_Vec4");
   value.popStyleColor=(decltype(value.popStyleColor))proc(module,"igPopStyleColor");
   value.isWindowHovered=(decltype(value.isWindowHovered))proc(module,"igIsWindowHovered");
   value.contentAvail=(decltype(value.contentAvail))proc(module,"igGetContentRegionAvail");
   value.getScrollY=(decltype(value.getScrollY))proc(module,"igGetScrollY");
   value.getScrollMaxY=(decltype(value.getScrollMaxY))proc(module,"igGetScrollMaxY");
   value.setScrollY=(decltype(value.setScrollY))proc(module,"igSetScrollY_Float");
   value.invisibleButton=(decltype(value.invisibleButton))proc(module,"igInvisibleButton");
   value.isItemActive=(decltype(value.isItemActive))proc(module,"igIsItemActive");
   value.isItemHovered=(decltype(value.isItemHovered))proc(module,"igIsItemHovered");
   /* Record what is missing by name: a silently unavailable panel is the
      hardest kind of failure to diagnose in a running game. */
   struct Entry{const char* name;const void* value;};
   const Entry entries[]={
    {"igGetFrameCount",(const void*)value.getFrameCount},{"igGetTime",(const void*)value.getTime},
    {"igGetWindowWidth",(const void*)value.getWindowWidth},{"igGetWindowHeight",(const void*)value.getWindowHeight},
    {"igGetCursorScreenPos",(const void*)value.cursorScreen},{"igGetMousePos",(const void*)value.mouseScreen},
    {"igSetCursorPos",(const void*)value.setCursorPos},{"igBeginChild_Str",(const void*)value.beginChild},
    {"igEndChild",(const void*)value.endChild},{"igPushID_Str",(const void*)value.pushId},
    {"igPopID",(const void*)value.popId},{"igTextUnformatted",(const void*)value.text},
    {"igSeparator",(const void*)value.separator},{"igPushTextWrapPos",(const void*)value.pushTextWrapPos},
    {"igPopTextWrapPos",(const void*)value.popTextWrapPos},{"igIsWindowHovered",(const void*)value.isWindowHovered},
    {nullptr,nullptr}};
   for(unsigned i=0;entries[i].name;++i){
    if(entries[i].value)continue;
    if(!value.missing.empty())value.missing+=", ";
    value.missing+=entries[i].name;
   }
  }
  return value;
 }
 /* Opaque panel background.  The value is the page colour this build uses for
    its own pages, read out of the executable (see sdk/tc_game_ui.h); the
    board panel must not be see-through or the canvas would show through the
    text. */
 static constexpr SlotApi::Vec4 slotBackground{0.1882f,0.1882f,0.2118f,1.0f};
 static constexpr int kSlotBackgroundColor=1;  /* ImGuiCol_ChildBg */
 /* Draws every registered board panel into the window that is current right
    now and reports whether the panel area owns the mouse for this frame.

    Called from the loader's igIsAnyItemActive proxy at the board's own input
    sampling site: the board window is open, and the game has not yet read
    "is an ImGui item active" for this frame.  Answering true there is what
    makes the game's own busy flag true, which is what keeps the click out of
    the circuit board.  Never throws. */
public:
 bool hasBoardSlots() const {
  for(auto& p:loaded){if(!p->active)continue;for(auto& slot:p->slots)if(slot.kind==TC_UI_SLOT_BOARD_SIDE&&!slot.failed)return true;}
  return false;
 }
 /* Tools registered into the game's own tool column (TC_UI_SLOT_BOARD_TOOLBAR).
    Drawn from the end of that column's child window, so a plugin's control sits
    between the game's own tools and inherits its layout and styling. */
 bool hasBoardTools() const {
  for(auto& p:loaded){if(!p->active)continue;for(auto& slot:p->slots)if(slot.kind==TC_UI_SLOT_BOARD_TOOLBAR&&!slot.failed)return true;}
  return false;
 }
 /* The loader reports the rectangle of each tool button the game draws, so
    plugin tools can line up under the last one. */
 void noteToolColumn(float x,float bottom){
  /* The game's tools are laid out in a grid; learn its pitch from the second
     column (x differs) and from consecutive rows (y differs), so plugin tools
     can continue the same grid instead of guessing 96 px. */
  if(toolColumnKnown){
   const float dx=x-toolColumnX,dy=bottom-toolColumnBottom;
   if(dx>8.f&&dx<400.f)toolColumnPitchX=dx;
   if(dy>8.f&&dy<400.f)toolColumnPitchY=dy;
   /* Keep the left edge and the lowest row: plugin tools start under everything
      the game drew, in the column the game's own tiles start in. */
   if(x<toolColumnX)toolColumnX=x;
   if(bottom>toolColumnBottom)toolColumnBottom=bottom;
   return;
  }
  toolColumnX=x;toolColumnBottom=bottom;toolColumnKnown=true;
 }
 /* The game keeps its fonts in a table and pushes them by index
    (`defined_fonts__presenterZimguiZimgui_u7413` +
    `igPushFont__presenterZimguiZimgui_u7614`, both in this build's symbol
    table).  The tool column draws its icons with the icon font current and
    switches to a text font only inside the scope where it draws a label
    itself: measured around the injection point, igPushFont(2) -> igText ->
    igPopFont inside the tile, and an igPopFont after it that closes the icon
    font scope.  A plugin tool runs between those two, so with the icon font
    current its text has no glyphs at all and shows nothing - which is what
    made every expanded tool an empty rectangle.  Borrowing the text font for
    the plugin draws fixes that without touching the game's own font scopes. */
 void resolveToolTextFont(){
  if(!symbols||toolTextFontIndex>=0)return;
  auto find=[&](const char* name)->void* {
   auto i=symbols->values.find(name?name:"");
   return i==symbols->values.end()?nullptr:i->second;
  };
  void** table=reinterpret_cast<void**>(find("defined_fonts__presenterZimguiZimgui_u7413"));
  pushGameFont=reinterpret_cast<void(*)(unsigned char)>(find("igPushFont__presenterZimguiZimgui_u7614"));
  popGameFont=reinterpret_cast<void(*)()>(proc(engine,"igPopFont"));
  auto name=reinterpret_cast<const char*(*)(const void*)>(proc(engine,"ImFont_GetDebugName"));
  if(!table||!pushGameFont||!popGameFont||!name)return;
  /* Prefer the regular text face (what the board panels and pages draw with),
     and settle for any face that is not the icon font. */
  int fallback=-1;
  for(int index=0;index<8;++index){
   void* font=table[index];
   if(!font)break;
   const char* text=name(font);
   const std::string face=text?text:"";
   if(face.empty()||face.find("Icon")!=std::string::npos)continue;
   if(face.find("Regular")!=std::string::npos){toolTextFontIndex=index;break;}
   if(fallback<0)fallback=index;
  }
  if(toolTextFontIndex<0)toolTextFontIndex=fallback;
  if(toolTextFontIndex<0)logger("Tool column text font: not found; plugin tool text will stay invisible");
  else {
   const char* chosen=name(table[toolTextFontIndex]);
   logger("Tool column text font: defined_fonts["+std::to_string(toolTextFontIndex)+"]="+
          (chosen?chosen:"?")+" (used for plugin tool draws)");
  }
 }
 bool boardToolFrame(){
  if(!hasBoardTools())return false;
  const SlotApi& api=slotApi();
  if(!api.available()){
   static bool reported=false;
   if(!reported){reported=true;logger("Board tool API unavailable: "+api.missing);}
   return false;
  }
  const int frame=api.getFrameCount();
  if(frame==lastToolFrame)return false;
  lastToolFrame=frame;
  /* The tools are drawn *inside the game's own tool column*, one after another,
     so the game's layout puts them in line with its own buttons.  The available
     space comes from the current child (not the display): that child is narrow,
     which is why the panel path's "window is big enough" check must not apply
     here. */
  float width=api.getWindowWidth(),height=api.getWindowHeight();
  if(api.contentAvail){
   SlotApi::Vec2 avail{};
   api.contentAvail(&avail);
   if(avail.x>1.f&&avail.y>1.f){width=avail.x;height=avail.y;}
  }
  if(!(width>1.f&&height>1.f))return false;
  /* Line the tools up under the game's own ones when that column is known, and
     fall back to the cursor the game left behind otherwise. */
  const bool placed=toolColumnKnown&&toolColumnX>0.f&&toolColumnBottom>0.f;
  int toolIndex=0;
  /* The font scale is deliberately left alone.  It used to be forced to the menu
     scale to work around the column's small label scale - with the icon font
     current that only changed the size of text that never had glyphs anyway.  Now
     that the game's text font is pushed (below), the window's own scale is what
     gives plugin text the same size as the game's UI text (measured 45 px in the
     column, the same as the board panels), so plugins that want another size pass
     a font scale to their own window, exactly like the side panels do. */
  /* Borrow the game's text font for the plugin draws: the column leaves its
     icon font current (see resolveToolTextFont), and a font without glyphs
     draws no text at all.  Pushed and popped symmetrically, so the game's own
     font scope - it pops the icon font after this call site - stays balanced. */
  const bool borrowedFont=toolTextFontIndex>=0&&pushGameFont&&popGameFont;
  if(borrowedFont)pushGameFont((unsigned char)toolTextFontIndex);
  TCFrame tick{sizeof(TCFrame),frame,api.getTime()};
  bool drawn=false;
  /* Stable order: by mod id, then by slot id, so two plugins always appear in
     the same sequence no matter which one the loader happened to load first. */
  std::vector<Loaded*> order;
  for(auto& p:loaded)if(p->active)order.push_back(p.get());
  std::sort(order.begin(),order.end(),[](Loaded* a,Loaded* b){return a->id<b->id;});
  for(Loaded* p:order){
   std::vector<BoardSlot*> slots;
   for(auto& slot:p->slots)if(slot.kind==TC_UI_SLOT_BOARD_TOOLBAR&&!slot.failed)slots.push_back(&slot);
   std::sort(slots.begin(),slots.end(),[](BoardSlot* a,BoardSlot* b){return a->id<b->id;});
   for(BoardSlot* slot:slots){
    try{
     /* Continue the game's own grid: two tiles per row, then wrap down. */
     if(placed&&api.setCursorScreen&&api.cursorScreen){
      const int column=toolIndex%2,row=toolIndex/2;
      /* The game leaves the cursor on the next free row already (measured: it
         sits exactly one tile pitch below the last tool), so only the column has
         to be corrected; later tools advance by the learned pitch. */
      SlotApi::Vec2 where{};
      api.cursorScreen(&where);
      api.setCursorScreen(SlotApi::Vec2{toolColumnX+toolColumnPitchX*(float)column,
                                        where.y+toolColumnPitchY*(float)row});
     }
     ++toolIndex;
     /* Each plugin draws in its own ID scope: two mods may use the same widget
        labels without ImGui merging them. */
     if(api.pushId)api.pushId(p->id.c_str());
     if(api.pushId)api.pushId(slot->id.c_str());
     slot->draw(slot->user,&tick,width,height);
     if(api.popId)api.popId();
     if(api.popId)api.popId();
     ++slot->drawn;drawn=true;
     if(slot->drawn==1){
      std::string where;
      if(api.cursorScreen){SlotApi::Vec2 pos{};api.cursorScreen(&pos);where=" at "+std::to_string((int)pos.x)+","+std::to_string((int)pos.y);}
      logger("Board tool "+p->id+"/"+slot->id+" drawn in the game's tool column (frame "+std::to_string(frame)+", child "+std::to_string((int)width)+"x"+std::to_string((int)height)+where+")");
     }
    }catch(const std::exception&e){slot->failed=true;logger("Board tool "+p->id+"/"+slot->id+" failed: "+e.what());}
    catch(...){slot->failed=true;}
   }
  }
  if(drawn){
   static bool layoutLogged=false;
   if(!layoutLogged){
    layoutLogged=true;
    std::string list;
    for(Loaded* p:order)for(auto& slot:p->slots)if(slot.kind==TC_UI_SLOT_BOARD_TOOLBAR&&!slot.failed)list+=(list.empty()?"":", ")+p->id+"/"+slot.id;
    logger("Board tools in the game's tool column (top to bottom): "+list);
   }
  }
  if(borrowedFont)popGameFont();
  return drawn;
 }
 /* Runs one slot's draw callback and applies the failure policy (disable the
    slot, keep the rest of the loader alive).  Shared by the expanded and the
    collapsed path: a folded panel still gets its callback, because plugins
    legitimately keep per-frame state there, and the panel's own clip rect
    keeps everything it submits invisible and unclickable. */
 bool runSlotDraw(Loaded& p,BoardSlot& slot,const TCFrame& tick,float width,float height){
  try{slot.draw(slot.user,&tick,width,height);}
  catch(const std::exception& e){
   slot.draw=nullptr;slot.failed=true;
   logger("Board panel disabled after an exception: "+p.id+"/"+slot.id+": "+e.what());
   return false;
  }catch(...){
   slot.draw=nullptr;slot.failed=true;
   logger("Board panel disabled after an exception: "+p.id+"/"+slot.id);
   return false;
  }
  return true;
 }
 bool boardSlotFrame(){
  if(!hasBoardSlots())return false;
  const SlotApi& api=slotApi();
  if(!api.available()){
   static int reported=0;
   if(reported++==0)logger("Board panel API unavailable: "+api.missing);
   return false;
  }
  const int frame=api.getFrameCount();
  if(frame==lastSlotFrame)return false;
  lastSlotFrame=frame;
  const float windowWidth=api.getWindowWidth(),windowHeight=api.getWindowHeight();
  if(!(windowWidth>320.f&&windowHeight>240.f))return false;
  const float scale=core.ui_scale>0.35f?core.ui_scale:1.f;
  const float margin=kSlotMargin*scale,pad=8.f*scale,header=34.f*scale;
  float y=kSlotTop*scale;
  bool consumed=false;
  TCFrame tick{sizeof(TCFrame),frame,api.getTime()};
  for(auto& p:loaded){
   if(!p->active)continue;
   for(auto& slot:p->slots){
    /* Side panels only: a toolbar tool is drawn by boardToolFrame() from inside
       the game's own tool column instead (it has no host-drawn container). */
    if(slot.failed||slot.kind!=TC_UI_SLOT_BOARD_SIDE)continue;
    float width=slot.width>0.f?slot.width*scale:kSlotDefaultWidth*scale;
    if(width>windowWidth*0.5f)width=windowWidth*0.5f;
    const float collapsedHeight=header+pad*0.5f;
    float height=slot.collapsed?collapsedHeight:(slot.height>0.f?slot.height*scale:kSlotDefaultHeight*scale);
    const float room=windowHeight-y-margin;
    if(height>room)height=room;
    const float minimumHeight=slot.collapsed?header:90.f*scale;
    if(width<120.f*scale||height<minimumHeight){
     if(slot.skipped++==0)logger("Board panel not drawn (no room): "+p->id+"/"+slot.id);
     continue;
    }
    const float x=windowWidth-width-margin;
    api.setCursorPos(SlotApi::Vec2{x,y});
    /* The panel's corner in screen coordinates.  Reading it here rather than
       assuming the board window sits at the origin keeps the hit test correct
       on a window that is offset or scrolled. */
    SlotApi::Vec2 origin{};
    api.cursorScreen(&origin);
    const std::string name=slot.title.empty()?slot.id:slot.title;
    const std::string child=name+"###TCBoardPanel_"+p->id+"_"+slot.id;
    /* The panel background has to be set before BeginChild: the child window
       draws its own background while it is being opened, and the engine's
       default child background is transparent. */
    const bool colored=api.pushStyleColor&&api.popStyleColor;
    if(colored)api.pushStyleColor(kSlotBackgroundColor,slotBackground);
    const bool visible=api.beginChild(child.c_str(),SlotApi::Vec2{width,height},1,0);
    bool hoveredInside=false;
    /* Screen position of the content region, for the log line below. */
    SlotApi::Vec2 contentOrigin{origin.x+pad,origin.y+header};
    /* Header toggle, also for the log (the playtest clicks it). */
    const float toggleSize=kSlotToggleSize*scale;
    SlotApi::Vec2 toggleOrigin{origin.x+width-pad-toggleSize,origin.y+pad};
    const float contentWidth=width>2.f*pad?width-2.f*pad:0.f;
    const float contentHeight=height>header+pad?height-header-pad:0.f;
    if(visible){
     api.pushId(p->id.c_str());
     api.pushId(slot.id.c_str());
     /* Header: title on the left, collapse toggle on the right.  The toggle is
        the host's own item - a plugin cannot fold a panel in a way the player
        cannot undo, and the state belongs to the panel rather than the plugin. */
     api.setCursorPos(SlotApi::Vec2{pad,pad});
     api.text(name.c_str(),nullptr);
     api.setCursorPos(SlotApi::Vec2{width-pad-toggleSize,pad});
     api.cursorScreen(&toggleOrigin);
     if(api.invisibleButton&&api.isItemActive){
      if(api.invisibleButton("###collapse",SlotApi::Vec2{toggleSize,toggleSize},0)){
       slot.collapsed=!slot.collapsed;
       logger(std::string("Board panel ")+p->id+"/"+slot.id+(slot.collapsed?" collapsed":" expanded"));
      }
      /* The glyph inside the toggle: ASCII so it cannot be missing from the
         font, and the same size as the item so the hit box and the label line
         up. */
      api.setCursorPos(SlotApi::Vec2{width-pad-toggleSize+toggleSize*0.35f,pad});
      api.text(slot.collapsed?"+":"-",nullptr);
     }
     if(!slot.collapsed)api.separator();
     const std::string who=p->id+"/"+slot.id;
     bool regionHovered=false;
     if(slot.collapsed){
      /* The only difference while folded: there is no content region and the
         panel is a header tall, so the panel's own clip rect is what removes
         whatever the plugin submits. */
      api.setCursorPos(SlotApi::Vec2{pad,header});
      api.pushTextWrapPos(contentWidth);
      runSlotDraw(*p,slot,tick,contentWidth,contentHeight);
      api.popTextWrapPos();
     }else{
      /* The content lives in its own clipped, scrollable region: see
         drawContentRegion.  The plugin gets the region's interior size to lay
         out in, and content that is scrolled out of view cannot be clicked. */
      api.setCursorPos(SlotApi::Vec2{pad,header});
      const bool ok=drawContentRegion(api,SlotApi::Vec2{contentWidth,contentHeight},scale,
                                      "###content",who,slot.draw,slot.user,tick,regionHovered,
                                      contentOrigin);
      if(!ok){
       slot.draw=nullptr;slot.failed=true;
       logger("Board panel disabled after an exception: "+who);
      }
     }
     hoveredInside=hoveredInside||regionHovered;
     /* Header strip and the padding around the content: still the panel. */
     if(api.isWindowHovered(0))hoveredInside=true;
     api.popId();
     api.popId();
    }
    api.endChild();
    if(colored)api.popStyleColor(1);
    /* Ownership of the mouse.  Two independent readings, both taken from the
       engine rather than from arithmetic: the panel window itself being
       hovered, and the mouse being inside the panel's screen rectangle while
       the board window is the hovered one.  Either means the click belongs to
       the panel. */
    bool inRect=false;
    if(api.mouseScreen){
     SlotApi::Vec2 mouse{};api.mouseScreen(&mouse);
     inRect=mouse.x>=origin.x&&mouse.x<origin.x+width&&mouse.y>=origin.y&&mouse.y<origin.y+height;
    }
    const bool boardHovered=api.isWindowHovered(0);
    const bool ownsMouse=hoveredInside||(inRect&&boardHovered);
    consumed=consumed||ownsMouse;
    ++slot.drawn;
    /* Rectangle lines are bounded, but a change of hover/ownership is logged
       as it happens: "the panel never sees the mouse" is otherwise invisible
       in a log that only shows the first frames. */
    static int lastHovered=-1,lastOwns=-1;
    static float lastWidth=-1.f,lastHeight=-1.f,lastX=-99999.f,lastY=-99999.f;
    static int lastCollapsed=-1;
    const int hovered=hoveredInside?1:0, owns=ownsMouse?1:0;
    /* Geometry changes matter too: a window resize (or a DPI/ratio change) has
       to move the panel, and the playtest asserts that the click still lands
       afterwards - so the log has to show the new rectangle. */
    const bool stateChanged=hovered!=lastHovered||owns!=lastOwns||width!=lastWidth||
                            height!=lastHeight||origin.x!=lastX||origin.y!=lastY||
                            (slot.collapsed?1:0)!=lastCollapsed;
    lastHovered=hovered;lastOwns=owns;
    lastWidth=width;lastHeight=height;lastX=origin.x;lastY=origin.y;
    lastCollapsed=slot.collapsed?1:0;
    if(slot.drawn<=3||slot.drawn%600==0||stateChanged)
     logger("Board panel "+p->id+"/"+slot.id+" frame="+std::to_string(frame)+" x="+
            std::to_string((int)origin.x)+" y="+std::to_string((int)origin.y)+" w="+
            std::to_string((int)width)+" h="+std::to_string((int)height)+" window="+
            std::to_string((int)windowWidth)+"x"+std::to_string((int)windowHeight)+" content="+
            std::to_string((int)contentOrigin.x)+","+std::to_string((int)contentOrigin.y)+" size="+
            std::to_string((int)contentWidth)+"x"+std::to_string((int)contentHeight)+" hovered="+
            std::to_string(hoveredInside?1:0)+" inRect="+std::to_string(inRect?1:0)+" owns="+
            std::to_string(ownsMouse?1:0)+" collapsed="+std::to_string(slot.collapsed?1:0)+
            " toggle="+std::to_string((int)toggleOrigin.x)+","+std::to_string((int)toggleOrigin.y)+
            " toggleSize="+std::to_string((int)toggleSize)+"x"+std::to_string((int)toggleSize));
    y+=height+margin;
   }
  }
  return consumed;
 }
public:
 std::map<std::string,std::string> statuses;
 /* Severity of the message in `statuses` for the same id: 0 info, 1 warning,
    2 error.  Absent means "the loader itself wrote this line" (it is shown in
    the neutral colour).  A plugin's own report also lands here, so the Mods
    page can show it the same way as a loader-detected failure. */
 std::map<std::string,int> statusLevels;
 NativeRuntime(Core& c,HMODULE e,std::function<void(const std::string&)> log):core(c),engine(e),logger(std::move(log)){activeInstance=this;}
 NativeRuntime(const NativeRuntime&)=delete;NativeRuntime& operator=(const NativeRuntime&)=delete;
 /* ---------------------------------------------------------------------
    Native UI page API (used by src/loader.cpp)
    ------------------------------------------------------------------ */
 /* True while the user is inside a registered page.  The loader then draws
    only that page's container instead of the home page's Mods button. */
 bool pageOpen() const {return !openPageId.empty();}
 /* The page the user is inside, or "" when none is resolvable (plugin never
    loaded, unknown page id, owner not active). */
 const std::string& currentPageId() const {return openPageId;}
 /* Requests a page, closing whatever was open.  Returns false and changes
    nothing when the owner has no such page: a plugin that failed to load is
    never reachable, so a rejected plugin leaves no entry behind. */
 bool openPage(const std::string& owner,const std::string& page){UiPage* found=nullptr;if(page.empty()||owner.empty())return false;for(auto& p:loaded){if(!p->active||p->id!=owner)continue;for(auto& candidate:p->pages)if(candidate.id==page)found=&candidate;}if(!found)return false;openPageOwner=owner;openPageId=page;lastUiFrame=-1;return true;}
 void closePage(){openPageId.clear();openPageOwner.clear();lastUiFrame=-1;}
 /* True when the given address is inside one of the board UI functions, i.e.
    when the board scene is the one being built.  Used by the loader's
    igInvisibleButton proxy; costs one vector scan per call and only while the
    function lists are short. */
 bool inBoardUi(uintptr_t rva) const {
  for(auto& range:boardRanges)if(rva>=range.first&&rva<range.second)return true;
  return false;
 }
 struct PageEntry{const char* owner;const char* id;const char* title;bool failed;};
 /* One entry per page of an *active* plugin, for the loader's home page.  The
    pointers stay valid until the native runtime next mutates its plugin list
    (a plugin load, which only happens during boot). */
 std::vector<PageEntry> pages(){
  std::vector<PageEntry> result;
  for(auto& p:loaded){if(!p->active)continue;for(auto& page:p->pages)result.push_back(PageEntry{p->id.c_str(),page.id.c_str(),page.title.c_str(),page.failed});}
  return result;
 }
 /* Builds the address ranges that mark "the board scene is drawing".

    Only addresses are kept, never game object pointers, and the list is built
    from the pinned build's own symbol table, so a different build simply
    yields no ranges (and the loader then never closes a page on its own). */
 void scanBoardUi(){
  if(!symbols)return;
  static const char* names[]={
   "build_bottom_panel__presenterZboard95uiZbottom95panel_u253",
   "build_buttons__presenterZboard95uiZmenu95bar_u187",
   "build_component__presenterZboard95uiZcomponent95menuZflat95list_u119",
   "build_edit_button__presenterZboard95uiZbottom95panelZuser95defined95ui_u2587",
   nullptr};
  std::vector<std::pair<uintptr_t,size_t>> sized;
  for(unsigned i=0;names[i];++i){
   auto found=symbols->values.find(names[i]);
   if(found==symbols->values.end())continue;
   sized.push_back({(uintptr_t)found->second,0});
  }
  if(sized.empty()){logger("Native UI: board scene markers not found; pages will not auto-close on level entry");return;}
  /* The symbol table does not carry sizes, so a range runs to the next known
    function address; board UI entries are large and well separated. */
  std::vector<uintptr_t> starts;
  for(auto& entry:sized)starts.push_back(entry.first);
  std::sort(starts.begin(),starts.end());
  for(auto& entry:sized){
   auto next=std::upper_bound(starts.begin(),starts.end(),entry.first);
   uintptr_t end=next!=starts.end()?*next:entry.first+0x4000;
   boardRanges.push_back({entry.first,end});
  }
  logger("Native UI: board scene markers armed ("+std::to_string(boardRanges.size())+")");
 }
 /* Draws the open page, at most once per engine frame and only while the home
    page is on screen.  Kept separate from frame(): both are driven from the
    loader's menu hook, and a shared frame counter would make one of them skip
    a frame.  Never throws. */
 void uiFrame(bool homeVisible){
  if(!pageOpen())return;
  /* While a page covers the menu the game may stop building the home page, so
     "the home page drew" is not required to keep drawing the page: the loader
     calls this every frame the game shows its menu UI, and closes the page
     itself as soon as a board or a scene change is seen. */
  (void)homeVisible;
  auto engineFrame=(int(*)())GetProcAddress(engine,"igGetFrameCount");
  auto engineTime=(double(*)())GetProcAddress(engine,"igGetTime");
  if(!engineFrame||!engineTime)return;
  const int number=engineFrame();
  textures.advance(number);
  if(number==lastUiFrame)return;
  lastUiFrame=number;
  Loaded* owner=nullptr;UiPage* page=nullptr;
  for(auto& p:loaded){if(!p->active||p->id!=openPageOwner)continue;for(auto& candidate:p->pages)if(candidate.id==openPageId){owner=p.get();page=&candidate;}}
  if(!owner||!page){closePage();return;}
  if(!page->draw){page->failed=true;return;}
  TCFrame frame{sizeof(TCFrame),number,engineTime()};
  {static int calls=0;++calls;if(calls<=3||calls%600==0)logger("UI page frame "+std::to_string(calls)+": "+openPageOwner+"/"+openPageId+" drawn="+std::to_string(page->draw!=nullptr)+" frame="+std::to_string(number));}
  try{drawPageContainer(*owner,*page,frame);}
  catch(const std::exception& e){page->draw=nullptr;page->failed=true;logger("Plugin UI page failed: "+owner->id+"/"+page->id+": "+e.what());statuses[owner->id]="UI 页面异常：已停用该页";}
  catch(...){page->draw=nullptr;page->failed=true;logger("Plugin UI page failed: "+owner->id+"/"+page->id);statuses[owner->id]="UI 页面异常：已停用该页";}
 }
 void boot(){if(started)return;started=true;textures.advance(0);try{
  /* The fault journal must be armed before the first plugin callback runs, so a
     crash inside a plugin callback is attributed instead of anonymous. */
  fault::install((core.dir/L"fault.log").c_str());
  if(GetAsyncKeyState(VK_SHIFT)&0x8000){logger("Native safe mode: Shift held; plugins skipped");for(auto& id:core.enabled_set())statuses[id]="安全模式：本次未加载";return;}
  symbols=std::make_unique<Symbols>(core.root/L"Turing Complete.exe");auto mh=MH_Initialize();if(mh!=MH_OK&&mh!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("MinHook initialization failed");
  logger("TC Mod Loader "+std::string(TC_MODLOADER_VERSION_STRING)+" capabilities: "+capability_names(loader_capabilities()));
  buildChains();
  /* Board handles and the SCENE_CHANGE event both hang off scene.change, so its
     detour is taken before any plugin gets a chance to hook it (see
     armSceneChangeTracking). */
  armSceneChangeTracking();
  scanBoardUi();
  resolveToolTextFont();
  timing::install(symbols->values,ownedHooks,logger);
  if(hash(read(core.root/L"compile.dll"))=="95a1de0c1cda0844946435a846e64b45a18af5575221c3dab169074b91e60aec") {
   auto compilerModule=GetModuleHandleW(L"compile.dll");
   if(compilerModule){Symbols compilerSymbols(core.root/L"compile.dll",compilerModule);
    if(!logic::install(symbols->values,compilerSymbols.values,ownedHooks,logger))logger("Native logic: compiler bridge unavailable");}
  }else logger("Native logic: unsupported compile.dll; bridge disabled");
  std::map<std::string,Mod*> mods;for(auto& m:core.mods)if(m.error.empty())mods[m.id]=&m;
  std::set<std::string> visiting,done,failed;std::function<void(const std::string&)> load=[&](const std::string& id){if(done.count(id))return;if(!visiting.insert(id).second){failed.insert(id);statuses[id]="依赖循环";return;}if(!mods.count(id)){failed.insert(id);statuses[id]="Mod 缺失或无效";done.insert(id);return;}auto& m=*mods.at(id);
   for(auto& dep:m.dependencies){if(!core.enabled(dep)){failed.insert(dep);}else load(dep);if(failed.count(dep))failed.insert(id);}
   if(failed.count(id)){statuses[id]="依赖未成功加载";done.insert(id);return;}
   if(!m.entry.empty()) {auto p=std::make_unique<Loaded>();p->owner=this;p->id=id;
    try {if(!core.state.contains("native")||core.state["native"].value(id,"")!=m.digest)throw std::runtime_error("包内容已改变，请在 Mods 页面重新应用后重启");
     auto cache=core.dir/L"plugins"/fs::u8path(id)/m.digest;no_links(core.root,cache);fs::create_directories(cache);for(auto& [rel,bytes]:m.native){auto out=cache/fs::u8path(rel);no_links(core.root,out);auto tmp=out;tmp+=L".tc-tmp";no_links(core.root,tmp);if(!fs::exists(out)||hash(read(out))!=hash(bytes))atomic(out,bytes);}
      auto data=core.dir/L"plugin-data"/fs::u8path(id);no_links(core.root,data);fs::create_directories(data);p->folder=data.u8string();p->packageRoot=cache;
      /* The capability mask below is the promise the manifest's "capabilities"
         list is checked against while the package is scanned, so both sides
         read the same table (src/capabilities.hpp). */
p->host={sizeof(TCHost),TC_MOD_API_VERSION,p.get(),TC_GAME_BUILD_LABEL,p->id.c_str(),p->folder.c_str(),log_api,resolve,engine_api,hook_api,register_logic_api,register_component_api,register_ui_page_api,create_ui_texture_api,load_ui_texture_api,release_ui_texture_api,register_ui_slot_api,TC_MODLOADER_VERSION_CODE,0u,loader_capabilities(),report_status_api,resolve_alias_api,register_hook_chain_api,add_event_listener_api,get_current_game_handle_api,validate_game_handle_api,resolve_game_handle_api};p->plugin.size=sizeof(TCPlugin);
     p->dll=LoadLibraryExW((cache/fs::u8path(m.entry)).c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);if(!p->dll)throw std::runtime_error("LoadLibrary failed: "+std::to_string(GetLastError()));auto entry=(TCModLoad)GetProcAddress(p->dll,"tc_mod_load");if(!entry)throw std::runtime_error("Missing tc_mod_load export");timing::Registration registration;if(entry(&p->host,&p->plugin)!=0||p->plugin.size!=sizeof(TCPlugin))throw std::runtime_error("Plugin rejected API / initialization failed");
     p->accepting=false;for(auto h:p->hooks)if(MH_EnableHook(h)!=MH_OK)throw std::runtime_error("Cannot enable hook");timing::commit(registration);logic::finish(p->logicIds,true);p->active=true;if(!p->reported)statuses[id]="运行中（原生代码）";
     logger("Native loaded: "+id+"; hooks="+std::to_string(p->hooks.size())+(m.capabilities.empty()?std::string():"; declared capabilities: "+[&]{std::string list;for(auto& name:m.capabilities){if(!list.empty())list+=", ";list+=name;}return list;}()));
    }catch(const std::exception&e){
     /* A plugin that explained itself through report_status keeps that line: it
        is more specific than "Plugin rejected API / initialization failed", and
        the player is looking at the Mods page, not at the log. */
     if(!p->reported)statuses[id]=e.what();
     failed.insert(id);logger("Native failed: "+id+": "+e.what());reject(*p);
    }loaded.push_back(std::move(p));
   }
   visiting.erase(id);done.insert(id);
  };for(auto& id:core.enabled_set())load(id);
  /* Event sources are armed before the chains are installed: a loader link is
     just another chain link, and this way the detour is created once, with the
     complete list. */
  armEventSources();
  /* Chains are installed once every plugin has had its chance to join, so a
     plugin that fails later cannot leave a half-built chain behind. */
  installChains();
 }catch(const std::exception&e){logger(std::string("Native runtime unavailable: ")+e.what());}}
 void frame(){if(inside)return;inside=true;boot();auto getFrame=(int(*)())GetProcAddress(engine,"igGetFrameCount");auto getTime=(double(*)())GetProcAddress(engine,"igGetTime");int n=getFrame();lastEngineFrame=n;textures.advance(n);if(n!=lastFrame){lastFrame=n;TCFrame f{sizeof(TCFrame),n,getTime()};for(auto& p:loaded)if(p->active&&p->plugin.on_frame){
   /* A faulting per-frame callback is dropped for the rest of the process: the
      game keeps running, the Mods page says what happened. */
   fault::Scope mark(p->id.c_str(),"on_frame");
   try{p->plugin.on_frame(p->plugin.user,&f);}catch(...){logger("Plugin callback threw: "+p->id);statuses[p->id]="回调异常：请停用后重启";statusLevels[p->id]=2;p->plugin.on_frame=nullptr;}
  }}inside=false;}
};
}
