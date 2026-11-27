// SPDX-License-Identifier: GPL-2.0-or-later
// A full store under Node: the file backend is given an artificial quota (run.mjs --quota),
// a program writes until the store says no (ENOSPC), checks the warnings and statfs, deletes
// files and writes again; a second boot checks that what fsync acknowledged survived.
//   node tools/tests/store-full.mjs [--build=build] [--quota=4194304]
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const root = new URL('../..', import.meta.url).pathname;
const opt = (n, d) => { const a = process.argv.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const build = opt('build', 'build'), quota = opt('quota', String(4 << 20));
const work = path.join(root, build, 'store-full-test');
fs.mkdirSync(work, { recursive: true });
const store = path.join(work, 'full.store');
fs.rmSync(store, { force: true }); // a single file this test made
let checks = 0, failed = 0;
const check = (name, ok, extra = '') => { checks++; if (!ok) failed++; console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${name}${extra ? '  ' + extra : ''}`); };
function run(args, extra = []) {
  const r = spawnSync('node', [path.join(root, 'tools/run.mjs'), `--build=${build}`, `--store=${store}`, `--quota=${quota}`, ...extra, '--init=/bin/persisttest', `--args=${args}`],
    { encoding: 'utf8', maxBuffer: 1 << 26, timeout: 60000 });
  return (r.stdout || '') + (r.stderr || '');
}
console.log(`store-full: quota ${quota} bytes, store ${store}`);
let out = run('full /home/user/sf');
for (const l of out.split('\n')) if (/FAIL|statfs|full after|level:|after deleting|\[store\]/.test(l)) console.log('    ' + l);
check('boot 1: fill until ENOSPC, warnings, recovery after deleting', /persisttest: full \d+ checks, 0 failed/.test(out));
check('the kernel said so on the console at 80%/95% and when full', /\[store\] browser storage is (80%|95%) full/.test(out) || /no room for more data/.test(out));
out = run('fullcheck /home/user/sf');
for (const l of out.split('\n')) if (/FAIL|persisttest/.test(l)) console.log('    ' + l);
check('boot 2: fsync-acknowledged data intact, deletions kept', /persisttest: fullcheck \d+ checks, 0 failed/.test(out));
// the same when the browser's figures are wrong: the kernel thinks there is room (it is told
// a quota four times larger), the batch is refused, the store goes "full" and keeps the changes
fs.rmSync(store, { force: true });
console.log('store-full: the browser refuses a batch the kernel thought would fit');
out = run('full /home/user/sf', [`--quota-report=${Number(quota) * 4}`]);
for (const l of out.split('\n')) if (/FAIL|full after|level:|after deleting|\[store\]/.test(l)) console.log('    ' + l);
check('boot 3: the refused batch makes the store full: ENOSPC, warnings, recovery after deleting', /persisttest: full \d+ checks, 0 failed/.test(out));
check('the kernel said the browser had no room', /no room for more data/.test(out));
out = run('fullcheck /home/user/sf', [`--quota-report=${Number(quota) * 4}`]);
for (const l of out.split('\n')) if (/FAIL|persisttest/.test(l)) console.log('    ' + l);
check('boot 4: fsync-acknowledged data intact, deletions kept', /persisttest: fullcheck \d+ checks, 0 failed/.test(out));
console.log(`store-full: ${checks} checks, ${failed} failed`);
process.exit(failed ? 1 : 0);
