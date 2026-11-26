// SPDX-License-Identifier: GPL-2.0-or-later
/* IEEE 754 binary32 and binary64 arithmetic in software, with RISC-V semantics: five
 * rounding modes, the five exception flags, the canonical NaN, tininess detected after
 * rounding, saturating float-to-integer conversions. Values travel as raw bit patterns.
 * Written from IEEE 754-2008 and the RISC-V F and D chapters; no third-party code.
 *
 * This is the reference: exact, and slow. rv_fp.c uses the host's floating point where
 * it can prove the result and flags identical, and falls back to these functions. */
#ifndef LX_SOFTFP_H
#define LX_SOFTFP_H
#include <stdint.h>

enum { RM_RNE = 0, RM_RTZ = 1, RM_RDN = 2, RM_RUP = 3, RM_RMM = 4 };
enum { FF_NX = 1, FF_UF = 2, FF_OF = 4, FF_DZ = 8, FF_NV = 16 };

#define F64_QNAN 0x7ff8000000000000ull
#define F32_QNAN 0x7fc00000u

uint64_t sf64_add(uint64_t a, uint64_t b, int rm, uint32_t *fl);
uint64_t sf64_sub(uint64_t a, uint64_t b, int rm, uint32_t *fl);
uint64_t sf64_mul(uint64_t a, uint64_t b, int rm, uint32_t *fl);
uint64_t sf64_div(uint64_t a, uint64_t b, int rm, uint32_t *fl);
uint64_t sf64_sqrt(uint64_t a, int rm, uint32_t *fl);
/* a*b + c with one rounding; negprod flips the product's sign, negadd the addend's. */
uint64_t sf64_fma(uint64_t a, uint64_t b, uint64_t c, int negprod, int negadd, int rm, uint32_t *fl);
uint32_t sf32_add(uint32_t a, uint32_t b, int rm, uint32_t *fl);
uint32_t sf32_sub(uint32_t a, uint32_t b, int rm, uint32_t *fl);
uint32_t sf32_mul(uint32_t a, uint32_t b, int rm, uint32_t *fl);
uint32_t sf32_div(uint32_t a, uint32_t b, int rm, uint32_t *fl);
uint32_t sf32_sqrt(uint32_t a, int rm, uint32_t *fl);
uint32_t sf32_fma(uint32_t a, uint32_t b, uint32_t c, int negprod, int negadd, int rm, uint32_t *fl);

/* Float to integer. bits: 32 or 64; sgn: 1 signed. Result is sign-extended to 64 bits
 * for 32-bit targets, as RISC-V writes it to the integer register. */
uint64_t sf64_to_int(uint64_t a, int bits, int sgn, int rm, uint32_t *fl);
uint64_t sf32_to_int(uint32_t a, int bits, int sgn, int rm, uint32_t *fl);
/* Integer to float: v holds the integer (sign-extended or zero-extended to 64 bits). */
uint64_t sf64_from_int(uint64_t v, int sgn, int rm, uint32_t *fl);
uint32_t sf32_from_int(uint64_t v, int sgn, int rm, uint32_t *fl);
uint32_t sf64_to_f32(uint64_t a, int rm, uint32_t *fl);
uint64_t sf32_to_f64(uint32_t a, uint32_t *fl);

/* Comparisons: feq is quiet (NV only for a signalling NaN), flt and fle signal on any NaN. */
int sf64_eq(uint64_t a, uint64_t b, uint32_t *fl);
int sf64_lt(uint64_t a, uint64_t b, uint32_t *fl);
int sf64_le(uint64_t a, uint64_t b, uint32_t *fl);
int sf32_eq(uint32_t a, uint32_t b, uint32_t *fl);
int sf32_lt(uint32_t a, uint32_t b, uint32_t *fl);
int sf32_le(uint32_t a, uint32_t b, uint32_t *fl);
uint64_t sf64_minmax(uint64_t a, uint64_t b, int max, uint32_t *fl);
uint32_t sf32_minmax(uint32_t a, uint32_t b, int max, uint32_t *fl);
unsigned sf64_class(uint64_t a);
unsigned sf32_class(uint32_t a);

#endif
