// SPDX-License-Identifier: GPL-2.0-or-later
// A tiny stand-in for the UC.OS kernel, for testing compiled programs quickly under
// Node: it implements just the system calls simple test programs make (arguments,
// read, write, files, exit, clock). The real check is running them in the OS image
// with tools/run.mjs; this is for the edit-compile-test loop.
// usage: node run.mjs program.wasm [args...]
import fs from 'node:fs';

const [, , path, ...args] = process.argv;
const mod = new WebAssembly.Module(fs.readFileSync(path));
let mem;
const u8 = () => new Uint8Array(mem.buffer);
const dv = () => new DataView(mem.buffer);
const str = (p) => { const b = u8(); let e = p; while (b[e]) e++; return Buffer.from(b.subarray(p, e)).toString(); };
const fds = new Map([[0, 0], [1, 1], [2, 2]]);
let nextfd = 3;
class Exit { constructor(c) { this.code = c; } }
const EBADF = 9, ENOENT = 2, ENOTTY = 25, ENOSYS = 38;
const out = [[], [], []];
function flushAll() {
  for (const fd of [1, 2]) if (out[fd].length) { fs.writeSync(fd, Buffer.concat(out[fd])); out[fd] = []; }
}
function sys(nr, a, b, c, d, e, f) {
  switch (nr) {
    case 501: { // getargs(buf, cap)
      const argv = [path, ...args];
      const parts = [Buffer.alloc(8)];
      parts[0].writeUInt32LE(argv.length, 0);
      parts[0].writeUInt32LE(0, 4);
      for (const s of argv) parts.push(Buffer.from(s + '\0'));
      const blob = Buffer.concat(parts);
      u8().set(blob.subarray(0, b), a >>> 0);
      return Math.min(blob.length, b);
    }
    case 64: { // write
      const fd = fds.get(a);
      if (fd === undefined) return -EBADF;
      const data = Buffer.from(u8().slice(b >>> 0, (b >>> 0) + c));
      if (fd === 1 || fd === 2) { out[fd].push(data); if (out[fd].length > 64) flushAll(); return c; }
      return fs.writeSync(fd, data);
    }
    case 63: { // read
      const fd = fds.get(a);
      if (fd === undefined) return -EBADF;
      const buf = Buffer.alloc(c);
      let n;
      try { n = fs.readSync(fd, buf, 0, c, null); } catch (err) { if (err.code === 'EOF') n = 0; else if (err.code === 'EAGAIN') n = 0; else throw err; }
      u8().set(buf.subarray(0, n), b >>> 0);
      return n;
    }
    case 56: { // openat(dirfd, path, flags, mode)
      const p = str(b >>> 0);
      let fl = 'r';
      const acc = c & 3;
      if (acc === 1) fl = (c & 0x400) ? 'a' : 'w';
      else if (acc === 2) fl = (c & 0x40) ? ((c & 0x200) ? 'w+' : 'a+') : 'r+';
      try { const h = fs.openSync(p, fl); const fd = nextfd++; fds.set(fd, h); return fd; } catch { return -ENOENT; }
    }
    case 57: { const h = fds.get(a); if (h === undefined) return -EBADF; if (h > 2) fs.closeSync(h); fds.delete(a); return 0; }
    case 29: return -ENOTTY;
    case 62: return -29; // lseek: ESPIPE
    case 93: case 94: throw new Exit(a);
    case 113: { // clock_gettime(clk, ts)
      const t = process.hrtime.bigint();
      dv().setBigInt64(b >>> 0, t / 1000000000n, true);
      dv().setBigInt64((b >>> 0) + 8, t % 1000000000n, true);
      return 0;
    }
    case 172: return 1;
    case 173: return 0;
    case 278: { for (let i = 0; i < b; i++) u8()[(a >>> 0) + i] = Math.random() * 256; return b; }
    case 80: case 79: return -ENOSYS;
    case 503: return 0;
    default: return -ENOSYS;
  }
}
const inst = new WebAssembly.Instance(mod, { host: { syscall: sys, block() {} } });
mem = inst.exports.memory;
let code = 0;
try { inst.exports._start(); } catch (err) {
  if (err instanceof Exit) code = err.code;
  else { flushAll(); console.error('trap: ' + err.message); code = 139; }
}
flushAll();
process.exit(code & 255);
