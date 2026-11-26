// SPDX-License-Identifier: GPL-2.0-or-later
/* The guest address space. The engine owns one contiguous window of its own memory and
 * places every guest mapping inside it, so a 64-bit guest runs entirely below g_size
 * (well under 4 GiB) and a guest address is an offset into the window.
 *
 * Layout, low to high:
 *   [0, g_lo)                 never mapped: null pointer guard (64 KiB)
 *   program image, then brk   the executable at its link address (PIE: at LX_PIE_BASE)
 *   ...                       mmap area, allocated top down from below the stack
 *   stack                     LX_STACK_SIZE bytes, ending one page below g_size
 *   [g_size - 4096, g_size)   never mapped: lets one compare cover 8-byte accesses
 *
 * A byte per page records whether it is mapped and its protection. Protections are
 * recorded and reported but not enforced on each access (see docs/LINUX-BINARIES.md,
 * "protection"). Unmapped pages inside the window read as whatever they last held. */
#include "lx.h"
#include "host.h"
#include "abi_lx.h"

uint8_t *g_mem;
uint64_t g_lo = 0x10000, g_size, g_span;
int mem_protect_enforced;

#define PF_MAP 1
#define PF_R 2
#define PF_W 4
#define PF_X 8
#define PF_DIRTY 16  /* was mapped once: must be cleared before it is handed out again */
#define PF_SHARED 32

#define LX_STACK_SIZE (8ull << 20)

uint8_t *pf;                  /* one byte per page (rv_interp.c reads it when LX_MEMCHECK=2) */
static uint64_t npages;
static uint64_t brk_start, brk_cur, brk_exact, mmap_top, mmap_low;

struct shmap { uint64_t addr, len; int64_t off; int fd; struct shmap *next; };
static struct shmap *shmaps;  /* MAP_SHARED file mappings, written back at msync/munmap/exit */

int mem_init(uint64_t size) {
  size = (size + 0xffff) & ~0xffffull;
  g_mem = hb_window(size);
  if (!g_mem) return -ENOMEM;
  g_size = size;
  g_span = g_size - g_lo - 8;
  npages = size / LX_PAGE;
  pf = calloc(npages, 1);
  if (!pf) return -ENOMEM;
  mem_reset();
  return 0;
}

static void flush_shared(struct shmap *s, uint64_t a, uint64_t len);
static void release(uint64_t a, uint64_t len);

void mem_reset(void) {
  while (shmaps) {
    struct shmap *s = shmaps;
    flush_shared(s, s->addr, s->len);
    hb1(LX_SYS_close, s->fd);
    shmaps = s->next;
    free(s);
  }
  release(g_lo, g_size - g_lo - LX_PAGE);
  brk_start = brk_cur = brk_exact = 0;
  mmap_top = g_size - LX_PAGE - LX_STACK_SIZE - (1ull << 20);
  mmap_low = mmap_top;
}

uint64_t mem_stack_top(void) { return g_size - LX_PAGE; }

uint64_t mem_used_bytes(void) {
  uint64_t n = 0;
  for (uint64_t p = 0; p < npages; p++) n += pf[p] & PF_MAP;
  return n * LX_PAGE;
}

static int prot_bits(int prot) {
  return (prot & LX_PROT_READ ? PF_R : 0) | (prot & LX_PROT_WRITE ? PF_W : 0) | (prot & LX_PROT_EXEC ? PF_X : 0);
}

int mem_is_mapped(uint64_t a, uint64_t len) {
  if (len == 0) return 1;
  if (a < g_lo || a + len > g_size - LX_PAGE || a + len < a) return 0;
  for (uint64_t p = a / LX_PAGE; p <= (a + len - 1) / LX_PAGE; p++)
    if (!(pf[p] & PF_MAP)) return 0;
  return 1;
}

static int range_free(uint64_t a, uint64_t len) {
  if (a < g_lo || a + len > g_size - LX_PAGE || a + len < a) return 0;
  for (uint64_t p = a / LX_PAGE; p < (a + len) / LX_PAGE; p++)
    if (pf[p] & PF_MAP) return 0;
  return 1;
}

/* Clear pages that held data before, then mark them mapped. */
static void claim(uint64_t a, uint64_t len, int bits) {
  uint64_t p0 = a / LX_PAGE, p1 = (a + len) / LX_PAGE;
  uint64_t run = 0;
  for (uint64_t p = p0; p <= p1; p++) {
    if (p < p1 && (pf[p] & PF_DIRTY)) { run++; continue; }
    if (run) memset(g_mem + (p - run) * LX_PAGE, 0, run * LX_PAGE), run = 0;
  }
  for (uint64_t p = p0; p < p1; p++) pf[p] = PF_MAP | PF_DIRTY | bits;
}

static void release(uint64_t a, uint64_t len) {
  uint64_t p0 = a / LX_PAGE, p1 = (a + len) / LX_PAGE;
  /* Hand whole runs back to the host; if it guarantees zeros, the pages are clean. */
  uint64_t p = p0;
  while (p < p1) {
    if (!(pf[p] & PF_MAP)) { p++; continue; }
    uint64_t q = p;
    while (q < p1 && (pf[q] & PF_MAP)) q++;
    int zero = hb_release(g_mem + p * LX_PAGE, (q - p) * LX_PAGE);
    for (uint64_t i = p; i < q; i++) pf[i] = zero ? 0 : PF_DIRTY;
    p = q;
  }
}

static void flush_shared(struct shmap *s, uint64_t a, uint64_t len) {
  uint64_t lo = a > s->addr ? a : s->addr, hi = a + len < s->addr + s->len ? a + len : s->addr + s->len;
  if (lo >= hi) return;
  /* Write back only within the file's current size: a shared mapping never extends a file. */
  struct lx_stat st;
  if (hb_fstatat(s->fd, 0, &st, 0) < 0) return;
  int64_t foff = s->off + (int64_t)(lo - s->addr);
  if (foff >= st.size) return;
  if ((int64_t)(hi - lo) > st.size - foff) hi = lo + (uint64_t)(st.size - foff);
  for (uint64_t done = 0; done < hi - lo;) {
    int64_t r = hb4(LX_SYS_pwrite64, s->fd, hbp(g_mem + lo + done), hi - lo - done, foff + (int64_t)done);
    if (r <= 0) break;
    done += (uint64_t)r;
  }
}

static void drop_shared(uint64_t a, uint64_t len) {
  for (struct shmap **pp = &shmaps; *pp;) {
    struct shmap *s = *pp;
    if (s->addr < a + len && a < s->addr + s->len) {
      flush_shared(s, a, len);
      if (a <= s->addr && a + len >= s->addr + s->len) {
        hb1(LX_SYS_close, s->fd); *pp = s->next; free(s); continue;
      }
      /* Partial unmap: keep the record; flushing a range that is gone is harmless
         because only mapped pages are written back on the next sync. */
    }
    pp = &s->next;
  }
}

static uint64_t find_free(uint64_t len) {
  /* Fast path: just below the lowest mapping made so far. */
  if (mmap_low > len && range_free(mmap_low - len, len) && mmap_low - len > brk_cur + (64ull << 20)) {
    mmap_low -= len;
    return mmap_low;
  }
  /* Slow path: first hole from the top. */
  uint64_t need = len / LX_PAGE, run = 0;
  for (uint64_t p = mmap_top / LX_PAGE; p-- > g_lo / LX_PAGE;) {
    if (pf[p] & PF_MAP) { run = 0; continue; }
    if (++run == need) {
      uint64_t a = p * LX_PAGE;
      if (a < brk_cur) return 0;
      if (a < mmap_low) mmap_low = a;
      return a;
    }
  }
  return 0;
}

int64_t mem_map(uint64_t addr, uint64_t len, int prot, int flags, int fd, int64_t off) {
  if (len == 0) return -EINVAL;
  if (off & (LX_PAGE - 1)) return -EINVAL;
  uint64_t alen = (len + LX_PAGE - 1) & ~(LX_PAGE - 1);
  if (alen < len) return -ENOMEM;
  int fixed = flags & (LX_MAP_FIXED | LX_MAP_FIXED_NOREPLACE);
  uint64_t a;
  if (fixed) {
    if (addr & (LX_PAGE - 1)) return -EINVAL;
    a = addr;
    if (a < g_lo || a + alen > g_size - LX_PAGE || a + alen < a) return -ENOMEM;
    if (!range_free(a, alen)) {
      if (flags & LX_MAP_FIXED_NOREPLACE) return -EEXIST;
      mem_unmap(a, alen);
    }
  } else {
    a = 0;
    addr &= ~(LX_PAGE - 1);
    if (addr && range_free(addr, alen) && addr >= brk_cur) a = addr; /* honour a free hint */
    if (!a) a = find_free(alen);
    if (!a) return -ENOMEM;
  }
  claim(a, alen, prot_bits(prot) | ((flags & LX_MAP_SHARED) ? PF_SHARED : 0));
  if (!(flags & LX_MAP_ANON) && fd >= 0) {
    /* A file mapping is implemented by reading the file in. Past the end of the file the
       pages stay zero, as they would read on Linux (no SIGBUS). */
    uint64_t done = 0;
    while (done < len) {
      int64_t r = hb4(LX_SYS_pread64, fd, hbp(g_mem + a + done), len - done, off + (int64_t)done);
      if (r < 0) { if (r == -EINTR) continue; release(a, alen); return r; }
      if (r == 0) break;
      done += (uint64_t)r;
    }
    if ((flags & LX_MAP_SHARED) && (prot & LX_PROT_WRITE)) {
      struct shmap *s = malloc(sizeof *s);
      int64_t nfd = hb1(LX_SYS_dup, fd);
      if (s && nfd >= 0) { s->addr = a; s->len = len; s->off = off; s->fd = (int)nfd; s->next = shmaps; shmaps = s; }
      else free(s);
    }
  }
  return (int64_t)a;
}

int64_t mem_unmap(uint64_t a, uint64_t len) {
  if ((a & (LX_PAGE - 1)) || len == 0) return -EINVAL;
  uint64_t alen = (len + LX_PAGE - 1) & ~(LX_PAGE - 1);
  if (a + alen < a) return -EINVAL;
  if (a >= g_size) return 0;
  if (a + alen > g_size) alen = g_size - a;
  if (shmaps) drop_shared(a, alen);
  release(a, alen);
  return 0;
}

int64_t mem_protect(uint64_t a, uint64_t len, int prot) {
  if (a & (LX_PAGE - 1)) return -EINVAL;
  uint64_t alen = (len + LX_PAGE - 1) & ~(LX_PAGE - 1);
  if (!mem_is_mapped(a, alen)) return -ENOMEM;
  for (uint64_t p = a / LX_PAGE; p < (a + alen) / LX_PAGE; p++)
    pf[p] = (pf[p] & ~(PF_R | PF_W | PF_X)) | prot_bits(prot);
  return 0;
}

int64_t mem_sync(uint64_t a, uint64_t len) {
  if (a & (LX_PAGE - 1)) return -EINVAL;
  for (struct shmap *s = shmaps; s; s = s->next) flush_shared(s, a, len);
  return 0;
}

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2
int64_t mem_remap(uint64_t old, uint64_t oldlen, uint64_t newlen, int flags, uint64_t newaddr) {
  if (old & (LX_PAGE - 1)) return -EINVAL;
  oldlen = (oldlen + LX_PAGE - 1) & ~(LX_PAGE - 1);
  newlen = (newlen + LX_PAGE - 1) & ~(LX_PAGE - 1);
  if (newlen == 0) return -EINVAL;
  if (!mem_is_mapped(old, oldlen ? oldlen : LX_PAGE)) return -EFAULT;
  int bits = pf[old / LX_PAGE] & (PF_R | PF_W | PF_X);
  if (flags & MREMAP_FIXED) {
    if (!(flags & MREMAP_MAYMOVE) || (newaddr & (LX_PAGE - 1))) return -EINVAL;
    if (newaddr < g_lo || newaddr + newlen > g_size - LX_PAGE) return -ENOMEM;
    mem_unmap(newaddr, newlen);
    claim(newaddr, newlen, bits);
    memmove(g_mem + newaddr, g_mem + old, oldlen < newlen ? oldlen : newlen);
    mem_unmap(old, oldlen);
    return (int64_t)newaddr;
  }
  if (newlen <= oldlen) {
    if (newlen < oldlen) mem_unmap(old + newlen, oldlen - newlen);
    return (int64_t)old;
  }
  if (range_free(old + oldlen, newlen - oldlen)) {
    claim(old + oldlen, newlen - oldlen, bits);
    return (int64_t)old;
  }
  if (!(flags & MREMAP_MAYMOVE)) return -ENOMEM;
  uint64_t a = find_free(newlen);
  if (!a) return -ENOMEM;
  claim(a, newlen, bits);
  memcpy(g_mem + a, g_mem + old, oldlen);
  mem_unmap(old, oldlen);
  return (int64_t)a;
}

void mem_set_brk(uint64_t start) {
  start = (start + LX_PAGE - 1) & ~(LX_PAGE - 1);
  brk_start = brk_cur = brk_exact = start;
}

/* brk: the break is page granular inside; the guest sees the exact value it asked for. */
int64_t mem_brk(uint64_t want) {
  if (want < brk_start) return (int64_t)brk_exact;
  uint64_t top = (want + LX_PAGE - 1) & ~(LX_PAGE - 1);
  if (top > brk_cur) {
    if (top > mmap_low || !range_free(brk_cur, top - brk_cur)) return (int64_t)brk_exact;
    claim(brk_cur, top - brk_cur, PF_R | PF_W);
  } else if (top < brk_cur) {
    release(top, brk_cur - top);
  }
  brk_cur = top;
  brk_exact = want;
  return (int64_t)want;
}

int64_t g_strncpy(char *buf, uint64_t ga, size_t cap) {
  for (size_t i = 0; i < cap; i++) {
    if (!g_inrange(ga + i)) return -EFAULT;
    if (!(buf[i] = (char)g_mem[ga + i])) return (int64_t)i;
  }
  return -ENAMETOOLONG;
}

/* Snapshot for the emulated fork: the page table, the break and mmap cursors, and the
 * contents of every writable mapped page that is not all zeros. */
struct msnap { uint8_t *pf; uint64_t brk_start, brk_cur, brk_exact, mmap_low; uint64_t n; uint64_t *pages; uint8_t *data; };
static int page_zero(const uint8_t *p) {
  const uint64_t *q = (const uint64_t *)(const void *)p;
  for (unsigned i = 0; i < LX_PAGE / 8; i++) if (q[i]) return 0;
  return 1;
}
void *mem_snapshot(void) {
  struct msnap *s = calloc(1, sizeof *s);
  if (!s) return 0;
  s->pf = malloc(npages);
  if (!s->pf) { free(s); return 0; }
  memcpy(s->pf, pf, npages);
  s->brk_start = brk_start; s->brk_cur = brk_cur; s->brk_exact = brk_exact; s->mmap_low = mmap_low;
  uint64_t n = 0;
  for (uint64_t p = 0; p < npages; p++)
    if ((pf[p] & (PF_MAP | PF_W)) == (PF_MAP | PF_W) && !page_zero(g_mem + p * LX_PAGE)) n++;
  s->pages = malloc((n ? n : 1) * sizeof *s->pages);
  s->data = malloc((n ? n : 1) * LX_PAGE);
  if (!s->pages || !s->data) { free(s->pages); free(s->data); free(s->pf); free(s); return 0; }
  uint64_t k = 0;
  for (uint64_t p = 0; p < npages && k < n; p++)
    if ((pf[p] & (PF_MAP | PF_W)) == (PF_MAP | PF_W) && !page_zero(g_mem + p * LX_PAGE)) {
      s->pages[k] = p;
      memcpy(s->data + k * LX_PAGE, g_mem + p * LX_PAGE, LX_PAGE);
      k++;
    }
  s->n = k;
  return s;
}
void mem_restore(void *v) {
  struct msnap *s = v;
  uint64_t k = 0;
  for (uint64_t p = 0; p < npages; p++) {
    if (k < s->n && s->pages[k] == p) {
      memcpy(g_mem + p * LX_PAGE, s->data + k * LX_PAGE, LX_PAGE);
      k++;
    } else if ((s->pf[p] & (PF_MAP | PF_W)) == (PF_MAP | PF_W) && !page_zero(g_mem + p * LX_PAGE)) {
      memset(g_mem + p * LX_PAGE, 0, LX_PAGE); /* was zero in the parent */
    }
  }
  for (uint64_t p = 0; p < npages; p++) {
    if ((pf[p] & PF_MAP) && !(s->pf[p] & PF_MAP)) pf[p] = PF_DIRTY; /* the child mapped it */
    else pf[p] = s->pf[p];
  }
  brk_start = s->brk_start; brk_cur = s->brk_cur; brk_exact = s->brk_exact; mmap_low = s->mmap_low;
}
void mem_snapshot_free(void *v) {
  struct msnap *s = v;
  free(s->pages); free(s->data); free(s->pf); free(s);
}
