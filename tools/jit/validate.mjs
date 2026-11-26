// SPDX-License-Identifier: GPL-2.0-or-later
// Validate and instantiate every .wasm file that tools/jit/xltest wrote.
//   node tools/jit/validate.mjs DIR
import fs from 'node:fs';
const dir = process.argv[2];
let bad = 0, n = 0;
for (const f of fs.readdirSync(dir).filter((x) => x.endsWith('.wasm')).sort()) {
  const bytes = fs.readFileSync(`${dir}/${f}`);
  n++;
  try {
    const mod = new WebAssembly.Module(bytes);
    const memory = new WebAssembly.Memory({ initial: 1100 });
    const table = new WebAssembly.Table({ initial: 10000, element: 'anyfunc' });
    const e = { memory, __indirect_function_table: table, jit_ecall: () => 0, jit_yield: () => 4096, jit_lookup: () => -1,
      jit_stub: () => 5, jit_fma64: (a, b, c) => a * b + c, jit_fma32: (a, b, c) => Math.fround(a * b + c), jit_fphelp: () => 0n };
    new WebAssembly.Instance(mod, { e });
  } catch (err) {
    bad++;
    console.log(`${f}: ${err.message}`);
  }
}
console.log(`validate: ${n} modules, ${bad} failed`);
process.exit(bad ? 1 : 0);
