// SPDX-License-Identifier: GPL-2.0-or-later
// Binary translator prototype, host side (research code, off by default).
//
// A program that can use generated code exports `jit_attach(slot)` and its function table.
// Right after such a program's instance is created (spawn, exec, clone or migrate: every
// path in boot/host.mjs makes it with `new WebAssembly.Instance`), this extension
//   1. makes a function load(ptr, len) -> 0 | -1 bad module | -2 too large, which compiles
//      `len` bytes at `ptr` in THAT program's own memory and instantiates them with one
//      import namespace, "e" = that program's own exports (nothing else is reachable:
//      not the kernel, not another process);
//   2. wraps it in a tiny wasm function, puts it into the program's table and calls
//      jit_attach(slot) before the program's entry point runs.
// A program started without this extension never gets a loader and keeps interpreting.
//
// It works without changing boot/host.mjs by replacing the global WebAssembly.Instance
// constructor with a subclass for the lifetime of the page (prototype only). The permanent
// form is a few lines in host.mjs's instantiate(): see build-patches/host-jit.patch.
//
//   import { jitExtension } from './jit-proto/ext.mjs';
//   await boot({ ..., extend: jitExtension().extend });
const MAX_MODULE = 4 << 20;   // Chromium refuses synchronous compiles above 8 MB on the main thread

// (module (type (func (param i32 i32) (result i32))) (import "j" "load" (func 0)) (func 1 (type 0) local.get 0 local.get 1 call 0) (export "f" (func 1)))
const WRAPPER = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
  0x02, 0x0a, 0x01, 0x01, 0x6a, 0x04, 0x6c, 0x6f, 0x61, 0x64, 0x00, 0x00,
  0x03, 0x02, 0x01, 0x00,
  0x07, 0x05, 0x01, 0x01, 0x66, 0x00, 0x01,
  0x0a, 0x0a, 0x01, 0x08, 0x00, 0x20, 0x00, 0x20, 0x01, 0x10, 0x00, 0x0b,
]);

export function jitExtension({ debug = false, log = () => {} } = {}) {
  const stats = { attached: 0, loads: 0, bytes: 0, ms: 0, failed: 0 };
  const Real = WebAssembly.Instance;
  let wrapper = null;

  function attach(inst) {
    const ex = inst.exports;
    if (typeof ex.jit_attach !== 'function' || !(ex.__indirect_function_table instanceof WebAssembly.Table) || !ex.memory) return;
    const load = (p, n) => {
      if (n > MAX_MODULE) return -2;
      const t0 = performance.now();
      try {
        const m = new WebAssembly.Module(new Uint8Array(ex.memory.buffer, p, n));
        new Real(m, { e: ex });
        stats.loads++; stats.bytes += n; stats.ms += performance.now() - t0;
        return 0;
      } catch (e) {
        stats.failed++;
        if (debug) log(`[jit] ${e.message}\n`);
        return -1;
      }
    };
    wrapper ||= new WebAssembly.Module(WRAPPER);
    const w = new Real(wrapper, { j: { load } });
    const table = ex.__indirect_function_table;
    const slot = table.grow(1);
    table.set(slot, w.exports.f);
    ex.jit_attach(slot);
    stats.attached++;
  }
  class JitInstance extends Real {
    constructor(module, imports) {
      super(module, imports);
      attach(this);
    }
  }
  function extend(host) {
    WebAssembly.Instance = JitInstance;
    return host;
  }
  return { extend, stats };
}
