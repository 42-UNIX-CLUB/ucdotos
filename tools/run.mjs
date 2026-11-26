// SPDX-License-Identifier: GPL-2.0-or-later
// Boot the system under Node, with the console on stdin/stdout. Used for tests.
//   node tools/run.mjs [--build=build] [--init=/bin/ktest] [--debug] [--input='text typed on the console']
//                      [--net-test=http://127.0.0.1:PORT]   the router's host in test mode (boot/hostnet.mjs)
import fs from 'node:fs';
import { boot, checkSupport } from '../boot/host.mjs';
const root = new URL('..', import.meta.url).pathname;
const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const missing = checkSupport();
if (missing.length) { console.error('missing: ' + missing.join(', ')); process.exit(2); }
const sys = await boot({
  kernel: fs.readFileSync(root + opt('build', 'build') + '/kernel.wasm'),
  initrd: fs.readFileSync(root + opt('build', 'build') + '/initrd.tar'),
  cmdline: 'init=' + opt('init', '/bin/init') + (opt('args', '') ? ' -- ' + opt('args', '') : ''),
  screen: { width: 640, height: 400 },
  log: (s) => process.stdout.write(s),
  debug: args.includes('--debug'),
  onIdle: () => process.exit(0),
  net: opt('net-test', null) ? { test: opt('net-test', null) } : {},
});
if (sys.rc) process.exit(1);
const input = opt('input', null);
if (input !== null) { sys.consoleInput(input.replace(/\\n/g, '\n')); sys.consoleEOF(); }
else if (!process.stdin.isTTY) { process.stdin.on('data', (d) => sys.consoleInput(d.toString())); process.stdin.on('end', () => sys.consoleEOF()); }
