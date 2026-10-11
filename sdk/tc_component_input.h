#pragma once
#include "tc_mod_api.h"

#ifdef __cplusplus
namespace tc::component_input {

inline int load(const TCHost* host,TCComponentInputApiV1* out){
    if(!host||!out||!host->query_service)return TC_SERVICE_ERR_ARGUMENT;
    *out={};
    return host->query_service(host->context,TC_SERVICE_COMPONENT_INPUT,
                               TC_COMPONENT_INPUT_API_VERSION_1,out,sizeof(*out));
}
inline int setHandler(TCComponentInputApiV1& api,uint64_t customId,
                      float minX,float minY,float maxX,float maxY,
                      TCComponentInputCallbackV1 callback,void* user=nullptr){
    return api.set_handler?api.set_handler(api.context,customId,minX,minY,maxX,maxY,callback,user):
                           TC_COMPONENT_INPUT_ERR_UNAVAILABLE;
}
inline int clearHandler(TCComponentInputApiV1& api,uint64_t customId){
    return api.clear_handler?api.clear_handler(api.context,customId):TC_COMPONENT_INPUT_ERR_UNAVAILABLE;
}
inline int route(TCComponentInputApiV1& api,const TCComponentInstanceHandle& instance,
                 uint32_t requests){
    return api.route?api.route(api.context,&instance,requests):TC_COMPONENT_INPUT_ERR_UNAVAILABLE;
}
inline int state(TCComponentInputApiV1& api,const TCComponentInstanceHandle& instance,
                 TCComponentInputStateV1* out){
    return api.state?api.state(api.context,&instance,out):TC_COMPONENT_INPUT_ERR_UNAVAILABLE;
}

} // namespace tc::component_input
#endif
