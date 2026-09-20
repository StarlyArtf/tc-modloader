#pragma once
#include "../sdk/tc_mod_api.h"
#include <windows.h>
#include <wincodec.h>
#include <GL/gl.h>
#include <algorithm>
#include <map>
#include <vector>
#include <limits>
#include <cstring>

namespace tc { namespace ui_texture {
inline bool pixelBytes(uint32_t width,uint32_t height,uint64_t& bytes) {
    if (!width || !height || width>4096 || height>4096) return false;
    bytes=uint64_t(width)*height*4;
    return true;
}
template<class T> struct Com {
    T* p=nullptr;
    ~Com(){if(p)p->Release();}
    T** out(){return &p;}
    T* operator->()const{return p;}
    Com()=default; Com(const Com&)=delete; Com& operator=(const Com&)=delete;
};
/* WIC decodes Unicode paths through the host's already validated byte buffer.
   Do not take over the game's COM apartment; RPC_E_CHANGED_MODE means its
   existing apartment can be used and must not be uninitialized here. */
inline bool decode(const void* data,size_t count,std::vector<unsigned char>& rgba,
                   uint32_t& width,uint32_t& height) {
    if(!data||!count||count>64u*1024u*1024u) return false;
    const HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(init)&&init!=RPC_E_CHANGED_MODE)return false;
    struct Apartment {bool owned;~Apartment(){if(owned)CoUninitialize();}} apartment{SUCCEEDED(init)};
    Com<IWICImagingFactory> factory;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
                              IID_IWICImagingFactory,reinterpret_cast<void**>(factory.out())))) return false;
    Com<IWICStream> stream;
    Com<IWICBitmapDecoder> decoder;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> converter;
    if(FAILED(factory->CreateStream(stream.out())) ||
       FAILED(stream->InitializeFromMemory((BYTE*)data,static_cast<DWORD>(count))) ||
       FAILED(factory->CreateDecoderFromStream(stream.p,nullptr,WICDecodeMetadataCacheOnDemand,decoder.out())) ||
       FAILED(decoder->GetFrame(0,frame.out()))) return false;
    UINT w=0,h=0; uint64_t bytes=0;
    if(FAILED(frame->GetSize(&w,&h))||!pixelBytes(w,h,bytes))return false;
    if(FAILED(factory->CreateFormatConverter(converter.out())) ||
       FAILED(converter->Initialize(frame.p,GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,
                                    nullptr,0,WICBitmapPaletteTypeCustom))) return false;
    std::vector<unsigned char> result(static_cast<size_t>(bytes));
    if(FAILED(converter->CopyPixels(nullptr,w*4,static_cast<UINT>(bytes),result.data())))return false;
    rgba=std::move(result);width=w;height=h;return true;
}

/* OpenGL entry points used by this game's renderer. Only glBindBuffer is
   newer than OpenGL 1.1. Resolve it in the current context on first use. */
struct GL {
    void (APIENTRY* bindBuffer)(GLenum,GLuint)=nullptr;
    HGLRC context=nullptr;
    bool ready() {
        HGLRC current=wglGetCurrentContext();
        if(!current)return false;
        if(current!=context) {
            auto p=wglGetProcAddress("glBindBuffer");
            const auto address=reinterpret_cast<uintptr_t>(p);
            if(address<=3 || address==std::numeric_limits<uintptr_t>::max())return false;
            std::memcpy(&bindBuffer,&p,sizeof(p)); context=current;
        }
        return bindBuffer!=nullptr;
    }
};
/* Upload must not inherit a game's pixel unpack buffer, row length or skip.
   It must also restore all touched state, including on an upload failure. */
struct UploadState {
    GL& gl;
    GLint binding=0,pbo=0,alignment=0,row=0,skipRows=0,skipPixels=0;
    explicit UploadState(GL& api):gl(api) {
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
        glGetIntegerv(0x88EF,&pbo); // GL_PIXEL_UNPACK_BUFFER_BINDING
        glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH,&row);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS,&skipRows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&skipPixels);
        gl.bindBuffer(0x88EC,0); // GL_PIXEL_UNPACK_BUFFER
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    }
    ~UploadState() {
        glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(binding));
        glPixelStorei(GL_UNPACK_ALIGNMENT,alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH,row);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,skipRows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS,skipPixels);
        gl.bindBuffer(0x88EC,static_cast<GLuint>(pbo));
    }
};

class Manager {
    struct Entry {void* owner;GLuint id;uint64_t bytes;int retired=-1;HGLRC context;};
    std::map<uint64_t,Entry> entries;
    uint64_t next=1;
    DWORD thread=0;
    int currentFrame=-1;
    GL gl;
public:
    /* Called only by the main-thread runtime, before invoking plugins. */
    void advance(int frame) {
        if(!thread)thread=GetCurrentThreadId();
        if(thread!=GetCurrentThreadId())return;
        currentFrame=frame;
        if(!gl.ready())return;
        for(auto i=entries.begin();i!=entries.end();) {
            if(i->second.retired>=0 && i->second.retired<frame && i->second.context==gl.context) {
                glDeleteTextures(1,&i->second.id);i=entries.erase(i);
            } else ++i;
        }
    }
    bool onThread()const {return thread && thread==GetCurrentThreadId();}
    int create(void* owner,const TCUiTexturePixels* pixels,TCUiTexture* out) {
        if(!onThread()||currentFrame<0||!gl.ready())return -1;
        if(!pixels||pixels->size<sizeof(*pixels)||!out||out->size<sizeof(*out)||
           !pixels->rgba||pixels->filter>1)return -2;
        uint64_t bytes=0;
        if(!pixelBytes(pixels->width,pixels->height,bytes)||pixels->byte_count<bytes)return -2;
        size_t owned=0;uint64_t ownedBytes=0;
        for(auto& item:entries)if(item.second.owner==owner){++owned;ownedBytes+=item.second.bytes;}
        if(owned>=64||ownedBytes+bytes>128u*1024u*1024u||!next)return -3;
        GLint maximum=0;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maximum);
        if(pixels->width>static_cast<uint32_t>(maximum)||pixels->height>static_cast<uint32_t>(maximum))return -3;
        // Allocate map storage BEFORE creating a GPU resource, so allocation
        // failure cannot orphan a texture.
        const uint64_t handle=next++;
        auto inserted=entries.emplace(handle,Entry{owner,0,bytes,-1,gl.context}).first;
        GLuint id=0;
        {
            UploadState restore(gl);
            glGenTextures(1,&id);
            if(id) {
                glBindTexture(GL_TEXTURE_2D,id);
                const GLint filter=pixels->filter?GL_NEAREST:GL_LINEAR;
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,filter);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,filter);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,0x812F); // CLAMP_TO_EDGE
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,0x812F);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,pixels->width,pixels->height,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels->rgba);
                GLint width=0,height=0;
                glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);
                glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&height);
                if(width!=static_cast<GLint>(pixels->width)||height!=static_cast<GLint>(pixels->height)) {
                    glDeleteTextures(1,&id);id=0;
                }
            }
        }
        if(!id){entries.erase(inserted);return -4;}
        inserted->second.id=id;
        *out=TCUiTexture{sizeof(TCUiTexture),pixels->width,pixels->height,0,handle,id};
        return 0;
    }
    int release(void* owner,uint64_t handle) {
        if(!onThread())return -1;
        auto i=entries.find(handle);
        if(i==entries.end()||i->second.owner!=owner||i->second.retired>=0)return -2;
        i->second.retired=currentFrame;return 0;
    }
    void reject(void* owner) {
        if(!onThread())return;
        for(auto& item:entries)if(item.second.owner==owner&&item.second.retired<0)item.second.retired=currentFrame;
    }
};
}}
