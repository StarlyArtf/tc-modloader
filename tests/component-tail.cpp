/* The custom-tail record: the format the host stores a native component's
   configuration in, and the reasons it refuses one.  The table itself is the
   game's, so this test drives the codec with a table of its own and never needs
   the game. */
#include "../src/component_tail.hpp"
#include <cassert>
#include <iostream>
#include <map>

namespace {
using namespace tc::component_tail;

std::map<uint64_t,uint64_t> table;
bool tableReadable=true;

bool read(void*,void*,uint64_t key,uint64_t* value,bool* found){
    if(!tableReadable)return false;
    const auto entry=table.find(key);
    *found=entry!=table.end();
    if(*found)*value=entry->second;
    return true;
}

void store(const std::vector<Entry>& entries){
    table.clear();
    for(const auto& entry:entries)table[entry.key]=entry.value;
}

constexpr uint64_t kTypeId=0x434647305F303031ULL;
constexpr uint32_t kSchema=7;
}  // namespace

int main(){
    assert(layoutKnown());   /* the pinned profile describes the record layout */

    /* A record round-trips at every length from empty to the whole budget,
       including the three that exercise the padding of the last chunk. */
    for(uint32_t bytes:{0u,1u,4u,7u,8u,9u,17u,kMaxConfigBytes}){
        std::vector<uint8_t> source(bytes);
        for(uint32_t i=0;i<bytes;++i)source[i]=static_cast<uint8_t>(i*7u+1u);
        const auto entries=encode(kTypeId,kSchema,source.data(),bytes);
        assert(!entries.empty());
        store(entries);
        std::vector<uint8_t> restored(bytes,0xEE);
        assert(decode(&read,nullptr,&table,kTypeId,kSchema,bytes,restored.data())==Status::Ok);
        assert(restored==source);
        assert(decodeEntries(entries,kTypeId,kSchema,bytes,restored.data())==Status::Ok);
        assert(restored==source);
    }

    /* Commit order: the chunks come first and the checksum last, so a commit
       that stops halfway leaves a record the next loader refuses. */
    {
        const uint8_t source[17]{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17};
        const auto entries=encode(kTypeId,kSchema,source,sizeof(source));
        assert(entries.size()==3+4);
        assert(entries[0].key==fieldKey(kFieldChunkBase));
        assert(entries[1].key==fieldKey(kFieldChunkBase+1));
        assert(entries[2].key==fieldKey(kFieldChunkBase+2));
        assert(entries[3].key==fieldKey(kFieldTypeId));
        assert(entries[4].key==fieldKey(kFieldFormat));
        assert(entries[5].key==fieldKey(kFieldSchemaLength));
        assert(entries.back().key==fieldKey(kFieldChecksum));
        /* A record that lost its checksum is refused rather than half applied. */
        auto truncated=entries;truncated.pop_back();store(truncated);
        uint8_t out[17]{};
        assert(decode(&read,nullptr,&table,kTypeId,kSchema,sizeof(source),out)==Status::BadChecksum);
        /* A record whose last chunk never landed is refused as truncated. */
        auto missingChunk=entries;
        for(auto it=missingChunk.begin();it!=missingChunk.end();++it)
            if(it->key==fieldKey(kFieldChunkBase+2)){missingChunk.erase(it);break;}
        store(missingChunk);
        assert(decode(&read,nullptr,&table,kTypeId,kSchema,sizeof(source),out)==Status::BadLength);
    }

    /* Every way a stored record can disagree with the definition is named, and
       none of them is reported as Ok. */
    {
        const uint8_t source[4]{0xaa,0xbb,0xcc,0xdd};
        store(encode(kTypeId,kSchema,source,sizeof(source)));
        uint8_t out[4]{};
        assert(decode(&read,nullptr,&table,kTypeId,kSchema,3,out)==Status::BadLength);
        assert(decode(&read,nullptr,&table,kTypeId,kSchema+1,4,out)==Status::BadSchema);
        assert(decode(&read,nullptr,&table,kTypeId+1,kSchema,4,out)==Status::ForeignType);
        /* An unknown format is preserved, not interpreted. */
        table[fieldKey(kFieldFormat)]=kRecordFormatVersion+1;
        assert(decode(&read,nullptr,&table,kTypeId,kSchema,4,out)==Status::BadFormat);
        table[fieldKey(kFieldFormat)]=kRecordFormatVersion;
        /* A corrupted chunk fails the checksum. */
        table[fieldKey(kFieldChunkBase)]^=0x00000000000000FFull;
        assert(decode(&read,nullptr,&table,kTypeId,kSchema,4,out)==Status::BadChecksum);
        /* A table with no record at all is "missing", which is not an error. */
        table.clear();
        assert(decode(&read,nullptr,&table,kTypeId,kSchema,4,out)==Status::Missing);
        /* A table the runtime cannot walk is reported as such. */
        tableReadable=false;
        assert(decode(&read,nullptr,&table,kTypeId,kSchema,4,out)==Status::Unreadable);
        tableReadable=true;
        assert(decode(&read,nullptr,nullptr,kTypeId,kSchema,4,out)==Status::Unreadable);
    }

    /* The budget is a hard wall in both directions. */
    {
        /* A record that no longer matches the definition still reads: that is
           what a migration needs, and it reports the schema it found. */
        const uint8_t old[6]{1,2,3,4,5,6};
        store(encode(kTypeId,kSchema-1,old,sizeof(old)));
        Stored stored;
        assert(readStored(&read,nullptr,&table,kTypeId,0,&stored)==Status::Ok);
        assert(stored.schema==kSchema-1&&stored.bytes==sizeof(old));
        assert(stored.data==std::vector<uint8_t>(old,old+sizeof(old)));
        assert(stored.checksum==checksum(old,sizeof(old)));
        /* decode() is the strict view: same record, wrong definition. */
        uint8_t out2[8]{};
        assert(decode(&read,nullptr,&table,kTypeId,kSchema-1,sizeof(old),out2)==Status::Ok);
        assert(decode(&read,nullptr,&table,kTypeId,kSchema,sizeof(old),out2)==Status::BadSchema);
        assert(decode(&read,nullptr,&table,kTypeId,kSchema-1,4,out2)==Status::BadLength);
        /* A foreign record is not a migration candidate: it never reaches one. */
        store(encode(kTypeId+1,kSchema,old,4));
        assert(readStored(&read,nullptr,&table,kTypeId,0,&stored)==Status::ForeignType);

        std::vector<uint8_t> tooBig(kMaxConfigBytes+1,0);
        assert(encode(kTypeId,kSchema,tooBig.data(),static_cast<uint32_t>(tooBig.size())).empty());
        assert(kMaxChunks==(kMaxConfigBytes+7u)/8u);
        /* A record that claims more bytes than the budget is refused even when
           its length matches what the caller asked for. */
        const uint8_t source[4]{1,2,3,4};
        store(encode(kTypeId,kSchema,source,sizeof(source)));
        table[fieldKey(kFieldSchemaLength)]=schemaAndLength(kSchema,kMaxConfigBytes+8);
        uint8_t out[kMaxConfigBytes+8]{};
        assert(decode(&read,nullptr,&table,kTypeId,kSchema,kMaxConfigBytes+8,out)==Status::TooLarge);
    }

    /* The keyspace cannot collide with a field added later, and an unrelated
       entry in the same table is left where it is. */
    {
        const uint8_t source[4]{9,9,9,9};
        store(encode(kTypeId,kSchema,source,sizeof(source)));
        table[0x54434D3343464701ULL]=0x13579BDF2468ACELL;   /* another writer's key */
        uint8_t out[4]{};
        assert(decode(&read,nullptr,&table,kTypeId,kSchema,4,out)==Status::Ok);
        assert(table[0x54434D3343464701ULL]==0x13579BDF2468ACELL);
        assert(fieldKey(kFieldChunkBase)>fieldKey(kFieldChecksum));
        assert((fieldKey(kFieldFormat)>>32)==kMagic);
    }

    std::cout<<"PASS custom tail record: encode/decode round trip, commit order, refusal reasons and budget\n";
    return 0;
}
