// SPDX-License-Identifier: GPL-2.0-or-later
// Fork and the file system's persistence together. Two boots of tools/run.mjs with the same
// --store, as two loads of the page would be:
//   boot 1: a forked child makes a tree (file, symbolic link, hard link, modes, directory);
//           clone and migrate run while the store is active; a program is copied into the
//           stored part of the tree.
//   boot 2: the tree is back with every attribute; the copied program (its contents now
//           come from the store) is cloned and migrated.
//   node tools/tests/integ-store.mjs [--build=build]
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const root = new URL('../..', import.meta.url).pathname;
const opt = (n, d) => { const a = process.argv.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const build = opt('build', 'build');
const work = path.join(root, build, 'integ-store');
fs.mkdirSync(work, { recursive: true });
const store = path.join(work, 'test.store');
fs.rmSync(store, { force: true }); // a single file this test made

let checks = 0, failed = 0;
const check = (name, ok, info = '') => { checks++; if (!ok) failed++; console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${name}${info ? '  ' + info : ''}`); };
function boot(input) {
  const t0 = Date.now();
  const r = spawnSync('node', [path.join(root, 'tools/run.mjs'), `--build=${build}`, `--store=${store}`, '--init=/bin/sh', `--input=${input}`], { encoding: 'utf8', timeout: 60000 });
  return { out: (r.stdout || '') + (r.stderr || ''), ms: Date.now() - t0 };
}

let r = boot('forktest x-tree /home/user/ft\\necho tree=$?\\nclonetest basic\\nclonetest migrate 64 1\\n' +
  'forktest x-treecheck /home/user/ft\\necho check=$?\\ncp /bin/clonetest /home/user/ct\\nexit\\n');
check('boot 1: a forked child made the tree', /tree=0/.test(r.out));
check('boot 1: clone with the store active', /child r0=0 r1=41 r2=1/.test(r.out) && /parent r0=pid r1=41 r2=7/.test(r.out));
check('boot 1: migrate with the store active', /migrated: state intact/.test(r.out));
check('boot 1: the parent sees the whole tree', /check=0/.test(r.out));
if (failed) console.log(r.out.slice(-2000));
console.log(`  info  store file ${fs.statSync(store).size} bytes after boot 1 (${r.ms} ms)`);

r = boot('forktest x-treecheck /home/user/ft\\necho check2=$?\\n/home/user/ct basic\\n/home/user/ct migrate 64 1\\ncat /proc/ucos-store\\nexit\\n');
check('boot 2: the tree made by the forked child is back, every attribute', /check2=0/.test(r.out));
check('boot 2: a program whose contents come from the store clones', /child r0=0 r1=41 r2=1/.test(r.out) && /parent r0=pid r1=41 r2=7/.test(r.out));
check('boot 2: ...and migrates', /migrated: state intact/.test(r.out));
const m = /\[store\] (\d+) records applied in ([\d.]+) ms/.exec(r.out);
console.log(`  info  ${m ? m[0] : 'no store line'}; boot 2 took ${r.ms} ms`);
if (failed) console.log(r.out.slice(-2000));
console.log(`integ-store: ${checks} checks, ${failed} failed`);
process.exit(failed ? 1 : 0);
