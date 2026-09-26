// Compares a VCD written by Icarus Verilog with the engine's own event list.
//
//   node vcd-compare.js <iverilog.vcd> <engine.expected>
//
// The expected file is one `<tick> <net> <value>` line per net change
// (`simcore-tests --emit-verilog`).  Both sides are turned into per-net step
// functions and compared after every time step either of them reported, which
// is stricter than comparing the printed lines: a missing transition and an
// extra one both fail, and a value that differs only at a time nobody printed
// still fails if the two agree on that time.

const fs = require('fs');

function parseExpected(text) {
  const events = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line) continue;
    const parts = line.split(/\s+/);
    if (parts.length !== 3) throw new Error('bad expected line: ' + line);
    const time = Number(parts[0]);
    if (!Number.isFinite(time)) throw new Error('bad expected time: ' + line);
    events.push({ time, net: parts[1], value: parts[2].toLowerCase() });
  }
  return events;
}

function parseVcd(text) {
  const codeToName = new Map();
  const events = [];
  let time = -1;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line) continue;
    if (line.startsWith('$var')) {
      const parts = line.split(/\s+/);
      if (parts.length >= 5) codeToName.set(parts[3], parts[4]);
      continue;
    }
    if (line.startsWith('$')) continue;
    if (line.startsWith('#')) {
      time = Number(line.slice(1));
      continue;
    }
    if (time < 0) continue;
    let value;
    let code;
    if (line[0] === 'b' || line[0] === 'B') {
      const space = line.indexOf(' ');
      if (space < 0) continue;
      value = line.slice(1, space).toLowerCase();
      code = line.slice(space + 1).trim();
    } else {
      value = line[0].toLowerCase();
      code = line.slice(1).trim();
    }
    const name = codeToName.get(code);
    if (!name) continue;
    events.push({ time, net: name, value });
  }
  return events;
}

// Per net: the value in force after every time step, keyed by time.
function timeline(events) {
  const nets = new Map();
  for (const event of events) {
    if (!nets.has(event.net)) nets.set(event.net, []);
    nets.get(event.net).push(event);
  }
  for (const list of nets.values()) list.sort((left, right) => left.time - right.time);
  return nets;
}

function stateAt(list, time) {
  let value = null;
  for (const event of list) {
    if (event.time > time) break;
    value = event.value;
  }
  return value;
}

// VCD lets a writer drop leading zeros on a multi-bit value (iverilog writes
// `b1` for a 4-bit 0001), so the short form is padded with zeros.  An unknown
// or high impedance digit extends with itself, since "dropping leading x" is
// not a thing a writer does.
function sameValue(left, right) {
  if (left === right) return true;
  if (!/^[01xz]+$/.test(left) || !/^[01xz]+$/.test(right)) return false;
  const width = Math.max(left.length, right.length);
  const pad = (text) => (/^[xz]/.test(text) ? text.padStart(width, text[0]) : text.padStart(width, '0'));
  return pad(left) === pad(right);
}

function main() {
  const [vcdPath, expectedPath] = process.argv.slice(2);
  if (!vcdPath || !expectedPath) {
    console.error('usage: node vcd-compare.js <iverilog.vcd> <engine.expected>');
    return 2;
  }
  const theirs = timeline(parseVcd(fs.readFileSync(vcdPath, 'utf8')));
  const ours = timeline(parseExpected(fs.readFileSync(expectedPath, 'utf8')));

  const problems = [];
  for (const name of ours.keys()) {
    if (!theirs.has(name)) problems.push('net only the engine knows about: ' + name);
  }
  for (const name of theirs.keys()) {
    if (!ours.has(name)) problems.push('net only iverilog knows about: ' + name);
  }

  const times = new Set();
  for (const list of ours.values()) for (const event of list) times.add(event.time);
  for (const list of theirs.values()) for (const event of list) times.add(event.time);
  const ordered = [...times].sort((left, right) => left - right);

  let compared = 0;
  for (const time of ordered) {
    for (const [name, list] of ours) {
      if (!theirs.has(name)) continue;
      const mine = stateAt(list, time);
      if (mine === null) continue; // before the engine knew about this net
      const other = stateAt(theirs.get(name), time);
      if (other === null) continue; // iverilog has not dumped it yet
      ++compared;
      if (!sameValue(mine, other)) {
        problems.push(`t=${time} net=${name}: engine ${mine} != iverilog ${other}`);
      }
    }
  }

  if (problems.length) {
    console.error(`${problems.length} mismatch(es), ${compared} comparisons:`);
    for (const problem of problems.slice(0, 20)) console.error('  ' + problem);
    return 1;
  }
  console.log(`${compared} comparisons across ${ours.size} nets agree`);
  return 0;
}

process.exit(main());
