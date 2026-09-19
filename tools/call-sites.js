// Development helper: list the callers of one address in the game executable,
// each labelled with the symbol that contains it, so a call site can be traced
// back to the game's own module and function.
// Usage: node tools/call-sites.js <exe> <target-rva-hex>
const fs = require('fs');
const { execFileSync } = require('child_process');

const exe = process.argv[2] || 'D:/p/Turing Complete.exe';
const target = Number(process.argv[3]);
const bin = fs.readFileSync(exe);

const pe = bin.readUInt32LE(0x3c);
const opt = pe + 24;
const sectionCount = bin.readUInt16LE(pe + 6);
const sectionAt = opt + bin.readUInt16LE(pe + 20);
const sections = [];
for (let i = 0; i < sectionCount; i++) {
    const at = sectionAt + i * 40;
    sections.push({ va: bin.readUInt32LE(at + 12), size: bin.readUInt32LE(at + 16),
                    raw: bin.readUInt32LE(at + 20) });
}
const text = sections.find((s) => s.raw === 0x600) || sections.find((s) => s.raw > 0);

// Symbols: COFF name -> rva (section VA + value).
const dump = execFileSync('C:/msys64/ucrt64/bin/objdump.exe', ['-t', exe],
                          { maxBuffer: 1 << 28 }).toString();
const symbols = [];
for (const line of dump.split(/\r?\n/)) {
    const match = line.match(/\(sec\s+(\d+)\).*?\(scl\s+\d+\).*?0x([0-9a-f]+)\s+(\S+)\s*$/);
    if (!match) continue;
    const section = Number(match[1]);
    const value = parseInt(match[2], 16);
    const name = match[3];
    const info = sections[section - 1];
    if (!info) continue;
    symbols.push({ rva: info.va + value, name });
}
symbols.sort((a, b) => a.rva - b.rva);
const containing = (rva) => {
    let found = '?';
    for (const symbol of symbols) { if (symbol.rva <= rva) found = symbol.name; else break; }
    return found;
};

const start = text.raw;
const end = text.raw + text.size;
const rows = [];
for (let at = start; at + 5 <= end; at++) {
    if (bin[at] !== 0xe8 && bin[at] !== 0xe9) continue;
    const rel = bin.readInt32LE(at + 1);
    const site = text.va + (at - start);
    if (site + 5 + rel !== target) continue;
    rows.push({ site, op: bin[at] === 0xe8 ? 'call' : 'jmp', owner: containing(site) });
}
console.log(`callers of 0x${target.toString(16)}: ${rows.length}`);
for (const row of rows) {
    console.log(`0x${row.site.toString(16)} ${row.op} -> ${row.owner}`);
}
