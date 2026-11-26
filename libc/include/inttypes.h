// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _INTTYPES_H
#define _INTTYPES_H
#include <stdint.h>
/* Format macros, built from clang's predefined per-type format strings. */
#define __PRI(t, c) __##t##_FMT##c##__
#define PRId8 "hhd"
#define PRId16 "hd"
#define PRId32 __PRI(INT32, d)
#define PRId64 __PRI(INT64, d)
#define PRIdPTR __PRI(INTPTR, d)
#define PRIdMAX __PRI(INTMAX, d)
#define PRIi8 "hhi"
#define PRIi16 "hi"
#define PRIi32 __PRI(INT32, i)
#define PRIi64 __PRI(INT64, i)
#define PRIiPTR __PRI(INTPTR, i)
#define PRIiMAX __PRI(INTMAX, i)
#define PRIu8 "hhu"
#define PRIu16 "hu"
#define PRIu32 __PRI(UINT32, u)
#define PRIu64 __PRI(UINT64, u)
#define PRIuPTR __PRI(UINTPTR, u)
#define PRIuMAX __PRI(UINTMAX, u)
#define PRIx8 "hhx"
#define PRIx16 "hx"
#define PRIx32 __PRI(UINT32, x)
#define PRIx64 __PRI(UINT64, x)
#define PRIxPTR __PRI(UINTPTR, x)
#define PRIxMAX __PRI(UINTMAX, x)
#define PRIX8 "hhX"
#define PRIX16 "hX"
#define PRIX32 __PRI(UINT32, X)
#define PRIX64 __PRI(UINT64, X)
#define PRIXPTR __PRI(UINTPTR, X)
#define PRIXMAX __PRI(UINTMAX, X)
#define PRIo8 "hho"
#define PRIo16 "ho"
#define PRIo32 __PRI(UINT32, o)
#define PRIo64 __PRI(UINT64, o)
#define PRIoPTR __PRI(UINTPTR, o)
#define PRIoMAX __PRI(UINTMAX, o)
#define SCNd8 "hhd"
#define SCNd16 "hd"
#define SCNd32 __PRI(INT32, d)
#define SCNd64 __PRI(INT64, d)
#define SCNdPTR __PRI(INTPTR, d)
#define SCNdMAX __PRI(INTMAX, d)
#define SCNi8 "hhi"
#define SCNi16 "hi"
#define SCNi32 __PRI(INT32, i)
#define SCNi64 __PRI(INT64, i)
#define SCNiPTR __PRI(INTPTR, i)
#define SCNiMAX __PRI(INTMAX, i)
#define SCNu8 "hhu"
#define SCNu16 "hu"
#define SCNu32 __PRI(UINT32, u)
#define SCNu64 __PRI(UINT64, u)
#define SCNuPTR __PRI(UINTPTR, u)
#define SCNuMAX __PRI(UINTMAX, u)
#define SCNx8 "hhx"
#define SCNx16 "hx"
#define SCNx32 __PRI(UINT32, x)
#define SCNx64 __PRI(UINT64, x)
#define SCNxPTR __PRI(UINTPTR, x)
#define SCNxMAX __PRI(UINTMAX, x)
#define SCNo8 "hho"
#define SCNo16 "ho"
#define SCNo32 __PRI(UINT32, o)
#define SCNo64 __PRI(UINT64, o)
#define SCNoPTR __PRI(UINTPTR, o)
#define SCNoMAX __PRI(UINTMAX, o)
typedef struct { intmax_t quot, rem; } imaxdiv_t;
intmax_t imaxabs(intmax_t);
imaxdiv_t imaxdiv(intmax_t, intmax_t);
intmax_t strtoimax(const char *, char **, int);
uintmax_t strtoumax(const char *, char **, int);
#endif
