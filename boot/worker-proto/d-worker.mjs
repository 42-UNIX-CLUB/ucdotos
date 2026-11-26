// SPDX-License-Identifier: GPL-2.0-or-later
// Worker-process prototype, approach D: the whole machine (boot/host.mjs, the kernel and
// every one-thread process) runs in this Worker. The page only forwards input, shows the
// screen through a transferred OffscreenCanvas, and watches a heartbeat. With
// `withWorkers`, approach A/B processes run in nested Workers (ext.mjs) as on the page.
// Research code; see docs/WORKER-PROCESSES.md.
import { boot } from '../host.mjs';
import { workerExtension } from './ext.mjs';

let sys = null, dirty = false, frames = 0;
const post = (m) => postMessage(m);
self.onmessage = async (e) => {
  const m = e.data;
  if (m.op === 'boot') {
    const canvas = m.canvas, ctx = canvas.getContext('2d');
    const ext = m.withWorkers ? workerExtension({
      onEvent: (what, W, info) => { if (what === 'killed' && info === 'kernel') post({ op: 'log', s: `[test] D: pid ${W.pid} (${W.mode}) nested worker terminated\n` }); },
    }) : null;
    const t0 = performance.now();
    sys = await boot({
      kernel: m.kernel, initrd: m.initrd, cmdline: m.cmdline,
      screen: { width: canvas.width, height: canvas.height },
      log: (s) => post({ op: 'log', s }),
      onFrame: () => { dirty = true; },
      extend: ext ? ext.extend : null,
    });
    post({ op: 'booted', ms: performance.now() - t0 });
    // The heartbeat runs only when this thread's event loop turns: a process that computes
    // without system calls on this thread stops it, and the page's watchdog notices.
    setInterval(() => post({ op: 'beat', t: performance.now(), frames }), 50);
    const draw = () => {
      if (dirty) {
        dirty = false;
        const f = sys.frame();
        if (f.addr) { ctx.putImageData(new ImageData(new Uint8ClampedArray(sys.memory.buffer, f.addr, f.width * f.height * 4), f.width, f.height), 0, 0); frames++; }
      }
      (self.requestAnimationFrame || ((f) => setTimeout(f, 16)))(draw);
    };
    draw();
  } else if (m.op === 'console') sys.consoleInput(m.s);
  else if (m.op === 'input') sys.input(m.type, m.code, m.value);
};
