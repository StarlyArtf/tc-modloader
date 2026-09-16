#pragma once
#include "../../vendor/json.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <array>
#include <algorithm>
using RGB=std::array<int,3>;
struct Palette {
 std::vector<RGB> colors;std::vector<int> recent;
 int add(RGB rgb){auto it=std::find(colors.begin(),colors.end(),rgb);int id;
  if(it!=colors.end())id=11+int(it-colors.begin());else{if(colors.size()>=244)throw std::runtime_error("已达到 244 种永久颜色上限；已有导线颜色不会被替换。");colors.push_back(rgb);id=10+(int)colors.size();}
  recent.erase(std::remove(recent.begin(),recent.end(),id),recent.end());recent.insert(recent.begin(),id);if(recent.size()>5)recent.resize(5);return id;
 }
 void load(const std::filesystem::path& path){if(!std::filesystem::exists(path))return;std::ifstream f(path);nlohmann::json j;f>>j;if(j.at("version")!=1)throw std::runtime_error("Unknown palette version");colors=j.at("colors").get<std::vector<RGB>>();recent=j.at("recent").get<std::vector<int>>();if(colors.size()>244||recent.size()>5)throw std::runtime_error("Invalid palette size");for(auto c:colors)for(int v:c)if(v<0||v>255)throw std::runtime_error("Invalid RGB");for(int id:recent)if(id<11||id>=11+(int)colors.size())throw std::runtime_error("Invalid history ID");}
 void save(const std::filesystem::path& path)const{auto temp=path;temp+=L".tmp";std::ofstream f(temp,std::ios::binary|std::ios::trunc);f<<nlohmann::json{{"version",1},{"colors",colors},{"recent",recent}}.dump(2);f.flush();if(!f)throw std::runtime_error("无法保存颜色记录");f.close();if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("无法替换颜色记录");}
};
