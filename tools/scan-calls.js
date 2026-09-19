// Development helper: find call sites of imported ImGui entry points inside a
// range of the game executable, so a probe can copy what the game itself does.
// Usage: node tools/scan-calls.js <dll-with-imports> <exe-to-scan> <name-regex> <rva-low> <rva-high>
const fs = require('fs');
const imported = process.argv[2] || 'D:/p/game_engine.dll';
const target = process.argv[3] || 'D:/p/Turing Complete.exe';
const wanted = new RegExp(process.argv[4] || 'igText|igPushFont|igPopFont|FontScale|AddText', 'i');
const low = Number(process.argv[5] || 0);
const high = Number(process.argv[6] || 0xffffffff);

function sections(bin) {
    const pe = bin.readUInt32LE(0x3c);
    const count = bin.readUInt16LE(pe + 6);
    const at = pe + 24 + bin.readUInt16LE(pe + 20);
    const rows = [];
    for (let i = 0; i < count; i++) {
        const p = at + i * 40;
        rows.push({ name: bin.toString('ascii', p, p + 8).replace(/\0+$/, ''),
                    vsize: bin.readUInt32LE(p + 8), va: bin.readUInt32LE(p + 12),
                    size: bin.readUInt32LE(p + 16), raw: bin.readUInt32LE(p + 20) });
    }
    return rows;
}
function toOffset(secs, rva) {
    const s = secs.find((s) => rva >= s.va && rva < s.va + Math.max(s.vsize, s.size));
    return s ? rva - s.va + s.raw : -1;
}

// Imports: name -> IAT address (RVA), i.e. the slot a call instruction targets.
const bin = fs.readFileSync(target);
const secs = sections(bin);
const pe = bin.readUInt32LE(0x3c);
const opt = pe + 24;
const importRva = bin.readUInt32LE(opt + 120);
const slots = new Map();
for (let descriptor = toOffset(secs, importRva); descriptor > 0; descriptor += 20) {
    const nameRva = bin.readUInt32LE(descriptor + 12);
    const firstThunk = bin.readUInt32LE(descriptor + 16);
    if (!nameRva) break;
    const dll = bin.toString('ascii', toOffset(secs, nameRva), bin.indexOf(0, toOffset(secs, nameRva)));
    if (!/game_engine/i.test(dll)) continue;
    for (let i = 0; ; i++) {
        const thunk = bin.readUInt32LE(toOffset(secs, bin.readUInt32LE(descriptor)) + i * 8);
        if (!thunk) break;
        const hint = toOffset(secs, bin.readUInt32LE(toOffset(secs, bin.readUInt32LE(descriptor)) + i * 8) + 2);
        const name = bin.toString('ascii', hint, bin.indexOf(0, hint));
        if (wanted.test(name)) slots.set(firstThunk + i * 8, name);
    }
}
console.log(`interesting import slots: ${slots.size}`);
for (const [rva, name] of slots) console.log(`  slot 0x${rva.toString(16)} ${name}`);

// This executable calls its imports through linker stubs: a direct `E8 rel32`
// call to a small `FF 25 rel32` (jmp qword ptr [rip+rel32]) thunk that in turn
// points at the IAT slot.  Both steps are resolved here, and direct FF 15 calls
// are accepted too in case another build inlines them.
const text = secs.find((s) => s.name === '.text');
const start = text.raw;
const end = text.raw + text.size;
const stubs = new Map();
for (let at = start; at + 6 <= end; at++) {
    if (bin[at] !== 0xff || bin[at + 1] !== 0x25) continue;
    const rel = bin.readInt32LE(at + 2);
    const target = text.va + (at - start) + 6 + rel;
    const name = slots.get(target);
    if (name) stubs.set(text.va + (at - start), name);
}
console.log(`import stubs: ${stubs.size}`);
const rows = [];
for (let at = start; at + 7 <= end; at++) {
    const site = text.va + (at - start);
    let name = null;
    if (process.env.TC_CALLTO && bin[at] === 0xe8) {
        const rel = bin.readInt32LE(at + 1);
        if (site + 5 + rel === Number(process.env.TC_CALLTO)) {
            rows.push(`rva=0x${site.toString(16)} -> call 0x${Number(process.env.TC_CALLTO).toString(16)}`);
        }
        continue;
    }
    if (bin[at] === 0xe8 || bin[at] === 0xe9) {
        const rel = bin.readInt32LE(at + 1);
        name = stubs.get(site + 5 + rel);
    } else if (bin[at] === 0xff && bin[at + 1] === 0x15) {
        const rel = bin.readInt32LE(at + 2);
        name = slots.get(site + 6 + rel);
    }
    if (!name) continue;
    if (site < low || site > high) continue;
    rows.push(`rva=0x${site.toString(16)} -> ${name}`);
}
console.log(`calls in [0x${low.toString(16)},0x${high.toString(16)}]: ${rows.length}`);
console.log(rows.join('\n'));
