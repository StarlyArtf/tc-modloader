#ifndef TC_COMPONENT_RENDER_H
#define TC_COMPONENT_RENDER_H

#include "tc_mod_api.h"

namespace tc::component_render {

using Api=TCComponentRenderApiV1;
using ApiV2=TCComponentRenderApiV2;
using ApiV3=TCComponentRenderApiV3;
using ApiV4=TCComponentRenderApiV4;
using ApiV5=TCComponentRenderApiV5;
using ApiV6=TCComponentRenderApiV6;
using Frame=TCComponentRenderFrameV1;
using Draw=TCComponentRenderDrawV1;
using DrawV2=TCComponentRenderDrawV2;

inline bool table(const TCHost* host,Api* out){
    if(!host||!out||!host->query_service)return false;
    Api queried{};
    if(host->query_service(host->context,TC_SERVICE_COMPONENT_RENDER,
                           TC_COMPONENT_RENDER_API_VERSION_1,&queried,
                           sizeof(queried))!=TC_SERVICE_OK)return false;
    if(queried.size<sizeof(queried)||queried.version!=TC_COMPONENT_RENDER_API_VERSION_1||
       !queried.context||!queried.set_draw_callback)return false;
    *out=queried;return true;
}

inline bool tableV2(const TCHost* host,ApiV2* out){
    if(!host||!out||!host->query_service)return false;
    ApiV2 queried{};
    if(host->query_service(host->context,TC_SERVICE_COMPONENT_RENDER,
                           TC_COMPONENT_RENDER_API_VERSION_2,&queried,
                           sizeof(queried))!=TC_SERVICE_OK)return false;
    if(queried.size<sizeof(queried)||queried.version!=TC_COMPONENT_RENDER_API_VERSION_2||
       !queried.context||!queried.set_draw_callback||!queried.set_default_drawing)return false;
    *out=queried;return true;
}

inline bool tableV3(const TCHost* host,ApiV3* out){
    if(!host||!out||!host->query_service)return false;
    ApiV3 queried{};
    if(host->query_service(host->context,TC_SERVICE_COMPONENT_RENDER,
                           TC_COMPONENT_RENDER_API_VERSION_3,&queried,
                           sizeof(queried))!=TC_SERVICE_OK)return false;
    if(queried.size<sizeof(queried)||queried.version!=TC_COMPONENT_RENDER_API_VERSION_3||
       !queried.context||!queried.set_draw_callback||!queried.set_default_drawing||
       !queried.set_selection_hint)return false;
    *out=queried;return true;
}

inline bool tableV4(const TCHost* host,ApiV4* out){
    if(!host||!out||!host->query_service)return false;
    ApiV4 queried{};
    if(host->query_service(host->context,TC_SERVICE_COMPONENT_RENDER,
                           TC_COMPONENT_RENDER_API_VERSION_4,&queried,
                           sizeof(queried))!=TC_SERVICE_OK)return false;
    if(queried.size<sizeof(queried)||queried.version!=TC_COMPONENT_RENDER_API_VERSION_4||
       !queried.context||!queried.set_draw_callback||!queried.set_default_drawing||
       !queried.set_selection_hint||!queried.set_foundry_button)return false;
    *out=queried;return true;
}

inline bool tableV5(const TCHost* host,ApiV5* out){
    if(!host||!out||!host->query_service)return false;
    ApiV5 queried{};
    if(host->query_service(host->context,TC_SERVICE_COMPONENT_RENDER,
                           TC_COMPONENT_RENDER_API_VERSION_5,&queried,
                           sizeof(queried))!=TC_SERVICE_OK)return false;
    if(queried.size<sizeof(queried)||queried.version!=TC_COMPONENT_RENDER_API_VERSION_5||
       !queried.context||!queried.set_draw_callback||!queried.set_default_drawing||
       !queried.set_selection_hint||!queried.set_foundry_button||!queried.set_picture)
        return false;
    *out=queried;return true;
}

inline bool tableV6(const TCHost* host,ApiV6* out){
    if(!host||!out||!host->query_service)return false;
    ApiV6 queried{};
    if(host->query_service(host->context,TC_SERVICE_COMPONENT_RENDER,
                           TC_COMPONENT_RENDER_API_VERSION_6,&queried,
                           sizeof(queried))!=TC_SERVICE_OK)return false;
    if(queried.size<sizeof(queried)||queried.version!=TC_COMPONENT_RENDER_API_VERSION_6||
       !queried.context||!queried.set_draw_callback||!queried.set_default_drawing||
       !queried.set_selection_hint||!queried.set_foundry_button||!queried.set_picture||
       !queried.set_placement_preview)return false;
    *out=queried;return true;
}

inline int setDrawCallback(const Api& api,uint64_t customId,
                           TCComponentRenderCallbackV1 callback,void* user=nullptr){
    return api.set_draw_callback
               ?api.set_draw_callback(api.context,customId,callback,user)
               :TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}

inline int setDrawCallback(const ApiV2& api,uint64_t customId,
                           TCComponentRenderCallbackV1 callback,void* user=nullptr){
    return api.set_draw_callback
               ?api.set_draw_callback(api.context,customId,callback,user)
               :TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}

inline int setDefaultDrawing(const ApiV2& api,uint64_t customId,bool enabled){
    return api.set_default_drawing
               ?api.set_default_drawing(api.context,customId,enabled?1:0)
               :TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}

inline int setSelectionHint(const ApiV3& api,uint64_t customId,bool enabled){
    return api.set_selection_hint
               ?api.set_selection_hint(api.context,customId,enabled?1:0)
               :TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}

/* The bottom panel's "edit this component in the foundry" button: enabled=false makes
   it invisible and inert for the calling Mod's own types (the panel keeps its exact
   layout), true restores the game's button. */
inline int setFoundryButton(const ApiV4& api,uint64_t customId,bool enabled){
    return api.set_foundry_button
               ?api.set_foundry_button(api.context,customId,enabled?1:0)
               :TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}

inline void localToScreen(const Frame& frame,float x,float y,float* screenX,float* screenY){
    if(screenX)*screenX=frame.origin_x+x*frame.axis_x_x+y*frame.axis_y_x;
    if(screenY)*screenY=frame.origin_y+x*frame.axis_x_y+y*frame.axis_y_y;
}

/* The picture a type is shown with - the item in the game's own component column
   and the preview picture in the bottom drawer.  Placement previews are handled
   by V6 below.  `pngPath` is an absolute path to a readable PNG
   of the Mod's own; NULL (or a file that is gone when the game asks for the
   picture) puts the type back on the game's own render.  May be called while the
   Mod loads, and later from a frame callback to change the picture: the loader
   re-reads the file whenever it changes, so a Mod that rewrites its PNG at run
   time is picked up on the next request. */
inline int setPicture(const ApiV5& api,uint64_t customId,const char* pngPath){
    return api.set_picture
               ?api.set_picture(api.context,customId,pngPath)
               :TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}

/* Lets the existing draw callback own the body, pins and label of the placement
   ghost.  The preview is opt-in per type and does not affect palette cards,
   drawer pictures, placed instances or another Mod's components. */
inline int setPlacementPreview(const ApiV6& api,uint64_t customId,bool enabled){
    return api.set_placement_preview
               ?api.set_placement_preview(api.context,customId,enabled?1:0)
               :TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}

/* The draw table as V2, or null when the loader that produced this frame is
   older than the text-size cut.  Every V2 entry point goes through here, so a
   Mod that only wants the sized text never has to know the table's layout. */
inline const DrawV2* drawV2(const Frame& frame){
    const Draw* base=frame.draw;
    if(!base||base->version<TC_COMPONENT_RENDER_DRAW_VERSION_2||
       base->size<sizeof(DrawV2))return nullptr;
    return reinterpret_cast<const DrawV2*>(base);
}

/* Text at an explicit pixel size - what a Mod needs to match the board's own
   labels and values, which the game draws at a size that follows the camera.
   `size` is in screen pixels, like every other coordinate the draw table takes;
   one board cell is `sqrt(axis_x^2 + axis_y^2)` pixels, so a part that wants
   the stock value height (0.6 of a cell) asks for 0.6 / 0.7 times that.  The
   call leaves the frame's own font and size untouched. */
inline int textSized(const Frame& frame,float x,float y,float size,uint32_t color,
                     bool bold,const char* utf8){
    const DrawV2* table=drawV2(frame);
    if(!table||!table->text_sized||!utf8)return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
    return table->text_sized(table->context,x,y,size,color,bold?1:0,utf8);
}

/* What `textSized` will occupy, in screen pixels, for layout: the same string
   the game measures before it right-aligns a label.  Returns false when this
   loader cannot measure, so the caller can fall back to an estimate. */
inline bool measureText(const Frame& frame,float size,bool bold,const char* utf8,
                        float* width,float* height){
    const DrawV2* table=drawV2(frame);
    if(!table||!table->measure_text||!utf8||!width||!height)return false;
    if(table->measure_text(table->context,size,bold?1:0,utf8,width,height)!=
       TC_COMPONENT_RENDER_OK)return false;
    return *width>0.f&&*height>0.f;
}

inline const char* errorText(int status){
    switch(status){
    case TC_COMPONENT_RENDER_OK:return "ok";
    case TC_COMPONENT_RENDER_ERR_UNAVAILABLE:return "unavailable";
    case TC_COMPONENT_RENDER_ERR_ARGUMENT:return "invalid argument";
    case TC_COMPONENT_RENDER_ERR_UNKNOWN:return "unknown component";
    case TC_COMPONENT_RENDER_ERR_OWNERSHIP:return "component type is not owned by this Mod";
    case TC_COMPONENT_RENDER_ERR_CAPACITY:return "draw command capacity exceeded";
    case TC_COMPONENT_RENDER_ERR_THREAD:return "wrong thread";
    default:return "unknown error";
    }
}

} // namespace tc::component_render

#endif
