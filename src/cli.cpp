#include "core.hpp"
#include "saves.hpp"
#include <iostream>
int wmain(int argc,wchar_t** argv){try{if(argc<3)throw std::runtime_error("Usage: tcmod-cli GAME-DIR list|apply|disable-all [mod-id ...]");tc::Core c(argv[1]);std::wstring op=argv[2];
 if(op==L"list"){c.scan();for(auto&m:c.mods)std::cout<<m.id<<" | "<<m.name<<" | "<<(c.enabled(m.id)?"enabled":"disabled")<<" | "<<m.error<<"\n";}
 else if(op==L"apply"||op==L"disable-all"){std::set<std::string>s;for(int i=3;op==L"apply"&&i<argc;i++)s.insert(tc::fs::path(argv[i]).u8string());c.apply(s);std::cout<<c.notice<<"\n";}
 else if(op==L"import-saves"){tc::SaveProfiles saves(c.root);auto id=saves.import_original();std::cout<<"Imported, restart required: "<<saves.path(id).u8string()<<"\n";}
 else if(op==L"save-path"){tc::SaveProfiles saves(c.root);std::cout<<saves.path(saves.next()).u8string()<<"\n";}
 else throw std::runtime_error("Unknown command");return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
