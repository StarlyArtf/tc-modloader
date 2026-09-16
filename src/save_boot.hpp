#pragma once
#include <windows.h>
#include <stdint.h>
#include <cstring>
#include <cwchar>
namespace tc_save_boot {
// Nim string literal: length + pointer to capacity/flags and UTF-8 bytes.
// This one literal is used only by get_game_save_path in the pinned build.
struct Payload {uint64_t capacity; char bytes[192];};
struct String {uint64_t length; Payload* payload;};
static Payload redirected{};
static wchar_t profile[80]=L"default";
inline bool valid(const wchar_t* s){size_t n=wcslen(s);if(!n||n>64)return false;for(size_t i=0;i<n;++i)if(!((s[i]>=L'a'&&s[i]<=L'z')||(s[i]>=L'0'&&s[i]<=L'9')||s[i]==L'-'))return false;return true;}
inline bool directory(const wchar_t* p){DWORD a=GetFileAttributesW(p);if(a==INVALID_FILE_ATTRIBUTES){if(!CreateDirectoryW(p,nullptr))return false;a=GetFileAttributesW(p);}return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)&&!(a&FILE_ATTRIBUTE_REPARSE_POINT);}
inline bool attach(){
 wchar_t exe[32768],ini[32768],path[32768];DWORD n=GetModuleFileNameW(nullptr,exe,32768);if(!n||n>=32768)return false;
 wchar_t* slash=wcsrchr(exe,L'\\');if(!slash||wcscmp(slash+1,L"Turing Complete.exe"))return false;*slash=0;
 if(wcslen(exe)+80>=32768)return false;wcscpy(ini,exe);wcscat(ini,L"\\tc-modloader-data\\saves.ini");
 GetPrivateProfileStringW(L"saves",L"profile",L"default",profile,80,ini);if(!valid(profile))return false;
 n=GetEnvironmentVariableW(L"USERPROFILE",path,32768);if(!n||n>32000||!directory(path))return false;
 for(auto part:{L"\\AppData",L"\\Roaming",L"\\Turing Complete Mods",L"\\profiles"}){wcscat(path,part);if(!directory(path))return false;}
 wcscat(path,L"\\");wcscat(path,profile);if(!directory(path))return false;
 auto base=(unsigned char*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return false;
 auto nt=(IMAGE_NT_HEADERS64*)(base+dos->e_lfanew);if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.SizeOfImage<0x4a84b0)return false;
 auto literal=(String*)(base+0x4a8480);if(literal->length!=15||(void*)literal->payload!=base+0x4a8490||memcmp(literal->payload->bytes,"Turing Complete",16))return false;
 strcpy(redirected.bytes,"Turing Complete Mods/profiles/");size_t k=strlen(redirected.bytes);for(size_t i=0;profile[i];++i)redirected.bytes[k++]=(char)profile[i];redirected.bytes[k]=0;redirected.capacity=(1ULL<<62)|k;
 DWORD old;if(!VirtualProtect(literal,sizeof(String),PAGE_READWRITE,&old))return false;literal->length=k;literal->payload=&redirected;DWORD ignored;VirtualProtect(literal,sizeof(String),old,&ignored);return true;
}
}
