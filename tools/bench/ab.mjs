// SPDX-License-Identifier: GPL-2.0-or-later
// A/B benchmark driver. Runs a benchmark program from two source trees alternately
// (A, B, A, B, ...) so that both see the same machine load, then prints the best,
// the median and the spread of every result row for each side.
//
//   node tools/bench/ab.mjs --a=TREE[:BUILD] --b=TREE[:BUILD] [--runs=10] [--mode=node|browser]
//                           [--prog=/bin/sysbench] [--args=--json] [--query=scale=1] [--only=name,name]
//
// TREE is a source tree (it must contain tools/run.mjs and BUILD/kernel.wasm; for --mode=browser
// also BUILD/dist). BUILD defaults to "build". The program must print JSON with a "results"
// array of {name, unit, value} (sysbench --json and ubench --json do). Each tree's own runner
// and boot/host.mjs are used, so host changes are part of what is compared.
import { spawnSync } from 'node:child_process';
import path from 'node:path';

const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const here = path.dirname(new URL(import.meta.url).pathname);
const side = (s) => { const [tree, build = 'build'] = s.split(':'); return { tree: path.resolve(tree), build }; };
const A = side(opt('a', '.')), B = side(opt('b', '.'));
const runs = Number(opt('runs', 10));
const mode = opt('mode', 'node');
const prog = opt('prog', '/bin/sysbench');
const pargs = opt('args', '--json');
const only = opt('only', '') ? opt('only', '').split(',') : null;

function runOnce(s) {
  let r;
  if (mode === 'node') {
    r = spawnSync('node', [path.join(s.tree, 'tools/run.mjs'), `--build=${s.build}`, `--init=${prog}`, `--args=${pargs}`], { encoding: 'utf8', timeout: 600000 });
  } else {
    const q = `${opt('query', 'scale=1')}&cmdline=${encodeURIComponent(`init=${prog} -- ${pargs}`)}`;
    r = spawnSync('node', [path.join(s.tree, 'tools/browser.mjs'), `--build=${s.build}`, `--query=${q}`, `--script=${path.join(here, 'page-log.mjs')}`],
      { encoding: 'utf8', timeout: 600000, cwd: s.tree });
  }
  const out = r.stdout || '';
  const i = out.indexOf('{'), j = out.lastIndexOf('}');
  if (i < 0 || j < 0) throw new Error(`no JSON from ${s.tree}:\n${out}\n${r.stderr}`);
  return JSON.parse(out.slice(i, j + 1)).results;
}

const data = [{}, {}];
const retry = (s) => { try { return runOnce(s); } catch (e) { process.stderr.write(`retrying after: ${e.message.slice(0, 300)}\n`); return runOnce(s); } };
for (let k = 0; k < runs; k++) {
  for (const [n, s] of [[0, A], [1, B]]) {
    for (const row of retry(s)) {
      if (only && !only.includes(row.name)) continue;
      (data[n][row.name] ||= { unit: row.unit, v: [] }).v.push(row.value);
    }
  }
  process.stderr.write(`run ${k + 1}/${runs}\n`);
}
const stat = (v) => { const s = [...v].sort((x, y) => x - y); return { best: s[0], med: s.length % 2 ? s[(s.length - 1) / 2] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2, worst: s[s.length - 1] }; };
const f = (x) => (x >= 100 ? x.toFixed(0) : x >= 10 ? x.toFixed(1) : x.toFixed(2));
console.log(`mode ${mode}, ${runs} runs each, alternating. A=${A.tree}:${A.build}  B=${B.tree}:${B.build}`);
console.log('row'.padEnd(16) + 'unit  ' + 'A best'.padStart(8) + 'A med'.padStart(8) + 'A worst'.padStart(9) + '  |' + 'B best'.padStart(8) + 'B med'.padStart(8) + 'B worst'.padStart(9) + '  med B/A');
for (const name of Object.keys(data[0])) {
  if (!data[1][name]) continue;
  const a = stat(data[0][name].v), b = stat(data[1][name].v);
  console.log(name.padEnd(16) + data[0][name].unit.padEnd(6) + f(a.best).padStart(8) + f(a.med).padStart(8) + f(a.worst).padStart(9) + '  |' +
    f(b.best).padStart(8) + f(b.med).padStart(8) + f(b.worst).padStart(9) + ('  ' + (b.med / a.med).toFixed(3)));
}
if (args.includes('--raw')) console.log(JSON.stringify(data));
