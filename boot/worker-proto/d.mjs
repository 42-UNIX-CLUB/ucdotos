// SPDX-License-Identifier: GPL-2.0-or-later
// Worker-process prototype, approach D: the page side. The machine runs in d-worker.mjs.
// This page measures its own animation frames (is the page responsive?), watches the
// machine's heartbeat, and when the heartbeat stops for longer than the watchdog limit
// (?wd=MS, default 1000) it terminates the machine's Worker and boots a fresh one.
// Scenario: 1) a pure CPU loop in a one-thread process inside the machine: the watchdog
// fires and the machine reboots; 2) the machine with nested worker processes (D + A): a
// spinning process is killed without a reboot. Lines starting with "[test]" are read by
// tools/worker-proto.mjs --test=d.
const params = new URLSearchParams(location.search);
const logEl = document.getElementById('log');
const out = (s) => { console.log(s); logEl.textContent += s + '\n'; };
const wd = Number(params.get('wd') || 1000);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const get = async (u) => new Uint8Array(await (await fetch(u)).arrayBuffer());
const [kernel, initrd] = await Promise.all([get('../kernel.wasm'), get('../initrd.tar')]);
out(`[test] D: crossOriginIsolated=${self.crossOriginIsolated} OffscreenCanvas=${typeof OffscreenCanvas}`);

// page responsiveness
let gapMax = 0, last = 0;
const frame = () => { const t = performance.now(); if (last && t - last > gapMax) gapMax = t - last; last = t; requestAnimationFrame(frame); };
requestAnimationFrame(frame);

let worker = null, lastBeat = 0, beatGapMax = 0, beatFrames = 0, onLine = () => {}, pending = '';
function onLog(s) {
  pending += s;
  let i;
  while ((i = pending.indexOf('\n')) >= 0) { const line = pending.slice(0, i); pending = pending.slice(i + 1); out(line); onLine(line); }
}
const waitLine = (re) => new Promise((r) => { onLine = (l) => { if (re.test(l)) { onLine = () => {}; r(l); } }; });

async function start(withWorkers) {
  const holder = document.getElementById('screen');
  holder.textContent = '';
  const c = document.createElement('canvas');
  c.width = 640; c.height = 400;
  holder.appendChild(c);
  const off = c.transferControlToOffscreen();   // a canvas can be transferred once: a new one per boot
  worker = new Worker('./d-worker.mjs', { type: 'module' });
  lastBeat = 0; beatGapMax = 0;
  let booted;
  const ready = new Promise((r) => { booted = r; });
  worker.onmessage = (e) => {
    const m = e.data;
    if (m.op === 'log') onLog(m.s);
    else if (m.op === 'beat') { const t = performance.now(); if (lastBeat && t - lastBeat > beatGapMax) beatGapMax = t - lastBeat; lastBeat = t; beatFrames = m.frames; }
    else if (m.op === 'booted') booted(m.ms);
  };
  worker.onerror = (e) => out('[test] D: worker error ' + e.message);
  worker.postMessage({ op: 'boot', canvas: off, kernel: kernel.slice(), initrd: initrd.slice(), cmdline: 'init=/bin/sh', withWorkers }, [off]);
  return ready;
}
const sh = (s) => worker.postMessage({ op: 'console', s });

// the watchdog
let fired = null;
setInterval(() => {
  if (worker && lastBeat && performance.now() - lastBeat > wd && !fired) {
    fired = { t: performance.now(), sinceBeat: performance.now() - lastBeat };
    worker.terminate();
    worker = null;
  }
}, 20);

// 1) a one-thread process spins inside the machine
let t = performance.now();
let ms = await start(false);
out(`[test] D: machine booted in its Worker in ${ms.toFixed(1)} ms (${(performance.now() - t).toFixed(1)} ms including the Worker start)`);
await sleep(1000);
out(`[test] D: idle: page longest frame gap ${gapMax.toFixed(1)} ms, heartbeat longest gap ${beatGapMax.toFixed(1)} ms, machine frames ${beatFrames}`);
gapMax = 0;
sh('wproto spin 3\n');
while (!fired) await sleep(10);
out(`[test] D: watchdog fired ${fired.sinceBeat.toFixed(0)} ms after the last heartbeat (limit ${wd} ms); page longest frame gap meanwhile ${gapMax.toFixed(1)} ms`);
const tReboot = performance.now();
ms = await start(true);
sh('echo rebooted\n');
await waitLine(/^rebooted/);
out(`[test] D: rebooted to a working shell in ${(performance.now() - tReboot).toFixed(1)} ms (boot itself ${ms.toFixed(1)} ms); the previous machine's state is lost`);

// 2) D + A: worker processes nested in the machine's Worker
gapMax = 0; beatGapMax = 0;
if (self.crossOriginIsolated) {
  sh('wproto killtest /bin/wproto-a 500\n');
  await waitLine(/^wproto killtest/);
  sh('wproto-a spin 3 &\nwproto tick 250 10 fb\n');
  await waitLine(/^wproto tick:/);
  out(`[test] D+A: page longest frame gap ${gapMax.toFixed(1)} ms, machine heartbeat longest gap ${beatGapMax.toFixed(1)} ms, watchdog ${fired.t > tReboot ? 'fired again' : 'did not fire'}`);
}
gapMax = 0; beatGapMax = 0;
sh('wproto-b spin 2 &\nwproto tick 150 10 fb\n');
await waitLine(/^wproto tick:/);
out(`[test] D+B: page longest frame gap ${gapMax.toFixed(1)} ms, machine heartbeat longest gap ${beatGapMax.toFixed(1)} ms`);
out('[test] done');
