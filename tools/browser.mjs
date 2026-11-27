// SPDX-License-Identifier: GPL-2.0-or-later
// Run the built system in a real browser (headless Chromium), drive it with real
// keyboard and mouse events through the DevTools protocol, and save screenshots.
// This tests what tools/shot.mjs cannot: the page itself (boot/page.mjs), the
// browser's event handling, its clock and its scheduler.
//
//   node tools/browser.mjs [--build=build] [--size=1280x720] [--out=<build>/shots] [--query='scale=1']
//                          [--script=path/to/script.mjs] [--wait=4000] [--chromium=chromium]
//                          [--at=http://127.0.0.1:PORT]
//
// It serves <build>/dist itself (run `make B=<build> dist` first), or with --at opens a
// server that is already running (the platform serving a dist folder). Without --script it waits
// --wait milliseconds and writes <out>/browser.png. A script's default export is
// `async (t) => { ... }` with the same helpers as tools/shot.mjs where they make sense:
//   t.sleep(ms)  t.shot(name)  t.key(code, down)  t.tap(code, {shift,ctrl,alt,meta})  t.type('text\n')
//   t.move(x, y)  t.click(x, y, button=0)  t.wheel(dy)  t.resize(w, h)
//   mods may also carry commands: ['paste'], the editing commands a real key press runs
//   (the protocol sends bare keys; a real browser maps Ctrl+V to its paste command itself)
//   t.eval('js expression')      evaluate in the page, returns the value
//   t.log                        everything the page printed to its console so far
//   t.requests                   every network request so far: { url, bytes (on the wire, once finished), status }
//   t.send(method, params)       a raw DevTools protocol call (network throttling, file chooser...)
//   t.openTab(query)             another tab of the same origin and profile: { eval, shot, close, log, cdp }
//   t.cdp(method, params)        a raw DevTools call on the first tab (for example Storage.*)
//   t.origin                     the origin the pages are served from
//   t.on(method, f)              call f(params) for every DevTools event of that name
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { spawn } from 'node:child_process';
import * as K from '../boot/keys.mjs';

const root = new URL('..', import.meta.url).pathname;
const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const build = opt('build', 'build');
const dist = path.join(root, build, 'dist');
const outDir = opt('out', path.join(root, build, 'shots'));
let [W, H] = opt('size', '1280x720').split('x').map(Number);
const at = opt('at', null);
if (!at && !fs.existsSync(path.join(dist, 'kernel.wasm'))) { console.error(`no ${dist}/kernel.wasm: run make B=${build} dist`); process.exit(2); }
fs.mkdirSync(outDir, { recursive: true });

// ---- a static file server for the dist folder ----
const types = { '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript', '.wasm': 'application/wasm', '.tar': 'application/x-tar' };
const server = http.createServer((req, res) => {
  const name = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const file = path.join(dist, name === '/' ? 'index.html' : name);
  if (!file.startsWith(dist) || !fs.existsSync(file)) { res.writeHead(404); res.end('not found'); return; }
  res.writeHead(200, { 'content-type': types[path.extname(file)] || 'application/octet-stream', 'cache-control': 'no-store' });
  fs.createReadStream(file).pipe(res);
});
if (!at) await new Promise((r) => server.listen(0, '127.0.0.1', r));
const url = `${at ? at.replace(/\/+$/, '') : 'http://127.0.0.1:' + server.address().port}/?${opt('query', 'scale=1')}`;

// ---- Chromium and the DevTools protocol ----
const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'ucos-chromium-'));
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
const requests = [], byId = new Map(), listeners = new Map();
ws.onmessage = (m) => {
  const msg = JSON.parse(m.data);
  if (msg.id && pending.has(msg.id)) {
    const { resolve, reject } = pending.get(msg.id);
    pending.delete(msg.id);
    msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result);
  } else if (msg.sessionId && tabLogs.has(msg.sessionId) && msg.method === 'Runtime.consoleAPICalled') {
    tabLogs.set(msg.sessionId, tabLogs.get(msg.sessionId) + msg.params.args.map((a) => a.value ?? a.description ?? '').join(' ') + '\n');
  } else if (msg.sessionId && tabLogs.has(msg.sessionId)) {
    // another tab's events
  } else if (msg.method === 'Runtime.consoleAPICalled') {
    logText += msg.params.args.map((a) => a.value ?? a.description ?? '').join(' ') + '\n';
  } else if (msg.method === 'Network.requestWillBeSent') {
    const r = { url: msg.params.request.url, bytes: 0, status: 0 };
    requests.push(r); byId.set(msg.params.requestId, r);
  } else if (msg.method === 'Network.responseReceived') {
    const r = byId.get(msg.params.requestId); if (r) r.status = msg.params.response.status;
  } else if (msg.method === 'Network.loadingFinished') {
    const r = byId.get(msg.params.requestId); if (r) r.bytes = msg.params.encodedDataLength;
  } else if (msg.method === 'Runtime.exceptionThrown') {
    logText += '[page exception] ' + (msg.params.exceptionDetails.exception?.description || msg.params.exceptionDetails.text) + '\n';
  }
  if (msg.method && listeners.has(msg.method)) for (const f of listeners.get(msg.method)) f(msg.params);
};
const tabLogs = new Map(); // sessionId -> console text of a tab made by t.openTab
const sendTo = (sid, method, params = {}) => new Promise((resolve, reject) => {
  const id = nextId++;
  pending.set(id, { resolve, reject });
  ws.send(JSON.stringify({ id, method, params, ...(sid ? { sessionId: sid } : {}) }));
});
const send = (method, params = {}) => sendTo(sessionId, method, params);
const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
sessionId = (await send('Target.attachToTarget', { targetId, flatten: true })).sessionId;
await send('Page.enable');
await send('Runtime.enable');
await send('Network.enable');
await send('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: 1, mobile: false });
await send('Page.navigate', { url });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const modBits = (m = {}) => (m.alt ? 1 : 0) | (m.ctrl ? 2 : 0) | (m.meta ? 4 : 0) | (m.shift ? 8 : 0);
const named = { Enter: '\r', Tab: '\t', Space: ' ' };
let mouseX = 0, mouseY = 0;
const t = {
  sleep, send,
  get log() { return logText; },
  get requests() { return requests; },
  on(method, f) { if (!listeners.has(method)) listeners.set(method, []); listeners.get(method).push(f); },
  async eval(expression) {
    const r = await send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
    return r.result.value;
  },
  async shot(name = 'browser') {
    const { data } = await send('Page.captureScreenshot', { format: 'png' });
    const file = `${outDir}/${name}.png`;
    fs.writeFileSync(file, Buffer.from(data, 'base64'));
    console.error(`[browser] ${file}`);
    return file;
  },
  // key: a KeyboardEvent.code name; text: the character it types, if any
  async key(code, down, mods = {}, text = '') {
    if (!(code in K.KEY)) throw new Error('unknown key ' + code);
    const key = text || (code.startsWith('Key') ? code.slice(3).toLowerCase() : code.startsWith('Digit') ? code.slice(5) : code);
    const typed = text || (mods.ctrl || mods.alt || mods.meta ? '' : named[code] || '');
    await send('Input.dispatchKeyEvent', { type: down ? (typed ? 'keyDown' : 'rawKeyDown') : 'keyUp', code, key, modifiers: modBits(mods),
      ...(down && typed ? { text: typed } : {}), ...(down && mods.commands ? { commands: mods.commands } : {}) });
  },
  async tap(code, mods = {}, text = '') {
    const held = [mods.ctrl && 'ControlLeft', mods.alt && 'AltLeft', mods.shift && 'ShiftLeft', mods.meta && 'MetaLeft'].filter(Boolean);
    for (const m of held) await t.key(m, true, { ...mods, commands: undefined });
    await t.key(code, true, mods, mods.ctrl || mods.alt || mods.meta ? '' : text);
    await sleep(8);
    await t.key(code, false, mods);
    for (const m of held.reverse()) await t.key(m, false);
    await sleep(8);
  },
  async type(text) {
    for (const ch of text) {
      const k = K.keyForChar(ch);
      if (!k) throw new Error('cannot type ' + JSON.stringify(ch));
      await t.tap(k.code, { shift: k.shift }, ch === '\n' ? '\r' : ch);
    }
  },
  async move(x, y) { mouseX = x; mouseY = y; await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y }); },
  async click(x, y, button = 0) {
    const b = ['left', 'middle', 'right'][button];
    await t.move(x, y);
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: b, clickCount: 1 });
    await sleep(16);
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: b, clickCount: 1 });
    await sleep(16);
  },
  async wheel(dy) { await send('Input.dispatchMouseEvent', { type: 'mouseWheel', x: mouseX, y: mouseY, deltaX: 0, deltaY: dy > 0 ? -120 : 120 }); },
  async resize(w, h) { W = w; H = h; await send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: false }); },
  origin: url.replace(/\/\?.*$/, ''),
  cdp: (method, params) => send(method, params),
  async closeFirst() { await sendTo(null, 'Target.closeTarget', { targetId }); }, // the first tab goes away, as when the user closes it
  async openTab(query) {
    const { targetId: tid } = await sendTo(null, 'Target.createTarget', { url: 'about:blank' });
    const sid = (await sendTo(null, 'Target.attachToTarget', { targetId: tid, flatten: true })).sessionId;
    tabLogs.set(sid, '');
    await sendTo(sid, 'Runtime.enable');
    await sendTo(sid, 'Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: 1, mobile: false });
    await sendTo(sid, 'Page.navigate', { url: url.replace(/\?.*$/, '?' + (query ?? opt('query', 'scale=1'))) });
    return {
      get log() { return tabLogs.get(sid); },
      async eval(expression) {
        const r = await sendTo(sid, 'Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
        if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
        return r.result.value;
      },
      async shot(name) {
        const { data } = await sendTo(sid, 'Page.captureScreenshot', { format: 'png' });
        const file = `${outDir}/${name}.png`;
        fs.writeFileSync(file, Buffer.from(data, 'base64'));
        console.error(`[browser] ${file}`);
        return file;
      },
      cdp: (method, params) => sendTo(sid, method, params),
      async close() { await sendTo(null, 'Target.closeTarget', { targetId: tid }); tabLogs.delete(sid); },
    };
  },
};

let code = 0;
try {
  const script = opt('script', null);
  if (script) await (await import(new URL(script, 'file://' + process.cwd() + '/').href)).default(t);
  else { await sleep(Number(opt('wait', 4000))); await t.shot('browser'); }
} catch (e) { console.error('[browser] ' + (e && e.stack || e)); code = 1; }
if (args.includes('--log')) process.stdout.write(logText);
chrome.kill();
if (!at) server.close();
process.exit(code);
