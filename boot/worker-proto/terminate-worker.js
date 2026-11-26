// SPDX-License-Identifier: GPL-2.0-or-later
// Worker for terminate.mjs. The wasm module (hand-assembled) imports a shared memory and
// exports spin(): loop { mem[0] += 1 } forever, with no call inside the loop.
const bytes = new Uint8Array([0,97,115,109,1,0,0,0, 1,4,1,96,0,0, 2,9,1,1,101,1,109,2,3,1,1, 3,2,1,0, 7,8,1,4,115,112,105,110,0,0,
 10,22,1,20,0, 3,64, 65,0, 65,0, 40,2,0, 65,1, 106, 54,2,0, 12,0, 11, 11]);
onmessage = (e) => {
  const [m, kind] = e.data;
  if (kind === 'js') { const v = new Int32Array(m.buffer); postMessage('go'); for (;;) v[0]++; }
  const inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), { e: { m } }); postMessage('go'); inst.exports.spin();
};
