// SPDX-License-Identifier: GPL-2.0-or-later
// The boot image loader: symbolic links, hard links, modes, owners and times found in the
// tar must arrive as such. Appends entries made by the host's tar to a copy of the build's
// initrd, boots it under Node with /bin/sh, and compares what the shell sees.
//   node tools/tests/vfs-tar.mjs [--build=build]
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { boot } from '../../boot/host.mjs';

const root = new URL('../..', import.meta.url).pathname;
const arg = process.argv.find((a) => a.startsWith('--build='));
const B = path.join(root, arg ? arg.slice(8) : 'build');
const work = path.join(B, 'vfs-tar');
const src = path.join(work, 'src');
fs.mkdirSync(path.join(src, 'vt/dir'), { recursive: true });
const p = (...a) => path.join(src, ...a);
for (const f of ['vt/file', 'vt/link', 'vt/hard', 'vt/abs', 'vt/dirlink', 'vt/suid']) fs.rmSync(p(f), { force: true });
fs.writeFileSync(p('vt/file'), 'tar data\n');
fs.symlinkSync('file', p('vt/link'));
fs.symlinkSync('/vt/dir', p('vt/dirlink'));
fs.linkSync(p('vt/file'), p('vt/hard'));
fs.writeFileSync(p('vt/suid'), 'x');
fs.chmodSync(p('vt/suid'), 0o4751);
fs.chmodSync(p('vt/dir'), 0o750);
fs.utimesSync(p('vt/file'), 1000000000, 1000000000);
const tarPath = path.join(work, 'initrd.tar');
fs.copyFileSync(path.join(B, 'initrd.tar'), tarPath);
fs.copyFileSync(path.join(B, 'kernel.wasm'), path.join(work, 'kernel.wasm'));
// GNU tar writes "file" first and "hard" as a type-1 entry pointing at it.
execFileSync('tar', ['--format=ustar', '--owner=123', '--group=45', '-C', src, '-rf', tarPath, 'vt/dir', 'vt/file', 'vt/hard', 'vt/link', 'vt/dirlink', 'vt/suid']);
const listing = execFileSync('tar', ['-tvf', tarPath, 'vt']).toString();
if (!/^h/m.test(listing) || !/^l/m.test(listing)) { console.log('host tar did not write a hard link and a symlink:\n' + listing); process.exit(2); }

const script = [
  'cd /vt',
  'stat -c "%n %F %a %u %g %h" file hard link dirlink dir suid',
  'readlink link dirlink',
  'cat link',
  'stat -c "%i" file hard',
  'stat -c "%Y" file',
  'ls dirlink/..',
  'exit',
].join('\n') + '\n';
let out = '';
const sys = await boot({
  kernel: fs.readFileSync(path.join(work, 'kernel.wasm')),
  initrd: fs.readFileSync(tarPath),
  cmdline: 'init=/bin/sh',
  log: (s) => { out += s; },
  onIdle: () => finish(),
});
sys.consoleInput(script);
sys.consoleEOF();
let checks = 0, failed = 0;
function check(name, ok) { checks++; if (!ok) failed++; console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${name}`); }
function finish() {
  const lines = out.split('\n');
  const has = (s) => lines.some((l) => l.trim() === s);
  check('a regular file keeps mode, owner and group', has('file regular file 644 123 45 2'));
  check('a hard link (type 1) shares the inode: link count 2', has('hard regular file 644 123 45 2'));
  check('a symbolic link (type 2)', has('link symbolic link 777 123 45 1'));
  check('a link to a directory', has('dirlink symbolic link 777 123 45 1'));
  check('a directory keeps its mode 750', has('dir directory 750 123 45 2'));
  check('set-user-ID and an odd mode survive', has('suid regular file 4751 123 45 1'));
  check('readlink gives the stored targets', has('file') && has('/vt/dir'));
  check('reading through the link', has('tar data'));
  const inos = lines.filter((l) => /^\d+$/.test(l.trim()));
  check('both names have the same inode number', inos.length >= 2 && inos[0] === inos[1]);
  check('the modification time from the tar', has('1000000000'));
  check('".." through the directory link is /vt', has('dir') && has('suid'));
  console.log(`vfs-tar: ${checks} checks, ${failed} failed`);
  if (failed) console.log(out);
  process.exit(failed ? 1 : 0);
}
