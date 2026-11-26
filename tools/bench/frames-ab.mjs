// SPDX-License-Identifier: GPL-2.0-or-later
// Runs tools/bench/frames.mjs in Chromium for two trees alternately and prints the page's
// time per presented frame (best and median of the per-run averages).
//   node tools/bench/frames-ab.mjs --a=TREE[:BUILD] --b=TREE[:BUILD] [--runs=5] [--scenario=type] [--scale=1]
import { spawnSync } from 'node:child_process';
import path from 'node:path';

const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const here = path.dirname(new URL(import.meta.url).pathname);
const side = (s) => { const [tree, build = 'build'] = s.split(':'); return { tree: path.resolve(tree), build }; };
const sides = [side(opt('a', '.')), side(opt('b', '.'))];
const runs = Number(opt('runs', 5)), scenario = opt('scenario', 'type'), scale = opt('scale', '1');
const query = `scale=${scale}&stats` + (scenario === 'full' ? `&cmdline=${encodeURIComponent('init=/bin/fbdemo')}` : '');
const res = [[], []];
for (let k = 0; k < runs; k++) {
  for (let n = 0; n < 2; n++) {
    const s = sides[n];
    const r = spawnSync('node', [path.join(s.tree, 'tools/browser.mjs'), `--build=${s.build}`, `--query=${query}`, `--script=${path.join(here, 'frames.mjs')}`],
      { encoding: 'utf8', timeout: 120000, cwd: s.tree, env: { ...process.env, SCENARIO: scenario, SHOT: k === 0 ? `frames-${scenario}-x${scale}` : '' } });
    const line = (r.stdout || '').split('\n').find((l) => l.startsWith('RESULT '));
    if (!line) { process.stderr.write(`no result from ${s.tree}: ${r.stdout}\n${r.stderr}\n`); continue; }
    res[n].push(JSON.parse(line.slice(7)));
  }
}
const med = (v) => { const s = [...v].sort((a, b) => a - b); return s.length % 2 ? s[(s.length - 1) / 2] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2; };
console.log(`scenario ${scenario}, scale ${scale}, ${runs} runs each, alternating`);
for (let n = 0; n < 2; n++) {
  const r = res[n];
  if (!r.length) { console.log(`  ${'AB'[n]}: no results`); continue; }
  const avg = r.map((x) => x.avgMs);
  console.log(`  ${'AB'[n]} ${sides[n].tree}: ms per frame best ${Math.min(...avg).toFixed(3)} median ${med(avg).toFixed(3)} worst ${Math.max(...avg).toFixed(3)}; ` +
    `max single frame ${med(r.map((x) => x.maxMs)).toFixed(2)} (median of runs); frames per run ${med(r.map((x) => x.frames))}; pixels per frame ${Math.round(med(r.map((x) => x.avgPixels)))}`);
}
