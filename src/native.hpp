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
#include "services.hpp"
#include "expression.hpp"
#include "board_objects.hpp"
#include "save_deps.hpp"
#include "board_pins.hpp"
#include "simulation_read.hpp"
#include "sim_control.hpp"
#include "board_edits.hpp"
#include "board_connect.hpp"
#include "component_geometry.hpp"
#include "picture_path.hpp"
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
 struct BoardSlot {std::string id,title;uint32_t kind=TC_UI_SLOT_BOARD_SIDE;
  void (*draw)(void*,const TCFrame*,float,float)=nullptr;
  /* TC_UI_SLOT_BOARD_COMPONENT_PANEL only (see sdk/tc_mod_api.h). */
  void (*drawComponent)(void*,const TCFrame*,uint64_t,float,float)=nullptr;
  void* user=nullptr;bool failed=false,collapsed=false;float width=0.f,height=0.f;int drawn=0,skipped=0;};
 /* `reported` is set when a plugin called report_status during tc_mod_load: the
    loader then keeps that message instead of overwriting it with "running" once
    the plugin finishes loading, which is what the author asked to see. */
 struct ComponentRenderer {uint64_t customId=0;TCComponentRenderCallbackV1 draw=nullptr;void* user=nullptr;bool failed=false;};
struct Loaded {NativeRuntime* owner;std::string id,folder,status;fs::path packageRoot;TCHost host{};TCPlugin plugin{};HMODULE dll{};std::vector<void*> hooks;std::vector<uint64_t> logicIds,componentIds;std::vector<UiPage> pages;std::vector<BoardSlot> slots;std::vector<ComponentRenderer> renderers;std::set<uint64_t> hiddenDefaultDrawings;std::set<uint64_t> hiddenSelectionHints;std::set<uint64_t> hiddenFoundryButtons;std::set<uint64_t> placementPreviews;
 /* V5: the PNG a type is shown with in the game's own component column, its
    drawer preview and the placement ghost (custom id -> absolute path). */
 std::map<uint64_t,std::string> pictures;bool accepting=true,active=false,reported=false;};
 struct InstanceFootprint {Loaded* owner=nullptr;uint64_t customId=0;float halfWidth=0.f,halfHeight=0.f;};
 ui_texture::Manager textures;
 Core& core;HMODULE engine;fs::path saveRoot;std::string currentLevelName;
 /* id → Mod/type cache the save page reads (src/save_deps.hpp): the only place
    the mapping exists, because a circuit file stores just the custom_id.  It
    lives with the loader's other data, not in any save. */
 TypeRegistry types;
 std::unique_ptr<Symbols> symbols;std::vector<std::unique_ptr<Loaded>> loaded;std::set<void*> ownedHooks;std::map<void*,std::string> loaderOwned;std::function<void(const std::string&)> logger;int lastFrame=-1,lastUiFrame=-1,lastSlotFrame=-1,lastToolFrame=-1;int64_t lastEngineFrame=-1;DWORD gameThreadId=0;bool inside=false,started=false;
 std::map<uint64_t,InstanceFootprint> instanceFootprints;
int lastMenuFrame=-1;
using BoardSetLen=uint64_t(*)(const void*);
 const void* selectedComponents=nullptr;
 const void* selectedWires=nullptr;
 const void* previousSelectedComponents=nullptr;
 const void* previousSelectedWires=nullptr;
 BoardSetLen boardSetLen=nullptr;
 /* Prototype access for the V5 pin read.  The built-in kind set is collected
    once at boot: an unknown key handed to the game's get_prototype() raises a
    Nim error a plugin cannot recover from, so it is never passed. */
 using PrototypeGet=void(*)(const void*,void*);
 using CustomPrototypeGet=void(*)(uint64_t,void*);
 using CustomPrototypeSet=void(*)(uint64_t,const void*);
 using PrototypeDestroy=void(*)(void*);
 using SequenceDestroy=void(*)(void*);
 using SequenceNew=void(*)(void*,int64_t);
 PrototypeGet prototypeGet=nullptr;
 CustomPrototypeGet customPrototypeGet=nullptr;
 CustomPrototypeSet customPrototypeSet=nullptr;
 PrototypeDestroy prototypeDestroy=nullptr;
 SequenceDestroy sequenceDestroy=nullptr;
 SequenceNew sequenceNew=nullptr;
 /* Board overlay rendering.  The host owns the draw list and forwards a small,
    checked primitive table; plugins never receive the renderer-private list. */
 struct RenderVec2 {float x,y;};
 using WorldToScreen=RenderVec2(*)(RenderVec2);
 using GetIo=void*(*)();
 using GetMainViewport=void*(*)();
 using GetBackgroundDrawList=void*(*)(void*);
 using DrawLine=void(*)(void*,RenderVec2,RenderVec2,uint32_t,float);
 using DrawRect=void(*)(void*,RenderVec2,RenderVec2,uint32_t,float,int,float);
 using DrawRectFilled=void(*)(void*,RenderVec2,RenderVec2,uint32_t,float,int);
 using DrawCircle=void(*)(void*,RenderVec2,float,uint32_t,int,float);
 using DrawCircleFilled=void(*)(void*,RenderVec2,float,uint32_t,int);
 using DrawText=void(*)(void*,RenderVec2,uint32_t,const char*,const char*);
 using PushClipRect=void(*)(void*,RenderVec2,RenderVec2,bool);
 using PopClipRect=void(*)(void*);
 WorldToScreen renderWorldToScreen=nullptr;
 GetIo renderGetIo=nullptr;
 GetMainViewport renderGetMainViewport=nullptr;
 GetBackgroundDrawList renderGetBackgroundDrawList=nullptr;
 DrawLine renderLine=nullptr;DrawRect renderRect=nullptr;DrawRectFilled renderRectFilled=nullptr;
 DrawCircle renderCircle=nullptr;DrawCircleFilled renderCircleFilled=nullptr;DrawText renderText=nullptr;
 PushClipRect renderPushClip=nullptr;PopClipRect renderPopClip=nullptr;
 /* ImFont_CalcTextSizeA: the game measures its own labels with it, and the draw
    table's measure_text answers with the same numbers (see renderTextSized). */
 void (*renderCalcTextSize)(void* out,const void* font,float size,float maxWidth,
                            float wrapWidth,const char* begin,const char* end,
                            const char** remaining)=nullptr;
 /* The game's bold face, borrowed for a Mod that wants a stock part's label
    weight (TCComponentRenderDrawV2::text_sized's `bold`).  Resolved with the
    tool text font; null means "use whichever face is current". */
 void* boldTextFont=nullptr;
 struct RenderDrawContext {NativeRuntime* owner=nullptr;void* list=nullptr;uint32_t commands=0;bool active=false;};
 /* The game's board redraw receives the complete component record as its sixth
    argument.  V2 suppresses that call only for an active Mod-owned custom id;
    the model, footprint and interaction structures are never changed. */
 using RedrawComponent=void(*)(void*,void*,void*,void*,uint64_t,const void*,const void*,uint64_t);
 RedrawComponent redrawComponentOriginal=nullptr;
 void* redrawComponentTarget=nullptr;
 using RedrawClipboardComponent=void(*)(void*,void*,void*,const void*);
 RedrawClipboardComponent redrawClipboardComponentOriginal=nullptr;
 void* redrawClipboardComponentTarget=nullptr;
 using HideClipboard=void(*)(void*);
 HideClipboard hideClipboardOriginal=nullptr;
 void* hideClipboardTarget=nullptr;
  using UpdateStateClipboard=void(*)(void*,void*,void*,void*,void*);
  UpdateStateClipboard updateStateClipboardOriginal=nullptr;
  void* updateStateClipboardTarget=nullptr;
 std::set<uint64_t> defaultDrawingSuppressedInstances;
 struct PlacementPreview {Loaded* owner=nullptr;const void* clipboard=nullptr;
  uint64_t customId=0;int16_t x=0,y=0;uint8_t rotation=0;bool active=false;};
 PlacementPreview placementPreview;
 /* One "taken over" and one "ended" line per placement, each capped: a long
    session must not grow the log without bound, and the first few are what a bug
    report needs.  The pair is also how a case reads the placement lifecycle. */
 int placementPreviewStartedCount=0;
 int placementPreviewEndedCount=0;
 const void* prototypeTable=nullptr;
 std::set<uint8_t> builtinKinds;
 /* Simulation reads.  The state buffer global is only used to answer "is there
    a buffer"; the values themselves come from the game's own reader. */
 unsigned char** simulationState=nullptr;
 /* The component-menu add helper takes the Board model itself, which is what a
    Board handle resolves to - that is why a placement can be a queued command
    without any presenter context. */
 using AddComponent=bool(*)(void*,void*);
 AddComponent addComponent=nullptr;
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
 static bool engine_hook_target(NativeRuntime& runtime,void* target){
  MEMORY_BASIC_INFORMATION memory{};
  if(!runtime.engine||!target||!VirtualQuery(target,&memory,sizeof(memory))||
     memory.AllocationBase!=runtime.engine||memory.State!=MEM_COMMIT)return false;
  const DWORD protection=memory.Protect&0xffu;
  return protection==PAGE_EXECUTE||protection==PAGE_EXECUTE_READ||
         protection==PAGE_EXECUTE_READWRITE||protection==PAGE_EXECUTE_WRITECOPY;
 }
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
 /* Game functions come from the symbol profile; cimgui/renderer entry points
    come from engine_proc.  The latter are accepted only when the pointer is in
    an executable page owned by the original engine module, so an arbitrary
    data pointer cannot be smuggled into MinHook. */
 const bool knownFunction=target&&r.symbols->functions.count(target);
 const bool engineFunction=target&&engine_hook_target(r,target);
 if(!p.accepting||!target||!detour||!original||(!knownFunction&&!engineFunction)){log_api(c,"Hook rejected: phase, target or conflict");return -1;}
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
 void bindBoardSnapshotSources(){
  auto address=[this](const char* name)->void*{auto found=symbols->values.find(name);return found==symbols->values.end()?nullptr:found->second;};
  selectedComponents=address("selected_components__modelZboardZboard_u22");
  selectedWires=address("selected_wires__modelZboardZboard_u30");
  previousSelectedComponents=address("prev_selected_components__modelZboardZboard_u41");
  previousSelectedWires=address("prev_selected_wires__modelZboardZboard_u44");
  boardSetLen=reinterpret_cast<BoardSetLen>(address("len__modelZboardZboard_u19087"));
 logger(std::string("Board snapshots: selection counters ")+
         (boardSetLen&&selectedComponents&&selectedWires?"armed":"unavailable"));
 }
 void bindBoardPinSources(){
  auto address=[this](const char* name)->void*{auto found=symbols->values.find(name);return found==symbols->values.end()?nullptr:found->second;};
  prototypeGet=reinterpret_cast<PrototypeGet>(address("get_prototype__modelZboardZcustom95prototype95list_u502"));
  customPrototypeGet=reinterpret_cast<CustomPrototypeGet>(address("get_custom_prototype__modelZboardZcustom95prototype95list_u451"));
  customPrototypeSet=reinterpret_cast<CustomPrototypeSet>(address("custom_prototypes_set__modelZboardZcustom95prototype95list_u192"));
  prototypeDestroy=reinterpret_cast<PrototypeDestroy>(address("eqdestroy___modelZboardZprototype95list_u3259"));
  sequenceDestroy=reinterpret_cast<SequenceDestroy>(address("eqdestroy___modelZsave95mongerZcommon_u4418"));
  sequenceNew=reinterpret_cast<SequenceNew>(address("newSeq__modelZboardZboard_u5624"));
  prototypeTable=address("PROTOTYPES__modelZboardZprototype95list_u3772");
  board_pins::collectBuiltinKinds(prototypeTable,builtinKinds);
  logger(std::string("Board pins: prototype reads ")+
         (prototypeGet&&customPrototypeGet&&prototypeDestroy?"armed":"unavailable")+
         ", "+std::to_string(builtinKinds.size())+" built-in kinds");
  logger(std::string("Component geometry: ")+
         (customPrototypeGet&&customPrototypeSet&&prototypeDestroy&&sequenceDestroy&&sequenceNew?
          "armed":"unavailable"));
  renderWorldToScreen=reinterpret_cast<WorldToScreen>(aliases.count("board.world_to_screen")?aliases["board.world_to_screen"]:nullptr);
  renderGetIo=reinterpret_cast<GetIo>(GetProcAddress(engine,"igGetIO"));
  renderGetMainViewport=reinterpret_cast<GetMainViewport>(GetProcAddress(engine,"igGetMainViewport"));
  renderGetBackgroundDrawList=reinterpret_cast<GetBackgroundDrawList>(GetProcAddress(engine,"igGetBackgroundDrawList"));
  renderLine=reinterpret_cast<DrawLine>(GetProcAddress(engine,"ImDrawList_AddLine"));
  renderRect=reinterpret_cast<DrawRect>(GetProcAddress(engine,"ImDrawList_AddRect"));
  renderRectFilled=reinterpret_cast<DrawRectFilled>(GetProcAddress(engine,"ImDrawList_AddRectFilled"));
  renderCircle=reinterpret_cast<DrawCircle>(GetProcAddress(engine,"ImDrawList_AddCircle"));
  renderCircleFilled=reinterpret_cast<DrawCircleFilled>(GetProcAddress(engine,"ImDrawList_AddCircleFilled"));
  renderText=reinterpret_cast<DrawText>(GetProcAddress(engine,"ImDrawList_AddText_Vec2"));
  renderCalcTextSize=reinterpret_cast<decltype(renderCalcTextSize)>(GetProcAddress(engine,"ImFont_CalcTextSizeA"));
  renderPushClip=reinterpret_cast<PushClipRect>(GetProcAddress(engine,"ImDrawList_PushClipRect"));
  renderPopClip=reinterpret_cast<PopClipRect>(GetProcAddress(engine,"ImDrawList_PopClipRect"));
  logger(std::string("Component render: overlay primitives ")+
         (renderWorldToScreen&&renderGetIo&&renderGetMainViewport&&renderGetBackgroundDrawList&&
          renderLine&&renderRect&&renderRectFilled&&renderCircle&&renderCircleFilled&&renderText&&
          renderPushClip&&renderPopClip?"armed":"unavailable"));
  simulationState=reinterpret_cast<unsigned char**>(address("simulation_state__modelZsimulator95types_u81"));
  logger(std::string("Simulation reads: ")+
         (aliases.count("sim.state.read")&&aliases.count("sim.cycle")?"armed":"unavailable")+
         ", state buffer "+std::to_string(tc::sim::kStateBufferSize)+" bytes");
  addComponent=reinterpret_cast<AddComponent>(address("add_component__presenterZutilitiesZhelper95functions_u5918"));
  logger(std::string("Board edits: component placement ")+(addComponent?"armed":"unavailable")+
         "; wire placement stays out of the command bus (add_wire_from_pos only starts a wire)");
 }
 static int captureBoardSnapshotService(void* context,const TCGameHandle* handle,TCBoardSnapshotV1* out,uint32_t outSize){
  return static_cast<NativeRuntime*>(context)->captureBoardSnapshot(handle,out,outSize);
 }
 static int captureBoardObjectsService(void* context,const TCGameHandle* handle,TCBoardObjectSnapshotV1* out,uint32_t outSize,const TCBoardObjectBuffersV1* buffers){
  return static_cast<NativeRuntime*>(context)->captureBoardObjects(handle,out,outSize,buffers);
 }
 int captureBoardSnapshot(const TCGameHandle* handle,TCBoardSnapshotV1* out,uint32_t outSize){
  if(!handle||!out)return TC_SNAPSHOT_ERR_ARGUMENT;
  if(outSize<sizeof(TCBoardSnapshotV1))return TC_SNAPSHOT_ERR_SIZE;
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return TC_SNAPSHOT_ERR_THREAD;
  const void* raw=nullptr;
  if(gameHandles.resolve(handle,&raw)!=TC_HANDLE_OK)return TC_SNAPSHOT_ERR_STALE;
  const int64_t frame=engineFrame();
  TCBoardSnapshotV1 snapshot{};
  snapshot.size=sizeof(snapshot);snapshot.version=TC_BOARD_SNAPSHOT_VERSION_1;snapshot.board=*handle;
  snapshot.engine_frame=frame;
  if(frame>=0)snapshot.flags|=TC_BOARD_SNAPSHOT_HAS_ENGINE_FRAME;
  auto cycle=aliases.find("sim.cycle");
  if(cycle!=aliases.end()&&cycle->second){snapshot.simulation_cycle=cycleNow();snapshot.flags|=TC_BOARD_SNAPSHOT_HAS_SIMULATION_CYCLE;}
  if(boardSetLen&&selectedComponents&&selectedWires){
   snapshot.selected_component_count=boardSetLen(selectedComponents);
   snapshot.selected_wire_count=boardSetLen(selectedWires);
   snapshot.flags|=TC_BOARD_SNAPSHOT_HAS_SELECTION;
   if(previousSelectedComponents&&previousSelectedWires){
    snapshot.previous_selected_component_count=boardSetLen(previousSelectedComponents);
    snapshot.previous_selected_wire_count=boardSetLen(previousSelectedWires);
    snapshot.flags|=TC_BOARD_SNAPSHOT_HAS_PREVIOUS_SELECTION;
   }
  }
  /* A snapshot is published only if its Board identity and frame survived the
     complete read.  Main-thread ownership prevents game mutation during this
     synchronous call; the second check catches re-entrant scene changes. */
  if(!gameHandles.valid(handle)||frame!=engineFrame())return TC_SNAPSHOT_ERR_RETRY;
  *out=snapshot;
  return TC_SNAPSHOT_OK;
 }
 int captureBoardObjects(const TCGameHandle* handle,TCBoardObjectSnapshotV1* out,uint32_t outSize,const TCBoardObjectBuffersV1* buffers){
  if(!handle||!out||!buffers)return TC_SNAPSHOT_ERR_ARGUMENT;
  if(outSize<sizeof(TCBoardObjectSnapshotV1)||buffers->size<sizeof(TCBoardObjectBuffersV1))return TC_SNAPSHOT_ERR_SIZE;
  if(buffers->version!=TC_BOARD_OBJECT_SNAPSHOT_VERSION_1)return TC_SNAPSHOT_ERR_ARGUMENT;
  if((buffers->component_capacity&&!buffers->components)||(buffers->wire_capacity&&!buffers->wires))return TC_SNAPSHOT_ERR_ARGUMENT;
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return TC_SNAPSHOT_ERR_THREAD;
  const void* raw=nullptr;
  if(gameHandles.resolve(handle,&raw)!=TC_HANDLE_OK)return TC_SNAPSHOT_ERR_STALE;
  const int64_t frame=engineFrame();
  const auto* boardBytes=static_cast<const unsigned char*>(raw);
  uint64_t componentCount=0,wireCount=0;const unsigned char* componentData=nullptr;const unsigned char* wireData=nullptr;
  std::memcpy(&componentCount,boardBytes+0x78,sizeof(componentCount));
  std::memcpy(&componentData,boardBytes+0x80,sizeof(componentData));
  std::memcpy(&wireCount,boardBytes+0x98,sizeof(wireCount));
  std::memcpy(&wireData,boardBytes+0xa0,sizeof(wireData));
  if(componentCount>1000000||wireCount>4000000||(componentCount&&!componentData)||(wireCount&&!wireData))return TC_SNAPSHOT_ERR_UNAVAILABLE;
  TCBoardObjectSnapshotV1 snapshot{};
  snapshot.size=sizeof(snapshot);snapshot.version=TC_BOARD_OBJECT_SNAPSHOT_VERSION_1;snapshot.board=*handle;
  snapshot.engine_frame=frame;snapshot.component_count=componentCount;snapshot.wire_count=wireCount;
  snapshot.flags=TC_BOARD_OBJECT_SNAPSHOT_HAS_COMPONENTS|TC_BOARD_OBJECT_SNAPSHOT_HAS_WIRES;
  uint64_t childGeneration=0;
  if(gameHandles.beginChildSnapshot(handle,frame,&childGeneration)!=TC_HANDLE_OK)return TC_SNAPSHOT_ERR_RETRY;
  snapshot.snapshot_generation=childGeneration;
  if(buffers->component_capacity<componentCount||buffers->wire_capacity<wireCount){
   if(!gameHandles.valid(handle)||frame!=engineFrame())return TC_SNAPSHOT_ERR_RETRY;
   *out=snapshot;return TC_SNAPSHOT_ERR_CAPACITY;
  }
  for(uint64_t i=0;i<componentCount;++i){
   const void* value=componentData+8+i*0x238;
   if(gameHandles.issueChild(TC_GAME_OBJECT_COMPONENT,value,childGeneration,&buffers->components[i])!=TC_HANDLE_OK)return TC_SNAPSHOT_ERR_RETRY;
   ++snapshot.component_written;
  }
  for(uint64_t i=0;i<wireCount;++i){
   const void* value=wireData+8+i*0x68;
   if(gameHandles.issueChild(TC_GAME_OBJECT_WIRE,value,childGeneration,&buffers->wires[i])!=TC_HANDLE_OK)return TC_SNAPSHOT_ERR_RETRY;
   ++snapshot.wire_written;
  }
  if(!gameHandles.valid(handle)||frame!=engineFrame())return TC_SNAPSHOT_ERR_RETRY;
  *out=snapshot;return TC_SNAPSHOT_OK;
 }
 static int readComponentService(void* context,const TCGameHandle* handle,TCComponentInfoV1* out,uint32_t outSize){
  return static_cast<NativeRuntime*>(context)->readBoardComponent(handle,out,outSize);
 }
 static int readWireService(void* context,const TCGameHandle* handle,TCWireInfoV1* out,uint32_t outSize){
  return static_cast<NativeRuntime*>(context)->readBoardWire(handle,out,outSize);
 }
 bool currentBoardRaw(const void** out) const {
  TCGameHandle board{};
  if(gameHandles.current(TC_GAME_OBJECT_BOARD,&board)!=TC_HANDLE_OK)return false;
  return gameHandles.resolve(&board,out)==TC_HANDLE_OK;
 }
 /* Resolves a V3 object handle to the record it names.  The handle must still be
    live, the Board must still be the one that issued it, and the record must sit
    exactly on an element of the Board's *current* sequence: an object table that
    was replaced by an edit after the enumeration is reported as STALE instead of
    being read as freed or reused memory. */
 int resolveBoardObject(const TCGameHandle* handle,uint32_t expectedKind,board_objects::Arrays& arrays,const void** record,int64_t& frame){
  if(!handle||!record)return TC_SNAPSHOT_ERR_ARGUMENT;
  if(handle->kind!=expectedKind)return TC_SNAPSHOT_ERR_ARGUMENT;
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return TC_SNAPSHOT_ERR_THREAD;
  frame=engineFrame();
  const void* raw=nullptr;
  if(gameHandles.resolve(handle,&raw)!=TC_HANDLE_OK)return TC_SNAPSHOT_ERR_STALE;
  const void* boardRaw=nullptr;
  if(!currentBoardRaw(&boardRaw))return TC_SNAPSHOT_ERR_STALE;
  if(!board_objects::readArrays(boardRaw,arrays))return TC_SNAPSHOT_ERR_UNAVAILABLE;
  const bool inside=expectedKind==TC_GAME_OBJECT_COMPONENT?board_objects::containsComponent(arrays,raw):board_objects::containsWire(arrays,raw);
  if(!inside)return TC_SNAPSHOT_ERR_STALE;
  *record=raw;return TC_SNAPSHOT_OK;
 }
 int readBoardComponent(const TCGameHandle* handle,TCComponentInfoV1* out,uint32_t outSize){
  if(!out)return TC_SNAPSHOT_ERR_ARGUMENT;
  if(outSize<sizeof(TCComponentInfoV1))return TC_SNAPSHOT_ERR_SIZE;
  board_objects::Arrays arrays{};const void* record=nullptr;int64_t frame=-1;
  const int status=resolveBoardObject(handle,TC_GAME_OBJECT_COMPONENT,arrays,&record,frame);
  if(status!=TC_SNAPSHOT_OK)return status;
  board_objects::decodeComponent(record,*handle,out);
  /* Same rule as the snapshot calls: data is published only if the Board and the
     engine frame survived the whole read. */
  if(gameHandles.valid(handle)!=1||frame!=engineFrame())return TC_SNAPSHOT_ERR_RETRY;
  return TC_SNAPSHOT_OK;
 }
 int readBoardWire(const TCGameHandle* handle,TCWireInfoV1* out,uint32_t outSize){
  if(!out)return TC_SNAPSHOT_ERR_ARGUMENT;
  if(outSize<sizeof(TCWireInfoV1))return TC_SNAPSHOT_ERR_SIZE;
  board_objects::Arrays arrays{};const void* record=nullptr;int64_t frame=-1;
  const int status=resolveBoardObject(handle,TC_GAME_OBJECT_WIRE,arrays,&record,frame);
  if(status!=TC_SNAPSHOT_OK)return status;
  board_objects::decodeWire(record,board_objects::wireIndex(arrays,record),*handle,out);
  if(gameHandles.valid(handle)!=1||frame!=engineFrame())return TC_SNAPSHOT_ERR_RETRY;
  return TC_SNAPSHOT_OK;
 }
 static int readPinsService(void* context,const TCGameHandle* handle,TCComponentPinsV1* out,uint32_t outSize,const TCComponentPinBuffersV1* buffers){
  return static_cast<NativeRuntime*>(context)->readBoardComponentPins(handle,out,outSize,buffers);
 }
 /* tc.component.types: register a V2 definition.  The table is handed out per
    plugin (like the command bus), because a registration needs that plugin's
    own host - its mod id and its data directory - exactly like the V1 entry. */
 static int component_types_register_api(void* c,const TCComponentTypeDefinitionV2* definition){
  auto& p=*(Loaded*)c;if(!p.accepting)return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
  try {
   const int result=tc::registerNativeComponentV2(&p.host,definition);
    if(!result){p.logicIds.push_back(definition->custom_id);p.componentIds.push_back(definition->custom_id);
     p.owner->noteComponentType(p,definition->custom_id,definition->name,
                                (int)definition->config_schema,definition->type_id);}
   else log_api(c,("Component V2 registration rejected: "+std::to_string(result)).c_str());
   return result;
  } catch(const std::exception& e) {log_api(c,e.what());return TC_COMPONENT_TYPES_ERR_GAME;}
   catch(...) {return TC_COMPONENT_TYPES_ERR_GAME;}
 }
 static int component_geometry_set_api(void* c,uint64_t customId,float halfWidth,float halfHeight){
  auto& p=*static_cast<Loaded*>(c);
  return p.owner->setComponentFootprint(p,customId,halfWidth,halfHeight);
 }
 static int component_geometry_read_api(void* c,const TCGameHandle* component,float* halfWidth,float* halfHeight){
  auto& p=*static_cast<Loaded*>(c);
  return p.owner->readComponentFootprint(p,component,halfWidth,halfHeight);
 }
 static int component_geometry_set_instance_api(void* c,const TCGameHandle* component,
                                                float halfWidth,float halfHeight){
  auto& p=*static_cast<Loaded*>(c);
  return p.owner->setComponentInstanceFootprint(p,component,halfWidth,halfHeight);
 }
 static int component_geometry_set_cells_api(void* c,uint64_t customId,int32_t x,int32_t y,
                                             uint32_t width,uint32_t height){
  auto& p=*static_cast<Loaded*>(c);
  return p.owner->setComponentFootprintCells(p,customId,x,y,width,height);
 }
 static int readWireEndsService(void* context,const TCGameHandle* handle,TCWireEndsV1* out,uint32_t outSize){
  return static_cast<NativeRuntime*>(context)->readBoardWireEnds(handle,out,outSize);
 }
 /* Which pin sits on each end of a wire.  The rule is the one the player sees:
     a pin's grid position is the component's position plus the pin's relative
     offset, and a wire end landing there is that connection.  Components far from
     both ends are skipped, so the walk stays local instead of cloning a prototype
     for every component on the board. */
 int readBoardWireEnds(const TCGameHandle* handle,TCWireEndsV1* out,uint32_t outSize){
  if(!handle||!out)return TC_SNAPSHOT_ERR_ARGUMENT;
  if(outSize<sizeof(TCWireEndsV1))return TC_SNAPSHOT_ERR_SIZE;
  if(handle->kind!=TC_GAME_OBJECT_WIRE)return TC_SNAPSHOT_ERR_ARGUMENT;
  board_objects::Arrays arrays{};const void* record=nullptr;int64_t frame=-1;
  const int status=resolveBoardObject(handle,TC_GAME_OBJECT_WIRE,arrays,&record,frame);
  if(status!=TC_SNAPSHOT_OK)return status;
  TCWireInfoV1 wire{};
  board_objects::decodeWire(record,board_objects::wireIndex(arrays,record),*handle,&wire);
  TCWireEndsV1 ends{};
  ends.size=sizeof(ends);ends.version=TC_WIRE_ENDS_VERSION_1;ends.wire=*handle;
  board_connect::fillEnd(ends.ends[0],wire.x1,wire.y1);
  board_connect::fillEnd(ends.ends[1],wire.x2,wire.y2);
  TCGameHandle board{};
  uint64_t childGeneration=0;
  if(gameHandles.current(TC_GAME_OBJECT_BOARD,&board)!=TC_HANDLE_OK)return TC_SNAPSHOT_ERR_STALE;
  if(gameHandles.beginChildSnapshot(&board,frame,&childGeneration)!=TC_HANDLE_OK)return TC_SNAPSHOT_ERR_RETRY;
  for(uint64_t i=0;i<arrays.components;++i){
   const unsigned char* component=arrays.componentData+board_objects::kRecordHeader+i*board_objects::kComponentStride;
   const uint32_t kind=board_objects::readU8(component,0);
   if(!kind)continue;
   const int32_t cx=board_objects::readI16(component,2),cy=board_objects::readI16(component,4);
   const bool nearFirst=board_connect::withinWindow(ends.ends[0].x,ends.ends[0].y,cx,cy);
   const bool nearSecond=board_connect::withinWindow(ends.ends[1].x,ends.ends[1].y,cx,cy);
   if(!nearFirst&&!nearSecond)continue;
   TCPrototype prototype{};
   bool owned=false;
   if(kind==board_objects::kCustomComponentKind){
    const uint64_t customId=board_objects::readU64(component,0x188);
    if(customPrototypeGet&&customId){customPrototypeGet(customId,&prototype);owned=true;}
   }else if(prototypeGet&&builtinKinds.count(static_cast<uint8_t>(kind))){
    TCPrototypeKind key{};
    key.tag=static_cast<uint8_t>(kind);
    prototypeGet(&key,&prototype);owned=true;
   }
   if(!owned)continue;
   /* Inputs first, then outputs: the same order read_component_pins reports. */
   const uint64_t inputs=prototypeInputCount(prototype),outputs=prototypeOutputCount(prototype);
   for(uint32_t direction=0;direction<2;++direction){
    const uint64_t count=direction?outputs:inputs;
    for(uint64_t pinIndex=0;pinIndex<count;++pinIndex){
     const TCPin* pin=direction?prototypeOutputPin(prototype,pinIndex):prototypeInputPin(prototype,pinIndex);
     if(!pin)continue;
     const TCPinInfoV1 info=board_pins::decodePin(*pin,direction?TC_PIN_OUTPUT:TC_PIN_INPUT);
     for(int endIndex=0;endIndex<2;++endIndex){
      TCWireEndV1& end=ends.ends[endIndex];
      if(end.flags&TC_WIRE_END_HAS_COMPONENT)continue;
      if(!board_connect::pinSitsOnPoint(end.x,end.y,cx,cy,info.x,info.y))continue;
      end.direction=direction?TC_PIN_OUTPUT:TC_PIN_INPUT;
      end.pin_index=static_cast<uint32_t>(pinIndex);
      if(gameHandles.issueChild(TC_GAME_OBJECT_COMPONENT,component,childGeneration,&end.component)!=TC_HANDLE_OK){
       if(prototypeDestroy)prototypeDestroy(&prototype);
       return TC_SNAPSHOT_ERR_RETRY;
      }
      end.flags|=TC_WIRE_END_HAS_COMPONENT;
     }
    }
   }
   if(prototypeDestroy)prototypeDestroy(&prototype);
  }
  if(gameHandles.valid(handle)!=1||frame!=engineFrame())return TC_SNAPSHOT_ERR_RETRY;
  *out=ends;
  return TC_SNAPSHOT_OK;
 }
 static int simulationStateService(void* context,TCSimulationStateV1* out,uint32_t outSize){
  return static_cast<NativeRuntime*>(context)->readSimulationState(out,outSize);
 }
 static int simulationValueService(void* context,uint64_t byteOffset,uint32_t bits,uint64_t* out){
  return static_cast<NativeRuntime*>(context)->readSimulationValue(byteOffset,bits,out);
 }
 /* ---- V2: control, consistent reads and channel addressing --------------- */
 static int simulationCycleService(void* context,int64_t* out){
  auto* self=static_cast<NativeRuntime*>(context);
  if(!out)return TC_SIMULATION_ERR_ARGUMENT;
  *out=self->cycleNow();
  return TC_SIMULATION_OK;
 }
 static int simulationStateSizeService(void* context,uint64_t* out){
  if(!context||!out)return TC_SIMULATION_ERR_ARGUMENT;
  *out=tc::sim::kStateBufferSize;
  return TC_SIMULATION_OK;
 }
 static int simulationSnapshotService(void* context,const TCSimChannelV1* channels,
                                      uint32_t count,uint64_t* values,uint32_t capacity,
                                      int64_t* outCycle,uint32_t* outStable){
  return static_cast<NativeRuntime*>(context)->readSimulationSnapshot(
      channels,count,values,capacity,outCycle,outStable);
 }
 static int simulationRunToService(void* context,int64_t target){
  return static_cast<NativeRuntime*>(context)->submitSimulationRequest(0,target);
 }
 static int simulationRunForService(void* context,int64_t cycles){
  auto* self=static_cast<NativeRuntime*>(context);
  if(!self||cycles<=0)return TC_SIMULATION_ERR_ARGUMENT;
  const int64_t current=self->cycleNow();
  if(current<0)return TC_SIMULATION_ERR_STATE;
  const int64_t target=current>INT64_MAX-cycles?INT64_MAX:current+cycles;
  return self->submitSimulationRequest(0,target);
 }
 static int simulationStepService(void* context,uint32_t cycles){
  auto* self=static_cast<NativeRuntime*>(context);
  if(!self||cycles==0)return TC_SIMULATION_ERR_ARGUMENT;
  const int64_t current=self->cycleNow();
  if(current<0)return TC_SIMULATION_ERR_STATE;
  const int64_t step=static_cast<int64_t>(cycles);
  const int64_t target=current>INT64_MAX-step?INT64_MAX:current+step;
  return self->submitSimulationRequest(0,target);
 }
 static int simulationPauseService(void* context){
  return static_cast<NativeRuntime*>(context)->submitSimulationRequest(1,0);
 }
 static int simulationResetService(void* context){
  return static_cast<NativeRuntime*>(context)->submitSimulationRequest(2,-1);
 }
 static int simulationSliceService(void* context,uint32_t cycles){
  if(!context)return TC_SIMULATION_ERR_ARGUMENT;
  tc::sim_control::store().setSlice(cycles);
  return TC_SIMULATION_OK;
 }
 static int simulationControlService(void* context,TCSimulationControlV1* out,uint32_t outSize){
  if(!context||!out)return TC_SIMULATION_ERR_ARGUMENT;
  if(outSize<sizeof(TCSimulationControlV1))return TC_SIMULATION_ERR_ARGUMENT;
  tc::sim_control::store().fill(out);
  return TC_SIMULATION_OK;
 }
 static int channelFromWireService(void* context,const TCGameHandle* wire,
                                   TCSimWireChannelV1* out,uint32_t outSize){
  return static_cast<NativeRuntime*>(context)->wireChannel(wire,out,outSize);
 }
 static int channelResolveService(void* context,const TCGameHandle* board,
                                  TCSimWireChannelV1* channels,uint32_t count,
                                  uint32_t* resolved){
  return static_cast<NativeRuntime*>(context)->resolveWireChannels(
      board,channels,count,resolved);
 }
 /* ---- the per-cycle capture (TC_SERVICE_SIM_CAPTURE) -------------------- */
 static int captureConfigureService(void* context,const TCSimChannelV1* channels,
                                    uint32_t count,uint32_t depth,
                                    const TCCaptureTriggerV1* trigger){
  auto* self=static_cast<NativeRuntime*>(context);
  const int status=tc::scope_capture::store().configure(channels,count,depth,trigger);
  if(status==TC_SIM_CAPTURE_OK&&self)
   self->logger("Scope capture: configured "+std::to_string(count)+" channel(s), depth "+
                std::to_string(depth));
  return status;
 }
 static int captureStartService(void* context){
  auto* self=static_cast<NativeRuntime*>(context);
  const int status=tc::scope_capture::store().start();
  if(self&&status==TC_SIM_CAPTURE_OK){
   TCCaptureStatusV1 state{};
   tc::scope_capture::store().fill(&state);
   self->logger(std::string("Scope capture: started; the running program carries the tick: ")+
                (state.injected?"yes":"no"));
  }
  return status;
 }
 static int captureStopService(void* context){
  (void)context;
  return tc::scope_capture::store().stop();
 }
 static int captureReadService(void* context,uint64_t* cycles,uint64_t* values,
                               uint32_t capacity,uint32_t* rows){
  if(!context)return TC_SIM_CAPTURE_ERR_ARGUMENT;
  return tc::scope_capture::store().read(cycles,values,capacity,rows);
 }
 static int captureStatusService(void* context,TCCaptureStatusV1* out,uint32_t outSize){
  if(!context||!out)return TC_SIM_CAPTURE_ERR_ARGUMENT;
  if(outSize<sizeof(TCCaptureStatusV1))return TC_SIM_CAPTURE_ERR_ARGUMENT;
  tc::scope_capture::store().fill(out);
  return TC_SIM_CAPTURE_OK;
 }
 /* A run request travels the game's own sim.do entry, so the sim.do hook chain
    (cycle-guard, the loader's slice, anything else) sees it exactly as it sees
    the player's run button.  The model is the one the loader's chain link
    remembered - there is no other way to name it without a board. */
 int submitSimulationRequest(uint8_t command,int64_t target){
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return TC_SIMULATION_ERR_THREAD;
  auto found=aliases.find("sim.do");
  if(found==aliases.end()||!found->second)return TC_SIMULATION_ERR_UNAVAILABLE;
  void* model=tc::sim_control::store().model();
  if(!model)return TC_SIMULATION_ERR_STATE;
  using Fn=void(*)(void*,uint8_t,int64_t);
  reinterpret_cast<Fn>(found->second)(model,command,target);
  return TC_SIMULATION_OK;
 }
 int readSimulationSnapshot(const TCSimChannelV1* channels,uint32_t count,
                            uint64_t* values,uint32_t capacity,int64_t* outCycle,
                            uint32_t* outStable){
  if(!channels||!values||count==0)return TC_SIMULATION_ERR_ARGUMENT;
  if(capacity<count)return TC_SIMULATION_ERR_ARGUMENT;
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return TC_SIMULATION_ERR_THREAD;
  const int64_t before=cycleNow();
  for(uint32_t index=0;index<count;++index){
   const TCSimChannelV1& channel=channels[index];
   if(channel.size<sizeof(TCSimChannelV1)||channel.version!=TCSIM_CHANNEL_VERSION_1)
    return TC_SIMULATION_ERR_ARGUMENT;
   uint64_t value=0;
   const int status=readSimulationValue(channel.byte_offset,channel.bits,&value);
   if(status!=TC_SIMULATION_OK)return status;
   values[index]=value;
  }
  const int64_t after=cycleNow();
  if(outCycle)*outCycle=after;
  if(outStable)*outStable=(before==after&&before>=0)?1u:0u;
  return TC_SIMULATION_OK;
 }
 /* A wire handle from a board object snapshot, turned into "read this at that
    offset, this many bits". */
 int wireChannel(const TCGameHandle* wire,TCSimWireChannelV1* out,uint32_t outSize){
  if(!wire||!out)return TC_SIM_CHANNEL_ERR_ARGUMENT;
  if(outSize<sizeof(TCSimWireChannelV1))return TC_SIM_CHANNEL_ERR_ARGUMENT;
  TCWireInfoV1 info{};
  const int status=readBoardWire(wire,&info,sizeof(info));
  if(status!=TC_SNAPSHOT_OK)return TC_SIM_CHANNEL_ERR_HANDLE;
  if(!(info.flags&TC_WIRE_INFO_HAS_STATE_SLOT))return TC_SIM_CHANNEL_ERR_STATE;
  TCSimWireChannelV1 channel{};
  channel.size=sizeof(channel);
  channel.version=TCSIM_WIRE_CHANNEL_VERSION_1;
  channel.channel_id=out->channel_id?out->channel_id:info.id;
  channel.byte_offset=info.state_byte_offset;
  channel.bits=(info.flags&TC_WIRE_INFO_HAS_WIDTH)?info.bit_width:1u;
  channel.wire_id=info.id;
  channel.x1=info.x1;channel.y1=info.y1;channel.x2=info.x2;channel.y2=info.y2;
  if(channel.bits<1u||channel.bits>64u)channel.bits=1u;
  *out=channel;
  return TC_SIM_CHANNEL_OK;
 }
 /* Re-resolve by wire id first and by endpoints after a recompile moved the
    records.  Both are read straight out of the board's wire table, in one
    frame, so a channel can never be re-pointed at a different wire. */
 int resolveWireChannels(const TCGameHandle* board,TCSimWireChannelV1* channels,
                         uint32_t count,uint32_t* resolved){
  if(!channels||count==0)return TC_SIM_CHANNEL_ERR_ARGUMENT;
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return TC_SIM_CHANNEL_ERR_THREAD;
  const void* boardRaw=nullptr;
  if(!board||gameHandles.resolve(board,&boardRaw)!=TC_HANDLE_OK)
   return TC_SIM_CHANNEL_ERR_HANDLE;
  board_objects::Arrays arrays{};
  if(!board_objects::readArrays(boardRaw,arrays))return TC_SIM_CHANNEL_ERR_STATE;
  const int64_t frame=engineFrame();
  const TCGameHandle none{};
  uint32_t live=0;
  for(uint32_t index=0;index<count;++index){
   TCSimWireChannelV1& channel=channels[index];
   if(channel.version!=TCSIM_WIRE_CHANNEL_VERSION_1)continue;
   const uint64_t id=channel.wire_id;
   const int32_t x1=channel.x1,y1=channel.y1,x2=channel.x2,y2=channel.y2;
   channel.bits=0;
   channel.byte_offset=0;
   bool found=false;
   const auto apply=[&](const TCWireInfoV1& info){
    if(!(info.flags&TC_WIRE_INFO_HAS_STATE_SLOT))return;
    channel.byte_offset=info.state_byte_offset;
    channel.bits=(info.flags&TC_WIRE_INFO_HAS_WIDTH)?info.bit_width:1u;
    if(channel.bits<1u||channel.bits>64u)channel.bits=1u;
    channel.wire_id=info.id;
    channel.x1=info.x1;channel.y1=info.y1;channel.x2=info.x2;channel.y2=info.y2;
    found=true;
   };
   if(id<arrays.wires){
    const auto* record=arrays.wireData+board_objects::kRecordHeader+
                       id*board_objects::kWireStride;
    TCWireInfoV1 info{};
    board_objects::decodeWire(record,id,none,&info);
    if(info.x1==x1&&info.y1==y1&&info.x2==x2&&info.y2==y2)apply(info);
   }
   if(!found){
    for(uint64_t other=0;other<arrays.wires;++other){
     const auto* record=arrays.wireData+board_objects::kRecordHeader+
                        other*board_objects::kWireStride;
     TCWireInfoV1 info{};
     board_objects::decodeWire(record,other,none,&info);
     if(info.x1==x1&&info.y1==y1&&info.x2==x2&&info.y2==y2){apply(info);break;}
    }
   }
   if(found)++live;
  }
  if(resolved)*resolved=live;
  if(frame!=engineFrame())return TC_SIM_CHANNEL_ERR_STATE;
  return TC_SIM_CHANNEL_OK;
 }
 /* A value read is the game's own sim_state_read_u64 with the low `bits` kept -
    the same operation the waveform probes are verified against.  The offset is
    bounded by the state buffer the simulator allocates once at init, so a Mod
    cannot turn a bad offset into a wild read. */
 int readSimulationState(TCSimulationStateV1* out,uint32_t outSize){
  if(!out)return TC_SIMULATION_ERR_ARGUMENT;
  if(outSize<sizeof(TCSimulationStateV1))return TC_SIMULATION_ERR_ARGUMENT;
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return TC_SIMULATION_ERR_THREAD;
  TCSimulationStateV1 state{};
  state.size=sizeof(state);state.version=TC_SIMULATION_STATE_VERSION_1;
  if(aliases.count("sim.cycle")){state.cycle=cycleNow();state.flags|=TC_SIMULATION_STATE_HAS_CYCLE;}
  const int64_t frame=engineFrame();
  if(frame>=0){state.engine_frame=frame;state.flags|=TC_SIMULATION_STATE_HAS_ENGINE_FRAME;}
  if(simulationState&&*simulationState&&aliases.count("sim.state.read")){
   state.state_size=tc::sim::kStateBufferSize;
   state.flags|=TC_SIMULATION_STATE_HAS_STATE_BUFFER;
  }
  *out=state;
  return TC_SIMULATION_OK;
 }
 int readSimulationValue(uint64_t byteOffset,uint32_t bits,uint64_t* out){
  if(!out)return TC_SIMULATION_ERR_ARGUMENT;
  if(bits<1||bits>tc::sim::kMaxBits)return TC_SIMULATION_ERR_ARGUMENT;
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return TC_SIMULATION_ERR_THREAD;
  auto reader=aliases.find("sim.state.read");
  if(reader==aliases.end()||!reader->second)return TC_SIMULATION_ERR_UNAVAILABLE;
  if(!tc::sim::offsetInRange(byteOffset))return TC_SIMULATION_ERR_RANGE;
  using Fn=uint64_t(*)(int64_t);
  const uint64_t word=reinterpret_cast<Fn>(reader->second)(static_cast<int64_t>(byteOffset));
  *out=tc::sim::lowBits(word,bits);
  return TC_SIMULATION_OK;
}
 /* ------------------------------------------------------------- IO values
    The game's own value editors (a level's global inputs, a component's global
    inputs and a constant's value field) are what TC_SERVICE_IO_VALUE exposes.
    The parts that are game code are reached through symbol-profile aliases, so
    a build without them answers UNAVAILABLE instead of calling a stale address;
    the parts that are plain record arithmetic (a component's declared width,
    its id) are read here, with the offsets the Board service and the punch-tape
    Mod already agreed on (docs/research/board-object-fields.md). */
 static constexpr uint64_t kIoRecordHeader=8,kIoRecordStride=0x238;
 static constexpr uint64_t kIoIdOffset=0x08,kIoKindOffset=0x00;
 static constexpr uint64_t kIoWidthOffset=0xe0,kIoWidthMirrorOffset=0xe8;
 static constexpr uint8_t kIoConstantKind=0x2e;
 /* The board's component array is a Nim seq: eight-byte header, then records. */
 bool boardComponentRecord(const TCGameHandle* board,uint64_t index,const unsigned char** record,
                           uint64_t* count){
  if(!board||!record)return false;
  const void* raw=nullptr;
  if(gameHandles.resolve(board,&raw)!=TC_HANDLE_OK||!raw)return false;
  const auto* bytes=static_cast<const unsigned char*>(raw);
  uint64_t total=0;const unsigned char* data=nullptr;
  std::memcpy(&total,bytes+0x78,sizeof(total));
  std::memcpy(&data,bytes+0x80,sizeof(data));
  if(!data||total==0||total>1000000)return false;
  if(count)*count=total;
  if(index>=total)return false;
  *record=data+kIoRecordHeader+index*kIoRecordStride;
  return true;
 }
 static void* ioAlias(const std::map<std::string,void*>& table,const char* name){
  auto found=table.find(name?name:"");
  return found==table.end()?nullptr:found->second;
 }
 static uint32_t ioRecordWidth(const unsigned char* record){
  uint64_t width=0,mirror=0;
  std::memcpy(&width,record+kIoWidthOffset,sizeof(width));
  std::memcpy(&mirror,record+kIoWidthMirrorOffset,sizeof(mirror));
  if(width>=1&&width<=64)return static_cast<uint32_t>(width);
  if(mirror>=1&&mirror<=64)return static_cast<uint32_t>(mirror);
  return 8u;
 }
 static void appendDecimal(char* buffer,std::size_t& length,uint64_t magnitude){
  char reversed[24];std::size_t digits=0;
  do{reversed[digits++]=static_cast<char>('0'+(magnitude%10));magnitude/=10;}while(magnitude);
  while(digits)buffer[length++]=reversed[--digits];
 }
 int ioThreadCheck() const {
  return (!gameThreadId||GetCurrentThreadId()!=gameThreadId)?TC_IO_VALUE_ERR_THREAD:TC_IO_VALUE_OK;
 }
 /* ------------------------------------------------- custom-tail persistence
    A native component's configuration is stored in the key/value table the
    component record itself owns (tc::component_tail owns the format, and
    docs/sdk/services.md the contract).  Everything below is the game-side half
    and every entry point is deliberately narrow:

      - game thread only, because the table belongs to the game's own data
        structures and the setter allocates;
      - the record is resolved again on every call from the *current* Board by
        (custom id, instance id), never kept: a board edit replaces the object
        table, and a saved pointer would then name freed memory;
      - the element walk stays inside one committed, readable region and stops
        at a bounded slot count;
      - every write goes through the game's own `[]=`, so the table's own
        growth and rehashing are the game's code, not a copy of it.

    A build whose profile does not describe the record layout gets no accessor
    at all, and tc.component.storage then reports no persistence. */
 static bool tailTableFind(void* context,uint64_t customId,uint64_t instance,void** table){
  auto* self=static_cast<NativeRuntime*>(context);
  if(!self||!table)return false;
  if(!self->gameThreadId||GetCurrentThreadId()!=self->gameThreadId)return false;
  if(!tc::component_tail::layoutKnown())return false;
  const void* board=nullptr;
  if(!self->currentBoardRaw(&board)||!board)return false;
  board_objects::Arrays arrays{};
  if(!board_objects::readArrays(board,arrays))return false;
  for(uint64_t i=0;i<arrays.components;++i){
   const unsigned char* record=arrays.componentData+board_objects::kRecordHeader+
                               i*board_objects::kComponentStride;
   if(record[0]!=board_objects::kCustomComponentKind)continue;
   if(board_objects::readU64(record,0x08)!=instance)continue;
   if(board_objects::readU64(record,0x188)!=customId)continue;
   *table=const_cast<unsigned char*>(record)+tc::component_tail::kTableOffset;
   return true;
  }
  return false;
 }
 /* The end of a placement, as the game itself reports it.  The redraw hook is
    only called while the game keeps redrawing the clipboard mesh, so it can arm
    the immediate-mode preview but cannot end it: the successful placement path
    stops redrawing without ever calling hide_clipboard (measured - that is the
    bug this replaces), and the Mod's ghost would stay at its last snapped cell.

    What does run on every clipboard change is
    update_state_clipboard__presenterZupdate95state95clipboard_u120: it is handed
    the record it is working with, which is the very record the redraw hook
    captured.  kind 0x4e + the same custom id means the placement is still live;
    an empty record or another component means the game has put its clipboard
    down (placed, cancelled, or replaced by a different selection) - the same
    event that ends the game's own ghost. */
 void observeClipboardRecord(const void* raw){
  if(!placementPreview.active)return;
  if(raw){
   const auto* record=static_cast<const unsigned char*>(raw);
   if(board_objects::readU8(record,0)==board_objects::kCustomComponentKind&&
      board_objects::readU64(record,0x188)==placementPreview.customId)return;
  }
  endPlacementPreview("the game's clipboard record is gone");
 }
 void endPlacementPreview(const char* reason){
  if(!placementPreview.active)return;
  if(placementPreviewEndedCount<12){
   ++placementPreviewEndedCount;
   logger(std::string("Component render: placement preview ended for custom=")+
          std::to_string(placementPreview.customId)+" ("+reason+")");
  }
  placementPreview=PlacementPreview{};
 }
 static bool tailTableRead(void*,void* table,uint64_t key,uint64_t* value,bool* found){
  if(!table||!value||!found)return false;
  *found=false;
  const auto* bytes=static_cast<const unsigned char*>(table);
  if(!board_objects::readableRegion(bytes,0x18))return false;
  const uint64_t slots=board_objects::readU64(bytes,0);
  if(!slots)return true;   /* an empty table simply has no key */
  const unsigned char* elements=board_objects::readPointer(bytes,8);
  if(slots>tc::component_tail::kMaxSlots||!elements)return false;
  const uint64_t span=tc::component_tail::kElementHeader+
                      slots*tc::component_tail::kElementStride;
  if(!board_objects::readableRegion(elements,static_cast<size_t>(span)))return false;
  for(uint64_t i=0;i<slots;++i){
   const unsigned char* element=elements+tc::component_tail::kElementHeader+
                                i*tc::component_tail::kElementStride;
   if(board_objects::readU64(element,tc::component_tail::kElementHashOffset)==0)continue;
   if(board_objects::readU64(element,tc::component_tail::kElementKeyOffset)!=key)continue;
   *value=board_objects::readU64(element,tc::component_tail::kElementValueOffset);
   *found=true;
   return true;
  }
  return true;
 }
 bool tailTableWrite(void* table,uint64_t key,uint64_t value){
  if(!table)return false;
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return false;
  const auto setter=ioAlias(aliases,"save.custom_tail_set");
  if(!setter)return false;
  using SetFn=void(*)(void*,int64_t,int64_t);
  reinterpret_cast<SetFn>(setter)(table,static_cast<int64_t>(key),static_cast<int64_t>(value));
  return true;
 }
 static bool tailTableWriteService(void* context,void* table,uint64_t key,uint64_t value){
  auto* self=static_cast<NativeRuntime*>(context);
  return self&&self->tailTableWrite(table,key,value);
 }
 void bindTailStorageSources(){
  if(!tc::component_tail::layoutKnown()){
   logger("Component storage: this build profile does not describe the custom tail layout; configuration stays in memory");
   tc::logic::clearTailAccess();
   return;
  }
  if(!ioAlias(aliases,"save.custom_tail_set")){
   logger("Component storage: save.custom_tail_set is not resolved on this build; configuration stays in memory");
   tc::logic::clearTailAccess();
   return;
  }
  tc::logic::bindTailAccess({this,&NativeRuntime::tailTableFind,&NativeRuntime::tailTableRead,
                             &NativeRuntime::tailTableWriteService});
  logger("Component storage: configuration persists in the component record's own table");
 }
 /* ---------------------------------------------- missing-Mod capture
    The game drops a custom component whose prototype nobody registered, and it
    does so inside the level load: the parse reads the whole record first, and
    only afterwards does the component fail to reach the Board (measured in
    tests/component-placeholder-playtest.ps1, where the file survives an unsaved
    load but a save in that state loses the record for good).

    The deserializer builds that record on its own stack and fills its key/value
    table through the game's own setter, so a detour on that setter sees every
    record a load reads - including the ones that are about to be dropped -
    together with the geometry the parse already wrote, because the record base
    is exactly `kTableOffset` bytes before the table.  That capture is what a
    diagnostic, a placeholder or a rescue needs, and it needs no knowledge of
    where the component is thrown away. */
 struct CapturedRecord {
  uint64_t customId=0,instanceId=0;
  int16_t x=0,y=0;
  uint8_t rotation=0;
  std::vector<std::pair<uint64_t,uint64_t>> entries;
 };
 using TailSetFn=void(*)(void*,int64_t,int64_t);
 std::map<uint64_t,CapturedRecord> stagedRecords;   /* by instance id: unique per board */
 TailSetFn tailSetOriginal=nullptr;
 void* tailSetTarget=nullptr;
 bool captureWindow=false,capturePending=false;
  /* A capture that outlives the session: the record of a component whose owner
     was missing is written next to the loader's own data, because the save the
     player makes in that state no longer contains it.  When the owner is back,
     the entry is put on the Board again from here. */
  std::string captureLevel;
  std::map<uint64_t,CapturedRecord> savedRecords;
 static void detourTailSet(void* table,int64_t key,int64_t value){
  auto* self=activeInstance;
  if(!self){return;}
  self->observeTailSet(table,key,value);
 }
 void observeTailSet(void* table,int64_t key,int64_t value){
  if(tailSetOriginal)tailSetOriginal(table,key,value);
  if(!captureWindow||!table)return;
  const auto* record=static_cast<const unsigned char*>(table)-tc::component_tail::kTableOffset;
  if(record[0]!=board_objects::kCustomComponentKind)return;
  const uint64_t instance=board_objects::readU64(record,8);
  auto& captured=stagedRecords[instance];
  if(captured.entries.empty()){
   captured.instanceId=instance;
   captured.customId=board_objects::readU64(record,0x188);
   captured.x=board_objects::readI16(record,2);
   captured.y=board_objects::readI16(record,4);
   captured.rotation=board_objects::readU8(record,6);
  }
  captured.entries.emplace_back(static_cast<uint64_t>(key),static_cast<uint64_t>(value));
 }
 void armMissingModCapture(){
  auto found=aliases.find("save.custom_tail_set");
  if(found==aliases.end())return;
  if(MH_CreateHook(found->second,reinterpret_cast<void*>(&NativeRuntime::detourTailSet),
                   reinterpret_cast<void**>(&tailSetOriginal))!=MH_OK)return;
  if(MH_EnableHook(found->second)!=MH_OK){MH_RemoveHook(found->second);tailSetOriginal=nullptr;return;}
  tailSetTarget=found->second;ownedHooks.insert(found->second);
  loaderOwned[found->second]="missing-Mod capture (custom tail writes during a level load)";
  logger("Missing-Mod capture: custom tail writes watched");
 }
 /* ------------------------------------------- configuration undo/redo
    The game's own undo entry is shared by the whole board, and a configuration
    change has no entry kind of its own, so the host answers it while one of its
    own steps is pending: the byte images the storage service kept are written
    back into the component record and the press is reported as handled.  With no
    pending step the game's own undo runs untouched, so board edits behave exactly
    as before. */
 using UndoFn=uint8_t(*)(void*);
 UndoFn undoOriginal=nullptr;
 UndoFn redoOriginal=nullptr;
 static uint8_t detourUndo(void* board){
  auto* self=activeInstance;
  if(tc::logic::undoConfigEdit())return 1;
  return self&&self->undoOriginal?self->undoOriginal(board):0;
 }
 static uint8_t detourRedo(void* board){
  auto* self=activeInstance;
  if(tc::logic::redoConfigEdit())return 1;
  return self&&self->redoOriginal?self->redoOriginal(board):0;
 }
 void armConfigUndo(){
  const auto hook=[&](const char* alias,void* detour,void** original,const char* what){
   auto found=aliases.find(alias);
   if(found==aliases.end()||!found->second)return;
   if(MH_CreateHook(found->second,detour,original)!=MH_OK)return;
   if(MH_EnableHook(found->second)!=MH_OK){MH_RemoveHook(found->second);return;}
   ownedHooks.insert(found->second);
   loaderOwned[found->second]=what;
  };
  hook("board.undo",reinterpret_cast<void*>(&NativeRuntime::detourUndo),
       reinterpret_cast<void**>(&undoOriginal),
       "configuration undo (the host's own edit steps answer it)");
  hook("board.redo",reinterpret_cast<void*>(&NativeRuntime::detourRedo),
       reinterpret_cast<void**>(&redoOriginal),
       "configuration redo (the host's own edit steps answer it)");
  logger("Component storage: configuration edits answer undo/redo while a step is pending");
 }
 /* ------------------------------------------------ the save moment
    Two entries reach a file: the level save (`save.level`, which the event bus
    already watches) and the schematic writer.  Both dispatch TC_LOGIC_SAVE to the
    live instances before the game serializes anything, so a definition can commit
    derived data or repair a configuration and have the file being written see it.
    The dispatch is cheap when no definition declares the callback: it walks the
    live bindings and checks a size-gated pointer. */
 void armSaveDispatch(){
  const auto hook=[&](const char* alias,void* detour,void** original,const char* what){
   auto found=aliases.find(alias);
   if(found==aliases.end()||!found->second)return;
   if(MH_CreateHook(found->second,detour,original)!=MH_OK)return;
   if(MH_EnableHook(found->second)!=MH_OK){MH_RemoveHook(found->second);return;}
   ownedHooks.insert(found->second);
   loaderOwned[found->second]=what;
  };
  if(!saveHooked){
   hook("save.level",reinterpret_cast<void*>(&NativeRuntime::detourSave),&saveOriginal,
        "the SAVE event and the TC_LOGIC_SAVE dispatch");
   saveHooked=saveOriginal!=nullptr;
  }
  hook("save.schematic",reinterpret_cast<void*>(&NativeRuntime::detourSaveSchematic),
       &saveSchematicOriginal,"the TC_LOGIC_SAVE dispatch for a written schematic");
  /* The event source's line is kept verbatim: it is armed here now, but tests and
     the documentation read it as the SAVE event's state. */
  if(saveHooked)logger("Event source save armed");
  logger(std::string("Component lifecycle: save dispatch ")+
         (saveHooked||saveSchematicOriginal?"armed":"unavailable"));
 }
 /* The capture window is the level load itself; the report waits for the frame
    after it, when the Board is complete and the missing components are known to
    be missing. */
 void beginMissingModCapture(const char* level){
  stagedRecords.clear();captureWindow=true;capturePending=true;
  captureLevel.clear();
  for(const char* c=level;c&&*c;++c){
   const unsigned char ch=static_cast<unsigned char>(*c);
   const bool unsafe=ch<32||ch=='/'||ch=='\\'||ch==':'||ch=='*'||ch=='?'||ch=='"'||ch=='<'||ch=='>'||ch=='|';
   captureLevel.push_back(unsafe?'_':static_cast<char>(ch));
  }
  if(captureLevel.empty())captureLevel="board";
 }
 fs::path rescueStorePath() const {
  return core.dir/L"missing-mods"/fs::u8path(captureLevel+".bin");
 }
 static void appendU32(std::string& out,uint32_t value){for(int i=0;i<4;++i)out.push_back(static_cast<char>(value>>(8*i)));}
 static void appendU64(std::string& out,uint64_t value){for(int i=0;i<8;++i)out.push_back(static_cast<char>(value>>(8*i)));}
 static bool takeU32(const std::string& data,size_t& at,uint32_t* out){
  if(at+4>data.size())return false;uint32_t value=0;
  for(int i=0;i<4;++i)value|=static_cast<uint32_t>(static_cast<unsigned char>(data[at+i]))<<(8*i);
  at+=4;*out=value;return true;
 }
 static bool takeU64(const std::string& data,size_t& at,uint64_t* out){
  if(at+8>data.size())return false;uint64_t value=0;
  for(int i=0;i<8;++i)value|=static_cast<uint64_t>(static_cast<unsigned char>(data[at+i]))<<(8*i);
  at+=8;*out=value;return true;
 }
 /* The store is one file per level: the records whose owner was missing the last
    time that level was loaded, minus the ones that made it back. */
 void writeRescueStore(){
  std::string data="TCM3RSQ1";
  appendU32(data,static_cast<uint32_t>(savedRecords.size()));
  for(const auto& item:savedRecords){
   const auto& record=item.second;
   appendU64(data,record.customId);appendU64(data,record.instanceId);
   appendU32(data,static_cast<uint32_t>(static_cast<uint16_t>(record.x)));
   appendU32(data,static_cast<uint32_t>(static_cast<uint16_t>(record.y)));
   appendU32(data,record.rotation);
   appendU32(data,static_cast<uint32_t>(record.entries.size()));
   for(const auto& entry:record.entries){appendU64(data,entry.first);appendU64(data,entry.second);}
  }
  try{
   const auto path=rescueStorePath();
   std::error_code status;fs::create_directories(path.parent_path(),status);
   atomic(path,data);
   logger("Missing Mod: kept "+std::to_string(savedRecords.size())+
          " record(s) for level "+captureLevel+" in the loader's own data");
  }catch(const std::exception& e){logger(std::string("Missing Mod: the rescue store could not be written: ")+e.what());}
 }
 bool readRescueStore(){
  savedRecords.clear();
  const auto path=rescueStorePath();
  std::error_code status;
  if(!fs::exists(path,status))return false;
  std::string data;
  try{data=read(path);}catch(...){return false;}
  if(data.size()<12||data.compare(0,8,"TCM3RSQ1")!=0)return false;
  size_t at=8;uint32_t count=0;
  if(!takeU32(data,at,&count))return false;
  for(uint32_t i=0;i<count;++i){
   CapturedRecord record;
   uint32_t x=0,y=0,rotation=0,entries=0;
   if(!takeU64(data,at,&record.customId)||!takeU64(data,at,&record.instanceId)||
      !takeU32(data,at,&x)||!takeU32(data,at,&y)||!takeU32(data,at,&rotation)||
      !takeU32(data,at,&entries))return false;
   record.x=static_cast<int16_t>(x);record.y=static_cast<int16_t>(y);
   record.rotation=static_cast<uint8_t>(rotation);
   for(uint32_t e=0;e<entries;++e){
    uint64_t key=0,value=0;
    if(!takeU64(data,at,&key)||!takeU64(data,at,&value))return false;
    record.entries.emplace_back(key,value);
   }
   savedRecords.emplace(record.instanceId,std::move(record));
  }
  return true;
 }
 /* Does this (custom id, instance id) sit on the Board right now?  A record that
    does not is one the load dropped. */
 const unsigned char* boardRecordOf(uint64_t customId,uint64_t instance) const {
  const void* board=nullptr;
  if(!currentBoardRaw(&board)||!board)return nullptr;
  board_objects::Arrays arrays{};
  if(!board_objects::readArrays(board,arrays))return nullptr;
  for(uint64_t i=0;i<arrays.components;++i){
   const unsigned char* record=arrays.componentData+board_objects::kRecordHeader+
                               i*board_objects::kComponentStride;
   if(record[0]!=board_objects::kCustomComponentKind)continue;
   if(board_objects::readU64(record,8)!=instance)continue;
   if(board_objects::readU64(record,0x188)!=customId)continue;
   return record;
  }
  return nullptr;
 }
 /* One line per component the load read and the Board does not have, with the
    owner it belongs to.  This is the diagnostic half of the placeholder work:
    the player learns that saving now would lose the component, and the captured
    record is kept for the rescue half. */
 uint32_t reportMissingMods(){
  uint32_t missing=0;
  for(const auto& item:stagedRecords){
   const auto& captured=item.second;
   if(!captured.customId)continue;
   {
    std::lock_guard<std::mutex> lock(tc::logic::registryMutex);
    if(tc::logic::definitions.count(captured.customId))continue;   /* owned: nothing missing */
   }
   if(boardRecordOf(captured.customId,captured.instanceId))continue;   /* it survived */
   ++missing;
   /* The record carries its own identity: the TCM3 type entry has to agree with
      the component's custom id, and the schema/length entry says how big the
      saved configuration is. */
   uint32_t schema=0,bytes=0;
   for(const auto& entry:captured.entries){
    if(entry.first==tc::component_tail::fieldKey(tc::component_tail::kFieldSchemaLength)){
     schema=tc::component_tail::schemaOf(entry.second);
     bytes=tc::component_tail::lengthOf(entry.second);
    }
   }
   logger("Missing Mod: custom "+tc::logic::hex64(captured.customId)+" at ("+
          std::to_string(captured.x)+","+std::to_string(captured.y)+") rotation "+
          std::to_string(captured.rotation)+" was dropped by the game because no Mod registered it"+
          (bytes?("; it carries a "+
                  std::to_string(bytes)+"-byte schema-"+std::to_string(schema)+
                  " configuration record, kept for a reinstall"):std::string()));
   /* Keep it: this is the state in which a save would lose the record. */
   savedRecords[captured.instanceId]=captured;
  }
  if(missing)
   logger("Missing Mod: "+std::to_string(missing)+
          " component(s) on this level have no owner; saving now would lose them");
  return missing;
 }
 /* Is a component of this type already standing at this position?  The rescue
    check has to be positional: a restored component gets a fresh instance id
    from the game, so the captured one no longer matches anything. */
 /* Every live entry of a record's own table, in the order the slots hold them. */
 bool boardTailEntries(const unsigned char* record,
                       std::vector<std::pair<uint64_t,uint64_t>>* out) const {
  if(!record||!out)return false;
  const uint64_t slots=board_objects::readU64(record,tc::component_tail::kTableOffset);
  if(!slots)return true;
  const unsigned char* elements=
      board_objects::readPointer(record,tc::component_tail::kTableOffset+8);
  if(slots>tc::component_tail::kMaxSlots||!elements)return false;
  const uint64_t span=tc::component_tail::kElementHeader+
                      slots*tc::component_tail::kElementStride;
  if(!board_objects::readableRegion(elements,static_cast<size_t>(span)))return false;
  for(uint64_t i=0;i<slots;++i){
   const unsigned char* element=elements+tc::component_tail::kElementHeader+
                                i*tc::component_tail::kElementStride;
   if(board_objects::readU64(element,tc::component_tail::kElementHashOffset)==0)continue;
   out->emplace_back(board_objects::readU64(element,tc::component_tail::kElementKeyOffset),
                     board_objects::readU64(element,tc::component_tail::kElementValueOffset));
  }
  return true;
 }
 /* Duplication: the placement is the game's own helper, so the game registers the
    undo entry and one press removes the copy; the source's configuration is then
    copied into the new record entry by entry, without an undo step of its own, so
    the whole duplication stays one player action.  The copy is marked for
    TC_LOGIC_CLONE on its first bind. */
 bool duplicateComponent(void* board,uint64_t customId,uint64_t sourceInstance,
                         int16_t x,int16_t y,uint8_t rotation){
  if(!board||!addComponent||!tailSetOriginal)return false;
  const unsigned char* source=boardRecordOf(customId,sourceInstance);
  if(!source)return false;
  std::vector<std::pair<uint64_t,uint64_t>> entries;
  if(!boardTailEntries(source,&entries))return false;
  unsigned char placement[board_edits::kComponentRecordSize];
  board_edits::buildPlacement(board_edits::kCustomKind,customId,x,y,rotation,placement);
  bool placed=false;
  try{placed=addComponent(board,placement);}catch(...){placed=false;}
  if(!placed)return false;
  const unsigned char* copy=boardRecordAt(customId,x,y);
  if(!copy)return false;
  if(!entries.empty()){
   void* table=const_cast<unsigned char*>(copy)+tc::component_tail::kTableOffset;
   for(const auto& entry:entries)
    tailSetOriginal(table,static_cast<int64_t>(entry.first),static_cast<int64_t>(entry.second));
  }
  /* The bytes the copy must start from: taken from the record itself, so a plugin
     gets the source's configuration even when the board lookup is not possible at
     bind time. */
  uint32_t schema=0;
  std::vector<uint8_t> copied;
  for(const auto& entry:entries)
   if(entry.first==tc::component_tail::fieldKey(tc::component_tail::kFieldSchemaLength)){
    schema=tc::component_tail::schemaOf(entry.second);
    copied.assign(tc::component_tail::lengthOf(entry.second),0);
   }
  if(!copied.empty())
   {
    std::vector<tc::component_tail::Entry> asEntries;
    asEntries.reserve(entries.size());
    for(const auto& entry:entries)asEntries.push_back({entry.first,entry.second});
    if(tc::component_tail::decodeEntries(asEntries,customId,schema,
                                         static_cast<uint32_t>(copied.size()),
                                         copied.data())!=tc::component_tail::Status::Ok)
    copied.clear();
   }
  logger("Component clone: carried "+std::to_string(copied.size())+" configuration byte(s), schema "+
         std::to_string(schema));
  /* The bound instance is the authority when there is one: the record may be
     mid-flight while a service write is being committed. */
  std::vector<uint8_t> bound=tc::logic::configOfInstance(customId,sourceInstance);
  if(!bound.empty())copied=std::move(bound);
  tc::logic::markClone(customId,board_objects::readU64(copy,8),std::move(copied));
  logger("Component clone: duplicated custom "+tc::logic::hex64(customId)+" from instance "+
         tc::logic::hex64(sourceInstance)+" to ("+std::to_string(x)+","+std::to_string(y)+
         ") with "+std::to_string(entries.size())+" configuration entr"+
         (entries.size()==1?"y":"ies"));
  return true;
 }
 const unsigned char* boardRecordAt(uint64_t customId,int16_t x,int16_t y) const {
  const void* board=nullptr;
  if(!currentBoardRaw(&board)||!board)return nullptr;
  board_objects::Arrays arrays{};
  if(!board_objects::readArrays(board,arrays))return nullptr;
  for(uint64_t i=0;i<arrays.components;++i){
   const unsigned char* record=arrays.componentData+board_objects::kRecordHeader+
                               i*board_objects::kComponentStride;
   if(record[0]!=board_objects::kCustomComponentKind)continue;
   if(board_objects::readU64(record,0x188)!=customId)continue;
   if(board_objects::readI16(record,2)!=x||board_objects::readI16(record,4)!=y)continue;
   return record;
  }
  return nullptr;
 }
 /* Puts the kept records back on the Board once their owner is installed again.
    The placement goes through the same menu helper the command bus uses, so the
    game sees an ordinary placement, and the configuration is then rebuilt entry
    by entry through the game's own table setter.  What is not restored is the
    old instance id - the game issues a fresh one - and that is harmless: wires
    name coordinates, not instance ids, and those are the coordinates the
    component is placed at, so the circuit reconnects on the next compile. */
 uint32_t rescueMissingMods(){
  if(!addComponent||!readRescueStore())return 0;
  void* board=nullptr;
  if(!currentBoardRaw(const_cast<const void**>(&board))||!board)return 0;
  uint32_t rescued=0;
  for(auto item=savedRecords.begin();item!=savedRecords.end();){
   const CapturedRecord& captured=item->second;
   bool owned=false;
   {
    std::lock_guard<std::mutex> lock(tc::logic::registryMutex);
    owned=tc::logic::definitions.count(captured.customId)!=0;
   }
   if(!owned){++item;continue;}
   if(boardRecordAt(captured.customId,captured.x,captured.y)){item=savedRecords.erase(item);continue;}
   unsigned char record[board_edits::kComponentRecordSize];
   board_edits::buildPlacement(board_edits::kCustomKind,captured.customId,captured.x,captured.y,
                               captured.rotation,record);
   bool placed=false;
   try{placed=addComponent(board,record);}catch(...){placed=false;}
   if(!placed){logger("Missing Mod rescue: the game refused to place custom "+
                      tc::logic::hex64(captured.customId)+" at ("+std::to_string(captured.x)+","+
                      std::to_string(captured.y)+")");++item;continue;}
   const unsigned char* live=boardRecordAt(captured.customId,captured.x,captured.y);
   uint32_t written=0;
   if(live&&tailSetOriginal){
    void* table=const_cast<unsigned char*>(live)+tc::component_tail::kTableOffset;
    for(const auto& entry:captured.entries){
     tailSetOriginal(table,static_cast<int64_t>(entry.first),static_cast<int64_t>(entry.second));
     ++written;
    }
   }
   logger("Missing Mod rescue: put custom "+tc::logic::hex64(captured.customId)+" back at ("+
          std::to_string(captured.x)+","+std::to_string(captured.y)+") rotation "+
          std::to_string(captured.rotation)+" with "+std::to_string(written)+
          " stored configuration entr"+(written==1?"y":"ies")+
          "; the game assigns a fresh instance id, the wires reconnect by position");
   ++rescued;
   item=savedRecords.erase(item);
  }
  if(rescued)writeRescueStore();
  return rescued;
 }
 static int ioEvaluateService(void* context,const char* expression,uint64_t* out){
  return static_cast<NativeRuntime*>(context)->evaluateIoExpression(expression,out);
 }
 static int ioFormatService(void* context,uint64_t value,uint32_t width,uint32_t format,char* out,
                            uint32_t outSize){
  return static_cast<NativeRuntime*>(context)->formatIoValue(value,width,format,out,outSize);
 }
 static int ioReadInputService(void* context,const TCGameHandle* board,uint64_t index,uint64_t* out){
  return static_cast<NativeRuntime*>(context)->readIoInput(board,index,out);
 }
 static int ioWriteInputService(void* context,const TCGameHandle* board,uint64_t index,uint64_t value){
  return static_cast<NativeRuntime*>(context)->writeIoInput(board,index,value);
 }
 static int ioFlipInputService(void* context,const TCGameHandle* board,uint64_t index,uint64_t bit){
  return static_cast<NativeRuntime*>(context)->flipIoInput(board,index,bit);
 }
 static int ioInputWidthService(void* context,const TCGameHandle* board,uint64_t index,uint32_t* out){
  return static_cast<NativeRuntime*>(context)->readIoInputWidth(board,index,out);
 }
 static int ioWriteConstantService(void* context,const TCGameHandle* board,uint64_t index,uint64_t value){
  return static_cast<NativeRuntime*>(context)->writeIoConstant(board,index,value);
 }
 static int ioWriteConstantSlotService(void* context,uint64_t component,uint64_t value){
  return static_cast<NativeRuntime*>(context)->writeIoConstantSlot(component,value);
 }
 /* The game's evaluator is deliberately not called yet: it takes a Nim string
    by reference and reports a bad expression by leaving Nim's error flag set,
    which the game then treats as a propagating exception, so it needs its exact
    calling shape confirmed on a probe build first (docs/sdk/services.md,
    "IO value service").  Until then the loader's parser owns the syntax, and it
    accepts what the field accepts for these values. */
 int evaluateIoExpression(const char* expression,uint64_t* out){
  if(!expression||!out)return TC_IO_VALUE_ERR_ARGUMENT;
  return tc::expr::evaluate(expression,out);
 }
 int formatIoValue(uint64_t value,uint32_t width,uint32_t format,char* out,uint32_t outSize){
  if(!out||outSize<2)return TC_IO_VALUE_ERR_ARGUMENT;
  if(width<1||width>64)return TC_IO_VALUE_ERR_RANGE;
  const uint64_t masked=tc::expr::truncate(value,width);
  char text[80];std::size_t length=0;
  if(format==TC_IO_VALUE_FORMAT_BINARY){
   text[length++]='0';text[length++]='b';
   for(uint32_t bit=width;bit-- >0;)text[length++]=static_cast<char>('0'+((masked>>bit)&1u));
  }else if(format==TC_IO_VALUE_FORMAT_HEX){
   static const char digits[]="0123456789ABCDEF";
   text[length++]='0';text[length++]='x';
   for(uint32_t nibble=(width+3)/4;nibble-- >0;)text[length++]=digits[(masked>>(nibble*4))&0xfu];
  }else{
   int64_t signedValue=static_cast<int64_t>(masked);
   if(format==TC_IO_VALUE_FORMAT_SIGNED&&width<64&&(masked&(1ull<<(width-1))))
    signedValue-=static_cast<int64_t>(1ull<<width);
   if(signedValue<0){text[length++]='-';appendDecimal(text,length,static_cast<uint64_t>(-signedValue));}
   else appendDecimal(text,length,static_cast<uint64_t>(signedValue));
  }
  const uint32_t keep=outSize-1;
  const uint32_t copied=static_cast<uint32_t>(length)<keep?static_cast<uint32_t>(length):keep;
  std::memcpy(out,text,copied);out[copied]='\0';
  return copied==length?TC_IO_VALUE_OK:TC_IO_VALUE_ERR_SIZE;
 }
 int readIoInput(const TCGameHandle* board,uint64_t index,uint64_t* out){
  if(!board||!out)return TC_IO_VALUE_ERR_ARGUMENT;
  const int thread=ioThreadCheck();
  if(thread!=TC_IO_VALUE_OK)return thread;
  const unsigned char* record=nullptr;
  if(!boardComponentRecord(board,index,&record,nullptr))return TC_IO_VALUE_ERR_RANGE;
  auto reader=ioAlias(aliases,"io.input.get");
  if(!reader)return TC_IO_VALUE_ERR_UNAVAILABLE;
  const void* raw=nullptr;
  if(gameHandles.resolve(board,&raw)!=TC_HANDLE_OK||!raw)return TC_IO_VALUE_ERR_STATE;
  using Fn=int64_t(*)(void*,int64_t);
  *out=static_cast<uint64_t>(reinterpret_cast<Fn>(reader)(const_cast<void*>(raw),
                                                          static_cast<int64_t>(index)));
  return TC_IO_VALUE_OK;
 }
 int writeIoInput(const TCGameHandle* board,uint64_t index,uint64_t value){
  if(!board)return TC_IO_VALUE_ERR_ARGUMENT;
  const int thread=ioThreadCheck();
  if(thread!=TC_IO_VALUE_OK)return thread;
  const unsigned char* record=nullptr;
  if(!boardComponentRecord(board,index,&record,nullptr))return TC_IO_VALUE_ERR_RANGE;
  auto writer=ioAlias(aliases,"io.input.set");
  if(!writer)return TC_IO_VALUE_ERR_UNAVAILABLE;
  const void* raw=nullptr;
  if(gameHandles.resolve(board,&raw)!=TC_HANDLE_OK||!raw)return TC_IO_VALUE_ERR_STATE;
  const uint64_t truncated=tc::expr::truncate(value,ioRecordWidth(record));
  using Fn=void(*)(void*,int64_t,int64_t);
  reinterpret_cast<Fn>(writer)(const_cast<void*>(raw),static_cast<int64_t>(index),
                               static_cast<int64_t>(truncated));
  return TC_IO_VALUE_OK;
 }
 int flipIoInput(const TCGameHandle* board,uint64_t index,uint64_t bit){
  if(!board)return TC_IO_VALUE_ERR_ARGUMENT;
  const int thread=ioThreadCheck();
  if(thread!=TC_IO_VALUE_OK)return thread;
  const unsigned char* record=nullptr;
  if(!boardComponentRecord(board,index,&record,nullptr))return TC_IO_VALUE_ERR_RANGE;
  if(bit>=ioRecordWidth(record))return TC_IO_VALUE_ERR_RANGE;
  auto flip=ioAlias(aliases,"io.input.flip");
  if(!flip)return TC_IO_VALUE_ERR_UNAVAILABLE;
  const void* raw=nullptr;
  if(gameHandles.resolve(board,&raw)!=TC_HANDLE_OK||!raw)return TC_IO_VALUE_ERR_STATE;
  using Fn=void(*)(void*,int64_t,int64_t);
  reinterpret_cast<Fn>(flip)(const_cast<void*>(raw),static_cast<int64_t>(index),
                             static_cast<int64_t>(bit));
  return TC_IO_VALUE_OK;
 }
 int readIoInputWidth(const TCGameHandle* board,uint64_t index,uint32_t* out){
  if(!board||!out)return TC_IO_VALUE_ERR_ARGUMENT;
  const unsigned char* record=nullptr;
  if(!boardComponentRecord(board,index,&record,nullptr))return TC_IO_VALUE_ERR_RANGE;
  *out=ioRecordWidth(record);
  return TC_IO_VALUE_OK;
 }
 int writeIoConstant(const TCGameHandle* board,uint64_t index,uint64_t value){
  if(!board)return TC_IO_VALUE_ERR_ARGUMENT;
  const int thread=ioThreadCheck();
  if(thread!=TC_IO_VALUE_OK)return thread;
  const unsigned char* record=nullptr;
  if(!boardComponentRecord(board,index,&record,nullptr))return TC_IO_VALUE_ERR_RANGE;
  if(record[kIoKindOffset]!=kIoConstantKind)return TC_IO_VALUE_ERR_STATE;
  auto writer=ioAlias(aliases,"io.constant.set");
  if(!writer)return TC_IO_VALUE_ERR_UNAVAILABLE;
  const void* raw=nullptr;
  if(gameHandles.resolve(board,&raw)!=TC_HANDLE_OK||!raw)return TC_IO_VALUE_ERR_STATE;
  const uint64_t truncated=tc::expr::truncate(value,ioRecordWidth(record));
  using SetFn=void(*)(void*,int64_t,int64_t,int64_t);
  reinterpret_cast<SetFn>(writer)(const_cast<void*>(raw),0,static_cast<int64_t>(index),
                                  static_cast<int64_t>(truncated));
  uint64_t componentId=0;
  std::memcpy(&componentId,record+kIoIdOffset,sizeof(componentId));
  tc::logic::setDynamicConstant(componentId,truncated);
  if(void* refresh=ioAlias(aliases,"io.constant.refresh")){
   using RefreshFn=void(*)(void*);
   reinterpret_cast<RefreshFn>(refresh)(const_cast<void*>(raw));
  }
  return TC_IO_VALUE_OK;
 }
 int writeIoConstantSlot(uint64_t component,uint64_t value){
  tc::logic::setDynamicConstant(component,value);
  return TC_IO_VALUE_OK;
 }
 /* The component's own kind (or custom id) names its prototype; the host owns
    that snapshot for the duration of this call and copies the pin values into
    the caller's buffer, so no borrowed prototype pointer escapes. */
 int readBoardComponentPins(const TCGameHandle* handle,TCComponentPinsV1* out,uint32_t outSize,const TCComponentPinBuffersV1* buffers){
  if(!handle||!out||!buffers)return TC_SNAPSHOT_ERR_ARGUMENT;
  if(outSize<sizeof(TCComponentPinsV1)||buffers->size<sizeof(TCComponentPinBuffersV1))return TC_SNAPSHOT_ERR_SIZE;
  if(buffers->version!=TC_COMPONENT_PINS_VERSION_1)return TC_SNAPSHOT_ERR_ARGUMENT;
  if(buffers->pin_capacity&&!buffers->pins)return TC_SNAPSHOT_ERR_ARGUMENT;
  board_objects::Arrays arrays{};const void* record=nullptr;int64_t frame=-1;
  const int status=resolveBoardObject(handle,TC_GAME_OBJECT_COMPONENT,arrays,&record,frame);
  if(status!=TC_SNAPSHOT_OK)return status;
  const auto* bytes=static_cast<const unsigned char*>(record);
  TCComponentPinsV1 pins{};
  pins.size=sizeof(pins);pins.version=TC_COMPONENT_PINS_VERSION_1;
  pins.component=*handle;pins.kind=board_objects::readU8(bytes,0);
  TCPrototype prototype{};
  bool owned=false;
  if(pins.kind==board_objects::kCustomComponentKind){
   pins.custom_prototype_id=board_objects::readU64(bytes,0x188);
   if(customPrototypeGet&&pins.custom_prototype_id){customPrototypeGet(pins.custom_prototype_id,&prototype);owned=true;}
  }else if(prototypeGet&&builtinKinds.count(static_cast<uint8_t>(pins.kind))){
   /* A kind outside the enumerated set is never passed: the game raises on
      unknown keys.  The zero-filled placeholder lands here too. */
   TCPrototypeKind key{};
   key.tag=static_cast<uint8_t>(pins.kind);
   prototypeGet(&key,&prototype);owned=true;
  }
  if(!owned){*out=pins;return TC_SNAPSHOT_ERR_UNAVAILABLE;}
  pins.flags|=TC_BOARD_PINS_HAS_PROTOTYPE;
  pins.input_count=prototypeInputCount(prototype);
  pins.output_count=prototypeOutputCount(prototype);
  const uint64_t total=pins.input_count+pins.output_count;
  const bool enough=buffers->pin_capacity>=total;
  if(enough){
   uint64_t written=0;
   for(uint64_t i=0;i<pins.input_count;++i){
    const TCPin* pin=prototypeInputPin(prototype,i);
    if(!pin)break;
    buffers->pins[written++]=board_pins::decodePin(*pin,TC_PIN_INPUT);
   }
   for(uint64_t i=0;i<pins.output_count;++i){
    const TCPin* pin=prototypeOutputPin(prototype,i);
    if(!pin)break;
    buffers->pins[written++]=board_pins::decodePin(*pin,TC_PIN_OUTPUT);
   }
   pins.pin_written=written;
  }
  if(prototypeDestroy)prototypeDestroy(&prototype);
  if(gameHandles.valid(handle)!=1||frame!=engineFrame())return TC_SNAPSHOT_ERR_RETRY;
  *out=pins;
  return enough?TC_SNAPSHOT_OK:TC_SNAPSHOT_ERR_CAPACITY;
 }
 static void invokeOriginalSimDo(TCHookCall* call){auto* chain=activeInstance?activeInstance->chainById(TC_HOOK_SIM_DO):nullptr;if(!chain||!chain->trampoline)return;auto* a=(TCHookSimDoArgs*)call->args;using Fn=void(*)(void*,uint8_t,int64_t);reinterpret_cast<Fn>(chain->trampoline)(a->model,(uint8_t)a->command,a->target);}
 static void invokeOriginalLevelLoad(TCHookCall* call){auto* chain=activeInstance?activeInstance->chainById(TC_HOOK_LEVEL_LOAD):nullptr;if(!chain||!chain->trampoline)return;auto* a=(TCHookLevelLoadArgs*)call->args;using Fn=void(*)(void*,const void*);reinterpret_cast<Fn>(chain->trampoline)(a->board_model,a->name);}
 /* igSetCursorPos takes ImVec2 by value, so the detour and the trampoline are
    both declared with the aggregate: two floats would arrive in the wrong
    registers (Win64 passes an eight-byte aggregate in an integer register). */
 struct HookVec2{float x,y;};
 static void invokeOriginalSetCursorPos(TCHookCall* call){auto* chain=activeInstance?activeInstance->chainById(TC_HOOK_SET_CURSOR_POS):nullptr;if(!chain||!chain->trampoline)return;auto* a=(TCHookSetCursorPosArgs*)call->args;using Fn=void(*)(HookVec2);reinterpret_cast<Fn>(chain->trampoline)(HookVec2{a->x,a->y});}
 static void invokeOriginalSetCursorPosY(TCHookCall* call){auto* chain=activeInstance?activeInstance->chainById(TC_HOOK_SET_CURSOR_POS_Y):nullptr;if(!chain||!chain->trampoline)return;auto* a=(TCHookSetCursorPosYArgs*)call->args;using Fn=void(*)(float);reinterpret_cast<Fn>(chain->trampoline)(a->y);}
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
 /* The engine's cursor placement: shared by every panel of the game, so the
    callback is told where the call came from and may change the position the
    game's own function then receives. */
 static void detourSetCursorPos(HookVec2 position){
  auto* self=activeInstance;if(!self)return;auto* chain=self->chainById(TC_HOOK_SET_CURSOR_POS);if(!chain)return;
  TCHookSetCursorPosArgs args{sizeof(TCHookSetCursorPosArgs),0u,position.x,position.y};
  TCHookCall call{};
  call.size=sizeof(TCHookCall);call.hook_id=chain->id;call.cycle=self->cycleNow();call.args=&args;
  call.run_chain=&NativeRuntime::chainRun;call.caller=__builtin_return_address(0);
  size_t cursor=0;bool originalRan=false;ChainCursor state{chain,&cursor,&originalRan};
  call.loader_state=&state;
  chainRun(&call);
 }
 static void detourSetCursorPosY(float y){
  auto* self=activeInstance;if(!self)return;auto* chain=self->chainById(TC_HOOK_SET_CURSOR_POS_Y);if(!chain)return;
  TCHookSetCursorPosYArgs args{sizeof(TCHookSetCursorPosYArgs),0u,y};
  TCHookCall call{};
  call.size=sizeof(TCHookCall);call.hook_id=chain->id;call.cycle=self->cycleNow();call.args=&args;
  call.run_chain=&NativeRuntime::chainRun;call.caller=__builtin_return_address(0);
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
  chains.push_back({this,TC_HOOK_SET_CURSOR_POS,"ig.set_cursor_pos",nullptr,nullptr,&NativeRuntime::invokeOriginalSetCursorPos,{},false});
  chains.push_back({this,TC_HOOK_SET_CURSOR_POS_Y,"ig.set_cursor_pos_y",nullptr,nullptr,&NativeRuntime::invokeOriginalSetCursorPosY,{},false});
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
 Services services;
 /* Commands are stored in the richer V2 form; a V1 submit converts into it, so
    the executor and the transaction layer only ever see one shape. */
 struct CommandRecord {Loaded* owner;TCCommandV2 command;TCCommandStatusV1 status;};
 std::map<uint64_t,CommandRecord> commands;
 std::vector<uint64_t> pendingCommands;
 uint64_t nextCommandId=0;
 static constexpr size_t kMaxPendingCommandsPerPlugin=64,kMaxCommandRecords=512;
 struct TransactionRecord {Loaded* owner;TCGameHandle board;uint32_t flags;uint64_t baselineHash;std::vector<TCCommandV2> steps;TCTransactionStatusV1 status;};
 std::map<uint64_t,TransactionRecord> transactions;
 std::vector<uint64_t> pendingTransactions;
 uint64_t nextTransactionId=0;
 static constexpr size_t kMaxOpenTransactionsPerPlugin=16,kMaxTransactionSteps=32,kMaxTransactionRecords=256;
 struct LifecycleListener {Loaded* owner;uint32_t kinds;TCGameLifecycleCallback callback;void* user;};
 struct BoardSummary {uint32_t flags=0;uint64_t components=0,wires=0,selectedComponents=0,selectedWires=0,objectHash=0,selectionHash=0;};
 struct BoardObservation {bool present=false;uint64_t generation=0;BoardSummary summary{};} observedBoard;
 std::vector<LifecycleListener> lifecycleListeners;
 uint64_t lifecycleSequence=0;
 static constexpr size_t kMaxLifecycleListeners=64;
 bool sceneHooked=false,saveHooked=false;
 bool afterPlaceHooked=false,instanceFootprintHooked=false;
 /* The context the game hands to change_scene.  The manager keeps it because
    the game's own board edits pass `context + 0x1a3b8` to the presenter's
    `upgrade` when they need the board to register a new component; see the
    placement command. */
 void* sceneContext=nullptr;
 /* The presenter slot the game passes to its own state upgrade.  It is what a
    board edit has to hand back to make the board register a new component; the
    loader cannot name that pointer itself, so it records it from the game's own
    call.  See board.after_place in the symbol profile. */
 void* afterPlaceSlot=nullptr;
 void* saveOriginal=nullptr;
 void* saveSchematicOriginal=nullptr;
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
 /* The loader's link on sim.do does three jobs, all of them things a Mod cannot
    do for itself without stepping on the others:

      * remembers the simulation model, which is the only way a service call
        (make a run request) can name the simulation the game is running;
      * applies the slice from TC_SERVICE_SIMULATION V2: a caller that wants one
        sample per cycle sets slice = 1 and re-issues, and this shortens the
        target instead of letting the game run past the sample point;
      * dispatches the SIM_COMMAND event, as it always did.

    It runs first in the chain (kLoaderLinkPriority), so both the clamp and the
    event happen before any Mod's link sees the call. */
 static int eventSimDoLink(TCHookCall* call){
 auto* args=hook::simDoArgs(call);auto* self=activeInstance;
 if(!args)return 0;
 if(args->command==0&&self){
   const int64_t current=self->cycleNow();
   const int64_t requested=args->target;
   bool clamped=false;
   const int64_t effective=tc::sim_control::clampTarget(
       requested,current,tc::sim_control::store().slice(),&clamped);
   if(effective!=requested)args->target=effective;
   tc::sim_control::store().noteSimDo(args->model,requested,effective,current,clamped);
  }
  if(self)self->dispatchEvent(TC_EVENT_SIM_COMMAND,args->command,nullptr,nullptr);
  return 0;
 }
 static int eventLevelLoadLink(TCHookCall* call){
  auto* args=hook::levelLoadArgs(call);auto* self=activeInstance;
  if(self&&args){
   /* A Constant can request a paused-board evaluation before the player has
      pressed Run.  level.load's first argument is the same model sim.do uses,
      so make simulation control ready at board entry instead of waiting for
      the first sim.do call. */
   tc::sim_control::store().remember(args->board_model);
   TCGameHandle old{};BoardSummary oldSummary{};const bool replaced=self->gameHandles.current(TC_GAME_OBJECT_BOARD,&old)==TC_HANDLE_OK;
   if(replaced)self->readBoardSummary(old,oldSummary);
   self->gameHandles.enterBoard(args->board_model,self->engineFrame());self->observedBoard={};
   self->instanceFootprints.clear();
   /* A new board means the old instances are gone: their callbacks would never
      be reached again, so release them (and fire on_destroy) before anything of
      the new board is bound. */
   tc::logic::releaseInstances(nullptr,"board left");
   /* A new board means the kept configuration images belong to records that are
      gone: the undo entry must not try to write them back. */
   tc::logic::clearConfigEdits();
   /* A new board cannot be the target of a mark left over from the old one. */
   tc::logic::clearPendingClones();
   if(replaced)self->dispatchLifecycle(TC_LIFECYCLE_BOARD_LEFT,old,oldSummary);
   TCGameHandle current{};BoardSummary currentSummary{};
   if(self->gameHandles.current(TC_GAME_OBJECT_BOARD,&current)==TC_HANDLE_OK){self->readBoardSummary(current,currentSummary);self->dispatchLifecycle(TC_LIFECYCLE_BOARD_ENTERED,current,currentSummary);}
   const auto* nimName=static_cast<const TCNimString*>(args->name);
   const char* levelName=nimName&&nimName->data
       ? static_cast<const char*>(nimName->data)+8
       : nullptr;
   self->currentLevelName=levelName?levelName:"";
   self->logger(std::string("Level loaded: ")+(levelName?levelName:"(unnamed)"));
   self->dispatchEvent(TC_EVENT_LEVEL_LOAD,0,args->board_model,levelName);
   /* Everything the load reads from here on is watched: a component whose owner
      is not registered is gone by the time this call returns, and the capture is
      the only place its record can still be seen. */
   self->beginMissingModCapture(levelName);
  }
  return 0;
 }
 static void detourSceneChange(void* context,int scene){
  auto* self=activeInstance;
  if(self){
   /* Remembered so a board edit can replay the presenter upgrade the game's own
      placement performs (see the placement command). */
   const bool firstContext=self->sceneContext!=context;
   self->sceneContext=context;
   if(firstContext){
    auto found=self->aliases.find("board.after_place");
    const uintptr_t upgrade=found!=self->aliases.end()?reinterpret_cast<uintptr_t>(found->second):0;
    self->logger("Scene context 0x"+[&]{char text[32];std::snprintf(text,sizeof(text),"%llx",(unsigned long long)(uintptr_t)context);return std::string(text);}()+
                 "; presenter slot would be 0x"+[&]{char text[32];std::snprintf(text,sizeof(text),"%llx",(unsigned long long)((uintptr_t)context+0x1a3b8));return std::string(text);}()+
                 "; upgrade=0x"+[&]{char text[32];std::snprintf(text,sizeof(text),"%llx",(unsigned long long)upgrade);return std::string(text);}());
   }
   TCGameHandle leaving{};BoardSummary leavingSummary{};const bool hadBoard=self->gameHandles.current(TC_GAME_OBJECT_BOARD,&leaving)==TC_HANDLE_OK;
   if(hadBoard)self->readBoardSummary(leaving,leavingSummary);
   /* A switch in the frame that loaded the level is the entry to the board
      scene, not a leave: killing the handle there would empty the registry for
      the whole level (the pinned build loads the level and switches the scene in
      one frame).  The handle state is settled before the event is raised, so a
      listener sees the same answer a later frame would. */
   const bool left=self->gameHandles.leaveBoardOnSceneChange(self->engineFrame());
   if(!left)
    self->logger("Game handles: scene "+std::to_string(scene)+" in the level's own frame; the Board handle stays valid");
   else {
   self->observedBoard={};
   self->instanceFootprints.clear();
   self->defaultDrawingSuppressedInstances.clear();
    self->hintSuppressedIndices.clear();
    self->hintSuppressedFrame=-1;
    self->hintSuppressedLoggedCount=-1;
    self->hintBoardArraysValid=false;
    self->hiddenFoundryIndices.clear();
    self->hiddenFoundryFrame=-1;
    self->hiddenFoundryLoggedCount=-1;
    /* Leaving the board scene releases every component instance: their
       callbacks cannot run again until a board exists. */
    tc::logic::releaseInstances(nullptr,"scene change");
    tc::sim_control::store().forget();
    if(hadBoard)self->dispatchLifecycle(TC_LIFECYCLE_BOARD_LEFT,leaving,leavingSummary);
   }
   /* subject: the context the game itself passes to change_scene.  It is what a
      mod (or a driver) has to hand back to change the scene again, and it was
      only reachable by hooking this very function before the loader took it
      over - the board-panel playtest lost its scene step to exactly that. */
   self->dispatchEvent(TC_EVENT_SCENE_CHANGE,static_cast<uint32_t>(scene),context,nullptr);
  }
 if(self&&self->sceneOriginal)reinterpret_cast<void(*)(void*,int)>(self->sceneOriginal)(context,scene);
}
 /* The pinned build's get_component_id(board, packedPoint) normally reads the
    point -> component table maintained by the presenter.  That table only knows
    a type's static geometry.  Geometry V2 adds a per-instance rectangle, so
    consult those rectangles first and return the same sequence index the game
    stores in its table.  The rest of the pointer path remains entirely native:
    selection, dragging, deletion and the component panel all receive the game's
    normal component id. */
 using GetComponentId=int64_t(*)(void*,int32_t);
 GetComponentId getComponentIdOriginal=nullptr;
 static int64_t detourGetComponentId(void* board,int32_t packedPoint){
  auto* self=activeInstance;
  if(self&&board&&!self->instanceFootprints.empty()){
   board_objects::Arrays arrays{};
   if(board_objects::readArrays(board,arrays)&&arrays.componentData){
    const int16_t pointX=static_cast<int16_t>(packedPoint&0xffff);
    const int16_t pointY=static_cast<int16_t>((static_cast<uint32_t>(packedPoint)>>16)&0xffff);
    /* Later sequence entries are drawn above earlier ones, so overlapping live
       rectangles choose the same intuitive topmost instance. */
    for(uint64_t remaining=arrays.components;remaining;--remaining){
     const uint64_t index=remaining-1;
     const unsigned char* record=arrays.componentData+board_objects::kRecordHeader+
                                 index*board_objects::kComponentStride;
     if(board_objects::readU8(record,0)!=board_objects::kCustomComponentKind)continue;
     const uint64_t instanceId=board_objects::readU64(record,8);
     auto found=self->instanceFootprints.find(instanceId);
     if(found==self->instanceFootprints.end())continue;
     const auto& footprint=found->second;
     if(!footprint.owner||!footprint.owner->active||
        footprint.customId!=board_objects::readU64(record,0x188))continue;
     const float centreX=static_cast<float>(board_objects::readI16(record,2));
     const float centreY=static_cast<float>(board_objects::readI16(record,4));
     if(static_cast<float>(pointX)>=centreX-footprint.halfWidth&&
        static_cast<float>(pointX)<=centreX+footprint.halfWidth&&
        static_cast<float>(pointY)>=centreY-footprint.halfHeight&&
        static_cast<float>(pointY)<=centreY+footprint.halfHeight)
      return static_cast<int64_t>(index);
    }
   }
  }
  return self&&self->getComponentIdOriginal?
      self->getComponentIdOriginal(board,packedPoint):-1;
 }
 void armInstanceFootprintHitTest(){
  if(instanceFootprintHooked)return;
  auto found=symbols->values.find("get_component_id__presenterZutilitiesZhelper95functions_u2052");
  if(found==symbols->values.end()||!found->second){
   logger("Component geometry V2: native point lookup is unavailable");return;
  }
  const auto status=MH_CreateHook(found->second,
      reinterpret_cast<void*>(&NativeRuntime::detourGetComponentId),
      reinterpret_cast<void**>(&getComponentIdOriginal));
  if(status!=MH_OK||MH_EnableHook(found->second)!=MH_OK){
   if(status==MH_OK)MH_RemoveHook(found->second);
   logger("Component geometry V2: native point lookup hook failed");return;
  }
  instanceFootprintHooked=true;ownedHooks.insert(found->second);
  loaderOwned[found->second]="component geometry V2 instance footprints";
  logger("Component geometry V2: native instance-footprint hit testing armed");
 }
/* Records the presenter slot the game hands to its own state upgrade, then
   forwards.  A component added through the command bus is not registered in the
   board's hit state until that upgrade runs again - the game's own placement
   does it right after add_component - and this is the only way the loader can
   name the pointer.  Evidence: docs/research/component-hitbox-path.md
   (2026-09-22, fourth round). */
 void* afterPlaceOriginal=nullptr;
 static void detourAfterPlace(void* slot,uint8_t target){
  auto* self=activeInstance;
  if(self&&slot&&!self->afterPlaceSlot){
   self->afterPlaceSlot=slot;
   char text[32];
   std::snprintf(text,sizeof(text),"0x%llx",(unsigned long long)(uintptr_t)slot);
   self->logger(std::string("Board registration: presenter slot ")+text+" learned from the game's own edit");
  }
  if(self&&self->afterPlaceOriginal)
   reinterpret_cast<void(*)(void*,uint8_t)>(self->afterPlaceOriginal)(slot,target);
 }
 void armBoardRegistration(){
  if(afterPlaceHooked)return;
  auto found=aliases.find("board.after_place");
  if(found==aliases.end()||!found->second){
   logger("Board registration: board.after_place did not resolve; a placed component stays unregistered until the board refreshes");
   return;
  }
  auto status=MH_CreateHook(found->second,reinterpret_cast<void*>(&NativeRuntime::detourAfterPlace),&afterPlaceOriginal);
  if(status!=MH_OK||MH_EnableHook(found->second)!=MH_OK){
   if(status==MH_OK)MH_RemoveHook(found->second);
   logger("Board registration: hook failed");
   return;
  }
  afterPlaceHooked=true;ownedHooks.insert(found->second);
  loaderOwned[found->second]="board registration after a placement";
  logger("Board registration: armed (a placed component is clickable without a manual edit)");
 }
static void detourSave(){
  auto* self=activeInstance;
  if(self){
   /* The save counter is a data symbol; read it if the profile resolved it, so a
      listener can tell "the fifth save" from the first. */
   uint32_t count=0;auto found=self->aliases.find("save.count");
   if(found!=self->aliases.end()&&found->second)count=static_cast<uint32_t>(*reinterpret_cast<const int64_t*>(found->second));
   self->dispatchEvent(TC_EVENT_SAVE,count,nullptr,nullptr);
   /* Definitions hear it before the game serializes: a project that keeps
      something in derived form, or that wants to repair a configuration it could
      not read, commits it here and the file being written sees it. */
   tc::logic::notifySave();
  }
  if(self&&self->saveOriginal)reinterpret_cast<void(*)()>(self->saveOriginal)();
 }
 /* The schematic writer is a second, narrower save entry: the loader's own probes
    and the game's "save this schematic" path both go through it. */
 static void detourSaveSchematic(const void* path,void* model,void* modelField,void* board,
                                 uint64_t setting){
  tc::logic::notifySave();
  auto* self=activeInstance;
  if(self&&self->saveSchematicOriginal)
   reinterpret_cast<void(*)(const void*,void*,void*,void*,uint64_t)>(self->saveSchematicOriginal)(
       path,model,modelField,board,setting);
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
  /* Always armed: the link is what remembers the simulation model (V2's run
     requests have no other way to name it) and what applies the run slice.  The
     event dispatch inside it stays free when nobody subscribes. */
  {const int status=registerOwnChainLink(TC_HOOK_SIM_DO,kLoaderLinkPriority,&eventSimDoLink);logger("Event source sim.do: "+std::string(hook::errorText(status)));logger("Simulation control sim.do: "+std::string(hook::errorText(status)));}
  /* The capture tick runs inside the compiled program, on the simulation
     thread, so it reads the state through the game's own reader instead of the
     service (which is the render thread's). */
  {
   auto found=aliases.find("sim.state.read");
   if(found!=aliases.end()&&found->second){
    tc::scope_capture::store().setStateReader(
        reinterpret_cast<uint64_t(*)(uint64_t)>(found->second));
    logger("Scope capture: per-cycle state reader armed");
   }else{
    logger("Scope capture: sim.state.read unavailable; a capture would record zeros");
   }
  }
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
 static void* chainDetour(uint32_t id){
  switch(id){
   case TC_HOOK_SIM_DO:return reinterpret_cast<void*>(&NativeRuntime::detourSimDo);
   case TC_HOOK_LEVEL_LOAD:return reinterpret_cast<void*>(&NativeRuntime::detourLevelLoad);
   case TC_HOOK_SET_CURSOR_POS:return reinterpret_cast<void*>(&NativeRuntime::detourSetCursorPos);
   case TC_HOOK_SET_CURSOR_POS_Y:return reinterpret_cast<void*>(&NativeRuntime::detourSetCursorPosY);
   default:return nullptr;
  }
 }
 void installChains(){
  for(auto& chain:chains){
   if(chain.installed||chain.links.empty())continue;
   std::stable_sort(chain.links.begin(),chain.links.end(),[](const HookLink&a,const HookLink&b){if(a.priority!=b.priority)return a.priority<b.priority;if(a.mod!=b.mod)return a.mod<b.mod;return a.order<b.order;});
   void* detour=chainDetour(chain.id);
   if(!detour){logger("Hook chain "+std::string(chain.alias)+": no detour for this point");continue;}
   auto status=MH_CreateHook(chain.target,detour,&chain.trampoline);
   if(status!=MH_OK||MH_EnableHook(chain.target)!=MH_OK){logger("Hook chain "+std::string(chain.alias)+": install failed ("+std::string(MH_StatusToString(status))+")");continue;}
   ownedHooks.insert(chain.target);chain.installed=true;
   std::string who;for(auto& link:chain.links){if(!who.empty())who+=", ";who+=link.mod+"@"+std::to_string(link.priority);}
   logger("Hook chain "+std::string(chain.alias)+" installed with "+std::to_string(chain.links.size())+" link(s): "+who);
  }
 }
 /* A rejected plugin keeps no link behind, exactly like its hooks and its UI. */
 void dropHookLinks(const std::string& id){for(auto& chain:chains)chain.links.erase(std::remove_if(chain.links.begin(),chain.links.end(),[&](const HookLink&link){return link.mod==id;}),chain.links.end());}
 static int register_logic_api(void* c,const TCLogicDefinition* d){auto& p=*(Loaded*)c;if(!p.accepting)return -1;int result=logic::add(d);if(!result){p.logicIds.push_back(d->custom_id);p.owner->noteComponentType(p,d->custom_id,nullptr,-1);}return result;}
 /* Registration is the only moment this mapping exists on the machine, so it is
    written down here: the save page has to name the Mods an *old* profile needs
    long after a Mod was uninstalled, and a circuit file only carries the 64 bit
    custom_id.  Version and digest come from the package manifest, which is what
    lets the page report "recorded v0.2.0, installed v0.1.0" instead of a bare
    id.  Failures are swallowed inside TypeRegistry::note: the cache is an
    optimisation, a Mod registration must never fail for it. */
 void noteComponentType(const Loaded& p,uint64_t id,const char* name,int schema,
                        const char* typeId=nullptr){
  ComponentType type;
  type.mod=p.id;type.name=name?name:"";type.type_id=typeId?typeId:"";
  type.schema=schema;
  for(auto& mod:core.mods)if(mod.id==p.id){type.version=mod.version;type.digest=mod.digest;break;}
  SYSTEMTIME now{};GetLocalTime(&now);
  char stamp[16]{};
  std::snprintf(stamp,sizeof(stamp),"%04d-%02d-%02d",now.wYear,now.wMonth,now.wDay);
  type.seen=stamp;
  types.note(id,type);
 }
 static int get_current_game_handle_api(void* c,uint32_t kind,TCGameHandle* out){auto& p=*(Loaded*)c;return p.owner->gameHandles.current(kind,out);}
 static int validate_game_handle_api(void* c,const TCGameHandle* handle){auto& p=*(Loaded*)c;return p.owner->gameHandles.valid(handle);}
 static int resolve_game_handle_api(void* c,const TCGameHandle* handle,const void** out){auto& p=*(Loaded*)c;return p.owner->gameHandles.resolve(handle,out);}
 static uint64_t hashMix(uint64_t hash,const void* data,size_t size){const auto* bytes=static_cast<const unsigned char*>(data);for(size_t i=0;i<size;++i){hash^=bytes[i];hash*=1099511628211ull;}return hash;}
 uint64_t hashSelectionSet(const void* set) const {
  if(!set||!boardSetLen)return 0;uint64_t capacity=0;const unsigned char* data=nullptr;
  std::memcpy(&capacity,set,sizeof(capacity));std::memcpy(&data,static_cast<const unsigned char*>(set)+8,sizeof(data));
  if(!data||capacity>1048576)return 0;uint64_t hash=1469598103934665603ull;
  for(uint64_t i=0;i<capacity;++i){const auto* bucket=data+i*0x20;uint64_t occupied=0,id=0;std::memcpy(&occupied,bucket+8,8);if(!occupied)continue;std::memcpy(&id,bucket+0x10,8);hash=hashMix(hash,&id,sizeof(id));}
  return hash;
 }
 bool readBoardSummary(const TCGameHandle& handle,BoardSummary& out){
  const void* raw=nullptr;if(gameHandles.resolve(&handle,&raw)!=TC_HANDLE_OK)return false;const auto* base=static_cast<const unsigned char*>(raw);
  const unsigned char* components=nullptr;const unsigned char* wires=nullptr;
  std::memcpy(&out.components,base+0x78,8);std::memcpy(&components,base+0x80,8);std::memcpy(&out.wires,base+0x98,8);std::memcpy(&wires,base+0xa0,8);
  if(out.components<=1000000&&out.wires<=4000000&&(!out.components||components)&&(!out.wires||wires)){
   out.flags|=TC_LIFECYCLE_HAS_OBJECT_COUNTS;uint64_t hash=1469598103934665603ull;hash=hashMix(hash,&out.components,8);hash=hashMix(hash,&out.wires,8);
   for(uint64_t i=0;i<out.components;++i){const auto* item=components+8+i*0x238;hash=hashMix(hash,item,0x20);hash=hashMix(hash,item+0x188,8);}
   for(uint64_t i=0;i<out.wires;++i){const auto* item=wires+8+i*0x68;hash=hashMix(hash,item+0x18,8);hash=hashMix(hash,item+0x30,16);}
   out.objectHash=hash;
  }
  if(boardSetLen&&selectedComponents&&selectedWires){
   out.selectedComponents=boardSetLen(selectedComponents);out.selectedWires=boardSetLen(selectedWires);
   out.selectionHash=hashSelectionSet(selectedComponents)^(hashSelectionSet(selectedWires)*0x9e3779b97f4a7c15ull);out.flags|=TC_LIFECYCLE_HAS_SELECTION_COUNTS;
  }
  return true;
 }
 void dispatchLifecycle(uint32_t kind,const TCGameHandle& board,const BoardSummary& summary){
  TCGameLifecycleEventV1 event{};event.size=sizeof(event);event.version=TC_LIFECYCLE_EVENT_VERSION_1;event.kind=kind;event.flags=summary.flags;
  event.sequence=++lifecycleSequence;event.engine_frame=engineFrame();event.simulation_cycle=cycleNow();event.board=board;
  event.component_count=summary.components;event.wire_count=summary.wires;event.selected_component_count=summary.selectedComponents;event.selected_wire_count=summary.selectedWires;
  for(auto& listener:lifecycleListeners){if(!listener.owner||!listener.owner->active||!(listener.kinds&kind))continue;event.user=listener.user;fault::Scope mark(listener.owner->id.c_str(),"a lifecycle listener");try{listener.callback(&event);}catch(...){logger("Lifecycle listener threw: "+listener.owner->id);}}
 }
 void observeBoardChanges(){
  TCGameHandle board{};if(gameHandles.current(TC_GAME_OBJECT_BOARD,&board)!=TC_HANDLE_OK){observedBoard={};return;}BoardSummary now{};if(!readBoardSummary(board,now))return;
  if(!observedBoard.present||observedBoard.generation!=board.generation){observedBoard={true,board.generation,now};return;}
  const auto before=observedBoard.summary;
  if((now.flags&TC_LIFECYCLE_HAS_OBJECT_COUNTS)&&(before.flags&TC_LIFECYCLE_HAS_OBJECT_COUNTS)&&now.objectHash!=before.objectHash)dispatchLifecycle(TC_LIFECYCLE_OBJECTS_CHANGED,board,now);
  if((now.flags&TC_LIFECYCLE_HAS_SELECTION_COUNTS)&&(before.flags&TC_LIFECYCLE_HAS_SELECTION_COUNTS)&&now.selectionHash!=before.selectionHash)dispatchLifecycle(TC_LIFECYCLE_SELECTION_CHANGED,board,now);
  observedBoard.summary=now;
 }
 static int lifecycle_subscribe_api(void* c,uint32_t kinds,TCGameLifecycleCallback callback,void* user){
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;const uint32_t all=TC_LIFECYCLE_BOARD_ENTERED|TC_LIFECYCLE_BOARD_LEFT|TC_LIFECYCLE_OBJECTS_CHANGED|TC_LIFECYCLE_SELECTION_CHANGED;
  if(!p.accepting)return TC_LIFECYCLE_ERR_UNAVAILABLE;if(!callback||!kinds||(kinds&~all))return TC_LIFECYCLE_ERR_ARGUMENT;if(r.lifecycleListeners.size()>=kMaxLifecycleListeners)return TC_LIFECYCLE_ERR_CAPACITY;
  r.lifecycleListeners.push_back(LifecycleListener{&p,kinds,callback,user});return TC_LIFECYCLE_OK;
 }
 int queryLifecycleService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;if(version!=TC_LIFECYCLE_API_VERSION_1)return TC_SERVICE_ERR_VERSION;if(outSize<sizeof(TCGameLifecycleApiV1))return TC_SERVICE_ERR_SIZE;
  *static_cast<TCGameLifecycleApiV1*>(out)=TCGameLifecycleApiV1{sizeof(TCGameLifecycleApiV1),TC_LIFECYCLE_API_VERSION_1,&p,&lifecycle_subscribe_api};return TC_SERVICE_OK;
 }
 void dropLifecycleListeners(Loaded& p){lifecycleListeners.erase(std::remove_if(lifecycleListeners.begin(),lifecycleListeners.end(),[&](const LifecycleListener& listener){return listener.owner==&p;}),lifecycleListeners.end());}
 void pruneCommands(){
  for(auto it=commands.begin();commands.size()>kMaxCommandRecords&&it!=commands.end();){
   if(it->second.status.state==TC_COMMAND_STATE_SUCCEEDED||it->second.status.state==TC_COMMAND_STATE_FAILED||it->second.status.state==TC_COMMAND_STATE_CANCELLED)it=commands.erase(it);else ++it;
  }
 }
 static int command_submit_api(void* c,const TCCommandV1* command,uint64_t* requestId){
  auto& p=*static_cast<Loaded*>(c);
  if(!command||!requestId||command->size<sizeof(TCCommandV1))return TC_COMMAND_ERR_ARGUMENT;
  TCCommandV2 converted{};
  converted.size=sizeof(converted);converted.type=command->type;converted.flags=command->flags;
  converted.reserved=command->reserved;converted.subject=command->subject;converted.argument=command->argument;
  return p.owner->command_submit_v2(p,converted,requestId);
 }
 static int command_submit_api_v2(void* c,const TCCommandV2* command,uint64_t* requestId){
  auto& p=*static_cast<Loaded*>(c);
  if(!command||!requestId||command->size<sizeof(TCCommandV2))return TC_COMMAND_ERR_ARGUMENT;
  return p.owner->command_submit_v2(p,*command,requestId);
 }
 /* Shared by both command versions: the queue, ownership and status machinery
    only ever sees the V2 shape. */
 int command_submit_v2(Loaded& p,const TCCommandV2& command,uint64_t* requestId){
  auto& r=*p.owner;
  if(!requestId)return TC_COMMAND_ERR_ARGUMENT;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_COMMAND_ERR_THREAD;
  if((!p.accepting&&!p.active)||command.flags||command.subject.size<sizeof(TCGameHandle)||command.subject.kind!=TC_GAME_OBJECT_BOARD)return TC_COMMAND_ERR_ARGUMENT;
  if(command.type<TC_COMMAND_SIM_RUN||command.type>TC_COMMAND_BOARD_DUPLICATE_COMPONENT)return TC_COMMAND_ERR_ARGUMENT;
  if(command.type==TC_COMMAND_SIM_RUN&&command.argument<0)return TC_COMMAND_ERR_ARGUMENT;
  if(command.type==TC_COMMAND_BOARD_PLACE_COMPONENT){
   /* A placement names either a built-in kind or a custom prototype, and both
      coordinates have to fit the record's 16-bit fields. */
   if(command.custom_prototype_id){if(command.kind!=board_edits::kCustomKind)return TC_COMMAND_ERR_ARGUMENT;}
   else if(!command.kind)return TC_COMMAND_ERR_ARGUMENT;
   if(command.rotation>255)return TC_COMMAND_ERR_ARGUMENT;
   if(command.x<-32768||command.x>32767||command.y<-32768||command.y>32767)return TC_COMMAND_ERR_ARGUMENT;
  }
  if(r.gameHandles.valid(&command.subject)!=1)return TC_COMMAND_ERR_STALE;
  size_t outstanding=0;for(auto& item:r.commands)if(item.second.owner==&p&&(item.second.status.state==TC_COMMAND_STATE_QUEUED||item.second.status.state==TC_COMMAND_STATE_RUNNING))++outstanding;
  if(outstanding>=kMaxPendingCommandsPerPlugin)return TC_COMMAND_ERR_CAPACITY;
  r.pruneCommands();if(r.commands.size()>=kMaxCommandRecords)return TC_COMMAND_ERR_CAPACITY;
  const uint64_t id=++r.nextCommandId;
  TCCommandStatusV1 status{sizeof(TCCommandStatusV1),1,TC_COMMAND_STATE_QUEUED,TC_COMMAND_OK,id,r.engineFrame(),-1};
  r.commands.emplace(id,CommandRecord{&p,command,status});r.pendingCommands.push_back(id);*requestId=id;
  return TC_COMMAND_OK;
 }
 static int command_status_api(void* c,uint64_t requestId,TCCommandStatusV1* out,uint32_t outSize){
  auto& p=*static_cast<Loaded*>(c);if(!out)return TC_COMMAND_ERR_ARGUMENT;if(outSize<sizeof(TCCommandStatusV1))return TC_COMMAND_ERR_ARGUMENT;
  if(!p.owner->gameThreadId||GetCurrentThreadId()!=p.owner->gameThreadId)return TC_COMMAND_ERR_THREAD;
  auto found=p.owner->commands.find(requestId);if(found==p.owner->commands.end()||found->second.owner!=&p)return TC_COMMAND_ERR_NOT_FOUND;
  *out=found->second.status;return TC_COMMAND_OK;
 }
 static int command_cancel_api(void* c,uint64_t requestId){
  auto& p=*static_cast<Loaded*>(c);if(!p.owner->gameThreadId||GetCurrentThreadId()!=p.owner->gameThreadId)return TC_COMMAND_ERR_THREAD;auto found=p.owner->commands.find(requestId);
  if(found==p.owner->commands.end()||found->second.owner!=&p)return TC_COMMAND_ERR_NOT_FOUND;
  if(found->second.status.state!=TC_COMMAND_STATE_QUEUED)return TC_COMMAND_ERR_STATE;
  found->second.status.state=TC_COMMAND_STATE_CANCELLED;found->second.status.result=TC_COMMAND_OK;found->second.status.completed_frame=p.owner->engineFrame();return TC_COMMAND_OK;
 }
 int queryCommandService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;
  if(version==TC_COMMAND_API_VERSION_1){
   if(outSize<sizeof(TCCommandApiV1))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCCommandApiV1*>(out)=TCCommandApiV1{sizeof(TCCommandApiV1),TC_COMMAND_API_VERSION_1,&p,&command_submit_api,&command_status_api,&command_cancel_api};
   return TC_SERVICE_OK;
  }
  if(version==TC_COMMAND_API_VERSION_2){
   if(outSize<sizeof(TCCommandApiV2))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCCommandApiV2*>(out)=TCCommandApiV2{sizeof(TCCommandApiV2),TC_COMMAND_API_VERSION_2,&p,&command_submit_api_v2,&command_status_api,&command_cancel_api};
   return TC_SERVICE_OK;
  }
  return TC_SERVICE_ERR_VERSION;
 }
 static int query_service_api(void* c,const char* id,uint32_t version,void* out,uint32_t outSize){
 auto& p=*(Loaded*)c;if(!id)return TC_SERVICE_ERR_ARGUMENT;
 if(std::strcmp(id,TC_SERVICE_COMMANDS)==0)return p.owner->queryCommandService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_LIFECYCLE)==0)return p.owner->queryLifecycleService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_TRANSACTIONS)==0)return p.owner->queryTransactionService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_COMPONENT_TYPES)==0)return p.owner->queryComponentTypesService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_COMPONENT_GEOMETRY)==0)return p.owner->queryComponentGeometryService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_COMPONENT_RENDER)==0)return p.owner->queryComponentRenderService(p,version,out,outSize);
 return p.owner->services.query(id,version,out,outSize);
 }
 void dropCommands(Loaded& p){for(auto& item:commands)if(item.second.owner==&p&&item.second.status.state==TC_COMMAND_STATE_QUEUED){item.second.status.state=TC_COMMAND_STATE_CANCELLED;item.second.status.result=TC_COMMAND_ERR_UNAVAILABLE;item.second.status.completed_frame=engineFrame();}}
 static bool sameHandle(const TCGameHandle& a,const TCGameHandle& b){return a.kind==b.kind&&a.generation==b.generation&&a.token==b.token;}
 void pruneTransactions(){for(auto it=transactions.begin();transactions.size()>=kMaxTransactionRecords&&it!=transactions.end();){const auto state=it->second.status.state;if(state==TC_TRANSACTION_STATE_COMMITTED||state==TC_TRANSACTION_STATE_FAILED||state==TC_TRANSACTION_STATE_ABORTED||state==TC_TRANSACTION_STATE_CONFLICT)it=transactions.erase(it);else ++it;}}
 static int transaction_begin_api(void* c,const TCGameHandle* board,uint32_t flags,uint64_t* transactionId){
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;if(!board||!transactionId||(flags&~TC_TRANSACTION_SAVE_ON_COMMIT))return TC_TRANSACTION_ERR_ARGUMENT;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_TRANSACTION_ERR_THREAD;if((!p.accepting&&!p.active)||r.gameHandles.valid(board)!=1)return TC_TRANSACTION_ERR_STALE;
  size_t open=0;for(auto& item:r.transactions)if(item.second.owner==&p&&(item.second.status.state==TC_TRANSACTION_STATE_OPEN||item.second.status.state==TC_TRANSACTION_STATE_QUEUED||item.second.status.state==TC_TRANSACTION_STATE_RUNNING))++open;
  if(open>=kMaxOpenTransactionsPerPlugin)return TC_TRANSACTION_ERR_CAPACITY;r.pruneTransactions();if(r.transactions.size()>=kMaxTransactionRecords)return TC_TRANSACTION_ERR_CAPACITY;
  BoardSummary summary{};if(!r.readBoardSummary(*board,summary)||(summary.flags&TC_LIFECYCLE_HAS_OBJECT_COUNTS)==0)return TC_TRANSACTION_ERR_UNAVAILABLE;
  const uint64_t id=++r.nextTransactionId;TCTransactionStatusV1 status{sizeof(status),TC_TRANSACTION_STATUS_VERSION_1,TC_TRANSACTION_STATE_OPEN,TC_TRANSACTION_OK,id,0,0,-1,-1};
  r.transactions.emplace(id,TransactionRecord{&p,*board,flags,summary.objectHash,{},status});*transactionId=id;return TC_TRANSACTION_OK;
 }
 static int transaction_stage_api(void* c,uint64_t transactionId,const TCCommandV1* command){
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_TRANSACTION_ERR_THREAD;
  auto found=r.transactions.find(transactionId);if(found==r.transactions.end()||found->second.owner!=&p)return TC_TRANSACTION_ERR_NOT_FOUND;auto& tx=found->second;
  if(tx.status.state!=TC_TRANSACTION_STATE_OPEN)return TC_TRANSACTION_ERR_STATE;
  if(!command||command->size<sizeof(TCCommandV1))return TC_TRANSACTION_ERR_ARGUMENT;
  TCCommandV2 converted{};
  converted.size=sizeof(converted);converted.type=command->type;converted.flags=command->flags;
  converted.reserved=command->reserved;converted.subject=command->subject;converted.argument=command->argument;
  return r.stageTransaction(tx,converted);
 }
 static int transaction_stage_api_v2(void* c,uint64_t transactionId,const TCCommandV2* command){
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_TRANSACTION_ERR_THREAD;
  auto found=r.transactions.find(transactionId);if(found==r.transactions.end()||found->second.owner!=&p)return TC_TRANSACTION_ERR_NOT_FOUND;auto& tx=found->second;
  if(tx.status.state!=TC_TRANSACTION_STATE_OPEN)return TC_TRANSACTION_ERR_STATE;
  if(!command||command->size<sizeof(TCCommandV2))return TC_TRANSACTION_ERR_ARGUMENT;
  return r.stageTransaction(tx,*command);
 }
 /* Validates one staged step the same way a direct submit does, so a transaction
    cannot smuggle in a command the bus would have refused. */
 int stageTransaction(TransactionRecord& tx,const TCCommandV2& command){
  if(command.flags||command.type<TC_COMMAND_SIM_RUN||!sameHandle(command.subject,tx.board))return TC_TRANSACTION_ERR_ARGUMENT;
  if(command.type==TC_COMMAND_SIM_RUN&&command.argument<0)return TC_TRANSACTION_ERR_ARGUMENT;
  if(command.type==TC_COMMAND_BOARD_PLACE_COMPONENT){
   if(command.custom_prototype_id){if(command.kind!=board_edits::kCustomKind)return TC_TRANSACTION_ERR_ARGUMENT;}
   else if(!command.kind)return TC_TRANSACTION_ERR_ARGUMENT;
   if(command.rotation>255||command.x<-32768||command.x>32767||command.y<-32768||command.y>32767)return TC_TRANSACTION_ERR_ARGUMENT;
  }
  if(tx.steps.size()>=kMaxTransactionSteps)return TC_TRANSACTION_ERR_CAPACITY;
  tx.steps.push_back(command);tx.status.staged_count=tx.steps.size();return TC_TRANSACTION_OK;
 }
 static int transaction_commit_api(void* c,uint64_t transactionId){
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_TRANSACTION_ERR_THREAD;
  auto found=r.transactions.find(transactionId);if(found==r.transactions.end()||found->second.owner!=&p)return TC_TRANSACTION_ERR_NOT_FOUND;auto& tx=found->second;
  if(tx.status.state!=TC_TRANSACTION_STATE_OPEN)return TC_TRANSACTION_ERR_STATE;if(tx.steps.empty()&&!(tx.flags&TC_TRANSACTION_SAVE_ON_COMMIT))return TC_TRANSACTION_ERR_ARGUMENT;
  tx.status.state=TC_TRANSACTION_STATE_QUEUED;tx.status.staged_count=tx.steps.size()+((tx.flags&TC_TRANSACTION_SAVE_ON_COMMIT)?1:0);tx.status.submitted_frame=r.engineFrame();r.pendingTransactions.push_back(transactionId);return TC_TRANSACTION_OK;
 }
 static int transaction_abort_api(void* c,uint64_t transactionId){
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_TRANSACTION_ERR_THREAD;
  auto found=r.transactions.find(transactionId);if(found==r.transactions.end()||found->second.owner!=&p)return TC_TRANSACTION_ERR_NOT_FOUND;auto& status=found->second.status;
  if(status.state!=TC_TRANSACTION_STATE_OPEN&&status.state!=TC_TRANSACTION_STATE_QUEUED)return TC_TRANSACTION_ERR_STATE;status.state=TC_TRANSACTION_STATE_ABORTED;status.completed_frame=r.engineFrame();return TC_TRANSACTION_OK;
 }
 static int transaction_status_api(void* c,uint64_t transactionId,TCTransactionStatusV1* out,uint32_t outSize){
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;if(!out||outSize<sizeof(TCTransactionStatusV1))return TC_TRANSACTION_ERR_ARGUMENT;if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_TRANSACTION_ERR_THREAD;
  auto found=r.transactions.find(transactionId);if(found==r.transactions.end()||found->second.owner!=&p)return TC_TRANSACTION_ERR_NOT_FOUND;*out=found->second.status;return TC_TRANSACTION_OK;
 }
/* tc.component.types V1: the V2 definition entry point.  The context handed to
   the plugin is its own Loaded record, because a registration needs that
   plugin's host (mod id, data directory) - same rule as the command bus. */
int queryComponentTypesService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;
  if(version!=TC_COMPONENT_TYPES_API_VERSION_1)return TC_SERVICE_ERR_VERSION;
  if(outSize<sizeof(TCComponentTypesApiV1))return TC_SERVICE_ERR_SIZE;
  *static_cast<TCComponentTypesApiV1*>(out)=TCComponentTypesApiV1{
      sizeof(TCComponentTypesApiV1),TC_COMPONENT_TYPES_API_VERSION_1,&p,
      &component_types_register_api};
  return TC_SERVICE_OK;
}
/* The three checks every geometry declaration shares: the caller has to be the
   game thread, inside its tc_mod_load, and the type has to be its own. */
int componentFootprintAllowed(const Loaded& p,uint64_t customId){
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return TC_COMPONENT_GEOMETRY_ERR_THREAD;
  if(!p.accepting)return TC_COMPONENT_GEOMETRY_ERR_UNAVAILABLE;
  if(!customId)return TC_COMPONENT_GEOMETRY_ERR_ARGUMENT;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())
   return TC_COMPONENT_GEOMETRY_ERR_OWNERSHIP;
  return TC_COMPONENT_GEOMETRY_OK;
}
/* Stores one rectangle in the type's prototype.  Both the half-extent form and
   the cell form come through here: the game keeps whole cells either way, and
   that rectangle is what it hit-tests, drags by and reserves. */
int writeComponentFootprint(uint64_t customId,const component_geometry::Rectangle& rectangle){
  if(!customPrototypeGet||!customPrototypeSet||!prototypeDestroy||!sequenceDestroy||!sequenceNew)
   return TC_COMPONENT_GEOMETRY_ERR_UNAVAILABLE;
  TCPrototype prototype{};
  try {
   customPrototypeGet(customId,&prototype);
   void* sequence=prototype.bytes+component_geometry::kShapeSequenceOffset;
   sequenceDestroy(sequence);std::memset(sequence,0,16);sequenceNew(sequence,1);
   unsigned char* storage=nullptr;
   std::memcpy(&storage,prototype.bytes+component_geometry::kShapeStorageOffset,sizeof(storage));
   if(!storage){prototypeDestroy(&prototype);return TC_COMPONENT_GEOMETRY_ERR_GAME;}
   const uint64_t packed=component_geometry::pack(rectangle);
   std::memcpy(storage+8,&packed,sizeof(packed));
   std::memcpy(prototype.bytes+component_geometry::kShapeRadiusOffset,&rectangle.radius,sizeof(rectangle.radius));
  customPrototypeSet(customId,&prototype);prototypeDestroy(&prototype);
  /* A footprint that reaches past the pin lane takes the pins' click and drag
     area with it (see component_geometry::footprintReachesPins).  Warn instead
     of refusing: a decorative type has no pins, and a Mod may have a reason. */
  {
   component_registry::Type declared{};
   const bool known=component_registry::find(customId,&declared);
   const bool hasPins=known&&(!declared.inputs.empty()||!declared.outputs.empty());
   /* The lane this type really has: a declarative type may have asked for a
      wider one than the default (TCComponentTypeDefinitionV2::pin_lane), and
      then a stock-sized body is legal instead of a warning. */
   const float pinLane=known?declared.pin_lane:component_geometry::kPinLaneX;
   const float effectiveHalfWidth=static_cast<float>(rectangle.width)*0.5f;
   if(component_geometry::footprintReachesPins(effectiveHalfWidth,hasPins,pinLane)) {
    char text[360];
    std::snprintf(text,sizeof(text),
     "Component geometry: the footprint declared for 0x%llx is %g cells wide (half %g), "
     "which reaches past the pin lane at %g.  This component now shares the pins' click "
     "and drag area, so a wire can no longer be started from a pin; keep the half-width "
     "at the lane or below for a type with pins (docs/sdk/services.md, footprint bounds)",
     static_cast<unsigned long long>(customId),
     static_cast<double>(effectiveHalfWidth)*2.0,
     static_cast<double>(effectiveHalfWidth),
     static_cast<double>(pinLane));
    logger(text);
   }
  }
  return TC_COMPONENT_GEOMETRY_OK;
  } catch(...) {prototypeDestroy(&prototype);return TC_COMPONENT_GEOMETRY_ERR_GAME;}
 }
int setComponentFootprint(Loaded& p,uint64_t customId,float halfWidth,float halfHeight){
  const int allowed=componentFootprintAllowed(p,customId);
  if(allowed!=TC_COMPONENT_GEOMETRY_OK)return allowed;
  component_geometry::Rectangle rectangle{};
  if(!component_geometry::fromHalfExtents(halfWidth,halfHeight,rectangle))
   return TC_COMPONENT_GEOMETRY_ERR_RANGE;
  return writeComponentFootprint(customId,rectangle);
 }
/* The cell form (geometry V3): the rectangle is stored as asked, so a face that
   is not centred on the component's own cell keeps its real top row instead of
   a half height the host rounds outwards.  Width and height are whole cells and
   must be positive; x/y are board cells relative to the component. */
int setComponentFootprintCells(Loaded& p,uint64_t customId,int32_t x,int32_t y,
                               uint32_t width,uint32_t height){
  const int allowed=componentFootprintAllowed(p,customId);
  if(allowed!=TC_COMPONENT_GEOMETRY_OK)return allowed;
  if(x<-32768||x>32767||y<-32768||y>32767||!width||!height||width>32767u||height>32767u)
   return TC_COMPONENT_GEOMETRY_ERR_RANGE;
  component_geometry::Rectangle rectangle{};
  rectangle.x=static_cast<int16_t>(x);
  rectangle.y=static_cast<int16_t>(y);
  rectangle.width=static_cast<uint16_t>(width);
  rectangle.height=static_cast<uint16_t>(height);
  /* The radius is the same corner figure fromHalfExtents computes: the largest
     distance from the centre to a corner, so the game can round the box. */
  const int left=std::abs(static_cast<int>(rectangle.x));
  const int right=std::abs(static_cast<int>(rectangle.x)+static_cast<int>(rectangle.width)-1);
  const int top=std::abs(static_cast<int>(rectangle.y));
  const int bottom=std::abs(static_cast<int>(rectangle.y)+static_cast<int>(rectangle.height)-1);
  const int radius=(left>right?left:right)+(top>bottom?top:bottom);
  if(radius>std::numeric_limits<uint16_t>::max())return TC_COMPONENT_GEOMETRY_ERR_RANGE;
  rectangle.radius=static_cast<uint16_t>(radius);
  return writeComponentFootprint(customId,rectangle);
 }
int setComponentInstanceFootprint(Loaded& p,const TCGameHandle* component,
                                  float halfWidth,float halfHeight){
  if(!component)return TC_COMPONENT_GEOMETRY_ERR_ARGUMENT;
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return TC_COMPONENT_GEOMETRY_ERR_THREAD;
  if((!p.accepting&&!p.active)||!instanceFootprintHooked)
   return TC_COMPONENT_GEOMETRY_ERR_UNAVAILABLE;
  component_geometry::Rectangle checked{};
  if(!component_geometry::fromHalfExtents(halfWidth,halfHeight,checked))
   return TC_COMPONENT_GEOMETRY_ERR_RANGE;
  board_objects::Arrays arrays{};const void* raw=nullptr;int64_t frame=-1;
  const int resolved=resolveBoardObject(component,TC_GAME_OBJECT_COMPONENT,arrays,&raw,frame);
  if(resolved!=TC_SNAPSHOT_OK)return TC_COMPONENT_GEOMETRY_ERR_STALE;
  const auto* record=static_cast<const unsigned char*>(raw);
  if(board_objects::readU8(record,0)!=board_objects::kCustomComponentKind)
   return TC_COMPONENT_GEOMETRY_ERR_UNKNOWN;
  const uint64_t customId=board_objects::readU64(record,0x188);
  if(!customId)return TC_COMPONENT_GEOMETRY_ERR_UNKNOWN;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())
   return TC_COMPONENT_GEOMETRY_ERR_OWNERSHIP;
  const uint64_t instanceId=board_objects::readU64(record,8);
  if(!instanceId)return TC_COMPONENT_GEOMETRY_ERR_UNKNOWN;
  if(gameHandles.valid(component)!=1||frame!=engineFrame())return TC_COMPONENT_GEOMETRY_ERR_STALE;
  instanceFootprints[instanceId]=InstanceFootprint{&p,customId,halfWidth,halfHeight};
  return TC_COMPONENT_GEOMETRY_OK;
 }
int readComponentFootprint(Loaded&,const TCGameHandle* component,float* halfWidth,float* halfHeight){
  if(!component||!halfWidth||!halfHeight)return TC_COMPONENT_GEOMETRY_ERR_ARGUMENT;
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return TC_COMPONENT_GEOMETRY_ERR_THREAD;
  if(!customPrototypeGet||!prototypeDestroy)return TC_COMPONENT_GEOMETRY_ERR_UNAVAILABLE;
  board_objects::Arrays arrays{};const void* raw=nullptr;int64_t frame=-1;
  const int resolved=resolveBoardObject(component,TC_GAME_OBJECT_COMPONENT,arrays,&raw,frame);
  if(resolved!=TC_SNAPSHOT_OK)return TC_COMPONENT_GEOMETRY_ERR_STALE;
  const auto* record=static_cast<const unsigned char*>(raw);
  if(record[0]!=board_objects::kCustomComponentKind)return TC_COMPONENT_GEOMETRY_ERR_UNKNOWN;
  const uint64_t customId=board_objects::readU64(record,0x188);
  if(!customId)return TC_COMPONENT_GEOMETRY_ERR_UNKNOWN;
  const uint64_t instanceId=board_objects::readU64(record,8);
  auto live=instanceFootprints.find(instanceId);
  if(live!=instanceFootprints.end()&&live->second.owner&&live->second.owner->active&&
     live->second.customId==customId){
   *halfWidth=live->second.halfWidth;*halfHeight=live->second.halfHeight;
   if(gameHandles.valid(component)!=1||frame!=engineFrame())return TC_COMPONENT_GEOMETRY_ERR_STALE;
   return TC_COMPONENT_GEOMETRY_OK;
  }
  TCPrototype prototype{};customPrototypeGet(customId,&prototype);
  uint64_t count=0;unsigned char* storage=nullptr;
  std::memcpy(&count,prototype.bytes+component_geometry::kShapeSequenceOffset,sizeof(count));
  std::memcpy(&storage,prototype.bytes+component_geometry::kShapeStorageOffset,sizeof(storage));
  if(count!=1||!storage){prototypeDestroy(&prototype);return TC_COMPONENT_GEOMETRY_ERR_GAME;}
  uint64_t packed=0;std::memcpy(&packed,storage+8,sizeof(packed));
  const auto rectangle=component_geometry::unpack(packed);prototypeDestroy(&prototype);
  if(!rectangle.width||!rectangle.height)return TC_COMPONENT_GEOMETRY_ERR_GAME;
  component_geometry::halfExtents(rectangle,board_objects::readU8(record,6),*halfWidth,*halfHeight);
  if(gameHandles.valid(component)!=1||frame!=engineFrame())return TC_COMPONENT_GEOMETRY_ERR_STALE;
  return TC_COMPONENT_GEOMETRY_OK;
 }
int queryComponentGeometryService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;
  if(version==TC_COMPONENT_GEOMETRY_API_VERSION_1){
   if(outSize<sizeof(TCComponentGeometryApiV1))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentGeometryApiV1*>(out)=TCComponentGeometryApiV1{
       sizeof(TCComponentGeometryApiV1),TC_COMPONENT_GEOMETRY_API_VERSION_1,&p,
       &component_geometry_set_api,&component_geometry_read_api};
   return TC_SERVICE_OK;
  }
  if(version==TC_COMPONENT_GEOMETRY_API_VERSION_2){
   if(outSize<sizeof(TCComponentGeometryApiV2))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentGeometryApiV2*>(out)=TCComponentGeometryApiV2{
       sizeof(TCComponentGeometryApiV2),TC_COMPONENT_GEOMETRY_API_VERSION_2,&p,
       &component_geometry_set_api,&component_geometry_read_api,
       &component_geometry_set_instance_api};
   return TC_SERVICE_OK;
 }
  if(version==TC_COMPONENT_GEOMETRY_API_VERSION_3){
   if(outSize<sizeof(TCComponentGeometryApiV3))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentGeometryApiV3*>(out)=TCComponentGeometryApiV3{
       sizeof(TCComponentGeometryApiV3),TC_COMPONENT_GEOMETRY_API_VERSION_3,&p,
       &component_geometry_set_api,&component_geometry_read_api,
       &component_geometry_set_instance_api,&component_geometry_set_cells_api};
   return TC_SERVICE_OK;
  }
  return TC_SERVICE_ERR_VERSION;
}
 bool defaultDrawingHidden(const void* raw) {
  if(!raw)return false;
  const auto* record=static_cast<const unsigned char*>(raw);
  if(board_objects::readU8(record,0)!=board_objects::kCustomComponentKind)return false;
  const uint64_t customId=board_objects::readU64(record,0x188);if(!customId)return false;
  const uint64_t instanceId=board_objects::readU64(record,8);
  /* The component chooser/preview uses an id-less temporary record.  V2 is a
     board-instance drawing takeover, so keep the game's chooser preview. */
  if(!instanceId)return false;
  for(const auto& plugin:loaded)
   if(plugin->active&&plugin->hiddenDefaultDrawings.count(customId)){
    if(defaultDrawingSuppressedInstances.insert(instanceId).second)
     logger("Component render: suppressed game default drawing custom="+
            std::to_string(customId)+" instance="+std::to_string(instanceId));
    return true;
   }
  return false;
 }
 bool capturePlacementPreview(const void* clipboard,const void* raw) {
  if(!raw)return false;
  const auto* record=static_cast<const unsigned char*>(raw);
  if(board_objects::readU8(record,0)!=board_objects::kCustomComponentKind)return false;
  const uint64_t customId=board_objects::readU64(record,0x188);
  if(!customId)return false;
  for(auto& plugin:loaded){
   if(!plugin->active||!plugin->placementPreviews.count(customId))continue;
   auto renderer=std::find_if(plugin->renderers.begin(),plugin->renderers.end(),
       [&](const ComponentRenderer& item){return item.customId==customId&&item.draw&&!item.failed;});
   if(renderer==plugin->renderers.end())return false;
   /* A placement that is already being previewed is only re-read here (the game
      calls this on every state change), so the log line marks the start of one
      placement, not one redraw. */
   const bool starting=!placementPreview.active;
   const int x=board_objects::readI16(record,2),y=board_objects::readI16(record,4);
   const uint8_t rotation=board_objects::readU8(record,6);
   placementPreview={plugin.get(),clipboard,customId,board_objects::readI16(record,2),
                     board_objects::readI16(record,4),rotation,true};
   if(starting&&placementPreviewStartedCount<12){
    ++placementPreviewStartedCount;
    logger("Component render: placement preview taken over for custom="+
           std::to_string(customId)+" at "+std::to_string(x)+","+std::to_string(y));
   }
   return true;
  }
  return false;
 }
static void detourRedrawComponent(void* a1,void* a2,void* a3,void* a4,uint64_t a5,
                                  const void* component,const void* a7,uint64_t a8){
  auto* self=activeInstance;
  if(self&&self->defaultDrawingHidden(component))return;
  if(self&&self->redrawComponentOriginal)
   self->redrawComponentOriginal(a1,a2,a3,a4,a5,component,a7,a8);
}
 static void detourRedrawClipboardComponent(void* a1,void* a2,void* a3,
                                            const void* component){
  auto* self=activeInstance;
  if(self&&self->capturePlacementPreview(a2,component))return;
  if(self&&self->redrawClipboardComponentOriginal)
   self->redrawClipboardComponentOriginal(a1,a2,a3,component);
 }
 static void detourHideClipboard(void* clipboard){
  auto* self=activeInstance;
  if(self)self->endPlacementPreview("hide_clipboard");
  if(self&&self->hideClipboardOriginal)self->hideClipboardOriginal(clipboard);
 }
 static void detourUpdateStateClipboard(void* a1,void* a2,void* a3,void* a4,void* a5){
  auto* self=activeInstance;
  if(self)self->observeClipboardRecord(a3);
  if(self&&self->updateStateClipboardOriginal)
   self->updateStateClipboardOriginal(a1,a2,a3,a4,a5);
 }
 /* The game's selection ring - the white broken arc a selected element shows - is
    drawn by redraw_selection(presenter).  It clears the selection mesh and then
    walks the presenter's selection container, which holds two Nim hash sets: the
    selected components at +0x00 and the selected wires at +0x18.  Each set is a
    capacity at +0x00 plus a bucket array pointer at +0x08, one 0x20 byte bucket
    per slot, and the drawing loop skips a bucket whose 64 bit word at +0x08 is
    zero:

      bucket +0x08  occupancy/hash   (what the loop tests)
      bucket +0x10  the element id   (a component sequence index for this set)
      bucket +0x18  the ring's scale

    One occupied bucket adds one instance to the selection mesh, built from
    init_transform_2d(bucket index) plus toPacked_transform_2d_same_scale_no_rotation.
    "SameScaleNoRotation" is why the ring cannot follow the footprint: its scale
    comes from the level tree's fixed context, so a Mod component always gets the
    same circle whatever rectangle it declared (rewriting the transform could only
    move a circle, never turn it into an 8x4 rectangle).

    Clearing the occupancy word of one bucket removes exactly that element's ring:
    the loop adds no instance for it at all, so no transform has to be faked, every
    other element keeps its own ring, and the word is restored the moment the
    original returns - nothing outside the draw call ever sees the patched bucket.
    The id space is the board's component sequence, the same one the host walks in
    dispatchComponentRender() and the one tc_board_model.h keys the selection with.
    Ground truth: the ids in this set answer "yes" to the game's own
    contains(selected_components, id) and "no" to contains(selected_wires, id),
    which is how the set (and the bucket layout) was identified on the real game.
    Evidence: build/exe-disasm.txt,
    redraw_selection__presenterZupdate95state95common_u5104 @0x140477480. */
 using RedrawSelection=void(*)(void*,void*);
 RedrawSelection redrawSelectionOriginal=nullptr;
 static constexpr uint64_t kSelectionBucketStride=0x20;
 std::set<uint64_t> hintSuppressedIndices;int hintSuppressedFrame=-1;int hintSuppressedLoggedCount=-1;
 bool selectionTrace=false;uint64_t traceComponentCount=0;
 int traceBoardStatus=999;std::string traceReason="unset";
 board_objects::Arrays hintBoardArrays{};bool hintBoardArraysValid=false;
 /* One live bucket of the component selection whose occupancy word this draw call
    has cleared, with the value to put back afterwards. */
 struct SelectionRingPatch {uint64_t* occupancy=nullptr;uint64_t saved=0;};
 std::vector<SelectionRingPatch> selectionRingPatches;
 static std::string hexValue(uintptr_t value){
  char text[32];std::snprintf(text,sizeof(text),"0x%llx",(unsigned long long)value);return std::string(text);
 }
 void refreshHintSuppressedIndices(){
  hintSuppressedIndices.clear();
  hintBoardArraysValid=false;
  bool any=false;for(auto& plugin:loaded)if(plugin->active&&!plugin->hiddenSelectionHints.empty()){any=true;break;}
  if(!any){traceReason="no suppressed type";return;}
  TCGameHandle board{};const int boardStatus=gameHandles.current(TC_GAME_OBJECT_BOARD,&board);
  traceBoardStatus=boardStatus;
  if(boardStatus!=TC_HANDLE_OK){traceReason="no board";return;}
  const void* raw=nullptr;if(gameHandles.resolve(&board,&raw)!=TC_HANDLE_OK){traceReason="board not resolvable";return;}
  board_objects::Arrays arrays{};if(!board_objects::readArrays(raw,arrays)){traceReason="no arrays";return;}
  hintBoardArrays=arrays;hintBoardArraysValid=true;
  traceComponentCount=arrays.components;
  traceReason="ok";
  for(uint64_t index=0;index<arrays.components;++index){
   const unsigned char* record=arrays.componentData+board_objects::kRecordHeader+index*board_objects::kComponentStride;
   if(board_objects::readU8(record,0)!=board_objects::kCustomComponentKind)continue;
   const uint64_t customId=board_objects::readU64(record,0x188);if(!customId)continue;
   for(auto& plugin:loaded)
    if(plugin->active&&plugin->hiddenSelectionHints.count(customId)){hintSuppressedIndices.insert(index);break;}
  }
 }
 void traceSelectionCall(const void* selection){
  static unsigned long long lastTrace=0;static int probeBudget=5;
  const unsigned long long now=GetTickCount64();
  if(probeBudget<=0&&now-lastTrace<1000)return;
  lastTrace=now;if(probeBudget>0)--probeBudget;
  const auto* bytes=static_cast<const unsigned char*>(selection);
  std::string line="Component render trace: selection="+hexValue(reinterpret_cast<uintptr_t>(selection))+
                   " wireEntries="+std::to_string(board_objects::readU64(bytes,0x00))+
                   " componentEntries="+std::to_string(board_objects::readU64(bytes,0x18))+
                   " board="+std::to_string(traceBoardStatus)+" reason="+traceReason+
                   " components="+std::to_string(traceComponentCount)+
                   " selectedComponents="+std::to_string(selectionLen&&selectedComponentSet?selectionLen(selectedComponentSet):0)+
                   " selectedWires="+std::to_string(selectionLen&&selectedWireSet?selectionLen(selectedWireSet):0)+
                   " ours=[";
  for(uint64_t index:hintSuppressedIndices)line+=std::to_string(index)+",";
  line+="]";
  logger(line);
  /* The two sequences the loop walks sit at +0x00 and +0x18 and are the game's own
     selection hash sets: a bucket array pointer at +0x08 with buckets the loop
     treats as "occupied" when the 64 bit word at +0x08 is non-zero.  Every word of
     the live buckets is tested with the game's own membership function, which is
     what names the set (and the id) without guessing the bucket layout. */
  for(int which=0;which<2;++which){
   const size_t header=which?0x18:0x00;
   const uint64_t buckets=board_objects::readU64(bytes,header);
   const unsigned char* data=board_objects::readPointer(bytes,header+0x08);
   std::string dump=std::string(which?"Component render trace: second set":"Component render trace: first set")+
                    " buckets="+std::to_string(buckets)+" data="+hexValue(reinterpret_cast<uintptr_t>(data))+
                    " componentSetData="+(selectedComponentSet?hexValue(reinterpret_cast<uintptr_t>(
                        board_objects::readPointer(static_cast<const unsigned char*>(selectedComponentSet),0x08))):std::string("none"))+
                    " wireSetData="+(selectedWireSet?hexValue(reinterpret_cast<uintptr_t>(
                        board_objects::readPointer(static_cast<const unsigned char*>(selectedWireSet),0x08))):std::string("none"));
   if(!data||!buckets||buckets>board_objects::kMaxComponents){logger(dump);continue;}
   if(board_objects::readableRegion(data,static_cast<size_t>(buckets)*kSelectionBucketStride))
    for(uint64_t i=0;i<buckets;++i){
     const unsigned char* bucket=data+i*kSelectionBucketStride;
     if(!board_objects::readU64(bucket,0x08))continue;
     dump+=" <#"+std::to_string(i);
     for(size_t word=0;word<4;++word){
      const uint64_t value=board_objects::readU64(bucket,word*8);
      dump+=" ["+hexValue(word*8)+"="+hexValue(value)+membershipOf(value)+"]";
     }
     dump+=">";
     break;   /* the first live bucket is the whole diagnostic */
    }
   logger(dump);
  }
 }
 void suppressSelectionRings(void* selection){
  selectionRingPatches.clear();
  if(!selection)return;
  const int64_t frame=engineFrame();
  if(hintSuppressedFrame!=frame){hintSuppressedFrame=frame;refreshHintSuppressedIndices();}
  if(selectionTrace)traceSelectionCall(selection);
  if(hintSuppressedIndices.empty())return;
  const auto* bytes=static_cast<const unsigned char*>(selection);
  /* Both sequences the drawing pass walks are candidate selection sets; only the
     one whose ids the game itself reports as selected components is patched, so a
     wire index that happens to equal one of our component indices can never lose
     its ring. */
  for(size_t header:{size_t{0x00},size_t{0x18}}){
   const uint64_t buckets=board_objects::readU64(bytes,header);
   auto* data=const_cast<unsigned char*>(board_objects::readPointer(bytes,header+0x08));
   if(!data||!buckets||buckets>board_objects::kMaxComponents)continue;
   if(!board_objects::readableRegion(data,static_cast<size_t>(buckets)*kSelectionBucketStride))continue;
   const bool componentSet=selectionBucketsAreComponents(data,buckets);
   if(!componentSet)continue;
   for(uint64_t i=0;i<buckets;++i){
    auto* bucket=data+i*kSelectionBucketStride;
    if(!board_objects::readU64(bucket,0x08))continue;
    if(!hintSuppressedIndices.count(board_objects::readU64(bucket,0x10)))continue;
    selectionRingPatches.push_back(SelectionRingPatch{reinterpret_cast<uint64_t*>(bucket+0x08),
                                                      board_objects::readU64(bucket,0x08)});
    *reinterpret_cast<uint64_t*>(bucket+0x08)=0;
   }
  }
  if(!selectionRingPatches.empty()&&
     hintSuppressedLoggedCount!=static_cast<int>(selectionRingPatches.size())){
   hintSuppressedLoggedCount=static_cast<int>(selectionRingPatches.size());
   logger("Component render: cleared the game's selection ring for "+
          std::to_string(selectionRingPatches.size())+" Mod component(s)");
  }
 }
 void restoreSelectionRings(){
  for(const SelectionRingPatch& patch:selectionRingPatches){
   if(!patch.occupancy)continue;
   *patch.occupancy=patch.saved;
  }
  selectionRingPatches.clear();
 }
 static void detourRedrawSelection(void* selection,void* mesh){
  auto* self=activeInstance;
  if(!self||!self->redrawSelectionOriginal)return;
  self->suppressSelectionRings(selection);
  self->redrawSelectionOriginal(selection,mesh);
  self->restoreSelectionRings();
 }
 /* Diagnostic only (armed by TC_SELECTION_TRACE=1): names every producer of the
    selection sprite, so "who drew this ring" is answered by the log instead of by
    reading the state loops.  The two entry points are the ones
    redraw_selection itself calls (see build/exe-disasm.txt). */
 using SelectionMeshIncrement=void(*)(void*,uint64_t);
 using SelectionMeshTransform=void(*)(void*,const void*,uint64_t);
 SelectionMeshIncrement selectionIncrementOriginal=nullptr;
 SelectionMeshTransform selectionTransformOriginal=nullptr;
 /* toPackedTransform2DSameScaleNoRotation(out, transform, mesh): the transform it
    packs is two doubles plus a flags byte, and it is the last place the screen
    position of a selection sprite is still readable as numbers. */
 using PackSameScaleTransform=void(*)(void*,const void*,const void*);
 PackSameScaleTransform packTransformOriginal=nullptr;
 /* The game's own selection sets and its membership test.  Asking the game what a
   bucket holds turns "which element is this ring for" into a fact instead of a
   guess about the bucket layout (the same functions tc_board_model.h uses). */
 void* selectedComponentSet=nullptr;void* selectedWireSet=nullptr;
 uint8_t(*selectionContains)(const void*,uint64_t)=nullptr;
 uint64_t(*selectionLen)(const void*)=nullptr;
 void resolveSelectionModel(){
  auto want=[&](const char* name)->void*{auto it=symbols?symbols->values.find(name):decltype(symbols->values.end()){};
   return it==symbols->values.end()?nullptr:it->second;};
  selectedComponentSet=want("selected_components__modelZboardZboard_u22");
  selectedWireSet=want("selected_wires__modelZboardZboard_u30");
  selectionContains=reinterpret_cast<uint8_t(*)(const void*,uint64_t)>(want("contains__modelZboardZboard_u1842"));
  selectionLen=reinterpret_cast<uint64_t(*)(const void*)>(want("len__modelZboardZboard_u19087"));
 }
 std::string membershipOf(uint64_t value){
  if(!selectionContains)return std::string();
  const char* kind=selectedComponentSet&&selectionContains(selectedComponentSet,value)?"components":
                   (selectedWireSet&&selectionContains(selectedWireSet,value)?"wires":nullptr);
  return kind?std::string(" ")+kind:std::string();
 }
 /* True when a bucket array really is the component selection: its ids are
    component sequence indices, which is exactly what the game's own
    selected_components set answers yes to.  A wire selection carries wire indices
    instead, so a wire index that happens to equal one of our component indices can
    never lose its ring. */
 bool selectionBucketsAreComponents(const unsigned char* data,uint64_t buckets){
  if(!selectionContains||!selectedComponentSet)return false;
  for(uint64_t i=0;i<buckets;++i){
   const unsigned char* bucket=data+i*kSelectionBucketStride;
   if(!board_objects::readU64(bucket,0x08))continue;
   return selectionContains(selectedComponentSet,board_objects::readU64(bucket,0x10))!=0;
  }
  return false;
 }
 static void detourPackTransform(void* out,const void* transform,const void* mesh){
  auto* self=activeInstance;
  if(!self||!self->packTransformOriginal)return;
  if(self->selectionTrace){
   double x=0,y=0;unsigned char flags=0;
   if(transform){std::memcpy(&x,transform,8);std::memcpy(&y,static_cast<const unsigned char*>(transform)+8,8);
    flags=board_objects::readU8(static_cast<const unsigned char*>(transform),0x10);}
   self->logger("Component render trace: pack x="+std::to_string(x)+" y="+std::to_string(y)+
                " flags="+std::to_string(flags)+
                " caller="+hexValue(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
  }
  self->packTransformOriginal(out,transform,mesh);
 }
 static void detourSelectionIncrement(void* mesh,uint64_t count){
  auto* self=activeInstance;
  if(!self||!self->selectionIncrementOriginal)return;
  if(self->selectionTrace)
   self->logger("Component render trace: selection increment count="+std::to_string(count)+
                " caller="+hexValue(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
  self->selectionIncrementOriginal(mesh,count);
 }
 static void detourSelectionTransform(void* mesh,const void* packed,uint64_t index){
  auto* self=activeInstance;
  if(!self||!self->selectionTransformOriginal)return;
  if(self->selectionTrace&&packed){
   const auto* raw=static_cast<const unsigned char*>(packed);
   self->logger("Component render trace: selection transform index="+std::to_string(index)+
                " packed="+hexValue(board_objects::readU64(raw,0))+
                " caller="+hexValue(reinterpret_cast<uintptr_t>(__builtin_return_address(0))));
  }
  self->selectionTransformOriginal(mesh,packed,index);
 }
 void armSelectionMeshTrace(){
  if(!selectionTrace||!symbols)return;
  resolveSelectionModel();
  {
   /* Every address in the trace is logged raw; the image base is what turns one
      into an RVA that can be looked up in build/exe-disasm.txt. */
   auto anchor=symbols->values.find("redraw_selection__presenterZupdate95state95common_u5104");
   if(anchor!=symbols->values.end()&&anchor->second)
    logger("Component render trace: image base "+hexValue(reinterpret_cast<uintptr_t>(anchor->second)-0x477480));
  }
  auto arm=[&](const char* name,void* detour,void** original,const char* what){
   auto found=symbols->values.find(name);
   if(found==symbols->values.end()||!found->second){logger(std::string("Component render trace: ")+what+" not found");return;}
   if(MH_CreateHook(found->second,detour,original)!=MH_OK||MH_EnableHook(found->second)!=MH_OK){
    MH_RemoveHook(found->second);*original=nullptr;logger(std::string("Component render trace: ")+what+" hook failed");return;}
   ownedHooks.insert(found->second);
   logger(std::string("Component render trace: ")+what+" traced");
  };
  arm("increment_instance_count__presenterZrendererZmulti95meshZselection95mesh_u713",
      reinterpret_cast<void*>(&NativeRuntime::detourSelectionIncrement),
      reinterpret_cast<void**>(&selectionIncrementOriginal),"selection instance count");
  arm("set_transform2d__presenterZrendererZmulti95meshZselection95mesh_u727",
      reinterpret_cast<void*>(&NativeRuntime::detourSelectionTransform),
      reinterpret_cast<void**>(&selectionTransformOriginal),"selection transform");
  arm("toPackedTransform2DSameScaleNoRotation__presenterZrendererZtransform952d_u139",
      reinterpret_cast<void*>(&NativeRuntime::detourPackTransform),
      reinterpret_cast<void**>(&packTransformOriginal),"packed transform");
 }
 /* The game's own "edit this component in the foundry" button - the one the bottom
    panel draws while a custom component is selected - is a single igButton call in
    build_component_description_panel (call site RVA 0x3a45ac).  Clicking it stores
    the component's inputs, pushes a level return stack entry and loads the foundry
    level with that component's schematic.

    A Mod-owned type has no such schematic: its logic comes from the Mod and its
    shape from tc.component.geometry, so the workshop entry is meaningless for it.
    V4 therefore lets a Mod drop that button for its own types.  The host does it at
    that one call site: the button is drawn with fully transparent styles (so the
    panel keeps its exact layout and nothing else moves) and the call reports "not
    clicked", so the button is invisible and inert.  Every other button in the game
    and in the loader forwards untouched.

    It is hidden only while every selected component is one of the calling Mod's
    hidden types - a game component in the same selection keeps its own button.
    Evidence: build/exe-disasm.txt, build_component_description_panel @0x14039b400,
    igButton at 0x1403a45a7 (return address 0x1403a45ac). */
 static constexpr uintptr_t kFoundryButtonCallRva=0x3a45ac;
 /* ImGui's own value types, spelled the way the loader already reads them: an ImVec2
    arrives in xmm0, an ImVec4 is four floats. */
 struct RenderVec4 {float x,y,z,w;};
 using IgButton=bool(*)(const char*,RenderVec2);
 using IgPushStyleColor=void(*)(int,RenderVec4);
 using IgPopStyleColor=void(*)(int);
 /* This build returns ImVec2 through a hidden out-pointer (the same measured fact the
    text-box plugin records), so the rect getters take the destination in rcx. */
 using IgItemRect=void(*)(RenderVec2*);
 IgButton igButtonOriginal=nullptr;
 IgPushStyleColor igPushStyleColor=nullptr;IgPopStyleColor igPopStyleColor=nullptr;
 IgItemRect igGetItemRectMin=nullptr;IgItemRect igGetItemRectMax=nullptr;
 bool foundryTrace=false;
 std::set<uint64_t> hiddenFoundryIndices;int hiddenFoundryFrame=-1;int hiddenFoundryLoggedCount=-1;
 uintptr_t gameRva(uintptr_t rva) const {
  const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  return base?base+rva:0;
 }
 void refreshHiddenFoundryIndices(){
  hiddenFoundryIndices.clear();
  /* The board is read here rather than borrowed from the selection-hint refresh: this
     list has to be right even for a Mod that never asked for the hint control. */
  bool any=false;for(auto& plugin:loaded)if(plugin->active&&!plugin->hiddenFoundryButtons.empty()){any=true;break;}
  if(!any)return;
  TCGameHandle board{};const void* raw=nullptr;
  if(gameHandles.current(TC_GAME_OBJECT_BOARD,&board)!=TC_HANDLE_OK)return;
  if(gameHandles.resolve(&board,&raw)!=TC_HANDLE_OK)return;
  board_objects::Arrays arrays{};if(!board_objects::readArrays(raw,arrays))return;
  for(uint64_t index=0;index<arrays.components;++index){
   const unsigned char* record=arrays.componentData+
                               board_objects::kRecordHeader+index*board_objects::kComponentStride;
   if(board_objects::readU8(record,0)!=board_objects::kCustomComponentKind)continue;
   const uint64_t customId=board_objects::readU64(record,0x188);if(!customId)continue;
   for(auto& plugin:loaded)
    if(plugin->active&&plugin->hiddenFoundryButtons.count(customId)){hiddenFoundryIndices.insert(index);break;}
  }
 }
 /* True when the panel is describing only Mod-owned types that asked for the button
    to be gone.  The selection set is the game's own, and membership is asked through
    the game's own contains(), the same way tc_board_model.h does it. */
 bool foundryButtonHidden(){
  if(hiddenFoundryIndices.empty())return false;
  if(!selectionContains||!selectedComponentSet||!selectionLen)return false;
  const uint64_t selected=selectionLen(selectedComponentSet);
  if(!selected)return false;
  uint64_t hidden=0;
  for(uint64_t index:hiddenFoundryIndices)if(selectionContains(selectedComponentSet,index))++hidden;
  return hidden==selected;
 }
 void traceFoundryButton(bool hidden){
  static unsigned long long lastTrace=0;
  const unsigned long long now=GetTickCount64();
  if(now-lastTrace<1000)return;
  lastTrace=now;
  std::string line="Component render trace: foundry button";
  if(igGetItemRectMin&&igGetItemRectMax){
   RenderVec2 minimum{},maximum{};
   igGetItemRectMin(&minimum);igGetItemRectMax(&maximum);
   line+=" box="+std::to_string(minimum.x)+","+std::to_string(minimum.y)+".."+
         std::to_string(maximum.x)+","+std::to_string(maximum.y);
  }
  line+=" hidden="+std::to_string(hidden?1:0)+
        " selected="+std::to_string(selectionLen&&selectedComponentSet?selectionLen(selectedComponentSet):0);
  logger(line);
 }
 static bool detourIgButton(const char* label,RenderVec2 size){
  auto* self=activeInstance;
  if(!self||!self->igButtonOriginal)return false;
  const uintptr_t caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  if(caller==self->gameRva(kFoundryButtonCallRva)){
   const int64_t frame=self->engineFrame();
   if(self->hiddenFoundryFrame!=frame){self->hiddenFoundryFrame=frame;self->refreshHiddenFoundryIndices();}
   const bool hidden=self->foundryButtonHidden();
   bool clicked=false;
   if(hidden){
    if(self->igPushStyleColor&&self->igPopStyleColor){
     /* Text, Button, ButtonHovered, ButtonActive, Border: every colour the button
        itself can paint with, so nothing of it reaches the frame. */
     static const int kColours[]={0,21,22,23,5};
     for(int index:kColours)self->igPushStyleColor(index,RenderVec4{0.f,0.f,0.f,0.f});
     self->igButtonOriginal(label,size);
     self->igPopStyleColor(static_cast<int>(std::size(kColours)));
    }else clicked=self->igButtonOriginal(label,size);
    /* The button was still submitted, only invisibly: the panel keeps its layout, and
       reporting "not clicked" is what makes it inert. */
    if(self->foundryTrace)self->traceFoundryButton(true);
    if(self->hiddenFoundryLoggedCount!=1){
     self->hiddenFoundryLoggedCount=1;
     self->logger("Component render: the foundry edit button is hidden for the selected Mod component");
    }
    return false;
   }
   clicked=self->igButtonOriginal(label,size);
   if(self->foundryTrace)self->traceFoundryButton(false);
   return clicked;
  }
  return self->igButtonOriginal(label,size);
 }
 void armFoundryButtonControl(){
  if(!engine)return;
  auto button=GetProcAddress(engine,"igButton");
  if(!button){logger("Component render: the panel's foundry button is unavailable");return;}
  if(MH_CreateHook(reinterpret_cast<void*>(button),
                   reinterpret_cast<void*>(&NativeRuntime::detourIgButton),
                   reinterpret_cast<void**>(&igButtonOriginal))!=MH_OK)return;
  if(MH_EnableHook(reinterpret_cast<void*>(button))!=MH_OK){
   MH_RemoveHook(reinterpret_cast<void*>(button));igButtonOriginal=nullptr;return;
  }
  igPushStyleColor=reinterpret_cast<IgPushStyleColor>(GetProcAddress(engine,"igPushStyleColor_Vec4"));
  igPopStyleColor=reinterpret_cast<IgPopStyleColor>(GetProcAddress(engine,"igPopStyleColor"));
  igGetItemRectMin=reinterpret_cast<IgItemRect>(GetProcAddress(engine,"igGetItemRectMin"));
  igGetItemRectMax=reinterpret_cast<IgItemRect>(GetProcAddress(engine,"igGetItemRectMax"));
  ownedHooks.insert(reinterpret_cast<void*>(button));
  loaderOwned[reinterpret_cast<void*>(button)]="component render V4 foundry button control";
  if(const char* trace=std::getenv("TC_FOUNDRY_TRACE"))foundryTrace=*trace&&trace[0]!='0';
  resolveSelectionModel();
  logger("Component render: foundry button control armed");
 }
 void armSelectionHintControl(){
  if(!symbols)return;
  auto found=symbols->values.find("redraw_selection__presenterZupdate95state95common_u5104");
  if(found==symbols->values.end()||!found->second){logger("Component render: selection hint control unavailable");return;}
  if(MH_CreateHook(found->second,reinterpret_cast<void*>(&NativeRuntime::detourRedrawSelection),
                   reinterpret_cast<void**>(&redrawSelectionOriginal))!=MH_OK)return;
  if(MH_EnableHook(found->second)!=MH_OK){MH_RemoveHook(found->second);redrawSelectionOriginal=nullptr;return;}
  ownedHooks.insert(found->second);
  loaderOwned[found->second]="component render selection hint control";
  if(const char* trace=std::getenv("TC_SELECTION_TRACE"))selectionTrace=*trace&&trace[0]!='0';
  resolveSelectionModel();
  logger("Component render: selection hint control armed");
 }
 void armDefaultDrawingControl(){
 if(!symbols)return;
 auto found=symbols->values.find("redraw_component__presenterZupdate95state95common_u4925");
 if(found==symbols->values.end()||!found->second){logger("Component render: default drawing control unavailable");return;}
 if(MH_CreateHook(found->second,reinterpret_cast<void*>(&NativeRuntime::detourRedrawComponent),
                  reinterpret_cast<void**>(&redrawComponentOriginal))!=MH_OK)return;
 if(MH_EnableHook(found->second)!=MH_OK){MH_RemoveHook(found->second);redrawComponentOriginal=nullptr;return;}
 redrawComponentTarget=found->second;ownedHooks.insert(found->second);
 loaderOwned[found->second]="component render V2 default drawing control";
 logger("Component render: default drawing control armed");
}
 void armPlacementPreviewControl(){
  if(!symbols)return;
  auto found=symbols->values.find(
      "redraw_clipboard_component__presenterZupdate95state95clipboard_u7");
  auto hide=symbols->values.find("hide_clipboard__presenterZcontext_u2917");
  /* The clipboard's own update is what ends a preview (see
     observeClipboardRecord): a build whose symbol table does not name it cannot
     be given the takeover, because the takeover would have no end signal. */
  auto update=symbols->values.find(
      "update_state_clipboard__presenterZupdate95state95clipboard_u120");
  if(found==symbols->values.end()||!found->second||
     update==symbols->values.end()||!update->second){
   logger("Component render: placement preview control unavailable");return;
  }
  if(MH_CreateHook(found->second,
                   reinterpret_cast<void*>(&NativeRuntime::detourRedrawClipboardComponent),
                   reinterpret_cast<void**>(&redrawClipboardComponentOriginal))!=MH_OK)return;
  if(MH_CreateHook(update->second,
                   reinterpret_cast<void*>(&NativeRuntime::detourUpdateStateClipboard),
                   reinterpret_cast<void**>(&updateStateClipboardOriginal))!=MH_OK){
   MH_RemoveHook(found->second);redrawClipboardComponentOriginal=nullptr;return;
  }
  if(MH_EnableHook(found->second)!=MH_OK){
   MH_RemoveHook(update->second);MH_RemoveHook(found->second);
   updateStateClipboardOriginal=nullptr;redrawClipboardComponentOriginal=nullptr;return;
  }
  if(MH_EnableHook(update->second)!=MH_OK){
   MH_DisableHook(found->second);MH_RemoveHook(found->second);MH_RemoveHook(update->second);
   redrawClipboardComponentOriginal=nullptr;updateStateClipboardOriginal=nullptr;return;
  }
  redrawClipboardComponentTarget=found->second;ownedHooks.insert(found->second);
  updateStateClipboardTarget=update->second;ownedHooks.insert(update->second);
  loaderOwned[found->second]="component render V6 placement preview control";
  loaderOwned[update->second]="component render V6 placement preview lifetime";
  /* hide_clipboard was the first end signal the takeover used, and it is still
     one of the game's ways to put the clipboard down (cancel, cut, scene
     change).  It is not required - the record observer above is - so a missing
     symbol only costs the belt-and-braces path. */
  if(hide!=symbols->values.end()&&hide->second&&
     MH_CreateHook(hide->second,reinterpret_cast<void*>(&NativeRuntime::detourHideClipboard),
                   reinterpret_cast<void**>(&hideClipboardOriginal))==MH_OK&&
     MH_EnableHook(hide->second)==MH_OK){
   hideClipboardTarget=hide->second;ownedHooks.insert(hide->second);
   loaderOwned[hide->second]="component render V6 placement preview hide signal";
  }
  logger("Component render: placement preview control armed");
 }
 /* The component drawer - the panel along the bottom the game shows for the
    *selected* component, where a built-in Constant's label and value fields
    live.  A Mod's editor belongs there, not in a window of its own, so the
    loader owns this hook and calls the owning Mod's
    TC_UI_SLOT_BOARD_COMPONENT_PANEL slots once the game has laid out its own
    rows.

    The injection point is the drawer's own title row
    (build_toggle_button, called by build_bottom_panel right after it opens the
    window and before whichever branch draws the content): the root returns with
    its window already closed, so drawing there would land in the wrong window,
    while the description builder the punch-tape example uses only runs for the
    component kinds that have a description panel.  The title row runs for every
    drawer state, inside the window, with the panel's geometry a call away.

    Which component the panel is for comes from the game's own selection set and
    its own membership function (resolveSelectionModel), so no argument layout
    has to be assumed. */
 using ComponentPanelFn=void(*)(void*,void*,void*,void*,void*,void*,void*,void*);
 ComponentPanelFn componentPanelOriginal=nullptr;
 void* componentPanelTarget=nullptr;
 static void detourComponentPanel(void* a,void* b,void* c,void* d,
                                  void* e,void* f,void* g,void* h){
  auto* self=activeInstance;
  if(self&&self->componentPanelOriginal)
   self->componentPanelOriginal(a,b,c,d,e,f,g,h);
  if(!self)return;
  self->drawComponentPanelSlots();
 }
 void armComponentPanelControl(){
  if(!symbols)return;
  auto found=symbols->values.find(
      "build_toggle_button__presenterZboard95uiZbottom95panelZcommon_u8");
  if(found==symbols->values.end()||!found->second){logger("Component panel: the game's component drawer was not found");return;}
  if(MH_CreateHook(found->second,reinterpret_cast<void*>(&NativeRuntime::detourComponentPanel),
                   reinterpret_cast<void**>(&componentPanelOriginal))!=MH_OK)return;
  if(MH_EnableHook(found->second)!=MH_OK){MH_RemoveHook(found->second);componentPanelOriginal=nullptr;return;}
  componentPanelTarget=found->second;ownedHooks.insert(found->second);
  loaderOwned[found->second]="component panel slot host";
  logger("Component panel: the game's component drawer is hooked for Mod editor rows");
 }
 bool hasComponentPanelSlots(){
  for(auto& p:loaded){if(!p->active)continue;
   for(auto& slot:p->slots)if(slot.kind==TC_UI_SLOT_BOARD_COMPONENT_PANEL&&!slot.failed)return true;}
  return false;
 }
 /* ---- V5: the picture a Mod's type is shown with --------------------------

    A custom component's picture - the item in the game's own component column,
    the preview in the bottom drawer and the ghost that follows the cursor
    while the component is placed - is one texture the game asks its renderer
    for every frame, and the game builds that texture's path itself:

      get_captured_path__presenterZio_u28  -> "?snapshot_cc/com_custom_<id>.png"
      create_texture_unsafe__…_u364(out, path, …)   (the resolved file path)
      create_texture__…_u1978(out, path, …)         (the checked wrapper around it)

    Measured, with the full evidence in docs/research/component-icons.md: the
    file at that path *is* read (and re-encoded) but the picture the player
    sees is the game's own render, so merely dropping a file there changes
    nothing.  What does change it is answering the texture request: the game
    keeps creating, owning and releasing the texture itself and simply reads a
    different file (proved by the takeover spike in tests/icon-probe.cpp, whose
    substitution is exactly what this detour ships).

    The loader therefore owns a copy of every picture a Mod registers
    (component_render_set_picture_api below).  Handing the game a copy rather
    than the Mod's own file is deliberate: the game rewrites the file it read
    (measured: the 1060-byte substitute came back re-encoded as 642 bytes), and
    a Mod's shipped file is journalled by the loader - rewriting it would make
    the next apply() refuse with "File changed outside loader".  The copy lives
    in <game>/tc-modloader-data/pictures and is refreshed whenever the Mod's own
    file changes, so a Mod may also replace its picture at run time.

    Every other texture request - the built-in "?snapshot/<kind>.png" pictures,
    sprites, menus, fonts, the game's own capture cache - is forwarded exactly
    as it arrived (src/picture_path.hpp is the rule that tells them apart). */
 using CreateTextureUnsafe=uint64_t(*)(void* out,const void* path,void* a3,void* a4,uint64_t a5);
 using CreateTexture=uint64_t(*)(void* out,const void* path,void* a3,void* a4);
 CreateTextureUnsafe createTextureUnsafeOriginal=nullptr;
 CreateTexture createTextureOriginal=nullptr;
 /* `payload` is the Nim string the game is handed: an eight-byte capacity word,
    the path and a NUL.  It is kept here, next to the copy, because the game may
    hold the pointer past the call (a texture created from it is destroyed
    whenever the game unloads that texture) - a buffer on the detour's own stack
    would be freed under it, which is exactly the access violation this replaced. */
 struct PictureCopy {std::string source,copy;uint64_t size=0,write=0;int64_t checkedFrame=-1;
                     std::vector<char> payload;};
 std::map<uint64_t,PictureCopy> pictureCopies;
 std::map<uint64_t,int64_t> pictureServed;      /* first serve per id, for the log */
 std::set<uint64_t> pictureMissing;             /* ids whose file went missing */
 bool pictureTrace=false;
 bool pictureTraceAll=false;
 int pictureTraceCount=0;
 /* A Nim string as the game passes it: a length and a pointer to a payload
    whose first eight bytes are the capacity word, the characters after it - so
    the characters start at payload+8, the same convention src/save_boot.hpp
    documents and tests/icon-probe.cpp reads. */
 static bool nimText(const void* pointer,const char** text,size_t* length){
  if(!pointer)return false;
  uint64_t size=0;const char* bytes=nullptr;
  std::memcpy(&size,pointer,sizeof(size));
  std::memcpy(&bytes,static_cast<const unsigned char*>(pointer)+8,sizeof(bytes));
  if(!bytes||!size||size>4096)return false;
  *text=bytes+8;*length=static_cast<size_t>(size);
  return true;
 }
 void tracePicture(const std::string& line){
  if(!pictureTrace||pictureTraceCount>400)return;
  ++pictureTraceCount;logger("Component picture trace: "+line);
 }
 /* Every request the factory receives, bounded: a path that names a *custom
    component* is always interesting (that is the one V5 answers), and
    TC_PICTURE_TRACE=all writes the rest as well, which is how a "the game never
    asked" report is told apart from "it asked for something else". */
 void tracePictureRequest(const char* text,size_t length,bool matched){
  if(!pictureTrace||matched||pictureTraceCount>400)return;
  const std::string path(text,length);
  if(!pictureTraceAll&&path.find("com_")==std::string::npos&&
     path.find("snapshot")==std::string::npos)return;
  ++pictureTraceCount;logger("Component picture trace: request \""+path+"\"");
 }
 /* The Mod that registered a picture for this id, or null.  A type belongs to
    exactly one plugin (the registry refuses duplicates), so the first hit is
    also the owner. */
 Loaded* pictureOwner(uint64_t customId,const std::string** source){
  for(auto& plugin:loaded){
   if(!plugin->active)continue;
   auto found=plugin->pictures.find(customId);
   if(found==plugin->pictures.end()||found->second.empty())continue;
   *source=&found->second;
   return plugin.get();
  }
  return nullptr;
 }
 /* The copy of one Mod picture, rewritten whenever the Mod's own file changed.
    Returns false when the file is gone or is not a PNG, which puts the type
    back on the game's own picture (the documented V5 fallback). */
 bool pictureCopyFor(Loaded& owner,uint64_t customId,const std::string& source,
                     std::string* out){
  PictureCopy& copy=pictureCopies[customId];
  const int64_t frame=engineFrame();
  if(frame>=0&&copy.checkedFrame==frame&&!copy.copy.empty()){
   *out=copy.copy;return true;
  }
  WIN32_FILE_ATTRIBUTE_DATA info{};
  const fs::path sourcePath=fs::u8path(source);
  if(!GetFileAttributesExW(sourcePath.c_str(),GetFileExInfoStandard,&info)||
     (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){
   if(pictureMissing.insert(customId).second)
    logger("Component picture: "+owner.id+" "+hexValue(customId)+
           " has no file at \""+source+"\"; the game's own picture is used");
   return false;
  }
  const uint64_t size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
  const uint64_t write=(uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|
                       info.ftLastWriteTime.dwLowDateTime;
  if(!copy.copy.empty()&&copy.source==source&&copy.size==size&&copy.write==write){
   copy.checkedFrame=frame;*out=copy.copy;return true;
  }
  const fs::path target=core.dir/L"pictures"/fs::u8path(std::to_string(customId)+".png");
  try{
   static const std::string kPngSignature("\x89PNG\r\n\x1a\n",8);
   const std::string bytes=read(sourcePath);
   if(bytes.size()<kPngSignature.size()||
      std::memcmp(bytes.data(),kPngSignature.data(),kPngSignature.size())!=0){
    logger("Component picture: "+owner.id+" "+hexValue(customId)+" points at \""+source+
           "\", which is not a PNG; the game's own picture is used");
    return false;
   }
   no_links(core.root,target);
   atomic(target,bytes);
  }catch(const std::exception& e){
   logger("Component picture: cannot copy \""+source+"\": "+e.what());
   return false;
  }
  const bool first=copy.source!=source;
  copy.source=source;copy.copy=target.u8string();copy.size=size;copy.write=write;
  copy.checkedFrame=frame;pictureMissing.erase(customId);
  {const std::string& text=copy.copy;
   copy.payload.assign(8+text.size()+1,0);
   const uint64_t capacity=uint64_t(text.size())|(uint64_t(1)<<62);
   std::memcpy(copy.payload.data(),&capacity,sizeof(capacity));
   std::memcpy(copy.payload.data()+8,text.data(),text.size());}
  if(first)tracePicture("copied \""+source+"\" to \""+copy.copy+"\"");
  *out=copy.copy;
  return true;
 }
 /* One texture request: does it name a Mod type's picture, and if so, hand back
    the loader-owned Nim string the game should read instead (see PictureCopy). */
 bool pictureRequest(const char* text,size_t length,TCNimString* substitute){
  uint64_t customId=0;
  if(!pictures::parseCustomSnapshot(text,length,&customId))return false;
  const std::string* source=nullptr;
  Loaded* owner=pictureOwner(customId,&source);
  if(!owner)return false;
  std::string resolved;
  if(!pictureCopyFor(*owner,customId,*source,&resolved))return false;
  const auto& copy=pictureCopies[customId];
  if(copy.payload.size()<9)return false;
  substitute->length=resolved.size();
  substitute->data=const_cast<char*>(copy.payload.data());
  auto served=pictureServed.find(customId);
  if(served==pictureServed.end()){
   pictureServed.emplace(customId,engineFrame());
   logger("Component picture: com_custom_"+std::to_string(customId)+" is served from \""+
          resolved+"\" ("+owner->id+"; the game asked for \""+
          std::string(text,length)+"\")");
  }else tracePicture("served com_custom_"+std::to_string(customId));
  return true;
 }
 /* The unchecked factory receives the *resolved* path, so it is where the
    substitution happens; the checked wrapper resolves the virtual path first
    and then calls this function, which means it arrives here either way. */
 static uint64_t detourCreateTextureUnsafe(void* out,const void* path,void* a3,void* a4,
                                           uint64_t a5){
  auto* self=activeInstance;
  if(!self||!self->createTextureUnsafeOriginal)return 0;
  const char* text=nullptr;size_t length=0;
  TCNimString substitute{};
  if(self->nimText(path,&text,&length)){
   if(self->pictureRequest(text,length,&substitute)){
    /* The game's own call shape, reused verbatim: only the path changes.  The
       string lives in the loader's own table, so whatever the game keeps from
       this call stays valid for as long as it holds the texture. */
    return self->createTextureUnsafeOriginal(out,&substitute,a3,a4,a5);
   }
   self->tracePictureRequest(text,length,false);
  }
  return self->createTextureUnsafeOriginal(out,path,a3,a4,a5);
 }
 /* The checked wrapper is hooked for the case where a caller hands it the
    virtual path directly (the palette asks the unsafe factory itself, which is
    why the spike saw only that one).  It needs no substitution of its own: it
    resolves the path and calls the factory above, where the request is caught.
    Forwarding it here keeps the two hooks independent of each other. */
 static uint64_t detourCreateTexture(void* out,const void* path,void* a3,void* a4){
  auto* self=activeInstance;
  if(!self||!self->createTextureOriginal)return 0;
  const char* text=nullptr;size_t length=0;
  uint64_t customId=0;
  if(self->nimText(path,&text,&length)&&
     pictures::parseCustomSnapshot(text,length,&customId))
   self->tracePicture("the checked wrapper resolved a request for com_custom_"+
                      std::to_string(customId));
  return self->createTextureOriginal(out,path,a3,a4);
 }
 void armPictureControl(){
  if(!symbols)return;
  if(const char* trace=std::getenv("TC_PICTURE_TRACE")){
   pictureTrace=*trace&&trace[0]!='0';
   pictureTraceAll=trace[0]=='2'||trace[0]=='a'||trace[0]=='A';
  }
  auto arm=[&](const char* name,void* detour,void** original)->bool{
   auto found=symbols->values.find(name);
   if(found==symbols->values.end()||!found->second){
    logger(std::string("Component picture: ")+name+" not found");
    return false;
   }
   if(MH_CreateHook(found->second,detour,original)!=MH_OK){
    logger(std::string("Component picture: ")+name+" hook creation failed");
    return false;
   }
   if(MH_EnableHook(found->second)!=MH_OK){
    MH_RemoveHook(found->second);*original=nullptr;
    logger(std::string("Component picture: ")+name+" hook enable failed");
    return false;
   }
   ownedHooks.insert(found->second);
   loaderOwned[found->second]="component render V5 picture control";
   return true;
  };
  const bool factory=arm("create_texture_unsafe__presenterZrendererZtextureZtexture_u364",
                         reinterpret_cast<void*>(&NativeRuntime::detourCreateTextureUnsafe),
                         reinterpret_cast<void**>(&createTextureUnsafeOriginal));
  const bool checked=arm("create_texture__presenterZrendererZtextureZtexture_u1978",
                         reinterpret_cast<void*>(&NativeRuntime::detourCreateTexture),
                         reinterpret_cast<void**>(&createTextureOriginal));
  if(!factory){
   logger("Component picture: the game's texture factory is unavailable; tc.component.render V5 stays closed");
   return;
  }
  if(!checked)
   logger("Component picture: the checked texture wrapper is unavailable; the factory hook covers every request");
  logger(std::string("Component render: picture control armed (")+
         (checked?"create_texture_unsafe + create_texture":"create_texture_unsafe only")+")");
 }
 /* Which component the drawer is describing: the game's own selection set holds
    the board's component indices (the same numbering the record array uses) and
    the game's own membership function answers for one index, so the scan below
    asks it rather than reading a bucket layout. */
 bool shownComponent(uint64_t* instance,uint64_t* customId){
  if(!selectedComponentSet||!selectionContains)return false;
  TCGameHandle board{};const void* boardRaw=nullptr;
  if(gameHandles.current(TC_GAME_OBJECT_BOARD,&board)!=TC_HANDLE_OK)return false;
  if(gameHandles.resolve(&board,&boardRaw)!=TC_HANDLE_OK)return false;
  board_objects::Arrays arrays{};
  if(!board_objects::readArrays(boardRaw,arrays))return false;
  for(uint64_t index=0;index<arrays.components;++index){
   const unsigned char* record=arrays.componentData+board_objects::kRecordHeader+
                               index*board_objects::kComponentStride;
   /* The board's component array is keyed by the record's own id; the selection
      set is asked about both that id and the array index, because the game's
      membership function is the only authority on which of them it stores. */
   const uint64_t id=board_objects::readU64(record,8);
   if(!selectionContains(selectedComponentSet,index)&&
      !(id&&selectionContains(selectedComponentSet,id)))continue;
   if(board_objects::readU8(record,0)!=board_objects::kCustomComponentKind)return false;
   const uint64_t custom=board_objects::readU64(record,0x188);
   if(!custom)return false;
   *customId=custom;*instance=id;
   return *instance!=0;
  }
  return false;
 }
 /* Called from inside the drawer, with that window's cursor sitting where the
    game's own rows ended: the slots draw ordinary widgets from there. */
 void drawComponentPanelSlots(){
  if(!hasComponentPanelSlots())return;
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return;
  uint64_t instance=0,customId=0;
  const bool shown=shownComponent(&instance,&customId);
  /* Bounded diagnostic: the first few times the game draws the drawer, say
     whether it was showing one of our components - the first thing to check
     when a Mod's rows do not appear. */
  static int traced=0;
  if(traced<3){
   ++traced;
   logger(std::string("Component panel: the drawer drew, selected=")+
          std::to_string(selectedComponentSet&&selectionLen?selectionLen(selectedComponentSet):0)+
          " showing "+
          (shown?("custom 0x"+hexValue(customId)+" instance 0x"+hexValue(instance)):
                 std::string("no Mod component")));
  }
  if(!shown)return;
  const SlotApi& api=slotApi();
  if(!api.available())return;
  for(auto& p:loaded){
   if(!p->active)continue;
   if(std::find(p->componentIds.begin(),p->componentIds.end(),customId)==p->componentIds.end())continue;
   for(auto& slot:p->slots){
    if(slot.failed||slot.kind!=TC_UI_SLOT_BOARD_COMPONENT_PANEL)continue;
    api.pushId(p->id.c_str());
    api.pushId(slot.id.c_str());
    const float width=api.getWindowWidth?api.getWindowWidth():0.f;
    const float height=api.getWindowHeight?api.getWindowHeight():0.f;
    TCFrame tick{sizeof(TCFrame),api.getFrameCount?api.getFrameCount():0,
                 api.getTime?api.getTime():0.0};
    /* This is the one slot kind whose callback carries the instance the drawer
       is showing: its editor reads and writes that instance's configuration,
       and there is no other way to name it.  Failure policy is the same as for
       every other slot (drop the slot, keep the loader alive). */
    if(slot.drawComponent){
     try{slot.drawComponent(slot.user,&tick,instance,width,height);}
     catch(const std::exception& e){
      slot.drawComponent=nullptr;slot.failed=true;
      logger("Component panel slot disabled after an exception: "+p->id+"/"+slot.id+": "+e.what());
     }catch(...){
      slot.drawComponent=nullptr;slot.failed=true;
      logger("Component panel slot disabled after an exception: "+p->id+"/"+slot.id);
     }
    }else{
     runSlotDraw(*p,slot,tick,width,height);
    }
    api.popId();
    api.popId();
   }
  }
 }
 static bool renderFinite(float value){return std::isfinite(value)&&std::abs(value)<10000000.f;}
 static RenderDrawContext* renderContext(void* context){
  auto* draw=static_cast<RenderDrawContext*>(context);
  return draw&&draw->active&&draw->owner&&draw->list?draw:nullptr;
 }
 static bool renderCommand(RenderDrawContext* draw){
  if(!draw||draw->commands>=TC_COMPONENT_RENDER_MAX_COMMANDS_PER_INSTANCE)return false;
  ++draw->commands;return true;
 }
 static int component_render_line(void* context,float x1,float y1,float x2,float y2,uint32_t color,float thickness){
  auto* draw=renderContext(context);if(!draw||!renderFinite(x1)||!renderFinite(y1)||!renderFinite(x2)||!renderFinite(y2)||!renderFinite(thickness)||thickness<=0)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(!renderCommand(draw))return TC_COMPONENT_RENDER_ERR_CAPACITY;
  draw->owner->renderLine(draw->list,{x1,y1},{x2,y2},color,thickness);return TC_COMPONENT_RENDER_OK;
 }
 static int component_render_rect(void* context,float minX,float minY,float maxX,float maxY,uint32_t color,float rounding,float thickness){
  auto* draw=renderContext(context);if(!draw||!renderFinite(minX)||!renderFinite(minY)||!renderFinite(maxX)||!renderFinite(maxY)||maxX<=minX||maxY<=minY||!renderFinite(rounding)||rounding<0||!renderFinite(thickness)||thickness<=0)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(!renderCommand(draw))return TC_COMPONENT_RENDER_ERR_CAPACITY;
  draw->owner->renderRect(draw->list,{minX,minY},{maxX,maxY},color,rounding,0,thickness);return TC_COMPONENT_RENDER_OK;
 }
 static int component_render_rect_filled(void* context,float minX,float minY,float maxX,float maxY,uint32_t color,float rounding){
  auto* draw=renderContext(context);if(!draw||!renderFinite(minX)||!renderFinite(minY)||!renderFinite(maxX)||!renderFinite(maxY)||maxX<=minX||maxY<=minY||!renderFinite(rounding)||rounding<0)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(!renderCommand(draw))return TC_COMPONENT_RENDER_ERR_CAPACITY;
  draw->owner->renderRectFilled(draw->list,{minX,minY},{maxX,maxY},color,rounding,0);return TC_COMPONENT_RENDER_OK;
 }
 static int component_render_circle(void* context,float x,float y,float radius,uint32_t color,float thickness){
  auto* draw=renderContext(context);if(!draw||!renderFinite(x)||!renderFinite(y)||!renderFinite(radius)||radius<=0||!renderFinite(thickness)||thickness<=0)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(!renderCommand(draw))return TC_COMPONENT_RENDER_ERR_CAPACITY;
  draw->owner->renderCircle(draw->list,{x,y},radius,color,0,thickness);return TC_COMPONENT_RENDER_OK;
 }
 static int component_render_circle_filled(void* context,float x,float y,float radius,uint32_t color){
  auto* draw=renderContext(context);if(!draw||!renderFinite(x)||!renderFinite(y)||!renderFinite(radius)||radius<=0)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(!renderCommand(draw))return TC_COMPONENT_RENDER_ERR_CAPACITY;
  draw->owner->renderCircleFilled(draw->list,{x,y},radius,color,0);return TC_COMPONENT_RENDER_OK;
 }
 static int component_render_text(void* context,float x,float y,uint32_t color,const char* utf8){
  auto* draw=renderContext(context);if(!draw||!renderFinite(x)||!renderFinite(y)||!utf8)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  const size_t length=strnlen(utf8,4097);if(length>4096)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(!renderCommand(draw))return TC_COMPONENT_RENDER_ERR_CAPACITY;
  draw->owner->renderText(draw->list,{x,y},color,utf8,utf8+length);return TC_COMPONENT_RENDER_OK;
 }
 /* The font and the size ImDrawList::AddText uses live in the draw list's
    shared data: list+0x38 is that block, +0x18 its font and +0x20 its size
    (read straight out of AddText_Vec2 in this build; the text-box example uses
    the same three numbers).  Both are swapped for exactly one call and put back
    before anything else runs, which is how the draw table offers a size and the
    game's bold face without disturbing the rest of the frame. */
 bool sizedText(void* list,const RenderVec2& position,uint32_t color,
                const char* begin,const char* end,float size,void* font){
  if(!list||!(size>0.f))return false;
  auto* bytes=static_cast<unsigned char*>(list);
  unsigned char* shared=nullptr;
  std::memcpy(&shared,bytes+0x38,sizeof(shared));
  if(!shared)return false;
  void* savedFont=nullptr;float savedSize=0.f;
  std::memcpy(&savedFont,shared+0x18,sizeof(savedFont));
  std::memcpy(&savedSize,shared+0x20,sizeof(savedSize));
  /* What AddText really paints is not the size it is given: this build paints a
     face about 0.79 of it (measured on the reference board: asking for 21.07 px
     produced a 16.7 px advance, 14.29 px produced 11.3 px).  The relation is
     logged once per process so a Mod's own layout constants can be checked
     against the build's, instead of living only in a screenshot. */
  {
   static bool logged=false;
   if(!logged){
    logged=true;
    auto* name=reinterpret_cast<const char*(*)(const void*)>(
        GetProcAddress(engine,"ImFont_GetDebugName"));
    char line[224];
    std::snprintf(line,sizeof(line),
        "Component render text: sized text armed, face=%s size field=%.2f",
        (savedFont&&name)?name(savedFont):"?",static_cast<double>(savedSize));
    logger(line);
   }
  }
  void* replacement=font?font:savedFont;
  std::memcpy(shared+0x18,&replacement,sizeof(replacement));
  std::memcpy(shared+0x20,&size,sizeof(size));
  renderText(list,position,color,begin,end);
  std::memcpy(shared+0x18,&savedFont,sizeof(savedFont));
  std::memcpy(shared+0x20,&savedSize,sizeof(savedSize));
  return true;
 }
 static int component_render_text_sized(void* context,float x,float y,float size,
                                        uint32_t color,int bold,const char* utf8){
  auto* draw=renderContext(context);
  if(!draw||!renderFinite(x)||!renderFinite(y)||!renderFinite(size)||!(size>0.f)||
     size>4096.f||!utf8)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  const size_t length=strnlen(utf8,4097);if(length>4096)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(!length)return TC_COMPONENT_RENDER_OK;
  if(!renderCommand(draw))return TC_COMPONENT_RENDER_ERR_CAPACITY;
  if(bold&&!draw->owner->boldTextFont)draw->owner->resolveToolTextFont();
  if(!draw->owner->sizedText(draw->list,{x,y},color,utf8,utf8+length,size,
                             bold?draw->owner->boldTextFont:nullptr))
   return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  return TC_COMPONENT_RENDER_OK;
 }
 /* The same measurement the game takes before it lays a label out, so a Mod can
    right-align a name or shrink a value that would not fit its own body. */
 static int component_render_measure_text(void* context,float size,int bold,const char* utf8,
                                          float* width,float* height){
  auto* draw=renderContext(context);
  if(!draw||!renderFinite(size)||!(size>0.f)||size>4096.f||!utf8||!width||!height)
   return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  const size_t length=strnlen(utf8,4097);if(length>4096)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  void* font=bold?draw->owner->boldTextFont:nullptr;
  if(bold&&!font)draw->owner->resolveToolTextFont();
  if(bold)font=draw->owner->boldTextFont;
  if(!font){
   auto* bytes=static_cast<unsigned char*>(draw->list);
   unsigned char* shared=nullptr;
   std::memcpy(&shared,bytes+0x38,sizeof(shared));
   if(shared)std::memcpy(&font,shared+0x18,sizeof(font));
  }
  if(!font||!draw->owner->renderCalcTextSize)return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  RenderVec2 measured{0.f,0.f};
  draw->owner->renderCalcTextSize(&measured,font,size,4096.f,0.f,utf8,utf8+length,nullptr);
  if(!renderFinite(measured.x)||!renderFinite(measured.y)||measured.x<=0.f||measured.y<=0.f)
   return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  *width=measured.x;*height=measured.y;
  return TC_COMPONENT_RENDER_OK;
 }
 static int component_render_set_callback_api(void* context,uint64_t customId,TCComponentRenderCallbackV1 callback,void* user){
  auto& p=*static_cast<Loaded*>(context);auto& r=*p.owner;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_COMPONENT_RENDER_ERR_THREAD;
  if(!p.accepting)return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  if(!customId)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())return TC_COMPONENT_RENDER_ERR_OWNERSHIP;
  auto found=std::find_if(p.renderers.begin(),p.renderers.end(),[&](const ComponentRenderer& item){return item.customId==customId;});
  if(!callback){if(found!=p.renderers.end())p.renderers.erase(found);return TC_COMPONENT_RENDER_OK;}
  if(found==p.renderers.end())p.renderers.push_back(ComponentRenderer{customId,callback,user,false});
  else {*found=ComponentRenderer{customId,callback,user,false};}
  return TC_COMPONENT_RENDER_OK;
 }
 static int component_render_set_default_drawing_api(void* context,uint64_t customId,int enabled){
  auto& p=*static_cast<Loaded*>(context);auto& r=*p.owner;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_COMPONENT_RENDER_ERR_THREAD;
  if(!p.accepting||!r.redrawComponentOriginal)return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  if(!customId||(enabled!=0&&enabled!=1))return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())return TC_COMPONENT_RENDER_ERR_OWNERSHIP;
  if(enabled)p.hiddenDefaultDrawings.erase(customId);
  else p.hiddenDefaultDrawings.insert(customId);
  return TC_COMPONENT_RENDER_OK;
 }
 static int component_render_set_selection_hint_api(void* context,uint64_t customId,int enabled){
  auto& p=*static_cast<Loaded*>(context);auto& r=*p.owner;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_COMPONENT_RENDER_ERR_THREAD;
  if(!p.accepting||!r.redrawSelectionOriginal)return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  if(!customId||(enabled!=0&&enabled!=1))return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())return TC_COMPONENT_RENDER_ERR_OWNERSHIP;
  if(enabled)p.hiddenSelectionHints.erase(customId);
  else p.hiddenSelectionHints.insert(customId);
  r.hintSuppressedFrame=-1;r.hintSuppressedLoggedCount=-1;
  return TC_COMPONENT_RENDER_OK;
 }
 static int component_render_set_foundry_button_api(void* context,uint64_t customId,int enabled){
  auto& p=*static_cast<Loaded*>(context);auto& r=*p.owner;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_COMPONENT_RENDER_ERR_THREAD;
  if(!p.accepting||!r.igButtonOriginal)return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  if(!customId||(enabled!=0&&enabled!=1))return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())return TC_COMPONENT_RENDER_ERR_OWNERSHIP;
  if(enabled)p.hiddenFoundryButtons.erase(customId);
  else p.hiddenFoundryButtons.insert(customId);
  r.hiddenFoundryFrame=-1;r.hiddenFoundryLoggedCount=-1;
  return TC_COMPONENT_RENDER_OK;
 }
 /* V5: the PNG the game shows for this type - the item in its component
    column, the drawer's preview picture and the placement ghost (see the
    comment above armPictureControl).  Called during tc_mod_load, or later from
    a frame callback to change the picture; NULL (or a path whose file is gone
    when the game asks) puts the type back on the game's own picture.  The file
    is read by the loader, never by the game directly, and the loader's copy is
    refreshed whenever the Mod's own file changes. */
 static int component_render_set_picture_api(void* context,uint64_t customId,const char* pngPath){
  auto& p=*static_cast<Loaded*>(context);auto& r=*p.owner;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_COMPONENT_RENDER_ERR_THREAD;
  if(!r.createTextureUnsafeOriginal)return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  if(!customId)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())return TC_COMPONENT_RENDER_ERR_OWNERSHIP;
  if(!pngPath||!*pngPath){
   if(p.pictures.erase(customId))
    r.logger("Component picture: "+p.id+" "+hexValue(customId)+" -> the game's own picture");
   return TC_COMPONENT_RENDER_OK;
  }
  const std::string path(pngPath);
  /* An absolute path is the contract: a relative one would be resolved against
     whatever the game's working directory happens to be at draw time. */
  const bool absolute=path.size()>=3&&path[1]==':'&&(path[2]=='\\'||path[2]=='/');
  if(!absolute&&path.compare(0,2,"\\\\")!=0)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  p.pictures[customId]=path;
  r.pictureMissing.erase(customId);
  r.logger("Component picture: "+p.id+" "+hexValue(customId)+" -> \""+path+"\"");
  return TC_COMPONENT_RENDER_OK;
 }
 static int component_render_set_placement_preview_api(void* context,uint64_t customId,int enabled){
  auto& p=*static_cast<Loaded*>(context);auto& r=*p.owner;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_COMPONENT_RENDER_ERR_THREAD;
  if(!p.accepting||!r.redrawClipboardComponentOriginal||!r.hideClipboardOriginal||
     !r.updateStateClipboardOriginal)
   return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  if(!customId||(enabled!=0&&enabled!=1))return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())
   return TC_COMPONENT_RENDER_ERR_OWNERSHIP;
  if(enabled)p.placementPreviews.insert(customId);
  else {
   p.placementPreviews.erase(customId);
   if(r.placementPreview.owner==&p&&r.placementPreview.customId==customId)
    r.placementPreview=PlacementPreview{};
  }
  return TC_COMPONENT_RENDER_OK;
 }
 int queryComponentRenderService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;
  if(!renderWorldToScreen||!renderGetIo||!renderGetMainViewport||!renderGetBackgroundDrawList||
     !renderLine||!renderRect||!renderRectFilled||!renderCircle||!renderCircleFilled||!renderText||
     !renderPushClip||!renderPopClip)return TC_SERVICE_ERR_UNAVAILABLE;
  if(version==TC_COMPONENT_RENDER_API_VERSION_1){
   if(outSize<sizeof(TCComponentRenderApiV1))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentRenderApiV1*>(out)=TCComponentRenderApiV1{
       sizeof(TCComponentRenderApiV1),TC_COMPONENT_RENDER_API_VERSION_1,&p,
       &component_render_set_callback_api};
   return TC_SERVICE_OK;
  }
  if(version==TC_COMPONENT_RENDER_API_VERSION_2){
   if(!redrawComponentOriginal)return TC_SERVICE_ERR_UNAVAILABLE;
   if(outSize<sizeof(TCComponentRenderApiV2))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentRenderApiV2*>(out)=TCComponentRenderApiV2{
       sizeof(TCComponentRenderApiV2),TC_COMPONENT_RENDER_API_VERSION_2,&p,
       &component_render_set_callback_api,&component_render_set_default_drawing_api};
   return TC_SERVICE_OK;
  }
  if(version==TC_COMPONENT_RENDER_API_VERSION_3){
   if(!redrawComponentOriginal||!redrawSelectionOriginal)return TC_SERVICE_ERR_UNAVAILABLE;
   if(outSize<sizeof(TCComponentRenderApiV3))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentRenderApiV3*>(out)=TCComponentRenderApiV3{
       sizeof(TCComponentRenderApiV3),TC_COMPONENT_RENDER_API_VERSION_3,&p,
       &component_render_set_callback_api,&component_render_set_default_drawing_api,
       &component_render_set_selection_hint_api};
   return TC_SERVICE_OK;
  }
  if(version==TC_COMPONENT_RENDER_API_VERSION_4){
   if(!redrawComponentOriginal||!redrawSelectionOriginal||!igButtonOriginal)return TC_SERVICE_ERR_UNAVAILABLE;
   if(outSize<sizeof(TCComponentRenderApiV4))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentRenderApiV4*>(out)=TCComponentRenderApiV4{
       sizeof(TCComponentRenderApiV4),TC_COMPONENT_RENDER_API_VERSION_4,&p,
       &component_render_set_callback_api,&component_render_set_default_drawing_api,
       &component_render_set_selection_hint_api,&component_render_set_foundry_button_api};
  return TC_SERVICE_OK;
 }
  if(version==TC_COMPONENT_RENDER_API_VERSION_5){
   /* The picture control is the one hook V5 adds; the V4 cut is required on
      top of it because the table keeps that prefix. */
   if(!redrawComponentOriginal||!redrawSelectionOriginal||!igButtonOriginal||
      !createTextureUnsafeOriginal)
    return TC_SERVICE_ERR_UNAVAILABLE;
   if(outSize<sizeof(TCComponentRenderApiV5))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentRenderApiV5*>(out)=TCComponentRenderApiV5{
       sizeof(TCComponentRenderApiV5),TC_COMPONENT_RENDER_API_VERSION_5,&p,
       &component_render_set_callback_api,&component_render_set_default_drawing_api,
       &component_render_set_selection_hint_api,&component_render_set_foundry_button_api,
       &component_render_set_picture_api};
   return TC_SERVICE_OK;
  }
  if(version==TC_COMPONENT_RENDER_API_VERSION_6){
   if(!redrawComponentOriginal||!redrawSelectionOriginal||!igButtonOriginal||
      !createTextureUnsafeOriginal||!redrawClipboardComponentOriginal||
      !updateStateClipboardOriginal)return TC_SERVICE_ERR_UNAVAILABLE;
   if(outSize<sizeof(TCComponentRenderApiV6))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentRenderApiV6*>(out)=TCComponentRenderApiV6{
       sizeof(TCComponentRenderApiV6),TC_COMPONENT_RENDER_API_VERSION_6,&p,
       &component_render_set_callback_api,&component_render_set_default_drawing_api,
       &component_render_set_selection_hint_api,&component_render_set_foundry_button_api,
       &component_render_set_picture_api,&component_render_set_placement_preview_api};
   return TC_SERVICE_OK;
  }
  return TC_SERVICE_ERR_VERSION;
 }
 void invokeComponentRenderer(Loaded& plugin,ComponentRenderer& renderer,
                              const TCGameHandle& component,uint64_t customId,
                              uint64_t instanceId,uint32_t rotation,
                              const RenderVec2& centre,const RenderVec2& axisX,
                              const RenderVec2& axisY,const RenderVec2& clipMin,
                              const RenderVec2& clipMax,void* list,
                              const uint8_t* config,uint32_t configSize,
                              uint32_t configSchema){
  RenderDrawContext drawContext{this,list,0,true};
  /* The newest draw table: its V1 prefix is what every existing Mod reads,
     and the appended sized-text pair is what a Mod uses to match the board's
     own text (a Mod checks draw->version/size, see sdk/tc_component_render.h). */
  const TCComponentRenderDrawV2 drawStorage{
      sizeof(TCComponentRenderDrawV2),TC_COMPONENT_RENDER_DRAW_VERSION_2,&drawContext,
      &component_render_line,&component_render_rect,&component_render_rect_filled,
      &component_render_circle,&component_render_circle_filled,&component_render_text,
      &component_render_text_sized,&component_render_measure_text};
  const TCComponentRenderDrawV1* drawApi=
      static_cast<const TCComponentRenderDrawV1*>(static_cast<const void*>(&drawStorage));
  TCComponentRenderFrameV1 renderFrame{};
  renderFrame.size=sizeof(renderFrame);renderFrame.version=TC_COMPONENT_RENDER_FRAME_VERSION_1;
  renderFrame.component=component;renderFrame.custom_id=customId;
  renderFrame.instance_id=instanceId;renderFrame.rotation=rotation;
  renderFrame.origin_x=centre.x;renderFrame.origin_y=centre.y;
  renderFrame.axis_x_x=axisX.x;renderFrame.axis_x_y=axisX.y;
  renderFrame.axis_y_x=axisY.x;renderFrame.axis_y_y=axisY.y;
  renderFrame.clip_min_x=clipMin.x;renderFrame.clip_min_y=clipMin.y;
  renderFrame.clip_max_x=clipMax.x;renderFrame.clip_max_y=clipMax.y;
  renderFrame.config=config;renderFrame.config_size=configSize;
  renderFrame.config_schema=configSchema;renderFrame.draw=drawApi;
  renderPushClip(list,clipMin,clipMax,true);
  fault::Scope mark(plugin.id.c_str(),"component render callback");
  try{renderer.draw(renderer.user,&renderFrame);}
  catch(...){renderer.failed=true;logger("Component render callback threw: "+plugin.id);
   statuses[plugin.id]="元件绘制回调异常：本次运行已停用";statusLevels[plugin.id]=2;}
  drawContext.active=false;renderPopClip(list);
 }
 void dispatchComponentRender(){
  bool any=false;for(auto& plugin:loaded)if(plugin->active)for(auto& item:plugin->renderers)if(item.draw&&!item.failed){any=true;break;}
  if(!any)return;
  TCGameHandle board{};if(gameHandles.current(TC_GAME_OBJECT_BOARD,&board)!=TC_HANDLE_OK)return;
  void* io=renderGetIo();void* viewport=renderGetMainViewport();
  if(!io||!viewport)return;
  const RenderVec2 display=*reinterpret_cast<const RenderVec2*>(static_cast<const unsigned char*>(io)+8);
  if(!renderFinite(display.x)||!renderFinite(display.y)||display.x<=8.f||display.y<=8.f)return;
  RenderVec2 clipMin{0.f,0.f},clipMax=display;
  /* Test-only diagnostic override.  It lets the true-game framebuffer test put
     all four clip edges over otherwise unobstructed board pixels.  Normal runs
     never set it and continue to receive the full viewport. */
  static bool diagnosticClipRead=false,diagnosticClipEnabled=false,diagnosticClipLogged=false;
  static float diagnosticClip[4]{};
  if(!diagnosticClipRead){
   diagnosticClipRead=true;char value[160]{};
   const DWORD length=GetEnvironmentVariableA("TC_MODLOADER_RENDER_CLIP",value,sizeof(value));
   diagnosticClipEnabled=length>0&&length<sizeof(value)&&
       std::sscanf(value,"%f,%f,%f,%f",&diagnosticClip[0],&diagnosticClip[1],
                   &diagnosticClip[2],&diagnosticClip[3])==4;
  }
  if(diagnosticClipEnabled&&renderFinite(diagnosticClip[0])&&renderFinite(diagnosticClip[1])&&
     renderFinite(diagnosticClip[2])&&renderFinite(diagnosticClip[3])&&
     diagnosticClip[0]>=0.f&&diagnosticClip[1]>=0.f&&diagnosticClip[2]<=display.x&&
     diagnosticClip[3]<=display.y&&diagnosticClip[2]>diagnosticClip[0]+1.f&&
     diagnosticClip[3]>diagnosticClip[1]+1.f){
   clipMin={diagnosticClip[0],diagnosticClip[1]};clipMax={diagnosticClip[2],diagnosticClip[3]};
   if(!diagnosticClipLogged){diagnosticClipLogged=true;logger(
       "Component render: diagnostic clip "+std::to_string(clipMin.x)+","+
       std::to_string(clipMin.y)+".."+std::to_string(clipMax.x)+","+
       std::to_string(clipMax.y));}
  }
  void* list=renderGetBackgroundDrawList(viewport);if(!list)return;
  const void* boardRaw=nullptr;if(gameHandles.resolve(&board,&boardRaw)!=TC_HANDLE_OK)return;
  board_objects::Arrays arrays{};if(!board_objects::readArrays(boardRaw,arrays))return;
  uint64_t childGeneration=0;
  if(gameHandles.beginChildSnapshot(&board,engineFrame(),&childGeneration)!=TC_HANDLE_OK)return;
  const RenderVec2 worldOrigin=renderWorldToScreen({0.f,0.f});
  const RenderVec2 worldX=renderWorldToScreen({1.f,0.f});
  const RenderVec2 worldY=renderWorldToScreen({0.f,1.f});
  const RenderVec2 baseOrigin{worldOrigin.x*display.x,worldOrigin.y*display.y};
  const RenderVec2 ex{worldX.x*display.x-baseOrigin.x,worldX.y*display.y-baseOrigin.y};
  const RenderVec2 ey{worldY.x*display.x-baseOrigin.x,worldY.y*display.y-baseOrigin.y};
  if(!renderFinite(ex.x)||!renderFinite(ex.y)||!renderFinite(ey.x)||!renderFinite(ey.y))return;
  for(uint64_t index=0;index<arrays.components;++index){
   const unsigned char* record=arrays.componentData+board_objects::kRecordHeader+
                               index*board_objects::kComponentStride;
   if(board_objects::readU8(record,0)!=board_objects::kCustomComponentKind)continue;
   const uint64_t customId=board_objects::readU64(record,0x188);if(!customId)continue;
   TCGameHandle component{};
   if(gameHandles.issueChild(TC_GAME_OBJECT_COMPONENT,record,childGeneration,&component)!=TC_HANDLE_OK)return;
   TCComponentInfoV1 info{};board_objects::decodeComponent(record,component,&info);
   for(auto& plugin:loaded){
    if(!plugin->active)continue;
    auto renderer=std::find_if(plugin->renderers.begin(),plugin->renderers.end(),[&](const ComponentRenderer& item){return item.customId==info.custom_prototype_id&&item.draw&&!item.failed;});
    if(renderer==plugin->renderers.end())continue;
    RenderVec2 axisX=ex,axisY=ey;
    switch(info.rotation&3u){
    case 1:axisX=ey;axisY={-ex.x,-ex.y};break;
    case 2:axisX={-ex.x,-ex.y};axisY={-ey.x,-ey.y};break;
    case 3:axisX={-ey.x,-ey.y};axisY=ex;break;
    default:break;
    }
    const RenderVec2 centre{baseOrigin.x+static_cast<float>(info.x)*ex.x+static_cast<float>(info.y)*ey.x,
                            baseOrigin.y+static_cast<float>(info.x)*ex.y+static_cast<float>(info.y)*ey.y};
    std::vector<uint8_t> config=tc::logic::configOfInstance(info.custom_prototype_id,info.id);
    invokeComponentRenderer(*plugin,*renderer,component,info.custom_prototype_id,
                            info.id,info.rotation,centre,axisX,axisY,clipMin,clipMax,
                            list,config.empty()?nullptr:config.data(),
                            static_cast<uint32_t>(config.size()),
                            tc::logic::configSchemaOfInstance(info.custom_prototype_id,info.id));
   }
  }
  /* The placement record is deliberately not part of the Board snapshot: it is
     an id-less temporary object.  A clipboard redraw captures its type and
     snapped position, and the clipboard's own update (observeClipboardRecord)
     ends the preview when the game puts the record down.  Unlike the game's
     retained mesh, the callback is an immediate-mode draw, so it is re-submitted
     every frame while the game still reports that record as live. */
  const PlacementPreview preview=placementPreview;
  if(preview.active&&preview.owner&&preview.owner->active){
   auto renderer=std::find_if(preview.owner->renderers.begin(),preview.owner->renderers.end(),
       [&](const ComponentRenderer& item){return item.customId==preview.customId&&
                                                item.draw&&!item.failed;});
   if(renderer!=preview.owner->renderers.end()){
    RenderVec2 axisX=ex,axisY=ey;
    switch(preview.rotation&3u){
    case 1:axisX=ey;axisY={-ex.x,-ex.y};break;
    case 2:axisX={-ex.x,-ex.y};axisY={-ey.x,-ey.y};break;
    case 3:axisX={-ey.x,-ey.y};axisY=ex;break;
    default:break;
    }
    const RenderVec2 centre{
        baseOrigin.x+static_cast<float>(preview.x)*ex.x+static_cast<float>(preview.y)*ey.x,
        baseOrigin.y+static_cast<float>(preview.x)*ex.y+static_cast<float>(preview.y)*ey.y};
    const TCGameHandle noComponent{};
    invokeComponentRenderer(*preview.owner,*renderer,noComponent,preview.customId,0,
                            preview.rotation,centre,axisX,axisY,clipMin,clipMax,list,
                            nullptr,0,0);
   }
  }
  if(gameHandles.valid(&board)!=1)return;
 }
 int queryTransactionService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;
  if(version==TC_TRANSACTION_API_VERSION_1){
   if(outSize<sizeof(TCTransactionApiV1))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCTransactionApiV1*>(out)=TCTransactionApiV1{sizeof(TCTransactionApiV1),TC_TRANSACTION_API_VERSION_1,&p,&transaction_begin_api,&transaction_stage_api,&transaction_commit_api,&transaction_abort_api,&transaction_status_api};
   return TC_SERVICE_OK;
  }
  if(version==TC_TRANSACTION_API_VERSION_2){
   if(outSize<sizeof(TCTransactionApiV2))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCTransactionApiV2*>(out)=TCTransactionApiV2{sizeof(TCTransactionApiV2),TC_TRANSACTION_API_VERSION_2,&p,&transaction_begin_api,&transaction_stage_api_v2,&transaction_commit_api,&transaction_abort_api,&transaction_status_api};
   return TC_SERVICE_OK;
 }
 return TC_SERVICE_ERR_VERSION;
 }
 void dropTransactions(Loaded& p){for(auto& item:transactions)if(item.second.owner==&p&&(item.second.status.state==TC_TRANSACTION_STATE_OPEN||item.second.status.state==TC_TRANSACTION_STATE_QUEUED)){item.second.status.state=TC_TRANSACTION_STATE_ABORTED;item.second.status.result=TC_TRANSACTION_ERR_UNAVAILABLE;item.second.status.completed_frame=engineFrame();}}
 /* The directory this level's board lives in, from the level's own campaign meta
    ("kind = architecture"): see saveCurrentCircuit for why the level's name is
    not that directory.  Empty when there is no meta to read (a level this build
    does not ship, or a malformed one): the caller then keeps the older
    name-based behaviour instead of inventing a path. */
 std::string levelSchematicKind() const{
  if(currentLevelName.empty())return std::string();
  const fs::path meta=core.root/L"campaign"/fs::u8path(currentLevelName)/L"meta.txt";
  std::error_code ec;
  if(!fs::exists(meta,ec)||!fs::is_regular_file(meta,ec))return std::string();
  std::string text;
  try{text=read(meta);}catch(...){return std::string();}
  const auto trim=[](std::string& value){
   const auto notSpace=[](char c){return !std::isspace(static_cast<unsigned char>(c));};
   value.erase(value.begin(),std::find_if(value.begin(),value.end(),notSpace));
   value.erase(std::find_if(value.rbegin(),value.rend(),notSpace).base(),value.end());
  };
  size_t start=0;
  while(start<=text.size()){
   const size_t end=text.find('\n',start);
   std::string line=text.substr(start,end==std::string::npos?std::string::npos:end-start);
   start=end==std::string::npos?text.size()+1:end+1;
   if(!line.empty()&&line.back()=='\r')line.pop_back();
   const size_t eq=line.find('=');
   if(eq==std::string::npos)continue;
   std::string key=line.substr(0,eq),value=line.substr(eq+1);
   trim(key);trim(value);
   if(key!="kind")continue;
   if(value.empty()||value=="."||value==".."||
      value.find_first_of("/\\:*?\"<>|")!=std::string::npos)
    return std::string();
   return value;
  }
  return std::string();
 }
 int saveCurrentCircuit(const void* boardModel){
  if(!boardModel||saveRoot.empty()||currentLevelName.empty())return TC_COMMAND_ERR_STATE;
  /* Which schematic directory the board belongs in is not the level's name.
     A campaign level names the directory it is stored in through its own meta
     ("campaign/<level>/meta.txt", "kind = …"), and *that* is the directory the
     game reads and writes: "The Sandbox" (campaign/sandbox) is kind
     "architecture", so its board lives in
     schematics/architecture/Default/circuit.data.  Writing under the level's
     name instead put the configuration where nothing ever looks for it - the
     player's own report of 2026-09-26 ("重进的时候，常量值输出变为默认") after a
     level was left and entered again, measured with one fixture in each of the
     two candidate directories (only the kind's board is loaded). */
  const std::string kind=levelSchematicKind();
  const std::string directory=kind.empty()?currentLevelName:kind;
  /* A directory component, never a path supplied by a plugin: keep the command
     confined to this profile even if a malformed load event is observed. */
  if(directory=="."||directory==".."||
     directory.find_first_of("/\\:*?\"<>|")!=std::string::npos)
   return TC_COMMAND_ERR_STATE;
  void* save=ioAlias(aliases,"save.schematic");
  void* settingGetter=ioAlias(aliases,"sim.setting.get");
  if(!save||!settingGetter)return TC_COMMAND_ERR_UNAVAILABLE;
  try{
   /* Board handles deliberately resolve to load_level's first argument: the
      whole board model.  The schematic writer additionally wants the embedded
      Board at +0x78 and the model-owned field at +0x48. */
   auto* model=const_cast<unsigned char*>(static_cast<const unsigned char*>(boardModel));
   void* board=model+0x78;
   void* modelField=nullptr;std::memcpy(&modelField,model+0x48,sizeof(modelField));
   const auto targetPath=saveRoot/"schematics"/fs::u8path(directory)/"Default"/"circuit.data";
   fs::create_directories(targetPath.parent_path());
   const std::string text=targetPath.generic_u8string();
   std::vector<char> payload(8+text.size()+1,0);
   const uint64_t capacity=uint64_t(text.size())|(uint64_t(1)<<62);
   std::memcpy(payload.data(),&capacity,sizeof(capacity));
   std::memcpy(payload.data()+8,text.data(),text.size());
   TCNimString path{uint64_t(text.size()),payload.data()};
   uint32_t count=0;auto found=aliases.find("save.count");
   if(found!=aliases.end()&&found->second)
    count=static_cast<uint32_t>(*reinterpret_cast<const int64_t*>(found->second));
   dispatchEvent(TC_EVENT_SAVE,count,nullptr,nullptr);
   const uint64_t setting=reinterpret_cast<uint64_t(*)(uint32_t)>(settingGetter)(2);
   reinterpret_cast<void(*)(const TCNimString*,void*,void*,void*,uint64_t)>(save)(
       &path,model,modelField,board,setting);
   logger("Circuit save: wrote current Board to "+text+
          (directory==currentLevelName
               ?std::string()
               :" (level \""+currentLevelName+"\" keeps its board in the \""+directory+
                "\" schematic the level's own meta names as its kind)"));
  }catch(...){return TC_COMMAND_ERR_EXECUTION;}
  return TC_COMMAND_OK;
 }
 int executeCommand(const TCCommandV2& command,Loaded& owner){
  const void* model=nullptr;if(gameHandles.resolve(&command.subject,&model)!=TC_HANDLE_OK)return TC_COMMAND_ERR_STALE;
  fault::Scope mark(owner.id.c_str(),"a command bus request");
  if(command.type==TC_COMMAND_BOARD_PLACE_COMPONENT){
   /* The menu helper takes the Board model itself (verified by
      tests/component-placement-probe.cpp), so a Board handle is enough. */
   if(!addComponent)return TC_COMMAND_ERR_UNAVAILABLE;
   unsigned char record[board_edits::kComponentRecordSize];
   board_edits::buildPlacement(board_edits::placementKind(command),command.custom_prototype_id,
                               static_cast<int16_t>(command.x),static_cast<int16_t>(command.y),
                               command.rotation,record);
   try{
    if(!addComponent(const_cast<void*>(model),record))return TC_COMMAND_ERR_STATE;
   }catch(...){return TC_COMMAND_ERR_EXECUTION;}
   /* The game's own placement calls the presenter upgrade right after
      add_component succeeds.  Without it the new component never enters the
      board's hit state: it is invisible to the mouse (cannot be selected or
      dragged) until something else refreshes the board.  Measured 2026-09-22:
      a component placed by hand made the three command-bus placements
      draggable with it, and replaying the same upgrade reproduced that. */
   if(afterPlaceSlot&&afterPlaceOriginal){
    reinterpret_cast<void(*)(void*,uint8_t)>(afterPlaceOriginal)(afterPlaceSlot,0x30);
   }else{
    static bool warned=false;
    if(!warned){
     warned=true;
     logger("Board registration: the presenter slot is not known yet; the placed component may not be clickable until the board refreshes");
    }
   }
   return TC_COMMAND_OK;
  }
  if(command.type==TC_COMMAND_BOARD_DUPLICATE_COMPONENT){
   /* `argument` names the source instance and `custom_prototype_id` its type: a
      duplication that cannot name both would silently copy the wrong thing. */
   if(!addComponent)return TC_COMMAND_ERR_UNAVAILABLE;
   if(!command.custom_prototype_id||!command.argument)return TC_COMMAND_ERR_ARGUMENT;
   if(!duplicateComponent(const_cast<void*>(model),command.custom_prototype_id,
                          static_cast<uint64_t>(command.argument),
                          static_cast<int16_t>(command.x),static_cast<int16_t>(command.y),
                          static_cast<uint8_t>(command.rotation)))
    return TC_COMMAND_ERR_STATE;
   return TC_COMMAND_OK;
  }
  if(command.type==TC_COMMAND_SAVE)return saveCurrentCircuit(model);
  const char* alias=command.type<=TC_COMMAND_SIM_RESET?"sim.do":(command.type==TC_COMMAND_BOARD_UNDO?"board.undo":"board.redo");
  auto target=aliases.find(alias);if(target==aliases.end()||!target->second)return TC_COMMAND_ERR_UNAVAILABLE;
  try{
   if(command.type<=TC_COMMAND_SIM_RESET){
    const uint8_t action=command.type==TC_COMMAND_SIM_RUN?0:(command.type==TC_COMMAND_SIM_STOP?1:2);
    const int64_t argument=command.type==TC_COMMAND_SIM_RUN?command.argument:(command.type==TC_COMMAND_SIM_RESET?-1:0);
    reinterpret_cast<void(*)(void*,uint8_t,int64_t)>(target->second)(const_cast<void*>(model),action,argument);
   }else if(command.type==TC_COMMAND_BOARD_UNDO||command.type==TC_COMMAND_BOARD_REDO){
    if(!reinterpret_cast<uint8_t(*)(void*)>(target->second)(const_cast<void*>(model)))return TC_COMMAND_ERR_STATE;
   }
  }catch(...){return TC_COMMAND_ERR_EXECUTION;}
  return TC_COMMAND_OK;
 }
 void executeCommands(int64_t frame){
  auto batch=std::move(pendingCommands);pendingCommands.clear();
  for(auto id:batch){
   auto found=commands.find(id);if(found==commands.end())continue;auto& record=found->second;
   if(record.status.state!=TC_COMMAND_STATE_QUEUED)continue;
   if(!record.owner||!record.owner->active){record.status.state=TC_COMMAND_STATE_CANCELLED;record.status.result=TC_COMMAND_ERR_UNAVAILABLE;record.status.completed_frame=frame;continue;}
   record.status.state=TC_COMMAND_STATE_RUNNING;
   const int result=executeCommand(record.command,*record.owner);
   record.status.result=result;record.status.state=result==TC_COMMAND_OK?TC_COMMAND_STATE_SUCCEEDED:TC_COMMAND_STATE_FAILED;record.status.completed_frame=frame;
   if(result!=TC_COMMAND_OK)logger("["+record.owner->id+"] command "+std::to_string(id)+" failed: "+std::to_string(result));
  }
  pruneCommands();
 }
 void executeTransactions(int64_t frame){
  auto batch=std::move(pendingTransactions);pendingTransactions.clear();
  for(auto id:batch){auto found=transactions.find(id);if(found==transactions.end())continue;auto& tx=found->second;if(tx.status.state!=TC_TRANSACTION_STATE_QUEUED)continue;
   if(!tx.owner||!tx.owner->active){tx.status.state=TC_TRANSACTION_STATE_ABORTED;tx.status.result=TC_TRANSACTION_ERR_UNAVAILABLE;tx.status.completed_frame=frame;continue;}
   BoardSummary current{};if(gameHandles.valid(&tx.board)!=1){tx.status.state=TC_TRANSACTION_STATE_CONFLICT;tx.status.result=TC_TRANSACTION_ERR_STALE;tx.status.completed_frame=frame;continue;}
   if(!readBoardSummary(tx.board,current)||(current.flags&TC_LIFECYCLE_HAS_OBJECT_COUNTS)==0||current.objectHash!=tx.baselineHash){tx.status.state=TC_TRANSACTION_STATE_CONFLICT;tx.status.result=TC_TRANSACTION_ERR_CONFLICT;tx.status.completed_frame=frame;continue;}
   tx.status.state=TC_TRANSACTION_STATE_RUNNING;int result=TC_COMMAND_OK;
   for(const auto& step:tx.steps){result=executeCommand(step,*tx.owner);if(result!=TC_COMMAND_OK)break;++tx.status.completed_count;}
   if(result==TC_COMMAND_OK&&(tx.flags&TC_TRANSACTION_SAVE_ON_COMMIT)){TCCommandV2 save{};save.size=sizeof(save);save.type=TC_COMMAND_SAVE;save.subject=tx.board;result=executeCommand(save,*tx.owner);if(result==TC_COMMAND_OK)++tx.status.completed_count;}
   tx.status.result=result;tx.status.state=result==TC_COMMAND_OK?TC_TRANSACTION_STATE_COMMITTED:TC_TRANSACTION_STATE_FAILED;tx.status.completed_frame=frame;
  }
 }
 static int register_component_api(void* c,const TCNativeComponentDefinition* d){auto& p=*(Loaded*)c;if(!p.accepting)return -1;try {int result=registerNativeComponent(&p.host,d);if(!result){p.logicIds.push_back(d->custom_id);p.componentIds.push_back(d->custom_id);p.owner->noteComponentType(p,d->custom_id,d->name,-1);}else log_api(c,("Component registration rejected: "+std::to_string(result)).c_str());return result;}catch(const std::exception& e){log_api(c,e.what());return -4;}catch(...){return -4;}}
 static int register_ui_page_api(void* c,const TCUiPageDefinition* d){auto& p=*(Loaded*)c;if(!p.accepting)return -1;if(!d||d->size<sizeof(TCUiPageDefinition)||!d->page_id||!d->page_id[0]||!d->draw)return -2;std::string id=d->page_id;if(id.size()>=64||id.find("###")!=std::string::npos)return -2;if(p.pages.size()>=kMaxPagesPerPlugin)return -4;for(auto& page:p.pages)if(page.id==id)return -3;p.pages.push_back(UiPage{id,d->title?d->title:"",d->draw,d->user,false});log_api(c,("UI page registered: "+id).c_str());return 0;}
 void reject(Loaded& p){logic::finish(p.logicIds,false);component_registry::dropMod(p.id);for(auto it=instanceFootprints.begin();it!=instanceFootprints.end();)if(it->second.owner==&p)it=instanceFootprints.erase(it);else ++it;if(!p.componentIds.empty()){TCGameModel game;if(game.load(&p.host))for(auto id:p.componentIds)game.removeCustomPrototype(id);p.componentIds.clear();}for(auto target:p.hooks){MH_DisableHook(target);MH_RemoveHook(target);ownedHooks.erase(target);}p.hooks.clear();dropHookLinks(p.id);dropEventListeners(p.id);dropLifecycleListeners(p);dropCommands(p);dropTransactions(p);if(p.plugin.on_unload){try{p.plugin.on_unload(p.plugin.user);}catch(...) {}}textures.reject(&p);p.plugin={};p.accepting=false;p.active=false;/* Keep rejected DLL mapped: it may have static destructors/threads. */}


 /* Registers a panel for one of the game's own screens.  Same validation and
    ownership rules as a page: the host copies the strings, rejects duplicates
    inside one plugin, and a rejected plugin leaves no entry behind. */
 static int register_ui_slot_api(void* c,const TCUiSlotDefinition* d){
  auto& p=*(Loaded*)c;if(!p.accepting)return -1;
 if(!d||d->size<sizeof(TCUiSlotDefinition)||!d->slot_id||!d->slot_id[0])return -2;
 /* The drawer's slot draws *for an instance*, so it is the one kind whose
    callback carries one; every other kind is drawn without. */
 const size_t componentTail=offsetof(TCUiSlotDefinition,draw_component)+
                            sizeof(d->draw_component);
 const bool componentPanel=d->kind==TC_UI_SLOT_BOARD_COMPONENT_PANEL;
 if(componentPanel){
  if(d->size<componentTail||!d->draw_component)return -2;
 }else if(!d->draw)return -2;
 if(d->kind!=TC_UI_SLOT_BOARD_SIDE&&d->kind!=TC_UI_SLOT_BOARD_TOOLBAR&&
    d->kind!=TC_UI_SLOT_BOARD_MENU&&d->kind!=TC_UI_SLOT_BOARD_COMPONENT_PANEL)return -2;
  std::string id=d->slot_id;if(id.size()>=64||id.find("###")!=std::string::npos)return -2;
  if(p.slots.size()>=kMaxSlotsPerPlugin)return -4;
  for(auto& slot:p.slots)if(slot.id==id)return -3;
  BoardSlot slot;
  slot.id=id;slot.title=d->title?d->title:"";slot.draw=d->draw;slot.user=d->user;
  slot.drawComponent=componentPanel?d->draw_component:nullptr;
  slot.kind=d->kind;
  /* A non-finite or negative request would poison the layout arithmetic. */
  slot.width=(std::isfinite(d->preferred_width)&&d->preferred_width>0.f)?d->preferred_width:0.f;
  slot.height=(std::isfinite(d->preferred_height)&&d->preferred_height>0.f)?d->preferred_height:0.f;
  p.slots.push_back(std::move(slot));
  log_api(c,("UI slot registered: "+id+(d->kind==TC_UI_SLOT_BOARD_TOOLBAR?" (board tool)":
             (d->kind==TC_UI_SLOT_BOARD_MENU?" (top menu bar)":
              (d->kind==TC_UI_SLOT_BOARD_COMPONENT_PANEL?" (component panel)":" (board side panel)")))).c_str());
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
  /* The current window's top-left corner, for a slot that has to place its rows
     at a fixed spot inside a game window (optional). */
  void (*windowPos)(Vec2*)=nullptr;
  void (*cursorScreen)(Vec2*)=nullptr;
  void (*mouseScreen)(Vec2*)=nullptr;
  void (*setCursorPos)(Vec2)=nullptr;
  /* Absolute placement, for tools that line up under the game's own controls.
     Optional: without it a tool is drawn wherever the cursor happens to be. */
  void (*setCursorScreen)(Vec2)=nullptr;
  /* Stay on the current row: used by the top menu bar, whose own entries are
     laid out left to right with SameLine.  spacing < 0 means "use the style's
     ItemSpacing", which is what the bar pushes for its own buttons. */
  void (*sameLine)(float,float)=nullptr;
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
   value.windowPos=(decltype(value.windowPos))proc(module,"igGetWindowPos");
   value.cursorScreen=(decltype(value.cursorScreen))proc(module,"igGetCursorScreenPos");
   value.mouseScreen=(decltype(value.mouseScreen))proc(module,"igGetMousePos");
   value.setCursorPos=(decltype(value.setCursorPos))proc(module,"igSetCursorPos");
   value.sameLine=(decltype(value.sameLine))proc(module,"igSameLine");
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
    {"igSameLine",(const void*)value.sameLine},
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
 /* Controls registered into the game's own top menu bar
    (TC_UI_SLOT_BOARD_MENU).  Drawn from inside that bar's button row, after the
    game's last button and while the button style it pushed (frame padding,
    rounding, item spacing and the three button colours) is still in effect, so a
    plugin's control comes out looking like the bar's own entries and lands on
    the same line. */
 bool hasMenuItems() const {
  for(auto& p:loaded){if(!p->active)continue;for(auto& slot:p->slots)if(slot.kind==TC_UI_SLOT_BOARD_MENU&&!slot.failed)return true;}
  return false;
 }
 bool boardMenuFrame(){
  if(!hasMenuItems())return false;
  const SlotApi& api=slotApi();
  if(!api.available()){
   static bool reported=false;
   if(!reported){reported=true;logger("Board menu bar API unavailable: "+api.missing);}
   return false;
  }
  const int frame=api.getFrameCount();
  if(frame==lastMenuFrame)return false;
  lastMenuFrame=frame;
  const float width=api.getWindowWidth(),height=api.getWindowHeight();
  /* The bar borrows the game's text font for its own labels and pops it right
     before this point, so the plugin draw has to borrow it too: the column's
     icon font is what is current otherwise, and that font has no Latin glyphs. */
  const bool borrowedFont=toolTextFontIndex>=0&&pushGameFont&&popGameFont;
  if(borrowedFont)pushGameFont((unsigned char)toolTextFontIndex);
  TCFrame tick{sizeof(TCFrame),frame,api.getTime()};
  bool drawn=false;
  std::vector<Loaded*> order;
  for(auto& p:loaded)if(p->active)order.push_back(p.get());
  std::sort(order.begin(),order.end(),[](Loaded* a,Loaded* b){return a->id<b->id;});
  for(Loaded* p:order){
   std::vector<BoardSlot*> slots;
   for(auto& slot:p->slots)if(slot.kind==TC_UI_SLOT_BOARD_MENU&&!slot.failed)slots.push_back(&slot);
   std::sort(slots.begin(),slots.end(),[](BoardSlot* a,BoardSlot* b){return a->id<b->id;});
   for(BoardSlot* slot:slots){
    /* Stay on the game's row: its own entries are laid out with SameLine, and
       spacing < 0 keeps the ItemSpacing the bar pushed. */
    if(api.sameLine)api.sameLine(0.f,-1.f);
    try{
     if(api.pushId)api.pushId(p->id.c_str());
     if(api.pushId)api.pushId(slot->id.c_str());
     slot->draw(slot->user,&tick,width,height);
     if(api.popId)api.popId();
     if(api.popId)api.popId();
     ++slot->drawn;drawn=true;
     if(slot->drawn==1)
      logger("Board menu item "+p->id+"/"+slot->id+" drawn in the game's top menu bar (frame "+std::to_string(frame)+")");
    }catch(const std::exception&e){slot->failed=true;logger("Board menu item "+p->id+"/"+slot->id+" failed: "+e.what());}
    catch(...){slot->failed=true;}
   }
  }
  if(borrowedFont)popGameFont();
  return drawn;
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
  void* bold=nullptr;
  /* The whole table is walked (the bold face may sit on either side of the
     regular one in a future build), but only the first face of each kind is
     kept. */
  for(int index=0;index<8;++index){
   void* font=table[index];
   if(!font)break;
   const char* text=name(font);
   const std::string face=text?text:"";
   if(face.empty()||face.find("Icon")!=std::string::npos)continue;
   /* The bold face is what the board's own part labels are set in; a Mod that
      draws a stock-style label asks for it through the draw table. */
   if(!bold&&face.find("Bold")!=std::string::npos)bold=font;
   if(toolTextFontIndex<0&&face.find("Regular")!=std::string::npos)toolTextFontIndex=index;
   if(fallback<0)fallback=index;
  }
  if(toolTextFontIndex<0)toolTextFontIndex=fallback;
  boldTextFont=bold;
  if(toolTextFontIndex<0)logger("Tool column text font: not found; plugin tool text will stay invisible");
  else {
   const char* chosen=name(table[toolTextFontIndex]);
   logger("Tool column text font: defined_fonts["+std::to_string(toolTextFontIndex)+"]="+
          (chosen?chosen:"?")+" (used for plugin tool draws), bold face "+
          (boldTextFont?std::string(name(boldTextFont)?name(boldTextFont):"?"):std::string("none"))+
          " (component labels)");
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
 /* Set by the loader before boot() when this session must not load native
    plugins for a reason of its own (today: the save redirect could not be
    applied).  Same effect as the Shift safe mode, but explained in the log and
    on the Mods page instead of silent. */
 std::string forcedSafeMode;
 std::map<std::string,std::string> statuses;
 /* Severity of the message in `statuses` for the same id: 0 info, 1 warning,
    2 error.  Absent means "the loader itself wrote this line" (it is shown in
    the neutral colour).  A plugin's own report also lands here, so the Mods
    page can show it the same way as a loader-detected failure. */
 std::map<std::string,int> statusLevels;
NativeRuntime(Core& c,HMODULE e,fs::path saves,std::function<void(const std::string&)> log):core(c),engine(e),saveRoot(std::move(saves)),types(c.dir/L"registry.json"),logger(std::move(log)),services(gameHandles){services.bindBoardSnapshot(this,&NativeRuntime::captureBoardSnapshotService);services.bindBoardObjects(&NativeRuntime::captureBoardObjectsService);services.bindBoardReaders(&NativeRuntime::readComponentService,&NativeRuntime::readWireService);services.bindBoardPinReader(&NativeRuntime::readPinsService);services.bindBoardWireEndsReader(&NativeRuntime::readWireEndsService);services.bindSimulation(this,&NativeRuntime::simulationStateService,&NativeRuntime::simulationValueService);services.bindSimulationControl(this,&NativeRuntime::simulationCycleService,&NativeRuntime::simulationStateSizeService,&NativeRuntime::simulationSnapshotService,&NativeRuntime::simulationRunToService,&NativeRuntime::simulationRunForService,&NativeRuntime::simulationPauseService,&NativeRuntime::simulationResetService,&NativeRuntime::simulationStepService,&NativeRuntime::simulationSliceService,&NativeRuntime::simulationControlService);services.bindSimChannels(this,&NativeRuntime::channelFromWireService,&NativeRuntime::channelResolveService);services.bindSimCapture(this,&NativeRuntime::captureConfigureService,&NativeRuntime::captureStartService,&NativeRuntime::captureStopService,&NativeRuntime::captureReadService,&NativeRuntime::captureStatusService);services.bindIoValue(this,&NativeRuntime::ioEvaluateService,&NativeRuntime::ioFormatService,&NativeRuntime::ioReadInputService,&NativeRuntime::ioWriteInputService,&NativeRuntime::ioFlipInputService,&NativeRuntime::ioInputWidthService,&NativeRuntime::ioWriteConstantService,&NativeRuntime::ioWriteConstantSlotService);activeInstance=this;}
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
 void boot(){if(started)return;started=true;gameThreadId=GetCurrentThreadId();textures.advance(0);try{
  /* The fault journal must be armed before the first plugin callback runs, so a
     crash inside a plugin callback is attributed instead of anonymous. */
  fault::install((core.dir/L"fault.log").c_str());
  if(!forcedSafeMode.empty()){
   /* The loader's own reason to skip plugins (a missing save redirect today):
      say it in the log and in every plugin's status line, so the Mods page can
      explain why nothing loaded instead of leaving the player guessing. */
   logger("Native safe mode: "+forcedSafeMode+" - plugins skipped");
   for(auto& id:core.enabled_set())statuses[id]="本次未加载："+forcedSafeMode;
   return;
  }
  if(GetAsyncKeyState(VK_SHIFT)&0x8000){logger("Native safe mode: Shift held; plugins skipped");for(auto& id:core.enabled_set())statuses[id]="安全模式：本次未加载";return;}
  symbols=std::make_unique<Symbols>(core.root/L"Turing Complete.exe");auto mh=MH_Initialize();if(mh!=MH_OK&&mh!=MH_ERROR_ALREADY_INITIALIZED)throw std::runtime_error("MinHook initialization failed");
  logger("TC Mod Loader "+std::string(TC_MODLOADER_VERSION_STRING)+" capabilities: "+capability_names(loader_capabilities()));
armDefaultDrawingControl();
armPlacementPreviewControl();
armSelectionHintControl();
 armFoundryButtonControl();
 armComponentPanelControl();
armPictureControl();
armSelectionMeshTrace();
 buildChains();
 bindBoardSnapshotSources();
  bindBoardPinSources();
 bindTailStorageSources();
  armMissingModCapture();
  armConfigUndo();
  armSaveDispatch();
  /* Board handles and the SCENE_CHANGE event both hang off scene.change, so its
     detour is taken before any plugin gets a chance to hook it (see
     armSceneChangeTracking). */
  armSceneChangeTracking();
  /* A placement through the command bus has to re-register the new component
     with the board exactly like the game's own placement does, so the presenter
     slot is learned before any plugin can place anything. */
  armBoardRegistration();
  armInstanceFootprintHitTest();
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
p->host={sizeof(TCHost),TC_MOD_API_VERSION,p.get(),TC_GAME_BUILD_LABEL,p->id.c_str(),p->folder.c_str(),log_api,resolve,engine_api,hook_api,register_logic_api,register_component_api,register_ui_page_api,create_ui_texture_api,load_ui_texture_api,release_ui_texture_api,register_ui_slot_api,TC_MODLOADER_VERSION_CODE,0u,loader_capabilities(),report_status_api,resolve_alias_api,register_hook_chain_api,add_event_listener_api,get_current_game_handle_api,validate_game_handle_api,resolve_game_handle_api,query_service_api};p->plugin.size=sizeof(TCPlugin);
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
 void frame(){if(inside)return;inside=true;boot();
  /* One line the first time a compile carries the per-cycle scope tick: "the
     capture never sees a cycle" and "the program was never instrumented" are
     different reports, and this tells them apart in the log. */
  {const uint64_t sites=tc::scope_capture::store().injectedSites();
   static uint64_t reported=0;
   if(sites!=reported){reported=sites;
    logger("Scope tick: instrumented "+std::to_string(sites)+" per-cycle call site(s) so far");}}
auto getFrame=(int(*)())GetProcAddress(engine,"igGetFrameCount");auto getTime=(double(*)())GetProcAddress(engine,"igGetTime");int n=getFrame();lastEngineFrame=n;gameHandles.beginFrame(n);textures.advance(n);if(n!=lastFrame){lastFrame=n;observeBoardChanges();
  /* Some custom instances are bound while level.load is still flattening the
     Board, before their saved tail records can be found.  Finish those restores
     automatically on the first frame that can see the records, before plugin
     on_frame callbacks attempt a refresh or render an editor. */
  tc::logic::restorePendingTailConfigs();
  /* One cycle sample per frame is what "the simulation is advancing" means
     here: a configuration write refreshes the board only when it is not (see
     the storage write path in native_logic.hpp). */
  TCFrame f{sizeof(TCFrame),n,getTime()};for(auto& p:loaded)if(p->active&&p->plugin.on_frame){
   /* A faulting per-frame callback is dropped for the rest of the process: the
      game keeps running, the Mods page says what happened. */
   fault::Scope mark(p->id.c_str(),"on_frame");
   try{p->plugin.on_frame(p->plugin.user,&f);}catch(...){logger("Plugin callback threw: "+p->id);statuses[p->id]="回调异常：请停用后重启";statusLevels[p->id]=2;p->plugin.on_frame=nullptr;}
  }dispatchComponentRender();executeCommands(n);executeTransactions(n);
  /* A level load is over by the time the next frame runs, so this is where the
     components that did not survive it are reported, the ones at risk are kept
     for later, and the ones whose owner is back are put on the Board again. */
  if(capturePending){capturePending=false;captureWindow=false;
   if(reportMissingMods())writeRescueStore();
   rescueMissingMods();}}inside=false;}
};
}
