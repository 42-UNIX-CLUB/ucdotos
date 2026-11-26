// SPDX-License-Identifier: GPL-2.0-or-later
// The "machine" the kernel runs on. This file is the only JavaScript in the
// system besides the page itself. It does what firmware and a CPU would do:
// load the kernel, start programs (a program is its own WebAssembly instance
// with its own memory), and suspend/resume their stacks. It is never on the
// path of a system call: programs call the kernel's wasm export directly.

const EXIT = Symbol('exit');
const P_RUN = 1;

// Per-process copy shim, hand-assembled. It imports two memories (kernel "k",
// process "p") and exports:  i(kdst, usrc, len)  copy process -> kernel
//                            o(udst, ksrc, len)  copy kernel -> process
//                            s() -> pages        size of the process memory
// This is what lets the kernel reach a program's buffers without JavaScript.
const SHIM = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x0b, 0x02, 0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x00, 0x60, 0x00, 0x01, 0x7f,       // types
  0x02, 0x13, 0x02, 0x01, 0x6b, 0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00, 0x00,             // import k.mem
  0x01, 0x70, 0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00, 0x00,                               // import p.mem
  0x03, 0x04, 0x03, 0x00, 0x00, 0x01,                                                 // functions
  0x07, 0x0d, 0x03, 0x01, 0x69, 0x00, 0x00, 0x01, 0x6f, 0x00, 0x01, 0x01, 0x73, 0x00, 0x02, // exports
  0x0a, 0x20, 0x03,
  0x0c, 0x00, 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0xfc, 0x0a, 0x00, 0x01, 0x0b,       // i: memory.copy k <- p
  0x0c, 0x00, 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0xfc, 0x0a, 0x01, 0x00, 0x0b,       // o: memory.copy p <- k
  0x04, 0x00, 0x3f, 0x01, 0x0b,                                                       // s: memory.size p
]);

// Per-process syscall trampoline, hand-assembled. It imports the kernel's entry
// point k.sys(pid, nr, a..f) and a constant global p.pid, and exports
//   c(nr, a, b, c, d, e, f) = k.sys(pid, nr, a, b, c, d, e, f)
// A program's system call is: program -> trampoline -> kernel, all WebAssembly.
// The pid is fixed by the host when the process is created, so a program cannot
// pretend to be another process, and the kernel always knows who is calling.
const TRAMPOLINE = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x18, 0x02,
  0x60, 0x07, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f,                   // type 0: 7 x i32 -> i32
  0x60, 0x08, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f,             // type 1: 8 x i32 -> i32
  0x02, 0x12, 0x02,
  0x01, 0x6b, 0x03, 0x73, 0x79, 0x73, 0x00, 0x01,                                     // import k.sys: func type 1
  0x01, 0x70, 0x03, 0x70, 0x69, 0x64, 0x03, 0x7f, 0x00,                               // import p.pid: const i32 global
  0x03, 0x02, 0x01, 0x00,                                                             // one function, type 0
  0x07, 0x05, 0x01, 0x01, 0x63, 0x00, 0x01,                                           // export "c" = function 1
  0x0a, 0x16, 0x01, 0x14, 0x00,
  0x23, 0x00, 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0x20, 0x03, 0x20, 0x04, 0x20, 0x05, 0x20, 0x06, // global.get pid; local.get 0..6
  0x10, 0x00, 0x0b,                                                                   // call k.sys; end
]);

export function checkSupport() {
  const missing = [];
  if (typeof WebAssembly !== 'object') missing.push('WebAssembly');
  else {
    if (typeof WebAssembly.Suspending !== 'function') missing.push('WebAssembly JSPI (Chrome 137+, Firefox 153+, Safari 27+)');
    if (!WebAssembly.validate(SHIM)) missing.push('WebAssembly multi-memory');
  }
  return missing;
}

// extend: research hook, off by default. extend(hostImports, ctx) returns the imports the
// kernel gets instead (boot/worker-proto/ext.mjs uses it to run some processes in Workers).
export async function boot({ kernel, initrd, cmdline = '', screen = null, log = () => {}, onIdle = () => {}, onFrame = () => {}, debug = false, extend = null }) {
  const dec = new TextDecoder();
  // What the device reports, for sysinfo. Browsers that do not say get a 4 GiB default.
  const deviceMemory = ((globalThis.navigator && globalThis.navigator.deviceMemory) || 4) * 2 ** 30;
  // yieldTask(f): run f after one turn of the event loop, without the 4 ms clamp that
  // browsers put on repeated setTimeout(0). Used when the kernel preempts a process.
  const yieldQueue = [];
  const yieldChannel = new MessageChannel();
  // A process that keeps yielding must not starve sleeping ones: some event loops serve a
  // steady stream of messages ahead of timers. So every yield first wakes sleepers that are due.
  const sleepers = new Set();
  yieldChannel.port1.onmessage = () => {
    if (sleepers.size) { const now = performance.now(); for (const S of sleepers) if (S.deadline <= now) S.fire(); }
    const f = yieldQueue.shift();
    if (f) f();
    if (!yieldQueue.length && port.unref) port.unref(); // Node: do not keep the process alive when idle
  };
  const port = yieldChannel.port1;
  if (port.unref) port.unref();
  const yieldTask = (f) => { if (port.ref) port.ref(); yieldQueue.push(f); yieldChannel.port2.postMessage(0); };
  const shimModule = await WebAssembly.compile(SHIM);
  const trampModule = await WebAssembly.compile(TRAMPOLINE);
  const procs = new Map();        // pid -> { pid, dead, wake, timer, slot }
  const modules = new Map();      // cache of compiled programs
  const freeSlots = [];
  let k, table;
  const mem = () => new Uint8Array(k.memory.buffer);
  const rnd = new Uint32Array(1);

  function stop(P) {              // let a parked stack unwind
    P.dead = true;
    if (P.wake) { const w = P.wake; P.wake = null; w(); }
  }
  const host = {
    log(p, n) { log(dec.decode(mem().slice(p, p + n), { stream: true })); },
    // Passed bound, not wrapped in an arrow function: Chromium then calls it from wasm about
    // 2.5 times faster (120 ns instead of 330 ns), which is most of the cost of clock_gettime.
    now: performance.now.bind(performance),
    epoch: () => Date.now(),
    random() { crypto.getRandomValues(rnd); return rnd[0]; },
    memory: () => deviceMemory,
    fb_dirty() { onFrame(); },
    wake(pid) {
      const P = procs.get(pid);
      if (P && P.wake) { const w = P.wake; P.wake = null; w(); }
    },
    reap(pid) {
      const P = procs.get(pid);
      if (!P) return;
      procs.delete(pid);
      stop(P);
      if (P.slot !== undefined) freeSlots.push(P.slot);
      if (procs.size === 0) queueMicrotask(() => { if (procs.size === 0) onIdle(); });
    },
    spawn(pid, ptr, len, cacheable) {
      const P = { pid, dead: false, wake: null, timer: 0, slot: undefined };
      procs.set(pid, P);
      const key = ptr + ':' + len;
      let compiled = cacheable ? modules.get(key) : undefined;
      if (!compiled) {
        compiled = WebAssembly.compile(mem().slice(ptr, ptr + len));
        if (cacheable) modules.set(key, compiled);
      }
      compiled.then((mod) => start(P, mod), (e) => { if (debug) log(`[host] pid ${pid}: ${e.message}\n`); if (!P.dead) k.proc_crashed(pid, 4); });
    },
  };

  async function start(P, mod) {
    if (P.dead) return;
    const pid = P.pid;
    const alive = () => !P.dead && k.proc_state(pid) === P_RUN;
    // block(): park this program's stack until the kernel wakes it or a timeout passes.
    const block = new WebAssembly.Suspending(async () => {
      if (!alive()) throw EXIT;
      k.proc_account(pid, performance.now() - P.t0);
      const ms = k.block_timeout(pid);
      await new Promise((resolve) => {
        P.wake = resolve;
        const fire = () => { if (P.wake === resolve) { P.wake = null; resolve(); } };
        if (ms === 0) yieldTask(fire);
        else if (ms > 0) { P.deadline = performance.now() + ms; P.fire = fire; sleepers.add(P); P.timer = setTimeout(fire, ms); }
      });
      sleepers.delete(P);
      clearTimeout(P.timer);
      if (!alive()) throw EXIT;
      k.set_current(pid);
      P.t0 = performance.now();
      k.proc_resumed(pid, P.t0);
    });
    let inst;
    try {
      const tramp = await WebAssembly.instantiate(trampModule, { k: { sys: k.syscall_pid }, p: { pid } });
      inst = await WebAssembly.instantiate(mod, { host: { syscall: tramp.exports.c, block } });
      if (P.dead) return;
      const shim = await WebAssembly.instantiate(shimModule, { k: { mem: k.memory }, p: { mem: inst.exports.memory } });
      if (P.dead) return;
      const slot = freeSlots.length ? freeSlots.pop() : table.grow(3);
      P.slot = slot;
      table.set(slot, shim.exports.i);
      table.set(slot + 1, shim.exports.o);
      table.set(slot + 2, shim.exports.s);
      k.proc_attach(pid, slot, slot + 1, slot + 2);
    } catch (e) {
      if (debug) log(`[host] pid ${pid} failed to load: ${e.message}\n`);
      if (!P.dead) k.proc_crashed(pid, 4);
      return;
    }
    k.set_current(pid);
    P.t0 = performance.now();
    k.proc_resumed(pid, P.t0);
    try {
      await WebAssembly.promising(inst.exports._start)();
    } catch (e) {
      if (e !== EXIT) {           // a wasm trap: bad memory access, unreachable, stack overflow
        if (debug) log(`[host] pid ${pid} trapped: ${e && e.message}\n`);
        if (!P.dead) k.proc_crashed(pid, 11);
      }
    }
  }

  const imports = extend ? extend(host, { get k() { return k; }, procs, log, debug }) : host;
  const { instance } = await WebAssembly.instantiate(kernel, { host: imports });
  k = instance.exports;
  table = k.__indirect_function_table;
  const put = (bytes) => { const p = k.boot_alloc(bytes.length); mem().set(bytes, p); return p; };
  const initrdPtr = put(initrd);
  const cmdPtr = put(new TextEncoder().encode(cmdline + '\0'));
  if (screen) k.fb_resize(screen.width, screen.height);
  const rc = k.kmain(initrdPtr, initrd.length, cmdPtr);
  return {
    k, rc, procs,
    memory: k.memory,
    // Screen: the kernel owns the pixels; the host only shows them.
    frame() { return { addr: k.fb_addr(), width: k.fb_width(), height: k.fb_height() }; },
    // The rectangle written since the last call, { x, y, w, h } (w = 0: nothing), and a new one starts.
    damage() {
      const d = new Uint32Array(k.memory.buffer, k.fb_damage(), 4);
      return d[0] > d[2] ? { x: 0, y: 0, w: 0, h: 0 } : { x: d[0], y: d[1], w: d[2] - d[0] + 1, h: d[3] - d[1] + 1 };
    },
    resize(width, height) { k.fb_resize(width, height); },
    // Input: Linux evdev triples (type, code, value); a batch ends with (0, 0, 0).
    input(type, code, value) { k.input_event(type, code, value); },
    consoleInput(str) { for (const b of new TextEncoder().encode(str)) k.console_input(b); },
    consoleEOF() { k.console_input(-1); },
  };
}
