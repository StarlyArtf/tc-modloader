#pragma once
#include "tc_mod_api.h"

#ifdef __cplusplus
namespace tc::component_channel {

inline int load(const TCHost* host,TCComponentChannelApiV1* out){
    if(!host||!out||!host->query_service)return TC_SERVICE_ERR_ARGUMENT;
    *out={};
    return host->query_service(host->context,TC_SERVICE_COMPONENT_CHANNEL,
                               TC_COMPONENT_CHANNEL_API_VERSION_1,out,sizeof(*out));
}
inline int open(TCComponentChannelApiV1& api,const TCComponentInstanceHandle& instance,
                const TCComponentChannelConfigV1& config){
    return api.open?api.open(api.context,&instance,&config):TC_COMPONENT_CHANNEL_ERR_UNAVAILABLE;
}
inline int close(TCComponentChannelApiV1& api,const TCComponentInstanceHandle& instance){
    return api.close?api.close(api.context,&instance):TC_COMPONENT_CHANNEL_ERR_UNAVAILABLE;
}
inline int send(TCComponentChannelApiV1& api,const TCComponentInstanceHandle& instance,
                uint32_t direction,const void* data,uint32_t bytes,uint64_t* sequence=nullptr){
    return api.send?api.send(api.context,&instance,direction,data,bytes,sequence):
                    TC_COMPONENT_CHANNEL_ERR_UNAVAILABLE;
}
inline int receive(TCComponentChannelApiV1& api,const TCComponentInstanceHandle& instance,
                   uint32_t direction,void* out,uint32_t capacity,
                   TCComponentChannelMessageV1* message){
    return api.receive?api.receive(api.context,&instance,direction,out,capacity,message):
                       TC_COMPONENT_CHANNEL_ERR_UNAVAILABLE;
}
inline int publishSnapshot(TCComponentChannelApiV1& api,const TCComponentInstanceHandle& instance,
                           const void* data,uint32_t bytes,uint64_t* revision=nullptr){
    return api.publish_snapshot?api.publish_snapshot(api.context,&instance,data,bytes,revision):
                                TC_COMPONENT_CHANNEL_ERR_UNAVAILABLE;
}
inline int readSnapshot(TCComponentChannelApiV1& api,const TCComponentInstanceHandle& instance,
                        uint64_t knownRevision,void* out,uint32_t capacity,uint32_t* bytes,
                        uint64_t* revision){
    return api.read_snapshot?api.read_snapshot(api.context,&instance,knownRevision,out,capacity,
                                               bytes,revision):TC_COMPONENT_CHANNEL_ERR_UNAVAILABLE;
}

} // namespace tc::component_channel
#endif
