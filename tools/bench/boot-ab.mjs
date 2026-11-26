// SPDX-License-Identifier: GPL-2.0-or-later
// Boot phases of the page in Chromium for two trees, alternately (tools/bench/boot.mjs does
// one run): milliseconds since navigation until the kernel and image are fetched, the kernel
// is up, and the first frame is on the canvas. Prints best and median of each phase.
//   node tools/bench/boot-ab.mjs --a=TREE[:BUILD] --b=TREE[:BUILD] [--runs=10] [--query=scale=1]
// Each tree must have BUILD/dist (make B=BUILD dist). Served from localhost: transfer time is
// close to zero here, so this measures everything except the network.
import { spawnSync } from 'node:child_process';
import path from 'node:path';

const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const here = path.dirname(new URL(import.meta.url).pathname);
const side = (s) => { const [tree, build = 'build'] = s.split(':'); return { tree: path.resolve(tree), build }; };
const sides = [side(opt('a', '.')), side(opt('b', '.'))];
const runs = Number(opt('runs', 10));
const res = [[], []];
for (let k = 0; k < runs; k++) {
  for (let n = 0; n < 2; n++) {
    const s = sides[n];
    const r = spawnSync('node', [path.join(s.tree, 'tools/browser.mjs'), `--build=${s.build}`, `--query=${opt('query', 'scale=1')}`, `--script=${path.join(here, 'boot.mjs')}`],
      { encoding: 'utf8', timeout: 120000, cwd: s.tree });
    const line = (r.stdout || '').split('\n').find((l) => l.startsWith('RESULT '));
    if (!line) { process.stderr.write(`no result from ${s.tree}: ${r.stdout}\n${r.stderr}\n`); continue; }
    res[n].push(JSON.parse(line.slice(7)));
  }
}
const med = (v) => { const s = [...v].sort((a, b) => a - b); return s.length % 2 ? s[(s.length - 1) / 2] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2; };
const f = (v) => `${Math.min(...v).toFixed(1)} / ${med(v).toFixed(1)}`;
console.log(`boot phases in ms since navigation, best / median of ${runs} runs each, alternating`);
for (let n = 0; n < 2; n++) {
  const r = res[n];
  if (!r.length) { console.log(`  ${'AB'[n]}: no results`); continue; }
  console.log(`  ${'AB'[n]} ${sides[n].tree}`);
  console.log(`     page script ${f(r.map((x) => x.script))}   fetched ${f(r.map((x) => x.fetched))}   kernel up ${f(r.map((x) => x.kernelUp))}   first frame ${f(r.map((x) => x.firstFrame))}`);
  console.log(`     fetch ${f(r.map((x) => x.fetched - x.script))}   kernel start ${f(r.map((x) => x.kernelUp - x.fetched))}   until first frame ${f(r.map((x) => x.firstFrame - x.kernelUp))}`);
  console.log(`     files: ${JSON.stringify(r[r.length - 1].resources)}`);
}
