// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
enum color { RED, GREEN = 5, BLUE };
typedef enum { A = -1, B } signed_enum;
#define STR(x) #x
#define XSTR(x) STR(x)
#define CAT(a, b) a##b
#define VA(fmt, ...) printf(fmt, ##__VA_ARGS__)
#define NUM 12
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static int counter(void) { static int c; return ++c; }
_Static_assert(sizeof(int) == 4, "int");
int main(void) {
  printf("%d %d %d\n", RED, GREEN, BLUE);
  signed_enum se = A;
  printf("%d %d\n", se < 0, (int)sizeof(enum color));
  printf("%s %s %d\n", STR(a + b), XSTR(NUM), CAT(N, UM));
  VA("no args\n");
  VA("%d %d\n", 1, 2);
  printf("%d\n", fib(20));
  counter(); counter();
  printf("%d\n", counter());
  bool t = 5;
  printf("%d %d\n", t, (bool)0.5);
  uint8_t u8 = 300; int16_t i16 = -2; uint64_t u64 = UINT64_MAX;
  printf("%u %d %" PRIu64 "\n", u8, i16, u64);
  printf("%d %d\n", (int)_Alignof(double), (int)_Alignof(struct { char c; long long l; }));
  int g = _Generic(1.0f, float: 1, double: 2, default: 3);
  int h = _Generic((char)1, int: 1, char: 2, default: 3);
  printf("%d %d\n", g, h);
  printf("%s:%d %s\n", strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__, __LINE__, __func__);
  int x = ({ int y = 3; y * 2; });
  printf("%d\n", x);
  char *dyn = malloc(16);
  strcpy(dyn, "heap");
  printf("%s\n", dyn);
  free(dyn);
  int arr[4];
  memset(arr, 0, sizeof arr);
  arr[2] = __builtin_popcount(255) + __builtin_clz(1) + __builtin_ctz(8);
  printf("%d\n", arr[2]);
  printf("%d %d\n", 'a', '\n' + '\x41' + '\101');
  printf("%s\n", "con" "cat" "enated");
  printf("%d\n", __STDC_VERSION__ >= 201112L);
  long l = 0x7fffffff;
  printf("%ld\n", l);
  printf("%d\n", __builtin_expect(x == 6, 1));
  printf("%u\n", __builtin_bswap32(0x11223344));
#if defined(__wasm__) && __SIZEOF_POINTER__ == 4
  printf("wasm32\n");
#endif
  return 0;
}
