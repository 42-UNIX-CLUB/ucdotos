// SPDX-License-Identifier: GPL-2.0-or-later
/* Floating point for the interpreter: fast paths on the host's IEEE arithmetic where the
 * result and the flags provably match, the soft-float reference otherwise. */
#ifndef LX_RV_FP_H
#define LX_RV_FP_H
#include <stdint.h>
#include "softfp.h"

static inline uint32_t fp_unbox(uint64_t x) { return (x >> 32) == 0xffffffffu ? (uint32_t)x : F32_QNAN; }
static inline uint64_t fp_box(uint32_t v) { return 0xffffffff00000000ull | v; }

uint64_t fpd_add(uint64_t a, uint64_t b, int rm, uint32_t *fl);
uint64_t fpd_sub(uint64_t a, uint64_t b, int rm, uint32_t *fl);
uint64_t fpd_mul(uint64_t a, uint64_t b, int rm, uint32_t *fl);
uint64_t fpd_div(uint64_t a, uint64_t b, int rm, uint32_t *fl);
uint64_t fpd_sqrt(uint64_t a, int rm, uint32_t *fl);
uint32_t fps_add(uint32_t a, uint32_t b, int rm, uint32_t *fl);
uint32_t fps_sub(uint32_t a, uint32_t b, int rm, uint32_t *fl);
uint32_t fps_mul(uint32_t a, uint32_t b, int rm, uint32_t *fl);
uint32_t fps_div(uint32_t a, uint32_t b, int rm, uint32_t *fl);
uint32_t fps_sqrt(uint32_t a, int rm, uint32_t *fl);
uint64_t fpd_fma(uint64_t a, uint64_t b, uint64_t c, int negp, int negc, int rm, uint32_t *fl);
uint32_t fps_fma(uint32_t a, uint32_t b, uint32_t c, int negp, int negc, int rm, uint32_t *fl);
extern int fp_fast_enabled; /* 0: soft-float only (for testing and measurement) */

#endif
