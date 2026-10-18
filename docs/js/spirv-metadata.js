// Mirrors tools/shaders/spirv_metadata.py and validate_vulkan_corpus.py.
// ABI reflection is deliberately bounded; DXC also validates the generated code.
export function inspectSpirv(binary, stage) {
  if (binary.length < 20 || binary.length % 4) throw new Error('Truncated SPIR-V');
  const view = new DataView(binary.buffer, binary.byteOffset, binary.byteLength);
  const w = Array.from({ length: binary.length / 4 }, (_, i) => view.getUint32(i * 4, true));
  if (w[0] !== 0x07230203 || w[1] < 0x10000 || w[1] > 0x10300 || !w[3] || w[3] > 1000000 || w[4]) throw new Error('Invalid Vulkan 1.1 SPIR-V');
  const types = new Map(), constants = new Map(), decorations = new Map(), variables = new Map(), aliases = new Map();
  const capabilities = [], extensions = [], entries = [], accesses = [];
  const string = (a) => {
    const bytes = new Uint8Array(a.length * 4), data = new DataView(bytes.buffer);
    a.forEach((n, i) => data.setUint32(i * 4, n, true));
    const end = bytes.indexOf(0); if (end < 0) throw new Error('Unterminated SPIR-V string');
    return new TextDecoder('utf-8', { fatal: true }).decode(bytes.subarray(0, end));
  };
  const id = (n) => { if (!n || n >= w[3]) throw new Error('SPIR-V ID out of bounds'); };
  const minimum = { 17: 1, 10: 1, 15: 3, 21: 3, 22: 2, 23: 3, 24: 3, 25: 8, 26: 1, 27: 2, 28: 3, 29: 2, 30: 1, 32: 3, 43: 3, 50: 3, 59: 3, 71: 2 };
  for (let offset = 5; offset < w.length;) {
    const count = w[offset] >>> 16, op = w[offset] & 65535;
    if (!count || offset + count > w.length) throw new Error('Invalid instruction length');
    const a = w.slice(offset + 1, offset + count); offset += count;
    if (a.length < (minimum[op] || 0)) throw new Error('Truncated operands');
    if (op === 17) capabilities.push(a[0]);
    else if (op === 10) extensions.push(string(a));
    else if (op === 15) { id(a[1]); entries.push(a[0]); }
    else if (op >= 19 && op <= 33) { id(a[0]); types.set(a[0], [op, a.slice(1)]); }
    else if (op === 43 || op === 50) { id(a[1]); constants.set(a[1], a[2]); }
    else if (op === 65 || op === 66) {
      if (a.length < 4) throw new Error('Truncated access chain');
      aliases.set(a[1], a[2]); accesses.push([a[2], a[3]]);
    } else if (op === 83) {
      if (a.length < 3) throw new Error('Truncated copy object'); aliases.set(a[1], a[2]);
    } else if (op === 59) { id(a[0]); id(a[1]); variables.set(a[1], { type: a[0], storage: a[2] }); }
    else if (op === 71) { id(a[0]); if (!decorations.has(a[0])) decorations.set(a[0], new Map()); decorations.get(a[0]).set(a[1], a.slice(2)); }
  }
  if (entries.length !== 1 || entries[0] !== (stage === 'vs' ? 0 : 4)) throw new Error('Shader entry stage mismatch');
  if (capabilities.some((n) => ![0, 1, 28, 29, 30, 31, 32, 33, 50, 51, 52].includes(n))) throw new Error('Unsupported shader capability');
  if (extensions.some((s) => s !== 'SPV_KHR_storage_buffer_storage_class')) throw new Error('Unsupported SPIR-V extension');
  const typeName = (tid, seen = new Set()) => {
    if (seen.has(tid) || !types.has(tid)) throw new Error('Invalid interface type');
    seen = new Set([...seen, tid]); const [op, a] = types.get(tid);
    if (op === 21) return `${a[1] ? 'int' : 'uint'}${a[0]}`;
    if (op === 22) return `float${a[0]}`;
    if (op === 20) return 'bool';
    if (op === 23) return `${typeName(a[0], seen)}x${a[1]}`;
    if (op === 24) return `${typeName(a[0], seen)}m${a[1]}`;
    if (op === 28) return `${typeName(a[0], seen)}[${constants.get(a[1]) || 0}]`;
    if (op === 32) return typeName(a[1], seen);
    if (op === 30) return `struct(${a.map((n) => typeName(n, seen)).join(',')})`;
    return `type${op}`;
  };
  const dynamic = new Set();
  for (let [base, index] of accesses) {
    if (constants.has(index)) continue;
    const seen = new Set();
    while (aliases.has(base)) { if (seen.has(base)) throw new Error('Cyclic access chain'); seen.add(base); base = aliases.get(base); }
    dynamic.add(base);
  }
  const descriptors = [], inputs = [], outputs = [];
  const allowed = new Map([['0:0', ['storage_buffer', 1]], ['0:1', ['storage_buffer', 1]], ['0:2', ['storage_buffer', 1]],
    ['1:0', ['sampled_image_2d', 32]], ['1:1', ['sampled_image_3d', 32]], ['1:2', ['sampled_image_cube', 32]], ['1:3', ['sampled_image_2d_array', 32]], ['2:0', ['sampler', 32]], ['3:0', ['storage_buffer', 32]]]);
  for (const [vid, v] of variables) {
    const deco = decorations.get(vid) || new Map(), pointer = types.get(v.type);
    if (!pointer || pointer[0] !== 32) throw new Error('Missing variable pointer type');
    let tid = pointer[1][1], [op, a] = types.get(tid) || [0, []], count = 1;
    if (op === 29) throw new Error('Runtime descriptor array unsupported');
    if (op === 28) { count = constants.get(a[1]); tid = a[0]; [op, a] = types.get(tid) || [0, []]; }
    if (deco.has(33) || deco.has(34)) {
      if (!deco.get(33)?.length || !deco.get(34)?.length) throw new Error('Incomplete descriptor binding');
      let kind = 'unsupported';
      if (op === 26) kind = 'sampler';
      else if (op === 25 && a[5] === 1) kind = a[1] === 1 && a[3] === 1 ? 'sampled_image_2d_array' : { 1: 'sampled_image_2d', 2: 'sampled_image_3d', 3: 'sampled_image_cube' }[a[1]];
      else if (op === 30 && [2, 12].includes(v.storage)) kind = v.storage === 12 || decorations.get(tid)?.has(3) ? 'storage_buffer' : 'uniform_buffer';
      const set = deco.get(34)[0], binding = deco.get(33)[0], expected = allowed.get(`${set}:${binding}`);
      if (!expected || expected[0] !== kind || expected[1] !== count) throw new Error('Descriptor ABI mismatch');
      descriptors.push({ set, kind, count, dynamic: count !== 1 && dynamic.has(vid) });
    } else if ([1, 3].includes(v.storage) && deco.get(30)?.length) {
      const row = { location: deco.get(30)[0], type: typeName(pointer[1][1]) };
      const rows = v.storage === 1 ? inputs : outputs;
      if (row.location > 255 || rows.length >= 256 || rows.some((r) => r.location === row.location)) throw new Error('Invalid interface location');
      rows.push(row);
    }
  }
  const components = (rows) => {
    let maximum = 0;
    for (const row of rows) {
      const match = /^(?:float|int|uint)(\d+)(?:x(\d+))?(?:m(\d+))?((?:\[\d+\])*)$|^(bool)$/.exec(row.type);
      if (!match) throw new Error('Unsupported interface component type');
      const lanes = Number(match[2] || 1) * (Number(match[1] || 32) === 64 ? 2 : 1);
      let slots = Number(match[3] || 1);
      for (const size of (match[4] || '').matchAll(/\[(\d+)\]/g)) slots *= Number(size[1]);
      maximum = Math.max(maximum, row.location * 4 + (slots - 1) * Math.ceil(lanes / 4) * 4 + lanes);
    }
    return maximum;
  };
  const sum = (prefix) => descriptors.filter((d) => d.kind.startsWith(prefix)).reduce((n, d) => n + d.count, 0);
  const requirements = [sum('storage_buffer'), sum('uniform_buffer'), sum('sampled_image'), sum('sampler'),
    Math.max(0, ...descriptors.map((d) => d.set + 1)), stage === 'vs' ? Math.max(0, ...inputs.map((r) => r.location + 1)) : 0,
    stage === 'vs' ? components(outputs) : 0, stage === 'ps' ? components(inputs) : 0,
    Number(capabilities.includes(29) || descriptors.some((d) => d.dynamic && (d.kind.startsWith('sampled_image') || d.kind === 'sampler'))),
    Number(capabilities.includes(30) || descriptors.some((d) => d.dynamic && d.kind === 'storage_buffer')),
    Number(capabilities.includes(32)), Number(capabilities.includes(33))];
  return { requirements, inputs, outputs };
}
