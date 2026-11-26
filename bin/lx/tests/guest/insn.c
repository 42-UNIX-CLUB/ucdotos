// SPDX-License-Identifier: GPL-2.0-or-later
/* Self-checking tests of instructions the C differential test cannot reach: the FP
 * control and status register, static rounding modes, LR/SC, fence.i after writing code,
 * FP flags, NaN boxing. riscv64 only. Prints "insn: N checks, 0 failed". */
#include "rt.h"

static int pass, fail;
static void check(const char *name, uint64_t got, uint64_t want) {
  if (got == want) { pass++; return; }
  fail++;
  rt_puts("FAIL "); rt_puts(name); rt_puts(": got "); rt_hex(got); rt_puts(" want "); rt_hex(want); rt_puts("\n");
}
static uint64_t dbits(double d) { union { double d; uint64_t u; } u = {d}; return u.u; }

#define CVT(insn, rm, x) ({ long r_; __asm__ volatile(insn " %0, %1, " rm : "=r"(r_) : "f"(x)); r_; })

int main(void) {
  uint64_t v;
  /* fcsr: write, read back, fields */
  __asm__ volatile("csrw fcsr, %0" :: "r"(0xffL));
  __asm__ volatile("csrr %0, fcsr" : "=r"(v)); check("fcsr all", v, 0xff);
  __asm__ volatile("csrr %0, frm" : "=r"(v)); check("frm", v, 7);
  __asm__ volatile("csrr %0, fflags" : "=r"(v)); check("fflags", v, 0x1f);
  __asm__ volatile("csrwi fflags, 0"); __asm__ volatile("csrr %0, fcsr" : "=r"(v)); check("fflags clear", v, 0xe0);
  __asm__ volatile("csrwi frm, 0"); __asm__ volatile("csrr %0, fcsr" : "=r"(v)); check("frm rne", v, 0);
  __asm__ volatile("csrrsi %0, fflags, 3" : "=r"(v)); check("csrrsi old", v, 0);
  __asm__ volatile("csrrci %0, fflags, 1" : "=r"(v)); check("csrrci old", v, 3);
  __asm__ volatile("csrr %0, fflags" : "=r"(v)); check("csrrci new", v, 2);
  __asm__ volatile("csrw fcsr, zero");

  /* static rounding modes on conversion: 2.5 and -2.5 */
  volatile double p = 2.5, m = -2.5;
  check("fcvt rne 2.5", (uint64_t)CVT("fcvt.l.d", "rne", p), 2);
  check("fcvt rtz 2.5", (uint64_t)CVT("fcvt.l.d", "rtz", p), 2);
  check("fcvt rdn -2.5", (uint64_t)CVT("fcvt.l.d", "rdn", m), (uint64_t)-3);
  check("fcvt rup 2.5", (uint64_t)CVT("fcvt.l.d", "rup", p), 3);
  check("fcvt rmm 2.5", (uint64_t)CVT("fcvt.l.d", "rmm", p), 3);
  check("fcvt rmm -2.5", (uint64_t)CVT("fcvt.l.d", "rmm", m), (uint64_t)-3);
  /* dynamic rounding mode taken from frm */
  __asm__ volatile("csrwi frm, 3");
  check("fcvt dyn=rup", (uint64_t)CVT("fcvt.l.d", "dyn", p), 3);
  __asm__ volatile("csrwi frm, 0");
  /* inexact flag from a conversion, invalid from NaN */
  __asm__ volatile("csrw fflags, zero");
  (void)CVT("fcvt.w.d", "rtz", p);
  __asm__ volatile("csrr %0, fflags" : "=r"(v)); check("fcvt NX", v, 1);
  volatile double nan = __builtin_nan("");
  __asm__ volatile("csrw fflags, zero");
  check("fcvt.w NaN", (uint64_t)CVT("fcvt.w.d", "rtz", nan), 0x7fffffff);
  __asm__ volatile("csrr %0, fflags" : "=r"(v)); check("fcvt NV", v, 16);
  /* division by zero flag, inexact on 1/3 */
  volatile double one = 1, zero = 0, three = 3, r;
  __asm__ volatile("csrw fflags, zero");
  r = one / zero; (void)r;
  __asm__ volatile("csrr %0, fflags" : "=r"(v)); check("fdiv DZ", v, 8);
  __asm__ volatile("csrw fflags, zero");
  r = one / three;
  __asm__ volatile("csrr %0, fflags" : "=r"(v)); check("fdiv NX", v, 1);
  check("1/3", dbits(r), 0x3fd5555555555555ull);
  /* rounding mode applied to arithmetic: 1/3 rounded up differs in the last bit */
  double ru;
  __asm__ volatile("fdiv.d %0, %1, %2, rup" : "=f"(ru) : "f"(one), "f"(three));
  check("1/3 rup", dbits(ru), 0x3fd5555555555556ull);
  /* canonical NaN from 0/0 */
  r = zero / zero;
  check("0/0 canonical", dbits(r), 0x7ff8000000000000ull);
  /* NaN boxing: a double in an f register read as single is the canonical NaN */
  float fs;
  __asm__ volatile("fadd.s %0, %1, %1" : "=f"(fs) : "f"(one)); /* f reg holds a double 1.0 */
  union { float f; uint32_t u; } uf = {fs};
  check("unboxed single", uf.u, 0x7fc00000u);
  /* fmv.x.w sign-extends */
  float neg = -1.0f;
  long xw;
  __asm__ volatile("fmv.x.w %0, %1" : "=r"(xw) : "f"(neg));
  check("fmv.x.w", (uint64_t)xw, 0xffffffffbf800000ull);
  /* fclass */
  long cl;
  __asm__ volatile("fclass.d %0, %1" : "=r"(cl) : "f"(nan)); check("fclass qnan", (uint64_t)cl, 512);

  /* LR/SC: success, then failure after the reservation is gone */
  static volatile uint64_t word = 5;
  long ok1, ok2, old;
  __asm__ volatile("lr.d %0, (%2)\n sc.d %1, %3, (%2)" : "=&r"(old), "=&r"(ok1) : "r"(&word), "r"(7L) : "memory");
  check("lr", (uint64_t)old, 5); check("sc ok", (uint64_t)ok1, 0); check("sc stored", word, 7);
  __asm__ volatile("sc.d %0, %2, (%1)" : "=&r"(ok2) : "r"(&word), "r"(9L) : "memory");
  check("sc without lr fails", (uint64_t)ok2, 1); check("sc failed no store", word, 7);
  static volatile uint32_t w32 = 0xfffffff0u;
  long r32;
  __asm__ volatile("lr.w %0, (%1)" : "=r"(r32) : "r"(&w32) : "memory");
  check("lr.w sign-extends", (uint64_t)r32, 0xfffffffffffffff0ull);
  __asm__ volatile("amomaxu.w %0, %2, (%1)" : "=r"(r32) : "r"(&w32), "r"(1L) : "memory");
  check("amomaxu.w", w32, 0xfffffff0u);
  __asm__ volatile("amomax.w %0, %2, (%1)" : "=r"(r32) : "r"(&w32), "r"(1L) : "memory");
  check("amomax.w", w32, 1);

  /* fence.i: write an instruction, run it, change it, fence.i, run again */
  static uint32_t code[4] __attribute__((aligned(16)));
  code[0] = 0x00100513u; /* li a0, 1 */
  code[1] = 0x00008067u; /* ret */
  __asm__ volatile("fence.i" ::: "memory");
  long (*fn)(void) = (long (*)(void))(void *)code;
  check("run written code", (uint64_t)fn(), 1);
  code[0] = 0x00200513u; /* li a0, 2 */
  __asm__ volatile("fence.i" ::: "memory");
  check("run rewritten code after fence.i", (uint64_t)fn(), 2);

  /* mulh family against known values */
  long hi;
  __asm__ volatile("mulh %0, %1, %2" : "=r"(hi) : "r"(-1L), "r"(-1L)); check("mulh -1*-1", (uint64_t)hi, 0);
  __asm__ volatile("mulhu %0, %1, %2" : "=r"(hi) : "r"(-1L), "r"(-1L)); check("mulhu", (uint64_t)hi, 0xfffffffffffffffeull);
  __asm__ volatile("mulhsu %0, %1, %2" : "=r"(hi) : "r"(-1L), "r"(-1L)); check("mulhsu", (uint64_t)hi, ~0ull);
  /* division edge cases defined by the ISA */
  long q;
  __asm__ volatile("div %0, %1, %2" : "=r"(q) : "r"(7L), "r"(0L)); check("div by 0", (uint64_t)q, ~0ull);
  __asm__ volatile("rem %0, %1, %2" : "=r"(q) : "r"(7L), "r"(0L)); check("rem by 0", (uint64_t)q, 7);
  __asm__ volatile("div %0, %1, %2" : "=r"(q) : "r"(INT64_MIN), "r"(-1L)); check("div overflow", (uint64_t)q, (uint64_t)INT64_MIN);
  __asm__ volatile("remw %0, %1, %2" : "=r"(q) : "r"((long)INT32_MIN), "r"(-1L)); check("remw overflow", (uint64_t)q, 0);
  __asm__ volatile("divuw %0, %1, %2" : "=r"(q) : "r"(5L), "r"(0L)); check("divuw by 0", (uint64_t)q, ~0ull);

  rt_puts("insn: "); rt_dec(pass + fail); rt_puts(" checks, "); rt_dec(fail); rt_puts(" failed\n");
  return fail != 0;
}
