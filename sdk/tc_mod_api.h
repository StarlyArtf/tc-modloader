#ifndef TC_MOD_API_H
#define TC_MOD_API_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define TC_MOD_API_VERSION 1u
#define TC_MOD_EXPORT __declspec(dllexport)
typedef struct TCFrame {uint32_t size; int32_t frame_number; double time_seconds;} TCFrame;
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
#endif
#endif
