#ifndef TC_COMPONENT_GEOMETRY_H
#define TC_COMPONENT_GEOMETRY_H

#include "tc_mod_api.h"

namespace tc::component_geometry {

using Api=TCComponentGeometryApiV1;
using ApiV2=TCComponentGeometryApiV2;
using ApiV3=TCComponentGeometryApiV3;

inline bool table(const TCHost* host,Api* out){
    if(!host||!out||!host->query_service)return false;
    Api queried{};
    if(host->query_service(host->context,TC_SERVICE_COMPONENT_GEOMETRY,
                           TC_COMPONENT_GEOMETRY_API_VERSION_1,&queried,
                           sizeof(queried))!=TC_SERVICE_OK)return false;
    if(queried.size<sizeof(queried)||queried.version!=TC_COMPONENT_GEOMETRY_API_VERSION_1||
       !queried.context||!queried.set_footprint||!queried.read_footprint)return false;
    *out=queried;return true;
}

inline bool tableV2(const TCHost* host,ApiV2* out){
    if(!host||!out||!host->query_service)return false;
    ApiV2 queried{};
    if(host->query_service(host->context,TC_SERVICE_COMPONENT_GEOMETRY,
                           TC_COMPONENT_GEOMETRY_API_VERSION_2,&queried,
                           sizeof(queried))!=TC_SERVICE_OK)return false;
    if(queried.size<sizeof(queried)||queried.version!=TC_COMPONENT_GEOMETRY_API_VERSION_2||
       !queried.context||!queried.set_footprint||!queried.read_footprint||
       !queried.set_instance_footprint)return false;
    *out=queried;return true;
}

inline bool tableV3(const TCHost* host,ApiV3* out){
    if(!host||!out||!host->query_service)return false;
    ApiV3 queried{};
    if(host->query_service(host->context,TC_SERVICE_COMPONENT_GEOMETRY,
                           TC_COMPONENT_GEOMETRY_API_VERSION_3,&queried,
                           sizeof(queried))!=TC_SERVICE_OK)return false;
    if(queried.size<sizeof(queried)||queried.version!=TC_COMPONENT_GEOMETRY_API_VERSION_3||
       !queried.context||!queried.set_footprint||!queried.read_footprint||
       !queried.set_instance_footprint||!queried.set_footprint_cells)return false;
    *out=queried;return true;
}

inline int setFootprint(const Api& api,uint64_t customId,float halfWidth,float halfHeight){
    return api.set_footprint?api.set_footprint(api.context,customId,halfWidth,halfHeight):
                             TC_COMPONENT_GEOMETRY_ERR_UNAVAILABLE;
}

inline int setFootprint(const ApiV2& api,uint64_t customId,float halfWidth,float halfHeight){
    return api.set_footprint?api.set_footprint(api.context,customId,halfWidth,halfHeight):
                             TC_COMPONENT_GEOMETRY_ERR_UNAVAILABLE;
}

/* The cell-rect form: the box is stored as asked, so a face that is not centred
   on the component's own cell declares its real top row instead of a half
   height that the host would round outwards. */
inline int setFootprintCells(const ApiV3& api,uint64_t customId,int x,int y,
                             int width,int height){
    return api.set_footprint_cells?
               api.set_footprint_cells(api.context,customId,x,y,
                                       static_cast<uint32_t>(width<0?0:width),
                                       static_cast<uint32_t>(height<0?0:height)):
               TC_COMPONENT_GEOMETRY_ERR_UNAVAILABLE;
}

inline int readFootprint(const Api& api,const TCGameHandle& component,
                         float* halfWidth,float* halfHeight){
    return api.read_footprint?api.read_footprint(api.context,&component,halfWidth,halfHeight):
                              TC_COMPONENT_GEOMETRY_ERR_UNAVAILABLE;
}

inline int readFootprint(const ApiV2& api,const TCGameHandle& component,
                         float* halfWidth,float* halfHeight){
    return api.read_footprint?api.read_footprint(api.context,&component,halfWidth,halfHeight):
                              TC_COMPONENT_GEOMETRY_ERR_UNAVAILABLE;
}

inline int setInstanceFootprint(const ApiV2& api,const TCGameHandle& component,
                                float halfWidth,float halfHeight){
    return api.set_instance_footprint?
               api.set_instance_footprint(api.context,&component,halfWidth,halfHeight):
               TC_COMPONENT_GEOMETRY_ERR_UNAVAILABLE;
}

inline const char* errorText(int status){
    switch(status){
    case TC_COMPONENT_GEOMETRY_OK:return "ok";
    case TC_COMPONENT_GEOMETRY_ERR_UNAVAILABLE:return "unavailable";
    case TC_COMPONENT_GEOMETRY_ERR_ARGUMENT:return "invalid argument";
    case TC_COMPONENT_GEOMETRY_ERR_UNKNOWN:return "unknown component";
    case TC_COMPONENT_GEOMETRY_ERR_OWNERSHIP:return "component type is not owned by this Mod";
    case TC_COMPONENT_GEOMETRY_ERR_RANGE:return "footprint is outside the supported range";
    case TC_COMPONENT_GEOMETRY_ERR_GAME:return "game rejected the footprint";
    case TC_COMPONENT_GEOMETRY_ERR_THREAD:return "wrong thread";
    case TC_COMPONENT_GEOMETRY_ERR_STALE:return "stale component handle";
    default:return "unknown error";
    }
}

} // namespace tc::component_geometry

#endif
