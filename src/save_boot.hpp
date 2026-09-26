#pragma once
#include <windows.h>
#include <stdint.h>
#include <cstring>
#include <cwchar>
#include <initializer_list>
namespace tc_save_boot {
// Nim string literal: length + pointer to capacity/flags and UTF-8 bytes.
// This one literal is used only by get_game_save_path in the pinned build.
struct Payload {uint64_t capacity; char bytes[192];};
struct String {uint64_t length; Payload* payload;};
static Payload redirected{};
static wchar_t profile[80]=L"default";
/* What happened to the save redirect.  This is deliberately *not* fatal: it used
   to be, and that made a game update a "the game will not start at all" event -
   the loader is the engine DLL, so failing DllMain means the game has no engine
   and the player has to uninstall before they can even see the main menu.  Now a
   build whose literal does not validate leaves the player's own save path alone,
   reports the reason, and the loader runs the session in a degraded mode. */
struct Result {
 bool applied=false;
 const char* reason="not attempted";
};
inline bool valid(const wchar_t* s){size_t n=wcslen(s);if(!n||n>64)return false;for(size_t i=0;i<n;++i)if(!((s[i]>=L'a'&&s[i]<=L'z')||(s[i]>=L'0'&&s[i]<=L'9')||s[i]==L'-'))return false;return true;}
inline bool directory(const wchar_t* p){DWORD a=GetFileAttributesW(p);if(a==INVALID_FILE_ATTRIBUTES){if(!CreateDirectoryW(p,nullptr))return false;a=GetFileAttributesW(p);}return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)&&!(a&FILE_ATTRIBUTE_REPARSE_POINT);}
/* The decision itself, without touching the process: given what the loader found
   in its own image, is the redirect safe to apply? */
inline bool patchable(size_t imageSize,uint64_t literalLength,const void* payload,
                      const void* expectedPayload,const void* bytes,size_t byteCount,
                      const char* expected,size_t expectedLength){
 if(imageSize<0x4a84b0)return false;
 if(literalLength!=15)return false;
 if(payload!=expectedPayload)return false;
 if(!bytes)return false;
 if(byteCount<expectedLength)return false;
 return memcmp(bytes,expected,expectedLength)==0;
}
inline Result attach(){
 Result result;
  wchar_t exe[32768],ini[32768],path[32768];DWORD n=GetModuleFileNameW(nullptr,exe,32768);if(!n||n>=32768){result.reason="the module file name is unavailable";return result;}
  wchar_t* slash=wcsrchr(exe,L'\\');if(!slash||wcscmp(slash+1,L"Turing Complete.exe")){result.reason="this process is not Turing Complete.exe";return result;}*slash=0;
  if(wcslen(exe)+80>=32768){result.reason="the game path is too long";return result;}wcscpy(ini,exe);wcscat(ini,L"\\tc-modloader-data\\saves.ini");
  GetPrivateProfileStringW(L"saves",L"profile",L"default",profile,80,ini);if(!valid(profile)){result.reason="saves.ini names an invalid profile";return result;}
  n=GetEnvironmentVariableW(L"USERPROFILE",path,32768);if(!n||n>32000||!directory(path)){result.reason="USERPROFILE is unavailable";return result;}
  for(auto part:{L"\\AppData",L"\\Roaming",L"\\Turing Complete Mods",L"\\profiles"}){wcscat(path,part);if(!directory(path)){result.reason="the isolated profile directory could not be created";return result;}}
  wcscat(path,L"\\");wcscat(path,profile);if(!directory(path)){result.reason="this profile's directory could not be created";return result;}
  auto base=(unsigned char*)GetModuleHandleW(nullptr);auto dos=(IMAGE_DOS_HEADER*)base;if(dos->e_magic!=IMAGE_DOS_SIGNATURE){result.reason="the game image has no DOS header";return result;}
  auto nt=(IMAGE_NT_HEADERS64*)(base+dos->e_lfanew);if(nt->Signature!=IMAGE_NT_SIGNATURE){result.reason="the game image has no PE header";return result;}
  auto literal=(String*)(base+0x4a8480);
  if(!patchable(nt->OptionalHeader.SizeOfImage,literal->length,literal->payload,base+0x4a8490,
                literal->payload?literal->payload->bytes:nullptr,16,"Turing Complete",16)){
   /* The one line a player will see in loader.log after a game update: the game
      still starts, the vanilla save stays where the game put it, and no native
      plugin runs against it. */
   result.reason="the game build changed: the save path literal is not where this loader expects it";
   return result;
  }
  strcpy(redirected.bytes,"Turing Complete Mods/profiles/");size_t k=strlen(redirected.bytes);for(size_t i=0;profile[i];++i)redirected.bytes[k++]=(char)profile[i];redirected.bytes[k]=0;redirected.capacity=(1ULL<<62)|k;
  DWORD old;if(!VirtualProtect(literal,sizeof(String),PAGE_READWRITE,&old)){result.reason="the save path literal is not writable";return result;}
  literal->length=k;literal->payload=&redirected;DWORD ignored;VirtualProtect(literal,sizeof(String),old,&ignored);
  result.applied=true;result.reason="applied";
  return result;
}
}
