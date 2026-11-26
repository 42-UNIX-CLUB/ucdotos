// SPDX-License-Identifier: GPL-2.0-or-later
// Host side of the standalone engine (build-NAME/jit/engine.wasm), for Node and browsers.
// It plays the part boot/jit-proto/ext.mjs plays inside UC.OS: `jit_load(ptr, len)` compiles
// the bytes the engine wrote into its own memory and instantiates them against the
// engine's own exports, nothing else.
const dec = new TextDecoder();
const engineModules = new Map();

export async function runEngine(engineBytes, elf, opts, print = () => {}) {
  let mod = engineModules.get(engineBytes);
  if (!mod) { mod = await WebAssembly.compile(engineBytes); engineModules.set(engineBytes, mod); }
  let inst;
  const loads = [];
  const env = {
    jit_load(p, n) {
      const t0 = performance.now();
      try {
        const m = new WebAssembly.Module(new Uint8Array(inst.exports.memory.buffer, p, n));
        new WebAssembly.Instance(m, { e: inst.exports });
        loads.push(performance.now() - t0);
        return 0;
      } catch (e) {
        print(`[jit_load] ${e.message}\n`);
        return -1;
      }
    },
    print(p, n) { print(dec.decode(new Uint8Array(inst.exports.memory.buffer, p, n))); },
    now: () => performance.now(),
    exit(c) { throw new Error('exit ' + c); },
  };
  inst = await WebAssembly.instantiate(mod, { env });
  const put = (bytes) => { const p = inst.exports.sa_alloc(bytes.length + 1); const u = new Uint8Array(inst.exports.memory.buffer); u.set(bytes, p); u[p + bytes.length] = 0; return p; };
  const pe = put(elf);
  const po = put(new TextEncoder().encode(opts));
  let out = '';
  const pr = print;
  print = (s) => { out += s; pr(s); };
  const code = inst.exports.sa_main(pe, elf.length, po, 0);
  return { code, out, loads };
}

// The same C compiled straight to wasm: call run() `reps` times, each timed.
export async function runDirect(bytes, reps, print = () => {}) {
  let inst;
  const env = { print(p, n) { print(dec.decode(new Uint8Array(inst.exports.memory.buffer, p, n))); } };
  inst = await WebAssembly.instantiate(await WebAssembly.compile(bytes), { env });
  const times = [];
  let sum;
  for (let i = 0; i < reps; i++) { const t0 = performance.now(); sum = inst.exports.run(); times.push(performance.now() - t0); }
  return { times, checksum: BigInt.asUintN(64, sum).toString(16).padStart(16, '0') };
}
