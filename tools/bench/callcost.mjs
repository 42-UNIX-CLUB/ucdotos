// SPDX-License-Identifier: GPL-2.0-or-later
// Where do the nanoseconds of a system call go? Times a loop of calls from a wasm
// "program" into (1) a stand-in kernel that only returns and (2) a real kernel.wasm,
// each called directly and through the per-process trampoline from boot/host.mjs.
// Runs under Node only (no browser), nothing else of the system is started.
//
//   node tools/bench/callcost.mjs [--tree=DIR[:BUILD] ...] [--nr=173] [--rounds=7]
//
// --tree may be repeated (default: this tree); each tree's BUILD/kernel.wasm (BUILD defaults
// to "build") is measured with the trampoline from that tree's boot/host.mjs, interleaved
// round by round. The "direct" row is skipped when the kernel entry does not take 8 arguments.
// The kernel gets stub host imports; pid 1's slot is all zero, so getppid (173) returns 0.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const root = new URL('../..', import.meta.url).pathname;
const args = process.argv.slice(2);
const opts = (n) => args.filter((x) => x.startsWith(`--${n}=`)).map((x) => x.slice(n.length + 3));
const trees = (opts('tree').length ? opts('tree') : [root]).map((t) => { const [dir, b = 'build'] = t.split(':'); return { dir: path.resolve(dir), kernel: path.join(path.resolve(dir), b, 'kernel.wasm') }; });
const nr = Number(opts('nr')[0] || 173);
const rounds = Number(opts('rounds')[0] || 7);

// the trampoline bytes, taken from boot/host.mjs so this always measures the real one
const trampolineOf = (dir) => new Uint8Array(eval(fs.readFileSync(path.join(dir, 'boot/host.mjs'), 'utf8').match(/const TRAMPOLINE = new Uint8Array\((\[[\s\S]*?\])\);/)[1]));

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'callcost-'));
const cc = (def, out) => {
  const r = spawnSync('clang', ['--target=wasm32-unknown-unknown', '-O2', '-nostdlib', '-Wl,--no-entry', `-D${def}`, '-o', out, path.join(root, 'tools/bench/callcost.c')], { encoding: 'utf8' });
  if (r.status) throw new Error(r.stderr);
  return fs.readFileSync(out);
};
const callerMod = await WebAssembly.compile(cc('CALLER', path.join(tmp, 'caller.wasm')));
const calleeMod = await WebAssembly.compile(cc('CALLEE', path.join(tmp, 'callee.wasm')));

const hostStub = { log() {}, spawn() {}, wake() {}, reap() {}, now: () => performance.now(), epoch: () => Date.now(), random: () => 4, memory: () => 2 ** 32, fb_dirty() {} };
async function entry(kind, file) {
  if (kind === 'floor') return (await WebAssembly.instantiate(calleeMod, {})).exports.sys;
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(file), { host: hostStub });
  return instance.exports.syscall_pid;
}
async function caller(sys, trampMod) {
  const syscall = trampMod ? (await WebAssembly.instantiate(trampMod, { k: { sys }, p: { pid: new WebAssembly.Global({ value: 'i32', mutable: false }, 1) } })).exports.c : () => 0;
  return (await WebAssembly.instantiate(callerMod, { host: { syscall, sys8: trampMod ? () => 0 : sys } })).exports;
}

const configs = [];
for (const [label, kind, t] of [['floor (callee returns)', 'floor', { dir: root }], ...trees.map((t) => [path.basename(t.dir), 'kernel', t])]) {
  const sys = await entry(kind, t.kernel);
  try { configs.push({ label: `${label}, direct`, run: ((c) => (n) => c.loop8(n, 1, nr))(await caller(sys, null)) }); } catch { /* entry has another arity */ }
  const trampMod = await WebAssembly.compile(trampolineOf(kind === 'floor' ? root : t.dir));
  try { configs.push({ label: `${label}, trampoline`, run: ((c) => (n) => c.loop7(n, nr))(await caller(sys, trampMod)) }); } catch (e) { console.log(`${label}: ${e.message}`); }
}
const N = 1 << 20;
for (const c of configs) { c.t = []; for (let i = 0; i < 30; i++) c.run(N); } // warm up: let the engine tier up
for (let r = 0; r < rounds; r++) {
  for (const c of configs) {
    let best = Infinity;
    for (let i = 0; i < 5; i++) { const t0 = performance.now(); c.run(N); const dt = performance.now() - t0; if (dt < best) best = dt; }
    c.t.push(best * 1e6 / N);
  }
}
console.log(`ns per call, nr ${nr}, ${rounds} rounds of best-of-5 batches of ${N} calls (best / median / worst round)`);
for (const c of configs) {
  const s = [...c.t].sort((a, b) => a - b);
  console.log(`  ${c.label.padEnd(48)} ${s[0].toFixed(2)} / ${s[s.length >> 1].toFixed(2)} / ${s[s.length - 1].toFixed(2)}`);
}
