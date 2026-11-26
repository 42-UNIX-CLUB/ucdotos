// SPDX-License-Identifier: GPL-2.0-or-later
// Worker-process prototype, approach B (no shared memory): which bytes of the program's
// memory a system call READS. The worker sends exactly these bytes with the call. Bytes the
// kernel WRITES are not in this table: the proxy shim records every copy_to_user, so only
// those ranges travel back. Research code; see docs/WORKER-PROCESSES.md.
//
// Each entry returns a list of [address, length] ranges for the call's arguments a[0..5].
// A string is sent up to and including its NUL (at most PATH_MAX bytes).

const PATH_MAX = 256, TIMESPEC = 16, TERMIOS = 36, WINSIZE = 8, POLLFD = 8;

export const SYS = {
  ioctl: 29, mkdirat: 34, unlinkat: 35, renameat: 38, utimensat: 88, chdir: 49,
  openat: 56, write: 64, pwrite64: 68, ppoll: 73, newfstatat: 79, nanosleep: 101,
  bind: 200, connect: 203, spawn: 500,
};

// helpers, given the program's memory as a Uint8Array and a DataView
const str = (u8, p, cap = PATH_MAX) => {
  if (!p) return [];
  const end = Math.min(u8.length, p + cap);
  let q = p;
  while (q < end && u8[q]) q++;
  return [[p, Math.min(q + 1, end) - p]];
};
const buf = (p, n) => (p && n > 0 ? [[p, n]] : []);
// argv / envp: a NULL-terminated array of pointers, then each string (spawn caps the blob at 64 KiB)
const strvec = (u8, dv, p) => {
  if (!p) return [];
  const out = [];
  let i = 0;
  for (;; i++) {
    const s = dv.getUint32(p + i * 4, true);
    if (!s) break;
    out.push(...str(u8, s, 65536));
  }
  out.push([p, (i + 1) * 4]);
  return out;
};

export const TABLE = {
  [SYS.write]: (a) => buf(a[1], a[2]),
  [SYS.pwrite64]: (a) => buf(a[1], a[2]),
  [SYS.openat]: (a, u8) => str(u8, a[1]),
  [SYS.newfstatat]: (a, u8) => str(u8, a[1]),
  [SYS.mkdirat]: (a, u8) => str(u8, a[1]),
  [SYS.unlinkat]: (a, u8) => str(u8, a[1]),
  [SYS.utimensat]: (a, u8) => str(u8, a[1]),
  [SYS.chdir]: (a, u8) => str(u8, a[0]),
  [SYS.renameat]: (a, u8) => [...str(u8, a[1]), ...str(u8, a[3])],
  [SYS.nanosleep]: (a) => buf(a[0], TIMESPEC),
  [SYS.ppoll]: (a) => [...buf(a[0], a[1] * POLLFD), ...buf(a[2], TIMESPEC)],
  [SYS.bind]: (a) => buf(a[1], a[2]),
  [SYS.connect]: (a) => buf(a[1], a[2]),
  [SYS.spawn]: (a, u8, dv) => [...str(u8, a[0]), ...strvec(u8, dv, a[1]), ...strvec(u8, dv, a[2]), ...buf(a[3], 12)],
  [SYS.ioctl]: (a) => {
    switch (a[1] >>> 0) {
      case 0x5402: case 0x5403: case 0x5404: return buf(a[2], TERMIOS); // TCSETS, TCSETSW, TCSETSF
      case 0x5414: return buf(a[2], WINSIZE);                           // TIOCSWINSZ
      case 0x5410: return buf(a[2], 4);                                 // TIOCSPGRP
      default: return [];
    }
  },
};

// Ranges to send for one call, clipped to the memory (the kernel returns -EFAULT for the rest).
export function inputs(nr, a, u8, dv) {
  const f = TABLE[nr];
  if (!f) return [];
  const out = [];
  for (const [p, n] of f(a, u8, dv)) {
    const p0 = p >>> 0;
    if (p0 >= u8.length) continue;
    out.push([p0, Math.min(n, u8.length - p0)]);
  }
  return out;
}
