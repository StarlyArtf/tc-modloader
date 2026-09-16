#include "core.hpp"
#include "compat.hpp"
#include <commdlg.h>
#include <shellapi.h>
#include <tlhelp32.h>
static std::wstring wide(const std::string&s){int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);std::wstring w(n,L'\0');MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,w.data(),n);return w;}
static void stopped(const tc::fs::path& root){HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snap==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot check running games");PROCESSENTRY32W e{};e.dwSize=sizeof(e);bool running=false;
 if(Process32FirstW(snap,&e))do{if(_wcsicmp(e.szExeFile,L"Turing Complete.exe"))continue;HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,e.th32ProcessID);if(p){wchar_t buf[32768];DWORD n=32768;if(QueryFullProcessImageNameW(p,0,buf,&n)&&tc::fs::path(buf).parent_path()==root)running=true;CloseHandle(p);}else running=true;}while(Process32NextW(snap,&e));CloseHandle(snap);if(running)throw std::runtime_error("Close Turing Complete before installing or uninstalling.");}
static std::string payload(){HRSRC r=FindResourceW(nullptr,MAKEINTRESOURCEW(101),RT_RCDATA);if(!r)throw std::runtime_error("Embedded loader is missing");HGLOBAL h=LoadResource(nullptr,r);return std::string((const char*)LockResource(h),SizeofResource(nullptr,r));}
static void install(const tc::fs::path& root,bool remove){
 auto exe=root/L"Turing Complete.exe",engine=root/L"game_engine.dll",backup=root/L"tc_game_engine.dll";tc::no_links(root,exe);tc::no_links(root,engine);tc::no_links(root,backup);stopped(root);
 if(tc::hash(tc::read(exe))!=TC_EXE_SHA)throw std::runtime_error("Unsupported Turing Complete.exe build. This release only supports the documented SHA-256.");
 auto bytes=payload();auto deployed=tc::hash(bytes);auto actual=tc::hash(tc::read(engine));
 if(remove){
  if(actual==TC_ENGINE_SHA&&!tc::fs::exists(backup))return;
  if(actual!=deployed)throw std::runtime_error("Engine changed since installation. Refusing to overwrite it.");
  if(tc::hash(tc::read(backup))!=TC_ENGINE_SHA)throw std::runtime_error("Original engine backup is invalid.");
  tc::Core core(root);core.apply({});tc::atomic(engine,tc::read(backup));tc::fs::remove(backup);return;
 }
 if(actual!=TC_ENGINE_SHA&&actual!=deployed&&actual!=TC_PREVIOUS_LOADER_SHA&&actual!=TC_LOADER_020_SHA)throw std::runtime_error("Engine is unsupported or another loader is installed.");
 if(tc::fs::exists(backup)){if(tc::hash(tc::read(backup))!=TC_ENGINE_SHA)throw std::runtime_error("Existing engine backup does not match.");}
 else {if(actual!=TC_ENGINE_SHA)throw std::runtime_error("Original engine backup is missing.");if(!CopyFileW(engine.c_str(),backup.c_str(),TRUE))throw std::runtime_error("Cannot back up original engine.");}
 tc::no_links(root,root/L"mods");tc::fs::create_directories(root/L"mods");tc::atomic(engine,bytes);
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){bool silent=false;try{int argc{};LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);bool remove=false;tc::fs::path root;
 if(argc==3&&(std::wstring(argv[1])==L"--install"||std::wstring(argv[1])==L"--uninstall")){silent=true;remove=std::wstring(argv[1])==L"--uninstall";root=tc::fs::absolute(argv[2]).lexically_normal();}
 else {wchar_t file[32768]=L"Turing Complete.exe";OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.lpstrFilter=L"Turing Complete.exe\0Turing Complete.exe\0\0";o.lpstrFile=file;o.nMaxFile=32768;o.lpstrTitle=L"选择游戏目录中的 Turing Complete.exe";o.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
  if(!GetOpenFileNameW(&o)){LocalFree(argv);return 0;}root=tc::fs::path(file).parent_path();int choice=MessageBoxW(nullptr,L"安装 TC Mod Loader？\n\n是：安装 / 升级 / 修复\n否：卸载并恢复 Mod 修改的文件\n取消：退出\n\n安装后正常启动游戏，主菜单右上角会出现 Mods。",L"TC Mod Loader 0.3.0",MB_YESNOCANCEL|MB_ICONQUESTION);if(choice==IDCANCEL){LocalFree(argv);return 0;}remove=choice==IDNO;}
 LocalFree(argv);install(root,remove);if(!silent)MessageBoxW(nullptr,remove?L"已卸载并恢复原文件。Mod 包和备份记录已保留。":L"安装完成。正常启动游戏，点击主菜单右上角 Mods。\n\n将 .mod 文件放入游戏目录的 mods 文件夹。",L"TC Mod Loader",MB_OK|MB_ICONINFORMATION);return 0;
 }catch(const std::exception&e){if(!silent)MessageBoxW(nullptr,wide(e.what()).c_str(),L"TC Mod Loader — 操作未完成",MB_OK|MB_ICONERROR);OutputDebugStringW(wide(e.what()).c_str());return 1;}}

