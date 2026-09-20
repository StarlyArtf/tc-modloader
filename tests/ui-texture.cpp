#include "../sdk/tc_ui_texture.h"
#include <cassert>
#include <iostream>
using namespace tc::ui;
static int releases=0,loads=0,creates=0;
static bool failLoad=false,failRelease=false;
static int release(void*,uint64_t handle){assert(handle==10);if(failRelease)return -1;++releases;return 0;}
static int load(void*,const char*,uint32_t,TCUiTexture* out){++loads;if(failLoad)return -4;*out={sizeof(*out),2,3,0,10,50};return 0;}
static int create(void*,const TCUiTexturePixels* p,TCUiTexture* out){++creates;assert(p->width==2&&p->byte_count==24);return load(nullptr,nullptr,0,out);}
static void image(void*,uint64_t,Vec2,Vec2,Vec2,Vec2,Color){}
static void* resolve(void*,const char*){auto fn=&image;void* value;std::memcpy(&value,&fn,sizeof(value));return value;}
int main(){
    TCHost host{};host.size=TC_HOST_BASE_SIZE;host.api_version=TC_MOD_API_VERSION;
    assert(!loadTextures(nullptr)&&!loadTextures(&host));
    Texture unavailable;assert(unavailable.load("native/a.png")==-1);
    host.size=sizeof(host);host.engine_proc=resolve;host.create_ui_texture=create;host.load_ui_texture=load;host.release_ui_texture=release;
    assert(loadTextures(&host));
    {
        Texture texture;assert(texture.load("native/a.png")==0&&texture.width()==2&&texture.height()==3);
        failLoad=true;assert(texture.load("missing")==-4&&texture&&releases==0);failLoad=false;
        Texture moved(std::move(texture));assert(!texture&&moved);
        failRelease=true;assert(moved.reset()==-1&&moved);failRelease=false;
        const unsigned char pixels[24]{};
        assert(moved.createRgba(2,3,pixels,sizeof(pixels))==0&&releases==1&&creates==1);
    }
    assert(releases==2&&loads==3);
    assert(!loadTextures(nullptr)&&!texturesReady());
    std::cout<<"PASS texture SDK old-host fallback, move ownership, failed reload preservation, release and RGBA forwarding\n";
}
