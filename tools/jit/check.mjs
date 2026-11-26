// SPDX-License-Identifier: GPL-2.0-or-later
// Correctness of the translator: every guest program must end in the same state (exit
// code, output, registers, memory) under every translated configuration as under the
// interpreter, and the lockstep mode (each region run under both and compared) must find
// no difference.
//   node tools/jit/check.mjs --build=build-NAME [--programs=a,b] [--quick] [--configs='opts;opts'] [--times]
// Keep each invocation short (a few programs) on a shared machine.
import fs from 'node:fs';
import { runEngine } from './sa-host.mjs';
const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const root = new URL('../..', import.meta.url).pathname;
const dir = `${root}${opt('build', 'build')}/jit/guest/`;
const engine = fs.readFileSync(`${root}${opt('build', 'build')}/jit/engine.wasm`);
const all = fs.readdirSync(dir).filter((f) => f.endsWith('.elf')).map((f) => f.slice(0, -4)).sort();
const programs = opt('programs', null)?.split(',') || all;
const configs = [
  'strategy=struct calls=1', 'strategy=struct calls=0', 'strategy=dispatch calls=1', 'strategy=dispatch calls=0', 'strategy=block calls=0',
  'mode=aot', 'bounds=checked', 'bounds=unchecked', 'threshold=1', 'threshold=1 maxblocks=3 maxinsns=20', 'budget=7', 'budget=7 yield_exit=1',
  'indirect=0', 'fp=0', 'chain=0', 'learn=0', 'ic=0', 'abi=1', 'fastfma=0 threshold=5', 'copytest=50', 'cache=8', 'calls=0 learn=1', 'fp=1 copytest=7 calls=0', 'lockstep=1 threshold=2',
];
const only = opt('configs', null);
const kv = (s) => Object.fromEntries((s || '').split(' ').filter(Boolean).map((x) => x.split('=')));
let checks = 0, fails = 0;
for (const p of programs) {
  const elf = fs.readFileSync(dir + p + '.elf');
  const ref = await runEngine(engine, elf, 'mode=interp');
  const rs = kv(ref.out.match(/@@STATE (.*)/)?.[1]);
  const list = only ? only.split(';') : args.includes('--quick') ? configs.slice(0, 3).concat(configs.slice(-1)) : configs;
  for (const c of list) {
    checks++;
    let out = '';
    const t0 = performance.now();
    try { out = (await runEngine(engine, elf, c + ' stats=1')).out; } catch (e) { out = 'exception: ' + e.message; }
    const st = kv(out.match(/@@STATE (.*)/)?.[1]);
    const ok = st.exit === rs.exit && st.out === rs.out && st.regs === rs.regs && st.mem === rs.mem && !/lockstep:|label bug|load failed|unexpected/.test(out);
    if (args.includes('--times')) console.log(`  ${p} [${c}] ${((performance.now() - t0) / 1000).toFixed(1)} s`);
    if (!ok) { fails++; console.log(`FAIL ${p} [${c}]: ${out.split('\n').filter((l) => !l.startsWith('checksum')).slice(0, 4).join(' / ')}`); }
  }
  console.log(`${p}: done (${rs.icount} instructions)`);
}
console.log(`check: ${checks} runs, ${fails} failed`);
process.exit(fails ? 1 : 0);
