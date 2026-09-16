// Evaluate the generated two-input AND fixture's internal netlist.
// The fixture is literal-only Snappy, so this decoder stays small and
// dependency-free; it still accepts copy chunks for future fixtures.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');

const file = path.resolve(process.argv[2] || 'build/and2_component.data');
const encoded = fs.readFileSync(file);
assert.equal(encoded[0], 14, 'AND fixture must use save format 14');

function readVarint(data, state) {
  let value = 0, shift = 0;
  while (state.pos < data.length) {
    const byte = data[state.pos++];
    value |= (byte & 0x7f) << shift;
    if (!(byte & 0x80)) return value >>> 0;
    shift += 7;
    assert(shift < 35, 'invalid Snappy varint');
  }
  throw new Error('truncated Snappy varint');
}

function uncompress(data) {
  const state = { pos: 0 };
  const expected = readVarint(data, state);
  const out = [];
  while (state.pos < data.length && out.length < expected) {
    const tag = data[state.pos++];
    const type = tag & 3;
    if (type === 0) {
      let length = (tag >> 2) + 1;
      if (length > 60) {
        const extra = length - 60;
        assert(state.pos + extra <= data.length, 'truncated literal length');
        length = 0;
        for (let i = 0; i < extra; i++)
          length |= data[state.pos++] << (8 * i);
        length += 1;
      }
      assert(state.pos + length <= data.length, 'truncated literal');
      for (let i = 0; i < length; i++) out.push(data[state.pos++]);
    } else {
      let length, offset;
      if (type === 1) {
        length = 4 + ((tag >> 2) & 7);
        offset = ((tag >> 5) << 8) | data[state.pos++];
      } else if (type === 2) {
        length = 1 + (tag >> 2);
        offset = data[state.pos] | (data[state.pos + 1] << 8);
        state.pos += 2;
      } else {
        length = 1 + (tag >> 2);
        offset = data[state.pos] | (data[state.pos + 1] << 8) |
                 (data[state.pos + 2] << 16) | (data[state.pos + 3] << 24);
        state.pos += 4;
      }
      assert(offset > 0 && offset <= out.length, 'invalid Snappy copy');
      for (let i = 0; i < length; i++) out.push(out[out.length - offset]);
    }
  }
  assert.equal(out.length, expected, 'Snappy length mismatch');
  return Buffer.from(out);
}

const raw = uncompress(encoded.subarray(1));
const state = { pos: 0 };
const take = n => {
  assert(state.pos + n <= raw.length, 'truncated circuit');
  const value = raw.subarray(state.pos, state.pos + n);
  state.pos += n;
  return value;
};
const u8 = () => take(1)[0];
const u16 = () => take(2).readUInt16LE(0);
const i16 = () => take(2).readInt16LE(0);
const i64 = () => take(8).readBigInt64LE(0);
const u64 = () => take(8).readBigUInt64LE(0);
const skipString = () => take(u16());
const skipSeqI64 = () => { for (let i = u16(); i; --i) i64(); };
const skipSeqU8 = () => take(u16());

i64(); take(4); i64(); i64(); u8(); i64();
skipSeqI64(); skipString(); u8(); u16(); skipSeqU8(); skipString();
assert.equal(state.pos + 512 <= raw.length, true, 'missing palette');
state.pos += 512;

const components = [];
for (let count = Number(i64()); count; --count) {
  const kind = u16();
  const x = i16();
  const y = i16();
  const flags = u8();
  const identity = i64();
  const name = skipString();
  skipSeqI64();
  const data = i64();
  const value16 = i16();
  const bits = i64();
  const boolA = u8();
  const valueA = i64();
  const valueB = i64();
  const boolB = u8();
  const init = u8();
  for (let n = u16(); n; --n) { i64(); i64(); skipString(); i64(); i64(); }
  for (let n = u16(); n; --n) { skipString(); skipString(); }

  if (kind === 0x4e) {
    // The fixture is a definition, not a custom-instance level.
    i64();
    for (let n = u16(); n; --n) { i64(); i64(); }
  }
  components.push({ kind, x, y, flags, identity, name, data, value16, bits,
                    boolA, valueA, valueB, boolB, init });
}

const wires = [];
for (let count = Number(i64()); count; --count) {
  const color = u8();
  const name = skipString();
  const x = i16();
  const y = i16();
  const segments = [];
  for (;;) {
    const segment = u16();
    segments.push(segment);
    if ((segment & 0x1fff) === 0) break;
  }
  wires.push({ color, name, x, y, segments });
}
assert.equal(state.pos, raw.length, 'parser did not consume the fixture');

if (process.env.TC_NETLIST_DEBUG) {
  console.log(components.map(c => ({ kind: c.kind, x: c.x, y: c.y,
    name: c.name.toString() })));
  console.log(wires);
}

const directions = {
  0: [1, 0], 1: [1, 1], 2: [0, 1], 3: [-1, 1],
  4: [-1, 0], 5: [-1, -1], 6: [0, -1], 7: [1, -1],
};
function pathPoints(wire) {
  const points = [[wire.x, wire.y]];
  let x = wire.x, y = wire.y;
  for (const segment of wire.segments.slice(0, -1)) {
    const [dx, dy] = directions[segment >> 13];
    const length = segment & 0x1fff;
    x += dx * length;
    y += dy * length;
    points.push([x, y]);
  }
  return points;
}

function pinPoints(component) {
  const { kind, x, y } = component;
  if (kind === 0x4f) return [{ point: [x + 3, y], pin: 'out' }];
  if (kind === 0x51) return [{ point: [x - 3, y], pin: 'in' }];
  if (kind === 0x04) {
    return [
      { point: [x - 1, y - 1], pin: 'in0' },
      { point: [x - 1, y + 1], pin: 'in1' },
      { point: [x + 2, y], pin: 'out' },
    ];
  }
  throw new Error(`unexpected component kind 0x${kind.toString(16)}`);
}

const key = ([x, y]) => `${x},${y}`;
const index = new Map();
const parent = [];
function find(value) {
  while (parent[value] !== value) {
    parent[value] = parent[parent[value]];
    value = parent[value];
  }
  return value;
}
function union(a, b) {
  a = find(a); b = find(b);
  if (a !== b) parent[b] = a;
}
function node(point) {
  const name = key(point);
  if (!index.has(name)) {
    index.set(name, parent.length);
    parent.push(parent.length);
  }
  return index.get(name);
}

const pins = [];
for (const component of components) {
  for (const pin of pinPoints(component)) pins.push({ ...pin, component });
}
for (const wire of wires) {
  const points = pathPoints(wire);
  const first = node(points[0]);
  for (const point of points.slice(1)) union(first, node(point));
}

function evaluate(values) {
  const and = components.find(component => component.kind === 0x04);
  const inputA = components.find(component => component.kind === 0x4f && component.name.toString() === 'A');
  const inputB = components.find(component => component.kind === 0x4f && component.name.toString() === 'B');
  const output = components.find(component => component.kind === 0x51);
  assert(and && inputA && inputB && output, 'missing AND fixture components');

  const netA = find(node([inputA.x + 3, inputA.y]));
  const netB = find(node([inputB.x + 3, inputB.y]));
  const netAnd0 = find(node([and.x - 1, and.y - 1]));
  const netAnd1 = find(node([and.x - 1, and.y + 1]));
  const netAndOut = find(node([and.x + 2, and.y]));
  const netOutput = find(node([output.x - 3, output.y]));

  assert.equal(netA, netAnd0, 'A is not connected to AND input 0');
  assert.equal(netB, netAnd1, 'B is not connected to AND input 1');
  assert.equal(netAndOut, netOutput, 'AND output is not connected to Out');

  const map = new Map([[netA, values.a], [netB, values.b]]);
  map.set(netAndOut, map.get(netA) & map.get(netB));
  return map.get(netOutput);
}

const truth = [[0, 0, 0], [0, 1, 0], [1, 0, 0], [1, 1, 1]];
for (const [a, b, expected] of truth) {
  assert.equal(evaluate({ a, b }), expected,
               `AND truth table failed for ${a}${b}`);
}

console.log("PASS AND fixture netlist: 00, 01, 10, 11 -> 0, 0, 0, 1");
