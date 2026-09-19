const fs=require('fs'),path=require('path'),crypto=require('crypto');
const root=path.resolve(__dirname,'../..');
// The original engine (tc_game_engine.dll), never game_engine.dll: once the
// loader is installed the latter IS the loader, so hashing it would produce a
// fingerprint the loader's own compatibility check can never match, and
// parsing its export table would turn the loader's self-implemented exports
// (igEnd, igInvisibleButton, igIsAnyItemActive, the tc_logic_* bridge) into
// forwarders.  tc_game_engine.dll is what loader.cpp validates at runtime.
const bin=fs.readFileSync(path.join(root,'tc_game_engine.dll'));
const pe=bin.readUInt32LE(0x3c), opt=pe+24, sect=opt+bin.readUInt16LE(pe+20);
const sections=[];
for(let i=0;i<bin.readUInt16LE(pe+6);i++){let p=sect+i*40;sections.push({rva:bin.readUInt32LE(p+12),size:Math.max(bin.readUInt32LE(p+8),bin.readUInt32LE(p+16)),off:bin.readUInt32LE(p+20)});}
function off(r){const s=sections.find(s=>r>=s.rva&&r<s.rva+s.size);if(!s)throw Error('Bad RVA');return r-s.rva+s.off;}
function str(r){let p=off(r);return bin.toString('ascii',p,bin.indexOf(0,p));}
const exp=off(bin.readUInt32LE(opt+112)),base=bin.readUInt32LE(exp+16),n=bin.readUInt32LE(exp+24),names=off(bin.readUInt32LE(exp+32)),ords=off(bin.readUInt32LE(exp+36));
// igEnd, igInvisibleButton and igIsAnyItemActive are implemented by the loader
// itself (src/loader.cpp), which is what gives it the caller's return address;
// every other export is a plain forwarder to the original engine.
let def='LIBRARY game_engine\nEXPORTS\n';
for(let i=0;i<n;i++){let name=str(bin.readUInt32LE(names+i*4)),ordinal=base+bin.readUInt16LE(ords+i*2);def+=' '+name+(['igEnd','igInvisibleButton','igIsAnyItemActive'].includes(name)?'':'=tc_game_engine.'+name)+' @'+ordinal+'\n';}
fs.writeFileSync(path.join(__dirname,'../src/proxy.def'),def);
const exe=fs.readFileSync(path.join(root,'Turing Complete.exe'));
let h='#pragma once\n';
for(const [k,b] of [['EXE',exe],['ENGINE',bin]])h+=`#define TC_${k}_SHA "${crypto.createHash('sha256').update(b).digest('hex')}"\n`;
// Verified from this executable's COFF symbols and call instructions.
h+='#define TC_MENU_END_RVA 0x456c33ULL\n#define TC_HOME_START_RVA 0x449df0ULL\n#define TC_HOME_END_RVA 0x44b610ULL\n';
// build_board_ui calls igIsAnyItemActive at 0x14046b58e (return address RVA
// 0x46b593) and igIsWindowHovered at 0x14046b5dd; the pair it stores decides
// whether handle_io_on_board() processes the mouse for this frame at all.
h+='#define TC_BOARD_INPUT_SAMPLE_RVA 0x46b593ULL\n#define TC_BOARD_WINDOW_HOVER_RVA 0x46b5e2ULL\n';
// Loader builds setup.cpp recognises as "a loader is already installed" so
// re-running the installer over an older install stays possible.  These are
// historical constants, not derivable from the engine.
h+='#define TC_LOADER_020_SHA "1612880bc7c3c198604b5b967900a32720700ebebd135b72348a3e3ac409877b"\n';
h+='#define TC_PREVIOUS_LOADER_SHA "f41ba881ebab46b8cced4eacd76193e1af97004667082b4cff5378b1d81f60c5"\n';
fs.writeFileSync(path.join(__dirname,'../src/compat.hpp'),h);
console.log(`Generated ${n} export forwarders and exact build fingerprint.`);
