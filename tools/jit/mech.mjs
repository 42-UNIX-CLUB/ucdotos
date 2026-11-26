// SPDX-License-Identifier: GPL-2.0-or-later
// Mechanism measurements for the binary translator (docs/BINARY-TRANSLATION.md, section 2):
//   1. what it costs to compile and instantiate a freshly generated module, by size;
//   2. how V8 tiers a fresh module up (baseline first, optimised later, no on-stack replacement).
// Runs under Node (node tools/jit/mech.mjs) and in Chromium (tools/jit/mech.html through
// tools/jit/chrome.mjs). Modules are built here with a tiny encoder; nothing is cached
// between iterations because every module gets different bytes (V8 caches identical ones).

const leb = (n) => { const o = []; for (;;) { const b = n & 0x7f; n >>>= 7; if (!n) { o.push(b); return o; } o.push(b | 0x80); } };
const sleb = (n) => { n = BigInt(n); const o = []; for (;;) { const b = Number(n & 0x7fn); n >>= 7n; if ((n === 0n && !(b & 0x40)) || (n === -1n && (b & 0x40))) { o.push(b); return o; } o.push(b | 0x80); } };
const str = (s) => [...leb(s.length), ...new TextEncoder().encode(s)];
const vec = (xs) => [...leb(xs.length), ...xs.flat()];
const sec = (id, b) => [id, ...leb(b.length), ...b];
const I32 = 0x7f, I64 = 0x7e;

// One function "work": (i32 n) -> i64. A loop of n iterations doing `ops` i64 operations
// on four locals (a stand-in for translated straight-line guest code).
function body(ops, salt) {
  const c = [];
  const L = (i) => [0x20, i], S = (i) => [0x21, i];
  c.push(0x42, ...sleb(salt), ...S(1), 0x42, ...sleb(7), ...S(2), 0x42, ...sleb(11), ...S(3), 0x42, ...sleb(13), ...S(4));
  c.push(0x03, 0x40);                                  // loop
  for (let k = 0; k < ops; k++) {
    const d = 1 + (k % 4), s = 1 + ((k + 1) % 4), t = 1 + ((k + 2) % 4);
    switch (k % 5) {
      case 0: c.push(...L(s), ...L(t), 0x7c, ...S(d)); break;                       // add
      case 1: c.push(...L(s), 0x42, ...sleb(0x9e37 + k), 0x7e, ...S(d)); break;    // mul by constant
      case 2: c.push(...L(s), ...L(t), 0x85, ...S(d)); break;                       // xor
      case 3: c.push(...L(s), 0x42, ...sleb(13), 0x88, ...S(d)); break;            // shr_u
      case 4: c.push(...L(d), ...L(s), 0x7d, ...S(d)); break;                       // sub
    }
  }
  c.push(...L(0), 0x41, 1, 0x6b, 0x22, 0, 0x0d, 0);     // n = n - 1; br_if (n != 0) loop
  c.push(0x0b);                                        // end loop
  c.push(...L(1), ...L(2), 0x85, ...L(3), 0x85, ...L(4), 0x85, 0x0b);
  const f = [...vec([[4, I64]]), ...c];                // 4 i64 locals
  return [...leb(f.length), ...f];
}

// A module of nf functions, each ops operations in its loop. Exports f0..f(nf-1).
export function makeModule(nf, ops, salt) {
  const type = [0x60, ...vec([[I32]]), ...vec([[I64]])];
  const funcs = [], exps = [], bodies = [];
  for (let i = 0; i < nf; i++) { funcs.push([0]); exps.push([...str('f' + i), 0, ...leb(i)]); bodies.push(body(ops, salt * 1000 + i)); }
  return new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, ...sec(1, vec([type])), ...sec(3, vec(funcs)), ...sec(7, vec(exps)), ...sec(10, vec(bodies))]);
}

const now = () => performance.now();
const med = (a) => { const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };
const fmt = (ms) => ms < 1 ? (ms * 1000).toFixed(1) + ' us' : ms.toFixed(2) + ' ms';

export async function run(log, opts = {}) {
  const reps = opts.reps || 7;
  log(`# translator mechanism measurements (${typeof process === 'object' && process.versions?.node ? 'Node ' + process.versions.node : navigator.userAgent.match(/Chrom\S+/)?.[0]})`);
  // ---- 1. compile + instantiate cost by size ----
  const sizes = [[1, 4], [8, 180], [10, 1800], [100, 1800], [800, 2300]];
  let salt = 1;
  log('\n## 1. compile and instantiate a generated module (best / median of ' + reps + ', each with different bytes)');
  log('| size | functions | new Module (sync) | new Instance | first call of one function | first call of every function | WebAssembly.compile (async) + instantiate | async, first call of every function |');
  log('|---|---|---|---|---|---|---|---|');
  // Each sample is a batch of k modules (k chosen so a batch lasts well over Chromium's
  // 0.1 ms clock tick); figures are per module.
  for (const [nf, ops] of sizes) {
    const r = { mod: [], inst: [], one: [], all: [], async: [], asyncAll: [] };
    const len = makeModule(nf, ops, 0).length;
    const k = Math.max(1, Math.min(200, Math.round(2e6 / len)));
    let refused = false;
    for (let i = 0; i < reps && !refused; i++) {
      const bytes = [], bytes2 = [];
      for (let j = 0; j < k; j++) { bytes.push(makeModule(nf, ops, salt++)); bytes2.push(makeModule(nf, ops, salt++)); }
      let t0 = now();
      const ms = [];
      try { for (const b of bytes) ms.push(new WebAssembly.Module(b)); } catch (e) { refused = e.message; break; }
      const t1 = now();
      const insts = ms.map((m) => new WebAssembly.Instance(m, {}));
      const t2 = now();
      for (const inst of insts) inst.exports.f0(1);
      const t3 = now();
      for (const inst of insts) for (let f = 1; f < nf; f++) inst.exports['f' + f](1);
      const t4 = now();
      r.mod.push((t1 - t0) / k); r.inst.push((t2 - t1) / k); r.one.push((t3 - t2) / k); r.all.push((t4 - t2) / k);
      t0 = now();
      const insts2 = [];
      for (const b of bytes2) insts2.push(await WebAssembly.instantiate(await WebAssembly.compile(b), {}));
      const t5 = now();
      for (const inst of insts2) for (let f = 0; f < nf; f++) inst.exports['f' + f](1);
      const t6 = now();
      r.async.push((t5 - t0) / k); r.asyncAll.push((t6 - t5) / k);
    }
    const c = (a) => a.length ? `${fmt(Math.min(...a))} / ${fmt(med(a))}` : 'refused';
    const label = len < 1024 ? len + ' B' : len < 1 << 20 ? (len / 1024).toFixed(0) + ' KiB' : (len / 1048576).toFixed(1) + ' MiB';
    log(`| ${label} | ${nf} | ${c(r.mod)} | ${c(r.inst)} | ${c(r.one)} | ${c(r.all)} | ${c(r.async)} | ${c(r.asyncAll)} |`);
    if (refused) log('(sync compile refused: ' + refused.split('.')[0] + ')');
  }

  // ---- 2. tiering ----
  log('\n## 2. tiering of a fresh module (one function, a loop of 20 i64 operations per iteration)');
  const iter = opts.iter || 200000;           // about 1 ms per call when optimised
  {
    const inst = new WebAssembly.Instance(new WebAssembly.Module(makeModule(1, 20, salt++)), {});
    const f = inst.exports.f0;
    const times = [];
    for (let i = 0; i < 300; i++) { const t0 = now(); f(iter); times.push(now() - t0); }
    const best = Math.min(...times);
    const firstFast = times.findIndex((t) => t < best * 1.25);
    const ns = (t) => (t * 1e6 / iter).toFixed(2);
    log(`(a) called repeatedly with ${iter} iterations: ns per iteration at call 1: ${ns(times[0])}, 2: ${ns(times[1])}, 3: ${ns(times[2])}, 5: ${ns(times[4])}, 10: ${ns(times[9])}, 50: ${ns(times[49])}, best: ${ns(best)}; first call within 1.25x of best: call ${firstFast + 1} (after ${times.slice(0, firstFast).reduce((a, b) => a + b, 0).toFixed(1)} ms)`);
    const inst2 = new WebAssembly.Instance(new WebAssembly.Module(makeModule(1, 20, salt++)), {});
    const big = iter * 150;
    const t0 = now(); inst2.exports.f0(big); const t = now() - t0;
    log(`(b) one call of ${big} iterations on a fresh module: ${(t * 1e6 / big).toFixed(2)} ns per iteration (${(t * 1e6 / big / (best * 1e6 / iter)).toFixed(2)}x the best of (a)); total ${t.toFixed(0)} ms`);
    // (c) after (a) warmed f0 up, is a single long call fast? (it should be: the new code is used by new calls)
    const t2 = now(); f(big); const tt = now() - t2;
    log(`(c) one call of ${big} iterations on the warmed module of (a): ${(tt * 1e6 / big).toFixed(2)} ns per iteration`);
  }
  {
    // (d) how much work before tier-up: calls of a short function (100 iterations) until fast
    const inst = new WebAssembly.Instance(new WebAssembly.Module(makeModule(1, 20, salt++)), {});
    const f = inst.exports.f0;
    const batch = 200, n = 100, rows = [];
    let best = Infinity;
    for (let b = 0; b < 400; b++) {
      const t0 = now(); for (let i = 0; i < batch; i++) f(n); const t = now() - t0;
      rows.push(t); best = Math.min(best, t);
    }
    const ff = rows.findIndex((t) => t < best * 1.25);
    log(`(d) short calls (${n} iterations each), batches of ${batch} calls: ns per iteration first batch ${(rows[0] * 1e6 / batch / n).toFixed(2)}, best ${(best * 1e6 / batch / n).toFixed(2)}; first batch within 1.25x of best: batch ${ff + 1} (${(ff * batch)} calls, ${(ff * batch * n)} iterations, ${rows.slice(0, ff).reduce((a, b) => a + b, 0).toFixed(1)} ms)`);
  }
  log('@@DONE');
}

if (typeof process === 'object' && process.argv?.[1]?.endsWith('mech.mjs')) await run((s) => console.log(s));
