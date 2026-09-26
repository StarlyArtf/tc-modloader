#include "core.hpp"
#include "saves.hpp"
#include "save_deps.hpp"
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
 /* The same operations the 存档 page offers, for scripts and for the cases that
    have to exercise them without synthetic mouse input.  Everything that changes
    the selection only takes effect on the next start, like the page says. */
 else if(op==L"saves"){
  tc::SaveProfiles saves(c.root);
  const std::wstring what=argc>3?argv[3]:L"list";
  auto argc4=[&](const wchar_t* fallback){return argc>4?std::wstring(argv[4]):std::wstring(fallback);};
  auto id=[](const std::wstring& value){return std::string(value.begin(),value.end());};
  if(what==L"list"){
   const auto current=tc::fs::path(saves.next()).filename().wstring();
   for(auto& entry:saves.list())
    std::cout<<id(entry.id)<<" | "<<(entry.current?"running":entry.selected?"next":"-")
             <<" | "<<entry.bytes<<" bytes | "<<entry.files<<" files | "<<entry.modified<<"\n";
   std::cout<<"selected: "<<id(current)<<"\n";
  }else if(what==L"create"){const auto fresh=saves.create();saves.select(fresh);std::cout<<"Created, restart required: "<<saves.path(fresh).u8string()<<"\n";}
  else if(what==L"duplicate"){const auto copy=saves.duplicate(argc4(L"default"));std::cout<<"Duplicated: "<<saves.path(copy).u8string()<<"\n";}
  else if(what==L"rename"){const auto renamed=saves.rename(argc4(L"default"),argc>5?std::wstring(argv[5]):L"");std::cout<<"Renamed: "<<saves.path(renamed).u8string()<<"\n";}
  else if(what==L"remove"){saves.remove(argc4(L"default"));std::cout<<"Moved to trash\n";}
  else if(what==L"switch"){saves.select(argc4(L"default"));std::cout<<"Selected, restart required: "<<saves.path(saves.next()).u8string()<<"\n";}
  else if(what==L"report"){
   /* Which Mods a profile needs: the circuits only carry custom_ids, so the
      answer is the registry cache built while Mods registered (src/save_deps.hpp)
      joined with what is installed and enabled right now. */
   const std::wstring profile=argc>4?std::wstring(argv[4]):saves.next();
   tc::TypeRegistry registry(c.dir/L"registry.json");
   c.scan();
   std::map<std::string,std::string> installed;
   for(auto& mod:c.mods)installed[mod.id]=mod.version;
   const auto enabled=c.enabled_set();
   const auto report=tc::buildReport(tc::scanProfile(saves.path(profile),registry),
                                     registry,installed,enabled);
   std::cout<<"profile "<<id(profile)<<"\n";
   std::cout<<"scan circuits="<<report.scan.circuits<<" skipped="<<report.scan.skipped
            <<" references="<<report.scan.references<<" types="<<report.dependencies.size()<<"\n";
   for(auto& item:report.dependencies){
    std::cout<<"type 0x"<<std::hex<<item.id<<std::dec<<" "<<(item.tag.empty()?"-":item.tag)
             <<" | "<<(item.type.empty()?"unknown":item.type)
             <<" | mod="<<(item.mod.empty()?"-":item.mod)
             <<" | recorded="<<(item.recordedVersion.empty()?"-":item.recordedVersion)
             <<" | installed="<<(item.installed?item.installedVersion:"-")
             <<" | enabled="<<(item.enabled?"1":"0")
             <<" | refs="<<item.references<<"\n";
   }
   std::cout<<"summary missing="<<report.missing<<" disabled="<<report.disabled
            <<" unknown="<<report.unknown<<" version-mismatch="<<report.versionMismatch<<"\n";
  }
  else throw std::runtime_error("Unknown saves command: list|create|duplicate|rename|remove|switch|report");
 }
 else throw std::runtime_error("Unknown command");return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
