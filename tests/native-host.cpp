#include "../src/native.hpp"
#include <iostream>
static volatile int64_t observed=-999;static volatile uint8_t observedCommand=255;static int64_t fakeSettings[32]{};
extern "C" void* simulationSettings asm("simulation_settings__modelZsimulator95types_u83");
void* simulationSettings=fakeSettings;
extern "C" __attribute__((noinline)) void fakeSim(void*,uint8_t,int64_t) asm("sim_do__modelZsimulationZcompile95thread_u3036");
void fakeSim(void*,uint8_t c,int64_t t){observed=t;observedCommand=c;}
extern "C" __attribute__((noinline)) int64_t fakeCycle() asm("sim_get_cycle__modelZsimulationZcompile95thread_u3041");
int64_t fakeCycle(){return 100;}
int wmain(int argc,wchar_t** argv){try{if(argc<2)return 1;wchar_t own[32768];GetModuleFileNameW(nullptr,own,32768);tc::Core core(tc::fs::path(own).parent_path());core.scan();std::set<std::string> selected;for(auto& m:core.mods)if(m.error.empty())selected.insert(m.id);core.apply(selected);
 bool tampered=argc==3&&std::wstring(argv[2])==L"--tamper";
 bool disabled=argc==3&&std::wstring(argv[2])==L"--disabled";
 if(disabled)core.apply({});
 if(tampered) {auto p=core.root/L"mods"/L"guard.mod";auto b=tc::read(p);b+="changed";tc::atomic(p,b);core.scan();}
 HMODULE engine=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);if(!engine)throw std::runtime_error("test engine unavailable");
 tc::NativeRuntime runtime(core,engine,[](auto&s){std::cout<<s<<"\n";});runtime.boot();using Fn=void(*)(void*,uint8_t,int64_t);Fn volatile call=fakeSim;
 call(nullptr,0,100000);
 if(tampered||disabled){if(observed!=100000)throw std::runtime_error("Inactive package executed");if(disabled&&!runtime.statuses.empty())throw std::runtime_error("Disabled plugin attempted initialization");std::cout<<(disabled?"PASS disabled package is not executed\n":"PASS changed package is not executed\n");return 0;}
 if(observed!=200||observedCommand!=0)throw std::runtime_error("Native detour did not limit target to current+100");std::cout<<"PASS native DLL hooks executable logic: 100000 -> 200\n";
 call(nullptr,0,150);if(observed!=150)throw std::runtime_error("Small run incorrectly modified");std::cout<<"PASS shorter run preserved\n";
 call(nullptr,1,9999);if(observed!=9999||observedCommand!=1)throw std::runtime_error("Stop modified");std::cout<<"PASS stop command preserved\n";
 call(nullptr,2,-1);if(observed!=-1||observedCommand!=2)throw std::runtime_error("Reset modified");std::cout<<"PASS reset command preserved\n";
 for(auto& [id,status]:runtime.statuses)std::cout<<id<<": "<<status<<"\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
