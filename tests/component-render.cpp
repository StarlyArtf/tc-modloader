#include "../sdk/tc_component_render.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

static void require(bool value,const char* message){
    if(!value){std::cerr<<message<<'\n';std::exit(1);}
}

static int setCallback(void*,uint64_t,TCComponentRenderCallbackV1,void*){
    return TC_COMPONENT_RENDER_OK;
}
static int setDefault(void*,uint64_t,int){return TC_COMPONENT_RENDER_OK;}
static int setHint(void*,uint64_t,int){return TC_COMPONENT_RENDER_OK;}
static int setFoundry(void*,uint64_t,int){return TC_COMPONENT_RENDER_OK;}
static int setPicture(void*,uint64_t,const char*){return TC_COMPONENT_RENDER_OK;}
static int setPlacementPreview(void*,uint64_t,int){return TC_COMPONENT_RENDER_OK;}

static int query(void*,const char* id,uint32_t version,void* out,uint32_t size){
    if(!id||std::string(id)!=TC_SERVICE_COMPONENT_RENDER)return TC_SERVICE_ERR_VERSION;
    if(version==TC_COMPONENT_RENDER_API_VERSION_1){
        if(size<sizeof(TCComponentRenderApiV1))return TC_SERVICE_ERR_SIZE;
        *static_cast<TCComponentRenderApiV1*>(out)=TCComponentRenderApiV1{
            sizeof(TCComponentRenderApiV1),TC_COMPONENT_RENDER_API_VERSION_1,
            reinterpret_cast<void*>(1),&setCallback};
        return TC_SERVICE_OK;
    }
    if(version==TC_COMPONENT_RENDER_API_VERSION_2){
        if(size<sizeof(TCComponentRenderApiV2))return TC_SERVICE_ERR_SIZE;
        *static_cast<TCComponentRenderApiV2*>(out)=TCComponentRenderApiV2{
            sizeof(TCComponentRenderApiV2),TC_COMPONENT_RENDER_API_VERSION_2,
            reinterpret_cast<void*>(1),&setCallback,&setDefault};
        return TC_SERVICE_OK;
    }
    if(version==TC_COMPONENT_RENDER_API_VERSION_3){
        if(size<sizeof(TCComponentRenderApiV3))return TC_SERVICE_ERR_SIZE;
        *static_cast<TCComponentRenderApiV3*>(out)=TCComponentRenderApiV3{
            sizeof(TCComponentRenderApiV3),TC_COMPONENT_RENDER_API_VERSION_3,
            reinterpret_cast<void*>(1),&setCallback,&setDefault,&setHint};
        return TC_SERVICE_OK;
    }
    if(version==TC_COMPONENT_RENDER_API_VERSION_4){
        if(size<sizeof(TCComponentRenderApiV4))return TC_SERVICE_ERR_SIZE;
        *static_cast<TCComponentRenderApiV4*>(out)=TCComponentRenderApiV4{
            sizeof(TCComponentRenderApiV4),TC_COMPONENT_RENDER_API_VERSION_4,
            reinterpret_cast<void*>(1),&setCallback,&setDefault,&setHint,&setFoundry};
        return TC_SERVICE_OK;
    }
    if(version==TC_COMPONENT_RENDER_API_VERSION_5){
        if(size<sizeof(TCComponentRenderApiV5))return TC_SERVICE_ERR_SIZE;
        *static_cast<TCComponentRenderApiV5*>(out)=TCComponentRenderApiV5{
            sizeof(TCComponentRenderApiV5),TC_COMPONENT_RENDER_API_VERSION_5,
            reinterpret_cast<void*>(1),&setCallback,&setDefault,&setHint,&setFoundry,
            &setPicture};
        return TC_SERVICE_OK;
    }
    if(version==TC_COMPONENT_RENDER_API_VERSION_6){
        if(size<sizeof(TCComponentRenderApiV6))return TC_SERVICE_ERR_SIZE;
        *static_cast<TCComponentRenderApiV6*>(out)=TCComponentRenderApiV6{
            sizeof(TCComponentRenderApiV6),TC_COMPONENT_RENDER_API_VERSION_6,
            reinterpret_cast<void*>(1),&setCallback,&setDefault,&setHint,&setFoundry,
            &setPicture,&setPlacementPreview};
        return TC_SERVICE_OK;
    }
    return TC_SERVICE_ERR_VERSION;
}

int main(){
    TCHost host{};host.size=sizeof(host);host.api_version=TC_MOD_API_VERSION;
    host.query_service=&query;
    TCComponentRenderApiV1 api{};
    require(tc::component_render::table(&host,&api),"render table query failed");
    require(tc::component_render::setDrawCallback(api,42,nullptr)==TC_COMPONENT_RENDER_OK,
            "render callback wrapper failed");
    TCComponentRenderApiV2 apiV2{};
    require(tc::component_render::tableV2(&host,&apiV2),"render V2 table query failed");
    require(tc::component_render::setDrawCallback(apiV2,42,nullptr)==TC_COMPONENT_RENDER_OK,
            "render V2 callback wrapper failed");
    require(tc::component_render::setDefaultDrawing(apiV2,42,false)==TC_COMPONENT_RENDER_OK,
            "render V2 default drawing wrapper failed");
    TCComponentRenderApiV3 apiV3{};
    require(tc::component_render::tableV3(&host,&apiV3),"render V3 table query failed");
    require(tc::component_render::setSelectionHint(apiV3,42,false)==TC_COMPONENT_RENDER_OK,
            "render V3 selection hint wrapper failed");
    require(tc::component_render::tableV2(&host,&apiV2)&&
                sizeof(TCComponentRenderApiV3)>sizeof(TCComponentRenderApiV2)&&
                offsetof(TCComponentRenderApiV3,set_selection_hint)>=
                    sizeof(TCComponentRenderApiV2),
            "render V3 does not extend the V2 prefix");

    TCComponentRenderApiV4 apiV4{};
    require(tc::component_render::tableV4(&host,&apiV4),"render V4 table query failed");
    require(tc::component_render::setFoundryButton(apiV4,42,false)==TC_COMPONENT_RENDER_OK,
            "render V4 foundry button wrapper failed");
    require(tc::component_render::setFoundryButton(apiV4,42,true)==TC_COMPONENT_RENDER_OK,
            "render V4 foundry button restore wrapper failed");
    require(offsetof(TCComponentRenderApiV4,set_foundry_button)>=sizeof(TCComponentRenderApiV3)&&
                sizeof(TCComponentRenderApiV4)>sizeof(TCComponentRenderApiV3),
            "render V4 does not extend the V3 prefix");

    TCComponentRenderApiV5 apiV5{};
    require(tc::component_render::tableV5(&host,&apiV5),"render V5 table query failed");
    require(tc::component_render::setPicture(apiV5,42,"C:\\icons\\constant.png")==
                TC_COMPONENT_RENDER_OK,
            "render V5 picture wrapper failed");
    require(tc::component_render::setPicture(apiV5,42,nullptr)==TC_COMPONENT_RENDER_OK,
            "render V5 picture clear wrapper failed");
    require(offsetof(TCComponentRenderApiV5,set_picture)>=sizeof(TCComponentRenderApiV4)&&
                sizeof(TCComponentRenderApiV5)>sizeof(TCComponentRenderApiV4),
            "render V5 does not extend the V4 prefix");

    TCComponentRenderApiV6 apiV6{};
    require(tc::component_render::tableV6(&host,&apiV6),"render V6 table query failed");
    require(tc::component_render::setPlacementPreview(apiV6,42,true)==
                TC_COMPONENT_RENDER_OK,
            "render V6 placement-preview wrapper failed");
    require(tc::component_render::setPlacementPreview(apiV6,42,false)==
                TC_COMPONENT_RENDER_OK,
            "render V6 placement-preview restore wrapper failed");
    require(offsetof(TCComponentRenderApiV6,set_placement_preview)>=
                sizeof(TCComponentRenderApiV5)&&
                sizeof(TCComponentRenderApiV6)>sizeof(TCComponentRenderApiV5),
            "render V6 does not extend the V5 prefix");

    TCComponentRenderFrameV1 frame{};
    frame.origin_x=100.f;frame.origin_y=200.f;
    frame.axis_x_x=0.f;frame.axis_x_y=10.f;
    frame.axis_y_x=-10.f;frame.axis_y_y=0.f;
    float x=0.f,y=0.f;
    tc::component_render::localToScreen(frame,2.f,3.f,&x,&y);
    require(std::abs(x-70.f)<0.001f&&std::abs(y-220.f)<0.001f,
            "affine local-to-screen mapping failed");
    require(TC_COMPONENT_RENDER_MAX_COMMANDS_PER_INSTANCE==4096u,
            "render command quota changed");
    std::cout<<"PASS component render SDK tables, placement preview, picture and affine mapping\n";
}
