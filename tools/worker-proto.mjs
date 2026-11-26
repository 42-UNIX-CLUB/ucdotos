// SPDX-License-Identifier: GPL-2.0-or-later
// Worker-process prototype runner (docs/WORKER-PROCESSES.md). Two modes:
//
// Under Node, like tools/run.mjs but with the worker extension switched on:
//   node tools/worker-proto.mjs --build=build-NAME/proto/run --init=/bin/sh --input='wproto-b basic\nexit\n'
//     [--check]        approach B: report any kernel read the marshalling table did not send
//     [--drop=NR,...]  approach B, test hook: leave these calls out of the table (to test --check)
//     [--spin-a=N]     approach A: the worker spins N times before Atomics.wait
//     [--main-spin=US] approach A: the main thread spins up to US microseconds before waitAsync
//     [--debug] [--args='...'] [--quiet]
//
// In headless Chromium (needs make -f tools/worker-proto/proto.mk B=build-NAME first):
//   node tools/worker-proto.mjs --browser --build=build-NAME [--test=bench|desktop|d|terminate] [--no-coi] [--query='cmd=...']
// It serves build-NAME/proto/dist itself, with or without the two cross-origin isolation
// headers, opens boot/worker-proto/test.html (or the desktop) and prints what the page reports.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { spawn } from 'node:child_process';

const root = new URL('..', import.meta.url).pathname;
const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const flag = (n) => args.includes('--' + n);

if (!flag('browser')) {
  const { boot, checkSupport } = await import('../boot/host.mjs');
  const { workerExtension } = await import('../boot/worker-proto/ext.mjs');
  const missing = checkSupport();
  if (missing.length) { console.error('missing: ' + missing.join(', ')); process.exit(2); }
  const build = opt('build', 'build/proto/run');
  const t0 = performance.now();
  const ext = workerExtension({
    check: flag('check'), dropNr: opt('drop', '').split(',').filter(Boolean).map(Number), spinA: Number(opt('spin-a', 0)), mainSpinUs: Number(opt('main-spin', 0)),
    onEvent: (what, W, info) => {
      if (flag('stats') && (what === 'killed' || what === 'exit'))
        process.stderr.write(`[worker-proto] pid ${W.pid} (${W.mode}): ${W.nsys} calls served, ${W.parks || 0} parks, ${W.spinHits || 0} main-spin hits, ${W.asyncWaits || 0} waitAsync, ${W.bytesIn} bytes in, ${W.bytesOut} bytes out\n`);
      if (flag('quiet')) return;
      if ((what === 'killed' && info === 'kernel') || (what === 'exited' && W.why === 'kernel') || what === 'crash' || what === 'loadfail')
        process.stderr.write(`[worker-proto] pid ${W.pid} (${W.mode}) ${what}${info ? ' ' + info : ''} t=${(performance.now() - t0).toFixed(1)} ms` +
          (what === 'exited' && W.t.kill ? `, ${(W.t.exited - W.t.kill).toFixed(2)} ms after the kill` : '') + '\n');
    },
  });
  const sys = await boot({
    kernel: fs.readFileSync(path.join(root, build, 'kernel.wasm')),
    initrd: fs.readFileSync(path.join(root, build, 'initrd.tar')),
    cmdline: 'init=' + opt('init', '/bin/init') + (opt('args', '') ? ' -- ' + opt('args', '') : ''),
    screen: { width: 640, height: 400 },
    log: (s) => process.stdout.write(s),
    debug: flag('debug'),
    onIdle: () => {
      if (ext.stats.violations) process.stderr.write(`[worker-proto] ${ext.stats.violations} marshalling violations\n`);
      process.exit(0);
    },
    extend: ext.extend,
  });
  if (sys.rc) process.exit(1);
  const input = opt('input', null);
  if (input !== null) { sys.consoleInput(input.replace(/\\n/g, '\n')); sys.consoleEOF(); }
} else {
  await browser();
}

// Proportional set size (KiB) of a process and all its descendants, from /proc (Linux).
function pss(top) {
  const kids = new Map();
  for (const d of fs.readdirSync('/proc')) {
    if (!/^\d+$/.test(d)) continue;
    try {
      const st = fs.readFileSync(`/proc/${d}/stat`, 'utf8');
      const ppid = Number(st.slice(st.lastIndexOf(')') + 2).split(' ')[1]);
      if (!kids.has(ppid)) kids.set(ppid, []);
      kids.get(ppid).push(Number(d));
    } catch { /* gone */ }
  }
  let total = 0;
  const walk = (pid) => {
    try { const m = fs.readFileSync(`/proc/${pid}/smaps_rollup`, 'utf8').match(/^Pss:\s+(\d+)/m); if (m) total += Number(m[1]); } catch { /* gone */ }
    for (const c of kids.get(pid) || []) walk(c);
  };
  walk(top);
  return total;
}

async function browser() {
  const build = opt('build', 'build');
  const dist = path.join(root, build, 'proto', 'dist');
  if (!fs.existsSync(path.join(dist, 'kernel.wasm'))) { console.error(`no ${dist}: run make -f tools/worker-proto/proto.mk B=${build}`); process.exit(2); }
  const coi = !flag('no-coi');
  // A static server like the one in tools/browser.mjs, plus the two headers that make the
  // page cross-origin isolated (SharedArrayBuffer, shared WebAssembly memory).
  const types = { '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript', '.wasm': 'application/wasm', '.tar': 'application/x-tar' };
  const server = http.createServer((req, res) => {
    const name = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    const file = path.join(dist, name === '/' ? 'index.html' : name);
    if (!file.startsWith(dist) || !fs.existsSync(file)) { res.writeHead(404); res.end('not found'); return; }
    const h = { 'content-type': types[path.extname(file)] || 'application/octet-stream', 'cache-control': 'no-store' };
    if (coi) { h['cross-origin-opener-policy'] = 'same-origin'; h['cross-origin-embedder-policy'] = 'require-corp'; }
    res.writeHead(200, h);
    fs.createReadStream(file).pipe(res);
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const base = `http://127.0.0.1:${server.address().port}`;
  const test = opt('test', 'bench');
  const W = 1280, H = 720;
  const page = test === 'desktop' ? `/worker-proto/desktop.html?scale=1&base=..` : test === 'd' ? `/worker-proto/d.html?${opt('query', '')}` : test === 'terminate' ? '/worker-proto/terminate.html' : `/worker-proto/test.html?${opt('query', '')}`;
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'ucos-wp-'));
  const chrome = spawn(opt('chromium', process.env.CHROMIUM || 'chromium'), [
    '--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`, '--no-first-run', '--disable-gpu',
    '--hide-scrollbars', `--window-size=${W},${H}`, 'about:blank',
  ], { stdio: ['ignore', 'ignore', 'pipe'] });
  const wsUrl = await new Promise((resolve, reject) => {
    let err = '';
    chrome.stderr.on('data', (d) => { err += d; const m = err.match(/DevTools listening on (ws:\/\/\S+)/); if (m) resolve(m[1]); });
    chrome.on('exit', (c) => reject(new Error('chromium exited early (' + c + ')\n' + err)));
    setTimeout(() => reject(new Error('chromium did not start\n' + err)), 20000);
  });
  const ws = new WebSocket(wsUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  let nextId = 1, sessionId, logText = '';
  const pending = new Map();
  let done;
  const finished = new Promise((r) => { done = r; });
  ws.onmessage = (m) => {
    const msg = JSON.parse(m.data);
    if (msg.id && pending.has(msg.id)) {
      const { resolve, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result);
    } else if (msg.method === 'Runtime.consoleAPICalled') {
      const line = msg.params.args.map((a) => a.value ?? a.description ?? '').join(' ');
      logText += line + '\n';
      if (!flag('quiet') || line.startsWith('[test]')) console.log(line);
      const rm = line.match(/^\[test\] rss (.*)/);
      if (rm) console.log(`[rss] ${rm[1]}: ${(pss(chrome.pid) / 1024).toFixed(1)} MiB (PSS summed over the Chromium process tree)`);
      if (line.startsWith('[test] done')) done();
    } else if (msg.method === 'Runtime.exceptionThrown') {
      const line = '[page exception] ' + (msg.params.exceptionDetails.exception?.description || msg.params.exceptionDetails.text);
      logText += line + '\n';
      console.log(line);
    }
  };
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    ws.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
  });
  const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
  sessionId = (await send('Target.attachToTarget', { targetId, flatten: true })).sessionId;
  await send('Page.enable');
  await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: base + page });
  const ev = async (expression) => {
    const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
    return r.result.value;
  };
  let code = 0;
  try {
    if (test === 'desktop') {
      const script = await import(new URL('./worker-proto/desktop-test.mjs', import.meta.url).href);
      await script.default({ send, ev, sleep: (ms) => new Promise((r) => setTimeout(r, ms)), shot: async (name) => {
        const { data } = await send('Page.captureScreenshot', { format: 'png' });
        const out = path.join(root, build, 'shots');
        fs.mkdirSync(out, { recursive: true });
        fs.writeFileSync(path.join(out, name + '.png'), Buffer.from(data, 'base64'));
        console.log('[shot] ' + path.join(out, name + '.png'));
      }, get log() { return logText; }, chromePid: chrome.pid });
    } else {
      const timeout = Number(opt('timeout', 120000));
      await Promise.race([finished, new Promise((_, j) => setTimeout(() => j(new Error('timeout')), timeout))]);
    }
  } catch (e) { console.error('[worker-proto] ' + (e && e.stack || e)); code = 1; }
  chrome.kill();
  server.close();
  process.exit(code);
}
