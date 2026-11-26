// SPDX-License-Identifier: GPL-2.0-or-later
// Native unit test of the translator: decode and translate every function of a guest ELF
// file with every structuring strategy and bounds mode, write the modules to a directory,
// and print statistics. tools/jit/validate.mjs then validates and instantiates them.
//   xltest guest.elf outdir
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xlat.h"
#include "port.h"

static uint8_t *mem;
uint8_t *rv_mem;              // the interpreter half of rvfp.c is linked but not used here
uint64_t rv_memsize;
void rv_store_hook(uint64_t a, int n) { (void)a; (void)n; }
void engine_frm_changed(void) {}
static const uint64_t memsize = 64u << 20;
static uint32_t next_slot = 100;

uint32_t engine_slot_for(uint64_t pc) { (void)pc; return next_slot++; }
int engine_site_targets(uint64_t site, uint64_t *out, int cap) { (void)site; (void)out; (void)cap; return 0; }
void engine_site_in_region(uint64_t site) { (void)site; }
int engine_callee_summary(uint64_t pc, uint64_t *u, uint64_t *d) { (void)pc; (void)u; (void)d; return 0; }
int engine_readable(uint64_t pc, uint32_t *raw) {
  if (pc & 1 || pc > memsize - 4) return 0;
  memcpy(raw, mem + pc, 4);
  return 1;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: xltest file.elf outdir\n"); return 2; }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 2; }
  static uint8_t elf[8 << 20];
  size_t len = fread(elf, 1, sizeof elf, f);
  fclose(f);
  mem = calloc(1, memsize + 65536);
  uint64_t phoff, shoff; uint16_t phentsize, phnum, shentsize, shnum;
  memcpy(&phoff, elf + 0x20, 8); memcpy(&phentsize, elf + 0x36, 2); memcpy(&phnum, elf + 0x38, 2);
  memcpy(&shoff, elf + 0x28, 8); memcpy(&shentsize, elf + 0x3a, 2); memcpy(&shnum, elf + 0x3c, 2);
  uint64_t text = 0;
  for (int i = 0; i < phnum; i++) {
    const uint8_t *ph = elf + phoff + (uint64_t)i * phentsize;
    uint32_t type, flags; uint64_t off, vaddr, filesz;
    memcpy(&type, ph, 4); memcpy(&flags, ph + 4, 4); memcpy(&off, ph + 8, 8); memcpy(&vaddr, ph + 0x10, 8); memcpy(&filesz, ph + 0x20, 8);
    if (type == 1 && off + filesz <= len) { memcpy(mem + vaddr, elf + off, filesz); if (flags & 1) text += filesz; }
  }
  uint64_t funcs[4096]; int nf = 0;
  for (int i = 0; i < shnum; i++) {
    const uint8_t *sh = elf + shoff + (uint64_t)i * shentsize;
    uint32_t type; uint64_t off, size, entsize;
    memcpy(&type, sh + 4, 4); memcpy(&off, sh + 0x18, 8); memcpy(&size, sh + 0x20, 8); memcpy(&entsize, sh + 0x38, 8);
    if (type != 2 || !entsize) continue;
    for (uint64_t k = 0; k < size / entsize && nf < 4096; k++) {
      const uint8_t *sym = elf + off + k * entsize;
      uint64_t value; memcpy(&value, sym + 8, 8);
      if ((sym[4] & 15) == 2 && value) funcs[nf++] = value;
    }
  }
  static const char *sname[] = { "block", "dispatch", "struct" };
  static const char *bname[] = { "unchecked", "masked", "checked" };
  int fails = 0;
  for (int st = 0; st < 3; st++) for (int bo = 0; bo < 3; bo++) for (int calls = 0; calls < 2; calls++) {
    xlat_opts o = { .strategy = st, .calls = calls, .bounds = bo, .budget = 1, .max_insns = 4096, .max_blocks = 512, .indirect = calls };
    xlat_env env = { .a_x = 1024, .a_f = 1024 + 264, .a_pc = 1024 + 256, .a_fcsr = 1024 + 520, .a_res = 1024 + 528, .a_budget = 1024 + 524, .a_icount = 1024 + 536, .a_fault = 1024 + 544, .a_site = 1024 + 568, .gbase = 1 << 20, .gsize = memsize };
    xmodule m;
    xl_module_init(&m);
    for (int i = 0; i < nf; i++) xl_add_entry(&m, funcs[i], 10 + (uint32_t)i);
    if (xl_translate_all(&m, &env, &o) < 0) { printf("FAIL label bug: %s %s calls=%d\n", sname[st], bname[bo], calls); fails++; }
    wbuf out; wb_init(&out);
    xl_module_bytes(&m, &out);
    char path[512];
    snprintf(path, sizeof path, "%s/%s-%s-calls%d.wasm", argv[2], sname[st], bname[bo], calls);
    FILE *w = fopen(path, "wb");
    if (!w) { perror(path); return 2; }
    fwrite(out.p, 1, out.n, w);
    fclose(w);
    printf("%-9s %-9s calls=%d: %d functions, %d guest instructions (%llu bytes of code), %d blocks, %d irreducible, %u bytes of wasm (%.2f per guest byte)\n",
           sname[st], bname[bo], calls, m.nfunc, m.ninsn_total, (unsigned long long)text, m.nblocks_total, m.irreducible, out.n, (double)out.n / (double)text);
    wb_free(&out);
    xl_module_free(&m);
  }
  wbuf sup; wb_init(&sup);
  xl_support_module(&sup, 4096);
  char path[512];
  snprintf(path, sizeof path, "%s/support.wasm", argv[2]);
  FILE *w = fopen(path, "wb"); fwrite(sup.p, 1, sup.n, w); fclose(w);
  return fails ? 1 : 0;
}
