// SPDX-License-Identifier: GPL-2.0-or-later
// Turn bin/make/tests/os/cases.mk into the in-OS test tree under
// rootfs/usr/share/tests/make/, with expected outputs computed by GNU Make on
// this machine. Run it again after editing cases.mk:
//   node bin/make/tests/os/gen.mjs
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const here = path.dirname(new URL(import.meta.url).pathname);
const repo = path.resolve(here, '../../../..');
const out = path.join(repo, 'rootfs/usr/share/tests/make');
const gnu = spawnSync('sh', ['-c', 'command -v make'], { encoding: 'utf8' }).stdout.trim();
if (!gnu || !/^GNU Make 4\./.test(spawnSync(gnu, ['--version'], { encoding: 'utf8' }).stdout)) {
  console.error('gen.mjs needs GNU Make 4.x to compute the expected outputs');
  process.exit(2);
}

const text = fs.readFileSync(path.join(here, 'cases.mk'), 'utf8');
const cases = [];
let cur = null, sec = null;
for (const line of text.split('\n')) {
  let m;
  if ((m = /^##### (.*)$/.exec(line))) { cur = { name: m[1].trim(), files: [], run: [] }; cases.push(cur); sec = null; continue; }
  if (!cur) continue;
  if ((m = /^--- (\S+)(?: (.*))?$/.exec(line))) {
    if (m[1] === 'Makefile') sec = { name: 'Makefile', lines: [] }, cur.files.push(sec);
    else if (m[1] === 'file') sec = { name: m[2], lines: [] }, cur.files.push(sec);
    else if (m[1] === 'run') sec = 'run';
    continue;
  }
  if (sec === 'run') { if (line.trim()) cur.run.push(line); }
  else if (sec) sec.lines.push(line);
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mkos-'));
const bin = path.join(tmp, 'bin');
fs.mkdirSync(bin);
fs.symlinkSync(gnu, path.join(bin, 'make'));
for (const d of ['cases', 'cmd', 'expected']) fs.mkdirSync(path.join(out, d), { recursive: true });
const names = [];
for (const c of cases) {
  names.push(c.name);
  const cdir = path.join(out, 'cases', c.name);
  const wdir = path.join(tmp, 'w', c.name);
  for (const f of c.files) {
    while (f.lines.length && f.lines[f.lines.length - 1] === '') f.lines.pop();
    const body = f.lines.join('\n') + '\n';
    for (const d of [cdir, wdir]) {
      fs.mkdirSync(path.dirname(path.join(d, f.name)), { recursive: true });
      fs.writeFileSync(path.join(d, f.name), body);
    }
  }
  fs.mkdirSync(wdir, { recursive: true });
  fs.mkdirSync(cdir, { recursive: true });
  const cmd = c.run.join('\n') + '\n';
  fs.writeFileSync(path.join(out, 'cmd', c.name), cmd);
  // the OS runner sends both streams to one file, in order
  const m = spawnSync('/bin/sh', ['-c', `exec 2>&1\n${cmd}`], {
    cwd: wdir, encoding: 'utf8', env: { PATH: `${bin}:/usr/bin:/bin`, LC_ALL: 'C', HOME: wdir },
  });
  fs.writeFileSync(path.join(out, 'expected', c.name), m.stdout);
}
fs.writeFileSync(path.join(out, 'cases.list'), names.join('\n') + '\n');
fs.rmSync(tmp, { recursive: true, force: true });
console.log(`wrote ${names.length} cases to ${path.relative(repo, out)}`);
