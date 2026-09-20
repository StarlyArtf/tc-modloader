#ifndef TC_UI_TEXTURE_H
#define TC_UI_TEXTURE_H
/* Optional host-managed images for the pinned OpenGL/ImGui renderer.
   load(host), loadDrawing(host), then loadTextures(host) at plugin load.
   All Texture operations (including destruction) belong to the render thread.
   Keep frequently used images alive across frames; do not decode every frame.
   reset() retires the ID; the host deletes it only on a later ImGui frame. */
#include "tc_ui_draw.h"
#include <utility>
namespace tc { namespace ui {
enum class TextureFilter : uint32_t { Linear=0, Nearest=1 };
namespace texture_detail {
struct Api {
    const TCHost* host=nullptr;
    // This build's cimgui compatibility export accepts a 64-bit ImTextureID,
    // not the newer 16-byte ImTextureRef. Verified at engine VA 0x180026d20.
    void (*image)(void*,uint64_t,Vec2,Vec2,Vec2,Vec2,Color)=nullptr;
};
inline Api& api(){static Api value;return value;}
}
inline bool texturesReady(){return texture_detail::api().host && texture_detail::api().image;}
inline bool loadTextures(const TCHost* host) {
    auto& api=texture_detail::api();api={};
    if(!host || host->api_version!=TC_MOD_API_VERSION ||
       host->size<offsetof(TCHost,release_ui_texture)+sizeof(host->release_ui_texture) ||
       !host->create_ui_texture || !host->load_ui_texture || !host->release_ui_texture || !host->engine_proc)
        return false;
    void* image=host->engine_proc(host->context,"ImDrawList_AddImage");
    if(!image)return false;
    std::memcpy(&api.image,&image,sizeof(image));api.host=host;return true;
}
class Texture {
    const TCHost* host_=nullptr;
    TCUiTexture value_{sizeof(TCUiTexture),0,0,0,0,0};
    int replace(int status,const TCUiTexture& created,const TCHost* host) {
        if(status)return status; // preserve the old image on failed decode/upload
        const int released=reset();
        if(released) { host->release_ui_texture(host->context,created.handle);return released; }
        value_=created;host_=host;return 0;
    }
public:
    Texture()=default;
    ~Texture(){reset();}
    Texture(const Texture&)=delete;
    Texture& operator=(const Texture&)=delete;
    Texture(Texture&& other) noexcept :host_(other.host_),value_(other.value_) {
        other.host_=nullptr;other.value_={sizeof(TCUiTexture),0,0,0,0,0};
    }
    // No move assignment: a fallible off-thread release must not silently
    // discard ownership. reset() explicitly, then move-construct instead.
    Texture& operator=(Texture&&)=delete;
    explicit operator bool()const{return value_.handle!=0;}
    uint32_t width()const{return value_.width;}
    uint32_t height()const{return value_.height;}
    Vec2 size()const{return {float(width()),float(height())};}
    int reset() {
        if(!value_.handle)return 0;
        const int result=host_->release_ui_texture(host_->context,value_.handle);
        if(!result){host_=nullptr;value_={sizeof(TCUiTexture),0,0,0,0,0};}
        return result;
    }
    int load(const char* packagePath,TextureFilter filter=TextureFilter::Linear) {
        auto* host=texture_detail::api().host;if(!texturesReady())return -1;
        TCUiTexture next{sizeof(TCUiTexture),0,0,0,0,0};
        const int status=host->load_ui_texture(host->context,packagePath,uint32_t(filter),&next);
        return replace(status,next,host);
    }
    int createRgba(uint32_t width,uint32_t height,const void* bytes,uint64_t byteCount,
                   TextureFilter filter=TextureFilter::Linear) {
        auto* host=texture_detail::api().host;if(!texturesReady())return -1;
        TCUiTexture next{sizeof(TCUiTexture),0,0,0,0,0};
        TCUiTexturePixels pixels{sizeof(TCUiTexturePixels),width,height,uint32_t(filter),bytes,byteCount};
        const int status=host->create_ui_texture(host->context,&pixels,&next);
        return replace(status,next,host);
    }
    /* min/max are canvas-local. UV 0,0 is the top-left pixel; reversing a UV
       axis flips the image. Tint multiplies straight-alpha RGBA channels. */
    void draw(const Canvas& canvas,Vec2 minimum,Vec2 maximum,
              Vec2 uvMinimum={0,0},Vec2 uvMaximum={1,1},Color tint=rgba(255,255,255))const {
        auto& api=texture_detail::api();
        if(!value_.handle||!api.image||!canvas.box(minimum,maximum)||
           !drawing_detail::finite(uvMinimum)||!drawing_detail::finite(uvMaximum))return;
        api.image(canvas.list_,value_.renderer_id,canvas.toScreen(minimum),canvas.toScreen(maximum),
                  uvMinimum,uvMaximum,tint);
    }
    /* Layout image with a rectangular left-click target; dimensions default
       to native pixels when BOTH components are zero. */
    bool imageButton(const char* id,Vec2 dimensions={0,0},Color tint=rgba(255,255,255))const {
        if(!*this)return false;
        if(dimensions.x==0 && dimensions.y==0)dimensions=size();
        Canvas canvas(id,dimensions);
        draw(canvas,{0,0},dimensions,{0,0},{1,1},tint);
        return canvas.clicked();
    }
};
}}
#endif
