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
using ApiV7=TCComponentRenderApiV7;
using ApiV8=TCComponentRenderApiV8;
using Frame=TCComponentRenderFrameV1;
using Draw=TCComponentRenderDrawV1;
using DrawV2=TCComponentRenderDrawV2;
using DrawV3=TCComponentRenderDrawV3;
using DrawV4=TCComponentRenderDrawV4;

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

/* V7 adds the images a Mod's own draw callback paints.  A loader that predates
   it answers ERR_VERSION, so this returns false and the caller keeps its own
   path (for example the hand-rolled WIC + ImDrawList route examples/clock used
   before this service existed). */
inline bool tableV7(const TCHost* host,ApiV7* out){
    if(!host||!out||!host->query_service)return false;
    ApiV7 queried{};
    if(host->query_service(host->context,TC_SERVICE_COMPONENT_RENDER,
                           TC_COMPONENT_RENDER_API_VERSION_7,&queried,
                           sizeof(queried))!=TC_SERVICE_OK)return false;
    if(queried.size<sizeof(queried)||queried.version!=TC_COMPONENT_RENDER_API_VERSION_7||
       !queried.context||!queried.set_draw_callback||!queried.set_default_drawing||
       !queried.set_selection_hint||!queried.set_foundry_button||!queried.set_picture||
       !queried.set_placement_preview||!queried.load_image||!queried.release_image)
        return false;
    *out=queried;return true;
}

inline bool tableV8(const TCHost* host,ApiV8* out){
    if(!host||!out||!host->query_service)return false;
    ApiV8 queried{};
    if(host->query_service(host->context,TC_SERVICE_COMPONENT_RENDER,
                           TC_COMPONENT_RENDER_API_VERSION_8,&queried,
                           sizeof(queried))!=TC_SERVICE_OK)return false;
    if(queried.size<sizeof(queried)||queried.version!=TC_COMPONENT_RENDER_API_VERSION_8||
       !queried.context||!queried.set_draw_callback||!queried.set_default_drawing||
       !queried.set_selection_hint||!queried.set_foundry_button||!queried.set_picture||
       !queried.set_placement_preview||!queried.load_image||!queried.release_image||
       !queried.load_font||!queried.release_font||!queried.load_instance_image||
       !queried.release_instance_image||!queried.set_device_callback||
       !queried.device_generation)return false;
    *out=queried;return true;
}

/* Loads (or replaces) one image for this Mod.  `key` is the Mod's own name for
   it - lowercase a-z 0-9 . _ -, at most 64 bytes - and `pngPath` is an absolute
   path to a readable PNG, read once here.  The returned `image` handle is what
   drawV3(frame)->image(...) names; loading the same key again keeps the handle
   and bumps `revision`. */
inline int loadImage(const ApiV7& api,const char* key,const char* pngPath,
                     TCComponentRenderImageV1* out){
    if(!out)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
    out->size=sizeof(*out);
    out->version=TC_COMPONENT_RENDER_IMAGE_VERSION_1;
    return api.load_image?api.load_image(api.context,key,pngPath,out)
                         :TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}

/* Drops one image, or every image of this Mod when `key` is NULL. */
inline int releaseImage(const ApiV7& api,const char* key){
    return api.release_image?api.release_image(api.context,key)
                            :TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}
inline int loadImage(const ApiV8& api,const char* key,const char* pngPath,
                     TCComponentRenderImageV1* out){
    if(!out)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
    out->size=sizeof(*out);out->version=TC_COMPONENT_RENDER_IMAGE_VERSION_1;
    return api.load_image?api.load_image(api.context,key,pngPath,out)
                         :TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}
inline int releaseImage(const ApiV8& api,const char* key){
    return api.release_image?api.release_image(api.context,key)
                            :TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}

inline int loadFont(const ApiV8& api,const char* key,const char* ttfPath,float nativeSize,
                    const TCComponentRenderFontRangeV1* ranges,uint32_t rangeCount,
                    TCComponentRenderFontV1* out){
    if(!out)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
    *out={};out->size=sizeof(*out);out->version=TC_COMPONENT_RENDER_FONT_VERSION_1;
    return api.load_font?api.load_font(api.context,key,ttfPath,nativeSize,ranges,rangeCount,out)
                        :TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}
inline int releaseFont(const ApiV8& api,const char* key){
    return api.release_font?api.release_font(api.context,key):TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}
inline int loadInstanceImage(const ApiV8& api,const TCComponentInstanceHandle* instance,
                             const char* key,const char* pngPath,
                             TCComponentRenderImageV1* out){
    if(!out)return TC_COMPONENT_RENDER_ERR_ARGUMENT;
    *out={};out->size=sizeof(*out);out->version=TC_COMPONENT_RENDER_IMAGE_VERSION_1;
    return api.load_instance_image?
        api.load_instance_image(api.context,instance,key,pngPath,out):
        TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}
inline int releaseInstanceImage(const ApiV8& api,const TCComponentInstanceHandle* instance,
                                const char* key){
    return api.release_instance_image?
        api.release_instance_image(api.context,instance,key):TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}
inline int setDeviceCallback(const ApiV8& api,TCComponentRenderDeviceCallbackV1 callback,
                             void* user=nullptr){
    return api.set_device_callback?api.set_device_callback(api.context,callback,user):
                                   TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}
inline int deviceGeneration(const ApiV8& api,uint64_t* generation){
    return api.device_generation?api.device_generation(api.context,generation):
                                 TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
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

/* The same call through a later table: V4's entries are a prefix of V5-V7, so a
   Mod that already holds one of those does not have to query the service twice. */
inline int setFoundryButton(const ApiV6& api,uint64_t customId,bool enabled){
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

/* The draw table as V3, or null when this frame came from a loader without the
   image primitive.  A Mod that only checks `draw->version >= 3` is equally
   correct: the loader hands out V2 when it cannot resolve the ImGui image entry
   point, so the version a Mod sees is the version it really gets. */
inline const DrawV3* drawV3(const Frame& frame){
    const Draw* base=frame.draw;
    if(!base||base->version<TC_COMPONENT_RENDER_DRAW_VERSION_3||
       base->size<sizeof(DrawV3))return nullptr;
    return reinterpret_cast<const DrawV3*>(base);
}

inline const DrawV4* drawV4(const Frame& frame){
    const Draw* base=frame.draw;
    if(!base||base->version<TC_COMPONENT_RENDER_DRAW_VERSION_4||
       base->size<sizeof(DrawV4))return nullptr;
    return reinterpret_cast<const DrawV4*>(base);
}

inline int textFont(const Frame& frame,uint64_t font,float x,float y,float size,
                    uint32_t color,uint32_t style,const char* utf8){
    const DrawV4* table=drawV4(frame);
    return table&&table->text_font?
        table->text_font(table->context,font,x,y,size,color,style,utf8):
        TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}
inline int measureFont(const Frame& frame,uint64_t font,float size,uint32_t style,
                       const char* utf8,float* width,float* height){
    const DrawV4* table=drawV4(frame);
    return table&&table->measure_font?
        table->measure_font(table->context,font,size,style,utf8,width,height):
        TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
}

/* Paints one quad of a loader-owned image (see loadImage above).  The corners
   are screen pixels in ImDrawList order; a Mod that wants the quad to follow the
   component's own rotation fills them with localToScreen, e.g.

   ```cpp
   TCComponentRenderQuadV1 quad{};
   tc::component_render::localToScreen(frame, x0, y0, &quad.x0, &quad.y0);
   tc::component_render::localToScreen(frame, x1, y0, &quad.x1, &quad.y1);
   tc::component_render::localToScreen(frame, x1, y1, &quad.x2, &quad.y2);
   tc::component_render::localToScreen(frame, x0, y1, &quad.x3, &quad.y3);
   quad.u1 = quad.v3 = 1.f;                 // or the sprite cell's own uv rect
   tc::component_render::image(frame, handle, quad, 0xffffffffu);
   ```

   Returns TC_COMPONENT_RENDER_ERR_IMAGE for a handle this Mod did not load (or
   has released), and ERR_UNAVAILABLE when the loader has no image path. */
inline int image(const Frame& frame,uint64_t imageHandle,TCComponentRenderQuadV1 quad,
                 uint32_t tint=0xffffffffu){
    const DrawV3* table=drawV3(frame);
    if(!table||!table->image)return TC_COMPONENT_RENDER_ERR_UNAVAILABLE;
    quad.tint=tint;
    quad.reserved=0;
    return table->image(table->context,imageHandle,&quad);
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
    case TC_COMPONENT_RENDER_ERR_IMAGE:return "image key, path, decode, size or quota";
    case TC_COMPONENT_RENDER_ERR_FONT:return "font key, path, parse, atlas or quota";
    default:return "unknown error";
    }
}

} // namespace tc::component_render

#endif
