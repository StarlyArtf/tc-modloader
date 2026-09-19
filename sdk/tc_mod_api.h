#ifndef TC_MOD_API_H
#define TC_MOD_API_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "tc_logic_api.h"
#include "tc_hook_api.h"
#include "tc_event_api.h"
#include "tc_handle_api.h"
#ifdef __cplusplus
extern "C" {
#endif
#define TC_MOD_API_VERSION 1u
#define TC_MOD_EXPORT __declspec(dllexport)

/* Packs a loader version the same way TCHost.host_version does, so a plugin can
   compare against the release that introduced an entry point it needs:
       if (host->host_version >= TC_HOST_VERSION_CODE(0,6,0)) ...
   Comparing host_version is a convenience, not a substitute for reading
   TCHost.capabilities - a plugin must not call an entry point whose capability
   bit is clear. */
#define TC_HOST_VERSION_CODE(major,minor,patch) \
    (((uint32_t)(major)<<16)|((uint32_t)(minor)<<8)|(uint32_t)(patch))

/* Capability bits in TCHost.capabilities.  A bit is set only when the loader
   really implements that part of the ABI, so a plugin refuses to start instead
   of discovering a missing entry point later.  The lower-case names in the
   comments are the stable spelling used by mod.json's "capabilities" list, so a
   package can be rejected while it is still being scanned, with a readable
   reason, instead of failing halfway through tc_mod_load. */
#define TC_CAP_LOG       (1ull<<0)  /* log */
#define TC_CAP_SYMBOL    (1ull<<1)  /* symbol: resolve_symbol and engine_proc */
#define TC_CAP_HOOK      (1ull<<2)  /* hook: create_hook */
#define TC_CAP_LOGIC     (1ull<<3)  /* logic: register_logic */
#define TC_CAP_COMPONENT (1ull<<4)  /* component: register_component */
#define TC_CAP_UI_PAGE   (1ull<<5)  /* ui_page: register_ui_page */
#define TC_CAP_UI_SLOT   (1ull<<6)  /* ui_slot: register_ui_slot */
#define TC_CAP_TEXTURE   (1ull<<7)  /* texture: create/load/release_ui_texture */
#define TC_CAP_STATUS    (1ull<<8)  /* status: report_status */
#define TC_CAP_SYMBOL_ALIAS (1ull<<9)  /* symbol_alias: resolve_alias */
#define TC_CAP_HOOK_CHAIN   (1ull<<10) /* hook_chain: register_hook_chain */
#define TC_CAP_EVENTS       (1ull<<11) /* events: add_event_listener */
#define TC_CAP_GAME_HANDLES (1ull<<12) /* game_handles: generation-checked game objects */
typedef struct TCFrame {uint32_t size; int32_t frame_number; double time_seconds;} TCFrame;

/* Native UI page registered with the optional host->register_ui_page entry
   point (see sdk/tc_ui.h).  page_id and title are copied by the host during
   tc_mod_load, so the plugin's buffers only have to live for that call.

   The page is namespaced by the loader: the host wraps draw() in
   PushID(<mod id>) / PushID(<page id>), so two plugins may use the same
   page_id and the same widget labels without colliding. */
typedef struct TCUiPageDefinition {
 uint32_t size;                /* sizeof(TCUiPageDefinition) */
 const char* page_id;          /* required, 1..63 bytes, no '###' */
 const char* title;            /* optional UTF-8; page_id is used when null/"" */
 /* Called from the game's main/render thread inside the host's own window,
    after the host has pushed both ID scopes.  Draw content only: do not open
    a window, do not call End, do not create a popup, never throw.
    content_width/height are the usable interior size in pixels. */
 void (*draw)(void* user,const TCFrame* frame,float content_width,float content_height);
 void* user;
} TCUiPageDefinition;

/* Slot kinds.  TC_UI_SLOT_BOARD_SIDE is a panel on the circuit board, inside
   the board's own window, above the canvas on the right edge.  It exists only
   while the board is on screen: the loader draws it from the board's own
   per-frame UI code and stops drawing it the moment that screen goes away, so
   leaving the level closes the panel without any plugin cleanup.

   The panel is drawn *before* the game samples its mouse state for the frame,
   so a click, a press or a drag that starts inside the panel belongs to the
   panel and is not delivered to the circuit board behind it. */
#define TC_UI_SLOT_BOARD_SIDE 1u

/* TC_UI_SLOT_BOARD_TOOLBAR is a tool *inside the game's own tool column* (the
   row of buttons the game draws for play/pause/rotate/delete and its colour and
   bit-width tools).  The loader draws it from the end of that column's child
   window, so a plugin's control sits between the game's own tools, in the same
   style, and only while a circuit board is on screen.  A tool is expected to be
   small (a button, or a colour chip) and to expand on hover. */
#define TC_UI_SLOT_BOARD_TOOLBAR 2u

/* Native UI slot registered with the optional host->register_ui_slot entry
   point (see sdk/tc_ui.h).  slot_id and title are copied by the host during
   tc_mod_load, so the plugin's buffers only have to live for that call. */
typedef struct TCUiSlotDefinition {
 uint32_t size;                /* sizeof(TCUiSlotDefinition) */
 uint32_t kind;                /* TC_UI_SLOT_BOARD_SIDE */
 const char* slot_id;          /* required, 1..63 bytes, no '###' */
 const char* title;            /* optional UTF-8; slot_id is used when null/"" */
 /* Called from the game's main/render thread inside the host's own panel,
    after the host has pushed both ID scopes.  Draw content only: do not open
    a window, do not call End, do not create a popup, never throw.
    content_width/height are the usable interior size in pixels. */
 void (*draw)(void* user,const TCFrame* frame,float content_width,float content_height);
 void* user;
 float preferred_width;        /* 0 = host default; panel width in pixels */
 float preferred_height;       /* 0 = host default; panel height in pixels */
} TCUiSlotDefinition;

/* Size of TCHost before any optional tail was appended.  Plugins must accept
   the base size and treat later fields as absent instead of rejecting the host,
   so a plugin built with a newer SDK keeps working in an older loader. */
#define TC_HOST_BASE_SIZE 64u

/* UI textures: top-to-bottom, tightly packed straight-alpha RGBA8. Handles
   belong to one plugin. renderer_id is for this pinned ImGui renderer only. */
typedef struct TCUiTexturePixels {
 uint32_t size,width,height,filter; /* filter: 0 linear, 1 nearest */
 const void* rgba;
 uint64_t byte_count;
} TCUiTexturePixels;
typedef struct TCUiTexture {
 uint32_t size,width,height,reserved;
 uint64_t handle,renderer_id;
} TCUiTexture;

typedef struct TCHost {
 uint32_t size,api_version;
 void* context;
 const char* game_build;
 const char* mod_id;
 const char* data_directory_utf8;
 void (*log)(void* context,const char* message);
 void* (*resolve_symbol)(void* context,const char* exact_coff_name);
 void* (*engine_proc)(void* context,const char* export_name);
 /* Creates a disabled hook. Host enables all hooks only after tc_mod_load succeeds.
    Targets must be game EXE functions; duplicate targets are rejected.
    Call only during tc_mod_load. Original trampoline remains valid for this process. */
 int (*create_hook)(void* context,void* target,void* detour,void** original);
 /* Optional tail extension: check host.size before accessing. Initialization
    only. Returns 0 on success; duplicate IDs / unsupported scaffolds fail. */
 int (*register_logic)(void* context,const TCLogicDefinition* definition);
 /* Optional declarative component API. Initialization only; generates the
    circuit scaffold, imports the prototype and registers its callback.
    0 success; -1 unavailable/lifecycle; -2 invalid definition; -3 duplicate ID;
    -4 import/model failure; -5 callback registration failure. */
 int (*register_component)(void* context,const TCNativeComponentDefinition* definition);
 /* Optional native UI page API (loader 0.5.0 and later). Initialization only,
    from tc_mod_load.  The page becomes visible on the game's main menu only
    after the plugin itself loads successfully; a rejected plugin leaves no
    entry behind.  Registering the same page_id twice for one plugin is
    rejected.  Returns 0 on success, -1 unavailable/out of phase, -2 invalid
    definition, -3 duplicate ID, -4 too many pages.  Check host->size >=
    offsetof(TCHost,register_ui_page)+sizeof(void*) before calling it. */
 int (*register_ui_page)(void* context,const TCUiPageDefinition* definition);
 /* Optional texture capability. Render thread only, during initialization or
    a drawing callback. 0 success; -1 unavailable/thread/lifecycle; -2 invalid
    arguments/path/handle; -3 capacity; -4 decode/upload failure.
    load path is package-relative UTF-8 under native/, e.g. native/images/a.png.
    Failed calls leave out untouched. A release invalidates the handle at once
    but GPU deletion waits until a later ImGui frame (submitted draws survive).
    Pixel dimensions <=4096; 64 MiB per texture; 128 MiB / 64 live or retiring
    textures per plugin. No hot unloading; OS reclaims remaining IDs at exit. */
 int (*create_ui_texture)(void* context,const TCUiTexturePixels* pixels,TCUiTexture* out);
 int (*load_ui_texture)(void* context,const char* package_path,uint32_t filter,TCUiTexture* out);
 int (*release_ui_texture)(void* context,uint64_t handle);
 /* Optional native UI slot API (loader 0.6.0 and later). Initialization only,
    from tc_mod_load.  A slot is a panel the host draws inside one of the
    game's own layouts; the host owns the container, the input ownership and
    the lifetime, and the panel exists only while that screen does.  Returns
    0 on success, -1 unavailable/out of phase, -2 invalid definition, -3
    duplicate ID, -4 too many slots.  Check host->size >=
    offsetof(TCHost,register_ui_slot)+sizeof(void*) before calling it. */
 int (*register_ui_slot)(void* context,const TCUiSlotDefinition* definition);
 /* Optional tail (loader 0.6.0 and later).  Keep checking host->size before
    reading anything below: an older loader passes the smaller size it knows.

    host_version is the loader build, packed with TC_HOST_VERSION_CODE, and
    capabilities is the bitmask of TC_CAP_* this loader really provides.  Both
    are constant for the life of the process.  A plugin that needs an entry
    point should test its TC_CAP_* bit before calling it. */
 uint32_t host_version;
 uint32_t host_version_reserved;
 uint64_t capabilities;
 /* Reports this plugin's state to the loader: it lands in the Mods page and in
    loader.log, so a plugin can say "my board is not connected yet" instead of
    leaving the player with a silent failure.  level is 0 info, 1 warning,
    2 error.  The loader copies the message, so the pointer only has to survive
    the call, and the call is safe from any thread.  Returns 0 on success and
    -1 when the loader cannot accept the report (older loader, null or
    over-long message, unknown level).  Messages are 512 bytes at most. */
 int (*report_status)(void* context,int level,const char* message);
 /* Stable alias -> address, resolved from the loader's symbol profile for this
    game build ("sim.do", "sim.cycle", "level.load", ...).  The alias names the
    loader has measured are listed in docs/reference/symbols.md; an unknown alias
    and an alias this build could not resolve both return null, and the loader
    log says which aliases are missing.  Use this instead of hard-coding a
    mangled Nim name: when the game is updated only the profile changes. */
 void* (*resolve_alias)(void* context,const char* alias);
 /* Joins one of the loader-owned hook chains (see sdk/tc_hook_api.h).  Several
    plugins may join the same point; they run in ascending priority order, then
    by mod id, then in registration order, so the sequence is reproducible.
    Initialization only, from tc_mod_load.  Returns TC_HOOK_OK or one of the
    TC_HOOK_ERR_* codes.  Check TC_CAP_HOOK_CHAIN before calling. */
 int (*register_hook_chain)(void* context,uint32_t hook_id,int32_t priority,TCHookCallback callback,void* user);
 /* Subscribes to the host event bus (see sdk/tc_event_api.h).  `kinds` is a
    bitmask of TC_EVENT_*.  Initialization only, from tc_mod_load.  Returns
    TC_EVENT_OK or one of the TC_EVENT_ERR_* codes.  Check TC_CAP_EVENTS. */
 int (*add_event_listener)(void* context,uint32_t kinds,TCEventCallback callback,void* user);
 /* Stable game-object handles.  BOARD is implemented first; the other kinds
    are reserved until their snapshot enumerators can issue trusted handles. */
 int (*get_current_game_handle)(void* context,uint32_t kind,TCGameHandle* out);
 int (*validate_game_handle)(void* context,const TCGameHandle* handle);
 int (*resolve_game_handle)(void* context,const TCGameHandle* handle,const void** out);
} TCHost;
typedef struct TCPlugin {
 uint32_t size;
 void* user;
 /* Main/render thread, at most once per ImGui frame. Can use engine cimgui exports.
    Always balance Begin/End and style stacks. Never throw across this C ABI. */
 void (*on_frame)(void* user,const TCFrame* frame);
 /* Called for failed initialization only; process exit uses OS cleanup. */
 void (*on_unload)(void* user);
} TCPlugin;
typedef int (*TCModLoad)(const TCHost* host,TCPlugin* plugin);
/* Required export. Return 0 on success; nonzero rejects this plugin and its hooks. */
TC_MOD_EXPORT int tc_mod_load(const TCHost* host,TCPlugin* plugin);
#ifdef __cplusplus
}
/* Capability helpers.  Every one of them tolerates an older loader, whose
   TCHost simply ends before the field being read, so a plugin can ask without
   first comparing host->size by hand. */
namespace tc {
inline bool hostHasField(const TCHost* host,size_t offset,size_t size) {
    return host != nullptr && host->size >= offset + size;
}
inline uint32_t hostVersion(const TCHost* host) {
    return hostHasField(host,offsetof(TCHost,host_version),sizeof(host->host_version)) ? host->host_version : 0u;
}
inline uint64_t hostCapabilities(const TCHost* host) {
    return hostHasField(host,offsetof(TCHost,capabilities),sizeof(host->capabilities)) ? host->capabilities : 0ull;
}
inline bool hostHas(const TCHost* host,uint64_t capability) {
    return (hostCapabilities(host) & capability) == capability;
}
/* Reports this plugin's state to the Mods page and loader.log.  Returns -1 when
   an older loader cannot accept reports.  level: 0 info, 1 warning, 2 error. */
inline int reportStatus(const TCHost* host,int level,const char* message) {
    if (!hostHasField(host,offsetof(TCHost,report_status),sizeof(host->report_status)) || !host->report_status) return -1;
    return host->report_status(host->context,level,message);
}
/* Subscribes to the host event bus; returns TC_EVENT_ERR_UNAVAILABLE on an older
   loader.  See sdk/tc_event_api.h for the kinds and the threading rules. */
inline int addEventListener(const TCHost* host,uint32_t kinds,TCEventCallback callback,void* user) {
    if (!host || !callback || kinds == 0) return TC_EVENT_ERR_ARGUMENT;
    if (!hostHasField(host,offsetof(TCHost,add_event_listener),sizeof(host->add_event_listener)) ||
        !host->add_event_listener)
        return TC_EVENT_ERR_UNAVAILABLE;
    return host->add_event_listener(host->context,kinds,callback,user);
}
inline int currentGameHandle(const TCHost* host,uint32_t kind,TCGameHandle* out) {
    if (!out) return TC_HANDLE_ERR_ARGUMENT;
    if (!hostHasField(host,offsetof(TCHost,get_current_game_handle),sizeof(host->get_current_game_handle)) ||
        !host->get_current_game_handle) return TC_HANDLE_ERR_UNAVAILABLE;
    return host->get_current_game_handle(host->context,kind,out);
}
inline int validateGameHandle(const TCHost* host,const TCGameHandle* handle) {
    if (!handle) return TC_HANDLE_ERR_ARGUMENT;
    if (!hostHasField(host,offsetof(TCHost,validate_game_handle),sizeof(host->validate_game_handle)) ||
        !host->validate_game_handle) return TC_HANDLE_ERR_UNAVAILABLE;
    return host->validate_game_handle(host->context,handle);
}
inline int resolveGameHandle(const TCHost* host,const TCGameHandle* handle,const void** out) {
    if (!handle || !out) return TC_HANDLE_ERR_ARGUMENT;
    if (!hostHasField(host,offsetof(TCHost,resolve_game_handle),sizeof(host->resolve_game_handle)) ||
        !host->resolve_game_handle) return TC_HANDLE_ERR_UNAVAILABLE;
    return host->resolve_game_handle(host->context,handle,out);
}
/* Address for a stable alias such as "sim.do" (the loader's symbol profile).
   Null when the alias is unknown to this loader or unresolved in this build. */
inline void* resolveAlias(const TCHost* host,const char* alias) {
    if (!hostHasField(host,offsetof(TCHost,resolve_alias),sizeof(host->resolve_alias)) || !host->resolve_alias) return nullptr;
    return host->resolve_alias(host->context,alias);
}
/* Same thing typed as a function pointer: tc::resolveAliasAs<SimDo>(host,"sim.do"). */
template <typename T> inline T resolveAliasAs(const TCHost* host,const char* alias) {
    void* address = resolveAlias(host,alias);
    T callable{};
    if (address) memcpy(&callable,&address,sizeof(callable));
    return callable;
}
}  // namespace tc
#endif
#endif
