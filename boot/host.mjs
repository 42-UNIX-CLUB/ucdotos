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

// Memory copier for fork's clone, hand-assembled. It imports the parent's memory "s" (0) and
// the child's "d" (1) and exports c(bytes): for each 4 KiB page, if the parent's page holds a
// non-zero byte, memory.copy it. The child's fresh memory is already zero, and a page that is
// never written is never faulted in by the OS: on a small program (whose 1 MiB stack region is
// mostly zero) this makes the copy about three times cheaper; on a full memory it costs the same
// as a plain copy (PERFORMANCE.md section 12).
const ZCOPY_BODY = [
  0x02, 0x02, 0x7f, 0x01, 0x7e,                                     // locals p, q: i32; (unused) i64
  0x03, 0x40,                                                       // loop pages
  0x02, 0x40,                                                       //  block skip
  0x02, 0x40,                                                       //   block nonzero
  0x20, 0x01, 0x21, 0x02,                                           //    q = p
  0x03, 0x40,                                                       //    loop words
  0x20, 0x02, 0x29, 0x03, 0x00, 0x20, 0x02, 0x29, 0x03, 0x08, 0x84, //     i64 at q | q+8
  0x20, 0x02, 0x29, 0x03, 0x10, 0x84, 0x20, 0x02, 0x29, 0x03, 0x18, 0x84, // | q+16 | q+24
  0x50, 0x45, 0x0d, 0x01,                                           //     non-zero: br nonzero
  0x20, 0x02, 0x41, 0x20, 0x6a, 0x22, 0x02,                         //     q += 32
  0x20, 0x01, 0x41, 0x80, 0x20, 0x6a, 0x49, 0x0d, 0x00,             //     q < p + 4096: again
  0x0b, 0x0c, 0x01,                                                 //    all zero: br skip
  0x0b,
  0x20, 0x01, 0x20, 0x01, 0x41, 0x80, 0x20, 0xfc, 0x0a, 0x01, 0x00, //   memory.copy d[p] <- s[p], 4096
  0x0b,
  0x20, 0x01, 0x41, 0x80, 0x20, 0x6a, 0x22, 0x01,                   //  p += 4096
  0x20, 0x00, 0x49, 0x0d, 0x00,                                     //  p < bytes: again
  0x0b, 0x0b,
];
const ZCOPY = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x05, 0x01, 0x60, 0x01, 0x7f, 0x00,                                           // type (i32) -> ()
  0x02, 0x13, 0x02, 0x01, 0x73, 0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00, 0x00,             // import s.mem
  0x01, 0x64, 0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00, 0x00,                               // import d.mem
  0x03, 0x02, 0x01, 0x00, 0x07, 0x05, 0x01, 0x01, 0x63, 0x00, 0x00,                   // func c, exported
  0x0a, ZCOPY_BODY.length + 2, 0x01, ZCOPY_BODY.length, ...ZCOPY_BODY,
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
export async function boot({ kernel, initrd, cmdline = '', screen = null, log = () => {}, onIdle = () => {}, onFrame = () => {}, debug = false, extend = null, store = null, storeFlags = 0 }) {
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
  const zcopyModule = await WebAssembly.compile(ZCOPY);
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
    // persistence (boot/store.mjs); without a store the kernel never calls these
    ...(store ? store.imports : { store_kick() {}, store_fetch() {}, store_wipe() {} }),
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
      load(P, ptr, len, cacheable);
    },
    // execve: the same pid gets a new program. The old instance is inside this very system
    // call; the kernel returns -ERESTARTSYS to it, and its block() finds it dead and throws
    // EXIT, which ends its stack without running any more of its code. Its memory goes with it.
    exec(pid, ptr, len, cacheable) {
      const old = procs.get(pid);
      const P = { pid, dead: false, wake: null, timer: 0, slot: undefined };
      if (old) { P.slot = old.slot; old.slot = undefined; stop(old); }
      procs.set(pid, P);
      load(P, ptr, len, cacheable);
    },
    // fork's primitive: a new instance of the parent's module, with a copy of its memory and
    // of its exported mutable globals, made right now (the parent is inside this call, so its
    // memory does not change). The child then starts at its export __fork_child.
    // Returns 0, or -errno: -ENOEXEC if the program has no __fork_child, -ENOMEM if it failed.
    clone(ppid, cpid) {
      const PP = procs.get(ppid);
      if (!PP || !PP.inst || typeof PP.inst.exports.__fork_child !== 'function') return -8;
      const P = { pid: cpid, dead: false, wake: null, timer: 0, slot: undefined };
      try {
        instantiate(P, PP.mod, new WebAssembly.Instance(trampModule, { k: { sys: k.syscall_pid }, p: { pid: cpid } }));
        const src = PP.inst.exports.memory, dst = P.inst.exports.memory;
        const grow = (src.buffer.byteLength - dst.buffer.byteLength) / 65536;
        if (grow > 0) dst.grow(grow);
        const bytes = src.buffer.byteLength;
        if (bytes <= 0xfffff000) new WebAssembly.Instance(zcopyModule, { s: { mem: src }, d: { mem: dst } }).exports.c(bytes);
        else new Uint8Array(dst.buffer).set(new Uint8Array(src.buffer)); // a full 4 GiB memory: c() takes a 32-bit size
        for (const name of mutableGlobals(PP)) P.inst.exports[name].value = PP.inst.exports[name].value;
        attach(P);
      } catch (e) {
        if (debug) log(`[host] clone of pid ${ppid} failed: ${e && e.message}\n`);
        if (P.slot !== undefined) freeSlots.push(P.slot);
        return -12;
      }
      procs.set(cpid, P);
      queueMicrotask(() => run(P, '__fork_child'));
      return 0;
    },
  };
  // migrate: the same pid continues in a new instance with a fresh memory of the same size,
  // holding only the ranges the process listed (u32 start, length pairs at ptr in its memory).
  // Outside them it holds what a new instance holds: the module's data segments, else zero.
  // The old instance is inside this call; like exec, it is thrown away when it asks to wait.
  // Returns 0, or -errno and nothing changed: -ENOEXEC without __migrate_resume, -EFAULT for a
  // range outside memory, -ENOMEM if the new instance cannot be made.
  host.migrate = (pid, ptr, n) => {
    const PP = procs.get(pid);
    if (!PP || !PP.inst || typeof PP.inst.exports.__migrate_resume !== 'function') return -8;
    const src = PP.inst.exports.memory, bytes = src.buffer.byteLength;
    const r = new Uint32Array(src.buffer, ptr, n * 2).slice();
    for (let i = 0; i < n; i++) if (r[2 * i] + r[2 * i + 1] > bytes) return -14;
    const P = { pid, dead: false, wake: null, timer: 0, slot: undefined };
    try {
      instantiate(P, PP.mod, new WebAssembly.Instance(trampModule, { k: { sys: k.syscall_pid }, p: { pid } }));
      const dst = P.inst.exports.memory;
      const grow = (bytes - dst.buffer.byteLength) / 65536;
      if (grow > 0) dst.grow(grow);
      const s8 = new Uint8Array(src.buffer), d8 = new Uint8Array(dst.buffer);
      for (let i = 0; i < n; i++) if (r[2 * i + 1]) d8.set(s8.subarray(r[2 * i], r[2 * i] + r[2 * i + 1]), r[2 * i]);
      for (const name of mutableGlobals(PP)) P.inst.exports[name].value = PP.inst.exports[name].value;
      P.slot = PP.slot;
      attach(P);
    } catch (e) {
      if (debug) log(`[host] migrate of pid ${pid} failed: ${e && e.message}\n`);
      return -12;
    }
    PP.slot = undefined;
    stop(PP);
    procs.set(pid, P);
    queueMicrotask(() => run(P, '__migrate_resume'));
    return 0;
  };
  function load(P, ptr, len, cacheable) {
    const key = ptr + ':' + len;
    let compiled = cacheable ? modules.get(key) : undefined;
    if (!compiled) {
      compiled = WebAssembly.compile(mem().slice(ptr, ptr + len));
      if (cacheable) modules.set(key, compiled);
    }
    compiled.then((mod) => start(P, mod), (e) => { if (debug) log(`[host] pid ${P.pid}: ${e.message}\n`); if (!P.dead) k.proc_crashed(P.pid, 4); });
  }
  // The names of the exported globals that can be written (the stack pointer, at least, when
  // the program is linked with --export=__stack_pointer), found once per module.
  const globalNames = new WeakMap();
  function mutableGlobals(P) {
    let names = globalNames.get(P.mod);
    if (!names) {
      names = [];
      for (const [name, v] of Object.entries(P.inst.exports)) {
        if (!(v instanceof WebAssembly.Global)) continue;
        try { v.value = v.value; names.push(name); } catch { /* immutable */ }
      }
      globalNames.set(P.mod, names);
    }
    return names;
  }

  function makeBlock(P) {
    const pid = P.pid;
    const alive = () => !P.dead && k.proc_state(pid) === P_RUN;
    // block(): park this program's stack until the kernel wakes it or a timeout passes.
    return new WebAssembly.Suspending(async () => {
      if (!alive()) throw EXIT;
      k.proc_account(pid, performance.now() - P.t0);
      const ms = k.block_timeout(pid);
      if (ms !== -2) await new Promise((resolve) => { // -2: do not wait (a signal handler is to run)
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
  }
  // The program instance (synchronous: clone needs it within the parent's system call).
  function instantiate(P, mod, tramp) {
    P.mod = mod;
    P.inst = new WebAssembly.Instance(mod, { host: { syscall: tramp.exports.c, block: makeBlock(P) } });
  }
  // The copy shim, in the kernel's table (an exec reuses the slots of the program it replaces).
  function attach(P) {
    const shim = new WebAssembly.Instance(shimModule, { k: { mem: k.memory }, p: { mem: P.inst.exports.memory } });
    const slot = P.slot !== undefined ? P.slot : freeSlots.length ? freeSlots.pop() : table.grow(3);
    P.slot = slot;
    table.set(slot, shim.exports.i);
    table.set(slot + 1, shim.exports.o);
    table.set(slot + 2, shim.exports.s);
    k.proc_attach(P.pid, slot, slot + 1, slot + 2);
  }
  async function start(P, mod) {
    if (P.dead) return;
    const pid = P.pid;
    try {
      const tramp = await WebAssembly.instantiate(trampModule, { k: { sys: k.syscall_pid }, p: { pid } });
      if (P.dead) return;
      instantiate(P, mod, tramp);
      attach(P);
    } catch (e) {
      if (debug) log(`[host] pid ${pid} failed to load: ${e.message}\n`);
      if (!P.dead) k.proc_crashed(pid, 4);
      return;
    }
    await run(P, '_start');
  }
  // Run the program from an entry point (_start, or __fork_child in a cloned process).
  async function run(P, entry) {
    if (P.dead) return;
    const pid = P.pid;
    k.set_current(pid);
    P.t0 = performance.now();
    k.proc_resumed(pid, P.t0);
    try {
      await WebAssembly.promising(P.inst.exports[entry])();
      // An entry point must not return; one that does has nowhere to go.
      if (!P.dead && k.proc_state(pid) === P_RUN) k.proc_crashed(pid, 11);
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
  if (store) store.preboot(k, put, storeFlags); // the stored layer goes over the image inside kmain, before init starts
  const rc = k.kmain(initrdPtr, initrd.length, cmdPtr);
  if (store) store.started();
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
