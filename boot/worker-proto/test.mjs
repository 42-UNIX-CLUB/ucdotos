// SPDX-License-Identifier: GPL-2.0-or-later
// Worker-process prototype: the browser test page. Boots the prototype image with the
// worker extension, runs /usr/share/tests/wp-tests.sh in a shell on the console, presents
// the framebuffer on every animation frame, and reports per phase the longest gap between
// animation frames (how responsive the main thread stayed) and how many frames it presented.
// Lines starting with "[test]" are read by tools/worker-proto.mjs.
//   test.html                 the scenario
//   test.html?mem=8&kind=b    start 8 sleeping worker processes, for memory sampling
import { boot, checkSupport } from '../host.mjs';
import { workerExtension } from './ext.mjs';

const params = new URLSearchParams(location.search);
const logEl = document.getElementById('log');
const canvas = document.getElementById('screen');
const ctx = canvas.getContext('2d');
const out = (s) => { console.log(s); logEl.textContent += s + '\n'; };

const missing = checkSupport();
out(`[test] crossOriginIsolated=${self.crossOriginIsolated} SharedArrayBuffer=${typeof SharedArrayBuffer} waitAsync=${typeof Atomics.waitAsync} JSPI=${typeof WebAssembly.Suspending} missing=[${missing.join(', ')}]`);
out(`[test] ${navigator.userAgent}`);

// ---- frames: longest gap between animation frames, per phase ----
let phase = 'boot', gapMax = 0, frames = 0, presents = 0, last = 0, dirty = false, sys = null;
const phaseStats = [];
function frame() {
  const t = performance.now(); // the callback's own timestamp can predate a freeze
  if (last) { const g = t - last; if (g > gapMax) gapMax = g; }
  last = t;
  frames++;
  if (dirty && sys) {
    dirty = false;
    const f = sys.frame();
    if (f.addr) { ctx.putImageData(new ImageData(new Uint8ClampedArray(sys.memory.buffer, f.addr, f.width * f.height * 4), f.width, f.height), 0, 0); presents++; }
  }
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
let phaseStart = performance.now();
function endPhase(next) {
  const ms = performance.now() - phaseStart;
  // a freeze that is still going on belongs to the phase that caused it
  if (last && performance.now() - last > gapMax) gapMax = performance.now() - last;
  last = performance.now();
  phaseStats.push({ phase, ms, gapMax, frames, presents });
  out(`[test] frames ${phase}: ${ms.toFixed(0)} ms, ${frames} animation frames, longest gap ${gapMax.toFixed(1)} ms, ${presents} framebuffer presents`);
  phase = next; gapMax = 0; frames = 0; presents = 0; phaseStart = performance.now();
}

// ---- console: line-buffer the kernel log and watch for phase markers ----
let pending = '';
function onLog(s) {
  pending += s;
  let i;
  while ((i = pending.indexOf('\n')) >= 0) {
    const line = pending.slice(0, i);
    pending = pending.slice(i + 1);
    const m = line.match(/^\[test\] phase (\S+)/);
    if (m) { endPhase(m[1]); if (m[1] === 'end') finish(); continue; }
    out(line);
  }
}
let finished = false;
function finish() {
  if (finished) return;
  finished = true;
  out(`[test] violations=${ext.stats.violations}`);
  out('[test] done');
}

const ext = workerExtension({
  check: params.has('check'),
  spinA: Number(params.get('spina') || 0),
  mainSpinUs: Number(params.get('mainspin') || 0),
  onEvent: (what, W, info) => {
    if (what === 'killed' && info === 'kernel') out(`[test] pid ${W.pid} (${W.mode}) worker terminated ${(W.t.terminated - W.t.kill).toFixed(2)} ms after the kernel killed it`);
    if (what === 'exited' && W.why === 'kernel') out(`[test] pid ${W.pid} (${W.mode}) its memory stopped changing ${(W.t.exited - W.t.kill).toFixed(1)} ms after the kill (2 ms polling)`);
    if (what === 'still-running') out(`[test] pid ${W.pid} (${W.mode}) its memory was STILL changing 5 s after the kill`);
    if (what === 'crash' || what === 'loadfail') out(`[test] pid ${W.pid} (${W.mode}) ${what}: ${info}`);
  },
});

const get = async (u) => new Uint8Array(await (await fetch(u)).arrayBuffer());
const [kernel, initrd] = await Promise.all([get('../kernel.wasm'), get('../initrd.tar')]);
const t0 = performance.now();
sys = await boot({
  kernel, initrd,
  cmdline: 'init=/bin/sh',
  screen: { width: 640, height: 400 },
  log: onLog,
  onFrame: () => { dirty = true; },
  onIdle: () => { out('[test] every process exited'); finish(); },
  debug: params.has('debug'),
  extend: ext.extend,
});
out(`[test] booted in ${(performance.now() - t0).toFixed(1)} ms`);

const mem = Number(params.get('mem') || 0);
if (mem) {
  const kind = params.get('kind') || 'b';
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  await sleep(1500);
  out('[test] rss baseline');
  await sleep(500);
  let script = '';
  for (let i = 0; i < mem; i++) script += `wproto${kind === 'main' ? '' : '-' + kind} tick 100000 100 &\n`;
  sys.consoleInput(script + 'echo started\n');
  await sleep(4000);
  out(`[test] rss after ${mem} ${kind}`);
  await sleep(500);
  finish();
} else if (params.get('cmd')) {         // ?cmd=...: run these shell lines instead of the scenario
  sys.consoleInput(params.get('cmd').replace(/\\n/g, '\n') + '\necho "[test] phase end"\n');
} else {
  sys.consoleInput(`sh /usr/share/tests/wp-tests.sh ${self.crossOriginIsolated ? 'coi' : 'nocoi'}\nexit\n`);
  sys.consoleEOF();
}
