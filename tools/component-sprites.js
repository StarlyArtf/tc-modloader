// Read-only dump of the built-in components' appearance table.
//
// The pinned build draws an ordinary component as one PNG sprite, and the
// sprite name is chosen by the component kind: the EXE carries a contiguous,
// kind-ordered list of `com_*` names (0x00 first).  This tool recovers that list
// from the binary's strings and pairs each entry with its PNG in
// `asset/component_sprites`, so the mapping can be checked instead of guessed.
//
//   node tools/component-sprites.js [--csv]
//
// Notes:
//   * `com_deleted_N` marks a kind the game no longer uses; the entry still
//     holds the slot, which is what makes the index equal to the kind.
//   * A name without a file is deliberate: `com_custom` (kind 0x4e) has no
//     sprite at all - custom instances are drawn from their 32x32 design
//     thumbnail plus a name watermark (docs/research/component-appearance.md).
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const exe = path.resolve(process.argv[2] && /\.exe$/i.test(process.argv[2]) ? process.argv[2]
  : process.env.TC_EXE || path.join(root, '..', 'Turing Complete.exe'));
const sprites = path.resolve(process.env.TC_SPRITES || path.join(root, '..', 'asset', 'component_sprites'));

const bytes = fs.readFileSync(exe);
const text = bytes.toString('latin1');
const matches = [...text.matchAll(/com_[a-z0-9_]+/g)].map((m) => m[0]);

// The name table is one contiguous run; a handful of camera/UI strings live
// elsewhere and would only ever match by accident, so keep the longest run of
// names that are all present (or deliberately deleted) in order.
let run = [];
for (const name of matches) {
  const last = run[run.length - 1];
  if (!last || name !== last) run.push(name);
}
// The list starts at com_none (kind 0); anything before it belongs to another
// table that happens to share the prefix.
const start = run.indexOf('com_none');
if (start < 0) throw new Error('com_none not found: the name table moved');
run = run.slice(start);
/* Every kind names a different sprite, so the kind table is exactly the longest
   prefix with no repeated name; the repeats after it belong to other string
   tables (manual pages, help images) that happen to start with `com_` too. */
{
  const seen = new Set();
  const unique = [];
  for (const name of run) {
    if (seen.has(name)) break;
    seen.add(name);
    unique.push(name);
  }
  run = unique;
}

function pngSize(file) {
  try {
    const head = Buffer.alloc(24);
    const fd = fs.openSync(file, 'r');
    fs.readSync(fd, head, 0, 24, 0);
    fs.closeSync(fd);
    if (head.readUInt32BE(0) !== 0x89504e47) return null;
    return { width: head.readUInt32BE(16), height: head.readUInt32BE(20) };
  } catch {
    return null;
  }
}

const rows = run.map((name, kind) => {
  const file = path.join(sprites, name + '.png');
  const size = pngSize(file);
  return {
    kind, name, file: path.basename(file),
    present: size !== null,
    width: size ? size.width : 0,
    height: size ? size.height : 0,
  };
});

if (process.argv.includes('--csv')) {
  console.log('kind,name,file,width,height');
  for (const row of rows)
    console.log(`0x${row.kind.toString(16).padStart(2, '0')},${row.name},${row.present ? row.file : ''},${row.width},${row.height}`);
} else {
  console.log(`sprite table: ${rows.length} entries, ${rows.filter((r) => r.present).length} files in ${sprites}`);
  for (const row of rows) {
    const size = row.present ? `${row.width}x${row.height}` : '-';
    console.log(`  0x${row.kind.toString(16).padStart(2, '0')}  ${row.name.padEnd(30)} ${size}`);
  }
}
