// SPDX-License-Identifier: GPL-2.0-or-later
// Open one page of this repository in headless Chromium and print what it logs to its
// console, until it logs a line that starts with "@@DONE" (or the time limit passes).
// Used for the translator's measurements in a real browser (docs/BINARY-TRANSLATION.md).
//
//   node tools/jit/chrome.mjs --page=tools/jit/mech.html [--query='a=1'] [--timeout=120000]
//                             [--root=.] [--chromium=chromium]
//
// It serves --root (default: the repository) itself, with no special headers.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { spawn } from 'node:child_process';

const repo = new URL('../..', import.meta.url).pathname;
const args = process.argv.slice(2);
const opt = (n, d) => { const a = args.find((x) => x.startsWith(`--${n}=`)); return a ? a.slice(n.length + 3) : d; };
const root = path.resolve(opt('root', repo));
const page = opt('page', 'tools/jit/mech.html');
const timeout = Number(opt('timeout', 120000));

const types = { '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript', '.wasm': 'application/wasm', '.tar': 'application/x-tar' };
const server = http.createServer((req, res) => {
  const name = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const file = path.join(root, name);
  if (!file.startsWith(root) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) { res.writeHead(404); res.end('not found'); return; }
  res.writeHead(200, { 'content-type': types[path.extname(file)] || 'application/octet-stream', 'cache-control': 'no-store' });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const url = `http://127.0.0.1:${server.address().port}/${page}?${opt('query', '')}`;

// The browser profile goes on the real disk, not /tmp (a RAM disk on the project laptop).
const profDir = path.join(repo, 'build-chrome-profiles');
fs.mkdirSync(profDir, { recursive: true });
// --profile=DIR keeps a profile (and its IndexedDB) between runs; otherwise a new one each time
const profile = opt('profile', null) ? path.resolve(opt('profile')) : fs.mkdtempSync(path.join(profDir, 'p-'));
fs.mkdirSync(profile, { recursive: true });
const chrome = spawn(opt('chromium', process.env.CHROMIUM || 'chromium'), [
  '--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`, '--no-first-run', '--disable-gpu', 'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });
const wsUrl = await new Promise((resolve, reject) => {
  let err = '';
  chrome.stderr.on('data', (d) => { err += d; const m = err.match(/DevTools listening on (ws:\/\/\S+)/); if (m) resolve(m[1]); });
  chrome.on('exit', (c) => reject(new Error('chromium exited early (' + c + ')\n' + err)));
  setTimeout(() => reject(new Error('chromium did not start\n' + err)), 20000);
});
const ws = new WebSocket(wsUrl);
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
let nextId = 1, sessionId, finish;
const done = new Promise((r) => { finish = r; });
const pending = new Map();
ws.onmessage = (m) => {
  const msg = JSON.parse(m.data);
  if (msg.id && pending.has(msg.id)) {
    const { resolve, reject } = pending.get(msg.id);
    pending.delete(msg.id);
    msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result);
  } else if (msg.method === 'Runtime.consoleAPICalled') {
    const line = msg.params.args.map((a) => a.value ?? a.description ?? '').join(' ');
    if (line.startsWith('@@DONE')) finish(0);
    else process.stdout.write(line + '\n');
  } else if (msg.method === 'Runtime.exceptionThrown') {
    process.stdout.write('[page exception] ' + (msg.params.exceptionDetails.exception?.description || msg.params.exceptionDetails.text) + '\n');
    finish(1);
  }
};
const send = (method, params = {}) => new Promise((resolve, reject) => {
  const id = nextId++;
  pending.set(id, { resolve, reject });
  ws.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
});
const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
sessionId = (await send('Target.attachToTarget', { targetId, flatten: true })).sessionId;
await send('Runtime.enable');
await send('Page.navigate', { url });
const timer = setTimeout(() => { process.stdout.write('[chrome] time limit reached\n'); finish(2); }, timeout);
const code = await done;
clearTimeout(timer);
// Close through the protocol so the profile (IndexedDB) is written out; kill only as a fallback.
const exited = new Promise((r) => chrome.on('exit', r));
try { sessionId = undefined; await Promise.race([send('Browser.close'), new Promise((r) => setTimeout(r, 3000))]); } catch {}
await Promise.race([exited, new Promise((r) => setTimeout(r, 5000))]);
chrome.kill();
server.close();
process.exit(code);
