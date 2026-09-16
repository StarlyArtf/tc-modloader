// Read-only analysis of the pinned, locally supplied game executable.
// No game code is executed. Full disassembly stays in ignored build/.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { execFileSync } = require('node:child_process');
const assert = require('node:assert/strict');

const root = path.resolve(__dirname, '..');
const exe = path.resolve(process.argv[2] || path.join(root, '..', 'Turing Complete.exe'));
const out = path.join(root, 'build', 'component-research');
const objdump = path.join(process.env.TC_MINGW_BIN || 'C:/msys64/ucrt64/bin', 'objdump.exe');
const bytes = fs.readFileSync(exe);
const hash = crypto.createHash('sha256').update(bytes).digest('hex');
assert.equal(hash, '8875da0e88cc878cb0fce30cfb63d20c5500cae341608af4d786649b88c8eb21',
  'Unsupported game build; do not interpret offsets on another executable');
fs.mkdirSync(out, { recursive: true });
const names = {
  lookup: 'get_prototype__modelZboardZcustom95prototype95list_u502',
  customLookup: 'get_custom_prototype__modelZboardZcustom95prototype95list_u451',
  update: 'update_custom_prototype__modelZboardZcustom95prototype_u2734',
  add: 'add_custom_prototype__modelZboardZcustom95prototype_u2718',
  reload: 'reload_custom_prototype__modelZboardZcustom95prototype_u1158',
  boardAdd: 'board_add_component__modelZboardZboard_u27308',
  preorder: 'preorder__modelZsimulationZpreorder_u8749',
  request: 'process_request__modelZsimulationZcompile95thread_u140',
  source: 'generate_source',
  codegen: 'add_circuit_code__modelZsimulationZcode95gen_u4263',
  compileRun: 'handle_request_compile_and_run__modelZsimulationZsimulator95functions_u20',
  dynInit: 'atmmodelatssimulationatssimulator_functionsdotnim_DatInit000',
  jitThread: 'jit_function__modelZsimulationZsimulator95functions_u84',
  jit: 'jit__modelZsimulationZjitZjit_u1807',
};
const asm = {};
const functions = {};
for (const [key, name] of Object.entries(names)) {
  const text = execFileSync(objdump, ['-d', '-Mintel', `--disassemble=${name}`, exe],
    { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024, windowsHide: true });
  assert(text.includes(`<${name}>:`), `Missing function: ${name}`);
  asm[key] = text;
  fs.writeFileSync(path.join(out, `${key}.asm`), text);
  functions[key] = { symbol: name,
    address: '0x' + text.match(/^([0-9a-f]+) <.+>:/m)[1],
    directCalls: [...new Set([...text.matchAll(/\bcall\s+[0-9a-f]+ <([^>]+)>/g)].map(m => m[1]))] };
}
const checks = [];
function check(name, condition) {
  assert(condition, name);
  checks.push(name);
}
function calls(from, to) {
  check(`${from} calls ${to}`, functions[from].directCalls.includes(names[to] || to));
}
check('Custom ID is loaded as QWORD at +0x188',
  /mov\s+rcx,QWORD PTR \[r8\+0x188\]/.test(asm.lookup));
check('Hash slot uses low 16 bits', /movzx\s+edx,si/.test(asm.customLookup));
check('Hash lookup compares full 64-bit identity', /cmp\s+rsi,rax/.test(asm.customLookup));
check('add_custom_prototype saves hidden result pointer', /mov\s+r12,rcx/.test(asm.add));
check('add_custom_prototype writes 16-byte result', /movups\s+XMMWORD PTR \[r12\],xmm0/.test(asm.add));
calls('update', 'add');
calls('add', 'parse_state__modelZsave95mongerZsave95monger_u74');
calls('add', 'reload');
calls('reload', 'custom_prototypes_set__modelZboardZcustom95prototype95list_u192');
calls('reload', 'update_custom_design__modelZboardZcustom95prototype_u81');
calls('reload', 'update_uses__modelZboardZcustom95prototype_u780');
calls('boardAdd', 'customLookup');
calls('preorder', 'customLookup');
calls('request', 'preorder');
calls('request', 'source');
calls('request', 'compileRun');
calls('source', 'codegen');
calls('compileRun', 'createThread__modelZsimulationZsimulator95functions_u94');
calls('jitThread', 'jit');
check('Compile call uses the resolved dynamic symbol', asm.compileRun.includes('<Dl_3590324229_>'));
check('Dynamic initializer sets the same symbol', asm.dynInit.includes('<Dl_3590324229_>'));
check('Code generator dispatches through a kind-indexed jump table',
  /movsxd\s+rax,DWORD PTR \[rdx\+rax\*4\]/.test(asm.codegen));
check('JIT transfers control to calculated entry address',
  /lea\s+rax,\[rsi\+rdi\*1\]/.test(asm.jit) && /jmp\s+rax/.test(asm.jit));

// Translate virtual addresses through PE sections, never assume file offset == RVA.
const pe = bytes.readUInt32LE(0x3c), optional = pe + 24;
const base = Number(bytes.readBigUInt64LE(optional + 24));
const sections = optional + bytes.readUInt16LE(pe + 20);
function offset(va) {
  const rva = va - base;
  for (let i = 0; i < bytes.readUInt16LE(pe + 6); i++) {
    const s = sections + i * 40, start = bytes.readUInt32LE(s + 12);
    const size = bytes.readUInt32LE(s + 16);
    if (rva >= start && rva < start + size) return bytes.readUInt32LE(s + 20) + rva - start;
  }
  throw Error(`Unmapped VA ${va.toString(16)}`);
}
check('Dynamic function name is compile', bytes.subarray(offset(0x140534860), offset(0x140534860) + 8).equals(Buffer.from('compile\0')));
check('Dynamic library name is compile.dll', bytes.subarray(offset(0x1405349c8), offset(0x1405349c8) + 12).equals(Buffer.from('compile.dll\0')));
const table = 0x140534a60;
const customBranch = table + bytes.readInt32LE(offset(table) + 0x4e * 4);
check('Custom kind dispatches to shared codegen branch', customBranch === 0x140227e30);
const report = { executableSha256: hash, scope: 'Static binary evidence only; no gameplay validation',
  checks, customCodegenBranch: `0x${customBranch.toString(16)}`, functions };
fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify(report, null, 2) + '\n');
console.log(`PASS ${checks.length} pinned-binary component checks`);
console.log(`Evidence: ${out}`);
