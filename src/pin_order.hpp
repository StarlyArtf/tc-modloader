#pragma once
/* Reordering the pins of the game's own IO panel.

   The panel does not walk the board: build_io_state_view draws three cached
   sequences the presenter context keeps at the offsets in io_state_cache.hpp
   (inputs, outputs, and the memory/register group), and the order of those
   elements *is* the order the player sees.  Reordering the panel is therefore
   a permutation of a cached array - no board edit, no rebuild, and the values
   keep working because every element carries the component index the panel
   addresses its own value field with.

   The element layout was measured on the pinned build - the loader's own dump
   (TC_MODLOADER_PIN_ORDER_LOG=dump) prints it and tests/pin-order.cpp exercises
   it offline: the descriptor at io_state_cache's offsets is {count, payload};
   the payload repeats the count at +0x00 and the entries follow at +0x08, 32
   bytes each, as {component index, bit width, name {length, data}}.

   A Mod asks for a new order; this store keeps it and applies it to the live
   cache right before the panel reads it - that is, at the top of
   build_io_state_view, which is also where the cache is rebuilt after a
   rename.  Applying it any later would mutate the array the panel is walking.

   The stored order is a list of keys, not positions, so it survives the cache
   being rebuilt with the same pins, and it is dropped as soon as the pins it
   names are not the pins the panel is about to draw (a different level). */

#include "io_state_cache.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace tc::pin_order {

inline constexpr std::size_t kGroupCount=3;
/* The payload starts with the count again and the entries follow it. */
inline constexpr std::size_t kPayloadCountOffset=0x00;
inline constexpr std::size_t kEntriesOffset=0x08;
inline constexpr std::size_t kEntryBytes=0x20;
inline constexpr std::size_t kEntryIndexOffset=0x00;
inline constexpr std::size_t kEntryWidthOffset=0x08;
inline constexpr std::size_t kEntryNameOffset=0x10;
/* The name field is a Nim string: {length, data}, and the block the pointer
   names starts with the string's own capacity, so the characters are eight
   bytes in.  The dump above shows both words. */
inline constexpr std::size_t kNameHeaderBytes=8;
/* Names are copied out, never kept by pointer; a longer one is truncated. */
inline constexpr std::size_t kNameCopyLimit=127;
/* Upper bound on the block word and on the entry count: both come from the
   game, and a value outside this range means this is not that structure. */
inline constexpr std::uint64_t kCapacityLimit=4096;

inline std::size_t groupOffset(std::size_t group){
    switch(group){
        case 0:return io_state_cache::input_offset;
        case 1:return io_state_cache::output_offset;
        default:return io_state_cache::other_offset;
    }
}

/* True when every page of [address, address+bytes) is committed and readable.

   The presenter context this is used on is about 56 KB, so it can - and does -
   straddle two VirtualQuery regions (a fresh allocation, a heap block that grew
   into the next region, an allocation whose tail was re-protected).  Asking one
   region to cover the whole range quietly answers "no" in that case, and the
   service then looks like "the panel is not on screen" for every group at once:
   nothing to reorder and, in the Mod that draws handles, no handle.  The walk
   below follows the region sizes instead, so only genuinely unreadable memory
   refuses. */
inline bool readable(const void* address,std::size_t bytes){
    if(!address||!bytes)return false;
#if defined(_WIN32)
    const auto begin=reinterpret_cast<std::uintptr_t>(address);
    const auto end=begin+bytes;
    if(end<begin)return false;
    std::uintptr_t at=begin;
    while(at<end){
        MEMORY_BASIC_INFORMATION information{};
        if(VirtualQuery(reinterpret_cast<const void*>(at),&information,sizeof(information))==0)
            return false;
        if(information.State!=MEM_COMMIT)return false;
        if(information.Protect&(PAGE_NOACCESS|PAGE_GUARD))return false;
        const auto regionEnd=reinterpret_cast<std::uintptr_t>(information.BaseAddress)+
                             information.RegionSize;
        if(regionEnd<=at)return false;   /* no progress: refuse rather than spin */
        at=regionEnd;
    }
    return true;
#else
    (void)address;
    return false;
#endif
}

struct Entry {
    std::uint64_t key=0;      /* the entry's component index on the board */
    std::uint64_t width=0;    /* the entry's bit width */
    std::string name;
};

/* The cached group as it currently stands, if the panel cache is built. */
struct View {
    std::uint8_t* entries=nullptr;
    std::uint64_t count=0;
};

inline bool viewOf(void* context,std::size_t group,View* out){
    if(!context||!out||group>=kGroupCount)return false;
    const auto* base=static_cast<const std::uint8_t*>(context);
    if(!readable(base,io_state_cache::other_offset+16))return false;
    const std::uint64_t count=*reinterpret_cast<const std::uint64_t*>(base+groupOffset(group));
    auto* payload=*reinterpret_cast<std::uint8_t* const*>(base+groupOffset(group)+8);
    if(count==0||count>kCapacityLimit||!payload)return false;
    const std::size_t bytes=kEntriesOffset+static_cast<std::size_t>(count)*kEntryBytes;
    if(!readable(payload,bytes))return false;
    /* The block's own first word is its *capacity*, not its length: the
       descriptor's count is what the panel draws, and the two are equal only
       when the list was allocated at its final size (a level's panel is built
       once with newSeq).  A panel whose list grew one pin at a time - the
       component workshop, and any board whose input devices were added by hand
       - doubles that capacity instead (1, 2, 4, 8, 16 ...), so requiring the
       two to be equal switched this whole service off for every count that was
       not a power of two: the panel drew its entries, the service answered
       "unavailable", and the Mod had nothing to draw a handle on.  The block
       word is therefore only used as a sanity bound. */
    const std::uint64_t reserved=
        *reinterpret_cast<const std::uint64_t*>(payload+kPayloadCountOffset);
    if(reserved<count||reserved>kCapacityLimit)return false;
    out->entries=payload+kEntriesOffset;
    out->count=count;
    return true;
}

inline std::uint64_t keyAt(const View& view,std::size_t index){
    return *reinterpret_cast<const std::uint64_t*>(view.entries+index*kEntryBytes+kEntryIndexOffset);
}

inline Entry entryAt(const View& view,std::size_t index){
    const std::uint8_t* record=view.entries+index*kEntryBytes;
    Entry entry{};
    entry.key=*reinterpret_cast<const std::uint64_t*>(record+kEntryIndexOffset);
    entry.width=*reinterpret_cast<const std::uint64_t*>(record+kEntryWidthOffset);
    const std::uint64_t length=*reinterpret_cast<const std::uint64_t*>(record+kEntryNameOffset);
    const auto* header=*reinterpret_cast<const char* const*>(record+kEntryNameOffset+8);
    const char* text=header?header+kNameHeaderBytes:nullptr;
    if(text&&length>0&&length<=kNameCopyLimit&&readable(text,static_cast<std::size_t>(length)))
        entry.name.assign(text,static_cast<std::size_t>(length));
    return entry;
}

namespace status {
inline constexpr int ok=0;
inline constexpr int unavailable=-1;
inline constexpr int argument=-2;
inline constexpr int group=-3;
inline constexpr int range=-4;
inline constexpr int state=-5;
}  // namespace status

class Store {
public:
    /* The presenter context the panel was last built with.  It is the same one
       the Mod's calls arrive from (the panel draws itself), so the service
       needs no context of its own. */
    void* context() const{
        std::lock_guard<std::mutex> lock(mutex_);
        return context_;
    }

    /* Installs the order a Mod asked for.  Keys that the panel is not showing
       are ignored; entries the Mod did not mention keep the panel's own
       relative order after the ones it did. */
    int setOrder(std::uint32_t group,const std::uint64_t* keys,std::uint32_t count){
        std::lock_guard<std::mutex> lock(mutex_);
        if(group>=kGroupCount)return status::group;
        if(!keys&&count)return status::argument;
        std::vector<std::uint64_t> requested(keys,keys+count);
        if(!acceptLocked(group,requested))return status::state;
        desired_[group]=std::move(requested);
        return status::ok;
    }

    /* Moves one entry of the order the panel is showing now: `from` and `to`
       are positions, `to` the slot the entry should end up in. */
    int move(std::uint32_t group,std::uint32_t from,std::uint32_t to){
        std::lock_guard<std::mutex> lock(mutex_);
        if(group>=kGroupCount)return status::group;
        std::vector<std::uint64_t> keys;
        if(!orderLocked(group,&keys))return status::state;
        if(from>=keys.size())return status::range;
        if(to>=keys.size())to=static_cast<std::uint32_t>(keys.size()-1);
        if(from==to)return status::ok;
        const std::uint64_t moved=keys[from];
        keys.erase(keys.begin()+from);
        keys.insert(keys.begin()+to,moved);
        desired_[group]=std::move(keys);
        return status::ok;
    }

    /* Drops the Mod's order and puts the cache back to the order the panel
       last built for itself, when it is still the same set of pins. */
    int clear(std::uint32_t group){
        std::lock_guard<std::mutex> lock(mutex_);
        if(group>=kGroupCount)return status::group;
        View view{};
        if(!viewOf(context_,group,&view))return status::state;
        std::vector<std::uint64_t> current=keysOf(view);
        std::vector<std::uint64_t> restored;
        if(sameSet(current,baseline_[group]))
            restored=baseline_[group];
        desired_[group].clear();
        if(restored.empty()||restored==current)return status::ok;
        applyLocked(group,restored);
        return status::ok;
    }

    /* The order the panel will draw next, as keys. */
    int order(std::uint32_t group,std::uint64_t* keys,std::uint32_t capacity,
              std::uint32_t* count) const{
        std::lock_guard<std::mutex> lock(mutex_);
        if(group>=kGroupCount)return status::group;
        if(count)*count=0;
        std::vector<std::uint64_t> current;
        if(!orderLocked(group,&current))return status::state;
        if(count)*count=static_cast<std::uint32_t>(current.size());
        if(!keys)return status::ok;
        const std::size_t written=std::min<std::size_t>(current.size(),capacity);
        std::copy(current.begin(),current.begin()+written,keys);
        return written==current.size()?status::ok:status::range;
    }

    std::uint32_t count(std::uint32_t group) const{
        std::lock_guard<std::mutex> lock(mutex_);
        View view{};
        if(group>=kGroupCount||!viewOf(context_,group,&view))return 0;
        return static_cast<std::uint32_t>(view.count);
    }

    int entry(std::uint32_t group,std::uint32_t index,Entry* out) const{
        std::lock_guard<std::mutex> lock(mutex_);
        if(group>=kGroupCount)return status::group;
        if(!out)return status::argument;
        View view{};
        if(!viewOf(context_,group,&view))return status::state;
        if(index>=view.count)return status::range;
        *out=entryAt(view,index);
        return status::ok;
    }

    /* Called at the top of every build_io_state_view, before the panel reads
       (and possibly rebuilds) its cache. */
    void apply(void* context){
        std::lock_guard<std::mutex> lock(mutex_);
        if(!context)return;
        context_=context;
        for(std::size_t group=0;group<kGroupCount;++group){
            View view{};
            if(!viewOf(context,group,&view))continue;
            std::vector<std::uint64_t> current=keysOf(view);
            /* The pins the panel is about to draw are what the stored order is
               about: a different set means the order belongs to another level
               (or the board changed), so it is not applied any more. */
            if(!desired_[group].empty()&&!sameSet(desired_[group],current))
                desired_[group].clear();
            if(!sameSet(baseline_[group],current))baseline_[group]=current;
            if(desired_[group].empty())continue;
            applyLocked(group,desired_[group]);
        }
    }

    /* Development aid (TC_MODLOADER_PIN_ORDER_LOG=dump): the raw structure of
       every group, as text, so the constants above can be re-derived on a build
       where the dump does not look like this layout any more. */
    std::vector<std::string> describe(void* context) const{
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> lines;
        char line[256];
        for(std::size_t group=0;group<kGroupCount;++group){
            View view{};
            if(!viewOf(context,group,&view)){
                /* Why it was refused, field by field: a rebuild that moves the
                   cache is exactly what this dump exists to catch. */
                const auto* base=static_cast<const std::uint8_t*>(context);
                const std::uint64_t count=context?
                    *reinterpret_cast<const std::uint64_t*>(base+groupOffset(group)):0;
                const auto* payload=context?
                    *reinterpret_cast<const std::uint8_t* const*>(base+groupOffset(group)+8):nullptr;
                std::snprintf(line,sizeof(line),
                              "pin order dump: group %llu unavailable count=%llu payload=%p "
                              "contextReadable=%d payloadReadable=%d header=%llu",
                              static_cast<unsigned long long>(group),
                              static_cast<unsigned long long>(count),
                              static_cast<const void*>(payload),
                              readable(context,io_state_cache::other_offset+16)?1:0,
                              payload&&readable(payload,kEntriesOffset+
                                  static_cast<std::size_t>(count)*kEntryBytes)?1:0,
                              payload&&readable(payload,kPayloadCountOffset+8)?
                                  static_cast<unsigned long long>(
                                      *reinterpret_cast<const std::uint64_t*>(
                                          payload+kPayloadCountOffset)):0);
                lines.push_back(line);
                continue;
            }
            std::snprintf(line,sizeof(line),"pin order dump: group %llu count=%llu entries=%p",
                          static_cast<unsigned long long>(group),
                          static_cast<unsigned long long>(view.count),
                          static_cast<const void*>(view.entries));
            lines.push_back(line);
            const std::size_t shown=std::min<std::size_t>(view.count,6);
            for(std::size_t index=0;index<shown;++index){
                const std::uint8_t* record=view.entries+index*kEntryBytes;
                const Entry entry=entryAt(view,index);
                std::snprintf(line,sizeof(line),
                              "pin order dump:   #%llu key=%llu width=%llu name=(%llu)\"%s\" "
                              "raw=%016llx %016llx %016llx %016llx",
                              static_cast<unsigned long long>(index),
                              static_cast<unsigned long long>(entry.key),
                              static_cast<unsigned long long>(entry.width),
                              static_cast<unsigned long long>(
                                  *reinterpret_cast<const std::uint64_t*>(record+kEntryNameOffset)),
                              entry.name.c_str(),
                              static_cast<unsigned long long>(
                                  *reinterpret_cast<const std::uint64_t*>(record+0)),
                              static_cast<unsigned long long>(
                                  *reinterpret_cast<const std::uint64_t*>(record+8)),
                              static_cast<unsigned long long>(
                                  *reinterpret_cast<const std::uint64_t*>(record+16)),
                              static_cast<unsigned long long>(
                                  *reinterpret_cast<const std::uint64_t*>(record+24)));
                lines.push_back(line);
            }
        }
        return lines;
    }

private:
    static std::vector<std::uint64_t> keysOf(const View& view){
        std::vector<std::uint64_t> keys(static_cast<std::size_t>(view.count));
        for(std::size_t index=0;index<keys.size();++index)keys[index]=keyAt(view,index);
        return keys;
    }

    /* Same pins, whatever the order: comparing sets rather than sequences is
       what lets a stored order survive a rename (which rebuilds the cache) and
       be dropped when a different level's pins are on screen. */
    static bool sameSet(std::vector<std::uint64_t> left,std::vector<std::uint64_t> right){
        std::sort(left.begin(),left.end());
        std::sort(right.begin(),right.end());
        return left==right;
    }

    bool acceptLocked(std::uint32_t group,const std::vector<std::uint64_t>& keys) const{
        View view{};
        if(!viewOf(context_,group,&view))return false;
        const std::vector<std::uint64_t> current=keysOf(view);
        for(std::uint64_t key:keys)
            if(std::find(current.begin(),current.end(),key)==current.end())return false;
        return true;
    }

    bool orderLocked(std::uint32_t group,std::vector<std::uint64_t>* out) const{
        View view{};
        if(!viewOf(context_,group,&view))return false;
        std::vector<std::uint64_t> current=keysOf(view);
        if(desired_[group].empty()||!sameSet(desired_[group],current)){
            *out=std::move(current);
            return true;
        }
        *out=desired_[group];
        return true;
    }

    /* Permutes the live array: the entries the order names first, in that
       order, then whatever it did not name, in the panel's own order.  Whole
       records move, so every name string keeps its owner and is freed exactly
       once when the game rebuilds the cache. */
    void applyLocked(std::uint32_t group,const std::vector<std::uint64_t>& order) const{
        View view{};
        if(!viewOf(context_,group,&view))return;
        std::vector<std::uint64_t> current=keysOf(view);
        std::vector<std::size_t> permutation;
        permutation.reserve(current.size());
        std::vector<bool> taken(current.size(),false);
        for(std::uint64_t key:order){
            for(std::size_t index=0;index<current.size();++index){
                if(taken[index]||current[index]!=key)continue;
                taken[index]=true;
                permutation.push_back(index);
                break;
            }
        }
        for(std::size_t index=0;index<current.size();++index)
            if(!taken[index])permutation.push_back(index);
        if(permutation.size()!=current.size())return;
        bool identity=true;
        for(std::size_t index=0;index<permutation.size();++index)
            if(permutation[index]!=index){identity=false;break;}
        if(identity)return;
        std::vector<std::uint8_t> copy(current.size()*kEntryBytes);
        std::memcpy(copy.data(),view.entries,copy.size());
        for(std::size_t index=0;index<permutation.size();++index)
            std::memcpy(view.entries+index*kEntryBytes,copy.data()+permutation[index]*kEntryBytes,
                        kEntryBytes);
    }

    mutable std::mutex mutex_;
    void* context_=nullptr;
    std::vector<std::uint64_t> desired_[kGroupCount];
    std::vector<std::uint64_t> baseline_[kGroupCount];
};

/* One store per process: the loader's build_io_state_view hook applies it right
   before the panel reads its cache, and the service table edits it. */
inline Store& store(){
    static Store value;
    return value;
}

}  // namespace tc::pin_order
