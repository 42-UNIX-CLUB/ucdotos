// SPDX-License-Identifier: GPL-2.0-or-later
/* bc: arbitrary precision calculator. */
#ifndef BC_H
#define BC_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A number is sign * D * 10^-scale, where D is the integer whose decimal
 * digits are d[n-1] ... d[0] (least significant first). */
typedef struct num { int sign, n, scale; unsigned char *d; } num;

num *num_zero(void);
num *num_int(long v);
num *num_dup(const num *a);
void num_free(num *a);
int num_is_zero(const num *a);
int num_cmp(const num *a, const num *b);
num *num_add(const num *a, const num *b);
num *num_sub(const num *a, const num *b);
num *num_mul(const num *a, const num *b, int scale);
num *num_div(const num *a, const num *b, int scale); /* NULL: divide by zero */
num *num_mod(const num *a, const num *b, int scale);
num *num_pow(const num *a, long e, int scale);
num *num_sqrt(const num *a, int scale);               /* NULL: negative */
num *num_trunc(const num *a, int scale);              /* to at most scale fraction digits */
num *num_parse(const char *s, int ibase);
char *num_str(const num *a, int obase);
long num_long(const num *a, int *overflow);
int num_length(const num *a);
#endif
