// SPDX-License-Identifier: GPL-2.0-or-later
// The translator benchmark under Node. Chromium: tools/jit/chrome.mjs --page=tools/jit/bench.html?build=...
//   node tools/jit/bench.mjs --build=build-NAME [--programs=sieve,fib] [--configs=interp,struct+calls]
//                            [--rounds=2] [--reps=5] [--extra='bounds=checked'] [--gc] [--no-native]
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
import { bench, formatHeader, formatDetail, PROGRAMS, CONFIGS } from './bench-core.mjs';
const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const root = new URL('../..', import.meta.url).pathname;
const dir = `${root}${opt('build', 'build')}/jit/`;
const programs = opt('programs', PROGRAMS.join(',')).split(',');
const configs = opt('configs', Object.keys(CONFIGS).join(',')).split(',');
const log = (s) => console.log(s);
log(`# translator benchmark, Node ${process.versions.node}; guest MIPS = interpreter-counted guest instructions / time; best (median)`);
log(formatHeader(configs));
const native = args.includes('--no-native') ? null : async (p) => {
  const out = execFileSync(`${dir}guest/${p}.native`, ['5']).toString();
  const m = out.match(/best_ms=(\d+) first_ms=(\d+)/);
  return { best: Number(m[1]) / 1000, first: Number(m[2]) / 1000 };
};
const results = await bench({ load: async (f) => fs.readFileSync(dir + f), log, programs, configs, rounds: Number(opt('rounds', 2)), reps: Number(opt('reps', 5)), extra: opt('extra', ''), native, gc: args.includes('--gc') });
log('\n' + formatDetail(results, configs));
if (opt('json', null)) fs.writeFileSync(opt('json'), JSON.stringify(results, null, 1));
