#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <functional>
#include "json.hpp"
#include "miniz.h"
#include "capabilities.hpp"
#include "version.hpp"
namespace tc {
namespace fs = std::filesystem;
using J = nlohmann::json;
inline std::string read(const fs::path& p) { std::ifstream f(p,std::ios::binary); if(!f) throw std::runtime_error("Cannot read: "+p.u8string()); return {std::istreambuf_iterator<char>(f),{}}; }
inline std::string hash(const std::string& bytes) {
 BCRYPT_ALG_HANDLE a{}; BCRYPT_HASH_HANDLE h{}; DWORD cb{},len{}; unsigned char out[32];
 if(BCryptOpenAlgorithmProvider(&a,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("SHA256 unavailable");
 BCryptGetProperty(a,BCRYPT_OBJECT_LENGTH,(PUCHAR)&len,sizeof(len),&cb,0); std::vector<unsigned char> obj(len);
 auto ok=BCryptCreateHash(a,&h,obj.data(),len,nullptr,0,0);
 if(ok>=0) ok=BCryptHashData(h,(PUCHAR)bytes.data(),(ULONG)bytes.size(),0);
 if(ok>=0) ok=BCryptFinishHash(h,out,32,0);
 if(h) BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(a,0);
 if(ok<0) throw std::runtime_error("SHA256 failed");
 std::ostringstream s; for(auto b:out) s<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)b; return s.str();
}
inline void atomic(const fs::path& p,const std::string& data) {
 fs::create_directories(p.parent_path()); auto tmp=p; tmp+=L".tc-tmp";
 {std::ofstream f(tmp,std::ios::binary|std::ios::trunc); f.write(data.data(),data.size()); f.flush(); if(!f) throw std::runtime_error("Write failed: "+p.u8string());}
 if(!MoveFileExW(tmp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Replace failed: "+p.u8string());
}
inline J jsonfile(const fs::path& p,J fallback=J::object()) {return fs::exists(p)?J::parse(read(p)):fallback;}
inline std::string lower(std::string s) { for(char& c:s) if(c>='A'&&c<='Z') c+=32; return s; }
inline bool safe(const std::string& s) {
 if(s.empty()||s.size()>200||s.front()=='/'||s.find('\\')!=s.npos||s.find(':')!=s.npos||s.find('\0')!=s.npos) return false;
 std::stringstream ss(s); std::string part;
 while(std::getline(ss,part,'/')) {if(part.empty()||part=="."||part==".."||part.back()=='.'||part.back()==' ')return false;
  for(unsigned char c:part) if(c<32||std::string("<>\"|?*").find(c)!=std::string::npos)return false;
  auto stem=lower(part.substr(0,part.find('.'))); if(stem=="con"||stem=="prn"||stem=="aux"||stem=="nul"||(stem.size()==4&&(stem.substr(0,3)=="com"||stem.substr(0,3)=="lpt")&&stem[3]>='0'&&stem[3]<='9')) return false;
 } return s.back()!='/';
}
inline void no_links(const fs::path& root,const fs::path& p) {
 auto rel=p.lexically_relative(root); if(rel.empty()||*rel.begin()=="..") throw std::runtime_error("Path outside game folder");
 fs::path cur=root; for(auto& x:rel) {cur/=x; DWORD a=GetFileAttributesW(cur.c_str()); if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Linked paths are not supported: "+cur.u8string());}
}
inline bool resource(const std::string& rel){auto prefix=rel.substr(0,rel.find('/'));return safe(rel)&&rel.find('/')!=rel.npos&&(prefix=="asset"||prefix=="campaign"||prefix=="translations");}
inline std::string trim(const std::string& s){auto a=s.find_first_not_of(" \t\r\n");if(a==std::string::npos)return "";auto b=s.find_last_not_of(" \t\r\n");return s.substr(a,b-a+1);}
inline bool valid_id(const std::string& id){return !id.empty()&&id.size()<=80&&id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-")==id.npos;}
/* Mod versions are free-form text, so a dependency constraint is checked
   against the leading dotted numeric run - 1.2.10 sorts after 1.9, which a
   plain string comparison gets backwards - and falls back to a string
   comparison when a version carries no numbers at all.  Accepted forms:
   "" or "*" (any), "1.2.3", "=1.2.3", "!=1.2.3", ">=1.2.3", ">1.2.3",
   "<=1.2.3", "<1.2.3", and several of those joined by commas (all must hold).
   Trailing text is ignored ("1.0.0-beta" compares as 1.0.0). */
inline std::vector<long long> version_numbers(const std::string& text){
 std::vector<long long> out;size_t i=0;if(i<text.size()&&(text[i]=='v'||text[i]=='V'))i++;
 while(i<text.size()){
  if(text[i]<'0'||text[i]>'9')break;long long value=0;
  while(i<text.size()&&text[i]>='0'&&text[i]<='9'){if(value<1000000000)value=value*10+(text[i]-'0');i++;}
  out.push_back(value);
  if(i<text.size()&&text[i]=='.'){i++;continue;}
  break;
 }
 return out;
}
inline int version_compare(const std::string& a,const std::string& b){
 auto x=version_numbers(a),y=version_numbers(b);
 if(!x.empty()&&!y.empty()){auto n=std::max(x.size(),y.size());
  for(size_t i=0;i<n;i++){auto l=i<x.size()?x[i]:0,r=i<y.size()?y[i]:0;if(l<r)return -1;if(l>r)return 1;}
  return 0;}
 if(a==b)return 0;return a<b?-1:1;
}
inline bool version_satisfies(const std::string& version,const std::string& constraint){
 std::string text=trim(constraint);
 if(text.empty()||text=="*")return true;
 size_t start=0;
 while(true){
  auto comma=text.find(',',start);
  std::string part=trim(text.substr(start,comma==std::string::npos?std::string::npos:comma-start));
  if(!part.empty()){
   std::string op="=",operand=part;
   for(auto candidate:{std::string("!="),std::string(">="),std::string("<="),std::string("=="),std::string("="),std::string(">"),std::string("<")})
    if(part.compare(0,candidate.size(),candidate)==0){op=candidate;operand=trim(part.substr(candidate.size()));break;}
   if(operand.empty())return false;
   auto cmp=version_compare(version,operand);
   auto ok=op=="!="?cmp!=0:op==">="?cmp>=0:op=="<="?cmp<=0:op==">"?cmp>0:op=="<"?cmp<0:cmp==0;
   if(!ok)return false;
  }
  if(comma==std::string::npos)break;start=comma+1;
 }
 return true;
}
struct Mod {
 std::string id,name,version,author,description,error,digest,entry; fs::path source;
 std::map<std::string,std::string> files,native;
 /* Required and optional dependencies, in declaration order, plus the version
    constraint each one carries ("" means "any version"). */
 std::vector<std::string> dependencies,optional;
 std::map<std::string,std::string> required_versions,optional_versions;
 /* Capability names this package asked for in mod.json; every one of them was
    already checked against the loader's own mask while the package was
    scanned, so a package that reaches apply() has what it declared. */
 std::vector<std::string> capabilities;
};
/* "requires" is either a list of ids (the original form) or an object that maps
   an id to a version constraint; "optional" reads the same way but a missing or
   unselected entry is not an error. */
inline void parse_dependencies(const J& j,const char* key,std::vector<std::string>& ids,std::map<std::string,std::string>& constraints){
 if(!j.contains(key))return;
 const auto& node=j.at(key);
 auto add=[&](const std::string& id,const std::string& constraint){
  if(!valid_id(id))throw std::runtime_error(std::string("Invalid mod id in ")+key+": "+id);
  if(constraints.count(id))return;
  ids.push_back(id);constraints[id]=trim(constraint);
 };
 if(node.is_array()){for(auto& entry:node){if(!entry.is_string())throw std::runtime_error(std::string(key)+" entries must be mod ids");add(entry.get<std::string>(),"");}}
 else if(node.is_object()){for(auto it=node.begin();it!=node.end();++it){if(!it.value().is_string())throw std::runtime_error(std::string(key)+" version constraints must be strings");add(it.key(),it.value().get<std::string>());}}
 else throw std::runtime_error(std::string(key)+" must be an array of ids or an object of id/constraint pairs");
}
inline std::string constraint_of(const std::map<std::string,std::string>& constraints,const std::string& id){
 auto it=constraints.find(id);return it==constraints.end()?std::string():it->second;
}
/* A package states what it needs from the loader by name, so the answer is
   decided while the package is scanned: the player sees "this loader does not
   provide ui_slot" in the list instead of a plugin that fails halfway through
   tc_mod_load. */
inline void parse_capabilities(const J& j,uint64_t loader,std::vector<std::string>& out){
 if(!j.contains("capabilities"))return;
 if(!j.at("capabilities").is_array())throw std::runtime_error("capabilities must be an array");
 for(auto& entry:j.at("capabilities")){
  if(!entry.is_string())throw std::runtime_error("capability entries must be names");
  auto name=entry.get<std::string>();auto bit=capability_bit(name);
  if(!bit)throw std::runtime_error("Unknown loader capability: "+name);
  if(!(loader&bit))throw std::runtime_error("This loader does not provide the capability: "+name);
  out.push_back(name);
 }
}
/* A dependency may carry a version constraint; check it only when the dependent
   package is actually selected, which is also the only case where the version
   in the enabled set is the one that will run. */
inline void check_dependency_version(const Mod& user,const std::string& id,const Mod& dep,const std::string& constraint){
 if(constraint.empty())return;
 if(!version_satisfies(dep.version,constraint))throw std::runtime_error(user.name+" requires "+id+" "+constraint+", but the enabled version is "+(dep.version.empty()?std::string("(none)"):dep.version));
}
struct Lock {HANDLE h; Lock(const fs::path& p) {h=CreateFileW(p.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr); if(h==INVALID_HANDLE_VALUE) throw std::runtime_error("Another Mod operation is running");} ~Lock(){CloseHandle(h);} };
class Core {
public:
 fs::path root,dir; std::vector<Mod> mods; J state; std::string notice;
 /* What this loader provides to native plugins, and therefore what a package may
    list in mod.json's "capabilities".  Settable so a test can stand in for an
    older or smaller loader. */
 uint64_t capabilities=loader_capabilities();
 /* Scale the game's main menu is currently drawn with (the home page
    multiplies its coordinates by it).  The loader records it each time it
    draws inside that menu, so the native page container can match the game's
    text size instead of guessing one. */
 float ui_scale=1.f;
 Core(fs::path r,uint64_t caps=loader_capabilities()):root(fs::absolute(r).lexically_normal()),dir(root/L"tc-modloader-data") {capabilities=caps;no_links(root,dir); no_links(root,dir/L"blobs"); fs::create_directories(dir/L"blobs"); for(auto name:{L"lock",L"state.json",L"transaction.json",L"state.json.tc-tmp",L"transaction.json.tc-tmp"})no_links(root,dir/name); Lock lock(dir/L"lock"); recover(); reload();}
 void reload(){state=jsonfile(dir/L"state.json",J{{"enabled",J::array()},{"files",J::object()}});}
 bool enabled(const std::string& id) const {for(auto& e:state.at("enabled")) if(e==id)return true;return false;}
 std::set<std::string> enabled_set() const {std::set<std::string> r;for(auto& e:state.at("enabled"))r.insert(e.get<std::string>());return r;}
 std::string current(const std::string& rel){auto p=root/fs::u8path(rel);no_links(root,p);return fs::exists(p)?hash(read(p)):"";}
 std::string blob(const std::string& data){auto h=hash(data);auto p=dir/L"blobs"/h;no_links(root,p);if(!fs::exists(p))atomic(p,data);return h;}
 void put(const std::string& rel,const std::string& h) {if(!resource(rel))throw std::runtime_error("Invalid journal path");auto p=root/fs::u8path(rel);no_links(root,p);auto tmp=p;tmp+=L".tc-tmp";no_links(root,tmp); if(h.empty()){if(fs::exists(p))fs::remove(p);}else {if(h.size()!=64||h.find_first_not_of("0123456789abcdef")!=h.npos)throw std::runtime_error("Invalid backup hash");no_links(root,dir/L"blobs"/h);auto data=read(dir/L"blobs"/h);if(hash(data)!=h)throw std::runtime_error("Backup checksum failed");atomic(p,data);}}
 void recover(){auto p=dir/L"transaction.json";if(!fs::exists(p))return;auto j=jsonfile(p);auto s=jsonfile(dir/L"state.json");if(s.value("transaction","")==j.at("id").get<std::string>()){fs::remove(p);return;}
  for(auto& e:j.at("changes")){auto c=current(e.at("path"));if(c!=e.at("before")&&c!=e.at("after"))throw std::runtime_error("Recovery stopped: externally modified file "+e.at("path").get<std::string>());}
  for(auto& e:j.at("changes"))put(e.at("path"),e.at("before"));fs::remove(p);notice="Recovered an interrupted operation.";
 }
 void scan(){mods.clear();auto md=root/L"mods";no_links(root,md);fs::create_directories(md);
  for(auto& item:fs::directory_iterator(md)){if(!item.is_regular_file()||lower(item.path().extension().u8string())!=".mod")continue;
   Mod m;m.source=item.path();m.name=item.path().filename().u8string(); mz_zip_archive z{};
   try {no_links(root,item.path());if(item.file_size()>128*1024*1024)throw std::runtime_error("Package exceeds 128 MiB");auto raw=read(item.path());m.digest=hash(raw);
    if(!mz_zip_reader_init_mem(&z,raw.data(),raw.size(),0))throw std::runtime_error("Not a valid .mod ZIP archive");
    std::map<std::string,std::string> contents;std::set<std::string> seen;uint64_t total=0;
    auto n=mz_zip_reader_get_num_files(&z);if(n>4096)throw std::runtime_error("Too many package entries");
    for(mz_uint i=0;i<n;i++){mz_zip_archive_file_stat st{};if(!mz_zip_reader_file_stat(&z,i,&st))throw std::runtime_error("Invalid archive entry");if(mz_zip_reader_is_file_a_directory(&z,i))continue;
     std::string path=st.m_filename;if(!safe(path))throw std::runtime_error("Unsafe package path: "+path);
     if(!seen.insert(lower(path)).second)throw std::runtime_error("Duplicate package path: "+path);
     total+=st.m_uncomp_size;if(st.m_uncomp_size>64*1024*1024||total>256*1024*1024)throw std::runtime_error("Expanded package is too large");
     if(path!="mod.json"&&path.rfind("files/",0)!=0&&path.rfind("native/",0)!=0)throw std::runtime_error("Only mod.json, files/ and native/ are supported");
     size_t len{};void* data=mz_zip_reader_extract_to_heap(&z,i,&len,0);if(!data)throw std::runtime_error("Archive CRC/decompression failed");contents[path]=std::string((char*)data,len);mz_free(data);
    }
    if(!contents.count("mod.json"))throw std::runtime_error("Missing mod.json");if(contents.at("mod.json").size()>65536)throw std::runtime_error("Manifest exceeds 64 KiB");auto j=J::parse(contents.at("mod.json"));if(j.at("format")!=1&&j.at("format")!=2)throw std::runtime_error("Unsupported Mod format");
    m.id=j.at("id").get<std::string>();if(!valid_id(m.id))throw std::runtime_error("Invalid Mod id");
    m.name=j.at("name");m.version=j.at("version");m.author=j.value("author","");m.description=j.value("description","");
    parse_dependencies(j,"requires",m.dependencies,m.required_versions);
    parse_dependencies(j,"optional",m.optional,m.optional_versions);
    parse_capabilities(j,capabilities,m.capabilities);
    if(j.contains("native")){if(j.at("format")!=2||j.at("native").at("api")!=1)throw std::runtime_error("Native plugin requires format 2 / API 1");m.entry=j.at("native").at("entry").get<std::string>();if(!safe(m.entry)||m.entry.rfind("native/",0)!=0||lower(fs::u8path(m.entry).extension().u8string())!=".dll")throw std::runtime_error("Invalid native entry");}
    for(auto& [p,data]:contents){if(p=="mod.json")continue;if(p.rfind("native/",0)==0){m.native[p]=std::move(data);continue;}auto rel=p.substr(6);if(!resource(rel))throw std::runtime_error("Unsupported target: "+rel);m.files[rel]=std::move(data);}
    if(!m.native.empty()&&m.entry.empty())throw std::runtime_error("Native files require a native manifest entry");
    if(!m.entry.empty()){if(!m.native.count(m.entry))throw std::runtime_error("Native entry DLL missing");auto& b=m.native.at(m.entry);if(b.size()<256||b.substr(0,2)!="MZ")throw std::runtime_error("Invalid native DLL");uint32_t pe{};memcpy(&pe,b.data()+0x3c,4);if(pe>b.size()-26||b.substr(pe,4)!=std::string("PE\0\0",4))throw std::runtime_error("Invalid native PE header");uint16_t machine{},flags{};memcpy(&machine,b.data()+pe+4,2);memcpy(&flags,b.data()+pe+22,2);if(machine!=0x8664||!(flags&0x2000))throw std::runtime_error("Native entry must be a Windows x64 DLL");}
    for(auto& patch:j.value("patches",J::array())) {auto rel=patch.at("path").get<std::string>();if(!resource(rel))throw std::runtime_error("Unsupported patch target: "+rel);
     if(!m.files.count(rel)){std::string original;bool managed=false;for(auto it=state["files"].begin();it!=state["files"].end();++it)if(lower(it.key())==lower(rel)){auto h=it.value().at("original").get<std::string>();if(h.size()!=64||h.find_first_not_of("0123456789abcdef")!=h.npos)throw std::runtime_error("Patch target has no original file");no_links(root,dir/L"blobs"/h);original=read(dir/L"blobs"/h);if(hash(original)!=h)throw std::runtime_error("Backup checksum failed");managed=true;break;}if(!managed){no_links(root,root/fs::u8path(rel));original=read(root/fs::u8path(rel));}m.files[rel]=original;}
     auto find=patch.at("find").get<std::string>(),replacement=patch.at("replace").get<std::string>();auto& data=m.files[rel];auto at=data.find(find);if(find.empty()||at==data.npos||data.find(find,at+find.size())!=data.npos)throw std::runtime_error("Patch must match exactly once: "+rel);data.replace(at,find.size(),replacement);
    }
    if(m.files.empty()&&m.entry.empty())throw std::runtime_error("Package has no resource files or native entry");
   }catch(const std::exception& e){m.error=e.what();m.files.clear();}if(z.m_pState)mz_zip_reader_end(&z);mods.push_back(std::move(m));
  }
  std::sort(mods.begin(),mods.end(),[](const Mod&a,const Mod&b){return a.id<b.id;});
  std::map<std::string,int> counts;for(auto& m:mods)if(!m.id.empty())counts[m.id]++;for(auto& m:mods)if(counts[m.id]>1)m.error="Duplicate Mod id";
 }
 void apply(const std::set<std::string>& selected){Lock lock(dir/L"lock");recover();reload();scan();std::map<std::string,const Mod*> byId;for(auto& m:mods)if(m.error.empty())byId[m.id]=&m;
  std::map<std::string,std::string> desired,owners;
  for(auto& id:selected){if(!byId.count(id))throw std::runtime_error("Missing or invalid Mod: "+id);auto& m=*byId.at(id);
   for(auto& req:m.dependencies){if(!selected.count(req)||!byId.count(req))throw std::runtime_error(m.name+" requires "+req);check_dependency_version(m,req,*byId.at(req),constraint_of(m.required_versions,req));}
   /* An optional dependency only has a version to check when it is enabled as
      well; skipping it is exactly what "optional" means. */
   for(auto& opt:m.optional){if(!selected.count(opt)||!byId.count(opt))continue;check_dependency_version(m,opt,*byId.at(opt),constraint_of(m.optional_versions,opt));}
   for(auto& [rel,data]:m.files){auto key=lower(rel);if(owners.count(key))throw std::runtime_error("File conflict: "+rel+" ("+owners[key]+" / "+id+")");owners[key]=id;desired[rel]=data;}}
  // Resolve case aliases against existing deployment paths on Windows.
  std::map<std::string,std::string> existing;for(auto it=state["files"].begin();it!=state["files"].end();++it)existing[lower(it.key())]=it.key();
  std::map<std::string,std::string> normalized;for(auto& [rel,data]:desired)normalized[existing.count(lower(rel))?existing[lower(rel)]:rel]=data;desired=std::move(normalized);
  std::set<std::string> visiting,visited;std::function<void(const std::string&)> visit=[&](const std::string& id){if(visited.count(id))return;if(!visiting.insert(id).second)throw std::runtime_error("Dependency cycle: "+id);for(auto& dep:byId.at(id)->dependencies)visit(dep);visiting.erase(id);visited.insert(id);};for(auto& id:selected)visit(id);
  J next={{"enabled",selected},{"files",J::object()},{"native",J::object()}};for(auto& id:selected)if(!byId.at(id)->entry.empty())next["native"][id]=byId.at(id)->digest;
  std::set<std::string> paths;for(auto& [rel,data]:desired)paths.insert(rel);for(auto it=state["files"].begin();it!=state["files"].end();++it)paths.insert(it.key());
  J changes=J::array();
  for(auto& rel:paths){auto before=current(rel);bool old=state["files"].contains(rel);if(old&&before!=state["files"][rel].at("deployed"))throw std::runtime_error("File changed outside loader; preserve it before retrying: "+rel);
   std::string original=old?state["files"][rel].at("original").get<std::string>():before;
   if(!old&&!before.empty())blob(read(root/fs::u8path(rel)));
   std::string after=desired.count(rel)?blob(desired.at(rel)):original;
   if(desired.count(rel))next["files"][rel]={{"original",original},{"deployed",after}};
   if(before!=after){if(!before.empty())blob(read(root/fs::u8path(rel)));changes.push_back({{"path",rel},{"before",before},{"after",after}});}}
  auto id=hash(next.dump()+std::to_string(GetTickCount64()));next["transaction"]=id;atomic(dir/L"transaction.json",J{{"id",id},{"changes",changes}}.dump(2));
  try {for(auto& e:changes){if(current(e["path"])!=e["before"])throw std::runtime_error("File changed during apply");put(e["path"],e["after"]);}atomic(dir/L"state.json",next.dump(2));fs::remove(dir/L"transaction.json");state=next;notice="Saved. Restart the game to fully apply changes.";}
  catch(...){recover();reload();throw;}
 }
};
}
