#include "../sdk/tc_ui_texture.h"
#include "../src/ui_texture.hpp"
#include <thread>
#include <stdexcept>
using namespace tc::ui;
static const TCHost* host;
static Texture image;
static GLuint retired=0;
static int drawnFrames=0;
static bool failed=false;
static const unsigned char pixels[]{255,0,0,255, 0,255,0,128, 0,0,255,255, 255,255,0,0};
static void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
static void report(const std::string& s){host->log(host->context,s.c_str());}
template<class T> static T read(void* p,size_t offset){T value;std::memcpy(&value,(char*)p+offset,sizeof(value));return value;}
static void verifyPixels(GLuint id){
    GLint binding=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
    glBindTexture(GL_TEXTURE_2D,id);
    unsigned char actual[16]{};glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,actual);
    glBindTexture(GL_TEXTURE_2D,binding);
    require(std::memcmp(actual,pixels,sizeof(pixels))==0,"GPU RGBA pixels/alpha/orientation mismatch");
}
static void test(){
    TCUiTexturePixels definition{sizeof(definition),2,2,1,pixels,sizeof(pixels)};
    TCUiTexture out{sizeof(out),0,0,0,0,0};
    int offThread=0;std::thread worker([&]{offThread=host->create_ui_texture(host->context,&definition,&out);});worker.join();
    require(offThread==-1&&!out.handle,"worker thread accepted");
    definition.byte_count=2;require(host->create_ui_texture(host->context,&definition,&out)==-2,"short RGBA accepted");definition.byte_count=sizeof(pixels);
    require(host->load_ui_texture(host->context,"../other.png",0,&out)==-2,"path escape accepted");
    require(host->load_ui_texture(host->context,"native/missing.png",0,&out)==-4,"missing file accepted");
    require(host->load_ui_texture(host->context,"native/broken.png",0,&out)==-4,"malformed file accepted");
    GLint binding=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
    using Gen=void(APIENTRY*)(GLsizei,GLuint*);
    using Bind=void(APIENTRY*)(GLenum,GLuint);
    using Data=void(APIENTRY*)(GLenum,ptrdiff_t,const void*,GLenum);
    using Delete=void(APIENTRY*)(GLsizei,const GLuint*);
    Gen gen=nullptr;Bind bind=nullptr;Data bufferData=nullptr;Delete remove=nullptr;
    auto p=wglGetProcAddress("glGenBuffers");std::memcpy(&gen,&p,sizeof(p));
    p=wglGetProcAddress("glBindBuffer");std::memcpy(&bind,&p,sizeof(p));
    p=wglGetProcAddress("glBufferData");std::memcpy(&bufferData,&p,sizeof(p));
    p=wglGetProcAddress("glDeleteBuffers");std::memcpy(&remove,&p,sizeof(p));
    require(gen&&bind&&bufferData&&remove,"PBO test unavailable");
    GLint previousPbo=0;glGetIntegerv(0x88EF,&previousPbo);
    GLuint pbo=0;gen(1,&pbo);bind(0x88EC,pbo);bufferData(0x88EC,16,nullptr,0x88E0);
    // Deliberately non-default unpack state: host upload must normalize it and restore it.
    GLint alignment=0,row=0,skip=0;glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH,&row);glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&skip);
    glPixelStorei(GL_UNPACK_ALIGNMENT,8);glPixelStorei(GL_UNPACK_ROW_LENGTH,17);glPixelStorei(GL_UNPACK_SKIP_PIXELS,3);
    require(host->create_ui_texture(host->context,&definition,&out)==0,"RGBA upload failed");
    GLint after=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&after);require(after==binding,"texture binding leaked");
    glGetIntegerv(GL_UNPACK_ALIGNMENT,&after);require(after==8,"unpack alignment leaked");
    glGetIntegerv(GL_UNPACK_ROW_LENGTH,&after);require(after==17,"unpack row leaked");
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&after);require(after==3,"unpack skip leaked");
    glGetIntegerv(0x88EF,&after);require(after==static_cast<GLint>(pbo),"PBO binding leaked");
    bind(0x88EC,previousPbo);remove(1,&pbo);
    glPixelStorei(GL_UNPACK_ALIGNMENT,alignment);glPixelStorei(GL_UNPACK_ROW_LENGTH,row);glPixelStorei(GL_UNPACK_SKIP_PIXELS,skip);
    require(out.width==2&&out.height==2,"RGBA dimensions");verifyPixels(GLuint(out.renderer_id));
    retired=GLuint(out.renderer_id);
    require(host->release_ui_texture(host->context,out.handle)==0,"release failed");
    require(glIsTexture(retired)==GL_TRUE,"released before frame render");
    require(host->release_ui_texture(host->context,out.handle)==-2,"double release accepted");
    TCUiTexture file{sizeof(file),0,0,0,0,0};
    require(host->load_ui_texture(host->context,"native/images/色块.png",1,&file)==0,"Unicode PNG failed");
    require(file.width==2&&file.height==2,"PNG dimensions");verifyPixels(GLuint(file.renderer_id));
    require(host->release_ui_texture(host->context,file.handle)==0,"file release failed");
    require(image.load("native/images/色块.png",TextureFilter::Nearest)==0,"SDK image load");
    require(image.load("native/missing.png")==-4&&image.width()==2,"failed replacement destroyed old image");
    // Test per-owner handles and quotas using the same actual GL context.
    tc::ui_texture::Manager manager;manager.advance(100);
    int ownerA=0,ownerB=0;TCUiTexture first{sizeof(first),0,0,0,0,0};
    require(manager.create(&ownerA,&definition,&first)==0,"manager create");
    require(manager.release(&ownerB,first.handle)==-2,"foreign owner accepted");
    for(int i=1;i<64;++i){TCUiTexture t{sizeof(t),0,0,0,0,0};require(manager.create(&ownerA,&definition,&t)==0,"capacity too low");}
    TCUiTexture excess{sizeof(excess),0,0,0,0,0};
    require(manager.create(&ownerA,&definition,&excess)==-3,"capacity not enforced");
    manager.reject(&ownerA);require(glIsTexture(GLuint(first.renderer_id))==GL_TRUE,"reject deleted this frame");
    manager.advance(101);require(glIsTexture(GLuint(first.renderer_id))==GL_FALSE,"reject did not clean up");
    report("TEXTURE PASS load/Unicode/decode/GL pixels/state/ownership/capacity/thread/deferred release");
}
static void frame(void*,const TCFrame*){
    if(failed||drawnFrames>=180)return;
    try{
        if(drawnFrames==0)test();
        if(drawnFrames==1)require(glIsTexture(retired)==GL_FALSE,"retired texture survived next frame");
        if(auto window=panel("Texture regression",nullptr,{420,360},{100,100})){
            Canvas canvas("image",{320,240});
            auto* list=drawing_detail::table().windowList();
            const int before=read<int>(list,32);
            image.draw(canvas,{20,20},{220,220},{0,0},{1,1},rgba(255,255,255));
            require(read<int>(list,32)==before+4,"image vertices absent");
            auto* data=read<void*>(list,40);
            Vec2 uv=read<Vec2>(data,before*20+8);require(uv.x==0&&uv.y==0,"UV min ABI");
            uv=read<Vec2>(data,(before+2)*20+8);require(uv.x==1&&uv.y==1,"UV max ABI");
        }
        if(++drawnFrames==180){require(image.reset()==0,"SDK reset");report("TEXTURE PASS frames=180 image draw/UV and next-frame deletion");}
    }catch(const std::exception& e){failed=true;report(std::string("TEXTURE FAIL ")+e.what());}
}
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* out){
    host=h;if(!load(h)||!loadDrawing(h)||!loadTextures(h))return 1;
    out->on_frame=frame;return 0;
}
