// SPDX-License-Identifier: GPL-2.0-or-later
// What does it cost wasm to ask JavaScript for the time? Times a wasm loop that calls an
// imported now() -> f64, for several ways of providing that import. Runs under Node, and in
// Chromium through tools/browser.mjs (the same function is evaluated inside the page):
//
//   node tools/bench/clockcost.mjs                       # Node
//   node tools/bench/clockcost.mjs --browser [--build=build]   # headless Chromium (needs BUILD/dist)
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const root = new URL('../..', import.meta.url).pathname;
const args = process.argv.slice(2);

// The measurement. Self-contained so it can be stringified and evaluated in a page.
async function measure(bytes) {
  const mod = await WebAssembly.compile(new Uint8Array(bytes));
  const floorMod = await WebAssembly.compile(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 124, 3, 2, 1, 0, 7, 5, 1, 1, 102, 0, 0, 10, 13, 1, 11, 0, 68, 0, 0, 0, 0, 0, 0, 240, 63, 11]));
  const wasmFloor = (await WebAssembly.instantiate(floorMod, {})).exports.f; // () -> 1.0, in wasm
  const variants = {
    'wasm function (floor)': wasmFloor,
    'empty JS function': () => 1,
    '() => performance.now()': () => performance.now(),
    'performance.now.bind(performance)': performance.now.bind(performance),
    'Date.now': Date.now,
  };
  const out = {};
  for (const [name, now] of Object.entries(variants)) {
    const loop = (await WebAssembly.instantiate(mod, { h: { now } })).exports.loop;
    let n = 1000;
    for (;;) { const t0 = performance.now(); loop(n); if (performance.now() - t0 > 20) break; n *= 2; } // also warms up
    const runs = [];
    for (let i = 0; i < 15; i++) { const t0 = performance.now(); loop(n); runs.push((performance.now() - t0) * 1e6 / n); }
    runs.sort((a, b) => a - b);
    out[name] = { best: runs[0], median: runs[runs.length >> 1] };
  }
  return out;
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'clockcost-'));
fs.writeFileSync(path.join(tmp, 'c.c'), `__attribute__((import_module("h"), import_name("now"))) double now(void);
__attribute__((export_name("loop"), noinline)) double loop(int n) { double s = 0; for (int i = 0; i < n; i++) s += now(); return s; }\n`);
const r = spawnSync('clang', ['--target=wasm32-unknown-unknown', '-O2', '-nostdlib', '-Wl,--no-entry', '-o', path.join(tmp, 'c.wasm'), path.join(tmp, 'c.c')], { encoding: 'utf8' });
if (r.status) throw new Error(r.stderr);
const bytes = [...fs.readFileSync(path.join(tmp, 'c.wasm'))];

let result, where;
if (!args.includes('--browser')) { result = await measure(bytes); where = `Node ${process.version}`; }
else {
  const build = (args.find((a) => a.startsWith('--build=')) || '--build=build').slice(8);
  const script = path.join(tmp, 'script.mjs');
  fs.writeFileSync(script, `export default async (t) => { await t.sleep(500); process.stdout.write('RESULT' + JSON.stringify(await t.eval(${JSON.stringify(`(${measure.toString()})(${JSON.stringify(bytes)})`)})) + '\\n'); };\n`);
  const b = spawnSync('node', [path.join(root, 'tools/browser.mjs'), `--build=${build}`, `--query=scale=1&cmdline=${encodeURIComponent('init=/bin/true')}`, `--script=${script}`], { encoding: 'utf8', cwd: root });
  const line = (b.stdout || '').split('\n').find((l) => l.startsWith('RESULT'));
  if (!line) throw new Error(b.stdout + b.stderr);
  result = JSON.parse(line.slice(6)); where = 'Chromium (headless)';
}
console.log(`ns per call of an imported now(), ${where} (best / median of 15 batches)`);
for (const [k, v] of Object.entries(result)) console.log(`  ${k.padEnd(36)} ${v.best.toFixed(1).padStart(7)} / ${v.median.toFixed(1)}`);
