#pragma once
#include "core.hpp"
#include "snappy_raw.hpp"
#include <algorithm>
#include <map>
#include <set>
namespace tc {
/* ---------------------------------------------------------------- type registry
   A circuit file does not say which Mod its custom components come from: it only
   carries each component's stable 64 bit custom_id (a tag the author picked,
   e.g. "F32ABS_1").  The one place that mapping exists is registration, so the
   loader writes down what it saw while a Mod loaded - id, type name, config
   schema, and the Mod's id/version/package digest - and keeps it in
   tc-modloader-data/registry.json.  That cache is what lets the save page name
   the Mods a profile needs even after the Mod has been uninstalled. */
struct ComponentType {
 std::string mod,name,type_id,version,digest,seen;
 int schema=-1;
};
class TypeRegistry {
 fs::path path;
 std::map<uint64_t,ComponentType> types;
 public:
 explicit TypeRegistry(fs::path file):path(std::move(file)){
  try{
   const auto j=jsonfile(path,J::object());
   for(auto& [key,value]:j.items()){
    const uint64_t id=std::strtoull(key.c_str(),nullptr,16);
    if(!id)continue;
    ComponentType type;
    type.mod=value.value("mod",std::string());
    type.name=value.value("name",std::string());
    type.type_id=value.value("type",std::string());
    type.version=value.value("version",std::string());
    type.digest=value.value("digest",std::string());
    type.seen=value.value("seen",std::string());
    type.schema=value.value("schema",-1);
    types.emplace(id,std::move(type));
   }
  }catch(...){/* a broken cache is not fatal: it only means unknown ids stay unknown */}
 }
 const std::map<uint64_t,ComponentType>& all()const{return types;}
 const ComponentType* find(uint64_t id)const{
  const auto found=types.find(id);return found==types.end()?nullptr:&found->second;
 }
 /* Called while a Mod registers a type.  Only writes when something changed, so
    a run over 22 types of one Mod is one file write, not 22. */
 void note(uint64_t id,const ComponentType& type){
  if(!id)return;
  const auto found=types.find(id);
  if(found!=types.end()&&found->second.mod==type.mod&&found->second.name==type.name&&
     found->second.type_id==type.type_id&&found->second.version==type.version&&
     found->second.digest==type.digest&&found->second.schema==type.schema)return;
  if(types.size()>=4096)return;
  types[id]=type;
  persist();
 }
 private:
 void persist(){
  try{
   J j=J::object();
   for(auto& [id,type]:types){
    char key[24]{};
    std::snprintf(key,sizeof(key),"%016llx",(unsigned long long)id);
    j[key]={{"mod",type.mod},{"name",type.name},{"type",type.type_id},
            {"version",type.version},{"digest",type.digest},{"seen",type.seen},
            {"schema",type.schema}};
   }
   atomic(path,j.dump(2));
  }catch(...){/* the cache is an optimisation; never fail a Mod registration for it */}
 }
};
/* ------------------------------------------------------------- profile scan
   What one circuit file contributes: the custom component ids it references.
   Two ways in, because the player's files are format v15/v16, whose component
   records this build cannot walk field by field (the pinned reader documents
   v13/v14 only):

     - every id the registry knows is searched for verbatim (the id is a plain 8
       byte value in the stream), which is exact;
     - 8 byte runs of printable ASCII are taken as *candidate* ids, which is how
       a type from an uninstalled Mod is still shown by its author's tag
       ("F32ABS_1") instead of a hex blob.

   The second half is a heuristic and the report says so. */
inline bool printableTag(const unsigned char* data,size_t size){
 for(size_t i=0;i<size;++i)
  if(data[i]<0x20||data[i]>=0x7f)return false;
 return true;
}
inline std::string tagOf(uint64_t id){
 unsigned char bytes[8]{};
 /* Most significant byte first: the ids in this project are picked so that
    their hex spelling reads as a tag ("F32ABS_1" = 0x4633324142535F31), and the
    circuit stores that value little-endian. */
 for(int i=0;i<8;++i)bytes[i]=(unsigned char)((id>>(8*(7-i)))&0xff);
 if(!printableTag(bytes,8))return std::string();
 return std::string(reinterpret_cast<char*>(bytes),8);
}
struct ScanResult {
 size_t circuits=0,skipped=0,references=0;
 std::map<uint64_t,size_t> ids;      /* id -> occurrences */
};
inline void scanCircuit(const std::vector<unsigned char>& raw,const TypeRegistry& registry,
                        ScanResult& out){
 ++out.circuits;
 /* One pass for both halves.  The known ids used to be searched one at a time -
    27 patterns over every byte of every circuit - which took ~190 ms for a
    played-through profile on the render thread; sliding an eight byte window and
    looking the value up in a set is one pass instead.

    Candidate tags: exactly eight printable bytes, not part of a longer run, and
    confirmed by context - either the loader's own configuration marker follows
    within 32 bytes (a component this loader configured), or the same tag occurs
    twice in the file (the level's imported-prototype list plus the component
    record).  Measured against a real profile: without the confirmation every
    eight letter level name ("and_gate", "keyboard") showed up as a "type". */
 std::set<uint64_t> known;
 for(auto& [id,type]:registry.all()){(void)type;known.insert(id);}
 for(size_t i=0;i+8<=raw.size();++i){
  uint64_t value=0;
  std::memcpy(&value,raw.data()+i,8);
  if(known.count(value)){++out.ids[value];++out.references;continue;}
  if(!printableTag(raw.data()+i,8))continue;
  if(i>0&&printableTag(raw.data()+i-1,1))continue;
  if(i+8<raw.size()&&printableTag(raw.data()+i+8,1))continue;
  const uint64_t id=value;
  /* "TCM3" is the loader's own configuration-record magic, not a type. */
  if(raw[i]=='T'&&raw[i+1]=='C'&&raw[i+2]=='M'&&raw[i+3]=='3')continue;
  bool confirmed=false;
  for(size_t j=i+1;j+32<=raw.size()&&j<i+32;++j)
   if(raw[j]=='T'&&raw[j+1]=='C'&&raw[j+2]=='M'&&raw[j+3]=='3'){confirmed=true;break;}
  if(!confirmed){
   size_t count=0;
   for(auto at=raw.begin();;){
    at=std::search(at,raw.end(),raw.begin()+i,raw.begin()+i+8);
    if(at==raw.end())break;
    ++count;++at;
   }
   confirmed=count>1;
  }
  if(!confirmed)continue;
  ++out.ids[id];++out.references;
 }
}
inline const size_t kMaxCircuits=512,kMaxCircuitBytes=8u<<20,kMaxRawBytes=32u<<20;
inline ScanResult scanProfile(const fs::path& profile,const TypeRegistry& registry){
 ScanResult result;
 std::error_code error;
 if(!fs::is_directory(profile,error))return result;
 for(auto& entry:fs::recursive_directory_iterator(profile,error)){
  if(!entry.is_regular_file())continue;
  if(entry.path().filename()!=L"circuit.data")continue;
  if(result.circuits>=kMaxCircuits){++result.skipped;continue;}
  if(entry.file_size()>kMaxCircuitBytes){++result.skipped;continue;}
  const auto blob=read(entry.path());
  if(blob.size()<2){++result.skipped;continue;}
  std::vector<unsigned char> raw;
  /* The first byte is the serialization version; the rest is the Snappy block.
     A file the decoder refuses is counted as skipped, never guessed at. */
  if(!snappy_raw::decode(reinterpret_cast<const unsigned char*>(blob.data())+1,
                         blob.size()-1,kMaxRawBytes,raw)){++result.skipped;continue;}
  scanCircuit(raw,registry,result);
 }
 return result;
}
/* ------------------------------------------------------------------- report */
struct Dependency {
 uint64_t id=0;
 std::string tag,type,mod;
 std::string recordedVersion,installedVersion;
 int schema=-1;
 size_t references=0;
 bool known=false,installed=false,enabled=false;
};
struct SaveReport {
 ScanResult scan;
 std::vector<Dependency> dependencies;   /* sorted: missing first, then by id */
 size_t missing=0,disabled=0,unknown=0,versionMismatch=0;
};
inline SaveReport buildReport(const ScanResult& scan,const TypeRegistry& registry,
                              const std::map<std::string,std::string>& installed,
                              const std::set<std::string>& enabled){
 SaveReport report;report.scan=scan;
 for(auto& [id,count]:scan.ids){
  Dependency dependency;
  dependency.id=id;dependency.tag=tagOf(id);dependency.references=count;
  if(const ComponentType* type=registry.find(id)){
   dependency.known=true;dependency.type=type->name;dependency.mod=type->mod;
   dependency.schema=type->schema;dependency.recordedVersion=type->version;
  }
  if(!dependency.mod.empty()){
   const auto found=installed.find(dependency.mod);
   if(found!=installed.end()){
    dependency.installed=true;dependency.installedVersion=found->second;
    dependency.enabled=enabled.count(dependency.mod)>0;
   }
  }
  if(!dependency.installed)++report.missing;
  else if(!dependency.enabled)++report.disabled;
  if(!dependency.known)++report.unknown;
  if(dependency.installed&&!dependency.recordedVersion.empty()&&
     !dependency.installedVersion.empty()&&dependency.recordedVersion!=dependency.installedVersion)
   ++report.versionMismatch;
  report.dependencies.push_back(std::move(dependency));
 }
 std::sort(report.dependencies.begin(),report.dependencies.end(),
           [](const Dependency& a,const Dependency& b){
  const int rankA=a.known?(a.installed?(a.enabled?3:2):1):0;
  const int rankB=b.known?(b.installed?(b.enabled?3:2):1):0;
  if(rankA!=rankB)return rankA<rankB;
  return a.id<b.id;
 });
 return report;
}
}
