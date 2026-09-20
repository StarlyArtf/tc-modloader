// Read-only cross-reference helper for the pinned game executable.
// Uses the COFF symbol table shipped in the binary plus raw byte scanning.
// No game code is executed.
//
//   node tools/xref.js callers <symbol-or-0xVA> [--all]
//   node tools/xref.js refs    <symbol-or-0xVA>
//   node tools/xref.js list    <substring>
//   node tools/xref.js dis     <symbol-or-0xVA>
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { execFileSync } = require('node:child_process');

const root = path.resolve(__dirname, '..');
const exe = path.resolve(process.argv[2] && /\.exe$/i.test(process.argv[2]) ? process.argv[2]
  : process.env.TC_EXE || path.join(root, '..', 'Turing Complete.exe'));
const objdump = path.join(process.env.TC_MINGW_BIN || 'C:/msys64/ucrt64/bin', 'objdump.exe');

const bytes = fs.readFileSync(exe);
const hash = crypto.createHash('sha256').update(bytes).digest('hex');
const PINNED = '8875da0e88cc878cb0fce30cfb63d20c5500cae341608af4d786649b88c8eb21';

const peOff = bytes.readUInt32LE(0x3c);
const optOff = peOff + 24;
const imageBase = Number(bytes.readBigUInt64LE(optOff + 24));
const numSections = bytes.readUInt16LE(peOff + 6);
const optSize = bytes.readUInt16LE(peOff + 20);
const sections = [];
for (let i = 0; i < numSections; i++) {
  const p = optOff + optSize + i * 40;
  const name = bytes.toString('ascii', p, p + 8).replace(/\0.*/, '');
  sections.push({
    name,
    va: imageBase + bytes.readUInt32LE(p + 12),
    vsize: bytes.readUInt32LE(p + 8),
    raw: bytes.readUInt32LE(p + 20),
    rawSize: bytes.readUInt32LE(p + 16),
    flags: bytes.readUInt32LE(p + 36),
  });
}
const text = sections.find((s) => s.name === '.text');
function vaToOff(va) {
  for (const s of sections) {
    const size = Math.max(s.vsize, s.rawSize);
    if (va >= s.va && va < s.va + size) return s.raw ? s.raw + (va - s.va) : -1;
  }
  return -1;
}

let symbols = null;
function loadSymbols() {
  if (symbols) return symbols;
  const out = execFileSync(objdump, ['-t', exe], { encoding: 'utf8', maxBuffer: 512 * 1024 * 1024, windowsHide: true });
  symbols = [];
  for (const line of out.split(/\r?\n/)) {
    const m = line.match(/^\s*\[\s*\d+\]\(sec\s+(\d+)\).*?\s0x([0-9a-f]{16})\s+(.+?)\s*$/);
    if (!m) continue;
    // objdump -t prints the raw COFF value (a section-relative offset), while
    // its disassembly text uses full virtual addresses. Normalise to VA.
    const index = Number(m[1]);
    const section = sections[index - 1];
    const value = BigInt('0x' + m[2]);
    const name = m[3];
    // Section symbols share names with sections; they are not real functions.
    if (sections.some((s) => s.name === name) || /^\.(text|data|rdata|bss|pdata|xdata)$/.test(name)) continue;
    symbols.push({ sec: index, va: section ? BigInt(section.va) + value : value, name });
  }
  symbols.sort((a, b) => (a.va < b.va ? -1 : a.va > b.va ? 1 : 0));
  return symbols;
}
function resolveSymbol(query) {
  const syms = loadSymbols();
  if (/^0x[0-9a-f]+$/i.test(query)) {
    const va = BigInt(query);
    return { va, name: '<' + query + '>' };
  }
  const exact = syms.filter((s) => s.name === query);
  if (exact.length) return exact[0];
  const partial = syms.filter((s) => s.name.includes(query));
  if (partial.length === 1) return partial[0];
  if (partial.length === 0) throw new Error(`No symbol matches ${query}`);
  const demangled = partial.map((s) => '  ' + '0x' + s.va.toString(16) + ' ' + s.name).join('\n');
  throw new Error(`${partial.length} symbols match ${query}:\n${demangled}`);
}
function symbolAt(va) {
  const syms = loadSymbols();
  let lo = 0;
  let hi = syms.length - 1;
  let best = null;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (syms[mid].va <= va) {
      best = syms[mid];
      lo = mid + 1;
    } else hi = mid - 1;
  }
  if (!best) return null;
  return { name: best.name, delta: Number(va - best.va) };
}
function fmt(va) {
  const s = symbolAt(va);
  const hex = '0x' + va.toString(16);
  return s ? `${hex} ${s.name}${s.delta ? '+0x' + s.delta.toString(16) : ''}` : hex;
}

// Scan .text for direct call/jmp rel32 (E8/E9) and common rip-relative memory
// operands (mov/lea/cmp/movzx/movsxd families) so global tables are traceable.
function scanText() {
  const calls = [];
  const refs = [];
  const start = text.raw;
  const end = text.raw + text.vsize;
  const loadOps = new Set([0x8b, 0x8d, 0x89, 0x8a, 0x88, 0x3b, 0x39, 0x3d, 0x0f]);
  const tlsPrefix = 0x64;
  for (let p = start; p < end; p++) {
    const op = bytes[p];
    if (op === 0xe8 || op === 0xe9) {
      const rel = bytes.readInt32LE(p + 1);
      const from = BigInt(text.va) + BigInt(p - text.raw);
      const target = from + 5n + BigInt(rel);
      calls.push({ from, target });
      p += 4;
      continue;
    }
    if ((op === 0x0f || loadOps.has(op)) && p < end - 7) {
      let q = p + 1;
      if (op === 0x0f) {
        const op2 = bytes[q];
        if (op2 !== 0xb6 && op2 !== 0xb7 && op2 !== 0xbe && op2 !== 0xbf && op2 !== 0x44 && op2 !== 0xb5) continue;
        q++;
      }
      const modrm = bytes[q];
      if ((modrm & 0xc7) !== 0x05) continue; // mod=00 rm=101 -> rip-relative disp32
      const rel = bytes.readInt32LE(q + 1);
      const from = BigInt(text.va) + BigInt(p - text.raw);
      // Without decoding prefixes backwards we may miss a REX byte that starts
      // the instruction, which shifts the rip base by one. Emit both candidates.
      const isPrefix = (b) => (b >= 0x40 && b <= 0x4f) || b === 0x66 || b === 0x67 || b === 0xf2 || b === 0xf3;
      const prefixed = p > text.raw && isPrefix(bytes[p - 1]);
      refs.push({ from, target: from + BigInt(q + 5 - p) + BigInt(rel), kind: 'rip' });
      if (prefixed) refs.push({ from, target: from + BigInt(q + 6 - p) + BigInt(rel), kind: 'rip+1' });
      continue;
    }
    if (op === tlsPrefix && bytes[p + 1] === 0x48 && bytes[p + 2] === 0x8b && (bytes[p + 3] & 0xc7) === 0x05) {
      // mov rXX, fs:[rip+disp32] style TLS access is handled as a normal rip ref above
      p += 3;
    }
  }
  return { calls, refs };
}

const [cmd, arg] = process.argv.slice(2);
if (hash !== PINNED) {
  console.error(`WARNING: executable hash ${hash} differs from pinned ${PINNED}`);
}
if (cmd === 'list') {
  for (const s of loadSymbols()) if (s.name.includes(arg)) console.log('0x' + s.va.toString(16) + ' ' + s.name);
} else if (cmd === 'dis') {
  const s = resolveSymbol(arg);
  const out = execFileSync(objdump, ['-d', '-Mintel', `--start-address=${s.va}`, '--stop-address=0x' + (s.va + 0x4000n).toString(16), exe],
    { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024, windowsHide: true });
  console.log(out);
} else if (cmd === 'callers' || cmd === 'refs') {
  const target = resolveSymbol(arg);
  const { calls, refs } = scanText();
  if (process.env.TC_XREF_DEBUG) console.error(`scanned: ${calls.length} calls, ${refs.length} rip refs`);
  const pool = cmd === 'callers' ? calls : refs;
  const hits = pool.filter((x) => x.target === target.va);
  const seen = new Set();
  for (const h of hits) {
    const key = String(h.from);
    if (seen.has(key)) continue;
    seen.add(key);
    console.log(`${fmt(h.from)}   ->  ${fmt(h.target)}`);
  }
  console.log(`${hits.length} ${cmd === 'callers' ? 'call' : 'reference'} site(s) to ${target.name} (0x${target.va.toString(16)})`);
} else if (cmd === 'loc') {
  const va = BigInt(arg);
  const { calls, refs } = scanText();
  const near = [...calls, ...refs].filter((x) => x.from >= va && x.from < va + 0x40n);
  for (const n of near) console.log(`${fmt(n.from)} (${n.kind || 'call'}) -> ${fmt(n.target)}`);
} else {
  console.log('usage: node tools/xref.js callers|refs|list|dis|loc <symbol|0xVA>');
}
