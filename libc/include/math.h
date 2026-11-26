// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _MATH_H
#define _MATH_H
/* Our own libm (libc/src/math.c). sqrt, floor, ceil, trunc, fabs,
 * copysign and rint are single WebAssembly instructions. */
typedef float float_t;
typedef double double_t;

#define NAN (__builtin_nanf(""))
#define INFINITY (__builtin_inff())
#define HUGE_VAL (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x) __builtin_signbit(x)
#define isgreater(x, y) __builtin_isgreater(x, y)
#define isgreaterequal(x, y) __builtin_isgreaterequal(x, y)
#define isless(x, y) __builtin_isless(x, y)
#define islessequal(x, y) __builtin_islessequal(x, y)
#define islessgreater(x, y) __builtin_islessgreater(x, y)
#define isunordered(x, y) __builtin_isunordered(x, y)

#define M_E 2.7182818284590452354
#define M_LOG2E 1.4426950408889634074
#define M_LOG10E 0.43429448190325182765
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_1_PI 0.31830988618379067154
#define M_2_PI 0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440

double sin(double), cos(double), tan(double), asin(double), acos(double), atan(double), atan2(double, double);
double sinh(double), cosh(double), tanh(double), asinh(double), acosh(double), atanh(double);
double exp(double), exp2(double), expm1(double), log(double), log2(double), log10(double), log1p(double);
double pow(double, double), cbrt(double), hypot(double, double);
double fmod(double, double), remainder(double, double), ldexp(double, int), scalbn(double, int), frexp(double, int *);
double modf(double, double *), logb(double), nextafter(double, double), fdim(double, double), fma(double, double, double);
double round(double), fmin(double, double), fmax(double, double), nan(const char *);
long lround(double), lrint(double);
long long llround(double), llrint(double);
int ilogb(double);
double sqrt(double);
double floor(double);
double ceil(double);
double trunc(double);
double fabs(double);
double copysign(double, double);
double rint(double);
double nearbyint(double);

float sinf(float), cosf(float), tanf(float), asinf(float), acosf(float), atanf(float), atan2f(float, float);
float sinhf(float), coshf(float), tanhf(float), asinhf(float), acoshf(float), atanhf(float);
float expf(float), exp2f(float), expm1f(float), logf(float), log2f(float), log10f(float), log1pf(float);
float powf(float, float), cbrtf(float), hypotf(float, float);
float fmodf(float, float), remainderf(float, float), ldexpf(float, int), scalbnf(float, int), frexpf(float, int *);
float modff(float, float *), roundf(float), fminf(float, float), fmaxf(float, float), fmaf(float, float, float);
float nextafterf(float, float), fdimf(float, float), nanf(const char *);
long lroundf(float), lrintf(float);
float sqrtf(float);
float floorf(float);
float ceilf(float);
float truncf(float);
float fabsf(float);
float copysignf(float, float);
float rintf(float);
float nearbyintf(float);
#endif
