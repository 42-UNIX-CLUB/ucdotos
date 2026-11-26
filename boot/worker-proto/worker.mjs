// SPDX-License-Identifier: GPL-2.0-or-later
// Worker-process prototype: the code that runs inside a Web Worker (or a Node worker
// thread) and hosts ONE UC.OS process. The kernel stays on the main thread; every
// system call goes there. Research code; see docs/WORKER-PROCESSES.md.
//
//   mode 'A'  the program was built with a shared memory. A system call is written into a
//             small SharedArrayBuffer and the worker sleeps in Atomics.wait until the main
//             thread has run it. The kernel copies straight to and from the program's
//             memory, because the main thread's copy shim imports that same memory.
//   mode 'B'  an ordinary build. Each system call suspends the program (JSPI), goes to the
//             main thread as a message carrying the bytes the call reads (marshal.mjs), and
//             the reply carries the bytes the kernel wrote.
import { inputs } from './marshal.mjs';

const isWeb = typeof WorkerGlobalScope !== 'undefined';
const port = isWeb ? self : (await import('node:worker_threads')).parentPort;
const post = (m, transfer) => port.postMessage(m, transfer || []);
const EXIT = Symbol('exit');
const ST = 0, OP = 1, NR = 2, ARG = 3, RES = 9;      // control block layout (Int32Array)
const IDLE = 0, REQ = 1, DONE = 2, OP_SYS = 1, OP_BLOCK = 2;

// B: replies arrive in order; one may come before the code that wants it is waiting
// (the "you may run again" message after a -ERESTARTSYS can beat the call to block()).
const inbox = [];
let reply = null;
const next = () => (inbox.length ? Promise.resolve(inbox.shift()) : new Promise((r) => { reply = r; }));
const onMessage = (m) => {
  if (m.op === 'start') start(m);
  else if (reply) { const r = reply; reply = null; r(m); }
  else inbox.push(m);
};
if (isWeb) self.onmessage = (e) => onMessage(e.data); else port.on('message', onMessage);

function start({ mode, module, pid, ctl: ctlBuf, spin = 0, drop = [] }) {
  if (mode === 'A') return startA(module, pid, new Int32Array(ctlBuf), spin);
  return startB(module, pid, drop);
}

function finish(e) {
  if (e === EXIT || e === undefined) post({ op: 'exit' });
  else post({ op: 'crash', msg: String(e && e.message || e) });
}

// ---- A: shared memory, Atomics.wait ----
function startA(module, pid, ctl, spin) {
  const call = (op) => {
    Atomics.store(ctl, ST, REQ);
    Atomics.notify(ctl, ST);
    for (let i = 0; i < spin && Atomics.load(ctl, ST) === REQ; i++);  // optional short spin before sleeping
    while (Atomics.load(ctl, ST) === REQ) Atomics.wait(ctl, ST, REQ);
    const r = ctl[RES];
    Atomics.store(ctl, ST, IDLE);
    return r;
  };
  const syscall = (nr, a, b, c, d, e, f) => {
    ctl[OP] = OP_SYS; ctl[NR] = nr;
    ctl[ARG] = a; ctl[ARG + 1] = b; ctl[ARG + 2] = c; ctl[ARG + 3] = d; ctl[ARG + 4] = e; ctl[ARG + 5] = f;
    return call(OP_SYS);
  };
  const block = () => { ctl[OP] = OP_BLOCK; if (call(OP_BLOCK)) throw EXIT; };
  let inst;
  try {
    inst = new WebAssembly.Instance(module, { host: { syscall, block } });
  } catch (e) { post({ op: 'crash', msg: 'instantiate: ' + e.message, load: true }); return; }
  post({ op: 'mem', memory: inst.exports.memory });  // the main thread builds the shim on it
  try { inst.exports._start(); finish(); } catch (e) { finish(e); }
}

// ---- B: no shared memory, JSPI + messages ----
// drop: test hook that skips table entries, to prove the checking mode catches a miss
async function startB(module, pid, drop) {
  let mem = null;
  const syscall = new WebAssembly.Suspending(async (nr, a, b, c, d, e, f) => {
    const args = [a, b, c, d, e, f];
    const u8 = new Uint8Array(mem.buffer);
    const ranges = drop.includes(nr) ? [] : inputs(nr, args, u8, new DataView(mem.buffer));
    let data = null, flat = null;
    if (ranges.length) {
      let total = 0;
      for (const [, n] of ranges) total += n;
      data = new Uint8Array(total);
      flat = new Int32Array(ranges.length * 2);
      let o = 0;
      ranges.forEach(([p, n], i) => { data.set(u8.subarray(p, p + n), o); o += n; flat[2 * i] = p; flat[2 * i + 1] = n; });
    }
    post({ op: 'sys', nr, a: args, pages: u8.length >>> 16, ranges: flat, data }, data ? [data.buffer] : []);
    const rep = await next();
    if (rep.data) {
      const dst = new Uint8Array(mem.buffer);
      let o = 0;
      for (let i = 0; i < rep.ranges.length; i += 2) { const p = rep.ranges[i], n = rep.ranges[i + 1]; dst.set(rep.data.subarray(o, o + n), p); o += n; }
    }
    return rep.r;
  });
  // Called by the libc right after a call returned -ERESTARTSYS. The main thread has already
  // parked the process at that moment; wait for its "run again" (or "you are dead") message.
  const block = new WebAssembly.Suspending(async () => {
    const rep = await next();
    if (rep.dead) throw EXIT;
  });
  let inst;
  try {
    inst = await WebAssembly.instantiate(module, { host: { syscall, block } });
  } catch (e) { post({ op: 'crash', msg: 'instantiate: ' + e.message, load: true }); return; }
  mem = inst.exports.memory;
  post({ op: 'ready', pages: mem.buffer.byteLength >>> 16 });
  try { await WebAssembly.promising(inst.exports._start)(); finish(); } catch (e) { finish(e); }
}
