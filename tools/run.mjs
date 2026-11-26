// SPDX-License-Identifier: GPL-2.0-or-later
// Boot the system under Node, with the console on stdin/stdout. Used for tests.
//   node tools/run.mjs [--build=build] [--init=/bin/ktest] [--debug] [--input='text typed on the console']
import fs from 'node:fs';
import { boot, checkSupport } from '../boot/host.mjs';
import { openStore } from '../boot/store.mjs';
const root = new URL('..', import.meta.url).pathname;
const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const missing = checkSupport();
if (missing.length) { console.error('missing: ' + missing.join(', ')); process.exit(2); }
// --store=FILE keeps the file system in FILE across runs (boot/store.mjs); --store-crash=N stops after N storage writes
const store = opt('store', null) && await openStore({ kind: 'file', fs, path: opt('store'), crashAfter: Number(opt('store-crash', 0)), log: (s) => process.stdout.write(s) });
const sys = await boot({
  kernel: fs.readFileSync(root + opt('build', 'build') + '/kernel.wasm'),
  initrd: fs.readFileSync(root + opt('build', 'build') + '/initrd.tar'),
  cmdline: 'init=' + opt('init', '/bin/init') + (opt('args', '') ? ' -- ' + opt('args', '') : ''),
  screen: { width: 640, height: 400 },
  log: (s) => process.stdout.write(s),
  debug: args.includes('--debug'),
  store, storeFlags: Number(opt('store-flags', 0)),
  onIdle: () => store ? store.flush().then(() => process.exit(0)) : process.exit(0),
});
if (sys.rc) process.exit(1);
const input = opt('input', null);
if (input !== null) { sys.consoleInput(input.replace(/\\n/g, '\n')); sys.consoleEOF(); }
else if (!process.stdin.isTTY) { process.stdin.on('data', (d) => sys.consoleInput(d.toString())); process.stdin.on('end', () => sys.consoleEOF()); }
