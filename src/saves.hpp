#pragma once
#include "core.hpp"
#include "save_boot.hpp"
namespace tc {
class SaveProfiles {
 fs::path root,config;
 static void unlinked(const fs::path& p){no_links(p.root_path(),p);}
 static std::map<fs::path,std::string> snapshot(const fs::path& source){
  unlinked(source);if(!fs::is_directory(source))throw std::runtime_error("原版存档目录不存在");
  std::map<fs::path,std::string> files;uint64_t total=0;
  for(auto& e:fs::recursive_directory_iterator(source)){unlinked(e.path());if(e.is_directory())continue;if(!e.is_regular_file())throw std::runtime_error("不支持的存档文件类型");
   auto rel=e.path().lexically_relative(source);if(lower(rel.filename().u8string())=="steam_autocloud.vdf")continue;
   auto size=e.file_size();total+=size;if(size>256ULL*1024*1024||total>1024ULL*1024*1024||files.size()>=100000)throw std::runtime_error("存档超过导入容量限制");files[rel]=hash(read(e.path()));
  }if(files.empty())throw std::runtime_error("原版存档为空");return files;
 }
 /* "YYYY-MM-DD HH:MM" for one file, from the OS.  The page shows this next to
    every profile, and std::filesystem's file_clock epoch is not portable to
    print, so the one path that won the newest-mtime comparison is asked
    directly. */
 static std::string stamp(const fs::path& path){
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data))return std::string();
  FILETIME local{};SYSTEMTIME time{};
  if(!FileTimeToLocalFileTime(&data.ftLastWriteTime,&local)||!FileTimeToSystemTime(&local,&time))
   return std::string();
  char text[32]{};
  std::snprintf(text,sizeof(text),"%04d-%02d-%02d %02d:%02d",time.wYear,time.wMonth,time.wDay,
                time.wHour,time.wMinute);
  return std::string(text);
 }
 static std::wstring fresh(const char* prefix){
  unsigned char random[12]{};if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
   throw std::runtime_error("Random profile ID unavailable");
  std::wstring id(prefix,prefix+std::strlen(prefix));id+=L"-";const char* hex="0123456789abcdef";
  for(auto b:random){id+=(wchar_t)hex[b>>4];id+=(wchar_t)hex[b&15];}
  return id;
 }
public:
 fs::path original,profiles;
 explicit SaveProfiles(const fs::path& game):root(game),config(game/L"tc-modloader-data"/L"saves.ini"){
  wchar_t home[32768];DWORD n=GetEnvironmentVariableW(L"USERPROFILE",home,32768);if(!n||n>=32768)throw std::runtime_error("USERPROFILE unavailable");
  auto roaming=fs::path(home)/L"AppData"/L"Roaming";original=roaming/L"Turing Complete";profiles=roaming/L"Turing Complete Mods"/L"profiles";
 }
 std::wstring next()const{wchar_t p[80];unlinked(config);GetPrivateProfileStringW(L"saves",L"profile",L"default",p,80,config.c_str());if(!tc_save_boot::valid(p))throw std::runtime_error("Invalid save profile");return p;}
 fs::path path(const std::wstring& id)const{if(!tc_save_boot::valid(id.c_str()))throw std::runtime_error("Invalid save profile");return profiles/id;}
 std::wstring import_original(){
  unlinked(config);auto temp=config;temp+=L".tc-tmp";unlinked(temp);unlinked(profiles);fs::create_directories(profiles);
  unlinked(root/L"tc-modloader-data"/L"save-import.lock");Lock lock(root/L"tc-modloader-data"/L"save-import.lock");
  auto files=snapshot(original);unsigned char random[12];if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("Random profile ID unavailable");
  std::string id="import-";const char* hex="0123456789abcdef";for(auto b:random){id+=hex[b>>4];id+=hex[b&15];}auto dest=profiles/id;unlinked(dest);if(!fs::create_directory(dest))throw std::runtime_error("Profile already exists");
  for(auto& [rel,digest]:files){auto source=original/rel,target=dest/rel;unlinked(source);unlinked(target);auto data=read(source);if(hash(data)!=digest)throw std::runtime_error("原版存档正在变化，请关闭原版游戏后重试");atomic(target,data);if(hash(read(target))!=digest)throw std::runtime_error("导入副本校验失败");}
  if(snapshot(original)!=files)throw std::runtime_error("原版存档正在变化，请关闭原版游戏后重试");
  atomic(config,"[saves]\r\nprofile="+id+"\r\n");return fs::u8path(id).wstring();
 }
 /* ---- managing the profiles ------------------------------------------------

    The page lists what is on disk, so this half deliberately owns no state: the
    list is a directory walk, and "current" and "selected" are read back from the
    boot profile and from saves.ini (a change made here only takes effect on the
    next start, exactly like a Mod being enabled).  A removed profile is moved
    aside as trash-<id>-<stamp> rather than deleted: the page offers no undo, and
    a save is the one thing in this loader that cannot be rebuilt. */
 struct Entry {
  std::wstring id;fs::path path;
  unsigned long long bytes=0,files=0;
  std::string modified;
  bool current=false,selected=false,imported=false;
 };
 std::vector<std::string> trash()const{
  std::vector<std::string> ids;
  std::error_code error;
  if(!fs::is_directory(profiles,error))return ids;
  for(auto& item:fs::directory_iterator(profiles,error)){
   const std::string name=item.path().filename().u8string();
   if(name.rfind("trash-",0)==0)ids.push_back(name);
  }
  std::sort(ids.begin(),ids.end());
  return ids;
 }
 std::vector<Entry> list()const{
  std::error_code error;
  if(!fs::is_directory(profiles,error))return {};
  const std::wstring selected=next();
  std::vector<Entry> entries;
  for(auto& item:fs::directory_iterator(profiles,error)){
   unlinked(item.path());
   if(!item.is_directory())continue;
   const std::wstring id=item.path().filename().wstring();
   if(!tc_save_boot::valid(id.c_str()))continue;
   if(id.rfind(L"trash-",0)==0)continue;
   Entry entry;entry.id=id;entry.path=item.path();
   entry.current=id==tc_save_boot::profile;entry.selected=id==selected;
   entry.imported=id.rfind(L"import-",0)==0;
   fs::file_time_type newest{};bool haveNewest=false;fs::path newestPath;
   for(auto& file:fs::recursive_directory_iterator(item.path(),error)){
    if(!file.is_regular_file())continue;
    if(++entry.files>100000)throw std::runtime_error("存档文件过多，无法统计");
    /* No per-file reparse-point walk here: no_links() stats every component of
       the path, and doing that for each of a played-through profile's ~140
       circuits cost ~100 ms per refresh on the render thread.  The profile
       directory itself is still checked above, and this walk only ever reads. */
    std::error_code sizeError;
    const uint64_t size=file.file_size(sizeError);
    if(sizeError)continue;
    entry.bytes+=size;
    if(entry.bytes>4096ULL*1024*1024*1024)throw std::runtime_error("存档过大，无法统计");
    if(!haveNewest||file.last_write_time()>newest){newest=file.last_write_time();newestPath=file.path();haveNewest=true;}
   }
   if(haveNewest)entry.modified=stamp(newestPath);
   entries.push_back(std::move(entry));
  }
  std::sort(entries.begin(),entries.end(),[](const Entry& a,const Entry& b){
   if(a.current!=b.current)return a.current;
   if(a.selected!=b.selected)return a.selected;
   if(a.modified!=b.modified)return a.modified>b.modified;
   return a.id<b.id;
  });
  return entries;
 }
 /* Takes effect on the next start; the running process keeps the profile it
    redirected at boot. */
 void select(const std::wstring& id){
  if(!tc_save_boot::valid(id.c_str()))throw std::runtime_error("存档名不合法");
  std::error_code error;
  if(!fs::is_directory(path(id),error))throw std::runtime_error("存档不存在");
  unlinked(config);
  /* Profile ids are ASCII by construction (valid()), so the ini stays a plain
     byte string; fs::u8path would need a char source to build the same id. */
  std::string text;for(auto c:id)text+=(char)c;
  atomic(config,"[saves]\r\nprofile="+text+"\r\n");
 }
 std::wstring create(){
  unlinked(config);fs::create_directories(profiles);
  for(int attempt=0;attempt<8;++attempt){
   const std::wstring id=fresh("local");
   std::error_code error;
   if(fs::create_directory(path(id),error))return id;
  }
  throw std::runtime_error("无法新建存档目录");
 }
 std::wstring duplicate(const std::wstring& id){
  if(!tc_save_boot::valid(id.c_str()))throw std::runtime_error("存档名不合法");
  unlinked(config);
  const auto files=snapshot(path(id));
  for(int attempt=0;attempt<8;++attempt){
   const std::wstring copy=fresh("copy");
   const fs::path dest=path(copy);
   std::error_code error;
   if(!fs::create_directory(dest,error))continue;
   for(auto& [rel,digest]:files){
    const auto data=read(path(id)/rel);
    if(hash(data)!=digest)throw std::runtime_error("复制时源存档发生变化");
    atomic(dest/rel,data);
    if(hash(read(dest/rel))!=digest)throw std::runtime_error("复制副本校验失败");
   }
   return copy;
  }
  throw std::runtime_error("无法新建存档目录");
 }
 std::wstring rename(const std::wstring& id,const std::wstring& to){
  if(!tc_save_boot::valid(id.c_str())||!tc_save_boot::valid(to.c_str()))
   throw std::runtime_error("存档名不合法（小写字母、数字与短横线，最长 64 字符）");
  std::error_code error;
  if(!fs::is_directory(path(id),error))throw std::runtime_error("存档不存在");
  if(fs::exists(path(to),error))throw std::runtime_error("同名存档已存在");
  unlinked(config);unlinked(path(id));unlinked(path(to));
  fs::rename(path(id),path(to),error);
  if(error)throw std::runtime_error("重命名失败："+error.message());
  if(next()==id)select(to);
  return to;
 }
 /* Refuses the profile the next start would use: switching is one click, and a
    save that is silently gone is not something this page can undo. */
 void remove(const std::wstring& id){
  if(!tc_save_boot::valid(id.c_str()))throw std::runtime_error("存档名不合法");
  if(next()==id)throw std::runtime_error("这是当前存档；先切换到别的存档再删除");
  unlinked(config);
  const fs::path source=path(id);std::error_code error;
  if(!fs::is_directory(source,error))throw std::runtime_error("存档不存在");
  char stampText[32]{};SYSTEMTIME now{};GetLocalTime(&now);
  std::snprintf(stampText,sizeof(stampText),"%04d%02d%02d-%02d%02d%02d",now.wYear,now.wMonth,
                now.wDay,now.wHour,now.wMinute,now.wSecond);
  fs::path target=profiles/(L"trash-"+id+L"-"+std::wstring(stampText,stampText+std::strlen(stampText)));
  unlinked(target);
  fs::rename(source,target,error);
  if(error)throw std::runtime_error("删除失败："+error.message());
 }
};
}
