#pragma once
/* The bookkeeping behind tc.component.render V7: the images a Mod's own draw
   callback can paint.

   The loader owns the GL objects through ui_texture::Manager; this header owns
   only the *names* and the lifetime rules, so tests/component-images.cpp can pin
   them offline:

   * the key is the Mod's own name for the image (lowercase `a-z 0-9 . _ -`, at
     most 64 bytes) and is what load/release take;
   * the value a draw call names is a loader-wide unique handle - never 0 - that
     stays the same when the same key is loaded again, while `revision` goes up
     so a Mod can notice that the pixels changed;
   * replacing retires the previous texture through the caller's callback, and
     dropping a key (or the whole table) does the same, so nothing is left to the
     Mod to free;
   * a per-Mod quota counts images and decoded bytes, and a refused call changes
     nothing at all (the caller then retires the texture it had just created).

   Tables are per Mod: a handle from another Mod's table simply is not found,
   which is the "resources are isolated per Mod" rule from the plan. */

#include "../sdk/tc_service_api.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace tc::component_images {

inline constexpr size_t kMaxKeyBytes = 64;

inline bool validKey(const std::string& key) {
    if (key.empty() || key.size() > kMaxKeyBytes) return false;
    for (char c : key)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            return false;
    return true;
}

struct Image {
    uint64_t image = 0;      /* what a draw call names */
    uint64_t texture = 0;    /* ui_texture::Manager handle, for release */
    uint64_t renderer = 0;   /* the renderer id the draw call paints with */
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t revision = 1;
    uint64_t bytes = 0;
    uint64_t customId = 0;
    uint64_t instanceId = 0; /* 0 = Mod-wide; otherwise generation-scoped */
    uint64_t generation = 0;
};

struct Key {
    uint64_t customId = 0;
    uint64_t instanceId = 0;
    uint64_t generation = 0;
    std::string name;
    bool operator<(const Key& other) const {
        if (customId != other.customId) return customId < other.customId;
        if (instanceId != other.instanceId) return instanceId < other.instanceId;
        if (generation != other.generation) return generation < other.generation;
        return name < other.name;
    }
};

class Table {
public:
    /* Retires one texture.  The loader passes ui_texture::Manager::release, the
       offline test counts calls. */
    using Retire = std::function<void(uint64_t texture)>;

    /* Adds `key`, or replaces the image already stored under it.  On success
       `out` receives the entry (with its stable handle and the new revision) and
       any previous texture has been retired.  On failure nothing changes. */
    int load(const std::string& key, uint64_t texture, uint64_t renderer, uint32_t width,
             uint32_t height, uint64_t bytes, uint64_t* nextImage, const Retire& retire,
             Image* out) {
        return loadScoped(nullptr,key,texture,renderer,width,height,bytes,nextImage,retire,out);
    }

    int loadScoped(const TCComponentInstanceHandle* instance,const std::string& key,
             uint64_t texture, uint64_t renderer, uint32_t width,
             uint32_t height, uint64_t bytes, uint64_t* nextImage, const Retire& retire,
             Image* out) {
        if (!out || !nextImage) return TC_COMPONENT_RENDER_ERR_ARGUMENT;
        if (!validKey(key)) return TC_COMPONENT_RENDER_ERR_ARGUMENT;
        if (!texture || !width || !height || !bytes ||
            bytes > TC_COMPONENT_RENDER_MAX_IMAGE_BYTES)
            return TC_COMPONENT_RENDER_ERR_IMAGE;
        const Key scoped{instance?instance->custom_id:0,instance?instance->instance_id:0,
                         instance?instance->generation:0,key};
        const auto found = byKey.find(scoped);
        if (found == byKey.end()) {
            if (byKey.size() >= TC_COMPONENT_RENDER_MAX_IMAGES ||
                storedBytes + bytes > TC_COMPONENT_RENDER_MAX_IMAGE_BYTES)
                return TC_COMPONENT_RENDER_ERR_IMAGE;
            const uint64_t handle = allocateHandle(nextImage);
            if (!handle) return TC_COMPONENT_RENDER_ERR_IMAGE;
            Image entry;
            entry.image = handle;
            entry.texture = texture;
            entry.renderer = renderer;
            entry.width = width;
            entry.height = height;
            entry.bytes = bytes;
            entry.customId=scoped.customId;
            entry.instanceId=scoped.instanceId;
            entry.generation=scoped.generation;
            byKey.emplace(scoped, entry);
            storedBytes += bytes;
            *out = entry;
            return TC_COMPONENT_RENDER_OK;
        }
        /* Replacing keeps the handle a Mod already drew with and only moves the
           pixels, so the quota being checked is the byte total, not the count. */
        if (storedBytes - found->second.bytes + bytes >
            TC_COMPONENT_RENDER_MAX_IMAGE_BYTES)
            return TC_COMPONENT_RENDER_ERR_IMAGE;
        const uint64_t previous = found->second.texture;
        storedBytes -= found->second.bytes;
        found->second.texture = texture;
        found->second.renderer = renderer;
        found->second.width = width;
        found->second.height = height;
        found->second.bytes = bytes;
        ++found->second.revision;
        storedBytes += bytes;
        if (retire) retire(previous);
        *out = found->second;
        return TC_COMPONENT_RENDER_OK;
    }

    /* Drops one key, or every key when `key` is empty. */
    int clear(const std::string& key, const Retire& retire) {
        if(key.empty()) {
            for(const auto& item:byKey)if(retire)retire(item.second.texture);
            byKey.clear();storedBytes=0;return TC_COMPONENT_RENDER_OK;
        }
        return clearScoped(nullptr,key,retire);
    }

    int clearScoped(const TCComponentInstanceHandle* instance,const std::string& key,
                    const Retire& retire) {
        const uint64_t customId=instance?instance->custom_id:0;
        const uint64_t instanceId=instance?instance->instance_id:0;
        const uint64_t generation=instance?instance->generation:0;
        if (key.empty()) {
            for (auto it=byKey.begin();it!=byKey.end();) {
                if (it->first.customId==customId&&it->first.instanceId==instanceId&&
                    it->first.generation==generation) {
                    if(retire)retire(it->second.texture);
                    storedBytes-=it->second.bytes;it=byKey.erase(it);
                } else ++it;
            }
            return TC_COMPONENT_RENDER_OK;
        }
        const auto found = byKey.find(Key{customId,instanceId,generation,key});
        if (found == byKey.end()) return TC_COMPONENT_RENDER_ERR_IMAGE;
        if (retire) retire(found->second.texture);
        storedBytes -= found->second.bytes;
        byKey.erase(found);
        return TC_COMPONENT_RENDER_OK;
    }

    /* Drops every key without retiring anything: the loader's texture manager
       already retires an unloaded Mod's whole texture set in one call. */
    void forget() {
        byKey.clear();
        storedBytes = 0;
    }

    /* The draw call's lookup.  Only this table is consulted, so a handle that
       belongs to another Mod is simply unknown here. */
    const Image* find(uint64_t image,const TCComponentInstanceHandle* instance=nullptr) const {
        if (!image) return nullptr;
        for (const auto& item : byKey)
            if (item.second.image == image) {
                if(!item.second.instanceId)return &item.second;
                if(instance&&item.second.customId==instance->custom_id&&
                   item.second.instanceId==instance->instance_id&&
                   item.second.generation==instance->generation)return &item.second;
                return nullptr;
            }
        return nullptr;
    }

    template<class Live>
    void prune(const Live& live,const Retire& retire) {
        for(auto it=byKey.begin();it!=byKey.end();) {
            if(it->first.instanceId&&
               !live(it->first.customId,it->first.instanceId,it->first.generation)) {
                if(retire)retire(it->second.texture);
                storedBytes-=it->second.bytes;it=byKey.erase(it);
            } else ++it;
        }
    }

    size_t count() const { return byKey.size(); }
    uint64_t bytes() const { return storedBytes; }

private:
    static uint64_t allocateHandle(uint64_t* nextImage) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const uint64_t handle = (*nextImage)++;
            if (handle) return handle;   /* 0 is "no image", never a handle */
        }
        return 0;
    }

    std::map<Key, Image> byKey;
    uint64_t storedBytes = 0;
};

}  // namespace tc::component_images
