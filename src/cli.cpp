#include "core.hpp"
#include "saves.hpp"
#include <iostream>
int wmain(int argc,wchar_t** argv){try{
 /* Two questions that do not need a game folder: what this build is, and what
    it promises.  Both are what a package's manifest is checked against, so the
    answers come from the same table the loader uses. */
 if(argc==2&&(std::wstring(argv[1])==L"--version"||std::wstring(argv[1])==L"-v")){std::cout<<"tcmod-cli "<<TC_MODLOADER_VERSION_STRING<<"\n";return 0;}
 if(argc==2&&(std::wstring(argv[1])==L"--capabilities"||std::wstring(argv[1])==L"--caps")){std::cout<<tc::capability_names(tc::loader_capabilities())<<"\n";return 0;}
 if(argc<3)throw std::runtime_error("Usage: tcmod-cli GAME-DIR list|apply|enable|disable|disable-all [mod-id ...]\n  apply        replace the enabled set with exactly these IDs\n  enable       add these IDs to the current enabled set\n  disable      remove these IDs from the current enabled set\n  disable-all  enable nothing\n  tcmod-cli --version | --capabilities");
 tc::Core c(argv[1]);std::wstring op=argv[2];
 if(op==L"list"){c.scan();for(auto&m:c.mods)std::cout<<m.id<<" | "<<m.name<<" | "<<(c.enabled(m.id)?"enabled":"disabled")<<" | "<<m.error<<"\n";}
 else if(op==L"apply"||op==L"disable-all"){std::set<std::string>s;for(int i=3;op==L"apply"&&i<argc;i++)s.insert(tc::fs::path(argv[i]).u8string());c.apply(s);std::cout<<c.notice<<"\n";}
 else if(op==L"enable"||op==L"disable"){auto s=c.enabled_set();for(int i=3;i<argc;i++){std::string id=tc::fs::path(argv[i]).u8string();if(op==L"enable")s.insert(id);else s.erase(id);}c.apply(s);std::cout<<c.notice<<"\n";std::cout<<"enabled now: ";for(auto&id:c.enabled_set())std::cout<<id<<" ";std::cout<<"\n";}
 else if(op==L"import-saves"){tc::SaveProfiles saves(c.root);auto id=saves.import_original();std::cout<<"Imported, restart required: "<<saves.path(id).u8string()<<"\n";}
 else if(op==L"save-path"){tc::SaveProfiles saves(c.root);std::cout<<saves.path(saves.next()).u8string()<<"\n";}
 else throw std::runtime_error("Unknown command");return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
