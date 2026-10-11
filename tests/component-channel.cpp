#include "../src/component_channel.hpp"
#include "../sdk/tc_component_channel.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>

int main(){
    tc::component_channel::Store store;
    int ownerA=0,ownerB=0;
    TCComponentInstanceHandle handle{sizeof(handle),1,0x44,7,3};
    TCComponentChannelConfigV1 config{sizeof(config),1,2,8,16,TC_COMPONENT_CHANNEL_DROP_NEWEST};
    assert(store.open(&ownerA,handle,config)==TC_COMPONENT_CHANNEL_OK);
    assert(store.open(&ownerA,handle,config)==TC_COMPONENT_CHANNEL_ERR_STATE);
    const char one[]="one",two[]="two",three[]="three";uint64_t sequence=0;
    assert(store.send(&ownerA,handle,TC_COMPONENT_CHANNEL_RENDER_TO_SIM,one,4,&sequence)==0&&sequence==1);
    assert(store.send(&ownerA,handle,TC_COMPONENT_CHANNEL_RENDER_TO_SIM,two,4,&sequence)==0&&sequence==2);
    assert(store.send(&ownerA,handle,TC_COMPONENT_CHANNEL_RENDER_TO_SIM,three,6,&sequence)==TC_COMPONENT_CHANNEL_ERR_FULL&&sequence==3);
    TCComponentChannelMessageV1 message{};message.size=sizeof(message);
    assert(store.receive(&ownerA,handle,TC_COMPONENT_CHANNEL_RENDER_TO_SIM,nullptr,0,&message)==0);
    assert(message.sequence==1&&message.bytes==4);
    char out[8]{};message.size=sizeof(message);
    assert(store.receive(&ownerA,handle,TC_COMPONENT_CHANNEL_RENDER_TO_SIM,out,3,&message)==TC_COMPONENT_CHANNEL_ERR_SIZE);
    message.size=sizeof(message);
    assert(store.receive(&ownerA,handle,TC_COMPONENT_CHANNEL_RENDER_TO_SIM,out,sizeof(out),&message)==0);
    assert(std::strcmp(out,"one")==0);
    TCComponentChannelStatsV1 stats{};stats.size=sizeof(stats);
    assert(store.stats(&ownerA,handle,&stats)==0);
    assert(stats.render_to_sim_queued==1&&stats.render_to_sim_sent==2&&stats.render_to_sim_dropped==1);
    uint64_t revision=0;const uint32_t snapshot=0x12345678;
    assert(store.publish(&ownerA,handle,&snapshot,sizeof(snapshot),&revision)==0&&revision==1);
    uint32_t copied=0,bytes=0;uint64_t readRevision=0;
    assert(store.read(&ownerA,handle,0,&copied,sizeof(copied),&bytes,&readRevision)==0);
    assert(copied==snapshot&&bytes==sizeof(snapshot)&&readRevision==1);
    copied=0;assert(store.read(&ownerA,handle,1,&copied,sizeof(copied),&bytes,&readRevision)==0&&copied==0);
    assert(store.send(&ownerB,handle,TC_COMPONENT_CHANNEL_RENDER_TO_SIM,one,4,nullptr)==TC_COMPONENT_CHANNEL_ERR_STATE);
    assert(store.close(&ownerA,handle)==0&&store.size()==0);

    config.drop_policy=TC_COMPONENT_CHANNEL_DROP_OLDEST;
    assert(store.open(&ownerA,handle,config)==0);
    assert(store.send(&ownerA,handle,1,one,4,nullptr)==0);
    assert(store.send(&ownerA,handle,1,two,4,nullptr)==0);
    assert(store.send(&ownerA,handle,1,three,6,nullptr)==0);
    message.size=sizeof(message);std::memset(out,0,sizeof(out));
    assert(store.receive(&ownerA,handle,1,out,sizeof(out),&message)==0&&std::strcmp(out,"two")==0);
    store.clearOwner(&ownerA);assert(store.open(&ownerA,handle,config)==0);
    for(unsigned i=0;i<1024;++i)assert(store.send(&ownerA,handle,1,&i,sizeof(i),nullptr)==0);
    stats={};stats.size=sizeof(stats);assert(store.stats(&ownerA,handle,&stats)==0);
    assert(stats.render_to_sim_queued==2&&stats.render_to_sim_sent==1024&&
           stats.render_to_sim_dropped==1022);
    store.clearOwner(&ownerA);config.message_capacity=256;config.drop_policy=TC_COMPONENT_CHANNEL_DROP_NEWEST;
    assert(store.open(&ownerA,handle,config)==0);
    constexpr uint32_t pressure=5000;
    std::thread producer([&]{
        for(uint32_t value=0;value<pressure;){
            const int result=store.send(&ownerA,handle,TC_COMPONENT_CHANNEL_RENDER_TO_SIM,
                                        &value,sizeof(value),nullptr);
            if(result==TC_COMPONENT_CHANNEL_OK)++value;
            else {assert(result==TC_COMPONENT_CHANNEL_ERR_FULL||result==TC_COMPONENT_CHANNEL_ERR_BUSY);std::this_thread::yield();}
        }
    });
    for(uint32_t expected=0;expected<pressure;){
        uint32_t value=~0u;message={};message.size=sizeof(message);
        const int result=store.receive(&ownerA,handle,TC_COMPONENT_CHANNEL_RENDER_TO_SIM,
                                       &value,sizeof(value),&message);
        if(result==TC_COMPONENT_CHANNEL_OK){assert(value==expected);++expected;}
        else {assert(result==TC_COMPONENT_CHANNEL_ERR_EMPTY||result==TC_COMPONENT_CHANNEL_ERR_BUSY);std::this_thread::yield();}
    }
    producer.join();
    store.clearOwner(&ownerA);assert(store.size()==0);
    std::cout<<"component-channel tests passed\n";
}
