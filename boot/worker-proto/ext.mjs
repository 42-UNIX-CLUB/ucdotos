// SPDX-License-Identifier: GPL-2.0-or-later
// Worker-process prototype, main-thread side. It wraps the kernel's host imports
// (boot/host.mjs, option `extend`) so that a program of the right kind runs in its own
// Worker instead of on the main thread. The kernel does not change and does not know:
// it still calls host.spawn, host.wake and host.reap, and still reaches the process's
// memory through a copy shim in its function table. Research code, off by default;
// see docs/WORKER-PROCESSES.md.
//
//   import { workerExtension } from './worker-proto/ext.mjs';
//   const ext = workerExtension({ check: false });
//   await boot({ ..., extend: ext.extend });
//
// Which programs: a module whose own memory is shared runs as approach A; a module that
// exports `ucos_worker` runs as approach B (see wasm.mjs). Everything else takes the
// default path untouched.
import { shimBytes, classify } from './wasm.mjs';

const isNode = typeof process === 'object' && !!(process.versions && process.versions.node) && typeof window === 'undefined';
const NodeWorker = isNode ? (await import('node:worker_threads')).Worker : null;
const P_RUN = 1;
const ST = 0, OP = 1, NR = 2, ARG = 3, RES = 9;
const IDLE = 0, REQ = 1, DONE = 2, GONE = 3, OP_SYS = 1, ERESTARTSYS = -512;

function makeWorker(url) {
  if (NodeWorker) {
    const w = new NodeWorker(url);
    return { post: (m, t) => w.postMessage(m, t), on: (f) => { w.on('message', f); w.on('error', (e) => f({ op: 'crash', msg: e.message })); }, terminate: () => w.terminate(), exited: new Promise((r) => w.on('exit', r)) };
  }
  const w = new Worker(url, { type: 'module' });
  return { post: (m, t) => w.postMessage(m, t), on: (f) => { w.onmessage = (e) => f(e.data); w.onerror = (e) => f({ op: 'crash', msg: e.message, load: true }); }, terminate: () => w.terminate(), exited: null };
}

export function workerExtension({ check = false, dropNr = [], spinA = 0, mainSpinUs = 0, onEvent = () => {}, workerUrl = new URL('./worker.mjs', import.meta.url) } = {}) {
  const workers = new Map();       // pid -> W
  const freeSlots = [];
  const modules = new Map();
  const shims = {};
  const shim = (kind) => shims[kind] || (shims[kind] = new WebAssembly.Module(shimBytes(
    kind === 'A' ? { shared: true } : kind === 'Bc' ? { recOut: true, recIn: true } : { recOut: true })));
  const now = () => performance.now();
  // one turn of the event loop without the 4 ms clamp on nested setTimeout(0) (as in host.mjs)
  const yq = [], ych = new MessageChannel();
  ych.port1.onmessage = () => { const f = yq.shift(); if (f) f(); if (!yq.length && ych.port1.unref) ych.port1.unref(); };
  if (ych.port1.unref) ych.port1.unref();
  const yieldTask = (f) => { if (ych.port1.ref) ych.port1.ref(); yq.push(f); ych.port2.postMessage(0); };

  let ctx, orig;
  const ext = { workers, extend, stats: { violations: 0 } };

  function extend(host, c) {
    orig = { ...host };
    ctx = c;
    return { ...host, spawn, wake, reap };
  }

  function attach(W, memory, kind) {
    const k = ctx.k, table = k.__indirect_function_table;
    const imports = { k: { mem: k.memory }, p: { mem: memory } };
    if (kind !== 'A') imports.r = { o: (p, n) => W.outs.push(p, n), i: (p, n) => checkRead(W, p, n) };
    const inst = new WebAssembly.Instance(shim(kind), imports);
    const slot = freeSlots.length ? freeSlots.pop() : table.grow(3);
    W.slot = slot;
    table.set(slot, inst.exports.i); table.set(slot + 1, inst.exports.o); table.set(slot + 2, inst.exports.s);
    k.proc_attach(W.pid, slot, slot + 1, slot + 2);
  }

  function spawn(pid, ptr, len, cacheable) {
    const k = ctx.k;
    const key = ptr + ':' + len;
    let entry = cacheable ? modules.get(key) : undefined;
    if (!entry) {
      const bytes = new Uint8Array(k.memory.buffer, ptr, len);
      let mode = null;
      try { mode = classify(bytes); } catch { mode = null; }
      entry = { mode, mod: mode ? WebAssembly.compile(bytes.slice()) : null };
      if (cacheable) modules.set(key, entry);
    }
    if (!entry.mode) return orig.spawn(pid, ptr, len, cacheable);
    const P = { pid, dead: false, wake: null, timer: 0 };
    ctx.procs.set(pid, P);                 // so host.reap and the idle check see it
    const W = { pid, P, mode: entry.mode, worker: null, slot: undefined, killed: false, done: false, outs: [], sent: null,
      t: { spawn: now() }, nsys: 0, bytesIn: 0, bytesOut: 0 };
    workers.set(pid, W);
    entry.mod.then((mod) => startWorker(W, mod), (e) => { if (!P.dead) k.proc_crashed(pid, 4); onEvent('loadfail', W, e); });
  }

  function startWorker(W, mod) {
    if (W.killed || W.P.dead) return;
    const k = ctx.k;
    W.worker = makeWorker(workerUrl);
    W.worker.on((m) => message(W, m));
    W.t.worker = now();
    if (W.mode === 'B') {
      W.proxy = new WebAssembly.Memory({ initial: 0 });
      W.proxyPages = 0;
      attach(W, W.proxy, check ? 'Bc' : 'B');
      W.worker.post({ op: 'start', mode: 'B', module: mod, pid: W.pid, drop: dropNr });
    } else {
      W.ctlBuf = new SharedArrayBuffer(64);
      W.ctl = new Int32Array(W.ctlBuf);
      W.worker.post({ op: 'start', mode: 'A', module: mod, pid: W.pid, ctl: W.ctlBuf, spin: spinA });
    }
    W.t0 = now();
    k.proc_resumed(W.pid, W.t0);
  }

  function message(W, m) {
    if (W.killed) return;
    const k = ctx.k;
    switch (m.op) {
      case 'mem':                          // A: the program's shared memory
        W.memory = m.memory;
        attach(W, m.memory, 'A');
        W.t.ready = now();
        serveA(W);
        break;
      case 'ready':
        W.t.ready = now();
        break;
      case 'sys':
        serveB(W, m);
        break;
      case 'exit':
        W.done = true;
        release(W);
        W.worker.terminate();
        onEvent('exit', W);
        break;
      case 'crash':
        if (!W.P.dead && k.proc_state(W.pid) === P_RUN) k.proc_crashed(W.pid, m.load ? 4 : 11);
        W.done = true;
        release(W);
        W.worker.terminate();
        onEvent('crash', W, m.msg);
        break;
    }
  }

  // ---- A: serve the control block. The main thread never blocks: Atomics.waitAsync. ----
  async function serveA(W) {
    const ctl = W.ctl, k = ctx.k, pid = W.pid;
    let since = now();
    for (;;) {
      if (W.killed || W.done) return;
      const v = Atomics.load(ctl, ST);
      if (v !== REQ) {
        // Spinning can catch call after call without returning to the event loop. Give the
        // page (input, frames, other processes) a turn at least every 4 ms.
        if (mainSpinUs > 0 && now() - since > 4) { await new Promise((r) => yieldTask(r)); since = now(); }
        if (mainSpinUs > 0) {              // optional: catch the next call without a task hop
          const end = now() + mainSpinUs / 1000;
          while (Atomics.load(ctl, ST) !== REQ && now() < end);
          if (Atomics.load(ctl, ST) === REQ) { W.spinHits = (W.spinHits || 0) + 1; continue; }
        }
        W.asyncWaits = (W.asyncWaits || 0) + 1;
        const w = Atomics.waitAsync(ctl, ST, v);
        if (w.async) { await w.value; since = now(); }
        continue;
      }
      if (!W.t.first) W.t.first = now();
      W.nsys++;
      if (ctl[OP] === OP_SYS) {
        const r = k.syscall_pid(pid, ctl[NR], ctl[ARG], ctl[ARG + 1], ctl[ARG + 2], ctl[ARG + 3], ctl[ARG + 4], ctl[ARG + 5]);
        ctl[RES] = r;
        // Arm the park NOW. On one thread, block() runs in the same turn as this return,
        // so no wake-up can fall in between. Here the worker's block request arrives later,
        // and a wake-up in that gap would be ignored and lost.
        if (r === ERESTARTSYS) W.parked = park(W);
      } else {
        W.parks = (W.parks || 0) + 1;
        const p = W.parked || park(W);
        W.parked = null;
        ctl[RES] = (await p) ? 0 : 1;
        since = now();
      }
      if (W.killed) return;
      Atomics.store(ctl, ST, DONE);
      Atomics.notify(ctl, ST);
    }
  }

  // ---- B: one message per call; the proxy memory stands in for the program's memory ----
  function serveB(W, m) {
    const k = ctx.k;
    if (!W.t.first) W.t.first = now();
    W.nsys++;
    if (m.pages > W.proxyPages) { W.proxy.grow(m.pages - W.proxyPages); W.proxyPages = m.pages; }
    const u8 = new Uint8Array(W.proxy.buffer);
    if (m.data) {
      let o = 0;
      for (let i = 0; i < m.ranges.length; i += 2) { const p = m.ranges[i], n = m.ranges[i + 1]; u8.set(m.data.subarray(o, o + n), p); o += n; }
      W.bytesIn += m.data.length;
    }
    W.sent = m.ranges;
    W.curNr = m.nr;
    W.outs.length = 0;
    const r = k.syscall_pid(W.pid, m.nr, m.a[0], m.a[1], m.a[2], m.a[3], m.a[4], m.a[5]);
    if (W.killed) return;
    let ranges = null, data = null;
    if (W.outs.length) {
      const src = new Uint8Array(W.proxy.buffer);
      let total = 0;
      for (let i = 1; i < W.outs.length; i += 2) total += W.outs[i];
      data = new Uint8Array(total);
      ranges = Int32Array.from(W.outs);
      let o = 0;
      for (let i = 0; i < ranges.length; i += 2) { data.set(src.subarray(ranges[i], ranges[i] + ranges[i + 1]), o); o += ranges[i + 1]; }
      W.bytesOut += total;
    }
    W.worker.post({ r, ranges, data }, data ? [data.buffer] : []);
    // As in serveA: arm the park in the same turn as the -ERESTARTSYS return. The worker's
    // block() simply waits for the message this sends when the process may run again.
    if (r === ERESTARTSYS) park(W).then((ok) => { if (!W.killed) W.worker.post({ dead: !ok }); });
  }

  // Checking mode for B: the kernel read [p, p+n). Was it sent? A string read may run past
  // what was sent as long as the sent part already contains the NUL.
  function checkRead(W, p, n) {
    const s = W.sent, u8 = new Uint8Array(W.proxy.buffer);
    if (s) for (let i = 0; i < s.length; i += 2) {
      const a = s[i], b = a + s[i + 1];
      if (p < a || p >= b) continue;
      if (p + n <= b) return;
      for (let q = p; q < b; q++) if (!u8[q]) return;
    }
    ext.stats.violations++;
    const msg = `[worker-proto] pid ${W.pid}: system call ${W.curNr} read ${n} bytes at ${p} that the marshalling table did not send\n`;
    ctx.log(msg);
    onEvent('violation', W, msg);
  }

  // ---- blocking: the same logic as host.mjs's block(), run on the main thread ----
  async function park(W) {
    const k = ctx.k, pid = W.pid, P = W.P;
    const alive = () => !P.dead && !W.killed && k.proc_state(pid) === P_RUN;
    if (!alive()) return false;
    k.proc_account(pid, now() - W.t0);
    const ms = k.block_timeout(pid);
    await new Promise((resolve) => {
      P.wake = resolve;
      const fire = () => { if (P.wake === resolve) { P.wake = null; resolve(); } };
      if (ms === 0) yieldTask(fire);
      else if (ms > 0) P.timer = setTimeout(fire, ms);
    });
    clearTimeout(P.timer);
    if (!alive()) return false;
    W.t0 = now();
    k.proc_resumed(pid, W.t0);
    return true;
  }

  function release(W) {                    // let a waiting serveA loop finish
    if (W.ctl) { Atomics.store(W.ctl, ST, GONE); Atomics.notify(W.ctl, ST); }
  }

  function kill(W, why) {
    if (W.killed || W.done) return;
    W.killed = true;
    W.why = why;
    W.t.kill = now();
    release(W);
    if (W.worker) {
      W.worker.terminate();
      W.t.terminated = now();
      if (W.worker.exited) W.worker.exited.then(() => { W.t.exited = now(); onEvent('exited', W); });
      else if (W.memory) watchStop(W);
    }
    onEvent('killed', W, why);
  }

  // Browsers give no event when a terminated Worker has really stopped. For approach A the
  // page can see the program's memory: report when it stops changing (two snapshots 2 ms
  // apart that are equal; the test program writes to it every 0.1 to 0.2 ms while it spins).
  function watchStop(W) {
    const mem = new Uint8Array(W.memory.buffer);
    let prev = mem.slice();
    const poll = () => {
      const cur = mem.slice();
      let same = cur.length === prev.length;
      for (let i = 0; same && i < cur.length; i += 4) if (cur[i] !== prev[i] || cur[i + 1] !== prev[i + 1] || cur[i + 2] !== prev[i + 2] || cur[i + 3] !== prev[i + 3]) same = false;
      if (same) { W.t.exited = now(); onEvent('exited', W); return; }
      prev = cur;
      if (now() - W.t.kill < 5000) setTimeout(poll, 2);
      else onEvent('still-running', W);
    };
    setTimeout(poll, 2);
  }

  function wake(pid) {
    const W = workers.get(pid);
    if (!W) return orig.wake(pid);
    // The kernel wakes a process it has just killed so that its stack can unwind. A process
    // in a Worker may be spinning without ever making another call: stop the Worker now.
    if (ctx.k.proc_state(pid) !== P_RUN) kill(W, 'kernel');
    const P = W.P;
    if (P.wake) { const w = P.wake; P.wake = null; w(); }
  }

  function reap(pid) {
    const W = workers.get(pid);
    if (W) {
      kill(W, 'reap');
      workers.delete(pid);
      if (W.slot !== undefined) freeSlots.push(W.slot);
    }
    return orig.reap(pid);
  }

  return ext;
}
