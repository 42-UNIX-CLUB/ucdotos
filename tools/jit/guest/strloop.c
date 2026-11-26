// SPDX-License-Identifier: GPL-2.0-or-later
// Byte-wise string work over a 256 KiB text: length, search, compare, case-folding copy.
#define REPS 3
#include "rt.h"
#define LEN (256 * 1024)
static char text[LEN + 1], out[LEN + 1];
__attribute__((noinline)) static uint64_t my_strlen(const char *s) { const char *p = s; while (*p) p++; return (uint64_t)(p - s); }
__attribute__((noinline)) static uint64_t count_word(const char *s, const char *w) {
  uint64_t n = 0;
  for (; *s; s++) {
    const char *a = s, *b = w;
    while (*b && *a == *b) { a++; b++; }
    if (!*b) n++;
  }
  return n;
}
__attribute__((noinline)) static void upper(char *d, const char *s) { while ((*d = *s)) { if (*d >= 'a' && *d <= 'z') *d -= 32; d++; s++; } }
uint64_t work(void) {
  static const char *words[] = { "the ", "unix ", "club ", "kernel ", "shell ", "pipe ", "wasm ", "lisboa " };
  int n = 0;
  while (n < LEN - 16) { const char *w = words[rt_rand() & 7]; while (*w && n < LEN - 16) text[n++] = *w++; }
  text[n] = 0;
  uint64_t r = my_strlen(text);
  for (int i = 0; i < 4; i++) r = r * 7 + count_word(text, words[i]);
  upper(out, text);
  r = r * 7 + count_word(out, "UNIX ");
  return r;
}
