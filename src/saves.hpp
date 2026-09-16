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
};
}
