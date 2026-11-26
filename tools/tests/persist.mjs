// SPDX-License-Identifier: GPL-2.0-or-later
// Persistence under Node: fill a tree with every kind of object, reboot, verify it byte for
// byte and attribute for attribute, reboot again, then boot a changed image over the same
// store (the upgrade case). Each boot is a separate Node process running tools/run.mjs
// with --store, as a reload of the page would be.
//   node tools/tests/persist.mjs [--build=build] [--small=20000] [--big=50]
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';

const root = new URL('../..', import.meta.url).pathname;
const opt = (n, d) => { const a = process.argv.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const build = opt('build', 'build'), B = path.join(root, build);
const small = opt('small', '20000'), big = opt('big', '50');
const work = path.join(B, 'persist-test');
fs.mkdirSync(work, { recursive: true });
const store = path.join(work, 'test.store');
fs.rmSync(store, { force: true }); // a single file this test made
let checks = 0, failed = 0;
const check = (name, ok, extra = '') => { checks++; if (!ok) failed++; console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${name}${extra ? '  ' + extra : ''}`); };
function run(init, args, b = build, input) {
  const t0 = performance.now();
  const a = [path.join(root, 'tools/run.mjs'), `--build=${b}`, `--store=${store}`, `--init=${init}`];
  if (args) a.push(`--args=${args}`);
  if (input) a.push(`--input=${input}`);
  const r = spawnSync('node', a, { encoding: 'utf8', maxBuffer: 1 << 26 });
  const out = (r.stdout || '') + (r.stderr || '');
  return { out, ms: performance.now() - t0, status: r.status };
}
const applied = (out) => { const m = /\[store\] (\d+) records applied in ([\d.]+) ms/.exec(out); return m ? `${m[1]} records applied in ${m[2]} ms` : 'no store line'; };

console.log(`persist: ${small} small files, a ${big} MiB file, store ${store}`);
let r = run('/bin/persisttest', `fill /home/user/pt ${small} ${big}`);
const filled = /persisttest: filled/.test(r.out);
check('boot 1: fill and sync', filled, `${(r.ms / 1000).toFixed(1)} s for the whole boot; ${(r.out.match(/sync took (\d+) ms/) || [])[0] || ''}`);
if (!filled) console.log(r.out.slice(-3000));
const size = fs.statSync(store).size;
const logical = Number(small) * (16 + 99.5) + Number(big) * 1048576 + 1.4e6;
console.log(`  info  store file ${(size / 1048576).toFixed(1)} MiB for about ${(logical / 1048576).toFixed(1)} MiB of file data`);
r = run('/bin/persisttest', `verify /home/user/pt ${small} ${big}`);
check('boot 2: everything is back', /verify: \d+ checks, 0 failed/.test(r.out), applied(r.out) + `, boot and verify ${(r.ms / 1000).toFixed(1)} s`);
if (!/0 failed/.test(r.out)) console.log(r.out.slice(-3000));
r = run('/bin/sh', null, build, 'rm -r /home/user/pt/many\\necho changed > /etc/motd\\nrm /etc/profile\\nmv /etc/welcome /home/user/welcome.moved\\nexit\\n');
r = run('/bin/sh', null, build, 'ls /home/user/pt/many; cat /etc/motd; ls /etc; ls /home/user\\nexit\\n');
check('boot 4: a removed tree stays removed', /cannot open directory|No such file/.test(r.out));
check('... an image file changed by the user keeps the change', /^changed$/m.test(r.out));
check('... a deleted image file stays deleted (whiteout)', !/\bprofile\b/.test(r.out));
check('... a moved image file is at its new name only', /welcome\.moved/.test(r.out) && !/^welcome$/m.test(r.out));
r = run('/bin/persisttest', `verify /home/user/pt 0 ${big}`);
{ /* expected failures: the removed directory, and the link count of the file one of whose links was in it */
  const fl = r.out.split('\n').filter((l) => /^  FAIL/.test(l));
  const other = fl.filter((l) => !/the big directory|hard links: one inode/.test(l));
  check('boot 5: the rest of the tree is intact', fl.length === 2 && !other.length, fl.length + ' failures as expected for the removed directory' + (other.length ? '; unexpected: ' + other.join(' | ') : '')); }

// The upgrade: a newer image changes /etc/motd (which the user changed), changes
// /etc/profile (deleted by the user) and adds a new file.
const up = path.join(work, 'upgrade');
fs.mkdirSync(path.join(up, 'src/etc'), { recursive: true });
fs.copyFileSync(path.join(B, 'kernel.wasm'), path.join(up, 'kernel.wasm'));
fs.copyFileSync(path.join(B, 'initrd.tar'), path.join(up, 'initrd.tar'));
fs.writeFileSync(path.join(up, 'src/etc/motd'), 'motd from the new image\n');
fs.writeFileSync(path.join(up, 'src/etc/profile'), 'profile from the new image\n');
fs.writeFileSync(path.join(up, 'src/etc/new-in-image'), 'new\n');
fs.writeFileSync(path.join(up, 'src/etc/hostname-image'), 'untouched by the user\n');
execFileSync('tar', ['--format=ustar', '--owner=0', '--group=0', '-C', path.join(up, 'src'), '-rf', path.join(up, 'initrd.tar'), 'etc/motd', 'etc/profile', 'etc/new-in-image', 'etc/hostname-image']);
r = run('/bin/sh', null, path.relative(root, up), 'cat /etc/motd /etc/new-in-image /etc/hostname-image; ls /etc\\nexit\\n');
check('upgrade: the user\'s version of a changed file wins', /^changed$/m.test(r.out) && !/motd from the new image/.test(r.out));
check('upgrade: a new image file appears', /^new$/m.test(r.out));
check('upgrade: an image file the user never touched comes from the new image', /untouched by the user/.test(r.out));
check('upgrade: a file the user deleted stays deleted', !/profile from the new image/.test(r.out) && !/\bprofile\b/.test(r.out));
console.log(`persist: ${checks} checks, ${failed} failed`);
process.exit(failed ? 1 : 0);
