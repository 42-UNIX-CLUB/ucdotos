// SPDX-License-Identifier: GPL-2.0-or-later
// Crash points: run a workload against a fresh store and stop the machine in the middle of
// storage write N (half of that write reaches the file), for many N; then boot again on
// what is left and check it with persisttest crash-check: the tree must be exactly the
// state after some change at or after the last completed sync. Also a crash in the middle
// of one large file.
//   node tools/tests/persist-crash.mjs [--build=build] [--points=1,2,3,...] [--ops=200] [--seed=7]
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const root = new URL('../..', import.meta.url).pathname;
const opt = (n, d) => { const a = process.argv.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const build = opt('build', 'build'), B = path.join(root, build);
const ops = opt('ops', '200'), seed = opt('seed', '7');
const work = path.join(B, 'persist-test');
fs.mkdirSync(work, { recursive: true });
const store = path.join(work, 'crash.store');
function run(init, args, crash) {
  const a = [path.join(root, 'tools/run.mjs'), `--build=${build}`, `--store=${store}`, `--init=${init}`, `--args=${args}`];
  if (crash) a.push(`--store-crash=${crash}`);
  const r = spawnSync('node', a, { encoding: 'utf8', maxBuffer: 1 << 26 });
  return (r.stdout || '') + (r.stderr || '');
}
// how many storage writes does the whole run make?
fs.rmSync(store, { force: true });
let out = run('/bin/persisttest', `crash-run ${seed} ${ops} /home/user/cr`, 1e9);
const total = (() => { const m = out.match(/stopped at storage write (\d+)/); return m ? Number(m[1]) : null; })();
const points = opt('points', null) ? opt('points').split(',').map(Number) : null;
let pass = 0, fail = 0, n = 0;
const list = points || (() => { const v = [], step = Math.max(1, Math.floor(4000 / 40)); for (let p = 1; p <= 4000; p += step) v.push(p); return v; })();
for (const p of list) {
  fs.rmSync(store, { force: true });
  out = run('/bin/persisttest', `crash-run ${seed} ${ops} /home/user/cr`, p);
  const crashed = /stopped at storage write/.test(out);
  const synced = [...out.matchAll(/^synced (\d+)$/gm)].map((m) => Number(m[1]));
  const k = synced.length ? synced[synced.length - 1] : -1;
  if (!crashed) { console.log(`  point ${p}: the run finished before ${p} writes; stopping here`); break; }
  // a sync that printed "synced K" returned before the crash: K must be there. Before the
  // first sync returned, nothing is promised: check against any state from change 0.
  const c = run('/bin/persisttest', `crash-check ${seed} ${ops} /home/user/cr ${Math.max(k, 0)}`);
  const ok = /crash-check: PASS/.test(c);
  ok ? pass++ : fail++;
  n++;
  console.log(`  ${ok ? 'ok  ' : 'FAIL'}  crash at storage write ${p} (last sync: ${k}): ${(c.match(/crash-check: .*/) || ['no result: ' + c.slice(-400)])[0]}`);
}
// one big file, stopped in the middle
fs.rmSync(store, { force: true });
out = run('/bin/persisttest', 'bigwrite /home/user/big 48', Number(opt('bigpoint', '40')));
const wrote = (out.match(/wrote (\d+) MiB/g) || []).pop();
const c = run('/bin/persisttest', 'bigcheck /home/user/big');
const bigok = /PASS/.test(c);
bigok ? pass++ : fail++;
console.log(`  ${bigok ? 'ok  ' : 'FAIL'}  crash during a 48 MiB write (${wrote || 'nothing'} written by then): ${(c.match(/bigcheck: .*/) || [c.slice(-300)])[0]}`);
console.log(`persist-crash: ${pass + fail} checks, ${fail} failed${total ? ` (the full run makes ${total} storage writes)` : ''}`);
process.exit(fail ? 1 : 0);
