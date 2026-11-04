#pragma once

#include "../sdk/tc_service_api.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <functional>
#include <vector>

namespace tc::component_channel {

struct Key {
    const void* owner=nullptr;
    uint64_t customId=0;
    uint64_t instanceId=0;
    uint64_t generation=0;
    bool operator<(const Key& other) const {
        if(owner!=other.owner)return std::less<const void*>{}(owner,other.owner);
        if(customId!=other.customId)return customId<other.customId;
        if(instanceId!=other.instanceId)return instanceId<other.instanceId;
        return generation<other.generation;
    }
};

inline Key key(const void* owner,const TCComponentInstanceHandle& handle){
    return {owner,handle.custom_id,handle.instance_id,handle.generation};
}

struct Message { uint64_t sequence=0;std::vector<uint8_t> bytes; };
struct Queue { std::deque<Message> items;uint64_t sent=0,dropped=0; };

struct Channel {
    std::mutex mutex;
    TCComponentChannelConfigV1 config{};
    Queue renderToSim,simToRender;
    std::vector<uint8_t> snapshots[2];
    uint32_t readableSnapshot=0;
    uint64_t snapshotRevision=0;
    uint64_t nextSequence=1;
};

class Store {
    std::mutex tableMutex;
    std::map<Key,std::shared_ptr<Channel>> channels;

    std::shared_ptr<Channel> find(const Key& wanted,bool& busy){
        std::unique_lock<std::mutex> lock(tableMutex,std::try_to_lock);
        if(!lock.owns_lock()){busy=true;return nullptr;}
        busy=false;
        const auto it=channels.find(wanted);
        return it==channels.end()?nullptr:it->second;
    }

public:
    int open(const void* owner,const TCComponentInstanceHandle& handle,
             const TCComponentChannelConfigV1& requested){
        if(!owner||requested.size<sizeof(requested)||
           requested.version!=TC_COMPONENT_CHANNEL_CONFIG_VERSION_1||
           requested.message_capacity<1||requested.message_capacity>256||
           requested.message_bytes<1||requested.message_bytes>65536||
           requested.snapshot_bytes>1048576||
           requested.drop_policy>TC_COMPONENT_CHANNEL_DROP_OLDEST)
            return TC_COMPONENT_CHANNEL_ERR_ARGUMENT;
        std::unique_lock<std::mutex> lock(tableMutex,std::try_to_lock);
        if(!lock.owns_lock())return TC_COMPONENT_CHANNEL_ERR_BUSY;
        const Key wanted=key(owner,handle);
        if(channels.count(wanted))return TC_COMPONENT_CHANNEL_ERR_STATE;
        auto channel=std::make_shared<Channel>();
        channel->config=requested;
        channel->config.size=sizeof(channel->config);
        channel->snapshots[0].reserve(requested.snapshot_bytes);
        channel->snapshots[1].reserve(requested.snapshot_bytes);
        channels.emplace(wanted,std::move(channel));
        return TC_COMPONENT_CHANNEL_OK;
    }

    int close(const void* owner,const TCComponentInstanceHandle& handle){
        std::unique_lock<std::mutex> lock(tableMutex,std::try_to_lock);
        if(!lock.owns_lock())return TC_COMPONENT_CHANNEL_ERR_BUSY;
        return channels.erase(key(owner,handle))?TC_COMPONENT_CHANNEL_OK:
                                                  TC_COMPONENT_CHANNEL_ERR_STATE;
    }

    int send(const void* owner,const TCComponentInstanceHandle& handle,uint32_t direction,
             const void* data,uint32_t bytes,uint64_t* sequence){
        bool tableBusy=false;auto channel=find(key(owner,handle),tableBusy);
        if(tableBusy)return TC_COMPONENT_CHANNEL_ERR_BUSY;
        if(!channel)return TC_COMPONENT_CHANNEL_ERR_STATE;
        std::unique_lock<std::mutex> lock(channel->mutex,std::try_to_lock);
        if(!lock.owns_lock())return TC_COMPONENT_CHANNEL_ERR_BUSY;
        if(direction!=TC_COMPONENT_CHANNEL_RENDER_TO_SIM&&
           direction!=TC_COMPONENT_CHANNEL_SIM_TO_RENDER)return TC_COMPONENT_CHANNEL_ERR_ARGUMENT;
        if(!data||!bytes)return TC_COMPONENT_CHANNEL_ERR_ARGUMENT;
        if(bytes>channel->config.message_bytes)return TC_COMPONENT_CHANNEL_ERR_SIZE;
        Queue& queue=direction==TC_COMPONENT_CHANNEL_RENDER_TO_SIM?
                     channel->renderToSim:channel->simToRender;
        const uint64_t assigned=channel->nextSequence++;
        if(queue.items.size()>=channel->config.message_capacity){
            ++queue.dropped;
            if(channel->config.drop_policy==TC_COMPONENT_CHANNEL_DROP_NEWEST){
                if(sequence)*sequence=assigned;
                return TC_COMPONENT_CHANNEL_ERR_FULL;
            }
            queue.items.pop_front();
        }
        Message message;message.sequence=assigned;
        const auto* first=static_cast<const uint8_t*>(data);
        message.bytes.assign(first,first+bytes);
        queue.items.push_back(std::move(message));++queue.sent;
        if(sequence)*sequence=assigned;
        return TC_COMPONENT_CHANNEL_OK;
    }

    int receive(const void* owner,const TCComponentInstanceHandle& handle,uint32_t direction,
                void* out,uint32_t capacity,TCComponentChannelMessageV1* metadata){
        bool tableBusy=false;auto channel=find(key(owner,handle),tableBusy);
        if(tableBusy)return TC_COMPONENT_CHANNEL_ERR_BUSY;
        if(!channel)return TC_COMPONENT_CHANNEL_ERR_STATE;
        std::unique_lock<std::mutex> lock(channel->mutex,std::try_to_lock);
        if(!lock.owns_lock())return TC_COMPONENT_CHANNEL_ERR_BUSY;
        if(direction!=TC_COMPONENT_CHANNEL_RENDER_TO_SIM&&
           direction!=TC_COMPONENT_CHANNEL_SIM_TO_RENDER)return TC_COMPONENT_CHANNEL_ERR_ARGUMENT;
        if(!metadata||metadata->size<sizeof(*metadata))return TC_COMPONENT_CHANNEL_ERR_ARGUMENT;
        Queue& queue=direction==TC_COMPONENT_CHANNEL_RENDER_TO_SIM?
                     channel->renderToSim:channel->simToRender;
        if(queue.items.empty())return TC_COMPONENT_CHANNEL_ERR_EMPTY;
        const Message& front=queue.items.front();
        TCComponentChannelMessageV1 value{};
        value.size=sizeof(value);value.version=1;value.sequence=front.sequence;
        value.bytes=static_cast<uint32_t>(front.bytes.size());*metadata=value;
        if(!out&&capacity==0)return TC_COMPONENT_CHANNEL_OK;
        if(!out)return TC_COMPONENT_CHANNEL_ERR_ARGUMENT;
        if(capacity<front.bytes.size())return TC_COMPONENT_CHANNEL_ERR_SIZE;
        std::memcpy(out,front.bytes.data(),front.bytes.size());queue.items.pop_front();
        return TC_COMPONENT_CHANNEL_OK;
    }

    int publish(const void* owner,const TCComponentInstanceHandle& handle,
                const void* data,uint32_t bytes,uint64_t* revision){
        bool tableBusy=false;auto channel=find(key(owner,handle),tableBusy);
        if(tableBusy)return TC_COMPONENT_CHANNEL_ERR_BUSY;
        if(!channel)return TC_COMPONENT_CHANNEL_ERR_STATE;
        std::unique_lock<std::mutex> lock(channel->mutex,std::try_to_lock);
        if(!lock.owns_lock())return TC_COMPONENT_CHANNEL_ERR_BUSY;
        if(bytes>channel->config.snapshot_bytes)return TC_COMPONENT_CHANNEL_ERR_SIZE;
        if(bytes&&!data)return TC_COMPONENT_CHANNEL_ERR_ARGUMENT;
        const uint32_t write=channel->readableSnapshot^1u;
        const auto* first=static_cast<const uint8_t*>(data);
        if(bytes)channel->snapshots[write].assign(first,first+bytes);
        else channel->snapshots[write].clear();
        channel->readableSnapshot=write;
        ++channel->snapshotRevision;if(!channel->snapshotRevision)++channel->snapshotRevision;
        if(revision)*revision=channel->snapshotRevision;
        return TC_COMPONENT_CHANNEL_OK;
    }

    int read(const void* owner,const TCComponentInstanceHandle& handle,uint64_t knownRevision,
             void* out,uint32_t capacity,uint32_t* bytes,uint64_t* revision){
        bool tableBusy=false;auto channel=find(key(owner,handle),tableBusy);
        if(tableBusy)return TC_COMPONENT_CHANNEL_ERR_BUSY;
        if(!channel)return TC_COMPONENT_CHANNEL_ERR_STATE;
        std::unique_lock<std::mutex> lock(channel->mutex,std::try_to_lock);
        if(!lock.owns_lock())return TC_COMPONENT_CHANNEL_ERR_BUSY;
        if(!channel->snapshotRevision)return TC_COMPONENT_CHANNEL_ERR_EMPTY;
        const auto& snapshot=channel->snapshots[channel->readableSnapshot];
        if(bytes)*bytes=static_cast<uint32_t>(snapshot.size());
        if(revision)*revision=channel->snapshotRevision;
        if(knownRevision==channel->snapshotRevision)return TC_COMPONENT_CHANNEL_OK;
        if(!out&&capacity==0)return TC_COMPONENT_CHANNEL_OK;
        if(!out)return TC_COMPONENT_CHANNEL_ERR_ARGUMENT;
        if(capacity<snapshot.size())return TC_COMPONENT_CHANNEL_ERR_SIZE;
        if(!snapshot.empty())std::memcpy(out,snapshot.data(),snapshot.size());
        return TC_COMPONENT_CHANNEL_OK;
    }

    int stats(const void* owner,const TCComponentInstanceHandle& handle,
              TCComponentChannelStatsV1* out){
        if(!out||out->size<sizeof(*out))return TC_COMPONENT_CHANNEL_ERR_ARGUMENT;
        bool tableBusy=false;auto channel=find(key(owner,handle),tableBusy);
        if(tableBusy)return TC_COMPONENT_CHANNEL_ERR_BUSY;
        if(!channel)return TC_COMPONENT_CHANNEL_ERR_STATE;
        std::unique_lock<std::mutex> lock(channel->mutex,std::try_to_lock);
        if(!lock.owns_lock())return TC_COMPONENT_CHANNEL_ERR_BUSY;
        TCComponentChannelStatsV1 value{};value.size=sizeof(value);value.version=1;
        value.render_to_sim_queued=static_cast<uint32_t>(channel->renderToSim.items.size());
        value.sim_to_render_queued=static_cast<uint32_t>(channel->simToRender.items.size());
        value.render_to_sim_sent=channel->renderToSim.sent;
        value.sim_to_render_sent=channel->simToRender.sent;
        value.render_to_sim_dropped=channel->renderToSim.dropped;
        value.sim_to_render_dropped=channel->simToRender.dropped;
        value.snapshot_revision=channel->snapshotRevision;
        value.snapshot_bytes=channel->snapshotRevision?
            static_cast<uint32_t>(channel->snapshots[channel->readableSnapshot].size()):0;
        *out=value;return TC_COMPONENT_CHANNEL_OK;
    }

    void clearOwner(const void* owner){
        std::lock_guard<std::mutex> lock(tableMutex);
        for(auto it=channels.begin();it!=channels.end();)
            if(it->first.owner==owner)it=channels.erase(it);else ++it;
    }
    template<class Predicate> void eraseIf(Predicate predicate){
        std::vector<Key> snapshot;
        {
            std::lock_guard<std::mutex> lock(tableMutex);
            snapshot.reserve(channels.size());
            for(const auto& item:channels)snapshot.push_back(item.first);
        }
        for(const Key& candidate:snapshot){
            if(!predicate(candidate))continue;
            std::lock_guard<std::mutex> lock(tableMutex);
            channels.erase(candidate);
        }
    }
    void clear(){std::lock_guard<std::mutex> lock(tableMutex);channels.clear();}
    size_t size(){std::lock_guard<std::mutex> lock(tableMutex);return channels.size();}
};

} // namespace tc::component_channel
