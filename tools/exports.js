const fs=require('fs'),path=require('path'),crypto=require('crypto');
const root=path.resolve(__dirname,'../..');
const bin=fs.readFileSync(path.join(root,'game_engine.dll'));
const pe=bin.readUInt32LE(0x3c), opt=pe+24, sect=opt+bin.readUInt16LE(pe+20);
const sections=[];
for(let i=0;i<bin.readUInt16LE(pe+6);i++){let p=sect+i*40;sections.push({rva:bin.readUInt32LE(p+12),size:Math.max(bin.readUInt32LE(p+8),bin.readUInt32LE(p+16)),off:bin.readUInt32LE(p+20)});}
function off(r){const s=sections.find(s=>r>=s.rva&&r<s.rva+s.size);if(!s)throw Error('Bad RVA');return r-s.rva+s.off;}
function str(r){let p=off(r);return bin.toString('ascii',p,bin.indexOf(0,p));}
const exp=off(bin.readUInt32LE(opt+112)),base=bin.readUInt32LE(exp+16),n=bin.readUInt32LE(exp+24),names=off(bin.readUInt32LE(exp+32)),ords=off(bin.readUInt32LE(exp+36));
let def='LIBRARY game_engine\nEXPORTS\n';
for(let i=0;i<n;i++){let name=str(bin.readUInt32LE(names+i*4)),ordinal=base+bin.readUInt16LE(ords+i*2);def+=' '+name+(['igEnd','igInvisibleButton'].includes(name)?'':'=tc_game_engine.'+name)+' @'+ordinal+'\n';}
fs.writeFileSync(path.join(__dirname,'../src/proxy.def'),def);
const exe=fs.readFileSync(path.join(root,'Turing Complete.exe'));
let h='#pragma once\n';
for(const [k,b] of [['EXE',exe],['ENGINE',bin]])h+=`#define TC_${k}_SHA "${crypto.createHash('sha256').update(b).digest('hex')}"\n`;
// Verified from this executable's COFF symbols and call instructions.
h+='#define TC_MENU_END_RVA 0x456c33ULL\n#define TC_HOME_START_RVA 0x449df0ULL\n#define TC_HOME_END_RVA 0x44b610ULL\n';
fs.writeFileSync(path.join(__dirname,'../src/compat.hpp'),h);
console.log(`Generated ${n} export forwarders and exact build fingerprint.`);
