// SPDX-License-Identifier: GPL-2.0-or-later
// Worker-process prototype: a tiny WebAssembly encoder for the copy shims, and a
// classifier that tells which kind of program a module is. Research code, not used by
// the default boot path. See docs/WORKER-PROCESSES.md.

const leb = (n) => { const out = []; do { let b = n & 0x7f; n >>>= 7; if (n) b |= 0x80; out.push(b); } while (n); return out; };
const name = (s) => [...leb(s.length), ...new TextEncoder().encode(s)];
const vec = (items) => [...leb(items.length), ...items.flat()];
const section = (id, body) => [id, ...leb(body.length), ...body];
const I32 = 0x7f;

// A copy shim like the one in boot/host.mjs (imports kernel memory "k.mem" and process
// memory "p.mem"; exports i(kdst, usrc, len), o(udst, ksrc, len), s() -> pages), with options:
//   shared:  the process memory is a shared memory (approach A: the worker's memory itself)
//   recOut:  o() also calls the import r.o(udst, len) so the host learns which bytes the
//            kernel wrote (approach B: only those bytes travel back to the worker)
//   recIn:   i() also calls r.i(usrc, len) (approach B, checking mode: catches a kernel read
//            of bytes the marshalling table did not send)
export function shimBytes({ shared = false, recOut = false, recIn = false, maxPages = 65536 } = {}) {
  const types = [
    [0x60, ...vec([[I32], [I32], [I32]]), ...vec([])],   // 0: (i32 i32 i32) -> ()
    [0x60, ...vec([]), ...vec([[I32]])],                  // 1: () -> i32
    [0x60, ...vec([[I32], [I32]]), ...vec([])],           // 2: (i32 i32) -> ()
  ];
  const imports = [
    [...name('k'), ...name('mem'), 0x02, 0x00, 0x00],
    shared ? [...name('p'), ...name('mem'), 0x02, 0x03, 0x00, ...leb(maxPages)] : [...name('p'), ...name('mem'), 0x02, 0x00, 0x00],
  ];
  let nfunc = 0, fOut = -1, fIn = -1;
  if (recOut) { imports.push([...name('r'), ...name('o'), 0x00, 0x02]); fOut = nfunc++; }
  if (recIn) { imports.push([...name('r'), ...name('i'), 0x00, 0x02]); fIn = nfunc++; }
  const body = (code) => { const b = [0x00, ...code, 0x0b]; return [...leb(b.length), ...b]; };
  const get = (i) => [0x20, i];
  const copyIn = [...get(0), ...get(1), ...get(2), 0xfc, 0x0a, 0x00, 0x01];   // memory.copy k <- p
  const copyOut = [...get(0), ...get(1), ...get(2), 0xfc, 0x0a, 0x01, 0x00];  // memory.copy p <- k
  const fi = body([...(recIn ? [...get(1), ...get(2), 0x10, ...leb(fIn)] : []), ...copyIn]);
  const fo = body([...copyOut, ...(recOut ? [...get(0), ...get(2), 0x10, ...leb(fOut)] : [])]);
  const fs = body([0x3f, 0x01]);                                               // memory.size p
  const exp = (n, i) => [...name(n), 0x00, ...leb(nfunc + i)];
  return new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    ...section(1, vec(types)),
    ...section(2, vec(imports)),
    ...section(3, vec([[0x00], [0x00], [0x01]])),
    ...section(7, vec([exp('i', 0), exp('o', 1), exp('s', 2)])),
    ...section(10, vec([fi, fo, fs])),
  ]);
}

// What kind of program is this module?
//   'A'    its memory is shared (built with --shared-memory): run it in a Worker that
//          uses the memory directly
//   'B'    it exports `ucos_worker`: an ordinary build marked to run in a Worker, with
//          system calls sent as messages
//   null   an ordinary program: the default one-thread path
// Works on the raw bytes, without compiling, so the default path pays only a section walk.
export function classify(bytes) {
  let p = 8, shared = false, marked = false;
  const u = () => { let r = 0, s = 0, b; do { b = bytes[p++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80); return r >>> 0; };
  while (p < bytes.length) {
    const id = bytes[p++], size = u(), end = p + size;
    if (id === 5) { u(); shared = (bytes[p] & 0x02) !== 0; }
    else if (id === 7) {
      for (let n = u(); n > 0; n--) {
        const len = u();
        if (len === 11 && String.fromCharCode(...bytes.subarray(p, p + 11)) === 'ucos_worker') marked = true;
        p += len + 1;
        u();
      }
    }
    p = end;
  }
  return shared ? 'A' : marked ? 'B' : null;
}
