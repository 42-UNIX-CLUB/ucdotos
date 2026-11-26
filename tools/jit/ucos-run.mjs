// SPDX-License-Identifier: GPL-2.0-or-later
// tools/run.mjs with the translator extension (boot/jit-proto/ext.mjs) switched on.
//   node tools/jit/ucos-run.mjs --build=build-NAME [--image=path/initrd.tar] [--init=/bin/sh] [--args=...]
//        [--input='...'] [--store=FILE] [--no-ext] [--stats] [--debug]
// --no-ext boots exactly like tools/run.mjs (the engine then has no loader and interprets).
// --stats prints one line at the end: what the host compiled for generated code, and the wall time.
import fs from 'node:fs';
import { boot, checkSupport } from '../../boot/host.mjs';
import { openStore } from '../../boot/store.mjs';
import { jitExtension } from '../../boot/jit-proto/ext.mjs';
const root = new URL('../..', import.meta.url).pathname;
const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const missing = checkSupport();
if (missing.length) { console.error('missing: ' + missing.join(', ')); process.exit(2); }
const B = root + opt('build', 'build');
const ext = args.includes('--no-ext') ? null : jitExtension({ debug: args.includes('--debug'), log: (s) => process.stdout.write(s) });
const store = opt('store', null) && await openStore({ kind: 'file', fs, path: opt('store'), log: (s) => process.stdout.write(s) });
const t0 = performance.now();
const done = () => {
  if (args.includes('--stats')) {
    const s = ext ? ext.stats : { attached: 0, loads: 0, bytes: 0, ms: 0, failed: 0 };
    const mu = process.memoryUsage();
    console.log(`@@HOST attached=${s.attached} loads=${s.loads} bytes=${s.bytes} ms=${s.ms.toFixed(2)} failed=${s.failed} wall=${(performance.now() - t0).toFixed(1)} rss_mib=${(mu.rss / 1048576).toFixed(1)} peak_rss_mib=${(process.resourceUsage().maxRSS / 1024).toFixed(1)}`);
  }
  process.exit(0);
};
const sys = await boot({
  kernel: fs.readFileSync(B + '/kernel.wasm'),
  initrd: fs.readFileSync(opt('image', B + '/initrd.tar')),
  cmdline: 'init=' + opt('init', '/bin/init') + (opt('args', '') ? ' -- ' + opt('args', '') : ''),
  screen: { width: 640, height: 400 },
  log: (s) => process.stdout.write(s),
  debug: args.includes('--debug'),
  store,
  extend: ext ? ext.extend : null,
  onIdle: () => store ? store.flush().then(done) : done(),
});
if (sys.rc) process.exit(1);
const input = opt('input', null);
if (input !== null) { sys.consoleInput(input.replace(/\\n/g, '\n')); sys.consoleEOF(); }
else if (!process.stdin.isTTY) { process.stdin.on('data', (d) => sys.consoleInput(d.toString())); process.stdin.on('end', () => sys.consoleEOF()); }
