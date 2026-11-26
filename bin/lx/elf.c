// SPDX-License-Identifier: GPL-2.0-or-later
/* ELF64 loader, written from the System V gABI and the RISC-V ELF psABI.
 *
 * Loads ET_EXEC (at its link address) and ET_DYN (static PIE, shared objects, the
 * dynamic linker) into the guest window, then builds the initial stack exactly as Linux
 * does for a new process: argc, argv[], NULL, envp[], NULL, auxv pairs, then the strings.
 * The loader applies no relocations: a static PIE relocates itself and a dynamically
 * linked program is relocated by its own dynamic linker (PT_INTERP), which we load too. */
#include "lx.h"
#include "host.h"
#include "abi_lx.h"
#include "arch.h"
#include "path.h"

#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6
#define PT_GNU_STACK 0x6474e551
#define ET_EXEC 2
#define ET_DYN 3
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* Where position independent code goes: the program and, above it, its interpreter. */
#define LX_PIE_BASE 0x100000ull
#define LX_INTERP_GAP (16ull << 20)

struct ehdr {
  uint8_t ident[16];
  uint16_t type, machine;
  uint32_t version;
  uint64_t entry, phoff, shoff;
  uint32_t flags;
  uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct phdr {
  uint32_t type, flags;
  uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

static int64_t readall(int fd, void *buf, uint64_t n, int64_t off) {
  uint64_t done = 0;
  while (done < n) {
    int64_t r = hb4(LX_SYS_pread64, fd, hbp((char *)buf + done), n - done, off + (int64_t)done);
    if (r == -EINTR) continue;
    if (r < 0) return r;
    if (r == 0) break;
    done += (uint64_t)r;
  }
  return (int64_t)done;
}

struct loaded { uint64_t bias, entry, phdr, phnum, phent, end; char interp[256]; };

/* Map one ELF file. want_bias: 0 for "pick", else the address for the lowest segment. */
static int64_t load_one(const char *hostpath, struct loaded *L, uint64_t want_base, int is_interp) {
  int64_t fd = hb4(LX_SYS_openat, LX_AT_FDCWD, hbp(hostpath), LX_O_CLOEXEC, 0);
  if (fd < 0) return fd;
  struct ehdr eh;
  int64_t r = readall((int)fd, &eh, sizeof eh, 0);
  int64_t err = -ENOEXEC;
  struct phdr *ph = 0;
  if (r != (int64_t)sizeof eh || memcmp(eh.ident, "\177ELF", 4) || eh.ident[4] != 2 || eh.ident[5] != 1) goto out;
  if (eh.machine != arch_cur->e_machine) goto out;
  if (eh.type != ET_EXEC && eh.type != ET_DYN) goto out;
  if (eh.phentsize != sizeof(struct phdr) || eh.phnum == 0 || eh.phnum > 128) goto out;
  ph = malloc(eh.phnum * sizeof *ph);
  if (!ph) { err = -ENOMEM; goto out; }
  if (readall((int)fd, ph, eh.phnum * sizeof *ph, (int64_t)eh.phoff) != (int64_t)(eh.phnum * sizeof *ph)) goto out;

  uint64_t lo = ~0ull, hi = 0;
  L->interp[0] = 0;
  for (int i = 0; i < eh.phnum; i++) {
    if (ph[i].type == PT_LOAD) {
      if (ph[i].vaddr < lo) lo = ph[i].vaddr;
      if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
    } else if (ph[i].type == PT_INTERP && !is_interp) {
      uint64_t n = ph[i].filesz < sizeof L->interp - 1 ? ph[i].filesz : sizeof L->interp - 1;
      if (readall((int)fd, L->interp, n, (int64_t)ph[i].offset) != (int64_t)n) goto out;
      L->interp[n] = 0;
    }
  }
  if (lo == ~0ull) goto out;
  lo &= ~(LX_PAGE - 1);
  hi = (hi + LX_PAGE - 1) & ~(LX_PAGE - 1);
  uint64_t bias = 0;
  if (eh.type == ET_DYN) {
    uint64_t base = want_base;
    if (!base) {
      int64_t a = mem_map(0, hi - lo, 0, LX_MAP_PRIVATE | LX_MAP_ANON, -1, 0);
      if (a < 0) { err = a; goto out; }
      base = (uint64_t)a;
    }
    bias = base - lo;
  }
  if (lo + bias < g_lo || hi + bias > g_size - LX_PAGE) { err = -ENOMEM; goto out; }
  /* Reserve the whole span first, so nothing lands in the gaps between segments. */
  if (mem_map(lo + bias, hi - lo, 0, LX_MAP_PRIVATE | LX_MAP_ANON | LX_MAP_FIXED, -1, 0) < 0) { err = -ENOMEM; goto out; }
  for (int i = 0; i < eh.phnum; i++) {
    if (ph[i].type != PT_LOAD || ph[i].memsz == 0) continue;
    uint64_t va = ph[i].vaddr + bias;
    if (ph[i].filesz > ph[i].memsz) goto out;
    if (readall((int)fd, g_mem + va, ph[i].filesz, (int64_t)ph[i].offset) != (int64_t)ph[i].filesz) goto out;
    uint64_t pa = va & ~(LX_PAGE - 1), pe = (va + ph[i].memsz + LX_PAGE - 1) & ~(LX_PAGE - 1);
    int prot = (ph[i].flags & PF_R ? LX_PROT_READ : 0) | (ph[i].flags & PF_W ? LX_PROT_WRITE : 0) |
               (ph[i].flags & PF_X ? LX_PROT_EXEC : 0);
    mem_protect(pa, pe - pa, prot);
  }
  L->bias = bias;
  L->entry = eh.entry + bias;
  L->phnum = eh.phnum;
  L->phent = eh.phentsize;
  L->end = hi + bias;
  L->phdr = 0;
  for (int i = 0; i < eh.phnum; i++)
    if (ph[i].type == PT_PHDR) L->phdr = ph[i].vaddr + bias;
  if (!L->phdr) /* no PT_PHDR: the headers are inside the first segment that maps offset 0 */
    for (int i = 0; i < eh.phnum; i++)
      if (ph[i].type == PT_LOAD && ph[i].offset <= eh.phoff && eh.phoff < ph[i].offset + ph[i].filesz) {
        L->phdr = ph[i].vaddr + bias + (eh.phoff - ph[i].offset);
        break;
      }
  err = 0;
out:
  free(ph);
  hb1(LX_SYS_close, fd);
  return err;
}

/* Push helpers: the stack grows down from sp. */
static uint64_t push_bytes(uint64_t *sp, const void *p, uint64_t n) {
  *sp -= n;
  memcpy(g_mem + *sp, p, n);
  return *sp;
}

int64_t elf_load(const char *hostpath, const char *execfn, char *const argv[], char *const envp[], struct lx_image *img) {
  struct loaded P, I;
  int64_t r = load_one(hostpath, &P, LX_PIE_BASE, 0);
  if (r < 0) return r;
  uint64_t entry = P.entry, base = 0;
  if (P.interp[0]) {
    char hp[LX_PATH_MAX];
    r = path_to_host(LX_AT_FDCWD, P.interp, 1, hp, sizeof hp);
    if (r < 0) return r;
    uint64_t ib = (P.end + LX_INTERP_GAP) & ~(LX_PAGE - 1);
    r = load_one(hp, &I, ib, 1);
    if (r < 0) return r == -ENOENT ? -ENOENT : r;
    entry = I.entry;
    base = I.bias;
  }
  /* brk starts after the program (not the interpreter), as on Linux. When the
     interpreter sits right above the program, start brk above the interpreter instead,
     so that the heap can grow. */
  uint64_t brk = P.end;
  if (P.interp[0]) brk = I.end;
  mem_set_brk(brk + LX_PAGE);

  /* stack */
  uint64_t top = mem_stack_top();
  int64_t sa = mem_map(top - (8ull << 20), 8ull << 20, LX_PROT_READ | LX_PROT_WRITE,
                       LX_MAP_PRIVATE | LX_MAP_ANON | LX_MAP_FIXED, -1, 0);
  if (sa < 0) return sa;
  uint64_t sp = top;
  int argc = 0, envc = 0;
  while (argv[argc]) argc++;
  while (envp[envc]) envc++;
  /* Strings first, at the very top: execfn, then env and argv strings. */
  uint64_t execfn_g = push_bytes(&sp, execfn, strlen(execfn) + 1);
  uint64_t *envg = malloc((envc + 1) * 8), *argg = malloc((argc + 1) * 8);
  if (!envg || !argg) { free(envg); free(argg); return -ENOMEM; }
  for (int i = envc - 1; i >= 0; i--) envg[i] = push_bytes(&sp, envp[i], strlen(envp[i]) + 1);
  for (int i = argc - 1; i >= 0; i--) argg[i] = push_bytes(&sp, argv[i], strlen(argv[i]) + 1);
  uint64_t plat_g = 0;
  if (arch_cur->platform) plat_g = push_bytes(&sp, arch_cur->platform, strlen(arch_cur->platform) + 1);
  uint8_t rnd[16];
  if (hb3(LX_SYS_getrandom, hbp(rnd), 16, 0) != 16) for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(i * 37 + 11);
  sp &= ~15ull;
  uint64_t rnd_g = push_bytes(&sp, rnd, 16);
  sp &= ~15ull;
  uint64_t auxv[2 * 24];
  int n = 0;
#define AUX(k, v) (auxv[n++] = (k), auxv[n++] = (v))
  AUX(LX_AT_PHDR, P.phdr);
  AUX(LX_AT_PHENT, P.phent);
  AUX(LX_AT_PHNUM, P.phnum);
  AUX(LX_AT_PAGESZ, LX_PAGE);
  AUX(LX_AT_BASE, base);
  AUX(LX_AT_FLAGS, 0);
  AUX(LX_AT_ENTRY, P.entry);
  AUX(LX_AT_UID, (uint64_t)lx_uid());
  AUX(LX_AT_EUID, (uint64_t)lx_uid());
  AUX(LX_AT_GID, (uint64_t)lx_gid());
  AUX(LX_AT_EGID, (uint64_t)lx_gid());
  AUX(LX_AT_HWCAP, arch_cur->hwcap);
  AUX(LX_AT_CLKTCK, 100);
  AUX(LX_AT_SECURE, 0);
  AUX(LX_AT_RANDOM, rnd_g);
  AUX(LX_AT_EXECFN, execfn_g);
  if (plat_g) AUX(LX_AT_PLATFORM, plat_g);
  AUX(LX_AT_NULL, 0);
#undef AUX
  /* Total words below: argc + argv + NULL + envp + NULL + auxv. Keep sp 16-aligned at the end. */
  uint64_t words = 1 + (uint64_t)argc + 1 + (uint64_t)envc + 1 + (uint64_t)n;
  if (words & 1) sp -= 8;
  sp -= words * 8;
  uint64_t *w = (uint64_t *)(void *)(g_mem + sp);
  uint64_t k = 0;
  w[k++] = (uint64_t)argc;
  for (int i = 0; i < argc; i++) w[k++] = argg[i];
  w[k++] = 0;
  for (int i = 0; i < envc; i++) w[k++] = envg[i];
  w[k++] = 0;
  for (int i = 0; i < n; i++) w[k++] = auxv[i];
  free(envg);
  free(argg);

  img->entry = entry;
  img->sp = sp;
  img->phdr = P.phdr;
  img->phent = P.phent;
  img->phnum = P.phnum;
  img->base = base;
  img->prog_entry = P.entry;
  img->brk = brk;
  img->machine = arch_cur->e_machine;
  return 0;
}

/* Is this file an ELF program for the current architecture? 1 yes, 0 no. */
int elf_probe(const unsigned char *head, int n) {
  return n >= 20 && !memcmp(head, "\177ELF", 4) && head[4] == 2 && head[5] == 1 &&
         (head[18] | head[19] << 8) == arch_cur->e_machine;
}
