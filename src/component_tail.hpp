#pragma once
/* The custom-tail record: how a native component's configuration travels with
   the circuit file.

   A custom component's record owns a small key/value table (`Table[int64,
   int64]` in the game's own save_monger/versions/v7 module), and the game
   serializes that table verbatim inside the schematic.  The host keeps its
   record in that table instead of in a sidecar, because a table entry is what
   the game itself copies, saves and loads with the component.

   Layout of the reserved keyspace - the top 32 bits are the ASCII magic "TCM3"
   and the low 32 bits select a field, so a hex dump of a saved schematic reads
   as `TCM3...` and a field added in a later loader cannot collide with the four
   below.  Every entry the host does not recognize is left untouched, so another
   Mod's or a newer loader's record survives a save.

     TCM3:00000001  format version
     TCM3:00000002  custom id of the definition that wrote it
     TCM3:00000003  schema (high 32 bits) and byte length (low 32 bits)
     TCM3:00000004  checksum of the configuration bytes
     TCM3:00001000+i  bytes 8i..8i+7, little endian, zero padded

   Commit order matters: the chunks go in first and the checksum last, so a
   record that is half written fails its checksum instead of being read as a
   valid but truncated configuration.  The host never deletes an entry (the
   game's setter has no delete), so shrinking a configuration leaves unreferenced
   chunk entries behind; the length field is what bounds a read.

   The offsets this file depends on are profile-controlled (see
   compat/profiles.json `layout`): layoutKnown() is false on a build the profile
   does not describe, and the runtime then leaves configuration in memory
   instead of guessing a table layout. */
#include "compat.generated.hpp"
#include <cstdint>
#include <cstring>
#include <vector>

#ifndef TC_LAYOUT_CUSTOM_TAIL_TABLE_OFFSET
#define TC_LAYOUT_CUSTOM_TAIL_TABLE_OFFSET 0
#endif
#ifndef TC_LAYOUT_CUSTOM_TAIL_ELEMENT_STRIDE
#define TC_LAYOUT_CUSTOM_TAIL_ELEMENT_STRIDE 0
#endif
#ifndef TC_LAYOUT_CUSTOM_TAIL_ELEMENT_HEADER
#define TC_LAYOUT_CUSTOM_TAIL_ELEMENT_HEADER 0
#endif

namespace tc::component_tail {
inline constexpr uint32_t kMagic=0x54434D33u;  /* "TCM3" */
inline constexpr uint32_t kFieldFormat=1u;
inline constexpr uint32_t kFieldTypeId=2u;
inline constexpr uint32_t kFieldSchemaLength=3u;
inline constexpr uint32_t kFieldChecksum=4u;
inline constexpr uint32_t kFieldChunkBase=0x1000u;
inline constexpr uint32_t kRecordFormatVersion=1u;

/* Where the table sits inside a component record, and how a table element is
   laid out.  Zero means "this build profile does not describe it". */
inline constexpr uint64_t kTableOffset=TC_LAYOUT_CUSTOM_TAIL_TABLE_OFFSET;
inline constexpr uint64_t kElementStride=TC_LAYOUT_CUSTOM_TAIL_ELEMENT_STRIDE;
inline constexpr uint64_t kElementHeader=TC_LAYOUT_CUSTOM_TAIL_ELEMENT_HEADER;
inline constexpr uint64_t kElementHashOffset=0;
/* The key and the value are the element's two trailing words, which is what an
   `(int64, int64)` table element is: hash, key, value. */
inline constexpr uint64_t kElementValueOffset=kElementStride-8;
inline constexpr uint64_t kElementKeyOffset=kElementStride-0x10;
/* Slots are a power of two and the game grows them by doubling; the same bound
   the probe uses keeps a corrupted count from turning into a long walk. */
inline constexpr uint64_t kMaxSlots=0x100000;

inline constexpr bool layoutKnown(){
    return kTableOffset!=0&&kElementStride!=0&&kElementHeader!=0;
}
/* Configuration bytes a single instance may persist.  Deliberately far below
   what the in-memory service accepts: every byte costs a table entry in the
   schematic, and the save path has not been stress-tested past this. */
inline constexpr uint32_t kMaxConfigBytes=1024u;
inline constexpr uint32_t kMaxChunks=(kMaxConfigBytes+7u)/8u;

inline uint64_t fieldKey(uint32_t field){
    return (static_cast<uint64_t>(kMagic)<<32)|static_cast<uint64_t>(field);
}
inline uint64_t schemaAndLength(uint32_t schema,uint32_t bytes){
    return (static_cast<uint64_t>(schema)<<32)|static_cast<uint64_t>(bytes);
}
inline uint32_t schemaOf(uint64_t value){return static_cast<uint32_t>(value>>32);}
inline uint32_t lengthOf(uint64_t value){return static_cast<uint32_t>(value&0xFFFFFFFFu);}

/* FNV-1a over the configuration bytes, so a torn or foreign record is refused
   instead of being installed as a configuration. */
inline uint64_t checksum(const uint8_t* data,uint32_t bytes){
    uint64_t hash=0xcbf29ce484222325ull;
    for(uint32_t i=0;i<bytes;++i){
        hash^=data[i];
        hash*=0x100000001b3ull;
    }
    return hash;
}

struct Entry{uint64_t key;uint64_t value;};

/* The entries of one record, in commit order: chunks, then the identity and
   schema, then the checksum that makes the record readable.  An empty
   configuration still writes its header, so "configured to nothing" and "never
   written" stay distinguishable. */
inline std::vector<Entry> encode(uint64_t customId,uint32_t schema,const uint8_t* data,
                                 uint32_t bytes){
    std::vector<Entry> entries;
    if(bytes>kMaxConfigBytes)return entries;
    const uint32_t chunks=(bytes+7u)/8u;
    for(uint32_t i=0;i<chunks;++i){
        uint64_t word=0;
        const uint32_t offset=i*8u;
        const uint32_t take=bytes-offset<8u?bytes-offset:8u;
        std::memcpy(&word,data+offset,take);
        entries.push_back({fieldKey(kFieldChunkBase+i),word});
    }
    entries.push_back({fieldKey(kFieldTypeId),customId});
    entries.push_back({fieldKey(kFieldFormat),kRecordFormatVersion});
    entries.push_back({fieldKey(kFieldSchemaLength),schemaAndLength(schema,bytes)});
    entries.push_back({fieldKey(kFieldChecksum),checksum(data,bytes)});
    return entries;
}

/* Reads one key out of the live table.  false means "the table could not be
   read at all"; a missing key is reported through `found`. */
using ReadFn=bool(*)(void* context,void* table,uint64_t key,uint64_t* value,bool* found);

enum class Status{
    Ok,          /* a complete record was installed */
    Missing,     /* no record for this instance */
    BadFormat,   /* written by an incompatible format */
    ForeignType, /* written by a different definition id */
    BadSchema,   /* same bytes, different schema: migration is a later cut */
    BadLength,   /* the record does not describe the registered size */
    BadChecksum, /* torn or corrupted record */
    TooLarge,    /* the record exceeds the persistence budget */
    Unreadable,  /* the table itself could not be walked */
};

/* The record exactly as the circuit stores it: not yet compared against any
   definition, but already verified against its own checksum. */
struct Stored {
    uint32_t schema=0,bytes=0;
    uint64_t checksum=0;
    std::vector<uint8_t> data;
};

/* Reads the stored record.  A record whose schema or length no longer matches
   the definition still reads: that is what the migration path needs, and it is
   also why a mismatch can report the schema it found instead of just failing.
   `maxBytes` is an extra caller limit (0 = only the persistence budget). */
inline Status readStored(ReadFn read,void* context,void* table,uint64_t customId,uint32_t maxBytes,
                         Stored* out){
    if(!read||!table)return Status::Unreadable;
    uint64_t format=0,typeId=0,shape=0,sum=0;
    bool found=false;
    if(!read(context,table,fieldKey(kFieldFormat),&format,&found))return Status::Unreadable;
    if(!found)return Status::Missing;
    if(format!=kRecordFormatVersion)return Status::BadFormat;
    if(!read(context,table,fieldKey(kFieldTypeId),&typeId,&found))return Status::Unreadable;
    if(!found)return Status::BadLength;
    if(typeId!=customId)return Status::ForeignType;
    if(!read(context,table,fieldKey(kFieldSchemaLength),&shape,&found))return Status::Unreadable;
    if(!found)return Status::BadLength;
    const uint32_t length=lengthOf(shape);
    if(length>kMaxConfigBytes)return Status::TooLarge;
    if(maxBytes&&length>maxBytes)return Status::TooLarge;
    std::vector<uint8_t> bytes(length,0);
    if(length){
        const uint32_t chunks=(length+7u)/8u;
        for(uint32_t i=0;i<chunks;++i){
            uint64_t word=0;
            if(!read(context,table,fieldKey(kFieldChunkBase+i),&word,&found))return Status::Unreadable;
            if(!found)return Status::BadLength;
            const uint32_t offset=i*8u;
            const uint32_t take=length-offset<8u?length-offset:8u;
            std::memcpy(bytes.data()+offset,&word,take);
        }
    }
    if(!read(context,table,fieldKey(kFieldChecksum),&sum,&found))return Status::Unreadable;
    if(!found)return Status::BadChecksum;
    if(sum!=checksum(bytes.data(),length))return Status::BadChecksum;
    if(out){
        out->schema=schemaOf(shape);
        out->bytes=length;
        out->checksum=sum;
        out->data=std::move(bytes);
    }
    return Status::Ok;
}

/* Decodes one record into `out`, requiring it to be the schema and size this
   definition registers.  On anything but Ok the caller keeps whatever
   configuration it already had: a rejected record is preserved, never
   overwritten and never applied. */
inline Status decode(ReadFn read,void* context,void* table,uint64_t customId,uint32_t schema,
                     uint32_t expectedBytes,uint8_t* out){
    Stored stored;
    const Status status=readStored(read,context,table,customId,0,&stored);
    if(status!=Status::Ok)return status;
    if(stored.bytes!=expectedBytes)return Status::BadLength;
    if(stored.schema!=schema)return Status::BadSchema;
    if(stored.bytes)std::memcpy(out,stored.data.data(),stored.bytes);
    return Status::Ok;
}

/* Decodes the entries of a record that has not been written yet - the check the
   host runs between building a record and committing it, so the encoder can
   never put a record in a save that the next launch would refuse. */
inline bool readEntries(void* context,void*,uint64_t key,uint64_t* value,bool* found){
    const auto* table=static_cast<const std::vector<Entry>*>(context);
    for(const auto& entry:*table)
        if(entry.key==key){*value=entry.value;*found=true;return true;}
    *found=false;
    return true;
}
inline Status decodeEntries(const std::vector<Entry>& entries,uint64_t customId,uint32_t schema,
                            uint32_t bytes,uint8_t* out){
    auto copy=entries;
    return decode(&readEntries,&copy,&copy,customId,schema,bytes,out);
}

inline const char* statusName(Status status){
    switch(status){
        case Status::Ok:return "ok";
        case Status::Missing:return "no record";
        case Status::BadFormat:return "unknown format";
        case Status::ForeignType:return "different definition";
        case Status::BadSchema:return "different schema";
        case Status::BadLength:return "different length";
        case Status::BadChecksum:return "checksum mismatch";
        case Status::TooLarge:return "above the persistence budget";
        case Status::Unreadable:return "table unreadable";
    }
    return "unknown";
}
}  // namespace tc::component_tail
