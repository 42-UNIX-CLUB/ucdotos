// SPDX-License-Identifier: GPL-2.0-or-later
#include "kernel.h"

void kprintf(const char *fmt, ...) {
  char buf[512];
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  __builtin_va_end(ap);
  host_log(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
}
void panic(const char *msg) {
  kprintf("KERNEL PANIC: %s\n", msg);
  __builtin_trap();
}

/* Heap: first-fit free list with coalescing, grown with memory.grow.
 * Block header: size (including header), low bit = in use. */
extern u8 __heap_base;
struct blk { u32 size; struct blk *next_free; };
#define HDR 8
static struct blk *free_list;
static u8 *heap_end;
static u32 used;

static struct blk *grow(u32 need) {
  u32 pages = (need + 65535) >> 16;
  if (pages < 16) pages = 16; /* 1 MiB at a time */
  u32 old = __builtin_wasm_memory_grow(0, pages);
  if (old == (u32)-1) return NULL;
  struct blk *b = (struct blk *)(usize)(old << 16);
  if (!heap_end) {
    /* first growth: also claim the gap between __heap_base and the old end of memory */
    u8 *base = (u8 *)(((usize)&__heap_base + 15) & ~15u);
    b = (struct blk *)base;
  }
  heap_end = (u8 *)(usize)((old + pages) << 16);
  b->size = heap_end - (u8 *)b;
  return b;
}
void kfree(void *p);
void *kmalloc(usize n) {
  if (!n) n = 1;
  if (n > 0x7fffffff) return NULL; /* also keeps the size arithmetic below from wrapping */
  u32 need = (n + HDR + 15) & ~15u;
  for (;;) {
    struct blk **pp = &free_list;
    for (struct blk *b = free_list; b; pp = &b->next_free, b = b->next_free) {
      if (b->size < need) continue;
      if (b->size - need >= 32) { /* split */
        struct blk *rest = (struct blk *)((u8 *)b + need);
        rest->size = b->size - need;
        rest->next_free = b->next_free;
        *pp = rest;
        b->size = need;
      } else *pp = b->next_free;
      used += b->size;
      b->size |= 1;
      return (u8 *)b + HDR;
    }
    struct blk *nb = grow(need);
    if (!nb) return NULL;
    nb->size |= 1; used += nb->size & ~1u;
    kfree((u8 *)nb + HDR); /* insert (and merge) into the free list, then retry */
  }
}
void kfree(void *p) {
  if (!p) return;
  struct blk *b = (struct blk *)((u8 *)p - HDR);
  b->size &= ~1u;
  used -= b->size;
  /* address-ordered insert, merge with neighbours */
  struct blk *prev = NULL, *n = free_list;
  while (n && n < b) prev = n, n = n->next_free;
  b->next_free = n;
  if (prev) prev->next_free = b; else free_list = b;
  if (n && (u8 *)b + b->size == (u8 *)n) { b->size += n->size; b->next_free = n->next_free; }
  if (prev && (u8 *)prev + prev->size == (u8 *)b) { prev->size += b->size; prev->next_free = b->next_free; }
}
void *kzalloc(usize n) { void *p = kmalloc(n); if (p) memset(p, 0, n); return p; }
void *krealloc(void *p, usize n) {
  if (!p) return kmalloc(n);
  struct blk *b = (struct blk *)((u8 *)p - HDR);
  u32 have = (b->size & ~1u) - HDR;
  if (n <= have) return p;
  void *q = kmalloc(n);
  if (!q) return NULL;
  memcpy(q, p, have);
  kfree(p);
  return q;
}
u32 kmem_used(void) { return used; }
