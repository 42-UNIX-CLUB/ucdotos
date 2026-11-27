// SPDX-License-Identifier: GPL-2.0-or-later
// Boot the system under Node, with the console on stdin/stdout. Used for tests.
//   node tools/run.mjs [--build=build] [--init=/bin/ktest] [--debug] [--input='text typed on the console']
//                      [--audio-capture=FILE]   save what /dev/dsp plays (raw 16-bit stereo, 48 kHz)
//                      [--remote-rate=BYTES_PER_SECOND] [--remote-off]   (remote files: tools/remote-node.mjs)
//                      [--net-test=http://127.0.0.1:PORT]   the router's host in test mode (boot/hostnet.mjs)
//   --admin: this session is entitled to administration (boot parameter ucos.admin=1: su, sudo, set-user-ID)
//   --platform=http://127.0.0.1:PORT --platform-cookie='name=value' [--platform-member] [--platform-proxy-first]:
//     the router's ways out through a running platform (tools/tests/platform.mjs); the host then has no sockets of its own
//   --quota=BYTES: with --store, the store behaves as if the browser gave it this much room (tests of a full store)
//   --quota-report=BYTES: ...but tells the kernel this larger figure (the browser refuses a batch the kernel thought fit)
import fs from 'node:fs';
import { boot, checkSupport } from '../boot/host.mjs';
import { openStore } from '../boot/store.mjs';
import { nodeRemote } from './remote-node.mjs';
const root = new URL('..', import.meta.url).pathname;
const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const missing = checkSupport();
if (missing.length) { console.error('missing: ' + missing.join(', ')); process.exit(2); }
// --store=FILE keeps the file system in FILE across runs (boot/store.mjs); --store-crash=N stops after N storage writes
const store = opt('store', null) && await openStore({ kind: 'file', fs, path: opt('store'), crashAfter: Number(opt('store-crash', 0)), quota: Number(opt('quota', 0)), reportQuota: Number(opt('quota-report', 0)), log: (s) => process.stdout.write(s) });
const sys = await boot({
  kernel: fs.readFileSync(root + opt('build', 'build') + '/kernel.wasm'),
  initrd: fs.readFileSync(root + opt('build', 'build') + '/initrd.tar'),
  cmdline: 'init=' + opt('init', '/bin/init') + (args.includes('--admin') ? ' ucos.admin=1' : '') + (opt('args', '') ? ' -- ' + opt('args', '') : ''),
  screen: { width: 640, height: 400 },
  log: (s) => process.stdout.write(s),
  debug: args.includes('--debug'),
  remote: nodeRemote(root + opt('build', 'build'), (s) => process.stdout.write(s), { rate: Number(opt('remote-rate', 0)), off: args.includes('--remote-off') }), // remote files: read from the build directory
  store, storeFlags: Number(opt('store-flags', 0)),
  onIdle: () => store ? store.flush().then(() => process.exit(0)) : process.exit(0),
  sound: { capture: opt('audio-capture', null) },
  net: opt('net-test', null) ? { test: opt('net-test', null) }
    : opt('platform', null) ? { platform: { base: opt('platform', null), cookie: opt('platform-cookie', ''), signedIn: true, asBrowser: true,
                                            member: args.includes('--platform-member'), proxyFirst: args.includes('--platform-proxy-first') } } : {},
});
if (sys.rc) process.exit(1);
const input = opt('input', null);
if (input !== null) {
  // the kernel's console holds 1024 bytes of input and drops what does not fit (kernel/dev.c)
  if (Buffer.byteLength(input.replace(/\\n/g, '\n')) > 1024) { console.error('run.mjs: --input is over 1024 bytes, more than the console holds at once: the rest would be lost'); process.exit(2); }
  sys.consoleInput(input.replace(/\\n/g, '\n')); sys.consoleEOF();
}
else if (!process.stdin.isTTY) { process.stdin.on('data', (d) => sys.consoleInput(d.toString())); process.stdin.on('end', () => sys.consoleEOF()); }
