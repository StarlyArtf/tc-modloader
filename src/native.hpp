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
#include "builtin_catalogue.hpp"
#include "component_netlist.hpp"
#include "save_deps.hpp"
#include "board_pins.hpp"
#include "simulation_read.hpp"
#include "sim_control.hpp"
#include "board_edits.hpp"
#include "board_connect.hpp"
#include "component_geometry.hpp"
#include "component_editor.hpp"
#include "component_images.hpp"
#include "component_input.hpp"
#include "component_channel.hpp"
#include "mod_storage.hpp"
#include "mod_settings.hpp"
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
 struct ComponentFont {uint64_t handle=0,texture=0;uint32_t revision=1;float nativeSize=0.f;
  void* atlas=nullptr;void* font=nullptr;};
struct Loaded {NativeRuntime* owner;std::string id,folder,status;fs::path packageRoot;TCHost host{};TCPlugin plugin{};HMODULE dll{};std::vector<void*> hooks;std::vector<uint64_t> logicIds,componentIds;std::vector<UiPage> pages;std::vector<BoardSlot> slots;std::vector<ComponentRenderer> renderers;std::set<uint64_t> hiddenDefaultDrawings;std::set<uint64_t> hiddenSelectionHints;std::set<uint64_t> hiddenFoundryButtons;std::set<uint64_t> placementPreviews;
  /* Ids this Mod registered while the game ran and then removed again.  The
     game's delete drops the id from the ordered live list but leaves the entry
     in its own prototype map, so a later add_custom_prototype of the same id is
     refused as a duplicate.  A re-registration of one of these ids therefore
     goes through the update path (update_custom_prototype), which rewrites
     every index the game keeps for that id and puts it back in the live list. */
  std::set<uint64_t> retiredIds;
  /* tc.component.render V7: the images this Mod loaded for its own draw
     callbacks.  The GL objects live in the texture manager (owner = this), this
     table only keeps the names, the handles and the revisions. */
  component_images::Table images;
  std::map<std::string,ComponentFont> fonts;
  TCComponentRenderDeviceCallbackV1 deviceCallback=nullptr;void* deviceUser=nullptr;
  uint64_t deviceGeneration=0;
  /* tc.component.editor V1: the editable rows a Mod declared for its own
     types, keyed by custom id.  The host draws them over the game's own
     component drawer and commits each edit through the configuration path
     tc.component.storage uses, so a Mod ships a table instead of a window. */
  std::map<uint64_t,std::vector<component_editor::Field>> editorFields;
  /* ...and the table-level flags that came with them (V4's CONFIRM).  Kept
     beside the rows rather than inside them: the flags describe the table, not
     any one field. */
  std::map<uint64_t,uint32_t> editorTableFlags;
   std::vector<mod_settings::Field> settings;
   TCModSettingChangedV1 settingChanged=nullptr;void* settingUser=nullptr;
 /* V5: the PNG a type is shown with in the game's own component column, its
    drawer preview and the placement ghost (custom id -> absolute path). */
  std::map<uint64_t,std::string> pictures;bool accepting=true,active=false,reported=false;};
 struct InstanceFootprint {Loaded* owner=nullptr;uint64_t customId=0;float halfWidth=0.f,halfHeight=0.f;};
 ui_texture::Manager textures;
 Core& core;HMODULE engine;fs::path saveRoot;std::string currentLevelName;
 mod_storage::Store modStorage;
 component_input::Router componentInputs;
 component_channel::Store componentChannels;
 UiPage settingsPage;
 /* id → Mod/type cache the save page reads (src/save_deps.hpp): the only place
    the mapping exists, because a circuit file stores just the custom_id.  It
    lives with the loader's other data, not in any save. */
 TypeRegistry types;
 std::unique_ptr<Symbols> symbols;std::vector<std::unique_ptr<Loaded>> loaded;std::set<void*> ownedHooks;std::map<void*,std::string> loaderOwned;std::function<void(const std::string&)> logger;int lastFrame=-1,lastUiFrame=-1,lastSlotFrame=-1,lastToolFrame=-1,lastComponentInputFrame=-1;int64_t lastEngineFrame=-1;DWORD gameThreadId=0;bool inside=false,started=false;
 std::map<uint64_t,InstanceFootprint> instanceFootprints;
 std::set<uint32_t> componentKeysDown;
 float componentWheelX=0.f,componentWheelY=0.f;
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
 /* Runtime component edits change the game's global prototype list while the
    editor is on the main menu.  A board presenter keeps its own menu copy, so
    the first component-column draw after a mutation must rebuild that exact
    copy.  Hooking build (not reload_component_menu itself) leaves diagnostic
    Mods free to hook the reload function. */
 using BuildComponentMenu=void(*)(void*,void*,void*,void*,uint64_t);
 using ReloadComponentMenu=void(*)(void*,void*,uint8_t);
 BuildComponentMenu buildComponentMenuOriginal=nullptr;
 ReloadComponentMenu reloadComponentMenu=nullptr;
 void* buildComponentMenuTarget=nullptr;
 uint64_t componentMenuGeneration=0;
 uint64_t componentMenuAppliedGeneration=0;
 void* componentMenuAppliedContext=nullptr;
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
 /* ImDrawList_AddImageQuad: the same entry the clock Mod used by hand before
    tc.component.render V7 took the image path into the loader. */
 using DrawImageQuad=void(*)(void*,uint64_t,RenderVec2,RenderVec2,RenderVec2,RenderVec2,
                             RenderVec2,RenderVec2,RenderVec2,RenderVec2,uint32_t);
 using PushClipRect=void(*)(void*,RenderVec2,RenderVec2,bool);
 using PopClipRect=void(*)(void*);
 using PushTextureId=void(*)(void*,uint64_t);
 using PopTextureId=void(*)(void*);
 WorldToScreen renderWorldToScreen=nullptr;
 GetIo renderGetIo=nullptr;
 GetMainViewport renderGetMainViewport=nullptr;
 GetBackgroundDrawList renderGetBackgroundDrawList=nullptr;
 DrawLine renderLine=nullptr;DrawRect renderRect=nullptr;DrawRectFilled renderRectFilled=nullptr;
 DrawCircle renderCircle=nullptr;DrawCircleFilled renderCircleFilled=nullptr;DrawText renderText=nullptr;
 DrawImageQuad renderImageQuad=nullptr;
 PushClipRect renderPushClip=nullptr;PopClipRect renderPopClip=nullptr;
 PushTextureId renderPushTexture=nullptr;PopTextureId renderPopTexture=nullptr;
 /* ImFont_CalcTextSizeA: the game measures its own labels with it, and the draw
    table's measure_text answers with the same numbers (see renderTextSized). */
 void (*renderCalcTextSize)(void* out,const void* font,float size,float maxWidth,
                            float wrapWidth,const char* begin,const char* end,
                            const char** remaining)=nullptr;
 /* The game's bold face, borrowed for a Mod that wants a stock part's label
    weight (TCComponentRenderDrawV2::text_sized's `bold`).  Resolved with the
    tool text font; null means "use whichever face is current". */
 void* boldTextFont=nullptr;
 /* `plugin` is whose draw callback is running: an image handle is resolved in
    that Mod's own table, so one Mod can never paint another's image. */
 struct RenderDrawContext {NativeRuntime* owner=nullptr;void* list=nullptr;uint32_t commands=0;
                           bool active=false;Loaded* plugin=nullptr;
                           TCComponentInstanceHandle instance{};};
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
 /* The built-in type catalogue behind tc.component.compose V1: one entry per
    prototype the boot enumeration found, read once with the same pin decoding
    the board reads use.  Lookups are by kind byte and by the stable spelling
    (src/builtin_catalogue.hpp owns the spelling rules). */
 std::vector<builtin_catalogue::Entry> builtinTypes;
 std::map<uint32_t,size_t> builtinTypeByKind;
 std::map<std::string,size_t> builtinTypeById;
 /* Open `tc.component.compose` V2 build sessions, keyed by the id the service
    handed the Mod.  A session is just a netlist under construction plus the
    reason the last call refused; it disappears on commit, abort, or when the
    Mod that opened it is rejected. */
 struct ComposeSession {
  Loaded* owner=nullptr;
  component_netlist::Definition definition;
  std::string status;
 };
 std::map<uint64_t,ComposeSession> composeSessions;
 uint64_t nextComposeSession=1;
 /* Simulation reads.  The state buffer global is only used to answer "is there
    a buffer"; the values themselves come from the game's own reader. */
 unsigned char** simulationState=nullptr;
 /* The component-menu add helper takes the Board model itself, which is what a
    Board handle resolves to - that is why a placement can be a queued command
    without any presenter context. */
 using AddComponent=bool(*)(void*,void*);
 /* The game's undo-aware delete: it takes the board model and two int64
    sequences (component indices, wire ids), copies what it is about to remove
    and pushes the change list onto the game's undo stack. */
 using TryDelete=bool(*)(void*,const void*,const void*);
 AddComponent addComponent=nullptr;
 TryDelete tryDelete=nullptr;
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
 /* Reads every built-in prototype once and keeps what the game declares about
    it.  Deliberately only the measured fields: the name and description the
    prototype carries, whether it carries a shape, and the pins - offsets, word
    sizes and the auto markers - decoded by the same helper the board reads use,
    so a kind's catalogue entry and a placed component's pin read can never
    disagree.  Costs and capability bits are not invented here; see the contract
    comment on TC_SERVICE_COMPONENT_COMPOSE for why. */
 void collectBuiltinCatalogue(){
  builtinTypes.clear();builtinTypeByKind.clear();builtinTypeById.clear();
  if(!prototypeGet||!prototypeDestroy||builtinKinds.empty())return;
  const auto readPins=[&](const TCPrototype& prototype,bool outputs,
                          std::vector<builtin_catalogue::Pin>& out){
   const uint64_t count=outputs?prototypeOutputCount(prototype):prototypeInputCount(prototype);
   for(uint64_t index=0;index<count&&out.size()<TC_BUILTIN_PIN_LIMIT;++index){
    const TCPin* pin=outputs?prototypeOutputPin(prototype,index):prototypeInputPin(prototype,index);
    if(!pin)continue;
    const TCPinInfoV1 info=board_pins::decodePin(*pin,outputs?TC_PIN_OUTPUT:TC_PIN_INPUT);
    builtin_catalogue::Pin entry{};
    entry.x=info.x;entry.y=info.y;entry.bits=info.bits;entry.wordSizeRaw=info.word_size_raw;
    out.push_back(entry);
   }
  };
  for(uint8_t kind:builtinKinds){
   TCPrototypeKind key{};key.tag=kind;
   TCPrototype prototype{};
   prototypeGet(&key,&prototype);
   builtin_catalogue::Entry entry;entry.kind=kind;
   if(const char* name=prototypeNameCStr(prototype))entry.name=name;
   if(const char* description=prototypeDescriptionCStr(prototype))entry.description=description;
   if(const char* shape=prototypeShapeSvgCStr(prototype))entry.hasShape=shape&&*shape;
   readPins(prototype,false,entry.inputs);
   readPins(prototype,true,entry.outputs);
   prototypeDestroy(&prototype);
   builtinTypes.push_back(std::move(entry));
  }
  /* The boot set is ordered, so the duplicate ranks the spelling uses are
     deterministic; then the two lookup tables are filled from the same pass. */
  builtin_catalogue::assignTypeIds(builtinTypes);
  for(size_t index=0;index<builtinTypes.size();++index){
   builtinTypeByKind[builtinTypes[index].kind]=index;
   builtinTypeById[builtinTypes[index].typeId]=index;
  }
  if(!builtinTypes.empty())
   logger("Component compose: built-in catalogue "+std::to_string(builtinTypes.size())+
          " kinds, "+std::to_string(builtin_catalogue::countWithoutDeclaredInputs(builtinTypes))+
          " with no declared input pin, "+
          std::to_string(builtin_catalogue::countWithShape(builtinTypes))+" with a shape");
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
  collectBuiltinCatalogue();
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
  renderImageQuad=reinterpret_cast<DrawImageQuad>(GetProcAddress(engine,"ImDrawList_AddImageQuad"));
  renderPushClip=reinterpret_cast<PushClipRect>(GetProcAddress(engine,"ImDrawList_PushClipRect"));
  renderPopClip=reinterpret_cast<PopClipRect>(GetProcAddress(engine,"ImDrawList_PopClipRect"));
   renderPushTexture=reinterpret_cast<PushTextureId>(GetProcAddress(engine,"ImDrawList_PushTextureID"));
   renderPopTexture=reinterpret_cast<PopTextureId>(GetProcAddress(engine,"ImDrawList_PopTextureID"));
  logger(std::string("Component render: overlay primitives ")+
         (renderWorldToScreen&&renderGetIo&&renderGetMainViewport&&renderGetBackgroundDrawList&&
          renderLine&&renderRect&&renderRectFilled&&renderCircle&&renderCircleFilled&&renderText&&
          renderPushClip&&renderPopClip?"armed":"unavailable"));
  simulationState=reinterpret_cast<unsigned char**>(address("simulation_state__modelZsimulator95types_u81"));
  logger(std::string("Simulation reads: ")+
         (aliases.count("sim.state.read")&&aliases.count("sim.cycle")?"armed":"unavailable")+
         ", state buffer "+std::to_string(tc::sim::kStateBufferSize)+" bytes");
 addComponent=reinterpret_cast<AddComponent>(address("add_component__presenterZutilitiesZhelper95functions_u5918"));
  tryDelete=reinterpret_cast<TryDelete>(address("try_delete__modelZutilities_u8585"));
  logger(std::string("Board edits: component placement ")+(addComponent?"armed":"unavailable")+
         ", component deletion "+(tryDelete?"armed":"unavailable")+
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
    std::vector<uint8_t> encoded;
    const bool adopt=p.owner->foundryEntryCarriesId(definition->name?definition->name:"",
                                                    definition->custom_id);
    const int result=tc::registerNativeComponentV2(&p.host,definition,nullptr,&encoded,false,adopt);
    if(!result){p.logicIds.push_back(definition->custom_id);p.componentIds.push_back(definition->custom_id);
     p.owner->noteComponentType(p,definition->custom_id,definition->name,
                                (int)definition->config_schema,definition->type_id);
     /* The generated inner circuit is the only picture this type can have; hand
        it to the foundry so the button opens the wiring instead of an empty
        definition (see provideFoundrySchematicBytes). */
     provideFoundrySchematicBytes(*p.owner,p.host,
                                  definition->name?definition->name:"",
                                  definition->custom_id,encoded);}
    else log_api(c,("Component V2 registration rejected: "+std::to_string(result)).c_str());
    return result;
   } catch(const std::exception& e) {log_api(c,e.what());return TC_COMPONENT_TYPES_ERR_GAME;}
    catch(...) {return TC_COMPONENT_TYPES_ERR_GAME;}
  }
  static int component_types_register_v3_api(void* c,const TCComponentTypeDefinitionV3* definition){
   auto& p=*(Loaded*)c;if(!p.accepting)return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
   try {
    std::vector<uint8_t> encoded;
    const bool adopt=p.owner->foundryEntryCarriesId(definition->name?definition->name:"",
                                                    definition->custom_id);
    const int result=tc::registerNativeComponentV3(&p.host,definition,&encoded,false,adopt);
    if(!result){
     p.logicIds.push_back(definition->custom_id);p.componentIds.push_back(definition->custom_id);
     const size_t configTail=offsetof(TCComponentTypeDefinitionV3,config_schema)+sizeof(definition->config_schema);
     const int schema=definition->size>=configTail?(int)definition->config_schema:-1;
     p.owner->noteComponentType(p,definition->custom_id,definition->name,schema,definition->type_id);
     provideFoundrySchematicBytes(*p.owner,p.host,
                                  definition->name?definition->name:"",
                                  definition->custom_id,encoded);
    } else log_api(c,("Component V3 registration rejected: "+std::to_string(result)).c_str());
    return result;
   } catch(const std::exception& e) {log_api(c,e.what());return TC_COMPONENT_TYPES_ERR_GAME;}
    catch(...) {return TC_COMPONENT_TYPES_ERR_GAME;}
  }
 /* tc.component.runtime: add a type that does not exist yet while the game is
    running.  The load-time entry stays closed (a type must exist before the
    game compiles a board that uses it); this one is the deliberate exception,
    and it only opens while the component model is idle - on the game thread
    with no board loaded, which is the main menu, where a Mod's own editor page
    lives.  It can therefore never move a port or a state layout under a live
    circuit, and it never touches a type that is already registered. */
 static int component_runtime_register_api(void* c,const TCComponentTypeDefinitionV3* definition){
  auto& p=*(Loaded*)c;
  if(!p.active)return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
  if(!definition)return TC_COMPONENT_TYPES_ERR_ARGUMENT;
  auto& r=*p.owner;
  if(r.gameThreadId&&GetCurrentThreadId()!=r.gameThreadId)return TC_COMPONENT_TYPES_ERR_STATE;
  const void* board=nullptr;
 if(r.currentBoardRaw(&board)&&board)return TC_COMPONENT_TYPES_ERR_STATE;
 try {
   std::vector<uint8_t> encoded;
   /* An id this Mod registered and then deleted is still in the game's own
      prototype map (the delete only takes it out of the ordered live list), so
      the ordinary import would be refused as a duplicate - and the editor's
      file manager always reuses the same custom id for a file it recreates.
      Import through the update path instead, which rewrites every index the
      game keeps for that id.  When the game really has forgotten it (a longer
      session, or an id deleted before this loader ran), the update path
      refuses and the plain import is tried once. */
   const bool retired=p.retiredIds.count(definition->custom_id)!=0;
   const bool adopt=r.foundryEntryCarriesId(definition->name?definition->name:"",
                                            definition->custom_id);
   int result=tc::registerNativeComponentV3(&p.host,definition,&encoded,retired,adopt);
   if(result&&retired)result=tc::registerNativeComponentV3(&p.host,definition,&encoded,false,adopt);
   if(result){
    log_api(c,("Component runtime registration rejected: "+std::to_string(result)).c_str());
    return result;
   }
   p.retiredIds.erase(definition->custom_id);
   r.ensureLiveCustomType(definition->custom_id);
   /* The bridge only emits for definitions it has activated, and the load-time
      commit ran long ago, so this one is activated here. */
   tc::logic::finish(std::vector<uint64_t>{definition->custom_id},true);
   p.logicIds.push_back(definition->custom_id);p.componentIds.push_back(definition->custom_id);
   const size_t configTail=offsetof(TCComponentTypeDefinitionV3,config_schema)+sizeof(definition->config_schema);
   const int schema=definition->size>=configTail?(int)definition->config_schema:-1;
   r.noteComponentType(p,definition->custom_id,definition->name,schema,definition->type_id);
   /* A type added from a Mod's own page (the tc-verilog editor's "save and hot
      reload") gets the same treatment as a load-time one: the foundry entry is
      filled with the generated inner circuit so the component can be opened and
      its wiring looked at. */
   provideFoundrySchematicBytes(r,p.host,definition->name?definition->name:"",
                                definition->custom_id,encoded);
   ++r.componentMenuGeneration;
   if(!r.componentMenuGeneration)++r.componentMenuGeneration;
   const std::string line="Component runtime: registered type "+
                          tc::logic::hex64(definition->custom_id)+
                          " live; the game files it into the component column when it next builds that menu";
   log_api(c,line.c_str());
   return TC_COMPONENT_TYPES_OK;
  } catch(const std::exception& e) {log_api(c,e.what());return TC_COMPONENT_TYPES_ERR_GAME;}
   catch(...) {return TC_COMPONENT_TYPES_ERR_GAME;}
 }
 static bool component_runtime_idle(Loaded& p){
  if(!p.active)return false;
  auto& r=*p.owner;
  if(r.gameThreadId&&GetCurrentThreadId()!=r.gameThreadId)return false;
  const void* board=nullptr;
  return !r.currentBoardRaw(&board)||!board;
 }
 static bool component_runtime_owned(const Loaded& p,uint64_t customId){
  return std::find(p.componentIds.begin(),p.componentIds.end(),customId)!=p.componentIds.end();
 }
static void component_runtime_forget(Loaded& p,uint64_t customId){
 p.logicIds.erase(std::remove(p.logicIds.begin(),p.logicIds.end(),customId),p.logicIds.end());
 p.componentIds.erase(std::remove(p.componentIds.begin(),p.componentIds.end(),customId),
                      p.componentIds.end());
 p.retiredIds.insert(customId);
 component_registry::dropType(customId,p.id);
}
static bool writableData(const void* address,size_t bytes){
 if(!address||!bytes)return false;
 MEMORY_BASIC_INFORMATION information{};
 if(VirtualQuery(address,&information,sizeof(information))!=sizeof(information))return false;
 if(information.State!=MEM_COMMIT||
    (information.Protect&(PAGE_NOACCESS|PAGE_GUARD|PAGE_READONLY)))return false;
 const auto* end=static_cast<const unsigned char*>(information.BaseAddress)+
                 information.RegionSize;
 return static_cast<const unsigned char*>(address)+bytes<=end;
}
/* The component column, the budget pass and the snapshot pass all read the
   game's ordered live list (`cc_length`/`cc_live_values`), while the prototype
   map is what add/update write.  The game appends to the live list only when
   its map slot was empty, and its delete drops the id from the live list
   without clearing that slot - so a type this Mod deleted and then registered
   again would never return to the column.  Append it exactly the way the
   game's own insert does, and only when it is missing. */
bool ensureLiveCustomType(uint64_t customId){
 if(!symbols)return false;
 auto& values=symbols->values;
 auto length=values.find("cc_length__modelZboardZcustom95prototype95list_u8");
 auto live=values.find("cc_live_values__modelZboardZcustom95prototype95list_u7");
 if(length==values.end()||!length->second||live==values.end()||!live->second)return false;
 auto* count=reinterpret_cast<uint64_t*>(length->second);
 auto* entries=static_cast<unsigned char*>(live->second);
 if(!writableData(count,sizeof(*count))||!writableData(entries,16))return false;
 const uint64_t now=*count;
 if(now>9999)return false;
 for(uint64_t index=0;index<now;++index){
  uint64_t value=0;
  std::memcpy(&value,entries+index*16,sizeof(value));
  if(value==customId)return true;
 }
 auto* slot=entries+now*16;
 if(!writableData(slot,16))return false;
 const uint16_t low=static_cast<uint16_t>(customId);
 std::memcpy(slot,&customId,sizeof(customId));
 std::memcpy(slot+8,&low,sizeof(low));
 *count=now+1;
 logger("Component runtime: put type "+logic::hex64(customId)+
        " back into the component column's live list");
 return true;
}
static int component_runtime_unregister_api(void* c,uint64_t customId){
  auto& p=*static_cast<Loaded*>(c);
  if(!customId||!component_runtime_owned(p,customId))return TC_COMPONENT_TYPES_ERR_ARGUMENT;
  if(!component_runtime_idle(p))return TC_COMPONENT_TYPES_ERR_STATE;
  try {
   TCGameModel game;
   if(!game.load(&p.host)||!game.hasCustomPrototype(customId))return TC_COMPONENT_TYPES_ERR_GAME;
   if(!game.removeCustomPrototype(customId))return TC_COMPONENT_TYPES_ERR_GAME;
   logic::finish(std::vector<uint64_t>{customId},false);
   component_runtime_forget(p,customId);
   ++p.owner->componentMenuGeneration;
   if(!p.owner->componentMenuGeneration)++p.owner->componentMenuGeneration;
   log_api(c,("Component runtime: unregistered type "+logic::hex64(customId)+
              "; the next component-menu build will omit it").c_str());
   return TC_COMPONENT_TYPES_OK;
  } catch(const std::exception& e) {log_api(c,e.what());return TC_COMPONENT_TYPES_ERR_GAME;}
   catch(...) {return TC_COMPONENT_TYPES_ERR_GAME;}
 }
 static int component_runtime_replace_api(void* c,const TCComponentTypeDefinitionV3* previous,
                                          const TCComponentTypeDefinitionV3* replacement){
  auto& p=*static_cast<Loaded*>(c);
  if(!previous||!replacement||!previous->custom_id||
     previous->custom_id!=replacement->custom_id||
     !component_runtime_owned(p,previous->custom_id))return TC_COMPONENT_TYPES_ERR_ARGUMENT;
  if(!component_runtime_idle(p))return TC_COMPONENT_TYPES_ERR_STATE;
  const uint64_t id=previous->custom_id;
  try {
   TCGameModel game;
   if(!game.load(&p.host)||!game.hasCustomPrototype(id))
    return TC_COMPONENT_TYPES_ERR_GAME;
   /* Do not erase and re-import this id.  The game keeps an additional custom
      id index outside the visible prototype table, so add_custom_prototype
      still reports a duplicate after the table entry is deleted.  Its native
      update path replaces those indexes in place. */
   logic::finish(std::vector<uint64_t>{id},false);
   std::vector<uint8_t> encoded;
   const int result=tc::registerNativeComponentV3(&p.host,replacement,&encoded,true);
   if(result==TC_COMPONENT_TYPES_OK){
    logic::finish(std::vector<uint64_t>{id},true);
    const size_t configTail=offsetof(TCComponentTypeDefinitionV3,config_schema)+
                            sizeof(replacement->config_schema);
    const int schema=replacement->size>=configTail?(int)replacement->config_schema:-1;
    p.owner->noteComponentType(p,id,replacement->name,schema,replacement->type_id);
    provideFoundrySchematicBytes(*p.owner,p.host,
                                 replacement->name?replacement->name:"",id,encoded);
    ++p.owner->componentMenuGeneration;
    if(!p.owner->componentMenuGeneration)++p.owner->componentMenuGeneration;
    log_api(c,("Component runtime: replaced type "+logic::hex64(id)+
               "; the next component-menu build will use the new definition").c_str());
    return result;
   }
   /* Registration is deliberately reversible: the editor keeps its old
      Component object alive until this call returns, so its definition can be
      restored if the game rejects the new shape. */
   logic::finish(std::vector<uint64_t>{id},false);
   const int restored=tc::registerNativeComponentV3(&p.host,previous,nullptr,true);
   if(restored!=TC_COMPONENT_TYPES_OK){
    log_api(c,("Component runtime: replacement and rollback both failed for "+
               logic::hex64(id)).c_str());
    return TC_COMPONENT_TYPES_ERR_GAME;
   }
   logic::finish(std::vector<uint64_t>{id},true);
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
 /* Handles for tc.component.render V7 images: loader-wide unique, so a handle
    from one Mod's table is simply unknown in another's. */
 uint64_t nextImageHandle=1,nextFontHandle=1;
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
   self->componentInputs.clearBoard();self->componentChannels.clear();self->componentKeysDown.clear();
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
   self->componentInputs.clearBoard();self->componentChannels.clear();self->componentKeysDown.clear();
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
 /* Component lifecycle, placement half: where every live instance sits, read
    once per frame from its own board record - the same record the handles
    already name.  A reading that differs from the last frame arrives as
    TC_LOGIC_MOVE / TC_LOGIC_ROTATE through the type's lifecycle table; the
    first reading of a freshly bound instance only seeds it (being compiled is
    not a move). */
 void pollComponentGeometry(){
  static constexpr uint32_t kMaxPolledInstances=512;
  TCComponentInstanceHandle handles[kMaxPolledInstances]{};
  uint32_t written=0,total=0;
  if(tc::logic::instanceEnumerate(0,handles,kMaxPolledInstances,&written,&total)!=TC_COMPONENT_INSTANCES_OK)return;
  for(uint32_t index=0;index<written;++index){
   const unsigned char* record=boardRecordOf(handles[index].custom_id,handles[index].instance_id);
   if(!record)continue;
   tc::logic::updateInstanceGeometry(handles[index].custom_id,handles[index].instance_id,
                                    board_objects::readI16(record,2),board_objects::readI16(record,4),
                                    board_objects::readU8(record,6));
  }
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
 /* The shape check a command has to pass before it is queued *or* staged: the
    transaction path runs the same steps through the same executor, so a check
    that only lived in `submit` would let a transaction smuggle in a command -
    or a type - the bus itself refuses.  The Board handle's liveness is checked
    by the caller, because only it knows whether the handle should be the
    current Board or the transaction's own. */
 int validateCommand(const Loaded& p,const TCCommandV2& command) const {
  if(command.flags||command.subject.size<sizeof(TCGameHandle)||
     command.subject.kind!=TC_GAME_OBJECT_BOARD)return TC_COMMAND_ERR_ARGUMENT;
  if(command.type<TC_COMMAND_SIM_RUN||command.type>TC_COMMAND_BOARD_DELETE_COMPONENT)
   return TC_COMMAND_ERR_ARGUMENT;
  if(command.type==TC_COMMAND_SIM_RUN&&command.argument<0)return TC_COMMAND_ERR_ARGUMENT;
  if(command.type==TC_COMMAND_BOARD_PLACE_COMPONENT){
   /* A placement names either a built-in kind or a custom prototype, and both
      coordinates have to fit the record's 16-bit fields. */
   if(command.custom_prototype_id){if(command.kind!=board_edits::kCustomKind)return TC_COMMAND_ERR_ARGUMENT;}
   else if(!command.kind)return TC_COMMAND_ERR_ARGUMENT;
   if(command.rotation>255)return TC_COMMAND_ERR_ARGUMENT;
   if(command.x<-32768||command.x>32767||command.y<-32768||command.y>32767)
    return TC_COMMAND_ERR_ARGUMENT;
  }
  if(command.type==TC_COMMAND_BOARD_DELETE_COMPONENT){
   /* The component is named by (instance id, type).  A type the session's
      catalogue attributes to *another* Mod is refused: the loader knows that
      owner, so it can tell the two apart.  A type it never brokered (a circuit
      the Mod imported itself - the path examples/circuit-and takes) carries no
      owner anywhere in the loader, so it is left to the caller, exactly like
      DUPLICATE_COMPONENT, which has never required ownership either. */
   if(!command.custom_prototype_id||!command.argument)return TC_COMMAND_ERR_ARGUMENT;
   if(typeOwnedByAnotherMod(p,command.custom_prototype_id))return TC_COMMAND_ERR_ARGUMENT;
  }
  return TC_COMMAND_OK;
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
  if(!p.accepting&&!p.active)return TC_COMMAND_ERR_ARGUMENT;
  const int shape=validateCommand(p,command);if(shape!=TC_COMMAND_OK)return shape;
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
 /* Whether the session's catalogue attributes a type to some *other* Mod.  A
    declarative registration and a loader-brokered circuit import both carry
    their owner's id; a circuit the Mod imported through the component model
    itself never passes through that path, so the loader has no record and no
    opinion. */
 bool typeOwnedByAnotherMod(const Loaded& p,uint64_t customId) const {
  component_registry::Type type{};
  if(!component_registry::find(customId,&type))return false;
  return !type.owner_mod.empty()&&type.owner_mod!=p.id;
 }
 bool ownsInstance(const Loaded& p,const TCComponentInstanceHandle* handle) const {
  return handle&&handle->size>=sizeof(*handle)&&
         handle->version==TC_COMPONENT_INSTANCES_API_VERSION_1&&
         std::find(p.componentIds.begin(),p.componentIds.end(),handle->custom_id)!=p.componentIds.end();
 }
 static int component_input_set_handler_api(void* c,uint64_t customId,float minX,float minY,
                                            float maxX,float maxY,
                                            TCComponentInputCallbackV1 callback,void* user){
  auto& p=*static_cast<Loaded*>(c);
  if(!p.accepting)return TC_COMPONENT_INPUT_ERR_STATE;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())
   return TC_COMPONENT_INPUT_ERR_OWNERSHIP;
  return p.owner->componentInputs.setHandler(&p,customId,minX,minY,maxX,maxY,callback,user);
 }
 static int component_input_clear_handler_api(void* c,uint64_t customId){
  auto& p=*static_cast<Loaded*>(c);
  return p.owner->componentInputs.clearHandler(&p,customId);
 }
 static int component_input_route_api(void* c,const TCComponentInstanceHandle* handle,
                                      uint32_t requests){
  auto& p=*static_cast<Loaded*>(c);
  if(!p.owner->ownsInstance(p,handle))return TC_COMPONENT_INPUT_ERR_OWNERSHIP;
  if(tc::logic::instanceValidate(handle)!=TC_COMPONENT_INSTANCES_OK)return TC_COMPONENT_INPUT_ERR_STALE;
  return p.owner->componentInputs.route(&p,*handle,requests);
 }
 static int component_input_state_api(void* c,const TCComponentInstanceHandle* handle,
                                      TCComponentInputStateV1* out){
  auto& p=*static_cast<Loaded*>(c);
  if(!p.owner->ownsInstance(p,handle))return TC_COMPONENT_INPUT_ERR_OWNERSHIP;
  if(tc::logic::instanceValidate(handle)!=TC_COMPONENT_INSTANCES_OK)return TC_COMPONENT_INPUT_ERR_STALE;
  return p.owner->componentInputs.state(&p,*handle,out);
 }
 int queryComponentInputService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;
  if(version!=TC_COMPONENT_INPUT_API_VERSION_1)return TC_SERVICE_ERR_VERSION;
  if(outSize<sizeof(TCComponentInputApiV1))return TC_SERVICE_ERR_SIZE;
  *static_cast<TCComponentInputApiV1*>(out)=TCComponentInputApiV1{
      sizeof(TCComponentInputApiV1),TC_COMPONENT_INPUT_API_VERSION_1,&p,
      &component_input_set_handler_api,&component_input_clear_handler_api,
      &component_input_route_api,&component_input_state_api};
  return TC_SERVICE_OK;
 }
 int validateChannelInstance(Loaded& p,const TCComponentInstanceHandle* handle){
  if(!ownsInstance(p,handle))return TC_COMPONENT_CHANNEL_ERR_OWNERSHIP;
  return tc::logic::instanceValidate(handle)==TC_COMPONENT_INSTANCES_OK?
         TC_COMPONENT_CHANNEL_OK:TC_COMPONENT_CHANNEL_ERR_STALE;
 }
 static int component_channel_open_api(void* c,const TCComponentInstanceHandle* handle,
                                       const TCComponentChannelConfigV1* config){
  auto& p=*static_cast<Loaded*>(c);if(!config)return TC_COMPONENT_CHANNEL_ERR_ARGUMENT;
  const int valid=p.owner->validateChannelInstance(p,handle);if(valid)return valid;
  return p.owner->componentChannels.open(&p,*handle,*config);
 }
 static int component_channel_close_api(void* c,const TCComponentInstanceHandle* handle){
  auto& p=*static_cast<Loaded*>(c);const int valid=p.owner->validateChannelInstance(p,handle);
  if(valid)return valid;
  return p.owner->componentChannels.close(&p,*handle);
 }
 static int component_channel_send_api(void* c,const TCComponentInstanceHandle* handle,
                                       uint32_t direction,const void* data,uint32_t bytes,
                                       uint64_t* sequence){
  auto& p=*static_cast<Loaded*>(c);const int valid=p.owner->validateChannelInstance(p,handle);if(valid)return valid;
  return p.owner->componentChannels.send(&p,*handle,direction,data,bytes,sequence);
 }
 static int component_channel_receive_api(void* c,const TCComponentInstanceHandle* handle,
                                          uint32_t direction,void* out,uint32_t capacity,
                                          TCComponentChannelMessageV1* message){
  auto& p=*static_cast<Loaded*>(c);const int valid=p.owner->validateChannelInstance(p,handle);if(valid)return valid;
  return p.owner->componentChannels.receive(&p,*handle,direction,out,capacity,message);
 }
 static int component_channel_publish_api(void* c,const TCComponentInstanceHandle* handle,
                                          const void* data,uint32_t bytes,uint64_t* revision){
  auto& p=*static_cast<Loaded*>(c);const int valid=p.owner->validateChannelInstance(p,handle);if(valid)return valid;
  return p.owner->componentChannels.publish(&p,*handle,data,bytes,revision);
 }
 static int component_channel_read_api(void* c,const TCComponentInstanceHandle* handle,
                                       uint64_t known,void* out,uint32_t capacity,
                                       uint32_t* bytes,uint64_t* revision){
  auto& p=*static_cast<Loaded*>(c);const int valid=p.owner->validateChannelInstance(p,handle);if(valid)return valid;
  return p.owner->componentChannels.read(&p,*handle,known,out,capacity,bytes,revision);
 }
 static int component_channel_stats_api(void* c,const TCComponentInstanceHandle* handle,
                                        TCComponentChannelStatsV1* out){
  auto& p=*static_cast<Loaded*>(c);const int valid=p.owner->validateChannelInstance(p,handle);if(valid)return valid;
  return p.owner->componentChannels.stats(&p,*handle,out);
 }
 int queryComponentChannelService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;
  if(version!=TC_COMPONENT_CHANNEL_API_VERSION_1)return TC_SERVICE_ERR_VERSION;
  if(outSize<sizeof(TCComponentChannelApiV1))return TC_SERVICE_ERR_SIZE;
  *static_cast<TCComponentChannelApiV1*>(out)=TCComponentChannelApiV1{
      sizeof(TCComponentChannelApiV1),TC_COMPONENT_CHANNEL_API_VERSION_1,&p,
      &component_channel_open_api,&component_channel_close_api,&component_channel_send_api,
      &component_channel_receive_api,&component_channel_publish_api,&component_channel_read_api,
      &component_channel_stats_api};
  return TC_SERVICE_OK;
 }
 static int query_service_api(void* c,const char* id,uint32_t version,void* out,uint32_t outSize){
 auto& p=*(Loaded*)c;if(!id)return TC_SERVICE_ERR_ARGUMENT;
 if(std::strcmp(id,TC_SERVICE_MOD_STORAGE)==0)return p.owner->queryModStorageService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_MOD_SETTINGS)==0)return p.owner->queryModSettingsService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_COMMANDS)==0)return p.owner->queryCommandService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_LIFECYCLE)==0)return p.owner->queryLifecycleService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_TRANSACTIONS)==0)return p.owner->queryTransactionService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_COMPONENT_TYPES)==0)return p.owner->queryComponentTypesService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_COMPONENT_RUNTIME)==0)return p.owner->queryComponentRuntimeService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_COMPONENT_COMPOSE)==0)return p.owner->queryComponentComposeService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_COMPONENT_GEOMETRY)==0)return p.owner->queryComponentGeometryService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_COMPONENT_RENDER)==0)return p.owner->queryComponentRenderService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_COMPONENT_INPUT)==0)return p.owner->queryComponentInputService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_COMPONENT_CHANNEL)==0)return p.owner->queryComponentChannelService(p,version,out,outSize);
 if(std::strcmp(id,TC_SERVICE_COMPONENT_EDITOR)==0)return p.owner->queryComponentEditorService(p,version,out,outSize);
 return p.owner->services.query(id,version,out,outSize);
 }
 static bool modStorageAvailable(const Loaded& p){return p.owner&&(p.accepting||p.active);}
 static int mod_storage_read_api(void* c,uint32_t scope,const char* key,void* out,
                                 uint32_t capacity,uint32_t* bytes){
  auto& p=*static_cast<Loaded*>(c);
  if(!modStorageAvailable(p))return TC_MOD_STORAGE_ERR_UNAVAILABLE;
  return p.owner->modStorage.readValue(p.id,scope,key,out,capacity,bytes);
 }
 static int mod_storage_write_api(void* c,uint32_t scope,const char* key,const void* data,
                                  uint32_t size){
  auto& p=*static_cast<Loaded*>(c);
  if(!modStorageAvailable(p))return TC_MOD_STORAGE_ERR_UNAVAILABLE;
  return p.owner->modStorage.writeValue(p.id,scope,key,data,size);
 }
 static int mod_storage_remove_api(void* c,uint32_t scope,const char* key){
  auto& p=*static_cast<Loaded*>(c);
  if(!modStorageAvailable(p))return TC_MOD_STORAGE_ERR_UNAVAILABLE;
  return p.owner->modStorage.removeValue(p.id,scope,key);
 }
 static int mod_storage_has_api(void* c,uint32_t scope,const char* key){
  auto& p=*static_cast<Loaded*>(c);
  if(!modStorageAvailable(p))return TC_MOD_STORAGE_ERR_UNAVAILABLE;
  return p.owner->modStorage.hasValue(p.id,scope,key);
 }
 static int mod_storage_version_api(void* c,uint32_t scope,const char* key,uint32_t* schema){
  auto& p=*static_cast<Loaded*>(c);
  if(!modStorageAvailable(p))return TC_MOD_STORAGE_ERR_UNAVAILABLE;
  return p.owner->modStorage.getVersion(p.id,scope,key,schema);
 }
 static int mod_storage_migrate_api(void* c,uint32_t scope,const char* key,uint32_t target,
                                    const void* defaults,uint32_t defaultSize,
                                    TCModStorageMigrateV1 callback,void* user,uint32_t* resulting){
  auto& p=*static_cast<Loaded*>(c);
  if(!modStorageAvailable(p))return TC_MOD_STORAGE_ERR_UNAVAILABLE;
  return p.owner->modStorage.migrateValue(p.id,scope,key,target,defaults,defaultSize,
                                          callback,user,resulting);
 }
 int queryModStorageService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;
   if(version==TC_MOD_STORAGE_API_VERSION_1){
    if(outSize<sizeof(TCModStorageApiV1))return TC_SERVICE_ERR_SIZE;
    *static_cast<TCModStorageApiV1*>(out)=TCModStorageApiV1{
        sizeof(TCModStorageApiV1),TC_MOD_STORAGE_API_VERSION_1,&p,
        &mod_storage_read_api,&mod_storage_write_api,&mod_storage_remove_api,
        &mod_storage_has_api};
    return TC_SERVICE_OK;
   }
   if(version==TC_MOD_STORAGE_API_VERSION_2){
    if(outSize<sizeof(TCModStorageApiV2))return TC_SERVICE_ERR_SIZE;
    *static_cast<TCModStorageApiV2*>(out)=TCModStorageApiV2{
        sizeof(TCModStorageApiV2),TC_MOD_STORAGE_API_VERSION_2,&p,
        &mod_storage_read_api,&mod_storage_write_api,&mod_storage_remove_api,
        &mod_storage_has_api,&mod_storage_version_api,&mod_storage_migrate_api};
    return TC_SERVICE_OK;
   }
   return TC_SERVICE_ERR_VERSION;
 }
 int applySetting(Loaded& p,mod_settings::Field& field,const mod_settings::Value& value){
  if(value.type!=field.type)return TC_MOD_SETTINGS_ERR_TYPE;
  const TCModSettingValueV1 publicValue=mod_settings::publicValue(value);
  if(p.settingChanged){
   try{if(p.settingChanged(p.settingUser,field.id.c_str(),&publicValue)!=TC_MOD_SETTINGS_OK)
    return TC_MOD_SETTINGS_ERR_ARGUMENT;}
   catch(...){return TC_MOD_SETTINGS_ERR_ARGUMENT;}
  }
  const std::string encoded=mod_settings::encode(value);
  const int stored=modStorage.writeVersioned(p.id,field.scope,field.storageKey.c_str(),1,
                                              encoded.data(),(uint32_t)encoded.size());
  if(stored!=TC_MOD_STORAGE_OK)return TC_MOD_SETTINGS_ERR_STORAGE;
  field.current=value;
  logger("Mod setting changed: "+p.id+"/"+field.id+" scope="+
         (field.scope==TC_MOD_STORAGE_SCOPE_GLOBAL?"global":"profile"));
  return TC_MOD_SETTINGS_OK;
 }
 static int mod_settings_set_fields_api(void* c,const TCModSettingFieldV1* fields,uint32_t count,
                                        TCModSettingChangedV1 changed,void* user){
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;
  if(!p.accepting)return TC_MOD_SETTINGS_ERR_UNAVAILABLE;
  if(r.gameThreadId&&GetCurrentThreadId()!=r.gameThreadId)return TC_MOD_SETTINGS_ERR_THREAD;
  std::vector<mod_settings::Field> copied;std::string reason;
  const int validation=mod_settings::copyFields(fields,count,&copied,&reason);
  if(validation!=TC_MOD_SETTINGS_OK){r.logger("Mod settings rejected: "+p.id+": "+reason);return validation;}
  for(auto& field:copied){
   uint32_t bytes=0;
   int status=r.modStorage.readValue(p.id,field.scope,field.storageKey.c_str(),nullptr,0,&bytes);
   if(status==TC_MOD_STORAGE_ERR_NOT_FOUND){
    const std::string encoded=mod_settings::encode(field.defaultValue);
    status=r.modStorage.writeVersioned(p.id,field.scope,field.storageKey.c_str(),1,
                                       encoded.data(),(uint32_t)encoded.size());
    if(status!=TC_MOD_STORAGE_OK)return TC_MOD_SETTINGS_ERR_STORAGE;
    field.current=field.defaultValue;
    continue;
   }
   if(status!=TC_MOD_STORAGE_OK)return TC_MOD_SETTINGS_ERR_STORAGE;
   std::string encoded(bytes,'\0');
   if(bytes){uint32_t written=0;status=r.modStorage.readValue(p.id,field.scope,
      field.storageKey.c_str(),encoded.data(),bytes,&written);
    if(status!=TC_MOD_STORAGE_OK||written!=bytes)return TC_MOD_SETTINGS_ERR_STORAGE;}
   mod_settings::Value value;
   if(mod_settings::decode(field,encoded,&value))field.current=std::move(value);
   else{field.current=field.defaultValue;r.logger("Mod setting invalid; using default without overwriting: "+
        p.id+"/"+field.id);}
  }
  p.settings=std::move(copied);p.settingChanged=changed;p.settingUser=user;
  r.logger("Mod settings registered: "+p.id+" fields="+std::to_string(p.settings.size()));
  return TC_MOD_SETTINGS_OK;
 }
 static int mod_settings_clear_fields_api(void* c){
  auto& p=*static_cast<Loaded*>(c);if(!p.accepting)return TC_MOD_SETTINGS_ERR_UNAVAILABLE;
  p.settings.clear();p.settingChanged=nullptr;p.settingUser=nullptr;return TC_MOD_SETTINGS_OK;
 }
 static int mod_settings_get_api(void* c,const char* id,TCModSettingValueV1* out,uint32_t outSize){
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;
  if((!p.accepting&&!p.active))return TC_MOD_SETTINGS_ERR_UNAVAILABLE;
  if(r.gameThreadId&&GetCurrentThreadId()!=r.gameThreadId)return TC_MOD_SETTINGS_ERR_THREAD;
  if(!id||!out||outSize<sizeof(TCModSettingValueV1))return TC_MOD_SETTINGS_ERR_ARGUMENT;
  for(auto& field:p.settings)if(field.id==id){*out=mod_settings::publicValue(field.current);return TC_MOD_SETTINGS_OK;}
  return TC_MOD_SETTINGS_ERR_NOT_FOUND;
 }
 static int mod_settings_reset_api(void* c,const char* id){
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;
  if((!p.accepting&&!p.active))return TC_MOD_SETTINGS_ERR_UNAVAILABLE;
  if(r.gameThreadId&&GetCurrentThreadId()!=r.gameThreadId)return TC_MOD_SETTINGS_ERR_THREAD;
  if(!id)return TC_MOD_SETTINGS_ERR_ARGUMENT;
  for(auto& field:p.settings)if(field.id==id)return r.applySetting(p,field,field.defaultValue);
  return TC_MOD_SETTINGS_ERR_NOT_FOUND;
 }
 int queryModSettingsService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;
  if(version!=TC_MOD_SETTINGS_API_VERSION_1)return TC_SERVICE_ERR_VERSION;
  if(outSize<sizeof(TCModSettingsApiV1))return TC_SERVICE_ERR_SIZE;
  *static_cast<TCModSettingsApiV1*>(out)=TCModSettingsApiV1{
      sizeof(TCModSettingsApiV1),TC_MOD_SETTINGS_API_VERSION_1,&p,
      &mod_settings_set_fields_api,&mod_settings_clear_fields_api,
      &mod_settings_get_api,&mod_settings_reset_api};
  return TC_SERVICE_OK;
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
  if(!sameHandle(command.subject,tx.board))return TC_TRANSACTION_ERR_ARGUMENT;
  if(validateCommand(*tx.owner,command)!=TC_COMMAND_OK)return TC_TRANSACTION_ERR_ARGUMENT;
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
  if(version==TC_COMPONENT_TYPES_API_VERSION_1){
   if(outSize<sizeof(TCComponentTypesApiV1))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentTypesApiV1*>(out)=TCComponentTypesApiV1{
       sizeof(TCComponentTypesApiV1),TC_COMPONENT_TYPES_API_VERSION_1,&p,
       &component_types_register_api};
   return TC_SERVICE_OK;
  }
  if(version!=TC_COMPONENT_TYPES_API_VERSION_2)return TC_SERVICE_ERR_VERSION;
  if(outSize<sizeof(TCComponentTypesApiV2))return TC_SERVICE_ERR_SIZE;
  *static_cast<TCComponentTypesApiV2*>(out)=TCComponentTypesApiV2{
      sizeof(TCComponentTypesApiV2),TC_COMPONENT_TYPES_API_VERSION_2,&p,
      &component_types_register_api,&component_types_register_v3_api};
  return TC_SERVICE_OK;
}
/* tc.component.runtime V1: the running-game registration entry, handed out per
   plugin for the same reason as above - the registration needs that plugin's
   own host. */
int queryComponentRuntimeService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;
  if(version==TC_COMPONENT_RUNTIME_API_VERSION_1){
   if(outSize<sizeof(TCComponentRuntimeApiV1))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentRuntimeApiV1*>(out)=TCComponentRuntimeApiV1{
       sizeof(TCComponentRuntimeApiV1),TC_COMPONENT_RUNTIME_API_VERSION_1,&p,
       &component_runtime_register_api};
   return TC_SERVICE_OK;
  }
  if(version!=TC_COMPONENT_RUNTIME_API_VERSION_2)return TC_SERVICE_ERR_VERSION;
  if(outSize<sizeof(TCComponentRuntimeApiV2))return TC_SERVICE_ERR_SIZE;
  *static_cast<TCComponentRuntimeApiV2*>(out)=TCComponentRuntimeApiV2{
      sizeof(TCComponentRuntimeApiV2),TC_COMPONENT_RUNTIME_API_VERSION_2,&p,
      &component_runtime_register_api,&component_runtime_replace_api,
      &component_runtime_unregister_api};
  return TC_SERVICE_OK;
}
/* tc.component.compose V1: the built-in type catalogue.  Loader-wide read-only
   data, so the context is the runtime itself (unlike the registration services,
   which need the calling plugin).  Nothing is published when the boot
   enumeration found no prototype table: an empty catalogue would read as "this
   build has no components" instead of "this loader cannot answer". */
static void composeFill(const builtin_catalogue::Entry& entry,TCBuiltinTypeInfoV1& info){
  info=TCBuiltinTypeInfoV1{};
  info.size=sizeof(info);
  info.kind=entry.kind;
  info.flags=(entry.inputs.empty()?TC_BUILTIN_FLAG_NO_DECLARED_INPUTS:0u)|
             (entry.hasShape?TC_BUILTIN_FLAG_HAS_SHAPE:0u);
  info.input_count=static_cast<uint32_t>(entry.inputs.size());
  info.output_count=static_cast<uint32_t>(entry.outputs.size());
  info.pin_limit=TC_BUILTIN_PIN_LIMIT;
  component_registry::copyField(info.type_id,sizeof(info.type_id),entry.typeId);
  component_registry::copyField(info.name,sizeof(info.name),entry.name);
  component_registry::copyField(info.description,sizeof(info.description),entry.description);
 }
 static uint32_t compose_count_api(void* c){
  return static_cast<uint32_t>(static_cast<Loaded*>(c)->owner->builtinTypes.size());
 }
 static int compose_get_api(void* c,uint32_t index,TCBuiltinTypeInfoV1* out){
  auto& r=*static_cast<Loaded*>(c)->owner;
  if(!out||out->size<sizeof(TCBuiltinTypeInfoV1))return TC_COMPONENT_COMPOSE_ERR_ARGUMENT;
  if(index>=r.builtinTypes.size())return TC_COMPONENT_COMPOSE_ERR_RANGE;
  composeFill(r.builtinTypes[index],*out);
  return TC_COMPONENT_COMPOSE_OK;
 }
 static int compose_find_api(void* c,uint32_t kind,TCBuiltinTypeInfoV1* out){
  auto& r=*static_cast<Loaded*>(c)->owner;
  if(!out||out->size<sizeof(TCBuiltinTypeInfoV1))return TC_COMPONENT_COMPOSE_ERR_ARGUMENT;
  const auto found=r.builtinTypeByKind.find(kind);
  if(found==r.builtinTypeByKind.end())return TC_COMPONENT_COMPOSE_ERR_UNKNOWN;
  composeFill(r.builtinTypes[found->second],*out);
  return TC_COMPONENT_COMPOSE_OK;
 }
 static int compose_find_by_type_id_api(void* c,const char* typeId,TCBuiltinTypeInfoV1* out){
  auto& r=*static_cast<Loaded*>(c)->owner;
  if(!out||out->size<sizeof(TCBuiltinTypeInfoV1))return TC_COMPONENT_COMPOSE_ERR_ARGUMENT;
  if(!typeId||!*typeId)return TC_COMPONENT_COMPOSE_ERR_ARGUMENT;
  const auto exact=r.builtinTypeById.find(typeId);
  if(exact!=r.builtinTypeById.end()){composeFill(r.builtinTypes[exact->second],*out);return TC_COMPONENT_COMPOSE_OK;}
  const int folded=builtin_catalogue::indexOfTypeId(r.builtinTypes,std::string(typeId));
  if(folded<0)return TC_COMPONENT_COMPOSE_ERR_UNKNOWN;
  composeFill(r.builtinTypes[static_cast<size_t>(folded)],*out);
  return TC_COMPONENT_COMPOSE_OK;
 }
 static int compose_pin_api(void* c,uint32_t kind,uint32_t direction,uint32_t index,
                            TCBuiltinPinV1* out){
  auto& r=*static_cast<Loaded*>(c)->owner;
  if(!out||out->size<sizeof(TCBuiltinPinV1))return TC_COMPONENT_COMPOSE_ERR_ARGUMENT;
  if(direction!=TC_COMPONENT_PIN_INPUT&&direction!=TC_COMPONENT_PIN_OUTPUT)
   return TC_COMPONENT_COMPOSE_ERR_ARGUMENT;
  const auto found=r.builtinTypeByKind.find(kind);
  if(found==r.builtinTypeByKind.end())return TC_COMPONENT_COMPOSE_ERR_UNKNOWN;
  const auto& pins=direction==TC_COMPONENT_PIN_INPUT?r.builtinTypes[found->second].inputs
                                                   :r.builtinTypes[found->second].outputs;
  if(index>=pins.size())return TC_COMPONENT_COMPOSE_ERR_RANGE;
  const builtin_catalogue::Pin& pin=pins[index];
  *out=TCBuiltinPinV1{};
  out->size=sizeof(*out);
  out->direction=direction;
  out->index=index;
  out->bits=pin.bits;
  out->x=pin.x;out->y=pin.y;
  out->word_size_raw=pin.wordSizeRaw;
  out->flags=(pin.bits?0u:TC_BUILTIN_PIN_AUTO_WIDTH);
  if(!pin.bits&&pin.wordSizeRaw!=board_pins::kPinAutoSize)out->flags=TC_BUILTIN_PIN_RAW_WIDTH;
  return TC_COMPONENT_COMPOSE_OK;
 }
 /* ---- V2: the internal-netlist builder ----------------------------------- */
 static void composeRefuse(ComposeSession& session,const std::string& why){
  session.status=why;
 }
 /* Resolves one inner part by the spelling the Mod asked for.  A built-in
    spelling comes from the boot catalogue (kind + declared pins); any other
    spelling is a registered Mod type, whose pins are read from its own prototype
    - that is the nesting path, and it is why the inner type has to exist before
    the outer definition is committed. */
 bool composePart(NativeRuntime& r,const std::string& spelling,component_netlist::Node& node,
                  std::string& why) const {
  const auto builtin=r.builtinTypeById.find(spelling);
  if(builtin!=r.builtinTypeById.end()){
   const builtin_catalogue::Entry& entry=r.builtinTypes[builtin->second];
   node.kind=entry.kind;
   node.sinks=entry.inputs;
   node.sources=entry.outputs;
   node.stateful=entry.inputs.empty();
   if(node.sinks.empty()&&node.sources.empty()){
    why="the part '"+spelling+"' declares no pin at all";
    return false;
   }
   return true;
  }
  std::string wanted=spelling;
  component_registry::Type found{};
  bool known=false;
  for(const component_registry::Type& candidate:component_registry::snapshot())
   if(candidate.active&&component_registry::typeId(candidate)==wanted){found=candidate;known=true;break;}
  if(!known){
   /* A custom id is accepted too - "0x<id>", or "<owner>/0x<id>" - so a Mod can
      nest a type it just registered without looking the spelling up first.  When
      an owner is spelled it has to match, so two Mods cannot name each other's
      ids by accident. */
   const size_t marker=wanted.rfind("0x");
   if(marker!=std::string::npos){
    const std::string owner=marker?wanted.substr(0,marker-1):std::string();
    const char* digits=wanted.c_str()+marker+2;
    if(*digits&&std::all_of(digits,digits+std::strlen(digits),
                            [](char letter){return std::isxdigit(static_cast<unsigned char>(letter))!=0;})){
     const uint64_t id=std::strtoull(digits,nullptr,16);
     for(const component_registry::Type& candidate:component_registry::snapshot())
      if(candidate.active&&candidate.custom_id==id&&
         (owner.empty()||candidate.owner_mod==owner)){found=candidate;known=true;break;}
    }
   }
  }
  if(!known){
   why="no such part: '"+spelling+"' is neither a built-in spelling nor a registered type's type_id";
   return false;
  }
  if(!customPrototypeGet){
   why="the game's custom prototype table is unavailable";
   return false;
  }
  node.kind=board_edits::kCustomKind;
  node.customId=found.custom_id;
  TCPrototype prototype{};
  customPrototypeGet(found.custom_id,&prototype);
  const auto readPins=[&](bool outputs,std::vector<builtin_catalogue::Pin>& out){
   const uint64_t count=outputs?prototypeOutputCount(prototype):prototypeInputCount(prototype);
   for(uint64_t index=0;index<count;++index){
    const TCPin* pin=outputs?prototypeOutputPin(prototype,index):prototypeInputPin(prototype,index);
    if(!pin)continue;
    const TCPinInfoV1 info=board_pins::decodePin(*pin,outputs?TC_PIN_OUTPUT:TC_PIN_INPUT);
    out.push_back(builtin_catalogue::Pin{info.x,info.y,info.bits,info.word_size_raw});
   }
  };
  readPins(false,node.sinks);
  readPins(true,node.sources);
  if(prototypeDestroy)prototypeDestroy(&prototype);
  if(node.sinks.empty()&&node.sources.empty()){
   why="the type '"+spelling+"' has no pin the builder can address";
   return false;
  }
  return true;
 }
 static int compose_begin_api(void* c,const TCComposeTypeV1* type,uint64_t* session){
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;
  if(!type||!session||type->size<sizeof(TCComposeTypeV1)||!type->custom_id||
     !type->name||!*type->name)return TC_COMPONENT_COMPOSE_ERR_ARGUMENT;
  if(!p.accepting)return TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
  if((!type->input_count&&!type->output_count)||
     type->input_count>component_netlist::kMaxPinsPerDirection||
     type->output_count>component_netlist::kMaxPinsPerDirection||
     (type->input_count&&!type->inputs)||(type->output_count&&!type->outputs))
   return TC_COMPONENT_COMPOSE_ERR_ARGUMENT;
  component_registry::Type existing{};
  if(component_registry::find(type->custom_id,&existing))return TC_COMPONENT_COMPOSE_ERR_DUPLICATE;
  {std::lock_guard<std::mutex> lock(logic::registryMutex);
   if(logic::definitions.count(type->custom_id))return TC_COMPONENT_COMPOSE_ERR_DUPLICATE;}
  ComposeSession opened;
  opened.owner=&p;
  opened.definition.customId=type->custom_id;
  opened.definition.name=type->name;
  opened.definition.gateCost=type->gate_cost<0?0:type->gate_cost;
  opened.definition.delay=type->delay<0?0:type->delay;
  const auto copyPins=[&](const TCComponentPinV2* pins,uint32_t count,
                          std::vector<component_netlist::Pin>& out){
   for(uint32_t index=0;index<count;++index){
    const TCComponentPinV2& pin=pins[index];
    if(pin.bits<1||pin.bits>64)return false;
    out.push_back(component_netlist::Pin{pin.name?pin.name:"",pin.bits});
   }
   return true;
  };
  if(!copyPins(type->inputs,type->input_count,opened.definition.inputs)||
     !copyPins(type->outputs,type->output_count,opened.definition.outputs))
   return TC_COMPONENT_COMPOSE_ERR_ARGUMENT;
  const uint64_t id=r.nextComposeSession++;
  r.composeSessions.emplace(id,std::move(opened));
  *session=id;
  return TC_COMPONENT_COMPOSE_OK;
 }
 static int compose_node_api(void* c,uint64_t session,const char* typeId,int32_t x,int32_t y,
                             uint32_t bits,uint32_t* nodeIndex){
  auto& r=*static_cast<Loaded*>(c)->owner;
  const auto found=r.composeSessions.find(session);
  if(found==r.composeSessions.end())return TC_COMPONENT_COMPOSE_ERR_BUILD;
  ComposeSession& open=found->second;
  if(!typeId||!*typeId){
   composeRefuse(open,"node() needs the part's spelling");
   return TC_COMPONENT_COMPOSE_ERR_ARGUMENT;
  }
  component_netlist::Node node;
  node.x=x;node.y=y;node.bits=bits?bits:1u;
  node.typeId=typeId;
  std::string why;
  if(!r.composePart(r,typeId,node,why)){
   composeRefuse(open,why);
   return TC_COMPONENT_COMPOSE_ERR_NETLIST;
  }
  open.definition.nodes.push_back(std::move(node));
  if(nodeIndex)
   *nodeIndex=open.definition.firstInner()+static_cast<uint32_t>(open.definition.nodes.size())-1;
  return TC_COMPONENT_COMPOSE_OK;
 }
 static int compose_connect_api(void* c,uint64_t session,uint32_t fromNode,uint32_t fromPin,
                                uint32_t toNode,uint32_t toPin){
  auto& r=*static_cast<Loaded*>(c)->owner;
  const auto found=r.composeSessions.find(session);
  if(found==r.composeSessions.end())return TC_COMPONENT_COMPOSE_ERR_BUILD;
  ComposeSession& open=found->second;
  const component_netlist::Definition& d=open.definition;
  component_netlist::Point scratch{};
  if(!component_netlist::sourcePoint(d,fromNode,fromPin,scratch)){
   composeRefuse(open,"node "+std::to_string(fromNode)+" pin "+std::to_string(fromPin)+
                      " is not a source pin");
   return TC_COMPONENT_COMPOSE_ERR_NETLIST;
  }
  if(!component_netlist::sinkPoint(d,toNode,toPin,scratch)){
   composeRefuse(open,"node "+std::to_string(toNode)+" pin "+std::to_string(toPin)+
                      " is not a sink pin");
   return TC_COMPONENT_COMPOSE_ERR_NETLIST;
  }
  open.definition.connections.push_back(component_netlist::Connection{fromNode,fromPin,toNode,toPin});
  return TC_COMPONENT_COMPOSE_OK;
 }
  /* The game's foundry opens a *player* component: it looks the selected type up
     by name under <profile>/schematics/foundry/<name>/circuit.data, and a Mod
     type has no such entry, so the button shows an empty definition.  That file
     is exactly the component definition the loader generates (measured
     2026-09-29 against the foundry's own files: same header, same pin records -
     writing our bytes there made the foundry draw all 65 parts of a composite).

     A callback type has no hand-drawn schematic, but it does have a generated
     inner circuit: the pins, one collector per input, the dependency cones that
     tell the game which inputs each output reads, the drivers and the wires
     between them (see component_definition::encode).  Writing those same bytes
     here is what lets a player open a Mod component in the foundry and actually
     look at its wiring, instead of an empty board.

     The entry is filled only when the game has nothing but its own placeholder;
     a copy the player has edited is left alone. */
  /* True when <profile>/schematics/foundry/<name>/circuit.data exists and carries
     this custom id: the entry the loader itself filled (possibly re-saved by the
     game in its own container since).  The game imports everything under
     `schematics/foundry/` as a player component when it boots - before any plugin
     loads - so a type whose entry is already there arrives at registration with
     its prototype already in the game's table.  The registration paths use this
     to replace that prototype in place instead of refusing the id. */
  bool foundryEntryCarriesId(const std::string& name,uint64_t customId) const {
   if(saveRoot.empty()||name.empty())return false;
   const fs::path file=saveRoot/"schematics"/"foundry"/fs::u8path(name)/"circuit.data";
   std::ifstream probe(file,std::ios::binary);
   if(!probe)return false;
   char head[64]={};
   probe.read(head,sizeof(head));
   const auto got=static_cast<size_t>(probe.gcount());
   uint8_t wanted[8]={};
   for(int i=0;i<8;++i)wanted[i]=static_cast<uint8_t>((customId>>(8*i))&0xff);
   for(size_t offset=0;offset+8<=got&&offset<sizeof(head);++offset)
    if(std::memcmp(head+offset,wanted,8)==0)return true;
   return false;
  }
  static void provideFoundrySchematicBytes(NativeRuntime& runtime,const TCHost& host,
                                           const std::string& name,uint64_t customId,
                                           const std::vector<uint8_t>& bytes){
   if(runtime.saveRoot.empty()||name.empty()||bytes.empty())return;
   std::error_code ec;
   const fs::path directory=runtime.saveRoot/"schematics"/"foundry"/fs::u8path(name);
   const fs::path file=directory/"circuit.data";
   const auto size=fs::file_size(file,ec);
   /* The game re-saves the entry in its own container (v16, real snappy) once the
      foundry has opened it, so "missing or empty" does not tell our entry from a
      player's component with the same name.  The id does: a definition starts
      with its custom id, so an entry carrying this type's id is ours and gets
      refreshed when the layout changes; anything else is the player's and is left
      alone. */
   if(!ec&&size>64&&!runtime.foundryEntryCarriesId(name,customId))
    return; /* the player's own component of the same name */
   fs::create_directories(directory,ec);
   std::ofstream out(file,std::ios::binary|std::ios::trunc);
   if(!out){host.log(host.context,("foundry view: cannot write "+file.u8string()).c_str());return;}
   out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
   host.log(host.context,("foundry view: filled "+file.u8string()+
                          " so the component can be opened in the foundry").c_str());
  }
  static void provideFoundrySchematic(NativeRuntime& runtime,const TCHost& host,
                                      const component_netlist::Definition& d){
   provideFoundrySchematicBytes(runtime,host,d.name,d.customId,component_netlist::encode(d));
  }
 static int compose_commit_api(void* c,uint64_t session){
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;
  auto found=r.composeSessions.find(session);
  if(found==r.composeSessions.end())return TC_COMPONENT_COMPOSE_ERR_BUILD;
  ComposeSession& open=found->second;
  std::string why;
  const component_netlist::Error error=component_netlist::validate(open.definition,why);
  if(error!=component_netlist::Error::None){
   composeRefuse(open,std::string(component_netlist::errorText(error))+
                      (why.empty()?std::string():std::string(": ")+why));
   return TC_COMPONENT_COMPOSE_ERR_NETLIST;
  }
  std::string status;
  const bool adopt=r.foundryEntryCarriesId(open.definition.name,open.definition.customId);
  const int result=tc::registerCompositeComponent(&p.host,open.definition,status,adopt);
  if(result){
   composeRefuse(open,status.empty()?"the game refused the composite definition":status);
   return result==-3?TC_COMPONENT_COMPOSE_ERR_DUPLICATE:TC_COMPONENT_COMPOSE_ERR_GAME;
  }
  p.componentIds.push_back(open.definition.customId);
  p.owner->noteComponentType(p,open.definition.customId,open.definition.name.c_str(),0,nullptr);
  {
   tc::component_registry::Type entry;
   entry.custom_id=open.definition.customId;
   entry.owner_mod=p.id;
   entry.name=open.definition.name;
   entry.gate_cost=static_cast<uint64_t>(open.definition.gateCost);
   entry.delay=static_cast<uint64_t>(open.definition.delay);
   entry.implementation=TC_COMPONENT_IMPL_COMPOSITE;
   entry.schema_version=0;
   entry.active=true;
   for(const component_netlist::Pin& pin:open.definition.inputs)
    entry.inputs.push_back(tc::component_registry::Pin{pin.name,pin.bits});
   for(const component_netlist::Pin& pin:open.definition.outputs)
    entry.outputs.push_back(tc::component_registry::Pin{pin.name,pin.bits});
   tc::component_registry::note(std::move(entry));
  }
  provideFoundrySchematic(r,p.host,open.definition);
  r.composeSessions.erase(found);
  return TC_COMPONENT_COMPOSE_OK;
 }
 static int compose_abort_api(void* c,uint64_t session){
  auto& r=*static_cast<Loaded*>(c)->owner;
  const auto found=r.composeSessions.find(session);
  if(found==r.composeSessions.end())return TC_COMPONENT_COMPOSE_ERR_BUILD;
  r.composeSessions.erase(found);
  return TC_COMPONENT_COMPOSE_OK;
 }
 static const char* compose_status_api(void* c,uint64_t session){
  auto& r=*static_cast<Loaded*>(c)->owner;
  const auto found=r.composeSessions.find(session);
  return found==r.composeSessions.end()?"":found->second.status.c_str();
 }
 /* How wide this type's own pin lane is: the room the inner circuit is laid out
    in.  The historic shape is two cells (-18 / +13); a wider lane moves the
    columns apart (the declarative API's `pin_lane`, same convention). */
 static int compose_pin_lane_api(void* c,uint64_t session,uint32_t lane){
  auto& r=*static_cast<Loaded*>(c)->owner;
  const auto found=r.composeSessions.find(session);
  if(found==r.composeSessions.end())return TC_COMPONENT_COMPOSE_ERR_BUILD;
  if(!component_netlist::validPinLane(lane)){
   composeRefuse(found->second,"the pin lane has to be 1..16 board cells");
   return TC_COMPONENT_COMPOSE_ERR_ARGUMENT;
  }
  found->second.definition.pinLane=lane;
  return TC_COMPONENT_COMPOSE_OK;
 }
 /* tc.component.compose V2: plan section 8.3's `clone_builtin_metadata`.  The
    fields it copies are the ones a *metadata* clone can honestly promise: the
    name and description the player reads, and (on request) the declared cost
    pair.  Pins are not copyable after registration, and the icon is not
    copyable at all - the game ignores a custom prototype's shape (measured), so
    the contract points at tc.component.render V5's picture instead. */
 static int compose_clone_metadata_api(void* c,uint64_t customId,const char* builtinTypeId,
                                       uint32_t flags){
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;
  if(!p.accepting&&!p.active)return TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
  if(!builtinTypeId||!*builtinTypeId||!flags||
     (flags&~(TC_COMPOSE_CLONE_NAME|TC_COMPOSE_CLONE_DESCRIPTION|TC_COMPOSE_CLONE_COST)))
   return TC_COMPONENT_COMPOSE_ERR_ARGUMENT;
  const auto builtin=r.builtinTypeById.find(builtinTypeId);
  if(builtin==r.builtinTypeById.end())return TC_COMPONENT_COMPOSE_ERR_UNKNOWN;
  component_registry::Type own{};
  if(!component_registry::find(customId,&own)||own.owner_mod!=p.id)
   return TC_COMPONENT_COMPOSE_ERR_ARGUMENT; /* only a Mod's own type */
  if(!r.customPrototypeGet||!r.customPrototypeSet||!r.prototypeDestroy)
   return TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
  const builtin_catalogue::Entry& source=r.builtinTypes[builtin->second];
  TCGameModel game;
  if(!game.load(&p.host))return TC_COMPONENT_COMPOSE_ERR_UNAVAILABLE;
  TCPrototype prototype{};
  r.customPrototypeGet(customId,&prototype);
  bool ok=true;
  if(flags&TC_COMPOSE_CLONE_NAME)ok=game.setPrototypeName(prototype,source.name.c_str())&&ok;
  if(flags&TC_COMPOSE_CLONE_DESCRIPTION)
   ok=game.setPrototypeDescription(prototype,source.description.c_str())&&ok;
  if(flags&TC_COMPOSE_CLONE_COST){
   TCPrototypeKind key{};key.tag=static_cast<uint8_t>(source.kind);
   TCPrototype builtinPrototype{};
   r.prototypeGet(&key,&builtinPrototype);
   ok=game.setPrototypeGateCost(prototype,prototypeGateCost(builtinPrototype))&&ok;
   ok=game.setPrototypeDelay(prototype,prototypeDelay(builtinPrototype))&&ok;
   r.prototypeDestroy(&builtinPrototype);
  }
  if(ok)r.customPrototypeSet(customId,&prototype);
  r.prototypeDestroy(&prototype);
  if(!ok)return TC_COMPONENT_COMPOSE_ERR_GAME;
  /* The loader's own catalogue follows, so the Mods page and any Mod querying
     tc.component.registry see the cloned text too. */
  component_registry::Type updated=own;
  if(flags&TC_COMPOSE_CLONE_NAME)updated.name=source.name;
  if(flags&TC_COMPOSE_CLONE_DESCRIPTION)updated.description=source.description;
  component_registry::note(std::move(updated));
  r.logger("["+p.id+"] Component compose: cloned metadata of '"+source.typeId+"' onto "+
         component_registry::typeId(own)+" flags="+std::to_string(flags));
  return TC_COMPONENT_COMPOSE_OK;
 }
 /* Plan section 8.3's `register_builtin_alias`: this build has no path for it
    (the game dispatches by kind byte, and a Mod type's instances carry 0x4e), so
    the call refuses explicitly rather than registering a look-alike shell.  The
    reasoning and the two measurements behind it are in the contract comment and
    docs/sdk/services.md. */
 static int compose_alias_api(void* c,uint64_t customId,const char* builtinTypeId){
  auto& p=*static_cast<Loaded*>(c);
  auto& r=*p.owner;
  if(!builtinTypeId||!*builtinTypeId)return TC_COMPONENT_COMPOSE_ERR_ARGUMENT;
  if(r.builtinTypeById.find(builtinTypeId)==r.builtinTypeById.end())
   return TC_COMPONENT_COMPOSE_ERR_UNKNOWN;
  r.logger("["+p.id+"] Component compose: alias of '"+std::string(builtinTypeId)+
         "' refused (this build dispatches by kind byte; use clone_metadata + a composite node)");
  (void)customId;
  return TC_COMPONENT_COMPOSE_ERR_UNSUPPORTED;
 }
 void dropComposeSessions(Loaded& p){
  for(auto it=composeSessions.begin();it!=composeSessions.end();)
   if(it->second.owner==&p)it=composeSessions.erase(it);else ++it;
 }
 int queryComponentComposeService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;
  if(builtinTypes.empty()||builtinTypeByKind.empty())return TC_SERVICE_ERR_UNAVAILABLE;
  if(version==TC_COMPONENT_COMPOSE_API_VERSION_1){
   if(outSize<sizeof(TCComponentComposeApiV1))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentComposeApiV1*>(out)=TCComponentComposeApiV1{
       sizeof(TCComponentComposeApiV1),TC_COMPONENT_COMPOSE_API_VERSION_1,&p,
       &compose_count_api,&compose_get_api,&compose_find_api,&compose_find_by_type_id_api,
       &compose_pin_api};
   return TC_SERVICE_OK;
  }
  if(version!=TC_COMPONENT_COMPOSE_API_VERSION_2)return TC_SERVICE_ERR_VERSION;
  if(outSize<sizeof(TCComponentComposeApiV2))return TC_SERVICE_ERR_SIZE;
  *static_cast<TCComponentComposeApiV2*>(out)=TCComponentComposeApiV2{
      sizeof(TCComponentComposeApiV2),TC_COMPONENT_COMPOSE_API_VERSION_2,&p,
      &compose_count_api,&compose_get_api,&compose_find_api,&compose_find_by_type_id_api,
      &compose_pin_api,&compose_begin_api,&compose_node_api,&compose_connect_api,
      &compose_commit_api,&compose_abort_api,&compose_status_api,
      &compose_clone_metadata_api,&compose_alias_api,&compose_pin_lane_api};
  return TC_SERVICE_OK;
 }
/* tc.component.editor V1: the declarative property rows (plan §14).  The
   context is the calling plugin's own Loaded record, because a declaration has
   to be checked against that plugin's types and it has to disappear with them. */
static int component_editor_set_fields_api(void* c,uint64_t customId,
                                           const TCComponentFieldV1* fields,uint32_t count){
  if(!c)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  auto& p=*static_cast<Loaded*>(c);
  auto& r=*p.owner;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_COMPONENT_EDITOR_ERR_THREAD;
  if(!p.accepting)return TC_COMPONENT_EDITOR_ERR_THREAD;
  if(!customId)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())
   return TC_COMPONENT_EDITOR_ERR_OWNERSHIP;
  if(!count){p.editorFields.erase(customId);p.editorTableFlags.erase(customId);return TC_COMPONENT_EDITOR_OK;}
  if(!fields)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  if(count>component_editor::kMaxFields)return TC_COMPONENT_EDITOR_ERR_LIMIT;
  /* The rows edit the instance's configuration, so the type has to have one.
     It is the definition the Mod registered a moment ago in this same load. */
  uint32_t configBytes=0;
  {
   std::lock_guard<std::mutex> lock(tc::logic::registryMutex);
   const auto found=tc::logic::definitions.find(customId);
   if(found==tc::logic::definitions.end())return TC_COMPONENT_EDITOR_ERR_OWNERSHIP;
   configBytes=static_cast<uint32_t>(found->second->defaultConfig.size());
  }
  if(!configBytes){
   r.logger("Component editor: type "+hexValue(customId)+
            " declared no configuration, so it has nothing to edit");
   return TC_COMPONENT_EDITOR_ERR_FIELD;
  }
  std::vector<component_editor::Field> converted;
  converted.reserve(count);
  for(uint32_t index=0;index<count;++index){
   if(fields[index].flags&~TC_COMPONENT_FIELD_V2_KNOWN_FLAGS){
    r.logger("Component editor: a V1/V2 table used V3-only flags");
    return TC_COMPONENT_EDITOR_ERR_FIELD;
   }
   component_editor::Field field=component_editor::fromDeclared(fields[index]);
   for(const auto& earlier:converted)
    if(earlier.id==field.id){
     r.logger("Component editor: refused the rows of type "+hexValue(customId)+
              ": two fields are called \""+field.id+"\"");
     return TC_COMPONENT_EDITOR_ERR_FIELD;
    }
   converted.push_back(std::move(field));
  }
  /* Field rules and then the table rules: a row that shares a line has to have
     something to share it with, and unknown flags are refused rather than
     ignored (a typo would otherwise change the layout silently). */
  {
   std::string why;
   if(!component_editor::validateTable(converted,configBytes,why)){
    r.logger("Component editor: refused the rows of type "+hexValue(customId)+": "+why);
    return TC_COMPONENT_EDITOR_ERR_FIELD;
   }
  }
  p.editorFields[customId]=std::move(converted);
  /* A V1/V2 table has no table-level flags, so a type re-declared this way must
     not keep a staged mode an earlier V4 declaration left behind. */
  p.editorTableFlags[customId]=0;
  std::string summary;
  for(const auto& field:p.editorFields[customId]){
   if(!summary.empty())summary+=", ";
   summary+=field.id+"("+component_editor::kindName(field)+")";
  }
  r.logger("Component editor: type "+hexValue(customId)+" edits "+summary);
  return TC_COMPONENT_EDITOR_OK;
 }
static int component_editor_clear_fields_api(void* c,uint64_t customId){
  if(!c)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  auto& p=*static_cast<Loaded*>(c);
  if(!p.accepting)return TC_COMPONENT_EDITOR_ERR_THREAD;
  if(!customId)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())
   return TC_COMPONENT_EDITOR_ERR_OWNERSHIP;
  p.editorFields.erase(customId);
  p.editorTableFlags.erase(customId);
  return TC_COMPONENT_EDITOR_OK;
 }
/* Shared by every V3/V4 table declaration: copy the Mod's fields, check them
   against the type's own configuration, check the table flags, and keep both.
   `knownFlags` is the flag set the caller's entry point is allowed to use and
   `tag` names that entry point in the log, so a refused table says which API
   version refused it. */
static int componentEditorDeclareV2(Loaded& p,uint64_t customId,
                                    const TCComponentFieldV2* fields,uint32_t count,
                                    uint32_t tableFlags,uint32_t knownFlags,
                                    const char* tag){
  auto& r=*p.owner;
  if(!fields)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  if(count>component_editor::kMaxFields)return TC_COMPONENT_EDITOR_ERR_LIMIT;
  uint32_t configBytes=0;
  {
   std::lock_guard<std::mutex> lock(tc::logic::registryMutex);
   const auto found=tc::logic::definitions.find(customId);
   if(found==tc::logic::definitions.end())return TC_COMPONENT_EDITOR_ERR_OWNERSHIP;
   configBytes=static_cast<uint32_t>(found->second->defaultConfig.size());
  }
  if(!configBytes)return TC_COMPONENT_EDITOR_ERR_FIELD;
  std::vector<component_editor::Field> converted;converted.reserve(count);
  for(uint32_t index=0;index<count;++index){
   const TCComponentFieldV2& declared=fields[index];
   if(declared.size<sizeof(TCComponentFieldV2)||
      declared.version!=TC_COMPONENT_FIELD_DEFINITION_VERSION_2||
      declared.context_actions>TC_COMPONENT_EDITOR_MAX_CONTEXT_ACTIONS||
      (declared.context_actions&&
       (!declared.context_action_ids||!declared.context_action_labels)))
    return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
   component_editor::Field field=component_editor::fromDeclared(declared);
   for(const auto& earlier:converted)if(earlier.id==field.id){
    r.logger(std::string("Component editor ")+tag+": type "+hexValue(customId)+
             " repeats field \""+field.id+"\"");
    return TC_COMPONENT_EDITOR_ERR_FIELD;
   }
   converted.push_back(std::move(field));
  }
  std::string why;
  if(!component_editor::validateTable(converted,configBytes,why,knownFlags)){
   r.logger(std::string("Component editor ")+tag+": refused the rows of type "+
            hexValue(customId)+": "+why);
   return TC_COMPONENT_EDITOR_ERR_FIELD;
  }
  if(!component_editor::validateTableFlags(tableFlags,converted,why)){
   r.logger(std::string("Component editor ")+tag+": refused the table of type "+
            hexValue(customId)+": "+why);
   return TC_COMPONENT_EDITOR_ERR_FIELD;
  }
  p.editorFields[customId]=std::move(converted);
  p.editorTableFlags[customId]=tableFlags;
  std::string summary;
  for(const auto& field:p.editorFields[customId]){
   if(!summary.empty())summary+=", ";
   summary+=field.id+"("+component_editor::kindName(field)+")";
  }
  r.logger(std::string("Component editor ")+tag+": type "+hexValue(customId)+
           " edits "+summary+
           ((tableFlags&TC_COMPONENT_EDITOR_TABLE_CONFIRM)?" table=confirm":""));
  return TC_COMPONENT_EDITOR_OK;
 }
static int component_editor_set_fields_v2_api(void* c,uint64_t customId,
                                              const TCComponentFieldV2* fields,
                                              uint32_t count){
  if(!c)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId||!p.accepting)
   return TC_COMPONENT_EDITOR_ERR_THREAD;
  if(!customId)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())
   return TC_COMPONENT_EDITOR_ERR_OWNERSHIP;
  if(!count){p.editorFields.erase(customId);p.editorTableFlags.erase(customId);return TC_COMPONENT_EDITOR_OK;}
  return componentEditorDeclareV2(p,customId,fields,count,0,
                                  TC_COMPONENT_FIELD_V3_KNOWN_FLAGS,"V3");
 }
/* The same call as seen by a V4 caller: the table may use the V4 flags. */
static int component_editor_set_fields_v2_v4_api(void* c,uint64_t customId,
                                                 const TCComponentFieldV2* fields,
                                                 uint32_t count){
  if(!c)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId||!p.accepting)
   return TC_COMPONENT_EDITOR_ERR_THREAD;
  if(!customId)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())
   return TC_COMPONENT_EDITOR_ERR_OWNERSHIP;
  if(!count){p.editorFields.erase(customId);p.editorTableFlags.erase(customId);return TC_COMPONENT_EDITOR_OK;}
  return componentEditorDeclareV2(p,customId,fields,count,0,
                                  TC_COMPONENT_FIELD_KNOWN_FLAGS,"V4");
 }
/* The V4 entry point: the same table plus its table-level flags.  The checks are
   identical, which is why both go through one helper - the only difference is
   which flag set the table is allowed to use and what the log calls it. */
static int component_editor_set_fields_v3_api(void* c,uint64_t customId,
                                              const TCComponentFieldV2* fields,
                                              uint32_t count,uint32_t tableFlags){
  if(!c)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId||!p.accepting)
   return TC_COMPONENT_EDITOR_ERR_THREAD;
  if(!customId)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),customId)==p.componentIds.end())
   return TC_COMPONENT_EDITOR_ERR_OWNERSHIP;
  if(!count){p.editorFields.erase(customId);p.editorTableFlags.erase(customId);return TC_COMPONENT_EDITOR_OK;}
  return componentEditorDeclareV2(p,customId,fields,count,tableFlags,
                                  TC_COMPONENT_FIELD_KNOWN_FLAGS,"V4");
 }
static int component_editor_begin_edit_api(void* c,const TCComponentInstanceHandle* handle){
  if(!c||!handle)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_COMPONENT_EDITOR_ERR_THREAD;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),handle->custom_id)==p.componentIds.end())
   return TC_COMPONENT_EDITOR_ERR_OWNERSHIP;
  return tc::logic::storageBeginEdit(handle);
 }
static int component_editor_commit_edit_api(void* c,const TCComponentInstanceHandle* handle){
  if(!c||!handle)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_COMPONENT_EDITOR_ERR_THREAD;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),handle->custom_id)==p.componentIds.end())
   return TC_COMPONENT_EDITOR_ERR_OWNERSHIP;
  return tc::logic::storageCommitEdit(handle);
 }
static int component_editor_abort_edit_api(void* c,const TCComponentInstanceHandle* handle){
  if(!c||!handle)return TC_COMPONENT_EDITOR_ERR_ARGUMENT;
  auto& p=*static_cast<Loaded*>(c);auto& r=*p.owner;
  if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)return TC_COMPONENT_EDITOR_ERR_THREAD;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),handle->custom_id)==p.componentIds.end())
   return TC_COMPONENT_EDITOR_ERR_OWNERSHIP;
  return tc::logic::storageAbortEdit(handle);
 }
int queryComponentEditorService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;
  if(version==TC_COMPONENT_EDITOR_API_VERSION_4){
   if(outSize<sizeof(TCComponentEditorApiV4))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentEditorApiV4*>(out)=TCComponentEditorApiV4{
       sizeof(TCComponentEditorApiV4),TC_COMPONENT_EDITOR_API_VERSION_4,&p,
       &component_editor_set_fields_api,&component_editor_clear_fields_api,
       &component_editor_set_fields_v2_v4_api,&component_editor_begin_edit_api,
       &component_editor_commit_edit_api,&component_editor_abort_edit_api,
       &component_editor_set_fields_v3_api};
   return TC_SERVICE_OK;
  }
  if(version==TC_COMPONENT_EDITOR_API_VERSION_3){
   if(outSize<sizeof(TCComponentEditorApiV3))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentEditorApiV3*>(out)=TCComponentEditorApiV3{
       sizeof(TCComponentEditorApiV3),TC_COMPONENT_EDITOR_API_VERSION_3,&p,
       &component_editor_set_fields_api,&component_editor_clear_fields_api,
       &component_editor_set_fields_v2_api,&component_editor_begin_edit_api,
       &component_editor_commit_edit_api,&component_editor_abort_edit_api};
   return TC_SERVICE_OK;
  }
  if(version==TC_COMPONENT_EDITOR_API_VERSION_2){
   if(outSize<sizeof(TCComponentEditorApiV2))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentEditorApiV2*>(out)=TCComponentEditorApiV2{
       sizeof(TCComponentEditorApiV2),TC_COMPONENT_EDITOR_API_VERSION_2,&p,
       &component_editor_set_fields_api,&component_editor_clear_fields_api};
   return TC_SERVICE_OK;
  }
  if(version!=TC_COMPONENT_EDITOR_API_VERSION_1)return TC_SERVICE_ERR_VERSION;
  if(outSize<sizeof(TCComponentEditorApiV1))return TC_SERVICE_ERR_SIZE;
  *static_cast<TCComponentEditorApiV1*>(out)=TCComponentEditorApiV1{
      sizeof(TCComponentEditorApiV1),TC_COMPONENT_EDITOR_API_VERSION_1,&p,
      &component_editor_set_fields_api,&component_editor_clear_fields_api};
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
  /* The instance's own rectangle changed (the type's declared footprint is the
     starting point), so the type's lifecycle hears TC_LOGIC_RESIZE. */
  tc::logic::updateInstanceFootprint(customId,instanceId,halfWidth,halfHeight);
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
  bool keptFoundryButtonsLogged=false;
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
  /* A Mod drops the foundry button because its type has no *hand-drawn*
     schematic.  Every type the loader registers does have a generated inner
     circuit though, and the loader writes that same definition into the
     foundry's own entry (provideFoundrySchematicBytes), so the button opens a
     real - if conservative - drawing of the part's pins and wiring.  A player
     who wants to look at it can keep the button with this switch; the hide
     requests become a no-op for the rest of the session. */
  static bool keepModFoundryButtons(){
   static const bool keep=[] {
    const char* value=std::getenv("TC_FOUNDRY_SHOW_MOD_TYPES");
    return value&&*value&&value[0]!='0';
   }();
   return keep;
  }
  bool foundryButtonHidden(){
   if(keepModFoundryButtons()){
    if(!hiddenFoundryIndices.empty()&&!keptFoundryButtonsLogged){
     keptFoundryButtonsLogged=true;
     logger("Component render: keeping the foundry edit button for Mod components "
            "(TC_FOUNDRY_SHOW_MOD_TYPES)");
    }
    return false;
   }
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
 /* ---- the picture the bottom panel draws (2026-09-28) ---------------------

    build_component_description_panel paints the selected component's picture
    twice with the engine's own igImage entry point (call sites 0x14039c916 and
    0x14039d336; the two are the component's snapshot and a second, smaller one
    under it - both are preceded by create_texture_unsafe / texture size / texture
    id in that function).  A Mod-owned type has no schematic behind it, which is
    the same fact that lets a Mod drop the foundry button (V4 above), so this hook
    hides those two pictures for exactly the same selection: every selected
    component belongs to a Mod that asked for the button to be gone.

    The picture is still *submitted*, with a fully transparent tint (and border),
    so the panel keeps its layout and nothing else moves - the same technique the
    foundry button uses.  The palette card and the placement ghost are different
    surfaces (V5 pixels, V6 callback) and are not touched. */
 /* The return addresses of every igImage call that paints the selected component
    in the bottom panel.  Measured on the pinned build (docs/research/…): the
    custom-component preview plus the nine pictures the description panel and its
    markup renderer draw for it - the schematic snapshot, the pin pictograms
    ("— R") and the rest of that panel's inline art.

       build_custom_component_preview                    @0x14038ed85
       render_cached_elements…component95description     @0x14035b26e, @0x1403723ce
       build_component_description_panel                 @0x14039c916, @0x14039d336,
                                                        @0x14039e0d6, @0x14039eb73,
                                                        @0x14039f5b1, @0x1403a4c25,
                                                        @0x1403a5632, @0x1403aa032

    All of them live in the executable (the module gameRva() reads) and all of
    them are hidden only while the *whole* selection is Mod-owned types whose Mod
    asked for the foundry button to be gone - the same "this type has no schematic
    behind it" decision, which is what the preview picture and the pictograms
    illustrate.  A stock component's panel is untouched. */
 static constexpr uintptr_t kDescriptionImageCallRvas[]={
     0x38ed8a,   /* build_custom_component_preview */
     0x35b273,   /* render_cached_elements…component95description (markup image) */
     0x3723d3,   /* ... the second markup image */
     0x39c91b,   /* build_component_description_panel … */
     0x39d33b,
     0x39e0db,
     0x39eb78,
     0x39f5b6,
     0x3a4c2a,
     0x3a5637,
     0x3aa037};
 /* cimgui's igImage takes the two ImVec4 colours by value; under this ABI a
    16-byte aggregate travels as a pointer to the caller's temporary (the
    disassembly shows exactly that: the 5th and 6th slots hold a pointer to a
    16-byte value, and the values themselves sit in the caller's frame).  Declared
    the same way as the loader's own ImVec4 entry points (igPushStyleColor). */
 /* (texture, image_size, uv0, uv1, tint_col, border_col) - the last two are the
    16-byte ImVec4s described above. */
 using IgImage=void(*)(uint64_t,RenderVec2,RenderVec2,RenderVec2,RenderVec4,RenderVec4);
 IgImage igImageOriginal=nullptr;
 int hiddenDescriptionPictureLoggedCount=0;
 static void detourIgImage(uint64_t texture,RenderVec2 size,RenderVec2 uv_minimum,
                           RenderVec2 uv_maximum,RenderVec4 tint,RenderVec4 border){
  auto* self=activeInstance;
  if(!self||!self->igImageOriginal)return;
  const uintptr_t caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  bool known=false;
  for(uintptr_t rva:kDescriptionImageCallRvas)
   if(caller==self->gameRva(rva)){known=true;break;}
  if(known){
   const int64_t frame=self->engineFrame();
   if(self->hiddenFoundryFrame!=frame){self->hiddenFoundryFrame=frame;
                                       self->refreshHiddenFoundryIndices();}
   if(self->foundryButtonHidden()){
    self->igImageOriginal(texture,size,uv_minimum,uv_maximum,RenderVec4{0.f,0.f,0.f,0.f},
                          RenderVec4{0.f,0.f,0.f,0.f});
    if(self->hiddenDescriptionPictureLoggedCount!=1){
     self->hiddenDescriptionPictureLoggedCount=1;
     self->logger("Component render: the panel preview picture is hidden for the "
                  "selected Mod component");
    }
    return;
   }
  }
  self->igImageOriginal(texture,size,uv_minimum,uv_maximum,tint,border);
 }
 void armDescriptionPictureControl(){
  if(!engine)return;
  auto image=GetProcAddress(engine,"igImage");
  if(!image){logger("Component render: the panel's preview picture entry is unavailable");return;}
  if(MH_CreateHook(reinterpret_cast<void*>(image),
                   reinterpret_cast<void*>(&NativeRuntime::detourIgImage),
                   reinterpret_cast<void**>(&igImageOriginal))!=MH_OK)return;
  if(MH_EnableHook(reinterpret_cast<void*>(image))!=MH_OK){
   MH_RemoveHook(reinterpret_cast<void*>(image));igImageOriginal=nullptr;return;
  }
  ownedHooks.insert(reinterpret_cast<void*>(image));
  loaderOwned[reinterpret_cast<void*>(image)]="component render panel preview control";
  logger("Component render: panel preview control armed");
 }
 /* ---- the pin pictograms the description panel draws (2026-09-28) ----------

    The player's second report: "中间这里还有引脚标识没删掉" - with the picture gone,
    the panel still showed the component's pin pictograms ("- R").  They are
    *markup box elements*: the description panel's cached markup calls
    render_box__presenterZmarkupZmarkup_u924 for each of them, six call sites in
    the two markup-cache functions of that panel (0x14035ac3b, 0x14035b13f,
    0x1403632d1, 0x140371da3, 0x14037229f, 0x1403756f1).

    The same gate as the picture and the foundry button decides: only while every
    selected component belongs to a Mod that asked for the button to be gone is
    the call skipped, so a stock component's panel keeps its pin pictograms.  The
    description *text* next to them is untouched. */
 static constexpr uintptr_t kDescriptionBoxCallRvas[]={
     0x35ac40, 0x35b144,   /* render_cached_elements…component95description_u1929 */
     0x3632d6,             /* render_bbcode_token…component95description_u1808 */
     0x371da8, 0x3722a4,   /* render_cached_elements…component95description_u3856 */
     0x3756f6};            /* render_bbcode_token…component95description_u3735 */
 using RenderBoxFn=void(*)(void*,void*,void*,void*,void*,void*,void*,void*);
 RenderBoxFn renderBoxOriginal=nullptr;
 int hiddenBoxLoggedCount=0;
 static void detourRenderBox(void* a,void* b,void* c,void* d,
                             void* e,void* f,void* g,void* h){
  auto* self=activeInstance;
  if(!self||!self->renderBoxOriginal)return;
  const uintptr_t caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  for(uintptr_t rva:kDescriptionBoxCallRvas)
   if(caller==self->gameRva(rva)){
    const int64_t frame=self->engineFrame();
    if(self->hiddenFoundryFrame!=frame){self->hiddenFoundryFrame=frame;
                                        self->refreshHiddenFoundryIndices();}
    if(self->foundryButtonHidden()){
     if(self->hiddenBoxLoggedCount!=1){
      self->hiddenBoxLoggedCount=1;
      self->logger("Component render: the panel pin pictograms are hidden for the "
                   "selected Mod component");
     }
     return;
    }
    break;
   }
  self->renderBoxOriginal(a,b,c,d,e,f,g,h);
 }
 void armDescriptionBoxControl(){
  if(!symbols)return;
  auto found=symbols->values.find("render_box__presenterZmarkupZmarkup_u924");
  if(found==symbols->values.end()||!found->second){
   logger("Component render: the panel's pin pictograms were not found");return;}
  if(MH_CreateHook(found->second,reinterpret_cast<void*>(&NativeRuntime::detourRenderBox),
                   reinterpret_cast<void**>(&renderBoxOriginal))!=MH_OK)return;
  if(MH_EnableHook(found->second)!=MH_OK){
   MH_RemoveHook(found->second);renderBoxOriginal=nullptr;return;}
  ownedHooks.insert(found->second);
  loaderOwned[found->second]="component render panel pin control";
  logger("Component render: panel pin control armed");
 }
 /* ---- who draws what in the panel (temporary diagnostic, 2026-09-28) -------

    The preview picture and the markup boxes are hidden, but the player still
    sees the component's pin pictogram ("- R") in the panel.  With
    TC_PANEL_TRACE=1 this logs one line per *distinct draw call site* the game
    uses while the panel describes a Mod component - the caller's RVA next to the
    entry point it called - so the pictogram can be traced to an address instead
    of guessed at. */
 bool panelTrace=false;
 struct PanelTraceState {
  std::set<std::string> seen;
  int lines=0;
 };
 PanelTraceState panelTraceState;
 /* True while the drawer describes one of our components.  The trace only keeps
    what is drawn in that state: the pin pictogram the player sees sits in the
    panel's own preview block, which is submitted *after* our rows in the same
    frame, so a per-frame flag from the panel hook covers it. */
 bool panelShowsModComponent=false;
 void notePanelDraw(const char* api,const uintptr_t caller){
  if(!panelTrace||!panelShowsModComponent||panelTraceState.lines>=60)return;
  /* The caller can be in the executable or in tc_game_engine.dll - the first run
     of this trace subtracted the *executable's* base and produced nonsense for a
     caller in the engine, which is how it was found that the pin pictogram is not
     drawn by the game's own panel code.  So: ask Windows which module owns the
     address, and log the RVA inside that module plus the module's name. */
  MEMORY_BASIC_INFORMATION info{};
  uintptr_t base=0;
  if(VirtualQuery(reinterpret_cast<const void*>(caller),&info,sizeof(info))==sizeof(info))
   base=reinterpret_cast<uintptr_t>(info.AllocationBase);
  wchar_t path[MAX_PATH]={};
  if(base)GetModuleFileNameW(reinterpret_cast<HMODULE>(base),path,MAX_PATH);
  std::string module="?";
  for(const wchar_t* p=path;*p;++p)module.push_back(*p<128?static_cast<char>(*p):'?');
  const size_t cut=module.find_last_of("\\/");
  if(cut!=std::string::npos)module=module.substr(cut+1);
  char text[320]={};
  std::snprintf(text,sizeof(text),"%s+0x%llx/%s",
                module.c_str(),
                static_cast<unsigned long long>(base?caller-base:caller),api);
  if(!panelTraceState.seen.insert(text).second)return;
  ++panelTraceState.lines;
  logger(std::string("Component render panel trace: ")+text);
 }
 using TraceTextFn=void(*)(void*,RenderVec2,uint32_t,const char*,const char*);
 using TraceLineFn=void(*)(void*,RenderVec2,RenderVec2,uint32_t,float,int,float);
 TraceTextFn panelTraceTextOriginal=nullptr;
 TraceLineFn panelTraceLineOriginal=nullptr;
 static void detourPanelTraceText(void* list,RenderVec2 position,uint32_t colour,
                                  const char* begin,const char* end){
  auto* self=activeInstance;
  if(!self||!self->panelTraceTextOriginal)return;
  self->notePanelDraw("AddText",reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  self->panelTraceTextOriginal(list,position,colour,begin,end);
 }
 static void detourPanelTraceLine(void* list,RenderVec2 from,RenderVec2 to,
                                  uint32_t colour,float thickness,int flags,float rounding){
  auto* self=activeInstance;
  if(!self||!self->panelTraceLineOriginal)return;
  self->notePanelDraw("AddLine",reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  self->panelTraceLineOriginal(list,from,to,colour,thickness,flags,rounding);
 }
 void armPanelTrace(){
  if(!engine)return;
  if(const char* trace=std::getenv("TC_PANEL_TRACE"))panelTrace=*trace&&trace[0]!='0';
  if(!panelTrace)return;
  auto text=GetProcAddress(engine,"ImDrawList_AddText_Vec2");
  if(!text)text=GetProcAddress(engine,"ImDrawList_AddText");
  if(text&&MH_CreateHook(reinterpret_cast<void*>(text),
                         reinterpret_cast<void*>(&NativeRuntime::detourPanelTraceText),
                         reinterpret_cast<void**>(&panelTraceTextOriginal))==MH_OK&&
     MH_EnableHook(reinterpret_cast<void*>(text))==MH_OK){
   ownedHooks.insert(reinterpret_cast<void*>(text));
   loaderOwned[reinterpret_cast<void*>(text)]="component render panel trace (text)";
  }else panelTraceTextOriginal=nullptr;
  auto line=GetProcAddress(engine,"ImDrawList_AddLine");
  if(line&&MH_CreateHook(reinterpret_cast<void*>(line),
                         reinterpret_cast<void*>(&NativeRuntime::detourPanelTraceLine),
                         reinterpret_cast<void**>(&panelTraceLineOriginal))==MH_OK&&
     MH_EnableHook(reinterpret_cast<void*>(line))==MH_OK){
   ownedHooks.insert(reinterpret_cast<void*>(line));
   loaderOwned[reinterpret_cast<void*>(line)]="component render panel trace (line)";
  }else panelTraceLineOriginal=nullptr;
  logger("Component render: panel draw trace armed");
 }
 /* ---- the preview block the description panel draws (2026-09-28) -----------

    Third report on the same panel: "清掉引脚图示" - with the picture transparent
    and the description text gone, the player still saw the component's pin
    pictogram ("- R") beside the value rows.  The pictogram is not a markup box
    and not one of the igImage call sites: it is artwork of the *preview block*
    the panel builds through
    build_custom_component_preview__presenterZboard95uiZbottom95panelZcommon_u486
    (executable RVA 0x38e990).  That function paints the component's snapshot
    picture, its pin stubs (ImDrawList_AddRectFilled @0x38efac) and the pin
    labels (igText @0x38f0ea) - all of it art for a component that has no
    schematic behind it.

    The panel calls it exactly once, from 0x39dc09, so the detour only has to
    answer for that call site: while every selected component belongs to a Mod
    that asked for the foundry button to be gone, the call is dropped and the
    panel keeps only its own header and the Mod's rows.  A stock component's
    panel is untouched. */
 using ComponentPreviewFn=void(*)(void*);
 ComponentPreviewFn componentPreviewOriginal=nullptr;
 int hiddenPreviewLoggedCount=0;
 static void detourComponentPreview(void* component){
  auto* self=activeInstance;
  if(!self||!self->componentPreviewOriginal)return;
  const uintptr_t caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  if(caller==self->gameRva(0x39dc0e)){
   const int64_t frame=self->engineFrame();
   if(self->hiddenFoundryFrame!=frame){self->hiddenFoundryFrame=frame;
                                       self->refreshHiddenFoundryIndices();}
   if(self->foundryButtonHidden()){
    if(self->hiddenPreviewLoggedCount!=1){
     self->hiddenPreviewLoggedCount=1;
     self->logger("Component render: the panel preview block (pin pictogram) is "
                  "hidden for the selected Mod component");
    }
    return;
   }
  }
  self->componentPreviewOriginal(component);
 }
 void armComponentPreviewControl(){
  if(!symbols)return;
  auto found=symbols->values.find(
      "build_custom_component_preview__presenterZboard95uiZbottom95panelZcommon_u486");
  if(found==symbols->values.end()||!found->second){
   logger("Component render: the panel preview block is unavailable");return;}
  if(MH_CreateHook(found->second,
                   reinterpret_cast<void*>(&NativeRuntime::detourComponentPreview),
                   reinterpret_cast<void**>(&componentPreviewOriginal))!=MH_OK)return;
  if(MH_EnableHook(found->second)!=MH_OK){
   MH_RemoveHook(found->second);componentPreviewOriginal=nullptr;return;}
  ownedHooks.insert(found->second);
  loaderOwned[found->second]="component render panel preview block control";
  logger("Component render: panel preview block control armed");
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
 static void detourBuildComponentMenu(void* model,void* context,void* a,void* b,
                                      uint64_t scaleBits){
  auto* self=activeInstance;
  if(!self||!self->buildComponentMenuOriginal)return;
  if(context&&self->componentMenuGeneration&&self->reloadComponentMenu&&
     (self->componentMenuAppliedContext!=context||
      self->componentMenuAppliedGeneration!=self->componentMenuGeneration)){
   /* `true` also recomputes the component budget after replacing the tree.
      This is the game's normal full-refresh path and is required for a newly
      added custom leaf to be enabled rather than merely present. */
   self->reloadComponentMenu(model,context,1);
   self->componentMenuAppliedContext=context;
   self->componentMenuAppliedGeneration=self->componentMenuGeneration;
   self->logger("Component runtime: refreshed the visible component column at generation "+
                std::to_string(self->componentMenuGeneration));
  }
  self->buildComponentMenuOriginal(model,context,a,b,scaleBits);
 }
 void armComponentMenuRefresh(){
  if(!symbols)return;
  auto build=symbols->values.find("build__presenterZboard95uiZcomponent95menu_u6");
  auto reload=symbols->values.find("reload_component_menu__presenterZutilities_u14192");
  if(build==symbols->values.end()||!build->second||reload==symbols->values.end()||!reload->second){
   logger("Component runtime: visible component-column refresh is unavailable");
   return;
  }
  reloadComponentMenu=reinterpret_cast<ReloadComponentMenu>(reload->second);
  if(MH_CreateHook(build->second,reinterpret_cast<void*>(&NativeRuntime::detourBuildComponentMenu),
                   reinterpret_cast<void**>(&buildComponentMenuOriginal))!=MH_OK||
     MH_EnableHook(build->second)!=MH_OK){
   MH_RemoveHook(build->second);buildComponentMenuOriginal=nullptr;reloadComponentMenu=nullptr;
   logger("Component runtime: component-column refresh hook failed");
   return;
  }
  buildComponentMenuTarget=build->second;ownedHooks.insert(build->second);
  loaderOwned[build->second]="runtime component-column refresh";
  logger("Component runtime: visible component-column refresh armed");
 }
 bool hasComponentPanelSlots(){
  for(auto& p:loaded){if(!p->active)continue;
   for(auto& slot:p->slots)if(slot.kind==TC_UI_SLOT_BOARD_COMPONENT_PANEL&&!slot.failed)return true;}
  return false;
 }
 /* True once any active Mod declared editable rows, so the drawer hook has a
    reason to look for the selected component at all. */
 bool hasComponentEditorRows(){
  for(auto& p:loaded){if(!p||!p->active)continue;if(!p->editorFields.empty())return true;}
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
  if(!gameThreadId||GetCurrentThreadId()!=gameThreadId)return;
  const bool haveSlots=hasComponentPanelSlots();
  /* The host's declarative editor rows are drawn for a type whose Mod declared
     them, with or without a panel slot: a Mod that only ships a field table
     gets an editor. */
  if(!haveSlots&&!hasComponentEditorRows())return;
  uint64_t instance=0,customId=0;
  const bool shown=shownComponent(&instance,&customId);
  /* The draw trace (TC_PANEL_TRACE=1) only keeps what is drawn while the drawer
     describes one of our components. */
  panelShowsModComponent=shown;
  /* Bounded diagnostic: what the drawer is describing, logged when that changes
     (and the first few times, so an empty board still says something).  This is
     the first thing to check when a Mod's rows do not appear: no line for the
     selected type means the drawer never showed it, and a line with the type but
     no rows means the host decided not to draw them. */
  static int traced=0;
  static uint64_t lastShown=~uint64_t(0);
  /* `shown` is a bool and `lastShown` an id, so comparing them directly was
     always true: the line below logged every frame (measured: 453 lines in one
     test run, one file write per frame).  Compare the id. */
  const uint64_t shownId=shown?customId:0;
  if(traced<3||shownId!=lastShown){
   ++traced;
   lastShown=shownId;
   logger(std::string("Component panel: the drawer drew, selected=")+
          std::to_string(selectedComponentSet&&selectionLen?selectionLen(selectedComponentSet):0)+
          " showing "+
          (shown?("custom "+hexValue(customId)+" instance "+hexValue(instance)):
                 std::string("no Mod component")));
  }
  if(!shown)return;
  /* The game's own fold switch is the last item it submitted in the title row,
     which is exactly where this hook runs.  Its rectangle is remembered so the
     row trace can publish it and a playtest can click it like a player would -
     the loader never guesses where the game put its own button. */
  {
   const EditorApi& editor=editorApi();
   if(editor.itemRectMin&&editor.itemRectMax){
    EditorApi::Vec2 low{},high{};
    editor.itemRectMin(&low);editor.itemRectMax(&high);
    panelToggleMin[0]=low.x;panelToggleMin[1]=low.y;
    panelToggleMax[0]=high.x;panelToggleMax[1]=high.y;
   }
   /* Published here, not from the row window: this runs in *both* drawer states,
      so a reader can find the unfold switch while the drawer is folded (its
      position moves with the fold, and clicking the remembered one misses).
      Throttled like the row trace. */
   if(editorTraceEnabled()){
    const DWORD now=GetTickCount64();
    if(now-lastEditorPanelTrace>900){
     lastEditorPanelTrace=now;
     EditorApi::Vec2 pos{},size{};
     if(editor.windowPos)editor.windowPos(&pos);
     if(editor.windowSize)editor.windowSize(&size);
     logger("Component panel: drawer geometry pos="+std::to_string((int)pos.x)+","+
            std::to_string((int)pos.y)+" size="+std::to_string((int)size.x)+","+
            std::to_string((int)size.y)+" toggle="+
            std::to_string((int)panelToggleMin[0])+","+std::to_string((int)panelToggleMin[1])+
            ","+std::to_string((int)panelToggleMax[0])+","+
            std::to_string((int)panelToggleMax[1]));
    }
   }
  }
  const SlotApi& api=slotApi();
  if(!api.available())return;
  const float width=api.getWindowWidth?api.getWindowWidth():0.f;
  const float height=api.getWindowHeight?api.getWindowHeight():0.f;
  if(!haveSlots){
   drawComponentEditorRows(instance,customId,width,height);
   return;
  }
  for(auto& p:loaded){
   if(!p->active)continue;
   if(std::find(p->componentIds.begin(),p->componentIds.end(),customId)==p->componentIds.end())continue;
   for(auto& slot:p->slots){
    if(slot.failed||slot.kind!=TC_UI_SLOT_BOARD_COMPONENT_PANEL)continue;
    api.pushId(p->id.c_str());
    api.pushId(slot.id.c_str());
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
  /* After the Mod's own panel rows, so a custom editor keeps the top of the
     panel and the declarative rows sit with the game's own fields. */
  drawComponentEditorRows(instance,customId,width,height);
 }
 /* ---- tc.component.editor V1: host-drawn property rows ------------------

    Plan §14 asks for a declarative property editor: a type declares its fields
    once, the host edits them.  The reason the host owns the rows instead of
    every Mod is measured, not stylistic - drawing widgets straight into the
    game's own drawer leaves them visible but dead (`IsWindowHovered` is false
    for the drawer while the pointer sits on a row drawn inside it, see
    docs/sdk/ui.md), so today each Mod repeats the same "window layered over the
    panel" recipe (examples/float-ops/components_ui.cpp).  The loader repeats it
    once, for every Mod, and commits each edit through the same configuration
    path tc.component.storage writes use - one committed field is one undo step
    and the type's on_config_changed fires.

    Layout is the working Mod's, unchanged: the row window is pinned to the
    drawer's content origin (the cursor's screen position minus its window-local
    one) at the fractions of the panel the game's own fields occupy.  The window
    is emitted after the drawer, so it stays in front without taking focus - and
    it must not call setNextWindowFocus, which would stop the game's top menu
    from receiving input while such a component stays selected. */
 struct EditorApi {
  struct Vec2{float x,y;};
  void (*cursorScreen)(Vec2*)=nullptr;
  float (*cursorPosX)()=nullptr;
  float (*cursorPosY)()=nullptr;
  void (*setNextWindowPos)(Vec2,int,Vec2)=nullptr;
  /* Bringing the row window to the front.  Without it the order is whatever
     ImGui's window list says, and the game's own bottom panel is re-created when
     a level is entered - measured 2026-09-28 from the player's report "re-enter
     the sandbox and the input boxes disappear": the re-created panel was appended
     after the row window and painted over it (the rows are NoBackground), so the
     widgets were still submitted and traced but invisible. */
  void (*setNextWindowFocus)()=nullptr;
  /* The row window's own text scale: the game's UI font is menu-sized, and a
     property row wants something closer to a form (see kEditorRowFontScale). */
  void (*setWindowFontScale)(float)=nullptr;
  void (*sizeConstraints)(Vec2,Vec2,void*,void*)=nullptr;
  bool (*begin)(const char*,bool*,int)=nullptr;
  void (*end)()=nullptr;
  void (*pushId)(const char*)=nullptr;
  void (*popId)()=nullptr;
  void (*text)(const char*,const char*)=nullptr;
  void (*textDisabled)(const char*,const char*)=nullptr;
  void (*separator)()=nullptr;
  void (*sameLine)(float,float)=nullptr;
  void (*setNextItemWidth)(float)=nullptr;
  bool (*checkbox)(const char*,bool*)=nullptr;
  bool (*radioButton)(const char*,bool)=nullptr;
  bool (*inputInt)(const char*,int*,int,int,int)=nullptr;
  bool (*inputFloat)(const char*,float*,float,float,const char*,int)=nullptr;
  bool (*inputText)(const char*,char*,unsigned long long,int,void*,void*)=nullptr;
  bool (*button)(const char*,Vec2)=nullptr;
  bool (*beginCombo)(const char*,const char*,int)=nullptr;
  void (*endCombo)()=nullptr;
  bool (*selectable)(const char*,bool,int,Vec2)=nullptr;
  bool (*colorEdit4)(const char*,float*,int)=nullptr;
  bool (*beginPopupContextItem)(const char*,int)=nullptr;
  void (*endPopup)()=nullptr;
  bool (*invisibleButton)(const char*,Vec2,int)=nullptr;
  /* Marks the *next* submitted item as one a later item may overlap.  A custom
     row's canvas is submitted before its resize grip, so without this the canvas
     takes the ActiveId and the grip (a small item on top of it) can never be
     clicked - measured on this build: the press reached the canvas every time. */
  void (*setNextItemAllowOverlap)()=nullptr;
  bool (*itemClicked)(int)=nullptr;
  bool (*itemDeactivated)()=nullptr;
  bool (*mouseDoubleClicked)(int)=nullptr;
  bool (*mouseDragging)(int,float)=nullptr;
  void (*mouseDragDelta)(Vec2*,int,float)=nullptr;
  void (*mousePos)(Vec2*)=nullptr;
  void (*setCursorScreen)(Vec2)=nullptr;
  void* (*windowDrawList)()=nullptr;
  bool (*deactivatedAfterEdit)()=nullptr;
  bool (*itemHovered)(int)=nullptr;
  bool (*itemActive)()=nullptr;
  bool (*itemFocused)()=nullptr;
  void (*itemRectMin)(Vec2*)=nullptr;
  void (*itemRectMax)(Vec2*)=nullptr;
  /* Text metrics: the label columns and the row's own text height are measured
     with these two rather than read back from the last item's rectangle (see
     drawComponentEditorRows). */
  /* cimgui's `pOut` convention, the same shape as igGetItemRectMin above (the
     executable's own call site passes the result buffer in the first slot). */
  void (*calcTextSize)(Vec2*,const char*,const char*,bool,float)=nullptr;
  float (*textLineHeight)()=nullptr;
  void* (*getIo)()=nullptr;
  void (*windowPos)(Vec2*)=nullptr;
  void (*windowSize)(Vec2*)=nullptr;
  float (*scrollMaxY)()=nullptr;
  void (*beginTooltip)()=nullptr;
  void (*endTooltip)()=nullptr;
  bool available() const {
   return cursorScreen&&cursorPosX&&cursorPosY&&setNextWindowPos&&begin&&end&&pushId&&popId&&
          text&&textDisabled&&sameLine&&setNextItemWidth&&checkbox&&inputInt&&inputFloat&&
          inputText&&button&&separator&&radioButton&&deactivatedAfterEdit&&itemRectMin&&
          itemRectMax&&itemActive&&
          itemFocused&&getIo&&windowPos&&windowSize&&scrollMaxY;
  }
 };
 static EditorApi& editorApi(){
  static EditorApi value;static bool bound=false;
  if(!bound){bound=true;
   HMODULE module=GetModuleHandleW(L"tc_game_engine.dll");if(!module)module=GetModuleHandleW(nullptr);
   value.cursorScreen=(decltype(value.cursorScreen))proc(module,"igGetCursorScreenPos");
   value.cursorPosX=(decltype(value.cursorPosX))proc(module,"igGetCursorPosX");
   value.cursorPosY=(decltype(value.cursorPosY))proc(module,"igGetCursorPosY");
   value.setNextWindowPos=(decltype(value.setNextWindowPos))proc(module,"igSetNextWindowPos");
   value.setNextWindowFocus=(decltype(value.setNextWindowFocus))proc(module,"igSetNextWindowFocus");
   value.setWindowFontScale=(decltype(value.setWindowFontScale))proc(module,"igSetWindowFontScale");
   value.sizeConstraints=(decltype(value.sizeConstraints))proc(module,"igSetNextWindowSizeConstraints");
   value.begin=(decltype(value.begin))proc(module,"igBegin");
   value.end=(decltype(value.end))proc(module,"igEnd");
   value.pushId=(decltype(value.pushId))proc(module,"igPushID_Str");
   value.popId=(decltype(value.popId))proc(module,"igPopID");
   value.text=(decltype(value.text))proc(module,"igTextUnformatted");
   value.textDisabled=(decltype(value.textDisabled))proc(module,"igTextDisabled");
   value.separator=(decltype(value.separator))proc(module,"igSeparator");
   value.sameLine=(decltype(value.sameLine))proc(module,"igSameLine");
   value.setNextItemWidth=(decltype(value.setNextItemWidth))proc(module,"igSetNextItemWidth");
   value.checkbox=(decltype(value.checkbox))proc(module,"igCheckbox");
   value.radioButton=(decltype(value.radioButton))proc(module,"igRadioButton_Bool");
   value.inputInt=(decltype(value.inputInt))proc(module,"igInputInt");
   value.inputFloat=(decltype(value.inputFloat))proc(module,"igInputFloat");
   value.inputText=(decltype(value.inputText))proc(module,"igInputText");
   value.button=(decltype(value.button))proc(module,"igButton");
   value.beginCombo=(decltype(value.beginCombo))proc(module,"igBeginCombo");
   value.endCombo=(decltype(value.endCombo))proc(module,"igEndCombo");
   value.selectable=(decltype(value.selectable))proc(module,"igSelectable_Bool");
   value.colorEdit4=(decltype(value.colorEdit4))proc(module,"igColorEdit4");
   value.beginPopupContextItem=(decltype(value.beginPopupContextItem))proc(module,"igBeginPopupContextItem");
   value.endPopup=(decltype(value.endPopup))proc(module,"igEndPopup");
   value.invisibleButton=(decltype(value.invisibleButton))proc(module,"igInvisibleButton");
   value.setNextItemAllowOverlap=(decltype(value.setNextItemAllowOverlap))proc(module,"igSetNextItemAllowOverlap");
   value.itemClicked=(decltype(value.itemClicked))proc(module,"igIsItemClicked");
   value.itemDeactivated=(decltype(value.itemDeactivated))proc(module,"igIsItemDeactivated");
   value.mouseDoubleClicked=(decltype(value.mouseDoubleClicked))proc(module,"igIsMouseDoubleClicked_Nil");
   value.mouseDragging=(decltype(value.mouseDragging))proc(module,"igIsMouseDragging");
   value.mouseDragDelta=(decltype(value.mouseDragDelta))proc(module,"igGetMouseDragDelta");
   value.mousePos=(decltype(value.mousePos))proc(module,"igGetMousePos");
   value.setCursorScreen=(decltype(value.setCursorScreen))proc(module,"igSetCursorScreenPos");
   value.windowDrawList=(decltype(value.windowDrawList))proc(module,"igGetWindowDrawList");
   value.deactivatedAfterEdit=(decltype(value.deactivatedAfterEdit))proc(module,"igIsItemDeactivatedAfterEdit");
   value.itemHovered=(decltype(value.itemHovered))proc(module,"igIsItemHovered");
   value.itemActive=(decltype(value.itemActive))proc(module,"igIsItemActive");
   value.itemFocused=(decltype(value.itemFocused))proc(module,"igIsItemFocused");
   value.itemRectMin=(decltype(value.itemRectMin))proc(module,"igGetItemRectMin");
   value.itemRectMax=(decltype(value.itemRectMax))proc(module,"igGetItemRectMax");
   value.calcTextSize=(decltype(value.calcTextSize))proc(module,"igCalcTextSize");
   value.textLineHeight=(decltype(value.textLineHeight))proc(module,"igGetTextLineHeight");
   value.getIo=(decltype(value.getIo))proc(module,"igGetIO");
   value.windowPos=(decltype(value.windowPos))proc(module,"igGetWindowPos");
   value.windowSize=(decltype(value.windowSize))proc(module,"igGetWindowSize");
   value.scrollMaxY=(decltype(value.scrollMaxY))proc(module,"igGetScrollMaxY");
   value.beginTooltip=(decltype(value.beginTooltip))proc(module,"igBeginTooltip");
   value.endTooltip=(decltype(value.endTooltip))proc(module,"igEndTooltip");
  }
  return value;
 }
 static void draw_settings_page_api(void* raw,const TCFrame*,float width,float){
  static_cast<NativeRuntime*>(raw)->drawSettingsPage(width);
 }
 void drawSettingsPage(float width){
  auto& api=editorApi();
  if(!api.text||!api.textDisabled||!api.separator||!api.pushId||!api.popId||
     !api.sameLine||!api.setNextItemWidth||!api.checkbox||!api.inputInt||
     !api.inputFloat||!api.inputText||!api.radioButton||!api.button)return;
  const float controlWidth=std::max(160.f,std::min(420.f,width*0.42f));
  const auto drawScope=[&](uint32_t scope,const char* title){
   bool any=false;for(auto& p:loaded)if(p->active)for(auto& field:p->settings)
    if(field.scope==scope)any=true;
   if(!any)return;
   api.text(title,nullptr);api.separator();
   for(auto& p:loaded){
    if(!p->active)continue;
    bool owns=false;for(auto& field:p->settings)if(field.scope==scope)owns=true;
    if(!owns)continue;
    api.pushId(p->id.c_str());api.text(p->id.c_str(),nullptr);
    for(auto& field:p->settings){
     if(field.scope!=scope)continue;
     api.pushId(field.id.c_str());
     api.text(field.label.c_str(),nullptr);api.sameLine(0.f,-1.f);
     mod_settings::Value proposed=field.current;bool changed=false;
     api.setNextItemWidth(controlWidth);
     if(field.type==TC_MOD_SETTING_BOOL){
      bool value=field.current.integer!=0;
      if(api.checkbox("###value",&value)){proposed.integer=value?1:0;changed=true;}
     }else if(field.type==TC_MOD_SETTING_INT){
      int value=(int)field.current.integer;
      const int step=(int)std::min<int64_t>(field.stepInteger,INT32_MAX);
      const int fast=(int)std::min<int64_t>((int64_t)step*10,INT32_MAX);
      if(api.inputInt("###value",&value,step,fast,0)){
       proposed.integer=std::max<int64_t>(field.minimumInteger,
                         std::min<int64_t>(field.maximumInteger,value));changed=true;}
     }else if(field.type==TC_MOD_SETTING_FLOAT){
      float value=(float)field.current.number;
      if(api.inputFloat("###value",&value,(float)field.stepNumber,
                        (float)(field.stepNumber*10.0),"%.6g",0)){
       proposed.number=std::max(field.minimumNumber,
                       std::min(field.maximumNumber,(double)value));changed=true;}
     }else if(field.type==TC_MOD_SETTING_ENUM){
      for(uint32_t i=0;i<field.choices.size();++i){
       if(i)api.sameLine(0.f,-1.f);
       const std::string choiceId=std::to_string(i);api.pushId(choiceId.c_str());
       if(api.radioButton(field.choices[i].c_str(),field.current.enumIndex==i)){
        proposed.enumIndex=i;proposed.integer=i;changed=true;}
       api.popId();
      }
     }else if(field.type==TC_MOD_SETTING_TEXT){
      std::array<char,TC_MOD_SETTING_MAX_TEXT_BYTES+1u> buffer{};
      std::memcpy(buffer.data(),field.current.text.data(),
                  std::min(field.current.text.size(),buffer.size()-1));
      if(api.inputText("###value",buffer.data(),buffer.size(),0,nullptr,nullptr)){
       proposed.text=buffer.data();changed=true;}
     }
     api.sameLine(0.f,-1.f);
     if(api.button("Reset###reset",EditorApi::Vec2{0.f,0.f})){
      proposed=field.defaultValue;changed=true;
     }
     if(changed){const int status=applySetting(*p,field,proposed);
      if(status!=TC_MOD_SETTINGS_OK)logger("Mod setting change rejected: "+p->id+"/"+
                                          field.id+" status="+std::to_string(status));}
     if(!field.description.empty())api.textDisabled(field.description.c_str(),nullptr);
     api.popId();
    }
    api.separator();api.popId();
   }
  };
  drawScope(TC_MOD_STORAGE_SCOPE_GLOBAL,"Global settings");
  drawScope(TC_MOD_STORAGE_SCOPE_PROFILE,"Current save settings");
 }
 /* ImGui window flags, spelled out because the loader does not include the
    plugin-facing tc_ui.h: no title, no resize/move, no collapse, auto size, no
    background, no saved settings and no focus on appearing.  A scrollbar is
    deliberately *enabled*: a type with many rows is capped at the screen by
    sizeConstraints below, and the rest of its table has to stay reachable. */
 static constexpr int kEditorRowWindowFlags=1|2|4|32|64|128|256|4096;
 /* The rows' text scale (`igSetWindowFontScale`, the entry the loader already
    uses for its own small print).  The game's UI font is sized for menus; a
    property row wants something closer to a form, and every widget in the row -
    the label, the input box, its height - follows this one number. */
 static constexpr float kEditorRowFontScale=0.78f;
  /* The widest a row's input box may get, in multiples of the row's own text
    height (`igGetTextLineHeight`, the metric the game draws its own rows with).
    The drawer spans the screen, and a box that filled the row read as a bar
    rather than a field (player report 2026-09-28: "这个输入框还是太大了，太宽了，
    可以适当缩小") - twelve text heights is a little over twenty Latin characters,
    which is what "0x40200000" and a decimal like "1.7976931348623157e+308" need
    to stay readable. */
  static constexpr float kEditorFieldMaxTextHeights=12.f;
  static constexpr float kEditorFieldMinTextHeights=6.f;
  /* How much room under the drawer's title a row needs to be worth drawing.
     Below it the drawer counts as folded: a folded drawer is just its title row,
     so the space under our own cursor is what tells the two apart. */
  static constexpr float kEditorMinRowRoom=34.f;
  /* The gap the rows keep from the game panel's own edges, so the editor never
     touches (or spills past) the panel it belongs to. */
  static constexpr float kEditorPanelMargin=6.f;
 static bool editorTraceEnabled(){
  static const bool enabled=[](){
   char value[8]{};
   return GetEnvironmentVariableA("TC_EDITOR_TRACE",value,sizeof(value))>0&&value[0]!='0';
  }();
  return enabled;
 }
 /* One instance's working copy of its configuration while the rows are on
    screen, plus the text buffers the string widgets edit in place. */
 struct EditorEditState {
  uint64_t customId=0,instance=0,revision=0;
  std::vector<uint8_t> bytes;
  /* V4 staged tables (TC_COMPONENT_EDITOR_TABLE_CONFIRM): `committed` is what
     the instance really holds and `pending` says the working copy has edits
     that only Apply writes.  Every other table leaves both at their defaults. */
  std::vector<uint8_t> committed;
  bool pending=false;
  bool valid=false;
  std::array<std::array<char,component_editor::kMaxTextBytes+1>,
             component_editor::kMaxFields> texts{};
  std::array<std::string,component_editor::kMaxFields> validation{};
  std::array<int,component_editor::kMaxFields> validationStatus{};
  bool colorTransaction=false;
  std::string colorField;
  bool resizeTransaction=false;
  std::string resizeField;
 };
 std::map<std::pair<uint64_t,uint64_t>,EditorEditState> editorEdits;
 DWORD lastEditorTrace=0;
 /* The row window's own height from the last frame, used to keep it on screen. */
 float lastEditorWindowHeight=0.f;
  /* The frame the rows were last drawn on, so "the panel just appeared" is a
    frame gap rather than a per-frame flag (see the front-order note below). */
  int64_t lastEditorRowFrame=-2;
 /* ...and how much of its content that height was hiding, so the clamp below
    works on the *content* size rather than on what the cap allowed to be shown:
    capping first and clamping afterwards would shrink a seven-row editor to the
    two rows that happen to fit above the screen's bottom edge. */
 float lastEditorScrollMax=0.f;
  /* Whether the game's drawer was open the last time the rows were drawn.  The
    hook on the title row runs in *both* states, so a collapsed drawer has to be
    recognised from the room it leaves under that title: while it is folded the
    rows are not drawn at all, and nothing about them is touched, so opening the
    drawer again brings back the same widgets, the same typed values and the
    same screen position (player requirement 2026-09-28). */
  bool lastEditorPanelOpen=true;
  /* The game's own fold/unfold button in the drawer's title row, read from the
    last item the game submitted.  Logged with the row trace so a playtest can
    click it instead of guessing where the game put it. */
  float panelToggleMin[2]={0.f,0.f},panelToggleMax[2]={0.f,0.f};
  /* When the drawer geometry was last written to the trace. */
  DWORD lastEditorPanelTrace=0;
  /* The last combo popup's rectangle, published with the trace so a playtest can
    click an entry without guessing where ImGui put the popup (the popup is only
    as wide as its widest entry and it flips above the field when it has to). */
  float lastComboPopup[4]={-1.f,-1.f,-1.f,-1.f};
  /* ...and each entry's own rectangle: the entries are not the row's height, so
    a reader that wants to pick one needs the entry, not an estimate. */
  float comboItem[8][4]={{-1.f,-1.f,-1.f,-1.f},{-1.f,-1.f,-1.f,-1.f},{-1.f,-1.f,-1.f,-1.f},
                         {-1.f,-1.f,-1.f,-1.f},{-1.f,-1.f,-1.f,-1.f},{-1.f,-1.f,-1.f,-1.f},
                         {-1.f,-1.f,-1.f,-1.f},{-1.f,-1.f,-1.f,-1.f}};
  /* Whether the last frame had a dropdown popup open, so each fresh open
    publishes its geometry even when it lands on the same rectangle. */
  bool comboOpen=false;
  /* The same for a custom row's context menu, so a playtest can click its
    entries instead of guessing where the popup landed. */
  float menuItem[8][4]={{-1.f,-1.f,-1.f,-1.f},{-1.f,-1.f,-1.f,-1.f},{-1.f,-1.f,-1.f,-1.f},
                        {-1.f,-1.f,-1.f,-1.f},{-1.f,-1.f,-1.f,-1.f},{-1.f,-1.f,-1.f,-1.f},
                        {-1.f,-1.f,-1.f,-1.f},{-1.f,-1.f,-1.f,-1.f}};
  bool menuOpen=false;
  /* The resize grip's own rectangle, published for the same reason: the clip of
    the row window can cut the bottom corner of the last row, so a reader must be
    told where the grip actually ended up. */
  float gripRect[4]={-1.f,-1.f,-1.f,-1.f};
  std::string gripField;
  /* V4: the swatch strip's rectangles, published so a playtest clicks the
     swatch the host drew rather than a point it guessed - the strip's geometry
     follows the row width, which follows the window. */
  float swatchRect[TC_COMPONENT_EDITOR_MAX_PALETTE_SWATCHES][4]={};
  std::string swatchField;
  /* ...and the validation marker's own rectangle plus the message last shown
     in its tooltip, for the same reason. */
  float validationIconRect[4]={-1.f,-1.f,-1.f,-1.f};
  std::string validationIconField,validationTipField;
  /* A staged table's Apply / Cancel buttons, published so the confirm and
     cancel cases can press them. */
  float dialogApplyRect[4]={-1.f,-1.f,-1.f,-1.f};
  float dialogCancelRect[4]={-1.f,-1.f,-1.f,-1.f};
  int dialogPending=-1;
 /* The Mod that declared rows for this type, or null.  Only an active plugin
    can be selected on the board, so a rejected one is skipped. */
 Loaded* editorOwner(uint64_t customId){
  for(auto& p:loaded){
   if(!p||!p->active)continue;
   if(p->editorFields.find(customId)!=p->editorFields.end())return p.get();
  }
  return nullptr;
 }
 /* Draws the rows of one type over the game's drawer.  Called from inside the
    drawer, after the Mod's own panel slots, so a Mod that draws a custom editor
    keeps its rows above the host's. */
 void drawComponentEditorRows(uint64_t instance,uint64_t customId,float width,float height){
  /* Why the host drew no rows for the instance the drawer is showing - the first
     question a Mod author asks when an editor does not appear.  Bounded, because
     this runs per frame while the drawer is open. */
  auto stop=[&](const char* why){
   static int lines=0;
   if(lines<6){++lines;
    logger(std::string("Component editor: skipped (")+why+") instance "+
           hexValue(instance)+" custom "+hexValue(customId));}
  };
  Loaded* owner=editorOwner(customId);
  if(!owner){stop("no rows declared for this type");return;}
  const auto found=owner->editorFields.find(customId);
  if(found==owner->editorFields.end()||found->second.empty()){stop("empty table");return;}
  const std::vector<component_editor::Field>& fields=found->second;
  /* V4: a table declared with TC_COMPONENT_EDITOR_TABLE_CONFIRM keeps its edits
     in the working copy and writes them with Apply. */
  const auto flagsFound=owner->editorTableFlags.find(customId);
  const uint32_t tableFlags=flagsFound==owner->editorTableFlags.end()?0u:flagsFound->second;
  const bool staged=(tableFlags&TC_COMPONENT_EDITOR_TABLE_CONFIRM)!=0;
  EditorApi& api=editorApi();
  if(!api.available()){stop("the engine's editor primitives are missing");return;}
  /* The drawer's own rectangle, read while the hook is still inside the game's
     window (ours does not exist yet), and the first line under the title row,
     which is where the game's own content starts.  Everything below stays inside
     this rectangle: the player requirement is that the editor never covers the
     board and never spills past the game's bottom panel. */
  EditorApi::Vec2 panelPos{},panelSize{};
  api.windowPos(&panelPos);api.windowSize(&panelSize);
  const bool havePanel=panelSize.x>1.f&&panelSize.y>1.f;
  const float panelLeft=havePanel?panelPos.x:0.f;
  const float panelTop=havePanel?panelPos.y:0.f;
  const float panelRight=havePanel?panelPos.x+panelSize.x:0.f;
  const float panelBottom=havePanel?panelPos.y+panelSize.y:0.f;
  EditorApi::Vec2 underTitle{};
  api.cursorScreen(&underTitle);
  /* A folded drawer is a title row and nothing else, and this hook runs in every
     state - so a folded drawer would otherwise have the rows painted over the
     board.  While it is folded nothing is drawn *and* nothing is touched, so
     opening it again brings back the same widgets, the same typed values and the
     same position (player requirement 2026-09-28). */
  const float roomUnderTitle=havePanel?panelBottom-underTitle.y:1e9f;
  if(havePanel&&roomUnderTitle<kEditorMinRowRoom){
   if(lastEditorPanelOpen){
    lastEditorPanelOpen=false;
    logger("Component editor: the drawer is folded ("+
           std::to_string((int)roomUnderTitle)+"px under its title); the rows are "
           "not drawn until it opens again");
   }
   return;
  }
  if(!lastEditorPanelOpen){
   lastEditorPanelOpen=true;
   logger("Component editor: the drawer is open again; the rows return to the same place");
  }
  TCComponentInstanceHandle handle{};
  if(!tc::logic::instanceHandleOf(customId,instance,&handle)){
   editorEdits.erase(std::make_pair(customId,instance));
   stop("the instance is not bound (no handle)");
   return;
  }
  TCComponentStorageInfoV1 info{};
  info.size=sizeof(info);info.version=TC_COMPONENT_STORAGE_INFO_VERSION_1;
  if(tc::logic::storageInfo(&handle,&info)!=TC_COMPONENT_STORAGE_OK){stop("storage info refused");return;}
  if(!info.config_size){stop("the type has no configuration");return;}
  /* Entries outlive a board only until the rows are drawn again; the bound
     keeps a long session from growing without limit. */
  if(editorEdits.size()>64&&editorEdits.find(std::make_pair(customId,instance))==editorEdits.end())
   editorEdits.clear();
  EditorEditState& edit=editorEdits[std::make_pair(customId,instance)];
  edit.customId=customId;edit.instance=instance;
  /* The text buffers are derived from the bytes, so a refused edit or a Cancel
     puts the box back to what the instance really holds: textual kinds each
     have their own spelling, and every other kind has no buffer at all. */
  auto fillTextBuffer=[&](const component_editor::Field& field,size_t index){
   if(index>=edit.texts.size())return;
   edit.texts[index].fill(0);
   component_editor::Value value{};
   if(!component_editor::read(field,edit.bytes.data(),
                              static_cast<uint32_t>(edit.bytes.size()),value))
    return;
   std::string owned;
   const char* text=nullptr;
   switch(field.kind){
    case TC_COMPONENT_FIELD_TEXT: text=value.text.c_str();break;
    case TC_COMPONENT_FIELD_UINT:
     if(field.hex){owned=component_editor::formatHex(field,value.integer);text=owned.c_str();}
     break;
    case TC_COMPONENT_FIELD_FLOAT_WIDTH:
     owned=component_editor::formatFloatText(field,value.integer);text=owned.c_str();break;
    case TC_COMPONENT_FIELD_COLOR_RGBA8:
     if(field.colorHex){
      owned=component_editor::formatColorHex(static_cast<uint32_t>(value.integer));
      text=owned.c_str();
     }
     break;
    default: break;
   }
   if(text)std::snprintf(edit.texts[index].data(),edit.texts[index].size(),"%s",text);
  };
  if(!edit.valid||edit.revision!=info.config_revision||edit.bytes.size()!=info.config_size){
   edit.bytes.assign(info.config_size,0);
   uint32_t written=0;
   if(tc::logic::storageReadConfig(&handle,edit.bytes.data(),info.config_size,&written)!=
      TC_COMPONENT_STORAGE_OK||written!=info.config_size){
    edit.valid=false;
    stop("reading the configuration failed");
    return;
   }
   edit.revision=info.config_revision;
   edit.valid=true;
   /* A fresh read is the baseline a staged table compares against and the
      bytes Cancel restores. */
   edit.committed=edit.bytes;
   edit.pending=false;
   /* The bytes are the truth, so the text buffers start from them.  A buffer
      left over from an earlier board would otherwise be committed back.  A
      hexadecimal field edits text too, and its text is the padded form of the
      stored bits. */
   for(size_t index=0;index<fields.size()&&index<component_editor::kMaxFields;++index)
    fillTextBuffer(fields[index],index);
  }
  /* Runs this Mod's validator against one proposed blob; ERROR also covers a
     callback that threw.  The message lands on the row and in the log, so the
     advisory marker has something to show and the text form has something to
     print. */
  auto runValidator=[&](const component_editor::Field& field,
                        const std::vector<uint8_t>& bytes,size_t fieldIndex){
   if(fieldIndex<edit.validation.size()){
    edit.validation[fieldIndex].clear();edit.validationStatus[fieldIndex]=0;
   }
   if(!field.validateValue)return static_cast<int>(TC_COMPONENT_EDITOR_VALIDATION_OK);
   std::array<char,TC_COMPONENT_EDITOR_MAX_VALIDATION_BYTES> message{};
   int validation=TC_COMPONENT_EDITOR_VALIDATION_ERROR;
   try{
    validation=field.validateValue(field.user,&handle,field.id.c_str(),bytes.data(),
                                   static_cast<uint32_t>(bytes.size()),message.data(),
                                   static_cast<uint32_t>(message.size()));
   }catch(...){
    logger("Component editor: validator threw for field \""+field.id+"\"");
   }
   message.back()=0;
   if(fieldIndex<edit.validation.size()){
    edit.validation[fieldIndex]=message.data();
    edit.validationStatus[fieldIndex]=validation;
   }
   return validation;
  };
  /* Writes one edited field back.  The record is written by the storage path
     first, the undo step is recorded there, and on_config_changed follows with
     the binding lock released - exactly like a Mod's own write_config.

     A staged table (V4's CONFIRM) has the same shape up to the write: the value
     lands in the working copy, the row shows it and the validator runs, but the
     record is not touched until Apply writes the whole blob once. */
  auto commit=[&](const component_editor::Field& field,const component_editor::Value& value){
   const size_t fieldIndex=static_cast<size_t>(&field-fields.data());
   if(staged){
    const std::vector<uint8_t> previous=edit.bytes;
    std::string note;
    if(!component_editor::apply(field,edit.bytes.data(),
                                static_cast<uint32_t>(edit.bytes.size()),value,&note))
     return;
    if(edit.bytes==previous)return;
    const int validation=runValidator(field,edit.bytes,fieldIndex);
    if(validation<0){
     edit.bytes=previous;
     fillTextBuffer(field,fieldIndex);
     if(fieldIndex<edit.validation.size()&&edit.validation[fieldIndex].empty())
      edit.validation[fieldIndex]="value rejected";
     logger("Component editor: validator refused staged field \""+field.id+"\" on instance "+
            hexValue(instance)+": "+edit.validation[fieldIndex]);
     return;
    }
    if(validation>0)
     logger("Component editor: validator warned for field \""+field.id+"\" on instance "+
            hexValue(instance)+": "+edit.validation[fieldIndex]);
    if(!note.empty())logger("Component editor: "+note);
    edit.pending=true;
    logger("Component editor: staged field \""+field.id+"\" on instance "+hexValue(instance)+
           " value="+component_editor::display(field,value));
    return;
   }
   std::vector<uint8_t> next=edit.bytes;
   std::string note;
   if(!component_editor::apply(field,next.data(),static_cast<uint32_t>(next.size()),value,&note))
    return;
   if(next==edit.bytes)return;
   const int validation=runValidator(field,next,fieldIndex);
   if(validation<0){
    if(fieldIndex<edit.validation.size()&&edit.validation[fieldIndex].empty())
     edit.validation[fieldIndex]="value rejected";
    logger("Component editor: validator refused field \""+field.id+"\" on instance "+
           hexValue(instance)+": "+edit.validation[fieldIndex]);
    return;
   }
   if(validation>0)
    logger("Component editor: validator warned for field \""+field.id+"\" on instance "+
           hexValue(instance)+": "+edit.validation[fieldIndex]);
   const int status=tc::logic::storageWriteConfig(&handle,info.config_schema,
                                                next.data(),static_cast<uint32_t>(next.size()));
   if(status!=TC_COMPONENT_STORAGE_OK){
    logger("Component editor: the configuration write for field \""+field.id+
           "\" on instance "+hexValue(instance)+" was refused ("+std::to_string(status)+
           "); the field is unchanged");
    return;
   }
   if(!note.empty())logger("Component editor: "+note);
   edit.valid=false;   /* re-read next frame: the write bumped the revision */
   logger("Component editor: committed field \""+field.id+"\" on instance "+
          hexValue(instance)+" value="+component_editor::display(field,value));
  };
  auto sendAction=[&](const component_editor::Field& field,uint32_t kind,uint32_t actionId,
                      float x,float y,float dx,float dy){
   if(!field.action)return;
   TCComponentEditorActionV1 action{};
   action.size=sizeof(action);action.version=1;action.instance=handle;
   action.field_id=field.id.c_str();action.kind=kind;action.action_id=actionId;
   action.screen_x=x;action.screen_y=y;action.delta_x=dx;action.delta_y=dy;
   action.config=edit.bytes.data();action.config_size=static_cast<uint32_t>(edit.bytes.size());
   fault::Scope mark(owner->id.c_str(),"component editor action");
   try{field.action(field.user,&action);}
   catch(...){logger("Component editor: action callback threw for field \""+field.id+"\"");}
  };
  /* A context-menu entry's rectangle, published (on a fresh open and whenever it
     changes) so a playtest can click the entry rather than guess where ImGui put
     the popup. */
  auto publishMenuEntry=[&](bool fresh,size_t slot,const std::string& fieldId,uint32_t actionId){
   if(slot>=8||!api.itemRectMin||!api.itemRectMax)return;
   EditorApi::Vec2 low{},high{};
   api.itemRectMin(&low);api.itemRectMax(&high);
   if(!fresh&&low.x==menuItem[slot][0]&&low.y==menuItem[slot][1])return;
   menuItem[slot][0]=low.x;menuItem[slot][1]=low.y;
   menuItem[slot][2]=high.x;menuItem[slot][3]=high.y;
   logger("Component editor: menu item instance="+hexValue(instance)+" field=\""+fieldId+
          "\" index="+std::to_string(slot)+" id="+std::to_string(actionId)+" rect="+
          std::to_string((int)low.x)+","+std::to_string((int)low.y)+","+
          std::to_string((int)high.x)+","+std::to_string((int)high.y));
  };
  EditorApi::Vec2 cursor{};api.cursorScreen(&cursor);
  const float originX=cursor.x-api.cursorPosX();
  const float originY=cursor.y-api.cursorPosY();
  /* Where the drawer's own content starts.  The hook runs after the game has
     drawn its title row, so the cursor sits under that title - and the row window
     must never climb above it (player report 2026-09-28: the first input box
     covered the component's name when the panel got shorter). */
  const float contentTop=cursor.y;
  /* The rows start at the panel's own middle, the way the game's description
     block does, so the preview above them stays visible - but never above the
     title row and never past the panel's bottom edge. */
  float top=originY+height*0.52f;
  if(contentTop>top)top=contentTop;
  /* The row window's left edge, kept inside the panel: a window narrower than
     the game's own buttons must shrink the editor, not push it off the drawer
     (a resize has to be absorbed by the layout, not by the panel's edge). */
  float windowLeft=originX+width*0.08f;
  if(havePanel){
   const float leftLimit=panelLeft+kEditorPanelMargin;
   const float rightLimit=std::max(leftLimit,panelRight-kEditorPanelMargin-60.f);
   if(windowLeft<leftLimit)windowLeft=leftLimit;
   if(windowLeft>rightLimit)windowLeft=rightLimit;
  }
  float displayHeight=0.f;
  if(api.getIo) {
   const auto* io=static_cast<const unsigned char*>(api.getIo());
   float display[2]={0.f,0.f};
   if(io)std::memcpy(display,io+8,sizeof(display));
   displayHeight=display[1];
  }
  /* The bottom the rows have to respect: the game panel's own edge, and the
     screen's, whichever is higher.  What the table really needs (its height plus
     whatever it was hiding) decides where it can start, so a table that does not
     fit scrolls instead of spilling out of the panel. */
  const float bottomLimit=havePanel
      ?std::min(panelBottom-kEditorPanelMargin,
                displayHeight>1.f?displayHeight-kEditorPanelMargin:panelBottom)
      :(displayHeight>1.f?displayHeight-8.f:1e9f);
  if(bottomLimit<1e8f){
   if(lastEditorWindowHeight>1.f){
    const float contentHeight=lastEditorWindowHeight+lastEditorScrollMax;
    const float ceiling=bottomLimit-contentHeight;
    if(ceiling>contentTop&&top>ceiling)top=ceiling;
    /* ... and when the table does not fit under the game's own block at all, it
       takes the whole panel below the title row instead of leaving a strip of it
       to the game.  A player who opens a property table wants to see as many of
       its rows as the panel can hold; the game's own preview is what the mod
       draws over anyway once it hides the foundry button (V4/V5). */
    if(bottomLimit-contentHeight<top)top=contentTop;
   }
   if(top>bottomLimit-kEditorMinRowRoom)top=bottomLimit-kEditorMinRowRoom;
   if(top<contentTop)top=contentTop;
   if(!havePanel&&top<8.f)top=8.f;
  }
  /* How big the row window may become: the panel's box is the outer limit, the
     screen is the fallback for a loader whose engine reports no panel. */
  const float maxWindowWidth=havePanel
      ?std::max(60.f,panelRight-kEditorPanelMargin-windowLeft)
      :100000.f;
  const float maxWindowHeight=std::max(kEditorMinRowRoom,bottomLimit-top);
  /* One decision per frame, not per field: the throttled trace has to report
     *every* row of that frame, or a reader (and the playtest driver) only ever
     sees the first field. */
  bool traceFrame=false;
  if(editorTraceEnabled()){
   const DWORD now=GetTickCount64();
   if(now-lastEditorTrace>900){lastEditorTrace=now;traceFrame=true;}
  }
  api.setNextWindowPos(EditorApi::Vec2{windowLeft,top},1,EditorApi::Vec2{0.f,0.f});
  /* ... and it has to be the window ImGui paints last.  The rows sit over the
     game's own panel with no background of their own, so whichever window comes
     later in the frame wins; a re-entered level re-creates the panel, which was
     enough to bury them.

     Bringing it to the front is a *focus* operation, so it is done once per
     appearance, not every frame: focusing every frame closed whatever popup the
     player had just opened - the player's report "with the bottom panel open the
     top menu bar does not work".  One call is enough, because ImGui keeps the
     window order until something else is focused, and the panel is only
     re-created while it is closed (entering a level), which is exactly when the
     gap below is large. */
  const int64_t editorFrame=engineFrame();
  const bool panelJustAppeared=editorFrame!=lastEditorRowFrame+1;
  lastEditorRowFrame=editorFrame;
  if(api.setNextWindowFocus&&panelJustAppeared)api.setNextWindowFocus();
  /* Cap the window at the space under its own top edge, and let ImGui scroll
     what does not fit.  Without this a type with many rows would grow past the
     screen and its lower rows could be drawn but never clicked; the clamp above
     only moves the whole window, it cannot make it smaller. */
  if(api.sizeConstraints)
   api.sizeConstraints(EditorApi::Vec2{0.f,0.f},
                       EditorApi::Vec2{maxWindowWidth,maxWindowHeight},nullptr,nullptr);
  if(api.begin("###tc-editor-rows",nullptr,kEditorRowWindowFlags)){
   api.pushId("tc-editor");
   /* Smaller text than the game's UI: the drawer's rows are dense - a label, a
      boxed value and often a third - and at the stock size they were tall enough
      to cover the panel's own title (player report 2026-09-28: "这个输入框和对应
      的文字明显太大了…排版要好看").  The scale belongs to this window, so the
      game and every other Mod keep their own type size; ImGui's input boxes also
      shrink with it, which is what makes the rows read as one block. */
   if(api.setWindowFontScale)api.setWindowFontScale(kEditorRowFontScale);
     /* The label columns are *measured*, not a fraction of the panel: the game's
        bottom drawer spans the screen, so a fraction left the label and its field
        a hand's width apart (player report 2026-09-28: "输入框和旁边的文字离得太远").

        They are measured from the *text*, never from the previous item's
        rectangle.  A rectangle says where the last submission landed, and an
        inline field's label is submitted after the field beside it, so that
        rectangle reported the pair's position instead of the label's width - and
        feeding it back into the column moved the pair a little further right
        every frame.  Measured on the editor-demo type (2026-09-28): the column
        went 204 -> 11981 -> ... -> 1.6 million pixels in eight trace frames, its
        fields left the screen and the host's own rows could no longer be clicked
        (tests/component-editor-rows).  igCalcTextSize answers with the metrics
        the game draws this very text with, including the row window's font
        scale, and a type's labels never change, so the numbers are stable. */
     float firstLabelWidth=0.f,secondLabelWidth=0.f;
     for(const component_editor::Field& field:fields){
      EditorApi::Vec2 extent{};
      if(api.calcTextSize)api.calcTextSize(&extent,field.label.c_str(),nullptr,false,0.f);
      if(field.inlineWithPrevious){
       if(extent.x>secondLabelWidth)secondLabelWidth=extent.x;
      }else if(extent.x>firstLabelWidth)firstLabelWidth=extent.x;
     }
     /* The row's own text height, which is what the field width is capped
        against.  `width*0.02` is only a fallback for a loader whose ImGui has no
        line-height entry at all. */
     const float lineHeight=api.textLineHeight?api.textLineHeight():0.f;
     const float ink=lineHeight>1.f?lineHeight:width*0.02f;
     /* The column starts at a compact fixed share of the drawer (a measured
        column alone is wrong on the first frame - measured while building this:
        the field landed on top of its label) and grows when a label really is
        wider than that. */
     const float labelFloor=width*0.08f;
     const float rowLeft=firstLabelWidth+10.f>labelFloor?firstLabelWidth+10.f:labelFloor;
     const float rowRight=width*0.06f;   /* a margin, so the box never touches the edge */
     float fieldWidth=width-rowLeft-rowRight;
     /* ... but not the whole row: a form field, not a bar.  The cap is in text
        heights because that is the metric the game's own rows are built from. */
     const float fieldCap=ink*kEditorFieldMaxTextHeights;
     const float fieldFloor=ink*kEditorFieldMinTextHeights;
     if(fieldWidth>fieldCap)fieldWidth=fieldCap;
     if(fieldWidth<fieldFloor)fieldWidth=fieldFloor;
     if(fieldWidth<60.f)fieldWidth=60.f;
     /* ... and never past the panel's right edge: a narrower game window has to
        shrink the fields, not push them out of the drawer. */
     if(havePanel){
      const float room=panelRight-kEditorPanelMargin-(windowLeft+rowLeft);
      if(room>60.f&&fieldWidth>room)fieldWidth=room;
     }
     /* A pair of fields sharing a line: both halves are the same width, the second
        label starts after the first field and the second field after that label -
        all three offsets computed here, so nothing has to be read back. */
     const float pairFieldWidth=fieldWidth*0.5f;
     const float pairLabelX=rowLeft+pairFieldWidth+10.f;
     const float pairFieldX=pairLabelX+secondLabelWidth+10.f;
  for(size_t index=0;index<fields.size();++index){
   const component_editor::Field& field=fields[index];
   component_editor::Value value{};
   if(!component_editor::read(field,edit.bytes.data(),
                              static_cast<uint32_t>(edit.bytes.size()),value))
    continue;
   /* A custom row's canvas rectangle.  The trace reports it instead of "the
      last submitted item", because a custom row with a resize grip ends on the
      tiny grip - and a reader (the playtest driver) needs the canvas it can
      click and drag across, not the handle in its corner. */
   EditorApi::Vec2 customMin{},customMax{};
   if(field.separatorBefore)api.separator();
    /* Sharing a line means going back to the row the previous field is on, at the
       offset the pair's own layout asked for. */
    if(field.inlineWithPrevious)api.sameLine(pairLabelX,-1.f);
    api.pushId(field.id.c_str());
    api.text(field.label.c_str(),nullptr);
    if(api.itemHovered&&api.itemHovered(0)&&!field.tooltip.empty()&&api.beginTooltip){
     api.beginTooltip();api.text(field.tooltip.c_str(),nullptr);
     if(api.endTooltip)api.endTooltip();
    }
    if(field.kind==TC_COMPONENT_FIELD_CUSTOM){
     api.sameLine(field.inlineWithPrevious?pairFieldX:rowLeft,-1.f);
     const float customHeight=field.customHeight>0.f?field.customHeight:ink*2.f;
     /* Let the resize grip (submitted after this canvas) win the hover where the
        two overlap. */
     if(field.resizeHandle&&api.setNextItemAllowOverlap)api.setNextItemAllowOverlap();
     if(api.invisibleButton)api.invisibleButton("##custom",EditorApi::Vec2{fieldWidth,customHeight},0);
     EditorApi::Vec2 low{},high{};api.itemRectMin(&low);api.itemRectMax(&high);
     customMin=low;customMax=high;
     /* Context and double-click belong to the whole custom canvas, before a
        possible resize grip becomes the last submitted item. */
     if(field.action&&api.itemHovered&&api.itemHovered(0)&&api.mouseDoubleClicked&&
        api.mouseDoubleClicked(0)){
      EditorApi::Vec2 mouse{};if(api.mousePos)api.mousePos(&mouse);
      sendAction(field,TC_COMPONENT_EDITOR_ACTION_DOUBLE_CLICK,0,mouse.x,mouse.y,0.f,0.f);
     }
     if(field.action&&!field.contextActionIds.empty()&&api.beginPopupContextItem&&
        api.selectable&&api.endPopup){
      if(api.beginPopupContextItem("##context",1)){
       const bool freshMenu=!menuOpen;menuOpen=true;
       for(size_t actionIndex=0;actionIndex<field.contextActionIds.size();++actionIndex){
        if(api.selectable(field.contextActionLabels[actionIndex].c_str(),false,0,
                          EditorApi::Vec2{0.f,0.f})){
         EditorApi::Vec2 mouse{};if(api.mousePos)api.mousePos(&mouse);
         sendAction(field,TC_COMPONENT_EDITOR_ACTION_MENU,field.contextActionIds[actionIndex],
                    mouse.x,mouse.y,0.f,0.f);
        }
        publishMenuEntry(freshMenu,actionIndex,field.id,field.contextActionIds[actionIndex]);
       }
       api.endPopup();
      }else menuOpen=false;
     }else menuOpen=false;
     if(field.drawCustom&&api.windowDrawList){
      void* list=api.windowDrawList();
      RenderDrawContext drawContext{this,list,0,true,owner};
      const TCComponentRenderDrawV2 draw{
          sizeof(TCComponentRenderDrawV2),TC_COMPONENT_RENDER_DRAW_VERSION_2,&drawContext,
          &component_render_line,&component_render_rect,&component_render_rect_filled,
          &component_render_circle,&component_render_circle_filled,&component_render_text,
          &component_render_text_sized,&component_render_measure_text};
      TCComponentEditorCustomRowV1 row{};row.size=sizeof(row);row.version=1;
      row.instance=handle;row.field_id=field.id.c_str();
      row.min_x=low.x;row.min_y=low.y;row.max_x=high.x;row.max_y=high.y;
      row.config=edit.bytes.data();row.config_size=static_cast<uint32_t>(edit.bytes.size());
      row.draw=reinterpret_cast<const TCComponentRenderDrawV1*>(&draw);
      if(renderPushClip)renderPushClip(list,{low.x,low.y},{high.x,high.y},true);
      fault::Scope mark(owner->id.c_str(),"component editor custom row");
      try{field.drawCustom(field.user,&row);}
      catch(...){logger("Component editor: custom row callback threw for field \""+field.id+"\"");}
      drawContext.active=false;
      if(renderPopClip)renderPopClip(list);
     }
     if(field.resizeHandle&&field.action&&api.invisibleButton&&api.setCursorScreen){
      EditorApi::Vec2 after{};api.cursorScreen(&after);
      /* The grip has to be reachable even when its row is the last one and the
         window is capped at the panel's bottom edge: it is sized generously and
         then clamped inside the window's own rectangle. */
      const float grip=std::max(14.f,ink*0.9f);
      float gripTop=high.y-grip;
      if(api.windowPos&&api.windowSize){
       EditorApi::Vec2 winPos{},winSize{};
       api.windowPos(&winPos);api.windowSize(&winSize);
       /* The window's own padding sits inside its rectangle, so the grip is kept
          above that line rather than at the bare edge - a clipped corner cannot
          be pressed, by the player or by a playtest. */
       const float innerBottom=winPos.y+winSize.y-10.f;
       if(gripTop+grip>innerBottom)gripTop=innerBottom-grip;
      }
      api.setCursorScreen(EditorApi::Vec2{high.x-grip,gripTop});
      api.invisibleButton("##resize",EditorApi::Vec2{grip,grip},0);
      if(api.itemRectMin&&api.itemRectMax){
       EditorApi::Vec2 gripMin{},gripMax{};
       api.itemRectMin(&gripMin);api.itemRectMax(&gripMax);
       if(gripMin.x!=gripRect[0]||gripMin.y!=gripRect[1]||field.id!=gripField){
        gripRect[0]=gripMin.x;gripRect[1]=gripMin.y;
        gripRect[2]=gripMax.x;gripRect[3]=gripMax.y;
        gripField=field.id;
        logger("Component editor: grip instance="+hexValue(instance)+" field=\""+field.id+
               "\" rect="+std::to_string((int)gripMin.x)+","+std::to_string((int)gripMin.y)+
               ","+std::to_string((int)gripMax.x)+","+std::to_string((int)gripMax.y));
       }
      }
      EditorApi::Vec2 mouse{};if(api.mousePos)api.mousePos(&mouse);
      if(api.itemClicked&&api.itemClicked(0))
       logger("Component editor: the resize grip of field \""+field.id+"\" on instance "+
              hexValue(instance)+" was pressed at "+std::to_string((int)mouse.x)+","+
              std::to_string((int)mouse.y));
      if(api.itemClicked&&api.itemClicked(0)&&!edit.resizeTransaction){
       const int beginStatus=tc::logic::storageBeginEdit(&handle);
       if(beginStatus==TC_COMPONENT_STORAGE_OK){
        edit.resizeTransaction=true;edit.resizeField=field.id;
        sendAction(field,TC_COMPONENT_EDITOR_ACTION_RESIZE_BEGIN,0,mouse.x,mouse.y,0.f,0.f);
       }else logger("Component editor: the resize grip of field \""+field.id+"\" on instance "+
                     hexValue(instance)+" could not open an edit transaction ("+
                     std::to_string(beginStatus)+")");
      }
      if(edit.resizeTransaction&&edit.resizeField==field.id&&api.itemActive&&api.itemActive()&&
         api.mouseDragging&&api.mouseDragging(0,0.f)){
       EditorApi::Vec2 delta{};if(api.mouseDragDelta)api.mouseDragDelta(&delta,0,0.f);
       sendAction(field,TC_COMPONENT_EDITOR_ACTION_RESIZE,0,mouse.x,mouse.y,delta.x,delta.y);
      }
      if(edit.resizeTransaction&&edit.resizeField==field.id&&api.itemDeactivated&&
         api.itemDeactivated()){
       sendAction(field,TC_COMPONENT_EDITOR_ACTION_RESIZE_END,0,mouse.x,mouse.y,0.f,0.f);
       tc::logic::storageCommitEdit(&handle);edit.resizeTransaction=false;edit.resizeField.clear();
       edit.valid=false;
      }
      api.setCursorScreen(after);
     }
    }else if(field.radio){
     /* A radio group takes the line under its label: a handful of named choices
        read better across the panel than stacked in a narrow column. */
     int64_t current=value.integer;
     for(size_t choice=0;choice<field.choices.size();++choice){
      if(choice)api.sameLine(0.f,-1.f);
      if(api.radioButton(field.choices[choice].c_str(),current==static_cast<int64_t>(choice))&&
         current!=static_cast<int64_t>(choice)){
       component_editor::Value next;
       next.integer=static_cast<int64_t>(choice);
       commit(field,next);
      }
     }
    }else{
      api.sameLine(field.inlineWithPrevious?pairFieldX:rowLeft,-1.f);
      /* A "full row" field gets the same capped width as any other: the flag is
         about the control taking its own line, not about it spanning the panel.
         Both fields of a pair get the same half, which is what makes the two
         controls on one line read as a pair. */
      const bool pairedFirst=index+1<fields.size()&&fields[index+1].inlineWithPrevious;
      api.setNextItemWidth(field.inlineWithPrevious||pairedFirst?pairFieldWidth:fieldWidth);
     if(field.readOnly){
      api.textDisabled(component_editor::display(field,value).c_str(),nullptr);
     }else{
      switch(field.kind){
       case TC_COMPONENT_FIELD_BOOL:{
        bool edited=value.boolean;
        /* The checkbox flips on the click frame and commits there: waiting for
           "deactivated after edit" would also work, but the click itself is the
           unambiguous moment, and a player who clicks and never releases still
           sees the value change. */
        if(api.checkbox("##v",&edited)&&edited!=value.boolean){
         component_editor::Value next;next.boolean=edited;commit(field,next);
        }
        break;
       }
       case TC_COMPONENT_FIELD_INT:{
        int edited=static_cast<int>(value.integer);
        api.inputInt("##v",&edited,0,0,0);
        if(api.deactivatedAfterEdit()&&edited!=value.integer){
         component_editor::Value next;next.integer=edited;commit(field,next);
        }
        break;
       }
       case TC_COMPONENT_FIELD_UINT:{
        if(field.hex){
         /* Hexadecimal text, so the whole 64-bit range is reachable without the
            signed decimal box's 31-bit ceiling.  Text that is not a number is
            refused: the configuration keeps its bytes and the box goes back to
            the value the instance really holds. */
         char* buffer=edit.texts[index].data();
         api.inputText("##v",buffer,edit.texts[index].size(),0,nullptr,nullptr);
         if(api.deactivatedAfterEdit()){
          uint64_t parsed=0;
          if(!component_editor::parseHex(buffer,&parsed)){
           logger("Component editor: field \""+field.id+"\" on instance "+
                  hexValue(instance)+" is not a hexadecimal number (\""+buffer+
                  "\"); the configuration keeps its old value");
           std::snprintf(buffer,edit.texts[index].size(),"%s",
                         component_editor::formatHex(field,value.integer).c_str());
          }else if(static_cast<uint64_t>(value.integer)!=parsed){
           component_editor::Value next;
           next.integer=static_cast<int64_t>(parsed);
           commit(field,next);
          }
         }
        }else{
         int edited=static_cast<int>(value.integer);
         api.inputInt("##v",&edited,0,0,0);
         if(api.deactivatedAfterEdit()&&edited!=value.integer){
          component_editor::Value next;next.integer=edited;commit(field,next);
         }
        }
        break;
       }
       case TC_COMPONENT_FIELD_ENUM:{
        const std::string current=component_editor::display(field,value);
         if(field.dropdown&&api.beginCombo&&api.endCombo&&api.selectable){
         if(api.beginCombo("##v",current.c_str(),0)){
           const bool freshOpen=!comboOpen;
           comboOpen=true;
           /* Inside the branch the current ImGui window *is* the popup, so its
              rectangle can be published for the trace. */
           /* Published whenever the popup's rectangle changes (not only on the
              slow trace frames): the popup is open for a couple of frames, and a
              reader that wants to click an entry needs its rectangle before the
              popup is gone.  The change check keeps the log bounded. */
           if(api.windowPos&&api.windowSize){
            EditorApi::Vec2 popupPos{},popupSize{};
            api.windowPos(&popupPos);api.windowSize(&popupSize);
            if(freshOpen||popupPos.x!=lastComboPopup[0]||popupPos.y!=lastComboPopup[1]||
               popupSize.x!=lastComboPopup[2]||popupSize.y!=lastComboPopup[3]){
             lastComboPopup[0]=popupPos.x;lastComboPopup[1]=popupPos.y;
             lastComboPopup[2]=popupSize.x;lastComboPopup[3]=popupSize.y;
             logger("Component editor: popup instance="+hexValue(instance)+" field=\""+field.id+
                    "\" rect="+std::to_string((int)popupPos.x)+","+
                    std::to_string((int)popupPos.y)+","+
                    std::to_string((int)(popupPos.x+popupSize.x))+","+
                    std::to_string((int)(popupPos.y+popupSize.y)));
            }
           }
           for(size_t choice=0;choice<field.choices.size();++choice)
           {
            const bool selected=value.integer==static_cast<int64_t>(choice);
            const bool clicked=api.selectable(field.choices[choice].c_str(),selected,0,
                                              EditorApi::Vec2{0.f,0.f});
            /* Publish the entry's rectangle (change-gated) so a playtest can
               click exactly this entry - the popup flips above the field in the
               bottom drawer and its entries are taller than the row. */
            if(choice<8&&api.itemRectMin&&api.itemRectMax){
             EditorApi::Vec2 low{},high{};
             api.itemRectMin(&low);api.itemRectMax(&high);
             if(freshOpen||low.x!=comboItem[choice][0]||low.y!=comboItem[choice][1]){
              comboItem[choice][0]=low.x;comboItem[choice][1]=low.y;
              comboItem[choice][2]=high.x;comboItem[choice][3]=high.y;
              logger("Component editor: popup item instance="+hexValue(instance)+" field=\""+
                     field.id+"\" index="+std::to_string(choice)+" rect="+
                     std::to_string((int)low.x)+","+std::to_string((int)low.y)+","+
                     std::to_string((int)high.x)+","+std::to_string((int)high.y));
             }
            }
            if(clicked&&!selected){
             component_editor::Value next;next.integer=static_cast<int64_t>(choice);commit(field,next);
            }
           }
           api.endCombo();
          }
          else comboOpen=false;
         }else if(api.button((current+"##v").c_str(),EditorApi::Vec2{0.f,0.f})){
         component_editor::Value next;
         next.integer=component_editor::nextChoice(field,value.integer);
         commit(field,next);
        }
        break;
       }
       case TC_COMPONENT_FIELD_TEXT:{
        char* buffer=edit.texts[index].data();
        api.inputText("##v",buffer,edit.texts[index].size(),0,nullptr,nullptr);
        if(api.deactivatedAfterEdit()&&value.text!=buffer){
         component_editor::Value next;next.text=buffer;commit(field,next);
        }
        break;
       }
       case TC_COMPONENT_FIELD_FLOAT:{
        float edited=static_cast<float>(value.real);
        api.inputFloat("##v",&edited,0.f,0.f,"%.6g",0);
        if(api.deactivatedAfterEdit()&&edited!=value.real){
         component_editor::Value next;next.real=edited;commit(field,next);
        }
        break;
       }
       case TC_COMPONENT_FIELD_FLOAT_WIDTH:{
        /* The width-aware decimal box: text in, pattern out.  Text that is not a
           number is refused (the box goes back to the value the instance really
           holds), which is the same contract the hexadecimal row next to it
           keeps - and the same one the Mod's own FP32 decimal field had before
           the rows moved into the host. */
        char* buffer=edit.texts[index].data();
        api.inputText("##v",buffer,edit.texts[index].size(),0,nullptr,nullptr);
        if(api.deactivatedAfterEdit()){
         uint64_t pattern=0;
         std::string note;
         if(!component_editor::parseFloatText(field,buffer,&pattern,&note)){
          logger("Component editor: field \""+field.id+"\" on instance "+
                 hexValue(instance)+" is not a decimal number (\""+buffer+
                 "\"); the configuration keeps its old value");
          std::snprintf(buffer,edit.texts[index].size(),"%s",
                        component_editor::formatFloatText(field,value.integer).c_str());
         }else{
          if(!note.empty())logger("Component editor: "+note);
          if(static_cast<uint64_t>(value.integer)!=pattern){
           component_editor::Value next;next.integer=static_cast<int64_t>(pattern);
           commit(field,next);
          }
         }
        }
        break;
       }
        case TC_COMPONENT_FIELD_COLOR_RGBA8:{
         const uint32_t packed=static_cast<uint32_t>(value.integer);
         /* V4's swatch strip: a palette is the same edit as the colour widget,
            drawn as the presets the type declared - one click instead of a drag
            to a remembered corner of the picker.  The strip comes first so the
            two shapes compose into one row. */
         if(field.colorPalette&&!field.swatches.empty()){
          const float gap=4.f;
          const float slots=static_cast<float>(field.swatches.size());
          float swatchSize=(fieldWidth-gap*(slots-1.f))/slots;
          const float swatchCap=ink*1.5f;
          if(swatchSize>swatchCap)swatchSize=swatchCap;
          if(swatchSize<6.f)swatchSize=6.f;
          if(field.id!=swatchField){
           swatchField=field.id;
           for(auto& rect:swatchRect){rect[0]=rect[1]=rect[2]=rect[3]=-1.f;}
          }
          for(size_t choice=0;choice<field.swatches.size();++choice){
           if(choice)api.sameLine(0.f,gap);
           const std::string id="##sw"+std::to_string(choice);
           if(api.invisibleButton)api.invisibleButton(id.c_str(),EditorApi::Vec2{swatchSize,swatchSize},0);
           EditorApi::Vec2 low{},high{};
           if(api.itemRectMin&&api.itemRectMax){api.itemRectMin(&low);api.itemRectMax(&high);}
           if(high.x>low.x&&high.y>low.y){
            if(api.windowDrawList){
             void* list=api.windowDrawList();
             renderRectFilled(list,{low.x,low.y},{high.x,high.y},field.swatches[choice],3.f,0);
             renderRect(list,{low.x,low.y},{high.x,high.y},
                        field.swatches[choice]==packed?0xffffffffu:0x50000000u,3.f,0,
                        field.swatches[choice]==packed?2.f:1.f);
            }
            /* Published (change-gated) so a playtest clicks the swatch the host
               drew: the strip's geometry follows the row width, which follows
               the window the player happened to size. */
            if(choice<TC_COMPONENT_EDITOR_MAX_PALETTE_SWATCHES&&
               (low.x!=swatchRect[choice][0]||low.y!=swatchRect[choice][1])){
             swatchRect[choice][0]=low.x;swatchRect[choice][1]=low.y;
             swatchRect[choice][2]=high.x;swatchRect[choice][3]=high.y;
             logger("Component editor: swatch instance="+hexValue(instance)+" field=\""+field.id+
                    "\" index="+std::to_string(choice)+" rect="+
                    std::to_string((int)low.x)+","+std::to_string((int)low.y)+","+
                    std::to_string((int)high.x)+","+std::to_string((int)high.y));
            }
           }
           if(api.itemHovered&&api.itemHovered(0)){
            const std::string literal=component_editor::formatColorLiteral(field.swatches[choice]);
            if(api.beginTooltip&&api.text&&api.endTooltip){
             api.beginTooltip();api.text(literal.c_str(),nullptr);api.endTooltip();
            }
            if(api.itemClicked&&api.itemClicked(0)&&field.swatches[choice]!=packed){
             component_editor::Value next;
             next.integer=static_cast<int64_t>(field.swatches[choice]);
             commit(field,next);
            }
           }
          }
         }else{
          float color[4]={float(packed&0xffu)/255.f,float((packed>>8)&0xffu)/255.f,
                          float((packed>>16)&0xffu)/255.f,float((packed>>24)&0xffu)/255.f};
          const bool changed=api.colorEdit4&&api.colorEdit4("##v",color,0);
          if(changed){
           /* A drag is one gesture, so it is one undo step: the storage
              transaction is opened on the first change and closed when the
              widget goes inactive.  A staged table writes nothing at all here -
              its whole batch is one write on Apply - so the transaction is
              skipped rather than opened and left hanging. */
           if(!staged&&!edit.colorTransaction&&
              tc::logic::storageBeginEdit(&handle)==TC_COMPONENT_STORAGE_OK){
            edit.colorTransaction=true;edit.colorField=field.id;
           }
           auto channel=[](float v){return static_cast<uint32_t>(std::lround(
               std::max(0.f,std::min(1.f,v))*255.f));};
           component_editor::Value next;
           next.integer=static_cast<int64_t>(channel(color[0])|(channel(color[1])<<8)|
                        (channel(color[2])<<16)|(channel(color[3])<<24));
           commit(field,next);
          }
          if(!staged&&edit.colorTransaction&&edit.colorField==field.id&&api.itemActive&&
             !api.itemActive()){
           tc::logic::storageCommitEdit(&handle);edit.colorTransaction=false;edit.colorField.clear();
          }
         }
         /* V4's mixed row: the exact four bytes, typed the way the log prints
            them.  Text the parser cannot read is refused and the box goes back
            to what the instance holds, the same contract the UINT hex row and
            the width-aware float rows keep. */
         if(field.colorHex){
          api.sameLine(0.f,8.f);
          const float hexWidth=std::min(fieldWidth,ink*9.5f);
          api.setNextItemWidth(hexWidth>60.f?hexWidth:60.f);
          char* buffer=edit.texts[index].data();
          api.inputText("##c",buffer,edit.texts[index].size(),0,nullptr,nullptr);
          if(api.deactivatedAfterEdit()){
           uint32_t parsed=0;
           if(!component_editor::parseColorHex(buffer,&parsed)){
            logger("Component editor: field \""+field.id+"\" on instance "+
                   hexValue(instance)+" is not a colour (\""+buffer+
                   "\"); the configuration keeps its old value");
            fillTextBuffer(field,index);
           }else if(parsed!=packed){
            component_editor::Value next;
            next.integer=static_cast<int64_t>(parsed);
            commit(field,next);
           }
          }
         }
         break;
        }
       default:break;
      }
     }
    }
     if(field.kind!=TC_COMPONENT_FIELD_CUSTOM&&field.action&&api.itemHovered&&
        api.itemHovered(0)&&api.mouseDoubleClicked&&api.mouseDoubleClicked(0)){
      EditorApi::Vec2 mouse{};if(api.mousePos)api.mousePos(&mouse);
      sendAction(field,TC_COMPONENT_EDITOR_ACTION_DOUBLE_CLICK,0,mouse.x,mouse.y,0.f,0.f);
     }
     if(field.kind!=TC_COMPONENT_FIELD_CUSTOM&&field.action&&!field.contextActionIds.empty()&&
        api.beginPopupContextItem&&api.selectable&&api.endPopup){
      if(api.beginPopupContextItem("##context",1)){
       const bool freshMenu=!menuOpen;menuOpen=true;
       for(size_t actionIndex=0;actionIndex<field.contextActionIds.size();++actionIndex){
        if(api.selectable(field.contextActionLabels[actionIndex].c_str(),false,0,
                          EditorApi::Vec2{0.f,0.f})){
         EditorApi::Vec2 mouse{};if(api.mousePos)api.mousePos(&mouse);
         sendAction(field,TC_COMPONENT_EDITOR_ACTION_MENU,field.contextActionIds[actionIndex],
                    mouse.x,mouse.y,0.f,0.f);
        }
        publishMenuEntry(freshMenu,actionIndex,field.id,field.contextActionIds[actionIndex]);
       }
       api.endPopup();
      }else menuOpen=false;
     }else menuOpen=false;
     if(index<edit.validation.size()&&!edit.validation[index].empty()){
      const int status=edit.validationStatus[index];
      if(field.validationIcon){
       /* V4: the message costs the row nothing - it is a small marker at the end
          of the control, with the text on hover and in the log.  Drawing it with
          the window's own draw list instead of an ImGui item is deliberate: the
          marker must not move the cursor, or the next row would start beside it
          instead of under it. */
       EditorApi::Vec2 low{},high{};
       if(field.kind==TC_COMPONENT_FIELD_CUSTOM&&customMax.x>customMin.x){
        low=customMin;high=customMax;
       }else if(api.itemRectMin&&api.itemRectMax){
        api.itemRectMin(&low);api.itemRectMax(&high);
       }
       if(high.x>low.x&&high.y>low.y&&api.windowDrawList){
        const float size=std::max(8.f,ink*0.95f);
        float x=high.x+6.f;
        const float limit=havePanel?panelRight-kEditorPanelMargin:1e9f;
        if(x+size>limit)x=std::max(low.x,limit-size-2.f);
        if(x<low.x)x=low.x;
        const float y=low.y+std::max(0.f,(high.y-low.y-size)*0.5f);
        void* list=api.windowDrawList();
        const uint32_t marker=status<0?0xff3030ffu:0xff20b0ffu;
        renderRectFilled(list,{x,y},{x+size,y+size},marker,2.f,0);
        renderRect(list,{x,y},{x+size,y+size},0x80000000u,2.f,0,1.f);
        if(validationIconField!=field.id||x!=validationIconRect[0]||y!=validationIconRect[1]){
         validationIconField=field.id;
         validationIconRect[0]=x;validationIconRect[1]=y;
         validationIconRect[2]=x+size;validationIconRect[3]=y+size;
         logger("Component editor: validation icon instance="+hexValue(instance)+
                " field=\""+field.id+"\" status="+std::to_string(status)+" rect="+
                std::to_string((int)x)+","+std::to_string((int)y)+","+
                std::to_string((int)(x+size))+","+std::to_string((int)(y+size)));
        }
        EditorApi::Vec2 mouse{};
        if(api.mousePos)api.mousePos(&mouse);
        if(mouse.x>=x&&mouse.x<=x+size&&mouse.y>=y&&mouse.y<=y+size){
         if(api.beginTooltip&&api.text&&api.endTooltip){
          api.beginTooltip();
          api.text(status<0?"Error":"Warning",nullptr);
          api.text(edit.validation[index].c_str(),nullptr);
          api.endTooltip();
         }
         if(validationTipField!=field.id+"|"+edit.validation[index]){
          validationTipField=field.id+"|"+edit.validation[index];
          logger("Component editor: validation tip instance="+hexValue(instance)+
                 " field=\""+field.id+"\" status="+std::to_string(status)+
                 " message=\""+edit.validation[index]+"\"");
         }
        }
       }
      }else{
       const std::string hint=(status<0?"Error: ":"Warning: ")+edit.validation[index];
       api.textDisabled(hint.c_str(),nullptr);
      }
     }
    /* The row rectangle a playtest clicks by; only when asked for, because it
       is one log line per row per second. */
    if(traceFrame){
     EditorApi::Vec2 low{},high{};
     if(field.kind==TC_COMPONENT_FIELD_CUSTOM&&customMax.x>customMin.x){
      low=customMin;high=customMax;
     }else{
      api.itemRectMin(&low);api.itemRectMax(&high);
     }
     std::string line="Component editor: row instance="+hexValue(instance)+" field=\""+field.id+
                      "\" rect="+std::to_string((int)low.x)+","+std::to_string((int)low.y)+","+
                      std::to_string((int)high.x)+","+std::to_string((int)high.y)+
                      " value=\""+component_editor::display(field,value)+"\""+
                      " active="+std::to_string(api.itemActive()?1:0)+
                      " focused="+std::to_string(api.itemFocused()?1:0);
     if(field.kind==TC_COMPONENT_FIELD_TEXT||field.kind==TC_COMPONENT_FIELD_FLOAT_WIDTH)
      line+=" buffer=\""+std::string(edit.texts[index].data())+"\"";
     logger(line);
    }
   api.popId();
  }
   /* V4: a table declared with TC_COMPONENT_EDITOR_TABLE_CONFIRM ends with the
      two buttons that decide what happens to its staged edits.  Apply is one
      configuration write, so however many fields were touched the batch is one
      undo step and one on_config_changed; Cancel re-reads the instance and
      throws the staged bytes away. */
   if(staged){
    int changed=0;
    for(size_t index=0;index<fields.size();++index){
     const component_editor::Field& field=fields[index];
     if(!field.bytes||static_cast<uint64_t>(field.offset)+field.bytes>edit.committed.size())
      continue;
     if(std::memcmp(edit.committed.data()+field.offset,edit.bytes.data()+field.offset,
                    field.bytes)!=0)
      ++changed;
    }
    api.separator();
    const bool applyPressed=api.button("Apply###tc-editor-apply",EditorApi::Vec2{0.f,0.f});
    EditorApi::Vec2 applyMin{},applyMax{};
    if(api.itemRectMin&&api.itemRectMax){api.itemRectMin(&applyMin);api.itemRectMax(&applyMax);}
    api.sameLine(0.f,-1.f);
    const bool cancelPressed=api.button("Cancel###tc-editor-cancel",EditorApi::Vec2{0.f,0.f});
    EditorApi::Vec2 cancelMin{},cancelMax{};
    if(api.itemRectMin&&api.itemRectMax){api.itemRectMin(&cancelMin);api.itemRectMax(&cancelMax);}
    if(applyMax.x>applyMin.x&&
       (applyMin.x!=dialogApplyRect[0]||applyMin.y!=dialogApplyRect[1]||
        cancelMin.x!=dialogCancelRect[0]||dialogPending!=changed)){
     dialogApplyRect[0]=applyMin.x;dialogApplyRect[1]=applyMin.y;
     dialogApplyRect[2]=applyMax.x;dialogApplyRect[3]=applyMax.y;
     dialogCancelRect[0]=cancelMin.x;dialogCancelRect[1]=cancelMin.y;
     dialogCancelRect[2]=cancelMax.x;dialogCancelRect[3]=cancelMax.y;
     dialogPending=changed;
     logger("Component editor: dialog instance="+hexValue(instance)+" apply="+
            std::to_string((int)applyMin.x)+","+std::to_string((int)applyMin.y)+","+
            std::to_string((int)applyMax.x)+","+std::to_string((int)applyMax.y)+
            " cancel="+std::to_string((int)cancelMin.x)+","+std::to_string((int)cancelMin.y)+
            ","+std::to_string((int)cancelMax.x)+","+std::to_string((int)cancelMax.y)+
            " pending="+std::to_string(changed));
    }
    if(applyPressed){
     std::vector<size_t> touched;
     for(size_t index=0;index<fields.size();++index){
      const component_editor::Field& field=fields[index];
      if(!field.bytes||static_cast<uint64_t>(field.offset)+field.bytes>edit.committed.size())
       continue;
      if(std::memcmp(edit.committed.data()+field.offset,edit.bytes.data()+field.offset,
                     field.bytes)!=0)
       touched.push_back(index);
     }
     if(touched.empty()){
      logger("Component editor: Apply on instance "+hexValue(instance)+
             " had nothing staged");
     }else{
      bool rejected=false;
      for(size_t index:touched){
       const component_editor::Field& field=fields[index];
       const int validation=runValidator(field,edit.bytes,index);
       if(validation>0){
        logger("Component editor: validator warned for field \""+field.id+"\" on instance "+
               hexValue(instance)+": "+edit.validation[index]);
        continue;
       }
       if(validation<0){
        if(edit.validation[index].empty())edit.validation[index]="value rejected";
        logger("Component editor: validator refused field \""+field.id+"\" on instance "+
               hexValue(instance)+": "+edit.validation[index]);
        rejected=true;
        break;
       }
      }
      if(rejected){
       logger("Component editor: the staged edits of instance "+hexValue(instance)+
              " were not applied");
      }else{
       const int status=tc::logic::storageWriteConfig(&handle,info.config_schema,
                                                      edit.bytes.data(),
                                                      static_cast<uint32_t>(edit.bytes.size()));
       if(status!=TC_COMPONENT_STORAGE_OK){
        logger("Component editor: the configuration write for instance "+hexValue(instance)+
               " was refused ("+std::to_string(status)+"); the staged edits are kept");
       }else{
        logger("Component editor: applied "+std::to_string(touched.size())+
               " staged field(s) on instance "+hexValue(instance)+" in one step");
        edit.valid=false;edit.pending=false;
       }
      }
     }
    }
    if(cancelPressed){
     int dropped=0;
     for(size_t index=0;index<fields.size();++index){
      const component_editor::Field& field=fields[index];
      if(!field.bytes||static_cast<uint64_t>(field.offset)+field.bytes>edit.committed.size())
       continue;
      if(std::memcmp(edit.committed.data()+field.offset,edit.bytes.data()+field.offset,
                     field.bytes)!=0)
       ++dropped;
     }
     uint32_t written=0;
     if(tc::logic::storageReadConfig(&handle,edit.bytes.data(),info.config_size,&written)==
        TC_COMPONENT_STORAGE_OK&&written==info.config_size){
      edit.committed=edit.bytes;edit.pending=false;edit.revision=info.config_revision;
      for(size_t index=0;index<fields.size()&&index<component_editor::kMaxFields;++index)
       fillTextBuffer(fields[index],index);
      for(auto& message:edit.validation)message.clear();
      for(auto& status:edit.validationStatus)status=0;
      logger("Component editor: cancelled "+std::to_string(dropped)+
             " staged field(s) on instance "+hexValue(instance));
     }else{
      logger("Component editor: the configuration of instance "+hexValue(instance)+
             " could not be re-read, so Cancel left the staged edits alone");
     }
    }
   }
   /* The window's own size, read while it is the current one. */
   {
    EditorApi::Vec2 origin{},size{};
    api.windowPos(&origin);api.windowSize(&size);
    lastEditorWindowHeight=size.y;
    lastEditorScrollMax=api.scrollMaxY?api.scrollMaxY():0.f;
    if(traceFrame)
     logger("Component editor: window instance="+hexValue(instance)+" pos="+
            std::to_string((int)origin.x)+","+std::to_string((int)origin.y)+" size="+
            std::to_string((int)size.x)+","+std::to_string((int)size.y)+" rows="+
            std::to_string(fields.size())+" scrollmax="+
            std::to_string((int)(api.scrollMaxY?api.scrollMaxY():0.f))+
            " panel="+std::to_string((int)panelLeft)+","+std::to_string((int)panelTop)+","+
            std::to_string((int)panelRight)+","+std::to_string((int)panelBottom)+
            " toggle="+std::to_string((int)panelToggleMin[0])+","+
            std::to_string((int)panelToggleMin[1])+","+
            std::to_string((int)panelToggleMax[0])+","+
            std::to_string((int)panelToggleMax[1]));
   }
   api.popId();
  }
  api.end();
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
 static ComponentFont* componentFont(Loaded* plugin,uint64_t handle){
  if(!plugin||!handle)return nullptr;
  for(auto& item:plugin->fonts)if(item.second.handle==handle)return &item.second;
  return nullptr;
 }
 static int component_render_text_font(void* context,uint64_t fontHandle,float x,float y,float size,
                                       uint32_t color,uint32_t style,const char* utf8){
  auto* draw=renderContext(context);
  if(!draw||!renderFinite(x)||!renderFinite(y)||!renderFinite(size)||!(size>0.f)||
     size>4096.f||!utf8||(style&~TC_COMPONENT_TEXT_KNOWN_FLAGS))
   return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  const size_t length=strnlen(utf8,4097);if(length>4096)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(!length)return TC_COMPONENT_RENDER_OK;
  void* font=nullptr;uint64_t renderer=0;ComponentFont* custom=nullptr;
  if(fontHandle){
   custom=componentFont(draw->plugin,fontHandle);if(!custom)return TC_COMPONENT_RENDER_ERR_FONT;
   if(!draw->owner->textures.resolve(draw->plugin,custom->texture,&renderer))
    return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
   font=custom->font;
   auto setTex=reinterpret_cast<void(*)(void*,uint64_t)>(
       GetProcAddress(draw->owner->engine,"ImFontAtlas_SetTexID"));
   if(setTex)setTex(custom->atlas,renderer);
   if(!draw->owner->renderPushTexture||!draw->owner->renderPopTexture)
    return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  }else if(style&TC_COMPONENT_TEXT_BOLD){
   if(!draw->owner->boldTextFont)draw->owner->resolveToolTextFont();
   font=draw->owner->boldTextFont;
  }
  if(!renderCommand(draw))return TC_COMPONENT_RENDER_ERR_CAPACITY;
  if(custom)draw->owner->renderPushTexture(draw->list,renderer);
  const auto paint=[&](float px,float py){
   draw->owner->sizedText(draw->list,{px,py},color,utf8,utf8+length,size,font);
   if(custom&&(style&TC_COMPONENT_TEXT_BOLD))
    draw->owner->sizedText(draw->list,{px+std::max(0.5f,size*0.025f),py},color,
                           utf8,utf8+length,size,font);
  };
  if(style&TC_COMPONENT_TEXT_ITALIC){
   RenderVec2 measured{size,size};
   if(font&&draw->owner->renderCalcTextSize)
    draw->owner->renderCalcTextSize(&measured,font,size,4096.f,0.f,utf8,utf8+length,nullptr);
   const float height=std::max(4.f,measured.y);
   const float reach=height*0.12f+2.f;
   for(float offset=0.f;offset<height;offset+=1.f){
    const float shift=0.22f*(height*0.5f-(offset+0.5f));
    draw->owner->renderPushClip(draw->list,{x-reach+shift,y+offset},
                                {x+measured.x+reach+shift,y+offset+1.f},true);
    paint(x+shift,y);draw->owner->renderPopClip(draw->list);
   }
  }else paint(x,y);
  if(custom)draw->owner->renderPopTexture(draw->list);
  return TC_COMPONENT_RENDER_OK;
 }
 static int component_render_measure_font(void* context,uint64_t fontHandle,float size,
                                          uint32_t style,const char* utf8,float* width,
                                          float* height){
  auto* draw=renderContext(context);
  if(!draw||!renderFinite(size)||!(size>0.f)||size>4096.f||!utf8||!width||!height||
     (style&~TC_COMPONENT_TEXT_KNOWN_FLAGS))return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  const size_t length=strnlen(utf8,4097);if(length>4096)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  void* font=nullptr;
  if(fontHandle){auto* custom=componentFont(draw->plugin,fontHandle);
   if(!custom)return TC_COMPONENT_RENDER_ERR_FONT;font=custom->font;
  }else if(style&TC_COMPONENT_TEXT_BOLD){
   if(!draw->owner->boldTextFont)draw->owner->resolveToolTextFont();font=draw->owner->boldTextFont;
  }
  if(!font){
   auto* bytes=static_cast<unsigned char*>(draw->list);unsigned char* shared=nullptr;
   std::memcpy(&shared,bytes+0x38,sizeof(shared));if(shared)std::memcpy(&font,shared+0x18,sizeof(font));
  }
  if(!font||!draw->owner->renderCalcTextSize)return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  RenderVec2 measured{};draw->owner->renderCalcTextSize(&measured,font,size,4096.f,0.f,
                                                        utf8,utf8+length,nullptr);
  if(!renderFinite(measured.x)||!renderFinite(measured.y))return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  if(style&TC_COMPONENT_TEXT_ITALIC)measured.x+=measured.y*0.22f;
  if(fontHandle&&(style&TC_COMPONENT_TEXT_BOLD))measured.x+=std::max(0.5f,size*0.025f);
  *width=measured.x;*height=measured.y;return TC_COMPONENT_RENDER_OK;
 }
 /* V3: one quad of a loader-owned image.  The handle is looked up in the calling
    Mod's own table, so another Mod's image is simply unknown here; the pixels,
    the GL object and its lifetime never leave the loader. */
 static int component_render_image(void* context,uint64_t image,
                                   const TCComponentRenderQuadV1* quad){
  auto* draw=renderContext(context);
  if(!draw||!quad)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  const float values[16]={quad->x0,quad->y0,quad->x1,quad->y1,
                          quad->x2,quad->y2,quad->x3,quad->y3,
                          quad->u0,quad->v0,quad->u1,quad->v1,
                          quad->u2,quad->v2,quad->u3,quad->v3};
  for(float value:values)if(!renderFinite(value))return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(!draw->plugin||!draw->owner->renderImageQuad)return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  const auto* entry=draw->plugin->images.find(image,
      draw->instance.instance_id?&draw->instance:nullptr);
  if(!entry)return TC_COMPONENT_RENDER_ERR_IMAGE;
  uint64_t renderer=0;
  if(!draw->owner->textures.resolve(draw->plugin,entry->texture,&renderer))
   return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  if(!renderCommand(draw))return TC_COMPONENT_RENDER_ERR_CAPACITY;
  draw->owner->renderImageQuad(draw->list,renderer,
                               {quad->x0,quad->y0},{quad->x1,quad->y1},
                               {quad->x2,quad->y2},{quad->x3,quad->y3},
                               {quad->u0,quad->v0},{quad->u1,quad->v1},
                               {quad->u2,quad->v2},{quad->u3,quad->v3},quad->tint);
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
 /* tc.component.render V7: load an image for this Mod's own draw callbacks.
    The loader reads the PNG once, decodes it with the same WIC path it uses for
    plugin textures, creates the GL object in its texture manager (owner = this
    Mod, so unload/reject frees it) and remembers only the name -> handle pair. */
 int loadComponentImage(Loaded& plugin,const char* key,const char* path,
                         TCComponentRenderImageV1* out,
                         const TCComponentInstanceHandle* scope=nullptr){
  if(!out||out->size<sizeof(*out)||!key||!path)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(!textures.onThread())return TC_COMPONENT_RENDER_ERR_THREAD;
  if(!plugin.accepting&&!plugin.active)return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
   if(scope){
    if(std::find(plugin.componentIds.begin(),plugin.componentIds.end(),scope->custom_id)==
       plugin.componentIds.end())return TC_COMPONENT_RENDER_ERR_OWNERSHIP;
    TCComponentStorageInfoV1 scopeInfo{};scopeInfo.size=sizeof(scopeInfo);
    if(tc::logic::storageInfo(scope,&scopeInfo)!=TC_COMPONENT_STORAGE_OK)
     return TC_COMPONENT_RENDER_ERR_IMAGE;
   }
  const std::string name(key);
  if(!component_images::validKey(name))return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  try{
   const fs::path file=fs::u8path(path);
   if(!file.is_absolute())return TC_COMPONENT_RENDER_ERR_ARGUMENT;
   const DWORD attributes=GetFileAttributesW(file.c_str());
   if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_DIRECTORY)||
      (attributes&FILE_ATTRIBUTE_REPARSE_POINT))
    return TC_COMPONENT_RENDER_ERR_IMAGE;
   std::error_code error;
   const auto length=fs::file_size(file,error);
   if(error||!length||length>TC_COMPONENT_RENDER_MAX_IMAGE_FILE_BYTES)
    return TC_COMPONENT_RENDER_ERR_IMAGE;
   const auto bytes=read(file);
   std::vector<unsigned char> pixels;
   uint32_t width=0,height=0;
   if(!ui_texture::decode(bytes.data(),bytes.size(),pixels,width,height))
    return TC_COMPONENT_RENDER_ERR_IMAGE;
   TCUiTexturePixels definition{sizeof(TCUiTexturePixels),width,height,0,pixels.data(),
                                pixels.size()};
   TCUiTexture texture{};
   texture.size=sizeof(texture);
   if(textures.create(&plugin,&definition,&texture)!=0)return TC_COMPONENT_RENDER_ERR_IMAGE;
   const auto retire=[this,&plugin](uint64_t handle){textures.release(&plugin,handle);};
   component_images::Image stored{};
    const int status=plugin.images.loadScoped(scope,name,texture.handle,texture.renderer_id,width,height,
                                       static_cast<uint64_t>(pixels.size()),
                                       &nextImageHandle,retire,&stored);
   if(status!=TC_COMPONENT_RENDER_OK){
    /* The table refused (quota, key, size): the texture created above has no
       owner left, so retire it rather than leak it. */
    textures.release(&plugin,texture.handle);
    return status;
   }
   out->image=stored.image;
   out->width=stored.width;
   out->height=stored.height;
   out->revision=stored.revision;
   logger("Component render: image \""+name+"\" for "+plugin.id+" is "+
          std::to_string(width)+"x"+std::to_string(height)+" (revision "+
          std::to_string(stored.revision)+")");
   return TC_COMPONENT_RENDER_OK;
  }catch(...){return TC_COMPONENT_RENDER_ERR_IMAGE;}
 }
 static int component_render_load_image_api(void* context,const char* key,const char* path,
                                            TCComponentRenderImageV1* out){
  auto& p=*static_cast<Loaded*>(context);
  return p.owner->loadComponentImage(p,key,path,out);
 }
 static int component_render_load_instance_image_api(
     void* context,const TCComponentInstanceHandle* instance,const char* key,const char* path,
     TCComponentRenderImageV1* out){
  if(!context||!instance)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  auto& p=*static_cast<Loaded*>(context);
  return p.owner->loadComponentImage(p,key,path,out,instance);
 }
 static int component_render_release_image_api(void* context,const char* key){
  auto& p=*static_cast<Loaded*>(context);
  return p.owner->releaseComponentImage(p,key);
 }
 int releaseComponentImage(Loaded& plugin,const char* key){
  if(!textures.onThread())return TC_COMPONENT_RENDER_ERR_THREAD;
  if(!plugin.accepting&&!plugin.active)return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  const auto retire=[this,&plugin](uint64_t handle){textures.release(&plugin,handle);};
  return plugin.images.clear(key?std::string(key):std::string(),retire);
 }
 static int component_render_release_instance_image_api(
     void* context,const TCComponentInstanceHandle* instance,const char* key){
  if(!context||!instance)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  auto& p=*static_cast<Loaded*>(context);auto& r=*p.owner;
  if(!r.textures.onThread())return TC_COMPONENT_RENDER_ERR_THREAD;
  if(std::find(p.componentIds.begin(),p.componentIds.end(),instance->custom_id)==p.componentIds.end())
   return TC_COMPONENT_RENDER_ERR_OWNERSHIP;
  const auto retire=[&r,&p](uint64_t handle){r.textures.release(&p,handle);};
  return p.images.clearScoped(instance,key?std::string(key):std::string(),retire);
 }
 void destroyComponentFont(Loaded& plugin,ComponentFont& font){
  if(font.texture)textures.release(&plugin,font.texture);
  auto destroy=reinterpret_cast<void(*)(void*)>(GetProcAddress(engine,"ImFontAtlas_destroy"));
  if(font.atlas&&destroy)destroy(font.atlas);
  font=ComponentFont{};
 }
 int loadComponentFont(Loaded& plugin,const char* key,const char* path,float nativeSize,
                       const TCComponentRenderFontRangeV1* ranges,uint32_t rangeCount,
                       TCComponentRenderFontV1* out){
  if(!out||out->size<sizeof(*out)||!key||!path||!renderCalcTextSize||
     !renderPushTexture||!renderPopTexture||!std::isfinite(nativeSize)||nativeSize<4.f||
     nativeSize>512.f||rangeCount>TC_COMPONENT_RENDER_MAX_FONT_RANGES||
     (rangeCount&&!ranges))return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  if(!textures.onThread())return TC_COMPONENT_RENDER_ERR_THREAD;
  if(!plugin.accepting&&!plugin.active)return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  const std::string name(key);if(!component_images::validKey(name))return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  auto atlasNew=reinterpret_cast<void*(*)()>(GetProcAddress(engine,"ImFontAtlas_ImFontAtlas"));
  auto atlasDestroy=reinterpret_cast<void(*)(void*)>(GetProcAddress(engine,"ImFontAtlas_destroy"));
  auto memAlloc=reinterpret_cast<void*(*)(size_t)>(GetProcAddress(engine,"igMemAlloc"));
  auto memFree=reinterpret_cast<void(*)(void*)>(GetProcAddress(engine,"igMemFree"));
  /* `ImWchar` is 32 bits in this build (see the glyph-range note below), so the
     range table is a `uint32_t` pair list terminated by zero. */
  auto addFont=reinterpret_cast<void*(*)(void*,void*,int,float,const void*,const uint32_t*)>(
      GetProcAddress(engine,"ImFontAtlas_AddFontFromMemoryTTF"));
  auto build=reinterpret_cast<bool(*)(void*)>(GetProcAddress(engine,"ImFontAtlas_Build"));
  auto texData=reinterpret_cast<void(*)(void*,unsigned char**,int*,int*,int*)>(
      GetProcAddress(engine,"ImFontAtlas_GetTexDataAsRGBA32"));
  auto setTex=reinterpret_cast<void(*)(void*,uint64_t)>(GetProcAddress(engine,"ImFontAtlas_SetTexID"));
  if(!atlasNew||!atlasDestroy||!memAlloc||!memFree||!addFont||!build||!texData||!setTex)
   return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
  try{
   const fs::path file=fs::u8path(path);if(!file.is_absolute())return TC_COMPONENT_RENDER_ERR_ARGUMENT;
   const DWORD attributes=GetFileAttributesW(file.c_str());
   if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_DIRECTORY)||
      (attributes&FILE_ATTRIBUTE_REPARSE_POINT))return TC_COMPONENT_RENDER_ERR_FONT;
   std::error_code error;const auto length=fs::file_size(file,error);
   if(error||!length||length>TC_COMPONENT_RENDER_MAX_FONT_FILE_BYTES||length>INT_MAX)
    return TC_COMPONENT_RENDER_ERR_FONT;
   const auto found=plugin.fonts.find(name);
   if(found==plugin.fonts.end()&&plugin.fonts.size()>=TC_COMPONENT_RENDER_MAX_FONTS)
    return TC_COMPONENT_RENDER_ERR_FONT;
   /* The glyph ranges this build's ImGui wants are 32-bit `ImWchar` pairs: its
      own default table is `20 00 00 00 / ff 00 00 00 / 0` (IMGUI_USE_WCHAR32),
      so a 16-bit pair table is read as a different - wrong - set and the face
      ends up with glyphs the drawn text never asks for.  Measured 2026-09-28:
      the text with such a face produced no vertices at all, while the atlas and
      its texture were fine. */
   std::vector<uint32_t> glyphs;
   if(rangeCount){glyphs.reserve(rangeCount*2+1);
    for(uint32_t index=0;index<rangeCount;++index){
     if(ranges[index].first<1||ranges[index].first>ranges[index].last||
        ranges[index].last>0xffffu)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
     glyphs.push_back(ranges[index].first);
     glyphs.push_back(ranges[index].last);
    }
    glyphs.push_back(0);
   }
   const auto bytes=read(file);void* atlas=atlasNew();if(!atlas)return TC_COMPONENT_RENDER_ERR_FONT;
   void* owned=memAlloc(bytes.size());
   if(!owned){atlasDestroy(atlas);return TC_COMPONENT_RENDER_ERR_FONT;}
   std::memcpy(owned,bytes.data(),bytes.size());
   void* face=addFont(atlas,owned,static_cast<int>(bytes.size()),nativeSize,nullptr,
                      glyphs.empty()?nullptr:glyphs.data());
   if(!face){memFree(owned);atlasDestroy(atlas);return TC_COMPONENT_RENDER_ERR_FONT;}
   if(!build(atlas)){atlasDestroy(atlas);return TC_COMPONENT_RENDER_ERR_FONT;}
   unsigned char* rgba=nullptr;int width=0,height=0,bpp=0;
   texData(atlas,&rgba,&width,&height,&bpp);
   uint64_t pixelBytes=0;
   if(!rgba||bpp!=4||width<=0||height<=0||
      !ui_texture::pixelBytes(static_cast<uint32_t>(width),static_cast<uint32_t>(height),pixelBytes)){
    atlasDestroy(atlas);return TC_COMPONENT_RENDER_ERR_FONT;
   }
   TCUiTexturePixels pixels{sizeof(TCUiTexturePixels),static_cast<uint32_t>(width),
                            static_cast<uint32_t>(height),0,rgba,pixelBytes};
   TCUiTexture texture{};texture.size=sizeof(texture);
   if(textures.create(&plugin,&pixels,&texture)!=0){atlasDestroy(atlas);return TC_COMPONENT_RENDER_ERR_FONT;}
   setTex(atlas,texture.renderer_id);
   ComponentFont replacement;replacement.texture=texture.handle;replacement.nativeSize=nativeSize;
   replacement.atlas=atlas;replacement.font=face;
   if(found==plugin.fonts.end()){replacement.handle=nextFontHandle++;plugin.fonts.emplace(name,replacement);}
   else {replacement.handle=found->second.handle;replacement.revision=found->second.revision+1;
    ComponentFont old=found->second;found->second=replacement;destroyComponentFont(plugin,old);
   }
   const ComponentFont& stored=plugin.fonts.find(name)->second;
   out->font=stored.handle;out->native_size=stored.nativeSize;out->revision=stored.revision;
   logger("Component render: font \""+name+"\" for "+plugin.id+" loaded (revision "+
          std::to_string(stored.revision)+")");
   return TC_COMPONENT_RENDER_OK;
  }catch(...){return TC_COMPONENT_RENDER_ERR_FONT;}
 }
 static int component_render_load_font_api(void* context,const char* key,const char* path,
                                           float nativeSize,
                                           const TCComponentRenderFontRangeV1* ranges,
                                           uint32_t rangeCount,TCComponentRenderFontV1* out){
  if(!context)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  auto& p=*static_cast<Loaded*>(context);
  return p.owner->loadComponentFont(p,key,path,nativeSize,ranges,rangeCount,out);
 }
 static int component_render_release_font_api(void* context,const char* key){
  if(!context)return TC_COMPONENT_RENDER_ERR_ARGUMENT;auto& p=*static_cast<Loaded*>(context);
  auto& r=*p.owner;if(!r.textures.onThread())return TC_COMPONENT_RENDER_ERR_THREAD;
  if(!key){for(auto& item:p.fonts)r.destroyComponentFont(p,item.second);p.fonts.clear();return TC_COMPONENT_RENDER_OK;}
  auto found=p.fonts.find(key);if(found==p.fonts.end())return TC_COMPONENT_RENDER_ERR_FONT;
  r.destroyComponentFont(p,found->second);p.fonts.erase(found);return TC_COMPONENT_RENDER_OK;
 }
 static int component_render_set_device_callback_api(void* context,
     TCComponentRenderDeviceCallbackV1 callback,void* user){
  if(!context)return TC_COMPONENT_RENDER_ERR_ARGUMENT;auto& p=*static_cast<Loaded*>(context);
  auto& r=*p.owner;if(!r.gameThreadId||GetCurrentThreadId()!=r.gameThreadId)
   return TC_COMPONENT_RENDER_ERR_THREAD;
  p.deviceCallback=callback;p.deviceUser=user;p.deviceGeneration=r.textures.generation();
  return TC_COMPONENT_RENDER_OK;
 }
 static int component_render_device_generation_api(void* context,uint64_t* generation){
  if(!context||!generation)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
  auto& p=*static_cast<Loaded*>(context);*generation=p.owner->textures.generation();
  return TC_COMPONENT_RENDER_OK;
 }
 int queryComponentRenderService(Loaded& p,uint32_t version,void* out,uint32_t outSize){
  if(!out)return TC_SERVICE_ERR_ARGUMENT;
  if(!renderWorldToScreen||!renderGetIo||!renderGetMainViewport||!renderGetBackgroundDrawList||
     !renderLine||!renderRect||!renderRectFilled||!renderCircle||!renderCircleFilled||!renderText||
     !renderPushClip||!renderPopClip)return TC_SERVICE_ERR_UNAVAILABLE;
   if(version==TC_COMPONENT_RENDER_API_VERSION_8){
    if(!renderImageQuad||!renderCalcTextSize||!renderPushTexture||!renderPopTexture||
       !redrawComponentOriginal||!redrawSelectionOriginal||!igButtonOriginal||
       !createTextureUnsafeOriginal||!redrawClipboardComponentOriginal||
       !updateStateClipboardOriginal)return TC_SERVICE_ERR_UNAVAILABLE;
    if(outSize<sizeof(TCComponentRenderApiV8))return TC_SERVICE_ERR_SIZE;
    *static_cast<TCComponentRenderApiV8*>(out)=TCComponentRenderApiV8{
        sizeof(TCComponentRenderApiV8),TC_COMPONENT_RENDER_API_VERSION_8,&p,
        &component_render_set_callback_api,&component_render_set_default_drawing_api,
        &component_render_set_selection_hint_api,&component_render_set_foundry_button_api,
        &component_render_set_picture_api,&component_render_set_placement_preview_api,
        &component_render_load_image_api,&component_render_release_image_api,
        &component_render_load_font_api,&component_render_release_font_api,
        &component_render_load_instance_image_api,&component_render_release_instance_image_api,
        &component_render_set_device_callback_api,&component_render_device_generation_api};
    return TC_SERVICE_OK;
   }
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
  if(version==TC_COMPONENT_RENDER_API_VERSION_7){
   /* The image path needs the ImGui image entry point on top of everything V6
      already required; without it there is nothing to draw the handle with. */
   if(!renderImageQuad||!redrawComponentOriginal||!redrawSelectionOriginal||
      !igButtonOriginal||!createTextureUnsafeOriginal||
      !redrawClipboardComponentOriginal||!updateStateClipboardOriginal)
    return TC_SERVICE_ERR_UNAVAILABLE;
   if(outSize<sizeof(TCComponentRenderApiV7))return TC_SERVICE_ERR_SIZE;
   *static_cast<TCComponentRenderApiV7*>(out)=TCComponentRenderApiV7{
       sizeof(TCComponentRenderApiV7),TC_COMPONENT_RENDER_API_VERSION_7,&p,
       &component_render_set_callback_api,&component_render_set_default_drawing_api,
       &component_render_set_selection_hint_api,&component_render_set_foundry_button_api,
       &component_render_set_picture_api,&component_render_set_placement_preview_api,
       &component_render_load_image_api,&component_render_release_image_api};
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
  /* `plugin` here is the Mod whose callback is about to run: the image lookup in
     the V3 primitive resolves in that Mod's own table. */
  RenderDrawContext drawContext{this,list,0,true,&plugin,{}};
  if(instanceId)tc::logic::instanceHandleOf(customId,instanceId,&drawContext.instance);
  /* The newest draw table this build can offer: its V1 prefix is what every
     existing Mod reads, the appended sized-text pair matches the board's own
     text, and the image quad needs the ImGui entry point to be resolvable (a
     build without it keeps handing out V2, so a Mod asking for >= 3 sees the
     version it really gets).  A Mod checks draw->version/size, see
     sdk/tc_component_render.h. */
  const TCComponentRenderDrawV4 drawStorageV4{
      sizeof(TCComponentRenderDrawV4),TC_COMPONENT_RENDER_DRAW_VERSION_4,&drawContext,
      &component_render_line,&component_render_rect,&component_render_rect_filled,
      &component_render_circle,&component_render_circle_filled,&component_render_text,
      &component_render_text_sized,&component_render_measure_text,
      renderImageQuad?&component_render_image:nullptr,
      &component_render_text_font,&component_render_measure_font};
  const TCComponentRenderDrawV3 drawStorageV3{
      sizeof(TCComponentRenderDrawV3),TC_COMPONENT_RENDER_DRAW_VERSION_3,&drawContext,
      &component_render_line,&component_render_rect,&component_render_rect_filled,
      &component_render_circle,&component_render_circle_filled,&component_render_text,
      &component_render_text_sized,&component_render_measure_text,
      renderImageQuad?&component_render_image:nullptr};
  const TCComponentRenderDrawV2 drawStorageV2{
      sizeof(TCComponentRenderDrawV2),TC_COMPONENT_RENDER_DRAW_VERSION_2,&drawContext,
      &component_render_line,&component_render_rect,&component_render_rect_filled,
      &component_render_circle,&component_render_circle_filled,&component_render_text,
      &component_render_text_sized,&component_render_measure_text};
  const bool haveV4=renderImageQuad&&renderCalcTextSize&&renderPushTexture&&renderPopTexture;
  const void* drawStorage=haveV4?static_cast<const void*>(&drawStorageV4):
      (renderImageQuad?static_cast<const void*>(&drawStorageV3):static_cast<const void*>(&drawStorageV2));
  const TCComponentRenderDrawV1* drawApi=
      static_cast<const TCComponentRenderDrawV1*>(drawStorage);
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
  bool anyRender=false;for(auto& plugin:loaded)if(plugin->active)for(auto& item:plugin->renderers)if(item.draw&&!item.failed){anyRender=true;break;}
  if(!anyRender&&!componentInputs.hasHandlers())return;
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
  std::vector<component_input::Target> inputTargets;
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
    TCComponentInstanceHandle inputHandle{};
    if(tc::logic::instanceHandleOf(info.custom_prototype_id,info.id,&inputHandle)){
     component_input::Target target{};target.handle=inputHandle;
     target.originX=centre.x;target.originY=centre.y;
     target.axisXX=axisX.x;target.axisXY=axisX.y;target.axisYX=axisY.x;target.axisYY=axisY.y;
     target.boardX=static_cast<float>(info.x);target.boardY=static_cast<float>(info.y);
     switch(info.rotation&3u){
     case 1:target.boardAxisXX=0;target.boardAxisXY=1;target.boardAxisYX=-1;target.boardAxisYY=0;break;
     case 2:target.boardAxisXX=-1;target.boardAxisXY=0;target.boardAxisYX=0;target.boardAxisYY=-1;break;
     case 3:target.boardAxisXX=0;target.boardAxisXY=-1;target.boardAxisYX=1;target.boardAxisYY=0;break;
     default:break;
     }
     target.order=index;inputTargets.push_back(target);
    }
    std::vector<uint8_t> config=tc::logic::configOfInstance(info.custom_prototype_id,info.id);
    invokeComponentRenderer(*plugin,*renderer,component,info.custom_prototype_id,
                            info.id,info.rotation,centre,axisX,axisY,clipMin,clipMax,
                            list,config.empty()?nullptr:config.data(),
                            static_cast<uint32_t>(config.size()),
                            tc::logic::configSchemaOfInstance(info.custom_prototype_id,info.id));
   }
  }
  componentInputs.setTargets(std::move(inputTargets));
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
  if(command.type==TC_COMMAND_BOARD_DELETE_COMPONENT){
   /* The undo-aware entry, not the low-level tombstone: `try_delete` copies what
      it removes and pushes the change list onto the game's undo stack, so one
      BOARD_UNDO brings the component (and its wires) back. */
   if(!tryDelete||!sequenceNew||!sequenceDestroy)return TC_COMMAND_ERR_UNAVAILABLE;
   if(!command.custom_prototype_id||!command.argument)return TC_COMMAND_ERR_ARGUMENT;
   int64_t index=-1;
   {
    board_objects::Arrays arrays{};
    if(!board_objects::readArrays(model,arrays))return TC_COMMAND_ERR_STATE;
    for(uint64_t i=0;i<arrays.components;++i){
     const unsigned char* record=arrays.componentData+board_objects::kRecordHeader+
                                 i*board_objects::kComponentStride;
     if(record[0]!=board_objects::kCustomComponentKind)continue;
     if(board_objects::readU64(record,8)!=static_cast<uint64_t>(command.argument))continue;
     if(board_objects::readU64(record,0x188)!=command.custom_prototype_id)continue;
     index=static_cast<int64_t>(i);
     break;
    }
   }
   if(index<0)return TC_COMMAND_ERR_NOT_FOUND;
   /* Two Nim sequences built with the game's own allocator: the one component to
      delete, and an empty wire list (the wrapper removes the wires that hang off
      the component itself).  A sequence variable is a two-word header - count
      then payload pointer - and the game's payload carries an 8-byte prefix, so
      the elements live at payload + 8 (verified against the game's own
      `newSeq` instantiation and its `add` helper, both of which write element i
      at data + i*8 + 8). */
   unsigned char indices[16]{},wires[16]{};
   bool removed=false;
   try{
    sequenceNew(indices,1);
    sequenceNew(wires,0);
    unsigned char* payload=nullptr;
    std::memcpy(&payload,indices+8,sizeof(payload));
    if(!payload)return TC_COMMAND_ERR_UNAVAILABLE;
    std::memcpy(payload+8,&index,sizeof(index));
    removed=tryDelete(const_cast<void*>(model),indices,wires);
   }catch(...){
    sequenceDestroy(indices);
    sequenceDestroy(wires);
    return TC_COMMAND_ERR_EXECUTION;
   }
   sequenceDestroy(indices);
   sequenceDestroy(wires);
   return removed?TC_COMMAND_OK:TC_COMMAND_ERR_STATE;
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
 static int register_component_api(void* c,const TCNativeComponentDefinition* d){auto& p=*(Loaded*)c;if(!p.accepting)return -1;try {std::vector<uint8_t> encoded;const bool adopt=p.owner->foundryEntryCarriesId(d&&d->name?d->name:"",d?d->custom_id:0);int result=registerNativeComponent(&p.host,d,&encoded,adopt);if(!result){p.logicIds.push_back(d->custom_id);p.componentIds.push_back(d->custom_id);p.owner->noteComponentType(p,d->custom_id,d->name,-1);provideFoundrySchematicBytes(*p.owner,p.host,d->name?d->name:"",d->custom_id,encoded);}else log_api(c,("Component registration rejected: "+std::to_string(result)).c_str());return result;}catch(const std::exception& e){log_api(c,e.what());return -4;}catch(...){return -4;}}
 static int register_ui_page_api(void* c,const TCUiPageDefinition* d){auto& p=*(Loaded*)c;if(!p.accepting)return -1;if(!d||d->size<sizeof(TCUiPageDefinition)||!d->page_id||!d->page_id[0]||!d->draw)return -2;std::string id=d->page_id;if(id.size()>=64||id.find("###")!=std::string::npos)return -2;if(p.pages.size()>=kMaxPagesPerPlugin)return -4;for(auto& page:p.pages)if(page.id==id)return -3;p.pages.push_back(UiPage{id,d->title?d->title:"",d->draw,d->user,false});log_api(c,("UI page registered: "+id).c_str());return 0;}
  void reject(Loaded& p){componentInputs.clearOwner(&p);componentChannels.clearOwner(&p);logic::finish(p.logicIds,false);component_registry::dropMod(p.id);p.editorFields.clear();p.settings.clear();p.settingChanged=nullptr;p.settingUser=nullptr;for(auto it=instanceFootprints.begin();it!=instanceFootprints.end();)if(it->second.owner==&p)it=instanceFootprints.erase(it);else ++it;if(!p.componentIds.empty()){TCGameModel game;if(game.load(&p.host))for(auto id:p.componentIds)game.removeCustomPrototype(id);p.componentIds.clear();}for(auto target:p.hooks){MH_DisableHook(target);MH_RemoveHook(target);ownedHooks.erase(target);}p.hooks.clear();dropHookLinks(p.id);dropEventListeners(p.id);dropLifecycleListeners(p);dropCommands(p);dropTransactions(p);dropComposeSessions(p);if(p.plugin.on_unload){try{p.plugin.on_unload(p.plugin.user);}catch(...) {}}for(auto& font:p.fonts)destroyComponentFont(p,font.second);p.fonts.clear();p.deviceCallback=nullptr;p.deviceUser=nullptr;/* The image table keeps only names and handles: the texture manager is about to retire every GL object this Mod owns, so nothing is released twice here. */p.images.forget();textures.reject(&p);p.plugin={};p.accepting=false;p.active=false;/* Keep rejected DLL mapped: it may have static destructors/threads. */}


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
 void drawPageContainer(const std::string& ownerId,UiPage& page,const TCFrame& frame){
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
   const float headerScale=(1.25f*scale)/1.6666666f;
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
   /* One fixed-height row: putting the title above the button made both of
      them exceed pageHeaderHeight() at large UI scales, so plugin content was
      drawn through the Back button.  Keep the control at the documented
      top-left hit box and place the human page title beside it. */
   if(api.button("< 返回###TCModPageBack",PageApi::Vec2{110.f*scale,40.f*scale}))closePage();
   api.setCursorPos(PageApi::Vec2{126.f*scale,8.f*scale});
   api.text((page.title.empty()?ownerId:page.title).c_str(),nullptr);
   api.setCursorPos(PageApi::Vec2{0.f,52.f*scale});
   api.separator();
   if(withFont)api.setWindowFontScale(1.f);
   api.setCursorPos(PageApi::Vec2{0.f,pageHeaderHeight()});
    api.pushId(ownerId.c_str());
   api.pushId(page.id.c_str());
   const float contentHeight=size.y>pageHeaderHeight()?size.y-pageHeaderHeight():0.f;
   /* The page content gets the same clipped, scrollable region as a board
      panel: a page that is taller than the window scrolls instead of drawing
      over the game's UI, and content that does not fit cannot be clicked
      where it is not visible. */
   bool hovered=false;
   PageApi::Vec2 region{0.f,pageHeaderHeight()};
    const std::string who=ownerId+"/"+page.id;
   const bool ok=drawContentRegion(api,PageApi::Vec2{size.x,contentHeight},scale,"###content",
                                    who,page.draw,page.user,frame,hovered,region);
   if(!ok){
    page.draw=nullptr;page.failed=true;
     statuses[ownerId]="UI page disabled after an exception";
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
 struct ComponentInputApi {
  using Vec2=SlotApi::Vec2;
  int (*getFrameCount)()=nullptr;
  void (*mousePos)(Vec2*)=nullptr;
  bool (*mouseDown)(int)=nullptr;
  bool (*mouseClicked)(int,bool)=nullptr;
  bool (*mouseReleased)(int)=nullptr;
  bool (*mouseDoubleClicked)(int)=nullptr;
  bool (*keyDown)(int)=nullptr;
  bool (*keyPressed)(int,bool)=nullptr;
  bool available() const{return getFrameCount&&mousePos&&mouseDown&&mouseClicked&&
                                mouseReleased&&mouseDoubleClicked&&keyDown&&keyPressed;}
 };
 static const ComponentInputApi& componentInputApi(){
  static ComponentInputApi value;static bool bound=false;
  if(!bound){bound=true;HMODULE module=GetModuleHandleW(L"tc_game_engine.dll");
   if(!module)module=GetModuleHandleW(nullptr);
   value.getFrameCount=(decltype(value.getFrameCount))proc(module,"igGetFrameCount");
   value.mousePos=(decltype(value.mousePos))proc(module,"igGetMousePos");
   value.mouseDown=(decltype(value.mouseDown))proc(module,"igIsMouseDown_Nil");
   value.mouseClicked=(decltype(value.mouseClicked))proc(module,"igIsMouseClicked_Bool");
   value.mouseReleased=(decltype(value.mouseReleased))proc(module,"igIsMouseReleased_Nil");
   value.mouseDoubleClicked=(decltype(value.mouseDoubleClicked))proc(module,"igIsMouseDoubleClicked_Nil");
   value.keyDown=(decltype(value.keyDown))proc(module,"igIsKeyDown_Nil");
   value.keyPressed=(decltype(value.keyPressed))proc(module,"igIsKeyPressed_Bool");
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
 void componentMouseWheel(float x,float y){componentWheelX+=x;componentWheelY+=y;}
 bool componentTextUtf8(const char* text){
  if(!text||!*text)return false;
  return componentInputs.text(TC_COMPONENT_INPUT_TEXT_COMMIT,text,
                              static_cast<uint32_t>(std::min<size_t>(std::strlen(text),63)));
 }
 bool componentTextCodepoint(uint32_t codepoint){
  char utf8[5]{};uint32_t count=0;
  if(codepoint<=0x7f){utf8[count++]=static_cast<char>(codepoint);}
  else if(codepoint<=0x7ff){utf8[count++]=static_cast<char>(0xc0|(codepoint>>6));utf8[count++]=static_cast<char>(0x80|(codepoint&0x3f));}
  else if(codepoint<=0xffff){utf8[count++]=static_cast<char>(0xe0|(codepoint>>12));utf8[count++]=static_cast<char>(0x80|((codepoint>>6)&0x3f));utf8[count++]=static_cast<char>(0x80|(codepoint&0x3f));}
  else if(codepoint<=0x10ffff){utf8[count++]=static_cast<char>(0xf0|(codepoint>>18));utf8[count++]=static_cast<char>(0x80|((codepoint>>12)&0x3f));utf8[count++]=static_cast<char>(0x80|((codepoint>>6)&0x3f));utf8[count++]=static_cast<char>(0x80|(codepoint&0x3f));}
  return count&&componentInputs.text(TC_COMPONENT_INPUT_TEXT_COMMIT,utf8,count,codepoint);
 }
 bool componentIme(uint32_t kind,const char* text){
  const char* value=text?text:"";
  return componentInputs.text(kind,value,
      static_cast<uint32_t>(std::min<size_t>(std::strlen(value),63)));
 }
 bool componentInputFrame(){
  if(!componentInputs.hasHandlers())return false;
  const ComponentInputApi& api=componentInputApi();if(!api.available())return false;
  const int frame=api.getFrameCount();if(frame==lastComponentInputFrame)return componentInputs.keyboardFocused();
  lastComponentInputFrame=frame;
  ComponentInputApi::Vec2 mouse{};api.mousePos(&mouse);
  uint32_t modifiers=0;
  if(api.keyDown(524)||api.keyDown(528))modifiers|=TC_COMPONENT_INPUT_MOD_CTRL;
  if(api.keyDown(525)||api.keyDown(529))modifiers|=TC_COMPONENT_INPUT_MOD_SHIFT;
  if(api.keyDown(526)||api.keyDown(530))modifiers|=TC_COMPONENT_INPUT_MOD_ALT;
  if(api.keyDown(527)||api.keyDown(531))modifiers|=TC_COMPONENT_INPUT_MOD_SUPER;
  component_input::PointerSample sample{};sample.screenX=mouse.x;sample.screenY=mouse.y;
  sample.down=api.mouseDown(0);sample.pressed=api.mouseClicked(0,false);
  sample.released=api.mouseReleased(0);sample.doubleClicked=api.mouseDoubleClicked(0);
  sample.wheelX=componentWheelX;sample.wheelY=componentWheelY;sample.modifiers=modifiers;
  componentWheelX=componentWheelY=0.f;
  bool consumed=componentInputs.pointer(sample);
  std::set<uint32_t> now;
  for(uint32_t key=512;key<=583;++key){
   if(!api.keyDown(static_cast<int>(key)))continue;
   now.insert(key);const bool wasDown=componentKeysDown.count(key)!=0;
   if(!wasDown||api.keyPressed(static_cast<int>(key),true))
    consumed|=componentInputs.keyboard(TC_COMPONENT_INPUT_KEY_DOWN,key,0,modifiers,wasDown);
  }
  for(uint32_t key:componentKeysDown)if(!now.count(key))
   consumed|=componentInputs.keyboard(TC_COMPONENT_INPUT_KEY_UP,key,0,modifiers,false);
  componentKeysDown=std::move(now);
  return consumed||componentInputs.keyboardFocused();
 }
 bool hasComponentInputs() const{return componentInputs.hasHandlers();}
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
 NativeRuntime(Core& c,HMODULE e,fs::path saves,std::function<void(const std::string&)> log):core(c),engine(e),saveRoot(std::move(saves)),modStorage(c.root,c.dir,saveRoot),settingsPage{"settings","Mod settings",&draw_settings_page_api,this,false},types(c.dir/L"registry.json"),logger(std::move(log)),services(gameHandles){services.bindBoardSnapshot(this,&NativeRuntime::captureBoardSnapshotService);services.bindBoardObjects(&NativeRuntime::captureBoardObjectsService);services.bindBoardReaders(&NativeRuntime::readComponentService,&NativeRuntime::readWireService);services.bindBoardPinReader(&NativeRuntime::readPinsService);services.bindBoardWireEndsReader(&NativeRuntime::readWireEndsService);services.bindSimulation(this,&NativeRuntime::simulationStateService,&NativeRuntime::simulationValueService);services.bindSimulationControl(this,&NativeRuntime::simulationCycleService,&NativeRuntime::simulationStateSizeService,&NativeRuntime::simulationSnapshotService,&NativeRuntime::simulationRunToService,&NativeRuntime::simulationRunForService,&NativeRuntime::simulationPauseService,&NativeRuntime::simulationResetService,&NativeRuntime::simulationStepService,&NativeRuntime::simulationSliceService,&NativeRuntime::simulationControlService);services.bindSimChannels(this,&NativeRuntime::channelFromWireService,&NativeRuntime::channelResolveService);services.bindSimCapture(this,&NativeRuntime::captureConfigureService,&NativeRuntime::captureStartService,&NativeRuntime::captureStopService,&NativeRuntime::captureReadService,&NativeRuntime::captureStatusService);services.bindIoValue(this,&NativeRuntime::ioEvaluateService,&NativeRuntime::ioFormatService,&NativeRuntime::ioReadInputService,&NativeRuntime::ioWriteInputService,&NativeRuntime::ioFlipInputService,&NativeRuntime::ioInputWidthService,&NativeRuntime::ioWriteConstantService,&NativeRuntime::ioWriteConstantSlotService);activeInstance=this;}
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
  bool openPage(const std::string& owner,const std::string& page){UiPage* found=nullptr;if(page.empty()||owner.empty())return false;if(owner=="tc.modloader"&&page==settingsPage.id&&!settingsPage.failed){for(auto& p:loaded)if(p->active&&!p->settings.empty())found=&settingsPage;}else for(auto& p:loaded){if(!p->active||p->id!=owner)continue;for(auto& candidate:p->pages)if(candidate.id==page)found=&candidate;}if(!found)return false;openPageOwner=owner;openPageId=page;lastUiFrame=-1;return true;}
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
   for(auto& p:loaded)if(p->active&&!p->settings.empty()){result.push_back(PageEntry{"tc.modloader",settingsPage.id.c_str(),settingsPage.title.c_str(),settingsPage.failed});break;}
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
  advanceTextures(number);
  if(number==lastUiFrame)return;
  lastUiFrame=number;
  UiPage* page=nullptr;
  if(openPageOwner=="tc.modloader"&&openPageId==settingsPage.id)page=&settingsPage;
  else for(auto& p:loaded){if(!p->active||p->id!=openPageOwner)continue;for(auto& candidate:p->pages)if(candidate.id==openPageId)page=&candidate;}
  if(!page){closePage();return;}
  if(!page->draw){page->failed=true;return;}
  TCFrame frame{sizeof(TCFrame),number,engineTime()};
  {static int calls=0;++calls;if(calls<=3||calls%600==0)logger("UI page frame "+std::to_string(calls)+": "+openPageOwner+"/"+openPageId+" drawn="+std::to_string(page->draw!=nullptr)+" frame="+std::to_string(number));}
  try{drawPageContainer(openPageOwner,*page,frame);}
  catch(const std::exception& e){page->draw=nullptr;page->failed=true;logger("UI page failed: "+openPageOwner+"/"+page->id+": "+e.what());statuses[openPageOwner]="UI 页面异常：已停用该页";}
  catch(...){page->draw=nullptr;page->failed=true;logger("UI page failed: "+openPageOwner+"/"+page->id);statuses[openPageOwner]="UI 页面异常：已停用该页";}
 }
 void advanceTextures(int frame){
  textures.advance(frame);const uint64_t generation=textures.generation();
  for(auto& plugin:loaded){if(!plugin)continue;
   const auto retire=[this,&plugin](uint64_t texture){textures.release(plugin.get(),texture);};
   plugin->images.prune([](uint64_t customId,uint64_t instanceId,uint64_t instanceGeneration){
    TCComponentInstanceHandle handle{};handle.size=sizeof(handle);handle.version=TC_COMPONENT_INSTANCES_API_VERSION_1;
    handle.custom_id=customId;handle.instance_id=instanceId;handle.generation=instanceGeneration;
    TCComponentStorageInfoV1 info{};info.size=sizeof(info);
    return tc::logic::storageInfo(&handle,&info)==TC_COMPONENT_STORAGE_OK;
   },retire);
   if(plugin->deviceCallback&&plugin->deviceGeneration&&plugin->deviceGeneration!=generation){
    TCComponentRenderDeviceEventV1 event{};event.size=sizeof(event);
    event.version=TC_COMPONENT_RENDER_DEVICE_EVENT_VERSION_1;event.generation=generation;
    event.reason=TC_COMPONENT_RENDER_DEVICE_REBUILT;
    try{plugin->deviceCallback(plugin->deviceUser,&event);}
    catch(...){logger("Component render: device callback threw for "+plugin->id);
     plugin->deviceCallback=nullptr;plugin->deviceUser=nullptr;}
   }
   plugin->deviceGeneration=generation;
  }
 }
 void boot(){if(started)return;started=true;gameThreadId=GetCurrentThreadId();advanceTextures(0);try{
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
armDescriptionPictureControl();
armDescriptionBoxControl();
armComponentPreviewControl();
armPanelTrace();
 armComponentPanelControl();
armComponentMenuRefresh();
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
   /* Test/development convenience mirroring TC_MODLOADER_OPEN for the manager:
      open the loader-owned settings page without guessing a home-screen click.
      It is checked once and has no effect in ordinary player processes. */
   {static bool checked=false;if(!checked){checked=true;char value[8]{};
    if(GetEnvironmentVariableA("TC_MODLOADER_OPEN_SETTINGS",value,sizeof(value))>0&&value[0]!='0'){
     if(openPage("tc.modloader","settings"))logger("Opened UI page tc.modloader/settings (auto)");
     else logger("Mod settings page auto-open requested but no fields are registered");
    }}}
   /* Generic visual-regression entry point.  The value is "mod-id/page-id";
      ordinary launches never set it, while a screenshot test can open the
      exact plugin page without guessing a scaled home-button rectangle. */
   {static bool checked=false;if(!checked){checked=true;char value[256]{};
    const DWORD length=GetEnvironmentVariableA("TC_MODLOADER_OPEN_PLUGIN_PAGE",value,sizeof(value));
    if(length>0&&length<sizeof(value)&&value[0]){
     const std::string request(value,length);const size_t slash=request.find('/');
     if(slash!=std::string::npos&&slash>0&&slash+1<request.size()&&
        openPage(request.substr(0,slash),request.substr(slash+1)))
      logger("Opened UI page "+request+" (auto)");
     else logger("Plugin page auto-open request was not found: "+request);
    }}}
  /* One line the first time a compile carries the per-cycle scope tick: "the
     capture never sees a cycle" and "the program was never instrumented" are
     different reports, and this tells them apart in the log. */
  {const uint64_t sites=tc::scope_capture::store().injectedSites();
   static uint64_t reported=0;
   if(sites!=reported){reported=sites;
    logger("Scope tick: instrumented "+std::to_string(sites)+" per-cycle call site(s) so far");}}
auto getFrame=(int(*)())GetProcAddress(engine,"igGetFrameCount");auto getTime=(double(*)())GetProcAddress(engine,"igGetTime");int n=getFrame();lastEngineFrame=n;gameHandles.beginFrame(n);advanceTextures(n);if(n!=lastFrame){lastFrame=n;observeBoardChanges();
  /* The placement half of the component lifecycle: one walk of the live
     instances per frame, after the board's own change detection. */
  pollComponentGeometry();
  /* Some custom instances are bound while level.load is still flattening the
     Board, before their saved tail records can be found.  Finish those restores
     automatically on the first frame that can see the records, before plugin
     on_frame callbacks attempt a refresh or render an editor. */
  tc::logic::restorePendingTailConfigs();
  componentChannels.eraseIf([](const component_channel::Key& key){
   TCComponentInstanceHandle handle{sizeof(TCComponentInstanceHandle),
       TC_COMPONENT_INSTANCES_API_VERSION_1,key.customId,key.instanceId,key.generation};
   return tc::logic::instanceValidate(&handle)!=TC_COMPONENT_INSTANCES_OK;
  });
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
