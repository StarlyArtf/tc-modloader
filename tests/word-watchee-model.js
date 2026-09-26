// Offline model check for the "WordWatchee 64" package.  Nothing here runs the
// game: it reads the pinned executable and the two shader files the package
// ships, and checks that
//
//   * the clamp the plugin rewrites is where the plugin says it is and really
//     means min(value_size, 32),
//   * a value wider than 32 bits becomes two lines of decimal integers - the
//     low 32 bits on top, the significant high bits below - and that this
//     choice does not follow the game's number format,
//   * a label of 32 bits or less still takes the game's original one-line path.
//
//   node tests/word-watchee-model.js [path-to-exe]
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const repo = path.resolve(__dirname, '..');
const game = path.resolve(repo, '..');
const exe = process.argv[2] || process.env.TC_EXE || path.join(game, 'Turing Complete.exe');
const pluginPath = path.join(repo, 'examples', 'word-watchee-64', 'plugin.cpp');
const manifestPath = path.join(repo, 'examples', 'word-watchee-64', 'mod.json');
const shaderDir = path.join(repo, 'examples', 'word-watchee-64', 'files', 'asset', 'shader');
const objdump = path.join(process.env.TC_MINGW_BIN || 'C:/msys64/ucrt64/bin', 'objdump.exe');

const SYMBOL = 'set_value_size__presenterZrendererZmulti95meshZword95watchee95mesh_u1008';
const CLAMP_OFFSET = 0x40;
const CLAMP_IMMEDIATE = 0x41;
/* `live` starts at CLAMP_OFFSET, so the ceiling byte sits this far into it. */
const CEILING = CLAMP_IMMEDIATE - CLAMP_OFFSET;

let failures = 0;
function check(ok, message) {
  if (ok) return;
  failures += 1;
  console.error(`FAIL ${message}`);
}
function ok(message) {
  console.log(`PASS ${message}`);
}

// ---------------------------------------------------------------- executable
const exeBytes = fs.readFileSync(exe);
const peOff = exeBytes.readUInt32LE(0x3c);
const optOff = peOff + 24;
const imageBase = Number(exeBytes.readBigUInt64LE(optOff + 24));
const numSections = exeBytes.readUInt16LE(peOff + 6);
const optSize = exeBytes.readUInt16LE(peOff + 20);
const sections = [];
for (let i = 0; i < numSections; i++) {
  const p = optOff + optSize + i * 40;
  sections.push({
    name: exeBytes.toString('ascii', p, p + 8).replace(/\0.*/, ''),
    va: imageBase + exeBytes.readUInt32LE(p + 12),
    vsize: exeBytes.readUInt32LE(p + 8),
    raw: exeBytes.readUInt32LE(p + 20),
    rawSize: exeBytes.readUInt32LE(p + 16),
  });
}
function symbolVa(name) {
  const out = execFileSync(objdump, ['-t', exe], { encoding: 'utf8', maxBuffer: 512 * 1024 * 1024, windowsHide: true });
  for (const line of out.split(/\r?\n/)) {
    const m = line.match(/^\s*\[\s*\d+\]\(sec\s+(\d+)\).*?\s0x([0-9a-f]{16})\s+(.+?)\s*$/);
    if (!m || m[3] !== name) continue;
    return sections[Number(m[1]) - 1].va + Number(BigInt('0x' + m[2]));
  }
  return null;
}
function vaToOffset(va) {
  for (const s of sections) {
    const size = Math.max(s.vsize, s.rawSize);
    if (va >= s.va && va < s.va + size) return s.raw ? s.raw + (va - s.va) : -1;
  }
  return -1;
}

const symbolAddress = symbolVa(SYMBOL);
check(symbolAddress !== null, `the pinned build exports ${SYMBOL}`);
if (symbolAddress === null) {
  process.exit(1);
}
const windowOffset = vaToOffset(symbolAddress + CLAMP_OFFSET);
const CLAMP_BYTES = 24;
const live = exeBytes.subarray(windowOffset, windowOffset + CLAMP_BYTES);

// The plugin carries the same window, and the offsets the test uses must be the
// ones the plugin writes at: read the array out of the source instead of
// repeating it here.
const pluginSource = fs.readFileSync(pluginPath, 'utf8');
const arrayMatch = pluginSource.match(/kClampBytes\[\]\s*=\s*\{([^}]*)\}/);
check(!!arrayMatch, 'plugin.cpp declares the clamp window');
const pluginBytes = Buffer.from(
  (arrayMatch ? arrayMatch[1] : '').replace(/\/\*[\s\S]*?\*\//g, '').split(',')
    .map((t) => t.replace(/\/\/.*$/m, '').trim()).filter(Boolean)
    .map((t) => Number.parseInt(t, 0)));
check(pluginBytes.equals(live), 'the plugin window matches the pinned executable byte for byte');
check(/kClampOffset\s*=\s*0x40/.test(pluginSource), 'the plugin reads the window at function + 0x40');

check(live[0] === 0xb8, 'the window opens with mov eax,imm32');
check(live.readUInt32LE(1) === 0x20, 'the immediate is the 32-bit ceiling');
check(live[CEILING] === 0x20, `the shipped ceiling is 32 (0x${live[CEILING].toString(16)})`);
check(live[19] === 0x38 && live[20] === 0xc3, 'cmp bl,al precedes the clamp');
check(live[21] === 0x0f && live[22] === 0x47, 'the clamp itself is cmova ebx,eax');

// What the plugin writes, and what that means for the value the shader sees.
const patchedWindow = Buffer.from(live);
patchedWindow[CEILING] = 0x40;
function clampOf(window, size) {
  return Math.min(size, window[CEILING]);
}
check(clampOf(live, 64) === 32, 'before the patch a 64-bit label is clamped to 32');
check(clampOf(patchedWindow, 64) === 64, 'after the patch a 64-bit label keeps its size');
check(clampOf(patchedWindow, 32) === 32, '32-bit boards are unaffected by the patch');
check(clampOf(patchedWindow, 8) === 8, '8-bit boards are unaffected by the patch');
ok(`clamp window at ${'0x' + windowOffset.toString(16)} (VA 0x${(symbolAddress + CLAMP_OFFSET).toString(16)})`);

// --------------------------------------------------------------- the shaders
const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
const vert = fs.readFileSync(path.join(shaderDir, 'word_watchee.vert'), 'utf8');
const frag = fs.readFileSync(path.join(shaderDir, 'word_watchee.frag'), 'utf8');

const strideMatch = vert.match(/#define ROW_STRIDE (\d+)/);
const stride = strideMatch ? Number(strideMatch[1]) : 0;
check(stride === 10, `one line holds ten decimal digits (ROW_STRIDE=${stride})`);
check(frag.includes(`#define ROW_STRIDE ${stride}`), 'both shaders declare the same row stride');
check(vert.includes('frag_label_value[2 * ROW_STRIDE]') && frag.includes('frag_label_value[2 * ROW_STRIDE]'),
  'both shaders declare two lines of glyphs');
check(/shift = 28;/.test(vert), 'the 64-bit hex path starts the low word at 28');
check(!/shift = 32;/.test(vert), 'the off-by-one shift is gone');
check(/float label_rows = 1\.0;/.test(vert) && /label_rows = 2\.0;/.test(vert),
  'one line by default, two for a wide value');
check(/int row\) \* ROW_STRIDE \+ int\(whole\)/.test(frag) || /int\(row\) \* ROW_STRIDE \+ int\(whole\)/.test(frag),
  'the fragment shader addresses glyphs by line');
check(manifest.version !== '1.0.0', `package carries the two-line label (version ${manifest.version})`);

/* The wide branch must be pure decimal: no u_format, no forced sign. */
const wideBranch = vert.slice(vert.indexOf('} else if (inst_value_size > 32u) {'),
                              vert.indexOf('} else if (inst_value_override == OVERRIDE_FORCE_SIGNED) {'));
check(wideBranch.length > 0, 'the wide branch is present');
check(!/u_format/.test(wideBranch), 'a wide label ignores the game number format');
check(!/OVERRIDE_FORCE_SIGNED/.test(wideBranch), 'a wide label ignores the forced-sign override');
check(/repr_wide\(inst_value\)/.test(wideBranch), 'the wide branch splits the value');
check(/divmod10\(probe\)/.test(vert) && /int bottom_cells = digits > ROW_STRIDE \? digits - ROW_STRIDE : 0;/.test(vert),
  'the wide label counts the digits and breaks them after ten of them');
check(!/scratch\[/.test(vert),
  'the digits are written straight to their cells (no dynamically indexed array)');
check(/current \/ 10u/.test(vert) && !/0xCCCDu/.test(vert),
  'the 64-bit divide by ten is the exact long division, not the game inverse multiply');
check(/pad_row\(0, top_cells\)/.test(vert) && /pad_row\(1, bottom_cells\)/.test(vert),
  'the shorter line is padded instead of sampling undefined glyph cells');
check(/frag_label_value\[row \* ROW_STRIDE \+ i\] = IDX_SPACE/.test(vert),
  'padding writes the space glyph');

const MAX32 = 0xffffffffn;
/* The shader's repr_wide: the decimal digits of the value read unsigned over its
   own width, cut after `stride` digits - the leading ten on top, the rest below
   (a value that fits ten digits shows 0 on the second line). */
function wideRows(value, size) {
  const digits = (value & ((1n << BigInt(size)) - 1n)).toString(10);
  const rest = digits.slice(stride);
  return [digits.slice(0, stride), rest === '' ? '0' : rest];
}

const wideCases = [
  /* The example from the request: 2^63 = 9223372036854775808. */
  { value: 0x8000000000000000n, size: 64, want: ['9223372036', '854775808'] },
  { value: 0xffffffffffffffffn, size: 64, want: ['1844674407', '3709551615'] },
  { value: 0x0000000100000002n, size: 64, want: ['4294967298', '0'] },
  { value: 0x00000000000000ffn, size: 64, want: ['255', '0'] },
  { value: 0xffffffffffn, size: 40, want: ['1099511627', '775'] },
  { value: 0x1ffffffffn, size: 33, want: ['8589934591', '0'] },
];
let longest = 0;
for (const testCase of wideCases) {
  const rows = wideRows(testCase.value, testCase.size);
  check(rows[0] === testCase.want[0], `width ${testCase.size} value 0x${testCase.value.toString(16)}: top line ${testCase.want[0]} (got ${rows[0]})`);
  check(rows[1] === testCase.want[1], `width ${testCase.size} value 0x${testCase.value.toString(16)}: second line ${testCase.want[1]} (got ${rows[1]})`);
  longest = Math.max(longest, rows[0].length, rows[1].length);
}
check(longest <= stride, `every line fits the ${stride}-cell stride (longest ${longest})`);

/* Independent invariant over the whole range: the two lines are exactly the
   value's decimal digits, cut after ten of them. */
let checked = 0;
for (let size = 33; size <= 64; size++) {
  const mask = (1n << BigInt(size)) - 1n;
  for (const value of [0n, mask, 1n << BigInt(size - 1), mask ^ 0x5a5an, 1234567890123456789n & mask]) {
    const digits = (value & mask).toString(10);
    const rows = wideRows(value, size);
    if (digits.length > stride) {
      check(rows[0] + rows[1] === digits, `width ${size}: the two lines join back into ${digits}`);
    } else {
      check(rows[0] === digits && rows[1] === '0', `width ${size}: ${digits} fits the top line, 0 below`);
    }
    check(rows[0].length <= stride && rows[1].length <= stride, `width ${size}: both lines fit the stride`);
    checked += 1;
  }
}
ok(`two-line decimal labels checked over ${checked} width/value pairs (example 2^63 = 9223372036 / 854775808)`);

/* The one-line path of 32 bits and less must keep following the format. */
function oneLine(value, size, format) {
  const masked = value & ((1n << BigInt(size)) - 1n);
  if (format === 'hex') return masked.toString(16);
  if (format === 'unsigned') return masked.toString(10);
  const signBit = 1n << BigInt(size - 1);
  return (masked & signBit) ? (masked - (1n << BigInt(size))).toString(10) : masked.toString(10);
}
const oneLineCases = [
  { value: 0xdeadbeefn, size: 32, format: 'hex', want: 'deadbeef' },
  { value: 0xdeadbeefn, size: 32, format: 'unsigned', want: '3735928559' },
  { value: 0xffffffffn, size: 32, format: 'signed', want: '-1' },
  { value: 0x80n, size: 8, format: 'signed', want: '-128' },
  { value: 0xffn, size: 8, format: 'unsigned', want: '255' },
  { value: 0b1010n, size: 4, format: 'signed', want: '-6' },
];
for (const testCase of oneLineCases) {
  const got = oneLine(testCase.value, testCase.size, testCase.format);
  check(got === testCase.want, `one-line width ${testCase.size} ${testCase.format}: ${testCase.want} (got ${got})`);
}
check(/repr16\(inst_value\)/.test(vert) && /repr10\(inst_value, u_format >> 2 < 0\)/.test(vert),
  'the <=32-bit path still follows the game number format');
ok('one-line labels of 1..32 bits still follow hex / unsigned / signed');

if (failures) {
  console.error(`FAIL word watchee model: ${failures} check(s) failed`);
  process.exit(1);
}
console.log('PASS word watchee model: clamp window, two-line wide labels and one-line narrow labels agree');
