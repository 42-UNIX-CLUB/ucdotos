// SPDX-License-Identifier: GPL-2.0-or-later
// Worker-process prototype: how long does Worker.terminate() take to stop a Worker that is
// computing (a wasm loop, then a JavaScript loop)? The loop increments a counter in a shared
// memory; the page reports when the counter stops. Needs cross-origin isolation.
//   node tools/worker-proto.mjs --browser --build=build-NAME --test=terminate
for (const kind of ['wasm', 'js']) {
  const mem = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true });
  const v = new Int32Array(mem.buffer);
  const w = new Worker('./terminate-worker.js');
  await new Promise((r) => { w.onmessage = r; w.postMessage([mem, kind]); });
  await new Promise((r) => setTimeout(r, 300));
  const t0 = performance.now(); w.terminate();
  let last = v[0], stopped = null;
  while (performance.now() - t0 < 10000) { await new Promise((r) => setTimeout(r, 5)); const c = v[0]; if (c === last) { if (stopped === null) stopped = performance.now() - t0; } else stopped = null; last = c; if (stopped !== null && performance.now() - t0 - stopped > 200) break; }
  console.log(`[test] ${kind} loop: counter stopped changing ${stopped === null ? 'NEVER within 10 s' : stopped.toFixed(1) + ' ms'} after terminate()`);
}
console.log('[test] done');
