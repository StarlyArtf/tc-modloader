#include "core.hpp"
#include "compat.hpp"
#include "native.hpp"
#include "pin_order.hpp"
#if defined(TC_PIN_PATCH_ONLY) || defined(TC_PIN_PATCH_PLUGIN)
#include "io_state_cache.hpp"
#include "preview_label_identity.hpp"
#endif
#include "saves.hpp"
#ifdef TC_PIN_PATCH_PLUGIN
#include "../sdk/tc_mod_api.h"
static const TCHost* pinPatchHost=nullptr;
#endif
#if defined(TC_PIN_PATCH_ONLY) && !defined(TC_PIN_PATCH_PLUGIN)
/* Exported marker of the standalone patch build, so the loader's installer can
   tell this file apart from an unknown engine and simply replace it.  An export
   name is used rather than a string: the linker is free to pool literal
   fragments, an export name it is not. */
extern "C" __declspec(dllexport) void tc_pin_patch_marker(){}
#endif
/* Building with TC_PIN_PATCH_ONLY produces the standalone fix for the component
   preview's pin names: the same hook code, but none of the loader - no mods are
   scanned or loaded, no save redirection, no Mods page.  It is a drop-in
   replacement for game_engine.dll for players who do not want the loader.  The
   address list is build specific, so the compatibility check below still has to
   pass: on another game build the patch refuses to install anything and the game
   behaves exactly as before. */
#include <memory>
#include <shellapi.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
struct V2 {float x,y;};
struct V4 {float x,y,z,w;};
static HMODULE engine;
template<class T> T api(const char* name){auto p=GetProcAddress(engine,name);if(!p)throw std::runtime_error(std::string("Missing engine API: ")+name);T result;static_assert(sizeof(result)==sizeof(p));memcpy(&result,&p,sizeof(p));return result;}
static std::unique_ptr<tc::Core> core;
static std::unique_ptr<tc::NativeRuntime> nativeRuntime;
static bool attempted=false, compatible=false, homeSeen=false, firstDraw=true;
/* Filled in by DllMain before anything else runs; see save_boot.hpp. */
static tc_save_boot::Result saveRedirect;
/* The reason a game this loader cannot drive gets a native message box instead
   of an in-game page: every RVA this build knows (the main menu's draw hook
   among them) is stale in that situation, so the loader's own UI may never be
   called at all.  Shown once per (loader version + game hash) - the marker file
   it writes is also what support can ask for. */
static bool managerOpen=false;
static bool savesOpen=false;
static bool boardSeen=false;
static uint64_t homeLastSeen=0, boardLastSeen=0;
static uint64_t pageOpenedAt=0;
static int debugFrames=0;
static std::string error;
static std::set<std::string> selected;
static tc::fs::path gameRoot;
static void reportUnsupportedBuild(const std::string& error){
 try{
  const std::string stamp=std::string(TC_MODLOADER_VERSION_STRING)+" "+
      tc::hash(tc::read(gameRoot/L"Turing Complete.exe"))+" "+
      tc::hash(tc::read(gameRoot/L"tc_game_engine.dll"));
  const auto marker=tc::fs::path(gameRoot)/L"tc-modloader-data"/L"unsupported-build.txt";
  bool show=true;
  try{if(tc::fs::exists(marker)&&tc::read(marker)==stamp)show=false;}catch(...){}
  try{tc::atomic(marker,stamp);}catch(...){}
  if(!show)return;
  const std::wstring text=L"TC Mod Loader 与这份游戏不匹配，本次运行不会加载任何 Mod。\n\n"+[&]{
   std::wstring wide;for(char c:error)wide+=(wchar_t)(unsigned char)c;return wide;}()+
   L"\n\n请安装与当前游戏版本匹配的加载器，或先运行安装器卸载加载器。\n详情见 tc-modloader-data\\loader.log。";
  MessageBoxW(nullptr,text.c_str(),L"TC Mod Loader",MB_ICONWARNING|MB_OK|MB_SETFOREGROUND|MB_TOPMOST);
 }catch(...){}
}
static std::unique_ptr<tc::SaveProfiles> saves;
/* The log is append-only and a long session with a chatty mod can grow it without
   bound (the live game directory was at 6.9 MB when this was added), so it is
   rotated once it passes kLogLimit: the current file becomes loader.log.1 and a
   new one starts.  One previous file is kept - that is what a bug report needs -
   and the size is only re-checked every few hundred lines, so the hot path stays
   a plain append. */
static constexpr uint64_t kLogLimit=8ull*1024*1024;
static void log(const std::string& msg){try{
#ifdef TC_PIN_PATCH_PLUGIN
 if(pinPatchHost&&pinPatchHost->log){pinPatchHost->log(pinPatchHost->context,msg.c_str());return;}
#endif
 static uint64_t written=0;static int sinceCheck=0;static bool measured=false;
 auto path=gameRoot/L"tc-modloader-data"/L"loader.log";
 if(!measured){measured=true;std::error_code error;written=gameRoot.empty()?0:tc::fs::file_size(path,error);if(error)written=0;}
 written+=msg.size()+1;
 if(++sinceCheck>=512){sinceCheck=0;
  std::error_code error;auto size=tc::fs::file_size(path,error);
  if(!error&&size>kLogLimit){auto previous=path;previous+=L".1";tc::fs::remove(previous,error);tc::fs::rename(path,previous,error);written=0;}}
 std::ofstream f(path,std::ios::app);f<<msg<<"\n";
}catch(...) {}}
/* Toolbar tools: igEndChild is the engine's own export, so the loader hooks it
   (MinHook, one target, owned by the loader) and checks the return address: the
   one call that closes the board's tool column is where plugin tools are drawn,
   after the game's own controls and inside the same child window. */
static void (*endChildOriginal)()=nullptr;
/* Development aid: TC_MODLOADER_TRACE_TEXT=<substring> logs every ImGui text
   submission whose string contains <substring>, together with the font, the
   size and the number of vertices the current window's draw list gained while
   that call ran.  It separates "the text was never submitted" from "it was
   submitted and produced geometry the screen never showed", which is the
   question the tool column keeps raising.  Only armed when the variable is
   set, so a normal run pays nothing. */
static const char* traceText=nullptr;
static void (*renderTextOriginal)(V2,const char*,const char*,bool)=nullptr;
static int drawListVertices(){
 auto drawList=reinterpret_cast<void*(*)()>(GetProcAddress(engine,"igGetWindowDrawList"));
 if(!drawList)return -1;
 void* list=drawList();
 return list?*reinterpret_cast<const int*>(list):-1;
}
static void hookRenderText(V2 position,const char* text,const char* text_end,bool hide){
 if(traceText&&text&&std::strstr(text,traceText)){
  const int before=drawListVertices();
  if(renderTextOriginal)renderTextOriginal(position,text,text_end,hide);
  const int after=drawListVertices();
  auto getFont=reinterpret_cast<void*(*)()>(GetProcAddress(engine,"igGetFont"));
  auto getSize=reinterpret_cast<float(*)()>(GetProcAddress(engine,"igGetFontSize"));
  auto getName=reinterpret_cast<const char*(*)(const void*)>(GetProcAddress(engine,"ImFont_GetDebugName"));
  void* font=getFont?getFont():nullptr;
  const char* name=(font&&getName)?getName(font):"?";
  char where[96]{};std::snprintf(where,sizeof(where),"%.60s",text);
  char line[512];
  std::snprintf(line,sizeof(line),
   "RenderText \"%s\" at %.0f,%.0f font=%s size=%.1f vertices %d -> %d",
   where,position.x,position.y,name?name:"?",getSize?getSize():-1.f,before,after);
  log(line);
  return;
 }
 if(renderTextOriginal)renderTextOriginal(position,text,text_end,hide);
}
/* Development aid: TC_MODLOADER_TRACE_LABEL=<substring> logs the game's
   label-mesh text submissions (Renderer/multi_mesh/label_mesh.set_text) that
   contain <substring>: the caller's return address, the rotation byte of the
   placement transform and the bounding box the call produced.  Labels are what
   component and pin names are drawn with, so this is where "a long name
   overlaps its neighbour" can be measured instead of guessed. */
static const char* traceLabel=nullptr;
/* Measured on this build: Renderer/multi_mesh/label_mesh.set_text (COFF symbol
   set_text__presenterZrendererZmulti95meshZlabel95mesh_u990, VA 0x140286600).
   The game's own functions are COFF symbols of the executable, not exports, so
   GetProcAddress cannot find them and the address is an RVA like the other
   measured call sites in compat.hpp. */
static constexpr uintptr_t TC_LABEL_SET_TEXT_RVA=0x286600;
/* The same module has a second text setter for fading labels (the ones the
   component drawer and hover info use): Renderer/multi_mesh/fading_label_mesh
   .set_text, VA 0x1402a4c10.  Both are hooked because either can be the one a
   given piece of UI text goes through. */
static constexpr uintptr_t TC_FADING_LABEL_SET_TEXT_RVA=0x2a4c10;
/* Return address of the set_text call inside redraw_component_label (measured:
   the call sits at 0x472f24, so the callee sees 0x472f29).  That is the path
   every component/IO name goes through - the level's own IO names and a custom
   component's pin names alike - which keeps the rotation away from level gate
   names, wire labels and the UI. */
static constexpr uintptr_t TC_COMPONENT_LABEL_CALL_RVA=0x472f29;
struct LabelText {unsigned long long length;const char* data;};
static void (*setTextOriginal)(void*,void*,const void*,void*,void*,void*,void*)=nullptr;
static int labelTraceCount=0;
/* Reading a pointer that may not be a string is how a probe kills a game; the
   pages behind it are checked first. */
static bool readableBytes(const void* address,size_t size){
 if(!address||!size)return false;
 const unsigned char* cursor=static_cast<const unsigned char*>(address);
 size_t left=size;
 while(left>0){
  MEMORY_BASIC_INFORMATION info{};
  if(!VirtualQuery(cursor,&info,sizeof(info)))return false;
  if(info.State!=MEM_COMMIT||(info.Protect&PAGE_GUARD)||(info.Protect&PAGE_NOACCESS))return false;
  const size_t offset=(size_t)(cursor-static_cast<const unsigned char*>(info.BaseAddress));
  const size_t available=(size_t)info.RegionSize-offset;
  if(available>=left)return true;
  left-=available;cursor+=available;
 }
 return true;
}
static void hookSetText(void* out,void* collection,const void* text,void* placement,
                        void* arg5,void* arg6,void* arg7){
 std::string value;
 const auto* label=static_cast<const LabelText*>(text);
 /* The label text is a plain byte string behind the length: the logged bytes read
   "4f 75 74 70" for a 6-unit label, i.e. "Outp" (an earlier attempt to read the
   elements as 32-bit code points was wrong and produced rubbish). */
 if(label&&readableBytes(text,sizeof(*label))&&label->data&&label->length>0&&
    label->length<512&&readableBytes(label->data,(size_t)label->length))
  value.assign(label->data,static_cast<size_t>(label->length));
 const unsigned char* in=static_cast<const unsigned char*>(placement);
 const unsigned rotation=in?in[0x10]:0;
 /* Development books: the label's measured width per text, used by the bounded
   self-diagnosis below.  (An earlier version also rotated wide labels here; that
   was not the feature anyone asked for - the labels a player reports live in the
   component workshop's preview, which is drawn from a rendered snapshot and never
   reaches this function - so it was removed.) */
 static std::map<std::string,float> knownWidths;
 if(setTextOriginal)setTextOriginal(out,collection,text,placement,arg5,arg6,arg7);
 /* Bounded self-diagnosis: the first few *distinct* label sources are written to
   the loader log with their call site, size and text.  A label that a player
   reports as overlapping is then traceable without anyone having to set an
   environment variable - the log says which call site draws it. */
 {
 static std::set<std::pair<uintptr_t,size_t>> seen;
 static int written=0;
  if(!value.empty()&&written<220){
   const auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);
   if(seen.insert({rva,value.size()}).second){
    ++written;
    const float* box=static_cast<const float*>(out);
    /* The raw bytes matter here: the label text's element size is not what it
       looks like (decoding as bytes and as 32-bit code points both produced
       nonsense), so the log carries the declared length and the first bytes. */
    const unsigned char* raw=label?reinterpret_cast<const unsigned char*>(label->data):nullptr;
    char hex[64]{};
    for(int i=0;i<12&&raw;++i)
     std::snprintf(hex+i*3,sizeof(hex)-i*3,"%02x ",raw[i]);
    char line[320];
    std::snprintf(line,sizeof(line),
                  "Label source: caller=0x%llx unit=%llu decoded=%llu box=%.3f,%.3f,%.3f,%.3f bytes=%s",
                  (unsigned long long)rva,(unsigned long long)(label?label->length:0),
                  (unsigned long long)value.size(),
                  box?box[0]:-1.f,box?box[1]:-1.f,box?box[2]:-1.f,box?box[3]:-1.f,hex);
    log(line);
   }
  }
 }
 if(!traceLabel)return;
 const bool dumpAll=!*traceLabel;
 const bool match=!dumpAll&&value.find(traceLabel)!=std::string::npos;
 /* The label text arrives as a length plus a pointer whose elements are not
   plain bytes, so a substring match is not enough on its own: long labels are
   always reported by length, which is what a pin name with an overflowing name
   looks like. */
 const unsigned long long length=label?label->length:0;
 if(match||(dumpAll&&(labelTraceCount<40||length>=10))){
  if(dumpAll)++labelTraceCount;
  const auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);
  const float* box=static_cast<const float*>(out);
  const unsigned char* raw=static_cast<const unsigned char*>(text);
  char line[640];
  std::snprintf(line,sizeof(line),
   "Label \"%.*s\" caller=0x%llx rotation=%u box=%.3f,%.3f,%.3f,%.3f size=%llu raw=%02x%02x%02x%02x %02x%02x%02x%02x",
   (int)(value.size()>60?60:value.size()),value.c_str(),(unsigned long long)rva,rotation,
   box?box[0]:-1.f,box?box[1]:-1.f,box?box[2]:-1.f,box?box[3]:-1.f,
   (unsigned long long)value.size(),raw?raw[0]:0,raw?raw[1]:0,raw?raw[2]:0,raw?raw[3]:0,
   raw?raw[4]:0,raw?raw[5]:0,raw?raw[6]:0,raw?raw[7]:0);
  log(line);
 }
}
/* The game's own text renderer may reach the draw list directly instead of
   going through ImGui's RenderText; this hook covers that path so a needle can
   be found wherever the game draws it. */
static void (*addTextOriginal)(void*,V2,unsigned,const char*,const char*)=nullptr;
static void hookAddText(void* self,V2 position,unsigned colour,const char* begin,const char* end){
 if(traceText&&begin&&std::strstr(begin,traceText)){
  const auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);
  char line[320];
  std::snprintf(line,sizeof(line),"AddText \"%.60s\" caller=0x%llx at %.0f,%.0f",
                begin,(unsigned long long)rva,position.x,position.y);
  log(line);
 }
 if(addTextOriginal)addTextOriginal(self,position,colour,begin,end);
}
/* TextEx is what both TextUnformatted and the formatted Text entry points end
   up in, so hooking it (a 3-argument function, no varargs to forward) covers
   every ImGui text the game draws - including text that never reaches
   ImGui::RenderText in this build. */
static void (*textExOriginal)(const char*,const char*,int)=nullptr;
static int textExCount=0;
static void hookTextEx(const char* text,const char* text_end,int flags){
 if(traceText&&text){
  const size_t length=text_end?static_cast<size_t>(text_end-text):std::strlen(text);
  const bool dumpAll=!*traceText;
  if(length<512&&(dumpAll?(textExCount<120):std::strstr(text,traceText)!=nullptr)){
   if(dumpAll)++textExCount;
   const auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);
   char line[320];
   std::snprintf(line,sizeof(line),"TextEx \"%.*s\" caller=0x%llx",
                 (int)(length>60?60:length),text,(unsigned long long)rva);
   log(line);
  }
 }
 if(textExOriginal)textExOriginal(text,text_end,flags);
}
/* The same idea one level up, and the reason the export hooks above see nothing:
   the engine's exported ig* names are thin wrappers (measured: igTextUnformatted
   at +0x21510 is `mov r8d,1; jmp 0x1800fb2d0`), while the game's own UI code
   calls the internal function directly.  The internal entry point is therefore
   hooked by address - engine+0xfb2d0, i.e. TextEx(text, text_end, flags). */
static void (*textUnformattedOriginal)(const char*,const char*,int)=nullptr;
static void hookTextUnformatted(const char* text,const char* text_end,int flags){
 if(traceText&&text){
  const size_t length=text_end?static_cast<size_t>(text_end-text):std::strlen(text);
  const bool dumpAll=!*traceText;
  if(length<512&&(dumpAll?(textExCount<200):std::strstr(text,traceText)!=nullptr)){
   if(dumpAll)++textExCount;
   const auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);
   char line[320];
   std::snprintf(line,sizeof(line),"Text \"%.*s\" caller=0x%llx",
                 (int)(length>60?60:length),text,(unsigned long long)rva);
   log(line);
  }
 }
 if(textUnformattedOriginal)textUnformattedOriginal(text,text_end,flags);
}
#if defined(TC_PIN_PATCH_ONLY) || defined(TC_PIN_PATCH_PLUGIN)
/* Component previews: pin names that do not fit become a number and a table.

   Measured on this build: the bottom panel's component preview - the same
   picture the component workshop shows - is built by
   build_custom_component_preview (executable RVA 0x38e990).  It draws the pins
   and then, once per pin, the pin's own name as ImGui text: the name is measured
   with igCalcTextSize, placed through get_label_offset and drawn with igText at
   0x38f0ea, so the text function sees the return address 0x38f0ef.  The preview
   scales the component but not that text, so a long name lands at full size on
   top of its neighbour's name (the complaint this fixes).

   Fitting those names into the picture does not work.  In the workshop one edge of
   the component carries pins 24 pixels apart while a name there grows to 470, and
   a name that long cannot be turned or shrunk into a 400 pixel tall panel and stay
   readable - spreading, turning and shrinking were all tried, and the result was a
   web of names drifting away from the pins.  So a name that does not fit its pin
   is not drawn at the pin at all: the pin gets a small number, and the numbers are
   listed, with their names, in a table in the free side of the panel.  That is how
   a schematic sheet answers the same problem.  Names that fit stay exactly where
   the game drew them, and every other caller of ImGui's text function is forwarded
   untouched.  TC_MODLOADER_PIN_TABLE=0 switches the whole thing off; any other
   number is the table's text size in percent (default 70). */
static constexpr uintptr_t TC_PREVIEW_LABEL_CALL_RVA=0x38f0ef;
/* The same thing happens once more in the foundry's own editor - the panel that
   opens when the player edits a component's appearance - where the pins and their
   names are drawn by build_editor (bar 0x3adf96, name 0x3ae126).  Both are handled
   the same way; the numbers are per picture, so the two never share a numbering. */
static constexpr uintptr_t TC_EDITOR_LABEL_CALL_RVA=0x3ae126;
/* One picture is drawn at a time - the bottom panel and the foundry's editor never
   appear together - so the list of names collected during a frame belongs to the
   picture in front of the player, whatever drew it. */
static int previewLabelRows=0;
/* ImGui keeps a window's font scale as a plain float on the window - its own
   SetWindowFontScale writes [window+0x308], measured - so the number and the table
   can be drawn at their own size and the window's scale put back exactly. */
static constexpr uintptr_t TC_WINDOW_FONT_SCALE_RVA=0x308;
static bool previewLabelNames=true;
static float previewLabelTableScale=0.70f;
static void (*previewTextV)(const char*,va_list)=nullptr;
static void (*previewTextUnformatted)(const char*,const char*)=nullptr;
static void (*previewCalcTextSize)(V2*,const char*,const char*,bool,float)=nullptr;
static void (*previewGetCursorScreenPos)(V2*)=nullptr;
static void (*previewSetCursorScreenPos)(V2)=nullptr;
static void (*previewGetWindowPos)(V2*)=nullptr;
static void (*previewGetWindowSize)(V2*)=nullptr;
static void (*setWindowFontScaleOriginal)(float)=nullptr;
static void* (*previewGetCurrentWindow)()=nullptr;
static void (*previewPushStyleColor)(int,V4)=nullptr;
static void (*previewPopStyleColor)(int)=nullptr;
/* Every pin name the preview drew, where the game put it.  A name only ever
   replaces itself, and whether it fits is decided against the *finished* frame
   before - the names of one row are not drawn one after another, so a list that
   grew while the frame was being drawn would judge the same row differently as it
   went and put its names in different places. */
struct PreviewLabelBox{
 float centreX,centreY,width,height;
 float markerX,markerY;   /* where this pin's number goes */
 float pinX,pinY;         /* where the pin itself is */
 float outwardX,outwardY; /* which way leads away from the part */
 float barLength;         /* how long the pin's bar is, from the pin outwards */
 bool hasBar;
 char text[64];
 int number;
};
static constexpr int previewLabelBoxMax=64;
static PreviewLabelBox previewLabelBoxes[previewLabelBoxMax];
static int previewLabelBoxCount=0;
static PreviewLabelBox previewLabelFresh[previewLabelBoxMax];
static int previewLabelFreshCount=0;
static unsigned long long previewLabelTick=0,previewLabelListTick=~0ull;
static unsigned long long previewLabelTableTick=~0ull;
static float previewLabelNumberScale=0.60f;
static int previewLabelRings=1;
/* The part of the panel the picture itself uses, numbers included: the table goes
   beside that, not beside the names the numbers replaced. */
static float previewLabelUsedLeft=0.f,previewLabelUsedRight=0.f;
/* The preview's centre, from the last frame: the pin bars are drawn before their
   names, so this is what tells the bar hook which way is away from the part. */
static float previewLabelCentreX=0.f,previewLabelCentreY=0.f;
static bool previewLabelBarPending=false;
static bool previewLabelBarEditor=false;
static V2 previewLabelBarMin{},previewLabelBarMax{};
static int previewLabelLogged=0;
static int previewLabelTableLogged=0;
/* The foundry's appearance editor shares the numbering (its pins keep their
   numbers, so the sprite is not covered in names) but not the table: its panel is
   full of the game's own colour palette, and the player is there to edit colours,
   not to read pin names. */
static bool previewLabelFromEditor=false;
/* Set while a frame draws names the last frame knew nothing about - that is what
   switching between the preview and the appearance editor looks like.  Those names
   are not drawn at all (their numbers arrive on the next frame), and the table
   waits for the picture to settle, so the change reads as a clean swap instead of
   one frame of overlapping names and a stale table. */
static bool previewLabelPictureChanged=false;
static bool previewLabelSamePin(const PreviewLabelBox& previous,
                                const PreviewLabelBox& current){
 return tc::preview_labels::samePin(
  previous.hasBar,previous.pinX,previous.pinY,previous.outwardX,previous.outwardY,
  previous.centreX,previous.centreY,
  current.hasBar,current.pinX,current.pinY,current.outwardX,current.outwardY,
  current.centreX,current.centreY);
}
static int previewLabelPreviousPin(const PreviewLabelBox& current){
 for(int i=0;i<previewLabelBoxCount;++i)
  if(previewLabelSamePin(previewLabelBoxes[i],current))return i;
 return -1;
}
static float previewWindowFontScale(){
 void* window=previewGetCurrentWindow?previewGetCurrentWindow():nullptr;
 if(!window)return 1.f;
 const char* field=static_cast<const char*>(window)+TC_WINDOW_FONT_SCALE_RVA;
 if(!readableBytes(field,sizeof(float)))return 1.f;
 float scale=0.f;
 std::memcpy(&scale,field,sizeof(scale));
 return scale>0.f?scale:1.f;
}

/* Every pin the preview shows gets a number; the names live in the table beside
   the picture.  The numbers are the loader's, assigned the way a data sheet does
   it: clockwise around the part, starting at its top left corner.  To do that the
   pin behind each name has to be found first.  The preview draws a little bar for
   every pin just before its name, and that bar is the exact record: it says where
   the pin is, which edge it sits on and which way leads away from the part.  Only
   on the very first frame (before any bar is known) the names themselves are used,
   with the rule that a name on a left or right pin is drawn against the part. */
static void previewLabelNumber(){
 V2 windowPos{},windowSize{};
 previewGetWindowPos(&windowPos);previewGetWindowSize(&windowSize);
 const float middleX=windowPos.x+windowSize.x*0.5f;
 /* Every name the frame drew belongs to the picture in front of the player. */
 int mine[previewLabelBoxMax];
 int mineCount=0;
 for(int i=0;i<previewLabelBoxCount;++i)mine[mineCount++]=i;
 /* The middle of the part: from the bars when they are known, else from the names. */
 float centreX=0.f,centreY=0.f;
 int centreCount=0;
 for(int m=0;m<mineCount;++m){
  const PreviewLabelBox& box=previewLabelBoxes[mine[m]];
  if(box.hasBar){centreX+=box.pinX-box.outwardX*box.barLength*0.5f;
                 centreY+=box.pinY-box.outwardY*box.barLength*0.5f;++centreCount;}
 }
 if(!centreCount)
  for(int m=0;m<mineCount;++m){
   centreX+=previewLabelBoxes[mine[m]].centreX;
   centreY+=previewLabelBoxes[mine[m]].centreY;++centreCount;
  }
 if(centreCount){centreX/=(float)centreCount;centreY/=(float)centreCount;}
 previewLabelCentreX=centreX;previewLabelCentreY=centreY;
 /* Which edge each pin is on, and how far along that edge it sits. */
 float pinLeft=0.f,pinTop=0.f,pinRight=0.f,pinBottom=0.f;
 for(int m=0;m<mineCount;++m){
  const int i=mine[m];
  PreviewLabelBox& box=previewLabelBoxes[i];
  box.number=0;
  if(!box.hasBar){
   /* First frame: fall back to reading the pin out of the name. */
   box.pinX=box.centreX;
   box.pinY=box.centreY;
   box.outwardX=box.outwardY=0.f;
   box.barLength=0.f;
   if(box.centreX+box.width*0.5f<middleX)box.pinX=box.centreX+box.width*0.5f-box.height*0.5f;
   else if(box.centreX-box.width*0.5f>middleX)box.pinX=box.centreX-box.width*0.5f+box.height*0.5f;
  }
  if(!i||box.pinX<pinLeft)pinLeft=box.pinX;
  if(!i||box.pinX>pinRight)pinRight=box.pinX;
  if(!i||box.pinY<pinTop)pinTop=box.pinY;
  if(!i||box.pinY>pinBottom)pinBottom=box.pinY;
 }
 /* Walk the perimeter of that box clockwise from its top left corner: the top edge
    left to right, the right edge down, the bottom edge right to left, the left edge
    back up. */
const float boxWidth=std::max(1.f,pinRight-pinLeft),boxHeight=std::max(1.f,pinBottom-pinTop);
float walk[previewLabelBoxMax];
int edge[previewLabelBoxMax];
for(int i=0;i<previewLabelBoxCount;++i){
  const PreviewLabelBox& box=previewLabelBoxes[i];
  const float toLeft=std::fabs(box.pinX-pinLeft),toRight=std::fabs(pinRight-box.pinX);
  const float toTop=std::fabs(box.pinY-pinTop),toBottom=std::fabs(pinBottom-box.pinY);
  float at=0.f;
  if(box.hasBar){
   /* The bar already says which way is out of the part. */
   if(box.outwardY<0.f){edge[i]=0;at=box.pinX-pinLeft;}
   else if(box.outwardX>0.f){edge[i]=1;at=boxWidth+(box.pinY-pinTop);}
   else if(box.outwardY>0.f){edge[i]=2;at=boxWidth+boxHeight+(pinRight-box.pinX);}
   else{edge[i]=3;at=2.f*boxWidth+boxHeight+(pinBottom-box.pinY);}
  }
  else{
   const float nearest=std::min(std::min(toLeft,toRight),std::min(toTop,toBottom));
   if(nearest==toTop){edge[i]=0;at=box.pinX-pinLeft;}
   else if(nearest==toRight){edge[i]=1;at=boxWidth+(box.pinY-pinTop);}
   else if(nearest==toBottom){edge[i]=2;at=boxWidth+boxHeight+(pinRight-box.pinX);}
   else{edge[i]=3;at=2.f*boxWidth+boxHeight+(pinBottom-box.pinY);}
  }
  walk[i]=at;
}
 int order[previewLabelBoxMax];
 for(int i=0;i<mineCount;++i)order[i]=mine[i];
 for(int i=1;i<mineCount;++i)for(int j=i;j>0;--j){
  if(walk[order[j-1]]<=walk[order[j]])break;
  const int moved=order[j-1];order[j-1]=order[j];order[j]=moved;
 }
 for(int i=0;i<mineCount;++i)previewLabelBoxes[order[i]].number=i+1;
 previewLabelRows=mineCount;
 /* How big a number can be.  All the numbers of one edge stay on one line, each
    directly outside its own pin: staggered rows were tried and read as misaligned,
    because the eye follows the row rather than the pin.  So the digits are as large
    as the pitch between the pins allows and no larger - the pitch is measured, not
    guessed. */
 float numberWidth=0.f,numberHeight=0.f;
 if(previewCalcTextSize){
 char widest[16];
 std::snprintf(widest,sizeof(widest),"%d",std::max(1,mineCount));
  const float restore=previewWindowFontScale();
  if(setWindowFontScaleOriginal)setWindowFontScaleOriginal(1.f);
  V2 text{};
  previewCalcTextSize(&text,widest,nullptr,false,0.f);
  if(setWindowFontScaleOriginal)setWindowFontScaleOriginal(restore);
  numberWidth=text.x;numberHeight=text.y;
 }
 /* The pitch of an edge is its span divided by its pins, not the closest pair of
    them: two pins of a hand made part can sit almost on top of each other, and one
    such pair would shrink every number in the picture. */
 float pitch=0.f;
 for(int side=0;side<4;++side){
  float low=0.f,high=0.f;
 int count=0;
  for(int m=0;m<mineCount;++m){
   const int i=mine[m];
   if(edge[i]!=side)continue;
   if(!count||walk[i]<low)low=walk[i];
   if(!count||walk[i]>high)high=walk[i];
   ++count;
  }
  if(count<2)continue;
  const float step=(high-low)/(float)(count-1);
  if(step>0.5f&&(!(pitch>0.f)||step<pitch))pitch=step;
 }
const int rings=1;
 float scale=0.65f;
 if(pitch>0.f&&numberWidth>0.f){
  scale=std::min(0.65f,0.85f*pitch/numberWidth);
  if(scale<0.25f)scale=0.25f;
 }
 previewLabelNumberScale=scale;
 previewLabelRings=rings;
 if(previewLabelTableLogged<6){
  ++previewLabelTableLogged;
  char line[160];
  std::snprintf(line,sizeof(line),
               "Preview pin numbering pins=%d pitch=%.1f rows=%d scale=%.2f",
               mineCount,pitch,rings,scale);
  log(line);
 }
 /* The numbers sit just outside their own pin's bar: close to it, with a gap. */
const float line=std::max(1.f,numberHeight*scale);
for(int m=0;m<mineCount;++m){
 const int i=mine[m];
 PreviewLabelBox& box=previewLabelBoxes[i];
  const float outward=box.barLength+line*0.5f+4.f;
  /* Outwards, unless the panel ends first - a pin on the bottom edge of a preview
     that sits at the bottom of the panel gets its number on the inside instead of
     having it cut off. */
  float direction=1.f;
  const float margin=outward;
  if(edge[i]==0&&box.pinY-margin<windowPos.y+4.f)direction=-1.f;
  else if(edge[i]==1&&box.pinX+margin>windowPos.x+windowSize.x-4.f)direction=-1.f;
  else if(edge[i]==2&&box.pinY+margin>windowPos.y+windowSize.y-4.f)direction=-1.f;
  else if(edge[i]==3&&box.pinX-margin<windowPos.x+4.f)direction=-1.f;
  if(edge[i]==0){box.markerX=box.pinX;box.markerY=box.pinY-direction*outward;}
  else if(edge[i]==1){box.markerX=box.pinX+direction*outward;box.markerY=box.pinY;}
  else if(edge[i]==2){box.markerX=box.pinX;box.markerY=box.pinY+direction*outward;}
  else{box.markerX=box.pinX-direction*outward;box.markerY=box.pinY;}
 }
 /* What the picture occupies now: the pins, the part around them and the numbers. */
 previewLabelUsedLeft=pinLeft-line-24.f;
 previewLabelUsedRight=pinRight+line+24.f;
}
/* The list itself: numbers and names, filled down one column and then the next,
   placed in the free side of the panel. */
static void previewLabelTable(){
 if(previewLabelRows<=0||!previewCalcTextSize||!previewTextUnformatted||
    !previewSetCursorScreenPos||!previewGetWindowPos||!previewGetWindowSize)return;
 V2 windowPos{},windowSize{};
 previewGetWindowPos(&windowPos);previewGetWindowSize(&windowSize);
 if(windowSize.x<240.f||windowSize.y<80.f){
  if(previewLabelTableLogged<6){
   ++previewLabelTableLogged;
   char line[192];
   std::snprintf(line,sizeof(line),"Preview pin table window=%.0fx%.0f rows=%d -> too small",
                 windowSize.x,windowSize.y,previewLabelRows);
   log(line);
  }
  return;
 }
 const float contentLeft=previewLabelUsedLeft,contentRight=previewLabelUsedRight;
 const float freeLeft=contentLeft-windowPos.x-24.f;
 const float freeRight=windowPos.x+windowSize.x-contentRight-24.f;
 /* The table goes on the right whenever it fits there: the game's own controls of
   these panels (the foundry's colour palette, the drawer's buttons) sit on the
   left, and the loader cannot see them. */
 const bool useRight=freeRight>=freeLeft;
 const float side=std::max(freeRight,freeLeft);
 if(side<200.f){
  if(previewLabelTableLogged<6){
   ++previewLabelTableLogged;
   char line[192];
   std::snprintf(line,sizeof(line),
                 "Preview pin table window=%.0fx%.0f content=%.0f..%.0f rows=%d -> no room",
                 windowSize.x,windowSize.y,contentLeft,contentRight,previewLabelRows);
   log(line);
  }
  return;
 }
 /* Measure at each candidate size: how tall a line is, how wide the name column
    is, and how many columns the list needs to fit the panel's height. */
 auto measure=[&](float size,float* columnWidth,float* rowHeight,float* blockWidth,
                  float* blockHeight,int* columnsNeeded)->bool{
  const float restore=previewWindowFontScale();
  if(setWindowFontScaleOriginal)setWindowFontScaleOriginal(size);
  float widest=0.f,tallest=0.f;
  V2 measured{};
  for(int i=0;i<previewLabelBoxCount;++i){
   if(previewLabelBoxes[i].number<=0)continue;
   previewCalcTextSize(&measured,previewLabelBoxes[i].text,nullptr,false,0.f);
   widest=std::max(widest,measured.x);
   tallest=std::max(tallest,measured.y);
  }
  if(setWindowFontScaleOriginal)setWindowFontScaleOriginal(restore);
  if(!(tallest>0.f))return false;
  *columnWidth=widest+96.f;
  *rowHeight=tallest+6.f;
  const int perColumn=std::max(1,(int)((windowSize.y-32.f)/(*rowHeight)));
  const int columns=(previewLabelRows+perColumn-1)/perColumn;
  *blockWidth=(float)columns*(*columnWidth)+24.f*(float)(columns-1);
  *blockHeight=*rowHeight*(float)std::min(previewLabelRows,perColumn);
  *columnsNeeded=columns;
  return true;
 };
 float scale=previewLabelTableScale,blockWidth=0.f,rowHeight=0.f,columnWidth=0.f,blockHeight=0.f;
 int columns=1;
 bool fits=false;
 for(;scale>=0.35f;scale-=0.05f){
  if(!measure(scale,&columnWidth,&rowHeight,&blockWidth,&blockHeight,&columns))return;
  if(blockWidth<=side&&columns<=6){fits=true;break;}
 }
 if(previewLabelTableLogged<6){
  ++previewLabelTableLogged;
  char line[192];
  std::snprintf(line,sizeof(line),
                "Preview pin table window=%.0fx%.0f side=%.0f right=%d rows=%d scale=%.2f"
                " columns=%d width=%.0f -> %s",
                windowSize.x,windowSize.y,side,useRight?1:0,previewLabelRows,scale,columns,
                blockWidth,fits?"drawn":"skipped");
  log(line);
 }
 if(!fits)return;
 /* Right if the block fits there - the game's own controls sit on the left. */
 const bool putRight=freeRight>=blockWidth;
 const float left=putRight?windowPos.x+windowSize.x-24.f-blockWidth:windowPos.x+24.f;
 /* Kept clear of the panel's own corner buttons, which sit in the top 60 pixels. */
 const float firstTop=windowPos.y+std::max(60.f,(windowSize.y-blockHeight)*0.5f);
 const float restore=previewWindowFontScale();
 if(setWindowFontScaleOriginal)setWindowFontScaleOriginal(scale);
 const int perColumn=std::max(1,(int)((windowSize.y-32.f)/rowHeight));
 for(int number=1;number<=previewLabelRows;++number){
  const PreviewLabelBox* row=nullptr;
  for(int i=0;i<previewLabelBoxCount;++i)
   if(previewLabelBoxes[i].number==number){row=&previewLabelBoxes[i];break;}
  if(!row)continue;
  const int column=(number-1)/perColumn;
  const float top=firstTop+(float)((number-1)%perColumn)*rowHeight;
  const float at=left+(float)column*(columnWidth+24.f);
  char label[16];
  std::snprintf(label,sizeof(label),"%d",number);
  V2 numberSize{};
  previewCalcTextSize(&numberSize,label,nullptr,false,0.f);
  previewSetCursorScreenPos(V2{at+(64.f-numberSize.x),top});
  previewTextUnformatted(label,nullptr);
  previewSetCursorScreenPos(V2{at+80.f,top});
  previewTextUnformatted(row->text,nullptr);
 }
 if(setWindowFontScaleOriginal)setWindowFontScaleOriginal(restore);
}
/* Called for every name the preview draws: the first call of a new frame promotes
   what the last frame collected and numbers its pins.  The list is drawn after
   this frame has supplied all of its current names (see previewLabelPin). */
static void previewLabelFrame(bool fromEditor){
 if(previewLabelListTick==previewLabelTick)return;
 previewLabelListTick=previewLabelTick;
 previewLabelFromEditor=fromEditor;
 /* What the frame before this one saw: a picture that had just changed gets its
    numbers but no table yet. */
 const bool changed=previewLabelPictureChanged;
 previewLabelPictureChanged=false;
 previewLabelBoxCount=previewLabelFreshCount;
 for(int i=0;i<previewLabelBoxCount;++i)previewLabelBoxes[i]=previewLabelFresh[i];
 previewLabelFreshCount=0;
 previewLabelNumber();
 /* The table is drawn after the last current-frame pin is collected below.
    That lets an edited input/output name appear in this frame instead of
    rendering the previous frame's cached text. */
 if(changed)previewLabelTableTick=previewLabelTick;
}
static int previewLabelRemember(float centreX,float centreY,float width,float height,
                                const char* text){
 /* The bar the preview drew just before this name says where the pin is, which edge
    it is on and which way leads away from the part. */
 float pinX=centreX,pinY=centreY,outwardX=0.f,outwardY=0.f,barLength=0.f;
 const bool hasBar=previewLabelBarPending;
 if(hasBar){
  const float barWidth=previewLabelBarMax.x-previewLabelBarMin.x;
  const float barHeight=previewLabelBarMax.y-previewLabelBarMin.y;
  const float barCentreX=(previewLabelBarMin.x+previewLabelBarMax.x)*0.5f;
  const float barCentreY=(previewLabelBarMin.y+previewLabelBarMax.y)*0.5f;
  if(std::fabs(barHeight)>=std::fabs(barWidth)){
   const bool above=previewLabelCentreY>0.f&&barCentreY<previewLabelCentreY;
   pinX=barCentreX;
   pinY=above?std::max(previewLabelBarMin.y,previewLabelBarMax.y)
             :std::min(previewLabelBarMin.y,previewLabelBarMax.y);
   outwardY=above?-1.f:1.f;
   barLength=std::fabs(barHeight);
  }
  else{
   const bool left=previewLabelCentreX>0.f&&barCentreX<previewLabelCentreX;
   pinX=left?std::max(previewLabelBarMin.x,previewLabelBarMax.x)
            :std::min(previewLabelBarMin.x,previewLabelBarMax.x);
   pinY=barCentreY;
   outwardX=left?-1.f:1.f;
   barLength=std::fabs(barWidth);
  }
 }
 previewLabelBarPending=false;
 PreviewLabelBox current{};
 current.centreX=centreX;current.centreY=centreY;current.width=width;current.height=height;
 current.hasBar=hasBar;
 current.pinX=pinX;current.pinY=pinY;current.outwardX=outwardX;current.outwardY=outwardY;
 current.barLength=barLength;
 std::snprintf(current.text,sizeof(current.text),"%.63s",text);
 for(int i=0;i<previewLabelFreshCount;++i)
  if(previewLabelSamePin(previewLabelFresh[i],current)){
   previewLabelFresh[i].width=width;previewLabelFresh[i].height=height;
   std::snprintf(previewLabelFresh[i].text,sizeof(previewLabelFresh[i].text),"%.63s",text);
   previewLabelFresh[i].hasBar=hasBar;
   previewLabelFresh[i].pinX=pinX;previewLabelFresh[i].pinY=pinY;
   previewLabelFresh[i].outwardX=outwardX;previewLabelFresh[i].outwardY=outwardY;
   previewLabelFresh[i].barLength=barLength;
   return i;
  }
 if(previewLabelFreshCount>=previewLabelBoxMax)return -1;
 PreviewLabelBox& box=previewLabelFresh[previewLabelFreshCount++];
 box=current;box.number=0;
 return previewLabelFreshCount-1;
}
/* One pin of the preview: a name that does not fit its pin is not drawn there,
   the pin shows its number instead.  True means "drawn here", so the hook drops
   the name the game was about to draw. */
static bool previewLabelPin(const char* text,bool fromEditor){
 if(!previewLabelNames||!previewCalcTextSize||!previewTextUnformatted||
    !previewSetCursorScreenPos||!previewGetCursorScreenPos)return false;
 const size_t length=std::strlen(text);
 if(!length||length>255)return false;
 V2 size{};previewCalcTextSize(&size,text,nullptr,false,0.f);
 if(!(size.x>0.f)||!(size.y>0.f))return false;
 V2 origin{};previewGetCursorScreenPos(&origin);
 /* The game places a label around its pin; the centre is where the name sits. */
 const float centreX=origin.x+size.x*0.5f,centreY=origin.y+size.y*0.5f;
 /* Either signal says "this is the foundry's appearance editor": the call that
    draws its names, or the call that draws its pin bars. */
 previewLabelFrame(fromEditor||previewLabelBarEditor);
 const int freshIndex=previewLabelRemember(centreX,centreY,size.x,size.y,text);
 const int previousIndex=freshIndex>=0?previewLabelPreviousPin(previewLabelFresh[freshIndex]):-1;
 const int number=previousIndex>=0?previewLabelBoxes[previousIndex].number:0;
 if(previewLabelLogged<16){
  ++previewLabelLogged;
  char line[192];
  std::snprintf(line,sizeof(line),
                "Preview pin name \"%.32s\" frame=%llu pins=%d width=%.1f -> %s",
                text,previewLabelTick,previewLabelBoxCount,size.x,
                number>0?"number":"held back");
  log(line);
 }
 /* A name this frame has no number for is one the last frame never saw: the
    picture has just changed.  It is not drawn - the game's own drawing would be
    the overlapping name this whole thing exists to remove - and its number appears
    on the next frame, when the pins are known. */
 if(number<=0){previewLabelPictureChanged=true;return true;}
 /* The current prototype already contains edits made in the component panel.
    Copy its text into the stable, numbered row before the table is drawn. */
 std::snprintf(previewLabelBoxes[previousIndex].text,
               sizeof(previewLabelBoxes[previousIndex].text),"%.63s",text);
 char label[16];
 std::snprintf(label,sizeof(label),"%d",number);
 const float markerX=previewLabelBoxes[previousIndex].markerX;
 const float markerY=previewLabelBoxes[previousIndex].markerY;
 const float restore=previewWindowFontScale();
 if(setWindowFontScaleOriginal)setWindowFontScaleOriginal(previewLabelNumberScale);
 /* The picture's own text is dim; a number has a single digit or two to be read
    from, so it is drawn in full white. */
 if(previewPushStyleColor)previewPushStyleColor(0,V4{1.f,1.f,1.f,1.f});
 V2 numberSize{};
 previewCalcTextSize(&numberSize,label,nullptr,false,0.f);
 previewSetCursorScreenPos(V2{markerX-numberSize.x*0.5f,markerY-numberSize.y*0.5f});
 previewTextUnformatted(label,nullptr);
 if(previewPopStyleColor)previewPopStyleColor(1);
 if(setWindowFontScaleOriginal)setWindowFontScaleOriginal(restore);
 /* For a label-only edit the pin count is unchanged.  At this point the last
    expected pin has supplied its current text, so the table can use the live
    names in the same rendered frame.  Structural pin changes still take the
    normal one settling frame. */
 if(!previewLabelFromEditor&&!previewLabelPictureChanged&&
    previewLabelFreshCount>=previewLabelBoxCount&&
    previewLabelTableTick!=previewLabelTick){
  previewLabelTableTick=previewLabelTick;
  V2 resume{};previewGetCursorScreenPos(&resume);
  previewLabelTable();
  /* Drawing the table at the end must not move the cursor seen by the game's
     caller; the old first-pin placement was naturally overwritten by the next
     pin's SetCursorPos, but the last pin has no such reset after it. */
  previewSetCursorScreenPos(resume);
 }
 return true;
}
/* ImGui's text entry point.  The preview reaches it through the engine's export
   table, so hooking it there covers the panel and the workshop alike.  Only the
   label call site above is ever taken over; every other call - and every name
   that fits - is forwarded with its own arguments. */
static void hookPreviewText(const char* format,...){
 static const uintptr_t exeBase=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
 va_list args;va_start(args,format);
 if(format){
  const uintptr_t rva=reinterpret_cast<uintptr_t>(__builtin_return_address(0))-exeBase;
  const bool ours=rva==TC_PREVIEW_LABEL_CALL_RVA||rva==TC_EDITOR_LABEL_CALL_RVA;
  if(ours&&previewLabelPin(format,rva==TC_EDITOR_LABEL_CALL_RVA)){
   va_end(args);
   return;
  }
 }
 if(previewTextV)previewTextV(format,args);
 va_end(args);
}
/* The preview draws each pin as a little bar just outside the part (the call is
   the AddRectFilled in build_custom_component_preview, return address 0x38efb1).
   Those bars used to line up with the pin names; with the names in the table they
   only mark where a pin is, so they are drawn thinner and shorter - the number
   beside them is what carries the meaning now. */
static constexpr uintptr_t TC_PREVIEW_PIN_BAR_CALL_RVA=0x38efb1;
/* The foundry editor draws the same bars for its own pins. */
static constexpr uintptr_t TC_EDITOR_PIN_BAR_CALL_RVA=0x3adf96;
static void (*addRectFilledOriginal)(void*,V2,V2,unsigned,float,int)=nullptr;
static void hookAddRectFilled(void* self,V2 minimum,V2 maximum,unsigned colour,
                              float rounding,int flags){
 static const uintptr_t exeBase=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
 const uintptr_t rva=reinterpret_cast<uintptr_t>(__builtin_return_address(0))-exeBase;
 if(previewLabelNames&&(rva==TC_PREVIEW_PIN_BAR_CALL_RVA||rva==TC_EDITOR_PIN_BAR_CALL_RVA)){
  const float dx=maximum.x-minimum.x,dy=maximum.y-minimum.y;
  const float centreX=(minimum.x+maximum.x)*0.5f,centreY=(minimum.y+maximum.y)*0.5f;
  /* This bar is also the only exact record of where the pin is, so the label hook
     that runs right after it reads it - the rect as it is drawn, not as the game
     asked for it, so the number that goes beside the bar sits beside what is on
     screen. */
  previewLabelBarPending=true;
  previewLabelBarEditor=rva==TC_EDITOR_PIN_BAR_CALL_RVA;
  /* Thinner, and shortened from its *outer* end: the end that meets the pin stays
     where the game put it, so the bar keeps touching its pin instead of drifting
     away from it. */
  const float keep=0.5f;
  if(std::fabs(dy)>=std::fabs(dx)){
   /* A vertical bar marks a pin on the top or bottom edge: the pin is at the end
      nearer the middle of the picture, the bar reaches away from it. */
   const bool above=previewLabelCentreY>0.f&&centreY<previewLabelCentreY;
   const float inner=above?std::max(minimum.y,maximum.y):std::min(minimum.y,maximum.y);
   const float outer=inner+(above?-1.f:1.f)*dy*keep;
   minimum.y=std::min(inner,outer);maximum.y=std::max(inner,outer);
   minimum.x=centreX-dx*0.25f;maximum.x=centreX+dx*0.25f;
  }
  else{
   /* A horizontal bar marks a pin on the left or right edge. */
   const bool left=previewLabelCentreX>0.f&&centreX<previewLabelCentreX;
   const float inner=left?std::max(minimum.x,maximum.x):std::min(minimum.x,maximum.x);
   const float outer=inner+(left?-1.f:1.f)*dx*keep;
   minimum.x=std::min(inner,outer);maximum.x=std::max(inner,outer);
   minimum.y=centreY-dy*0.25f;maximum.y=centreY+dy*0.25f;
  }
  previewLabelBarMin=minimum;previewLabelBarMax=maximum;
 }
 if(addRectFilledOriginal)addRectFilledOriginal(self,minimum,maximum,colour,rounding,flags);
}
#endif
/* The left IO panel renders three cached sequences ("input state", "output
   state" and the workshop's memory group) instead of reading the board, and
   this build rebuilds them in exactly one place: load_level_frontend, which
   calls get_io_states once while a level is loaded.  An edit made inside the
   component workshop - a renamed pin, a pin added or removed - therefore never
   reaches that panel until the level is loaded again, which is what "leave the
   workshop and come back to see it" was.  The editor does call
   reload_this_custom_prototype for such an edit, so that call is remembered
   here and the cache is rebuilt right before the panel's next draw, through
   the game's own get_io_states and its managed copy/destroy (string ownership
   stays with the game).

   This is loader-side rather than Mod-side because everything that reads the
   panel - the panel itself, and a Mod that reorders its pins - depends on the
   cache being the board's pins; an edit must not depend on which Mods are
   enabled.  The rebuild happens before the pin order is applied, so the order
   is re-applied to the fresh cache in the same frame.

   These addresses and the context offsets in io_state_cache.hpp are pinned by
   the executable compatibility hash, just like the preview call sites above. */
#ifndef TC_PIN_PATCH_PLUGIN
static constexpr uintptr_t TC_RELOAD_CUSTOM_PROTOTYPE_RVA=0x2e8cd0;
static constexpr uintptr_t TC_BUILD_IO_STATE_VIEW_RVA=0x45ea20;
static constexpr uintptr_t TC_GET_IO_STATES_RVA=0x2e50e0;
static constexpr uintptr_t TC_COPY_IO_STATE_GROUP_RVA=0x2d8a10;
static constexpr uintptr_t TC_DESTROY_IO_STATES_RVA=0x2d9060;
static std::atomic<bool> ioStateNamesDirty{false};
static void (*reloadCustomPrototypeOriginal)(void*)=nullptr;
static void (*buildIoStateViewOriginal)(void*,void*,double,void*,double)=nullptr;
static void hookReloadCustomPrototype(void* presenter){
 if(reloadCustomPrototypeOriginal)reloadCustomPrototypeOriginal(presenter);
 ioStateNamesDirty.store(true,std::memory_order_release);
}
static bool refreshIoStateCache(void* board,void* context){
 /* Development aid: TC_MODLOADER_NO_IO_REFRESH=1 leaves the cache alone, so a
    run that misbehaves with the rebuild can be compared against one without. */
 if(GetEnvironmentVariableW(L"TC_MODLOADER_NO_IO_REFRESH",nullptr,0)>0)return false;
 const auto base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
 const auto generate=reinterpret_cast<void(*)(void*,void*)>(base+TC_GET_IO_STATES_RVA);
 const auto copy=reinterpret_cast<void(*)(void*,const void*)>(base+TC_COPY_IO_STATE_GROUP_RVA);
 const auto destroy=reinterpret_cast<void(*)(void*)>(base+TC_DESTROY_IO_STATES_RVA);
 if(!generate||!copy||!destroy)return false;
 return tc::io_state_cache::refresh(board,context,generate,copy,destroy);
}
/* One detour for the panel's own build entry: first the cache an edit made
   stale, then a Mod's pin order, then the panel. */
static void hookBuildIoStateView(void* board,void* context,double height,
                                 void* layout,double scale){
 if(ioStateNamesDirty.exchange(false,std::memory_order_acq_rel)){
  if(refreshIoStateCache(board,context))
   log("IO panel cache rebuilt (a custom prototype was reloaded)");
  else
   log("IO panel cache rebuild failed: the game's io state helpers are missing");
 }
#ifndef TC_PIN_PATCH_ONLY
 /* TC_SERVICE_PIN_ORDER.  The panel draws three cached sequences, and the order
    of those elements *is* the order the player sees, so a Mod's order is
    applied to the live array here - before the panel walks it, and after the
    rebuild above (which puts the cache back into the panel's own order). */
 tc::pin_order::store().apply(context);
 /* Development aid.  TC_MODLOADER_PIN_ORDER_LOG=1 reports the first panels this
    hook sees, so a Mod that reports "nothing to order" can be told apart from a
    panel that never reached the hook; =dump or =2 adds the raw structure of the
    cached groups whenever that structure changes, which is what the layout in
    pin_order.hpp is read from (and what a rebuild that does not look like the
    measured one shows up as). */
 {
  wchar_t mode[16]{};
  const DWORD length=GetEnvironmentVariableW(L"TC_MODLOADER_PIN_ORDER_LOG",mode,16);
  if(length>0&&length<16){
   static int seen=0;
   if(seen++<3)log(std::string("IO panel pin order: build_io_state_view context=")+
                   std::to_string(reinterpret_cast<uintptr_t>(context)));
   /* The raw structure is dumped whenever it *changes* (at most 24 times), not
      only when asked for: a panel whose cached groups and the entries it draws
      disagree is exactly the report this dump answers, and asking a player to
      set an environment variable first is how the evidence gets lost. */
   const bool dumpMode=true;
   if(dumpMode){
    static std::string previous;
    static int dumps=0;
    const std::vector<std::string> lines=tc::pin_order::store().describe(context);
    bool hasEntries=false;
    for(const std::string& line:lines)
     if(line.find("count=")!=std::string::npos&&line.find("count=0")==std::string::npos)
      hasEntries=true;
    /* The shape, not the order: a rebuild with the same pins must not print
       again, but one that adds, drops or re-lays a group must. */
    std::string shape;
    for(const std::string& line:lines){
     const size_t at=line.find(" group ");
     if(at!=std::string::npos)shape+=line.substr(at)+"\n";
    }
    if(hasEntries&&shape!=previous&&dumps<24){
     previous=shape;
     ++dumps;
     for(const std::string& line:lines)log(line);
    }
   }
  }
 }
#endif
 if(buildIoStateViewOriginal)buildIoStateViewOriginal(board,context,height,layout,scale);
}
#endif
/* Text is drawn from more than one thread in this build, so the hook only
   records; a frame's own thread flushes the record. */
struct TextTraceRow {std::array<char,208> text{};};
static TextTraceRow textTraceRows[48];
static std::atomic<int> textTraceWritten{0};
static int textTraceRead=0;
/* The game's own panels do not draw text with ImGui: they build a "markup text"
   object first (to_markup_text, executable RVA 0x3690a0 - it simply copies a Nim
   string into the markup value, so its two arguments are a string pointer and an
   output pointer).  Logging it says which panel a piece of text comes from and
   whether it is the markup renderer that will draw it. */
static constexpr uintptr_t TC_TO_MARKUP_TEXT_RVA=0x3690a0;
static void (*toMarkupTextOriginal)(const void*,void*)=nullptr;
static void hookToMarkupText(const void* text,void* out){
 if(traceLabel){
  const auto length=text?*static_cast<const unsigned long long*>(text):0;
  const char* data=text?*reinterpret_cast<const char* const*>(static_cast<const char*>(text)+8):nullptr;
  if(data&&length>0&&length<512&&readableBytes(data,(size_t)length)){
   const bool dumpAll=!*traceLabel;
   /* Bounded: this is called for every piece of panel text on every frame. */
   static int written=0;
   if(written<300&&(dumpAll||std::string_view(data,(size_t)length).find(traceLabel)!=
                    std::string_view::npos)){
    ++written;
    const auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);
    const int slot=textTraceWritten.fetch_add(1)%(int)(sizeof(textTraceRows)/sizeof(textTraceRows[0]));
    std::snprintf(textTraceRows[slot].text.data(),textTraceRows[slot].text.size(),
                  "Markup \"%.*s\" caller=0x%llx",
                  (int)(length>60?60:length),data,(unsigned long long)rva);
   }
  }
 }
 if(toMarkupTextOriginal)toMarkupTextOriginal(text,out);
}
static void flushTextTrace(){
 if(!traceText)return;
 const int written=textTraceWritten.load();
 while(textTraceRead<written){
  const int slot=textTraceRead%(int)(sizeof(textTraceRows)/sizeof(textTraceRows[0]));
  ++textTraceRead;
  if(textTraceRows[slot].text[0])log(std::string(textTraceRows[slot].text.data()));
 }
}
static void hookEndChild(){
 const auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);
 /* Learn where the game's own tool buttons are: each one is a child window, so
    while that child is still open its window rectangle is the button.  The
    bottom-most one tells plugin tools where to line up. */
 if(compatible&&nativeRuntime&&rva>=0x463000&&rva<0x46a000){
  auto getPos=reinterpret_cast<void(*)(void*)>(GetProcAddress(engine,"igGetWindowPos"));
  auto getSize=reinterpret_cast<void(*)(void*)>(GetProcAddress(engine,"igGetWindowSize"));
  struct V2L{float x,y;};
  if(getPos&&getSize){
   V2L pos{},size{};
   getPos(&pos);getSize(&size);
   if(size.x>32.f&&size.x<200.f&&size.y>32.f&&size.y<200.f&&pos.x>0.f&&pos.y>0.f)
    nativeRuntime->noteToolColumn(pos.x,pos.y+size.y);
  }
 }
 /* Development aid: list the call sites once, so the tool column's own child
    end can be told apart from every other igEndChild in the game. */
 if(GetEnvironmentVariableW(L"TC_MODLOADER_LOG_CHILD",nullptr,0)>0){
  static std::set<uintptr_t> seen;
  if(seen.size()<64&&seen.insert(rva).second){
   char text[64];std::snprintf(text,sizeof(text),"0x%llx",(unsigned long long)rva);
   std::string where="igEndChild from rva="+std::string(text);
   /* Geometry of the child that is ending: which call site wraps which area is
      the whole question when picking an injection point. */
   try{
    HWND window=GetForegroundWindow();
    if(window&&nativeRuntime){
     struct V2L{float x,y;};
     auto cursorScreen=reinterpret_cast<void(*)(V2L*)>(GetProcAddress(engine,"igGetCursorScreenPos"));
     auto getWidth=reinterpret_cast<float(*)()>(GetProcAddress(engine,"igGetWindowWidth"));
     auto getHeight=reinterpret_cast<float(*)()>(GetProcAddress(engine,"igGetWindowHeight"));
     V2L pos{};
     if(cursorScreen)cursorScreen(&pos);
     where+=" cursor="+std::to_string((int)pos.x)+","+std::to_string((int)pos.y);
     if(getWidth&&getHeight)where+=" window="+std::to_string((int)getWidth())+"x"+std::to_string((int)getHeight());
    }
   }catch(...){}
   log(where);
  }
 }
 /* The original runs first: the tool is drawn *after* that child closes, so it
   lands in the column's own window (right below the game's last tool) instead of
   inside the tile that is ending. */
 if(endChildOriginal)endChildOriginal();
 if(compatible&&nativeRuntime&&rva==TC_BOARD_TOOLBAR_END_RVA){
  try{nativeRuntime->boardToolFrame();}catch(const std::exception&e){log(std::string("Board tool error: ")+e.what());}catch(...){}
}
}
/* The top menu bar (TC_UI_SLOT_BOARD_MENU).  build_buttons draws the bar's own
   entries with a style it pushes for the row: frame padding, rounding, item
   spacing, window border hover padding and the three button colours.  The first
   thing it does after the loop is pop that style (igPopStyleColor, RVA
   0x45a43f), and that point is *unconditional* - unlike the label of the last,
   optional "return to level" button, which is where the first attempt hooked and
   never fired in a plain level.  The plugin controls are therefore drawn from the
   pop's call site, before the pop runs, so they sit inside exactly that style
   scope and - with the SameLine the runtime submits first - on the bar's own
   row. */
static void (*popStyleColorOriginal)(int)=nullptr;
static void hookPopStyleColor(int count){
 const auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);
 /* Development aid: list the style pops the menu bar itself makes, so the
    unconditional one can be told apart from the optional buttons' labels. */
 if(GetEnvironmentVariableW(L"TC_MODLOADER_LOG_MENU",nullptr,0)>0&&rva>=0x459000&&rva<0x45d000){
  static std::set<uintptr_t> seen;
  if(seen.size()<32&&seen.insert(rva).second){
   char text[64];std::snprintf(text,sizeof(text),"0x%llx",(unsigned long long)rva);
   log(std::string("igPopStyleColor from rva=")+text);
  }
 }
 if(compatible&&nativeRuntime&&rva==TC_BOARD_MENU_END_RVA){
  try{nativeRuntime->boardMenuFrame();}catch(const std::exception&e){log(std::string("Board menu error: ")+e.what());}catch(...){}
 }
 if(popStyleColorOriginal)popStyleColorOriginal(count);
}
#if defined(TC_PIN_PATCH_ONLY) || defined(TC_PIN_PATCH_PLUGIN)
static bool pinPatchHooksReady=false;
static bool installPinPatchHook(void* target,void* detour,void** original){
 if(!target||!detour||!original)return false;
#ifdef TC_PIN_PATCH_PLUGIN
 return pinPatchHost&&pinPatchHost->create_hook&&
        pinPatchHost->create_hook(pinPatchHost->context,target,detour,original)==0;
#else
 const MH_STATUS initialized=MH_Initialize();
 if(initialized!=MH_OK&&initialized!=MH_ERROR_ALREADY_INITIALIZED)return false;
 return MH_CreateHook(target,detour,original)==MH_OK&&MH_EnableHook(target)==MH_OK;
#endif
}
#endif
static void init(){if(attempted)return;attempted=true;wchar_t buf[32768];GetModuleFileNameW(nullptr,buf,32768);gameRoot=tc::fs::path(buf).parent_path();
 try{engine=GetModuleHandleW(L"tc_game_engine.dll");if(!engine)throw std::runtime_error("Original engine is missing");
 compatible=tc::hash(tc::read(buf))==TC_EXE_SHA && tc::hash(tc::read(gameRoot/L"tc_game_engine.dll"))==TC_ENGINE_SHA;
if(!compatible)throw std::runtime_error("Unsupported game build. Reinstall a compatible loader.");
log("Compatibility profile: " TC_COMPAT_PROFILE_ID " (game " TC_GAME_VERSION ")");
#ifndef TC_PIN_PATCH_ONLY
 saves=std::make_unique<tc::SaveProfiles>(gameRoot);
 core=std::make_unique<tc::Core>(gameRoot);core->scan();selected=core->enabled_set();nativeRuntime=std::make_unique<tc::NativeRuntime>(*core,engine,saves->path(tc_save_boot::profile),[](const std::string& s){log(s);});log(std::string("TC Mod Loader ")+TC_MODLOADER_VERSION_STRING+"; ImGui "+api<const char*(*)()>("igGetVersion")());log("Isolated save directory: "+saves->path(tc_save_boot::profile).u8string());
 /* A session without the save redirect must not run native plugins: they write
    into the profile the game is using, and that profile would be the player's
    own.  Resource Mods are unaffected (they are file-level and applied from the
    Mods page), the game starts, and both the log and the Mods page say why. */
 if(saveRedirect.applied){
  log("Save isolation applied: "+saves->path(tc_save_boot::profile).u8string());
 }else{
  log(std::string("Save isolation unavailable: ")+saveRedirect.reason);
  log("This session runs with no save redirect and no native plugins; resource Mods still apply");
  nativeRuntime->forcedSafeMode=std::string("存档隔离不可用：")+saveRedirect.reason;
 }
#else
 /* The standalone patch: nothing but the component preview's pin names.  No mods
    are scanned or loaded, and the player's own save profile is left alone. */
#ifdef TC_PIN_PATCH_PLUGIN
 log(std::string("TC Pin Names Mod v0.0.4; ImGui ")+api<const char*(*)()>("igGetVersion")());
#else
 log(std::string("TC Pin Names patch; ImGui ")+api<const char*(*)()>("igGetVersion")());
#endif
#endif
#ifndef TC_PIN_PATCH_PLUGIN
 if(void* endChild=reinterpret_cast<void*>(GetProcAddress(engine,"igEndChild"))){
   /* The rest of the loader initialises MinHook a little later; doing it here
      too is fine (it reports "already initialised" once that happened). */
   const MH_STATUS mh=MH_Initialize();
   const MH_STATUS created=(mh==MH_OK||mh==MH_ERROR_ALREADY_INITIALIZED)
                               ?MH_CreateHook(endChild,reinterpret_cast<void*>(hookEndChild),reinterpret_cast<void**>(&endChildOriginal))
                               :mh;
   if(created==MH_OK&&MH_EnableHook(endChild)==MH_OK)log("Toolbar tool slot armed (igEndChild)");
  else log("Toolbar tool slot unavailable (igEndChild hook: "+std::string(MH_StatusToString(created))+")");
 }
 if(void* popStyleColor=reinterpret_cast<void*>(GetProcAddress(engine,"igPopStyleColor"))){
   const MH_STATUS mh=MH_Initialize();
   const MH_STATUS created=(mh==MH_OK||mh==MH_ERROR_ALREADY_INITIALIZED)
                               ?MH_CreateHook(popStyleColor,reinterpret_cast<void*>(hookPopStyleColor),reinterpret_cast<void**>(&popStyleColorOriginal))
                               :mh;
   if(created==MH_OK&&MH_EnableHook(popStyleColor)==MH_OK)log("Top menu bar slot armed (igPopStyleColor)");
  else log("Top menu bar slot unavailable (igPopStyleColor hook: "+std::string(MH_StatusToString(created))+")");
 }
#ifndef TC_PIN_PATCH_ONLY
 {
  /* The IO panel's own build/draw entry: the loader's one detour there keeps
     the cached IO states in step with edits made in the workshop, and applies a
     Mod's pin order (TC_SERVICE_PIN_ORDER).  Both are this loader's business,
     so no Mod has to hook this function - which is also why the pin-names Mod
     can no longer be refused for wanting to. */
  auto* base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
  void* reload=base+TC_RELOAD_CUSTOM_PROTOTYPE_RVA;
  void* buildView=base+TC_BUILD_IO_STATE_VIEW_RVA;
  const MH_STATUS mh=MH_Initialize();
  const bool ready=(mh==MH_OK||mh==MH_ERROR_ALREADY_INITIALIZED);
  const MH_STATUS reloadCreated=ready
      ?MH_CreateHook(reload,reinterpret_cast<void*>(hookReloadCustomPrototype),reinterpret_cast<void**>(&reloadCustomPrototypeOriginal))
      :mh;
  const MH_STATUS viewCreated=ready
      ?MH_CreateHook(buildView,reinterpret_cast<void*>(hookBuildIoStateView),reinterpret_cast<void**>(&buildIoStateViewOriginal))
      :mh;
  const bool reloadArmed=reloadCreated==MH_OK&&MH_EnableHook(reload)==MH_OK;
  const bool viewArmed=viewCreated==MH_OK&&MH_EnableHook(buildView)==MH_OK;
  if(viewArmed)log("IO panel armed (cache rebuild after a prototype reload + tc.pin_order)");
  else log("IO panel unavailable (build_io_state_view hook: "+std::string(MH_StatusToString(viewCreated))+")");
  if(!reloadArmed)
   log("IO panel cache rebuild unavailable (reload hook: "+std::string(MH_StatusToString(reloadCreated))+")");
 }
#endif
#ifdef TC_PIN_PATCH_ONLY
 /* The standalone patch has no Mod loader and no pin-order service, so it
    installs the same two panel hooks itself. */
 {
  auto* base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
  void* reload=base+TC_RELOAD_CUSTOM_PROTOTYPE_RVA;
  void* buildView=base+TC_BUILD_IO_STATE_VIEW_RVA;
  const bool reloadReady=installPinPatchHook(
   reload,reinterpret_cast<void*>(hookReloadCustomPrototype),
   reinterpret_cast<void**>(&reloadCustomPrototypeOriginal));
  const bool viewReady=installPinPatchHook(
   buildView,reinterpret_cast<void*>(hookBuildIoStateView),
   reinterpret_cast<void**>(&buildIoStateViewOriginal));
  if(reloadReady&&viewReady)
   log("IO panel cache rebuild armed (input and output)");
  else
   log(std::string("IO panel cache rebuild unavailable (reload=")+
       (reloadReady?"ok":"failed")+", view="+(viewReady?"ok":"failed")+")");
  pinPatchHooksReady=pinPatchHooksReady&&reloadReady&&viewReady;
 }
#endif
 /* The text trace is armed before anything draws, so the first text the game
    submits in the tool column is already covered. */
 {
  wchar_t buffer[256]{};
  const DWORD length=GetEnvironmentVariableW(L"TC_MODLOADER_TRACE_TEXT",buffer,256);
  if(length>0&&length<256){
   static std::string needle;needle=std::string(buffer,buffer+length);
   /* "*" dumps the first calls instead of filtering, for finding which path
      draws a piece of text. */
   if(needle=="*")needle.clear();
   traceText=needle.c_str();
   void* renderText=reinterpret_cast<void*>(GetProcAddress(engine,"igRenderText"));
   if(renderText&&MH_CreateHook(renderText,reinterpret_cast<void*>(hookRenderText),
                                reinterpret_cast<void**>(&renderTextOriginal))==MH_OK&&
      MH_EnableHook(renderText)==MH_OK)
    log("Text trace armed for \""+needle+"\" (igRenderText)");
   else
    log("Text trace unavailable (igRenderText hook failed)");
   void* addText=reinterpret_cast<void*>(GetProcAddress(engine,"ImDrawList_AddText_Vec2"));
   if(addText&&MH_CreateHook(addText,reinterpret_cast<void*>(hookAddText),
                             reinterpret_cast<void**>(&addTextOriginal))==MH_OK&&
      MH_EnableHook(addText)==MH_OK)
    log("Draw-list text trace armed for \""+needle+"\" (ImDrawList_AddText_Vec2)");
   else
    log("Draw-list text trace unavailable (ImDrawList_AddText_Vec2 hook failed)");
   void* textEx=reinterpret_cast<void*>(GetProcAddress(engine,"igTextEx"));
   if(textEx&&MH_CreateHook(textEx,reinterpret_cast<void*>(hookTextEx),
                            reinterpret_cast<void**>(&textExOriginal))==MH_OK&&
      MH_EnableHook(textEx)==MH_OK)
    log("TextEx trace armed for \""+needle+"\" (igTextEx)");
   else
    log("TextEx trace unavailable (igTextEx hook failed)");
   /* Internal ImGui text entry point (not an export): see the note above
      hookTextUnformatted. */
   void* textUn=engine?reinterpret_cast<void*>(reinterpret_cast<unsigned char*>(engine)+0xfb2d0):nullptr;
   if(textUn&&readableBytes(textUn,16)&&
      MH_CreateHook(textUn,reinterpret_cast<void*>(hookTextUnformatted),
                    reinterpret_cast<void**>(&textUnformattedOriginal))==MH_OK&&
      MH_EnableHook(textUn)==MH_OK)
    log("Text trace armed for \""+needle+"\" (ImGui TextEx internal, engine+0xfb2d0)");
   else
    log("Text trace unavailable (ImGui TextEx internal hook failed)");
  }
  wchar_t label[256]{};
  const DWORD labelLength=GetEnvironmentVariableW(L"TC_MODLOADER_TRACE_LABEL",label,256);
  /* Development aid: TC_MODLOADER_TRACE_LABEL=<substring> logs the label-mesh
     submissions ("*" = the first ones of every call site).  Only armed when the
     variable is set, so a normal run pays nothing. */
  if(labelLength>0&&labelLength<256){
   static std::string needle;
   needle.clear();
   if(labelLength>0&&labelLength<256){
    needle=std::string(label,label+labelLength);
    /* "*" means "log every label" - used to learn the call's argument layout
       before a substring filter can be trusted. */
    if(needle=="*")needle.clear();
   }
   traceLabel=needle.c_str();
   /* The game's own panels hand their text to the markup renderer through this
      conversion, so hooking it shows which panel draws what. */
   HMODULE markupExe=GetModuleHandleW(nullptr);
   void* toMarkup=markupExe?reinterpret_cast<void*>(reinterpret_cast<unsigned char*>(markupExe)+TC_TO_MARKUP_TEXT_RVA):nullptr;
   if(toMarkup&&readableBytes(toMarkup,8)&&
      MH_CreateHook(toMarkup,reinterpret_cast<void*>(hookToMarkupText),
                    reinterpret_cast<void**>(&toMarkupTextOriginal))==MH_OK&&
      MH_EnableHook(toMarkup)==MH_OK)
    log("Markup text trace armed for \""+needle+"\" (to_markup_text)");
   else
    log("Markup text trace unavailable (to_markup_text hook failed)");
   HMODULE exe=GetModuleHandleW(nullptr);
   void* setText=exe?reinterpret_cast<void*>(reinterpret_cast<unsigned char*>(exe)+TC_LABEL_SET_TEXT_RVA):nullptr;
   void* fadingText=exe?reinterpret_cast<void*>(reinterpret_cast<unsigned char*>(exe)+TC_FADING_LABEL_SET_TEXT_RVA):nullptr;
   if(setText&&MH_CreateHook(setText,reinterpret_cast<void*>(hookSetText),
                             reinterpret_cast<void**>(&setTextOriginal))==MH_OK&&
      MH_EnableHook(setText)==MH_OK)
    log("Label trace armed for \""+needle+"\" (label_mesh.set_text)");
   else
    log("Label trace unavailable (label_mesh.set_text hook failed)");
   static void (*fadingOriginal)(void*,void*,const void*,void*,void*,void*,void*)=nullptr;
   if(fadingText&&MH_CreateHook(fadingText,reinterpret_cast<void*>(hookSetText),
                                reinterpret_cast<void**>(&fadingOriginal))==MH_OK&&
      MH_EnableHook(fadingText)==MH_OK)
    log("Fading label trace armed for \""+needle+"\" (fading_label_mesh.set_text)");
  else
   log("Fading label trace unavailable (fading_label_mesh.set_text hook failed)");
  }
 }
#endif
#if defined(TC_PIN_PATCH_ONLY) || defined(TC_PIN_PATCH_PLUGIN)
/* The component preview's pin names (see hookPreviewText): always armed, since
   this is the feature rather than a probe. */
 {
 wchar_t table[32]{};
 const DWORD tableLength=GetEnvironmentVariableW(L"TC_MODLOADER_PIN_TABLE",table,32);
 if(tableLength>0&&tableLength<32){
  const int percent=_wtoi(table);
  previewLabelNames=percent>0;
  if(percent>0)previewLabelTableScale=std::min(1.f,std::max(0.45f,percent/100.f));
 }
  void* text=reinterpret_cast<void*>(GetProcAddress(engine,"igText"));
  static void (*trampoline)(const char*,...)=nullptr;
  const bool textReady=installPinPatchHook(text,reinterpret_cast<void*>(hookPreviewText),
                                           reinterpret_cast<void**>(&trampoline));
  if(textReady){
   previewTextV=reinterpret_cast<void(*)(const char*,va_list)>(GetProcAddress(engine,"igTextV"));
   previewTextUnformatted=reinterpret_cast<void(*)(const char*,const char*)>(GetProcAddress(engine,"igTextUnformatted"));
   previewCalcTextSize=reinterpret_cast<void(*)(V2*,const char*,const char*,bool,float)>(GetProcAddress(engine,"igCalcTextSize"));
   previewGetCursorScreenPos=reinterpret_cast<void(*)(V2*)>(GetProcAddress(engine,"igGetCursorScreenPos"));
   previewGetWindowPos=reinterpret_cast<void(*)(V2*)>(GetProcAddress(engine,"igGetWindowPos"));
   previewGetWindowSize=reinterpret_cast<void(*)(V2*)>(GetProcAddress(engine,"igGetWindowSize"));
   previewSetCursorScreenPos=reinterpret_cast<void(*)(V2)>(GetProcAddress(engine,"igSetCursorScreenPos"));
   previewGetCurrentWindow=reinterpret_cast<void*(*)()>(GetProcAddress(engine,"igGetCurrentWindow"));
   setWindowFontScaleOriginal=reinterpret_cast<void(*)(float)>(GetProcAddress(engine,"igSetWindowFontScale"));
   previewPushStyleColor=reinterpret_cast<void(*)(int,V4)>(GetProcAddress(engine,"igPushStyleColor_Vec4"));
   previewPopStyleColor=reinterpret_cast<void(*)(int)>(GetProcAddress(engine,"igPopStyleColor"));
   log(std::string("Component preview pin table ")+(previewLabelNames?"armed":"off")+
       " (table text "+std::to_string((int)(previewLabelTableScale*100.f))+"%)");
  }
  else
   log("Component preview pin names unavailable (igText hook failed)");
  /* The pin bars of that same preview (see hookAddRectFilled). */
  void* addRect=reinterpret_cast<void*>(GetProcAddress(engine,"ImDrawList_AddRectFilled"));
  const bool barsReady=installPinPatchHook(addRect,reinterpret_cast<void*>(hookAddRectFilled),
                                           reinterpret_cast<void**>(&addRectFilledOriginal));
  if(barsReady)
   log("Component preview pin bars armed (thinner and shorter)");
 else
   log("Component preview pin bars unavailable (ImDrawList_AddRectFilled hook failed)");
  pinPatchHooksReady=textReady&&barsReady;
 }
 #endif
}catch(const std::exception& e){error=e.what();log(error);
 /* Anything that got this far is a loader/game mismatch the player has to know
    about, and in-game pages cannot be trusted to appear (see the note above
    reportUnsupportedBuild). */
 reportUnsupportedBuild(error);}}
static void text(const std::string& s){api<void(*)(const char*,const char*)>("igTextUnformatted")(s.c_str(),nullptr);}
static bool button(const char* label,V2 size={0,0}){return api<bool(*)(const char*,V2)>("igButton")(label,size);}
static void line(){api<void(*)()>("igSeparator")();}
static void same(){api<void(*)(float,float)>("igSameLine")(0,-1);}
/* One line per frame with the home-page button rectangles.  The UI playtest
   driver uses it to click the real hit box instead of guessing coordinates;
   it is one log line per frame and only while the menu is drawn. */
static float pageGameWidth=0,pageGameHeight=0,pageWindowWidth=0,pageWindowHeight=0;
static void logButtonRect(const char* label,float x,float y,float w,float h){
 static int frames=0;if(frames++>8)return;
 log(std::string("home button ")+label+" x="+std::to_string((int)x)+" y="+std::to_string((int)y)+" w="+std::to_string((int)w)+" h="+std::to_string((int)h)+" game="+std::to_string((int)pageGameWidth)+"x"+std::to_string((int)pageGameHeight)+" window="+std::to_string((int)pageWindowWidth)+"x"+std::to_string((int)pageWindowHeight));
}
/* The game's own window: the first visible top-level window of this process
   that is not one of ours (the proxy's own helper windows are not created
   yet at this point, but the class name filter keeps it honest). */
static HWND gameWindowHandle(){
 static HWND cached=nullptr;if(cached&&IsWindow(cached))return cached;
 struct Finder{DWORD process;HWND found;} finder{GetCurrentProcessId(),nullptr};
 EnumWindows([](HWND window,LPARAM data)->BOOL{auto& f=*(Finder*)data;DWORD owner=0;GetWindowThreadProcessId(window,&owner);if(owner!=f.process||!IsWindowVisible(window))return TRUE;RECT r{};if(!GetClientRect(window,&r)||r.right<320||r.bottom<240)return TRUE;f.found=window;return FALSE;},(LPARAM)&finder);
 cached=finder.found;return cached;
}

/* ---- Mods manager UI -----------------------------------------------------
   Plain ImGui calls from the engine's own export table (every one of them was
   checked against the pinned build's exports before use).  Nothing here draws
   the game's window: the loader only describes controls, the game renders them
   in its own frame. */
static void pushStyleColor(int index,V4 color){api<void(*)(int,V4)>("igPushStyleColor_Vec4")(index,color);}
static void popStyleColor(int count=1){api<void(*)(int)>("igPopStyleColor")(count);}
static void textColor(V4 color,const std::string& s){pushStyleColor(0,color);text(s);popStyleColor();}
static void textDim(const std::string& s){pushStyleColor(0,V4{0.60f,0.63f,0.70f,1.f});text(s);popStyleColor();}
static void textWrapped(const std::string& s){api<void(*)(float)>("igPushTextWrapPos")(0);text(s);api<void(*)()>("igPopTextWrapPos")();}
static bool selectableRow(const std::string& label,bool selected,float width){return api<bool(*)(const char*,bool,int,V2)>("igSelectable_Bool")(label.c_str(),selected,0,V2{width,0});}
static void spacing(){api<void(*)()>("igSpacing")();}
static bool collapsing(const char* label){return api<bool(*)(const char*,int)>("igCollapsingHeader_TreeNodeFlags")(label,0);}
static V4 colGood(){return V4{0.40f,0.82f,0.50f,1.f};}
/* Profile ids and paths the page shows as byte strings: they are ASCII by
   construction (tc_save_boot::valid), so a wide-to-narrow copy is exact. */
static std::string narrow(const wchar_t* text){std::string out;for(;text&&*text;++text)out+=(char)*text;return out;}
static V4 colWarn(){return V4{0.95f,0.74f,0.34f,1.f};}
static V4 colBad(){return V4{0.94f,0.42f,0.42f,1.f};}
static V4 colInfo(){return V4{0.45f,0.72f,0.98f,1.f};}
/* Defined with the Mod manager but drawn after it, from `draw()` itself. */
static void savesManagerUi(float scale,bool& requestClose);

/* Development helper: dump what the game is showing (the GL backbuffer) as a
   32-bit BMP, so the loader's own pages can actually be looked at.  Outside
   captures of this window come back black - it runs fullscreen in independent
   flip mode - so the only way to see a page is from inside the process.
   Enabled with TC_MODLOADER_SHOT=<file.bmp>; the picture is taken a few frames
   after the manager opens. */
static void writeLe32(unsigned char* out,unsigned value){out[0]=(unsigned char)(value&0xff);out[1]=(unsigned char)((value>>8)&0xff);out[2]=(unsigned char)((value>>16)&0xff);out[3]=(unsigned char)((value>>24)&0xff);}
static bool captureFrame(const wchar_t* path){
 try{
  GLint viewport[4]{};
  glGetIntegerv(GL_VIEWPORT,viewport);
  const int width=viewport[2],height=viewport[3];
  if(width<=0||height<=0||width>8192||height>8192)return false;
  std::vector<unsigned char> pixels((size_t)width*height*4);
  glPixelStorei(GL_PACK_ALIGNMENT,1);
  glReadPixels(viewport[0],viewport[1],width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
  std::FILE* file=_wfopen(path,L"wb");
  if(!file)return false;
  const unsigned pixelBytes=(unsigned)width*(unsigned)height*4u,offset=54u;
  unsigned char header[54]{};
  header[0]='B';header[1]='M';
  writeLe32(header+2,offset+pixelBytes);
  writeLe32(header+10,offset);
  writeLe32(header+14,40);
  writeLe32(header+18,(unsigned)width);
  writeLe32(header+22,(unsigned)(-height));
  header[26]=1;header[28]=32;
  writeLe32(header+34,pixelBytes);
  std::fwrite(header,1,sizeof(header),file);
  std::vector<unsigned char> row((size_t)width*4u);
  for(int y=0;y<height;++y){
   const unsigned char* source=pixels.data()+(size_t)(height-1-y)*width*4u;
   for(int x=0;x<width;++x){row[x*4+0]=source[x*4+2];row[x*4+1]=source[x*4+1];row[x*4+2]=source[x*4+0];row[x*4+3]=255;}
   std::fwrite(row.data(),1,row.size(),file);
  }
  std::fclose(file);
  return true;
 }catch(...) {return false;}
}

/* Timed capture.  The menu page's draw() only runs while the home page is up,
   so anything to do with a level (board panels, the circuit) has to be captured
   from a hook that runs every frame - igEnd is one.  TC_MODLOADER_SHOT=<file>
   plus TC_MODLOADER_SHOT_DELAY=<ms> (default 15000). */
static void devShotTick(){
 static bool init=false,done=false;
 static const wchar_t* path=nullptr;
 static unsigned long long start=0,delay=15000;
 if(done)return;
 if(!init){
  init=true;start=GetTickCount64();
  wchar_t buffer[1024]{};
  const DWORD length=GetEnvironmentVariableW(L"TC_MODLOADER_SHOT",buffer,1024);
  if(length>0&&length<1024)path=_wcsdup(buffer);
  wchar_t delayText[32]{};
  if(GetEnvironmentVariableW(L"TC_MODLOADER_SHOT_DELAY",delayText,32)>0)delay=_wtoi(delayText);
 }
 if(path&&GetTickCount64()-start>=delay){done=true;log(captureFrame(path)?"Captured frame screenshot":"Screenshot failed");}
}

/* Two panes: the package list on the left, the picked package on the right.
   Everything the old page showed is still here - it is just laid out instead of
   stacked as one column of text. */
static void modsManagerUi(float scale, bool& requestClose, bool& openSaves){
 static std::string picked;
 const float width=api<float(*)()>("igGetWindowWidth")();
 const float footer=118.f*scale;
 const float toolbar=34.f*scale;
 int broken=0;
 if(core)for(auto& m:core->mods)if(!m.error.empty())++broken;
 /* Header */
 textColor(colInfo(),std::string("TC MOD LOADER  ")+TC_MODLOADER_VERSION_STRING);
 same();textDim("   NATIVE API 1");
 if(core){same();textDim("         "+std::to_string(core->mods.size())+" 个 Mod · 已勾选 "+std::to_string(selected.size())+" · 异常 "+std::to_string(broken));}
 textDim("把 .mod 放进游戏目录的 mods 文件夹，重新打开此页即可识别；勾选后点「应用更改」，重启游戏才生效。");
 /* The two ways this session can run without native plugins, both said out loud
    instead of leaving the player with a list of Mods that never loads. */
 if(!saveRedirect.applied)
  textColor(colBad(),std::string("存档隔离不可用：")+saveRedirect.reason+
                    "。本次运行没有重定向存档、也没有加载任何原生插件（资源 Mod 仍生效）。"
                    "这通常意味着游戏更新了：请重新安装与当前版本匹配的加载器。");
 else if(nativeRuntime&&!nativeRuntime->forcedSafeMode.empty())
  textColor(colWarn(),nativeRuntime->forcedSafeMode+"。本次运行未加载原生插件。");
 spacing();
 /* Toolbar */
 if(button("刷新列表",{92.f*scale,toolbar})){try{if(core){core->scan();selected=core->enabled_set();picked.clear();}error.clear();}catch(const std::exception&e){error=e.what();}}
 same();if(button("打开文件夹",{104.f*scale,toolbar}))ShellExecuteW(nullptr,L"open",(gameRoot/L"mods").c_str(),nullptr,nullptr,SW_SHOWNORMAL);
 same();if(button("全部启用",{88.f*scale,toolbar})){if(core)for(auto& m:core->mods)if(m.error.empty())selected.insert(m.id);}
 same();if(button("全部停用",{88.f*scale,toolbar})){if(core)for(auto& m:core->mods)selected.erase(m.id);}
 line();
 /* Panes */
 const float listWidth=width*(core&&!core->mods.empty()?0.46f:0.99f);
 const float paneHeight=-footer;
 /* Something to look at on open: preselect the first package so the right pane
    is useful immediately instead of showing a hint. */
 if(picked.empty()&&core&&!core->mods.empty())picked=core->mods.front().source.u8string();
 if(api<bool(*)(const char*,V2,int,int)>("igBeginChild_Str")("TCModList",{listWidth,paneHeight},0,0)){
  if(!core||core->mods.empty())textDim("还没有 Mod。把 .mod 文件放进 mods 文件夹后按「刷新列表」。");
  else for(auto& m:core->mods){
   api<void(*)(const char*)>("igPushID_Str")(m.source.u8string().c_str());
   const bool on=selected.count(m.id)>0;
   const float box=26.f*scale;
   bool value=on;
   if(api<bool(*)(const char*,bool*)>("igCheckbox")("##on",&value)){if(value)selected.insert(m.id);else selected.erase(m.id);}
   same();
   std::string label=(m.error.empty()?"":"!  ")+m.name;
   if(!m.version.empty())label+="   "+m.version;
   const bool isPicked=picked==m.source.u8string();
   if(m.error.empty())pushStyleColor(0,on?colGood():V4{0.72f,0.74f,0.80f,1.f});
   else pushStyleColor(0,colBad());
   if(selectableRow(label,isPicked,listWidth-box-24.f*scale))picked=m.source.u8string();
   popStyleColor();
   if(!m.error.empty()){same();textColor(colBad(),"  包有问题");}
   api<void(*)()>("igPopID")();
  }
  /* Packages that are still enabled but whose .mod file is gone. */
  if(core)for(auto& id:core->enabled_set()){
   bool found=false;for(auto& m:core->mods)if(m.id==id)found=true;
   if(found)continue;
   api<void(*)(const char*)>("igPushID_Str")(id.c_str());
   bool on=selected.count(id)>0;
   if(api<bool(*)(const char*,bool*)>("igCheckbox")("##missing",&on)){if(on)selected.insert(id);else selected.erase(id);}
   same();textColor(colWarn(),"缺失的包："+id);
   textDim("    文件已不在 mods 文件夹；取消勾选并「应用更改」可还原它改过的文件。");
   api<void(*)()>("igPopID")();
  }
  api<void(*)()>("igEndChild")();
 }
 same();
 if(api<bool(*)(const char*,V2,int,int)>("igBeginChild_Str")("TCModDetails",{0,paneHeight},0,0)){
  const tc::Mod* m=nullptr;
  if(core&&!picked.empty())for(auto& candidate:core->mods)if(candidate.source.u8string()==picked)m=&candidate;
  if(!m)textDim("在左侧点一个 Mod，这里显示它的详细信息。");
  else {
   textColor(colInfo(),m->name);
   textDim(m->id+(m->version.empty()?"":"   v"+m->version)+(m->author.empty()?"":"   by "+m->author));
   spacing();
   const bool on=selected.count(m->id)>0;
   textColor(on?colGood():V4{0.72f,0.74f,0.80f,1.f},on?"● 已勾选（重启后启用）":"○ 未勾选");
   std::string runtime="本次运行：";
   /* A plugin that reported its own state (host->report_status) shows up here
      with the severity it chose, in the same place as a loader-detected
      failure.  -1 means the line came from the loader itself. */
   int runtimeLevel=-1;
   if(!m->entry.empty()){
    auto it=nativeRuntime->statuses.find(m->id);
    auto level=nativeRuntime->statusLevels.find(m->id);
    if(level!=nativeRuntime->statusLevels.end())runtimeLevel=level->second;
    const std::string status=it==nativeRuntime->statuses.end()?(core->enabled(m->id)?"已在本次运行加载":"本次未加载"):it->second;
    runtime+=status;
   } else runtime+="资源包（只改资源文件）";
   if(runtimeLevel>=0)pushStyleColor(0,runtimeLevel>=2?colBad():runtimeLevel==1?colWarn():colInfo());
   textWrapped(runtime);
   if(runtimeLevel>=0)popStyleColor();
   spacing();
   pushStyleColor(0,V4{0.60f,0.63f,0.70f,1.f});
   textWrapped(std::string(m->entry.empty()?"资源 Mod":"原生代码 Mod")+"   ·   "+std::to_string(m->files.size())+" 个资源文件"+
               (m->native.empty()?"":"   ·   "+std::to_string(m->native.size())+" 个原生文件"));
   textWrapped("包："+m->source.filename().u8string()+"   ·   SHA "+m->digest.substr(0,12));
   popStyleColor();
   if(!m->dependencies.empty()||!m->optional.empty()){
    std::string deps;for(auto& d:m->dependencies){deps+=(deps.empty()?"":"、")+d;auto c=tc::constraint_of(m->required_versions,d);if(!c.empty())deps+=" "+c;}
    std::string opts;for(auto& d:m->optional){opts+=(opts.empty()?"":"、")+d;auto c=tc::constraint_of(m->optional_versions,d);if(!c.empty())opts+=" "+c;}
    pushStyleColor(0,V4{0.60f,0.63f,0.70f,1.f});
    if(!deps.empty())textWrapped("依赖："+deps+(opts.empty()?"":"   ·   可选："+opts));
    else textWrapped("可选依赖："+opts);
    if(!m->capabilities.empty()){std::string caps;for(auto& c:m->capabilities)caps+=(caps.empty()?"":"、")+c;textWrapped("需要加载器能力："+caps);}
    popStyleColor();
   }
   if(!m->description.empty()){spacing();textWrapped(m->description);}
   if(!m->error.empty()){spacing();textColor(colBad(),"这个包无法启用：");textWrapped(m->error);}
   spacing();
   if(button(on?"从启用列表移除":"加入启用列表",{150.f*scale,toolbar})){if(on)selected.erase(m->id);else selected.insert(m->id);}
   same();if(button("打开文件夹",{110.f*scale,toolbar}))ShellExecuteW(nullptr,L"open",m->source.parent_path().c_str(),nullptr,nullptr,SW_SHOWNORMAL);
  }
  api<void(*)()>("igEndChild")();
 }
  line();
  /* Save profiles moved out of this page into their own main-menu entry: one
     line here keeps the old path discoverable without two pages maintaining the
     same list. */
  if(saves){
   textDim("存档（已隔离）："+narrow(tc_save_boot::profile));
   same();if(button("存档管理 →",{112.f*scale,toolbar}))openSaves=true;
  }
 /* Footer: unsaved hint + the two actions + one status line. */
 if(core&&selected!=core->enabled_set())textColor(colWarn(),"有未保存的勾选：点「应用更改」后重启游戏生效。");
 if(button("应用更改",{120.f*scale,toolbar})){try{if(!core)throw std::runtime_error("Loader backend unavailable");core->apply(selected);selected=core->enabled_set();core->notice="更改已保存。请关闭并重新启动游戏，让所有修改完整生效。";error.clear();log("Applied selected Mods");}catch(const std::exception&e){error=e.what();log(error);}}
 same();if(button("关闭",{96.f*scale,toolbar}))requestClose=true;
 if(!error.empty())textColor(colBad(),"操作未完成："+error);
 else if(core)textDim(core->notice);
}
static void draw(){
 /* How this process relates to the display, logged once at boot.  A DPI-aware
    manifest decides whether Windows virtualises the game's coordinates (an app
    without one is scaled as a bitmap and the OS rewrites mouse coordinates into
    its space) or whether the game has to handle per-monitor changes itself.
    The pinned build ships no DPI manifest, so this line is where that fact - and
    the surface/client ratio the playtests rely on - is recorded for anyone
    looking at a scaled or multi-monitor setup. */
 static bool displayLogged=false;
 if(!displayLogged){
  displayLogged=true;
  try{
   using GetProcessDpiAwarenessFn=HRESULT(WINAPI*)(HANDLE,DPI_AWARENESS*);
   using GetDpiForWindowFn=UINT(WINAPI*)(HWND);
   static GetProcessDpiAwarenessFn getAwareness=nullptr;static GetDpiForWindowFn getDpi=nullptr;static bool resolved=false;
   if(!resolved){resolved=true;
    if(HMODULE shcore=LoadLibraryW(L"shcore.dll"))getAwareness=(GetProcessDpiAwarenessFn)GetProcAddress(shcore,"GetProcessDpiAwareness");
    if(HMODULE user32=GetModuleHandleW(L"user32.dll"))getDpi=(GetDpiForWindowFn)GetProcAddress(user32,"GetDpiForWindow");}
   std::string awareness="unknown";
   if(getAwareness){DPI_AWARENESS value=DPI_AWARENESS_INVALID;
    if(getAwareness(nullptr,&value)==S_OK){
     awareness=value==DPI_AWARENESS_UNAWARE?"unaware":value==DPI_AWARENESS_SYSTEM_AWARE?"system-aware":
      value==DPI_AWARENESS_PER_MONITOR_AWARE?"per-monitor-aware":"other";}}
   struct Counter{DWORD process;int monitors;} counter{GetCurrentProcessId(),0};
   EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR,HDC,LPRECT,LPARAM data)->BOOL{
    ++reinterpret_cast<Counter*>(data)->monitors;return TRUE;},(LPARAM)&counter);
   HWND window=gameWindowHandle();UINT dpi=0;RECT client{};
   if(window){if(getDpi)dpi=getDpi(window);GetClientRect(window,&client);}
   const int surfaceWidth=(int)api<float(*)()>("igGetWindowWidth")(),surfaceHeight=(int)api<float(*)()>("igGetWindowHeight")();
   const float ratioX=client.right>0?(float)client.right/(float)(surfaceWidth>0?surfaceWidth:1):1.f;
   const float ratioY=client.bottom>0?(float)client.bottom/(float)(surfaceHeight>0?surfaceHeight:1):1.f;
   log("Display: dpi-awareness="+awareness+" window-dpi="+std::to_string((int)dpi)+" monitors="+
       std::to_string(counter.monitors)+" screen="+std::to_string((int)GetSystemMetrics(SM_CXSCREEN))+"x"+
       std::to_string((int)GetSystemMetrics(SM_CYSCREEN))+" client="+std::to_string((int)client.right)+"x"+
       std::to_string((int)client.bottom)+" surface="+std::to_string(surfaceWidth)+"x"+
       std::to_string(surfaceHeight)+" ratio="+std::to_string(ratioX)+","+std::to_string(ratioY));
  }catch(...){}
 }
 auto setpos=api<void(*)(V2)>("igSetCursorPos");
 /* Both coordinate spaces: the game's ImGui window is not the OS window, so
    tests that have to click a button need the ratio between them. */
 const float gameWidth=api<float(*)()>("igGetWindowWidth")(),gameHeight=api<float(*)()>("igGetWindowHeight")();
 HWND gameWindow=gameWindowHandle();RECT client{};if(gameWindow)GetClientRect(gameWindow,&client);
 /* The OS client size, which is what a PostMessage-based click has to use:
    the game's ImGui surface can be larger than its window (the machine here
    draws 2560x1600 into a 1462x914 window).  Logging both makes that ratio
    recoverable for tests instead of guessing it. */
 const float windowWidth=client.right>0?(float)client.right:gameWidth,windowHeight=client.bottom>0?(float)client.bottom:gameHeight;
 pageGameWidth=gameWidth;pageGameHeight=gameHeight;pageWindowWidth=windowWidth;pageWindowHeight=windowHeight;
 float w=gameWidth, h=gameHeight;
 float scale=std::max(0.65f,std::min(w/1600.f,h/900.f));
 if(core)core->ui_scale=scale;
 api<void(*)(float)>("igSetWindowFontScale")(scale);
 setpos({w-180*scale,24*scale});
 logButtonRect("Mods",w-180*scale,24*scale,150*scale,46*scale);
 if(button("Mods",{150*scale,46*scale})) {if(core){core->scan();selected=core->enabled_set();}managerOpen=true;savesOpen=false;debugFrames=5;api<void(*)(const char*,int)>("igOpenPopup_Str")("Mod 管理###TCMods",0);log("Manager opened");}
 /* The save manager is the loader's second own entry: same size, one slot under
    Mods, and the plugin-registered pages shift down by one.  Having it as its
    own page is what keeps the Mod list about Mods. */
 const float savesY=(24.f+56.f)*scale;
 setpos({w-180*scale,savesY});
 logButtonRect("Saves",w-180*scale,savesY,150*scale,46*scale);
 if(button("存档",{150*scale,46*scale})) {savesOpen=true;managerOpen=false;debugFrames=5;api<void(*)(const char*,int)>("igOpenPopup_Str")("存档管理###TCSaves",0);log("Save manager opened");}
 /* Development convenience: open the manager without a click, so a screenshot
    run does not need synthetic mouse input (TC_MODLOADER_OPEN=1). */
 if(!managerOpen&&core&&homeSeen){
  static bool autoEnv=false,autoWanted=false,autoDone=false;
  static int homeFrames=0;
  if(!autoEnv){
   autoEnv=true;
   wchar_t buffer[8]{};
   autoWanted=GetEnvironmentVariableW(L"TC_MODLOADER_OPEN",buffer,8)>0;
  }
  /* Once per process: reopening whenever the player closes it would fight the
     close button (and spammed the log while this was being written). */
  if(autoWanted&&!autoDone&&++homeFrames>60){autoDone=true;core->scan();selected=core->enabled_set();managerOpen=true;debugFrames=5;api<void(*)(const char*,int)>("igOpenPopup_Str")("Mod 管理###TCMods",0);log("Manager opened (auto)");}
 }
 /* One entry per page an active plugin registered.  A plugin that failed to
    load has no entry, and the entry disappears with the plugin on the next
    boot - the loader never hot-unloads. */
 if(core&&nativeRuntime){
  auto pages=nativeRuntime->pages();
  for(size_t i=0;i<pages.size();++i){
   const float y=(24.f+56.f*(float)(i+2))*scale;
   setpos({w-180*scale,y});
   logButtonRect(pages[i].id,w-180*scale,y,150*scale,46*scale);
   auto push=api<void(*)(const char*)>("igPushID_Str");
   push(pages[i].owner);
   const std::string label=std::string(pages[i].title&&pages[i].title[0]?pages[i].title:pages[i].id)+(pages[i].failed?" (已停用)":"");
   if(button(label.c_str(),{150*scale,46*scale})){if(nativeRuntime->openPage(pages[i].owner,pages[i].id))log("Opened UI page "+std::string(pages[i].owner)+"/"+pages[i].id);}
   api<void(*)()>("igPopID")();
  }
 }
 api<void(*)(float)>("igSetWindowFontScale")(1.f);
 api<void(*)(V2,int)>("igSetNextWindowSize")({std::min(w-24.f,1120*scale),std::min(h-24.f,820*scale)},1);
 api<void(*)(V2,int,V2)>("igSetNextWindowPos")({w/2,h/2},1,{0.5f,0.5f});
 api<void(*)(float)>("igSetNextWindowBgAlpha")(1.f);
 bool open=true;
 bool visible=api<bool(*)(const char*,bool*,int)>("igBeginPopupModal")("Mod 管理###TCMods",&open,2|32);
 static const wchar_t* shotPath=nullptr;
 static bool shotEnv=false;
 static int managerFrames=0;
 static int shotFrame=0;
 static int drawFrames=0;
 ++drawFrames;
 if(!shotEnv){
  shotEnv=true;
  wchar_t buffer[1024]{};
  const DWORD length=GetEnvironmentVariableW(L"TC_MODLOADER_SHOT",buffer,1024);
  if(length>0&&length<1024)shotPath=_wcsdup(buffer);
  shotFrame=(int)GetEnvironmentVariableW(L"TC_MODLOADER_SHOT_FRAME",buffer,1024)>0?atoi(std::string(buffer,buffer+wcslen(buffer)).c_str()):0;
 }
 /* Frame-numbered capture: lets a screenshot run pick a moment (e.g. after a
    driver has entered a level) without needing the manager open. */
 if(shotPath&&shotFrame>0&&drawFrames==shotFrame){
  log(captureFrame(shotPath)?"Captured frame screenshot":"Screenshot failed");
 }
 if(visible){
  ++managerFrames;
  /* A few frames in: the popup has been rendered at least once, so the
     backbuffer actually contains it (a read is two frames behind). */
  if(shotPath&&managerFrames==8){
   const bool ok=captureFrame(shotPath);
   log(ok?"Captured manager screenshot":"Screenshot failed");
  }
 }
 if(debugFrames>0){--debugFrames;log(std::string("Popup frame: home=")+(homeSeen?"1":"0")+" visible="+(visible?"1":"0"));}
 if(!open)managerOpen=false;
 if(visible) {
  api<void(*)(float)>("igSetWindowFontScale")(0.62f*scale);
  bool requestClose=false;
  bool openSaves=false;
  modsManagerUi(scale,requestClose,openSaves);
  if(requestClose){managerOpen=false;api<void(*)()>("igCloseCurrentPopup")();}
  if(openSaves){
   managerOpen=false;savesOpen=true;
   api<void(*)()>("igCloseCurrentPopup")();
   api<void(*)(const char*,int)>("igOpenPopup_Str")("存档管理###TCSaves",0);
  }
  api<void(*)()>("igEndPopup")();
 }
 /* The save manager: its own modal, its own entry, the same size as the Mod
    one so the two pages cannot fight over the screen. */
 api<void(*)(V2,int)>("igSetNextWindowSize")({std::min(w-30.f,1000*scale),std::min(h-40.f,740*scale)},1);
 api<void(*)(V2,int,V2)>("igSetNextWindowPos")({w/2,h/2},1,{0.5f,0.5f});
 api<void(*)(float)>("igSetNextWindowBgAlpha")(1.f);
 bool savesModal=true;
 bool savesVisible=api<bool(*)(const char*,bool*,int)>("igBeginPopupModal")("存档管理###TCSaves",&savesModal,2|32);
 static int savesFrames=0;
 if(savesVisible){
  ++savesFrames;
  if(shotPath&&savesFrames==8){
   const bool ok=captureFrame(shotPath);
   log(ok?"Captured save manager screenshot":"Screenshot failed");
  }
 }
 if(!savesModal)savesOpen=false;
 if(savesVisible){
  api<void(*)(float)>("igSetWindowFontScale")(0.62f*scale);
  bool savesClose=false;
  savesManagerUi(scale,savesClose);
  if(savesClose){savesOpen=false;api<void(*)()>("igCloseCurrentPopup")();}
  api<void(*)()>("igEndPopup")();
 }
 /* Development convenience: open the save manager without a click, so a
    screenshot run needs no synthetic mouse input (TC_MODLOADER_OPEN_SAVES=1). */
 if(!savesOpen&&core&&homeSeen){
  static bool savesEnvDone=false,savesWanted=false;
  static int savesHomeFrames=0;
  if(!savesEnvDone){
   savesEnvDone=true;
   wchar_t buffer[8]{};
   savesWanted=GetEnvironmentVariableW(L"TC_MODLOADER_OPEN_SAVES",buffer,8)>0;
  }
  if(savesWanted&&++savesHomeFrames>60){
   savesWanted=false;savesOpen=true;debugFrames=5;
   api<void(*)(const char*,int)>("igOpenPopup_Str")("存档管理###TCSaves",0);
   log("Save manager opened (auto)");
  }
 }
 if(firstDraw){firstDraw=false;log("Main menu Mods button rendered");}
}
/* ---- save profile manager UI ---------------------------------------------
   Opened from the loader's own 存档 button on the main menu.  Every frame the
   popup is up the page walks the profiles directory again, so the list cannot
   drift from disk and the loader keeps no profile state of its own.  "本次运行"
   is the profile this process redirected to at boot, "下次启动" is what
   saves.ini says - the same restart semantics the Mod list already has. */
static std::string humanBytes(unsigned long long bytes){
 char text[32]{};
 if(bytes>=1024ULL*1024*1024)std::snprintf(text,sizeof(text),"%.2f GB",(double)bytes/(1024.0*1024*1024));
 else if(bytes>=1024ULL*1024)std::snprintf(text,sizeof(text),"%.1f MB",(double)bytes/(1024.0*1024));
 else std::snprintf(text,sizeof(text),"%llu KB",bytes/1024);
 return std::string(text);
}
/* A path that has to fit one line: keep both ends, which is what identifies a
   profile ("…\Turing Complete Mods\profiles\default"). */
static std::string elideMiddle(const std::string& text,size_t limit=52){
 if(text.size()<=limit)return text;
 const size_t head=(limit-3)/3,tail=limit-3-head;
 return text.substr(0,head)+"..."+text.substr(text.size()-tail);
}
static void savesManagerUi(float scale,bool& requestClose){
 static std::string picked,notice,renameFor;
 static char renameText[80]{};
 static bool confirming=false;
 /* The list and the report used to be rebuilt on every frame the page was up:
    walking every profile (the live one has 139 circuit files), reading
    saves.ini twice and re-parsing every Mod package per scan.  Measured while a
    player had the page open, that is a stutter on every frame.  Now the list is
    refreshed on open, on the button, or when the profiles directory itself
    changes (one stat per frame), and each profile's report is scanned once and
    kept for the session. */
 static std::vector<tc::SaveProfiles::Entry> entries;
 static std::map<std::string,tc::SaveReport> reports;
 static std::string selectedId;
 static tc::fs::file_time_type entriesStamp{};
 static bool entriesReady=false,refreshWanted=true,haveStamp=false;
 static std::map<std::string,std::string> installed;
 static std::set<std::string> enabled;
 static bool modsKnown=false;
 const float toolbar=34.f*scale,footer=104.f*scale;
 const float width=api<float(*)()>("igGetWindowWidth")();
 textColor(colInfo(),std::string("存档管理"));
 std::string currentPath,nextId,trashNote;
 if(saves){
  bool stale=refreshWanted||!entriesReady;
  if(!stale&&haveStamp){
   std::error_code error;
   const auto now=tc::fs::last_write_time(saves->profiles,error);
   stale=error||now!=entriesStamp;
  }
  if(stale){
   try{
    const unsigned long long started=GetTickCount64();
    entries=saves->list();
    currentPath=saves->path(tc_save_boot::profile).u8string();
    const std::wstring selected=saves->next();
    selectedId=std::string(selected.begin(),selected.end());
    const auto trashed=saves->trash();
    if(!trashed.empty())trashNote="回收站 "+std::to_string(trashed.size())+" 份";
    std::error_code error;
    entriesStamp=tc::fs::last_write_time(saves->profiles,error);
    haveStamp=!error;
    entriesReady=true;refreshWanted=false;
    log("Save profiles refreshed: "+std::to_string(entries.size())+" profiles, "+
        std::to_string(GetTickCount64()-started)+" ms");
   }catch(const std::exception&e){notice=e.what();}
  }
 }
 /* Read from the cache: saves.ini is only touched by the refresh above, so a
    switch made here shows up after the list is refreshed, exactly like the file
    the next start will read. */
 nextId=selectedId;
 same();textDim("   "+std::to_string(entries.size())+" 个副本"+(trashNote.empty()?"":"   ·   "+trashNote));
 /* Ids on this line, the full path in the details pane: a wrapped absolute path
    here pushed the list down and read like part of it. */
 textDim("本次运行："+narrow(tc_save_boot::profile));
 if(nextId!=narrow(tc_save_boot::profile))
  textColor(colWarn(),"下次启动将使用："+nextId+"（重启游戏后生效）");
 else textDim("切换、新建与导入都在重启后生效；删除会移入回收站，不直接销毁。");
 spacing();
 if(button("刷新",{72.f*scale,toolbar})){notice.clear();refreshWanted=true;reports.clear();modsKnown=false;}
 same();if(button("新建存档",{128.f*scale,toolbar})){
  try{const auto id=saves->create();saves->select(id);notice="已新建 "+std::string(id.begin(),id.end())+"，重启游戏后使用";confirming=false;refreshWanted=true;reports.clear();}
  catch(const std::exception&e){notice=e.what();}
 }
 same();if(button("导入原版存档",{196.f*scale,toolbar})){
  try{const auto id=saves->import_original();notice="已导入为 "+std::string(id.begin(),id.end())+"，重启游戏后使用";confirming=false;refreshWanted=true;reports.clear();}
  catch(const std::exception&e){notice=e.what();}
 }
 same();if(button("打开当前文件夹",{224.f*scale,toolbar})){
  if(saves)ShellExecuteW(nullptr,L"open",saves->path(tc_save_boot::profile).c_str(),nullptr,nullptr,SW_SHOWNORMAL);
 }
 line();
 /* Stacked, not side by side: the pane split depends on the popup's own width,
    which varies with the player's window, and at the smaller sizes the details
    column ran off the popup.  A full-width list with the details underneath
    reads the same at every size. */
 const float listHeight=-footer-380.f*scale;
 if(picked.empty()&&!entries.empty())picked=std::string(entries.front().id.begin(),entries.front().id.end());
 if(api<bool(*)(const char*,V2,int,int)>("igBeginChild_Str")("TCSaveList",{0,listHeight},0,0)){
  if(!saves)textDim("存档后端不可用。");
  else if(entries.empty())textDim("还没有存档副本。用「新建存档」开一份新的，或用「导入原版存档」把原版进度复制过来。");
  else for(auto& entry:entries){
   const std::string id(entry.id.begin(),entry.id.end());
   api<void(*)(const char*)>("igPushID_Str")(id.c_str());
   std::string label=id;
   if(entry.current)label+="   本次运行";
   else if(entry.selected)label+="   下次启动";
   if(entry.imported)label+="   导入副本";
   const bool isPicked=picked==id;
   if(entry.current)pushStyleColor(0,colGood());
   if(selectableRow(label,isPicked,width*0.5f)){picked=id;confirming=false;}
   if(entry.current)popStyleColor();
   /* Under the name, not beside it: a right-aligned run of these values ran
      past the list pane and printed inside the details pane.  Kept short for the
      same reason: an ImGui child clips what is drawn *after* it, not text that
      started inside it. */
   std::string when=entry.modified.empty()?"时间未知":entry.modified;
   if(when.size()>11)when=when.substr(5);   /* "YYYY-" dropped: the pane is narrow */
   same();textDim("      "+humanBytes(entry.bytes)+" · "+std::to_string(entry.files)+" 文件 · "+when);
   api<void(*)()>("igPopID")();
  }
  api<void(*)()>("igEndChild")();
 }
 if(api<bool(*)(const char*,V2,int,int)>("igBeginChild_Str")("TCSaveDetails",{0,-footer},0,0)){
  tc::SaveProfiles::Entry* entry=nullptr;
  for(auto& candidate:entries)if(picked==std::string(candidate.id.begin(),candidate.id.end()))entry=&candidate;
  if(!entry)textDim("在左侧点一个存档，这里显示详情与操作。");
  else{
   const std::string id(entry->id.begin(),entry->id.end());
   textColor(colInfo(),id);
   /* One short line each, and the operations stacked one per line: at the
      player's own window size this pane is around 300 px wide, and a row of
      buttons plus a wrapped absolute path ran off the page (measured, then
      this layout). */
   textDim(elideMiddle(entry->path.u8string(),26));
   std::string when=entry->modified.empty()?"时间未知":entry->modified;
   if(when.size()>11)when=when.substr(5);       /* the pane is ~26 glyphs wide */
   textDim(humanBytes(entry->bytes)+" · "+std::to_string(entry->files)+" 文件 · "+when);
   if(entry->current)textColor(colGood(),"● 本次运行正在使用这一份");
   else if(entry->selected)textColor(colWarn(),"● 下次启动会使用这一份（重启后生效）");
   else textDim("○ 未使用");
   /* The dependency answer goes right under the name: it is the reason the page
      exists, and at the player's window size the detailed list below the
      operations can need a scroll.  Compute first, then draw the summary here
      and the breakdown at the end. */
   /* Scanned once per profile per session: the scan decompresses every circuit in
      the profile (~150 ms for a played-through one), which is fine on a click but
      was not fine on every frame or on every selection change. */
   std::string summary;
   auto scanNow=[&]{
    const unsigned long long started=GetTickCount64();
    tc::TypeRegistry registry(core->dir/L"registry.json");
    if(!modsKnown&&core){
     for(auto& mod:core->mods)installed[mod.id]=mod.version;
     enabled=core->enabled_set();
     modsKnown=true;
    }
    reports[id]=tc::buildReport(tc::scanProfile(saves->path(entry->id),registry),registry,
                                installed,enabled);
    log("Dependency scan "+id+": "+std::to_string(reports[id].scan.circuits)+" circuits, "+
        std::to_string(GetTickCount64()-started)+" ms");
   };
   /* One automatic scan per page session (the profile that is selected when the
      page opens); switching rows then shows what is cached and offers the
      button.  The scan costs a few hundred ms on a played-through profile, which
      is a hitch worth spending once, not on every click. */
   if(!reports.count(id)&&reports.empty()){
    try{scanNow();}
    catch(const std::exception&e){notice=e.what();}
   }
   const tc::SaveReport* report=reports.count(id)?&reports.at(id):nullptr;
   const bool reportReady=report!=nullptr;
   if(reportReady){
    if(report->missing)summary+="缺失 "+std::to_string(report->missing)+" 项";
    if(report->disabled)summary+=std::string(summary.empty()?"":" · ")+"未启用 "+std::to_string(report->disabled)+" 项";
    if(report->versionMismatch)summary+=std::string(summary.empty()?"":" · ")+"版本不同 "+std::to_string(report->versionMismatch)+" 项";
    if(summary.empty())summary="用到的 Mod 都装好并启用了";
   }
   if(reportReady)
    textColor(report->missing?colBad():(report->disabled||report->versionMismatch)?colWarn():colGood(),
              "Mod 依赖："+summary);
   else textDim("Mod 依赖：未扫描（点下面的「重新扫描」）");
   spacing();
   /* Auto-sized: the player's own window decides the popup width, and a fixed
      button size clipped its Chinese label at the smaller ones (measured, after
      two rounds of guessing at glyph widths). */
   if(entry->current){api<void(*)(bool)>("igBeginDisabled")(true);button("已在使用");api<void(*)()>("igEndDisabled")();}
   else if(button("切换")){
    try{saves->select(entry->id);notice="已选择 "+id+"，重启游戏后生效";confirming=false;refreshWanted=true;}
    catch(const std::exception&e){notice=e.what();}
   }
   same();if(button("复制")){
    try{const auto copy=saves->duplicate(entry->id);notice="已复制为 "+std::string(copy.begin(),copy.end());confirming=false;refreshWanted=true;reports.clear();}
    catch(const std::exception&e){notice=e.what();}
   }
   same();if(button("打开"))ShellExecuteW(nullptr,L"open",entry->path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
   if(entry->current){api<void(*)(bool)>("igBeginDisabled")(true);button("删除");api<void(*)()>("igEndDisabled")();}
   else if(!confirming){
    same();if(button("删除")){confirming=true;notice="再点一次确认：删除 "+id+"（移入回收站）";}
   }else{
    pushStyleColor(0,colBad());
    if(button("确认删除")){
     try{saves->remove(entry->id);notice="已把 "+id+" 移入回收站";picked.clear();refreshWanted=true;reports.clear();}
     catch(const std::exception&e){notice=e.what();}
     confirming=false;
    }
    popStyleColor();
   }
   spacing();line();
   if(renameFor!=id){renameFor=id;std::snprintf(renameText,sizeof(renameText),"%s",id.c_str());}
   api<void(*)(float)>("igPushItemWidth")(150.f*scale);
   const bool edited=api<bool(*)(const char*,char*,unsigned long long,int,void*,void*)>("igInputText")(
       "##rename",renameText,sizeof(renameText),0,nullptr,nullptr);
   api<void(*)()>("igPopItemWidth")();
   same();if(button("重命名")&&edited){
    try{
     const std::string to(renameText);
     const auto renamed=saves->rename(entry->id,std::wstring(to.begin(),to.end()));
     notice="已重命名为 "+std::string(renamed.begin(),renamed.end());picked=to;confirming=false;refreshWanted=true;reports.clear();
    }catch(const std::exception&e){notice=e.what();}
   }
   textWrapped("名字只能用小写字母、数字与短横线（最长 64 字符）。");
   spacing();
   spacing();line();
   /* Which Mods this profile needs.  A circuit file only carries each custom
      component's 64 bit id, so the answer is the id → Mod/type cache the loader
      built while Mods registered (src/save_deps.hpp), joined with what is
      installed and enabled right now.  Scanned on demand and remembered per
      profile: the scan decompresses every circuit in the profile. */
   textColor(colInfo(),"Mod 依赖");
   same();
   if(button(reportReady?"重新扫描":"扫描",{120.f*scale,toolbar})){
    reports.erase(id);modsKnown=false;
    try{scanNow();}catch(const std::exception&e){notice=e.what();}
   }
   if(reportReady){
    textDim("扫了 "+std::to_string(report->scan.circuits)+" 个电路"+
            (report->scan.skipped?"（跳过 "+std::to_string(report->scan.skipped)+"）":"")+
            "，引用 "+std::to_string(report->scan.references)+" 次自定义元件");
    size_t shown=0;
    for(auto& item:report->dependencies){
     if(shown++>=8){textDim("…还有 "+std::to_string(report->dependencies.size()-8)+" 项");break;}
     /* Known ids carry the type and the Mod that provides it; a tag-only entry
        comes from the heuristic half of the scan (an uninstalled Mod's type). */
     const std::string label=item.known?(item.type.empty()?item.tag:item.type):item.tag;
     if(!item.known){
      textColor(colWarn(),"  未识别类型 "+label+"   （未安装的 Mod，或本机从未加载过）");
     }else if(!item.installed){
      textColor(colBad(),"  需要 "+label+"   ·   "+item.mod+
                         (item.recordedVersion.empty()?"":" v"+item.recordedVersion)+"（未安装）");
     }else if(!item.enabled){
      textColor(colWarn(),"  "+label+"   ·   "+item.mod+" 已安装但未启用");
     }else{
      const bool differs=!item.recordedVersion.empty()&&!item.installedVersion.empty()&&
                         item.recordedVersion!=item.installedVersion;
      textColor(differs?colWarn():colGood(),
                std::string("  ")+label+"   ·   "+item.mod+" v"+
                (item.installedVersion.empty()?"?":item.installedVersion)+
                (differs?"（存档记录 v"+item.recordedVersion+"）":""));
     }
    }
    pushStyleColor(0,V4{0.60f,0.63f,0.70f,1.f});
    textWrapped("电路文件里没有 Mod 清单：已知类型按加载器登记表（本机见过的注册）反查，"
                "带标记的未知项只能按作者写的 8 字节标记显示。");
    popStyleColor();
   }
  }
  api<void(*)()>("igEndChild")();
 }
 if(!notice.empty())textColor(colWarn(),notice);
 else textDim("删除只是移入回收站（trash-*），改回名字即可恢复。");
 if(button("关闭",{96.f*scale,toolbar}))requestClose=true;
}

extern "C" __declspec(dllexport) bool igInvisibleButton(const char* id,V2 size,int flags){
 auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);init();
 if(compatible){
  if(rva>=TC_HOME_START_RVA&&rva<TC_HOME_END_RVA){homeSeen=true;homeLastSeen=GetTickCount64();}
  /* The board scene's own builders: whether or not this is a level, seeing one
     means the main menu is gone and a menu page must not stay on screen. */
  else if(nativeRuntime&&nativeRuntime->inBoardUi(rva)){
   if(!boardSeen)log("Board scene detected (a board UI builder is drawing); menu pages will close");
   boardSeen=true;boardLastSeen=GetTickCount64();
  }
 }
 return api<bool(*)(const char*,V2,int)>("igInvisibleButton")(id,size,flags);
}
/* The circuit board's own input sampling site.  build_board_ui calls this at
   0x14046b58e and keeps the answer as "an ImGui item is active" before asking
   igIsWindowBgActive and igIsWindowHovered; the pair it then hands to
   handle_io_on_board() decides whether the board looks at the mouse at all
   (either half set means it does not).

   The loader is the provider of this import, so a registered board panel can
   be drawn here: inside the board's own window, and *before* the game samples
   the input state for the frame.  A click that landed in the panel is
   therefore already accounted for when the game asks, which is what keeps it
   out of the circuit board.  Every other call site is forwarded untouched. */
extern "C" __declspec(dllexport) bool igIsAnyItemActive(){
 auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);init();
 /* A plugin may have hooked this entry point itself, in which case the call
    reaches us through MinHook's trampoline and the immediate return address is
    inside the plugin rather than in the game.  The board's sampling site is
    then still on the stack a few frames up, so the check falls back to a short
    stack walk; the fast path stays for the ordinary case. */
 bool atBoardSample=rva==TC_BOARD_INPUT_SAMPLE_RVA;
 if(!atBoardSample&&compatible&&nativeRuntime&&nativeRuntime->hasBoardSlots()){
  void* frames[10]{};
  const unsigned short count=RtlCaptureStackBackTrace(2,10,frames,nullptr);
  const uintptr_t base=(uintptr_t)GetModuleHandleW(nullptr);
  for(unsigned short i=0;i<count&&!atBoardSample;++i)
   atBoardSample=((uintptr_t)frames[i]-base)==TC_BOARD_INPUT_SAMPLE_RVA;
 }
 if(compatible&&nativeRuntime&&atBoardSample){
  /* One line the first time it happens: the panel is drawn from here, so no
     line means the panel could never appear. */
  static bool sampleSeen=false;
  if(!sampleSeen&&nativeRuntime->hasBoardSlots()){
   sampleSeen=true;log("Board input sample site reached; board panels are drawn from here");
  }
  bool consumed=false;
  try{consumed=nativeRuntime->boardSlotFrame();}catch(const std::exception&e){log(std::string("Board panel error: ")+e.what());}catch(...){}
  if(consumed)return true;
 }
 return api<bool(*)()>("igIsAnyItemActive")();
}
extern "C" __declspec(dllexport) void igEnd(){
 auto rva=(uintptr_t)__builtin_return_address(0)-(uintptr_t)GetModuleHandleW(nullptr);init();
 devShotTick();
 flushTextTrace();
#if defined(TC_PIN_PATCH_ONLY) || defined(TC_PIN_PATCH_PLUGIN)
 /* Frame boundary: the component preview's name list is rebuilt from here. */
 ++previewLabelTick;
#endif
const bool menuEnd=compatible&&rva==TC_MENU_END_RVA;
#ifndef TC_PIN_PATCH_ONLY
if(menuEnd){if(homeSeen||managerOpen||savesOpen){try{draw();}catch(const std::exception&e){log(e.what());}}homeSeen=false;}
#endif
api<void(*)()>("igEnd")();
#ifndef TC_PIN_PATCH_ONLY
if(compatible&&nativeRuntime){
  nativeRuntime->frame();
  if(nativeRuntime->pageOpen()){
   /* A page belongs to the main menu.  It is drawn every frame the game is
      compositing its UI - while the page covers the menu the game may stop
      building the home page, so "home drew this frame" cannot be required -
      but it is closed as soon as a board or another scene takes over.  The
      short grace period covers the frames between a menu click and the scene
      actually changing. */
   /* The board counts as "took over" when its UI drew after the home page was
      last seen, and not before the page was opened: opening a page makes the
      game stop building the home page, so an old board sighting must not
      close it. */
   const bool boardActive=boardSeen&&boardLastSeen>=homeLastSeen&&boardLastSeen>=pageOpenedAt;
   if(boardActive){log("Closed UI page: a board scene took over");nativeRuntime->closePage();}
   else try{nativeRuntime->uiFrame(true);}catch(const std::exception&e){log(e.what());}
 } else {boardSeen=false;pageOpenedAt=GetTickCount64();}
}
#else
(void)menuEnd;
#endif
}
#ifdef TC_PIN_PATCH_PLUGIN
static void pinPatchFrame(void*,const TCFrame*){++previewLabelTick;}
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* host,TCPlugin* out){
 if(!host||host->api_version!=TC_MOD_API_VERSION||host->size<TC_HOST_BASE_SIZE||
    !host->create_hook||!host->engine_proc||!host->log||!out||out->size<sizeof(TCPlugin))
  return 1;
 pinPatchHost=host;
 init();
 const bool canReport=tc::hostHasField(host,offsetof(TCHost,report_status),
                                       sizeof(host->report_status))&&host->report_status;
 if(!compatible||!error.empty()||!pinPatchHooksReady){
  if(canReport)
   host->report_status(host->context,2,error.empty()?"补丁钩子安装失败":error.c_str());
  return 2;
 }
 out->user=nullptr;
 out->on_frame=pinPatchFrame;
 out->on_unload=nullptr;
 /* The panel's own live refresh belongs to the loader (it owns
    build_io_state_view), so this package is only the preview side. */
 if(canReport)host->report_status(host->context,0,"v0.0.4 已启用：引脚编号与名字对照表");
 return 0;
}
#endif
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(h);
#ifdef TC_PIN_PATCH_ONLY
 /* The patch does not redirect saves: the player's own profile stays where it is. */
 return TRUE;
#else
 /* Never fail the load here.  This used to return FALSE when the save redirect
    could not be applied, and because this DLL *is* the game's engine, a game
    update turned into "the game does not start at all" with no way out except
    uninstalling.  Now the result is recorded, logged by init(), and the session
    runs in a degraded mode (see NativeRuntime::forcedSafeMode): the player's own
    save path is left untouched and no native plugin runs against it. */
 saveRedirect=tc_save_boot::attach();
 return TRUE;
#endif
}return TRUE;}
