// SPDX-License-Identifier: GPL-2.0-or-later
/* The live ranges of this process for SYS_pmigrate (see libc/src/migrate.c). Separate from
 * migrate.c so that a program with its own __migrate_resume (one that calls SYS_pmigrate
 * itself) can use it. */
int __malloc_holes(unsigned *out, int cap, unsigned min);
#define MAXR 1024
static unsigned holes[2 * MAXR];
/* Fill out[] with the live ranges from `low` to the end of memory; returns how many. Holes
 * smaller than 64 KiB are kept: they could free at most a few pages. */
int __migrate_ranges(unsigned *out, int cap, unsigned low) {
  unsigned end = __builtin_wasm_memory_size(0) << 16, pos = low;
  int nh = __malloc_holes(holes, cap - 1 < MAXR ? cap - 1 : MAXR, 65536), n = 0;
  for (int i = 0; i < nh; i++) {
    unsigned a = holes[2 * i], b = a + holes[2 * i + 1];
    if (b <= pos) continue;
    if (a > pos) { out[2 * n] = pos; out[2 * n + 1] = a - pos; n++; }
    pos = b;
  }
  if (pos < end) { out[2 * n] = pos; out[2 * n + 1] = end - pos; n++; }
  return n;
}
