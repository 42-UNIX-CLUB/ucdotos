// SPDX-License-Identifier: GPL-2.0-or-later
// What does starting a process cost, step by step? Repeats each step of boot/host.mjs's
// start() many times in isolation, with the real trampoline and shim bytes and a real
// program from the build, and prints microseconds per step. The program is never run.
//
//   node tools/bench/spawncost.mjs [--build=build] [--prog=true] [--browser]
//
// With --browser the same function runs inside headless Chromium (needs BUILD/dist), which
// also shows whether the page may instantiate the program synchronously.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const root = new URL('../..', import.meta.url).pathname;
const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const build = opt('build', 'build');
const hostSrc = fs.readFileSync(path.join(root, 'boot/host.mjs'), 'utf8');
const bytesOf = (name) => [...new Uint8Array(eval(hostSrc.match(new RegExp(`const ${name} = new Uint8Array\\((\\[[\\s\\S]*?\\])\\);`))[1]))];
const input = { shim: bytesOf('SHIM'), tramp: bytesOf('TRAMPOLINE'), prog: [...fs.readFileSync(path.join(root, build, 'bin', opt('prog', 'true')))] };

async function measure({ shim, tramp, prog }) {
  const N = 300;
  const kmem = new WebAssembly.Memory({ initial: 64 });
  const sys = (await WebAssembly.instantiate(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 13, 1, 96, 8, 127, 127, 127, 127, 127, 127, 127, 127, 1, 127, 3, 2, 1, 0, 7, 5, 1, 1, 115, 0, 0, 10, 6, 1, 4, 0, 65, 0, 11]))).instance.exports.s;
  const t0 = performance.now();
  const progMod = await WebAssembly.compile(new Uint8Array(prog));
  const compileMs = performance.now() - t0;
  const shimMod = await WebAssembly.compile(new Uint8Array(shim));
  const trampMod = await WebAssembly.compile(new Uint8Array(tramp));
  const pid = (i) => new WebAssembly.Global({ value: 'i32', mutable: false }, 1 + (i & 31));
  const block = new WebAssembly.Suspending(async () => {});
  const out = { program_bytes: prog.length, compile_once_us: compileMs * 1000 };
  const time = async (name, f) => {
    for (let i = 0; i < 20; i++) await f(i); // warm up
    const t = performance.now();
    for (let i = 0; i < N; i++) await f(i);
    out[name] = (performance.now() - t) * 1000 / N;
  };
  const progImports = (c) => ({ host: { syscall: c, block } });
  await time('new Suspending', () => new WebAssembly.Suspending(async () => {}));
  await time('trampoline, await instantiate', async (i) => WebAssembly.instantiate(trampMod, { k: { sys }, p: { pid: pid(i) } }));
  await time('trampoline, new Instance', (i) => new WebAssembly.Instance(trampMod, { k: { sys }, p: { pid: pid(i) } }));
  const tr = new WebAssembly.Instance(trampMod, { k: { sys }, p: { pid: pid(0) } }).exports.c;
  await time('program, await instantiate', async () => WebAssembly.instantiate(progMod, progImports(tr)));
  try { await time('program, new Instance', () => new WebAssembly.Instance(progMod, progImports(tr))); }
  catch (e) { out['program, new Instance'] = 'refused: ' + e.message; }
  const pm = new WebAssembly.Instance(progMod, progImports(tr)).exports.memory;
  await time('shim, await instantiate', async () => WebAssembly.instantiate(shimMod, { k: { mem: kmem }, p: { mem: pm } }));
  await time('shim, new Instance', () => new WebAssembly.Instance(shimMod, { k: { mem: kmem }, p: { mem: pm } }));
  const inst = new WebAssembly.Instance(progMod, progImports(tr));
  const pages = inst.exports.memory.buffer.byteLength / 65536;
  out.program_initial_pages = pages;
  await time('new Memory (that many pages)', () => new WebAssembly.Memory({ initial: pages }));
  await time('WebAssembly.promising(_start)', () => WebAssembly.promising(inst.exports._start));
  await time('one await (microtask)', async () => {});
  return out;
}

let result, where;
if (!args.includes('--browser')) { result = await measure(input); where = `Node ${process.version}`; }
else {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'spawncost-'));
  const script = path.join(tmp, 'script.mjs');
  fs.writeFileSync(script, `export default async (t) => { await t.sleep(300); process.stdout.write('RESULT' + JSON.stringify(await t.eval(${JSON.stringify(`(${measure.toString()})(${JSON.stringify(input)})`)})) + '\\n'); };\n`);
  const b = spawnSync('node', [path.join(root, 'tools/browser.mjs'), `--build=${build}`, `--query=scale=1&cmdline=${encodeURIComponent('init=/bin/true')}`, `--script=${script}`], { encoding: 'utf8', cwd: root, maxBuffer: 1 << 26 });
  const line = (b.stdout || '').split('\n').find((l) => l.startsWith('RESULT'));
  if (!line) throw new Error(b.stdout + b.stderr);
  result = JSON.parse(line.slice(6)); where = 'Chromium (headless)';
}
console.log(`process start, step by step (${where}, program ${opt('prog', 'true')}), microseconds per step:`);
for (const [k, v] of Object.entries(result)) console.log(`  ${k.padEnd(34)} ${typeof v === 'number' ? v.toFixed(1) : v}`);
