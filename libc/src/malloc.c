// SPDX-License-Identifier: GPL-2.0-or-later
/* Heap for user programs: first-fit free list with coalescing, grown with memory.grow.
 * No system call is involved: a wasm instance grows its own memory. */
#include <stddef.h>
#include <string.h>
extern unsigned char __heap_base;
struct blk { unsigned size; struct blk *next_free; };
#define HDR 8
static struct blk *free_list;
static unsigned char *heap_end;

static struct blk *grow(unsigned need) {
  unsigned pages = (need + 65535) >> 16;
  if (pages < 4) pages = 4;
  unsigned old = __builtin_wasm_memory_grow(0, pages);
  if (old == (unsigned)-1) return 0;
  struct blk *b = (struct blk *)(size_t)(old << 16);
  if (!heap_end) b = (struct blk *)(((size_t)&__heap_base + 15) & ~15u);
  else if ((unsigned char *)b != heap_end) { /* someone else grew memory in between: start a new region */ }
  heap_end = (unsigned char *)(size_t)((old + pages) << 16);
  b->size = heap_end - (unsigned char *)b;
  return b;
}
void free(void *p) {
  if (!p) return;
  struct blk *b = (struct blk *)((unsigned char *)p - HDR);
  b->size &= ~1u;
  struct blk *prev = 0, *n = free_list;
  while (n && n < b) prev = n, n = n->next_free;
  b->next_free = n;
  if (prev) prev->next_free = b; else free_list = b;
  if (n && (unsigned char *)b + b->size == (unsigned char *)n) { b->size += n->size; b->next_free = n->next_free; }
  if (prev && (unsigned char *)prev + prev->size == (unsigned char *)b) { prev->size += b->size; prev->next_free = b->next_free; }
}
void *malloc(size_t n) {
  if (!n) n = 1;
  unsigned need = (n + HDR + 15) & ~15u;
  for (;;) {
    struct blk **pp = &free_list;
    for (struct blk *b = free_list; b; pp = &b->next_free, b = b->next_free) {
      if (b->size < need) continue;
      if (b->size - need >= 32) {
        struct blk *rest = (struct blk *)((unsigned char *)b + need);
        rest->size = b->size - need; rest->next_free = b->next_free;
        *pp = rest; b->size = need;
      } else *pp = b->next_free;
      b->size |= 1;
      return (unsigned char *)b + HDR;
    }
    struct blk *nb = grow(need);
    if (!nb) return 0;
    nb->size |= 1;
    free((unsigned char *)nb + HDR);
  }
}
void *calloc(size_t a, size_t b) { size_t n = a * b; void *p = malloc(n); if (p) memset(p, 0, n); return p; }
void *realloc(void *p, size_t n) {
  if (!p) return malloc(n);
  if (!n) { free(p); return 0; }
  struct blk *b = (struct blk *)((unsigned char *)p - HDR);
  unsigned have = (b->size & ~1u) - HDR;
  if (n <= have) return p;
  void *q = malloc(n < have * 2 ? have * 2 : n);
  if (!q) return 0;
  memcpy(q, p, have);
  free(p);
  return q;
}

/* For migration (libc/src/migrate.c): the free blocks of at least `min` bytes, as address-sorted
 * (start, length) pairs of their contents (their 8-byte headers stay live: the free list runs
 * through them). Returns how many were written (at most cap; the rest are then simply kept). */
int __malloc_holes(unsigned *out, int cap, unsigned min) {
  int n = 0;
  for (struct blk *b = free_list; b && n < cap; b = b->next_free)
    if (b->size - HDR >= min) { out[2 * n] = (unsigned)(size_t)b + HDR; out[2 * n + 1] = b->size - HDR; n++; }
  return n;
}
