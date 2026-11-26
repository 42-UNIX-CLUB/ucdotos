// SPDX-License-Identifier: GPL-2.0-or-later
/* Pieces for tools/bench/callcost.mjs. Built twice:
 *   -DCALLER: a "program" whose loop calls an imported system call entry n times.
 *   -DCALLEE: a stand-in kernel whose entry does nothing but return, for the floor. */
#ifdef CALLER
__attribute__((import_module("host"), import_name("syscall"))) long sc7(long, long, long, long, long, long, long);
__attribute__((import_module("host"), import_name("sys8"))) long sc8(int, long, long, long, long, long, long, long);
/* the same shape as libc's __syscall6: retry while the kernel says -ERESTARTSYS */
__attribute__((export_name("loop7"), noinline)) long loop7(int n, long nr) {
  long s = 0, r;
  for (int i = 0; i < n; i++) { while ((r = sc7(nr, 0, 0, 0, 0, 0, 0)) == -512) ; s += r; }
  return s;
}
__attribute__((export_name("loop8"), noinline)) long loop8(int n, int pid, long nr) {
  long s = 0, r;
  for (int i = 0; i < n; i++) { while ((r = sc8(pid, nr, 0, 0, 0, 0, 0, 0)) == -512) ; s += r; }
  return s;
}
#endif
#ifdef CALLEE
__attribute__((export_name("sys"))) long sys(int pid, long nr, long a, long b, long c, long d, long e, long f) { return pid + nr; }
#endif
