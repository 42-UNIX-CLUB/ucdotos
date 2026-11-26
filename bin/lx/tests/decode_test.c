// SPDX-License-Identifier: GPL-2.0-or-later
/* Helper for decode_test.py: reads hex instruction words (one per line) and prints what
 * the decoder makes of each: "op rd rs1 rs2 rs3 rm len imm", or "ILLEGAL". */
#include <stdio.h>
#include <stdlib.h>
#include "../rv.h"

int main(int argc, char **argv) {
  char line[128];
  if (argc > 1) rv_decode_ext = RV_EXT_ZBA | RV_EXT_ZBB | RV_EXT_ZBS;
  while (fgets(line, sizeof line, stdin)) {
    unsigned long w = strtoul(line, 0, 16);
    struct rv_insn in;
    rv_decode((uint32_t)w, &in);
    if (in.op == RV_ILLEGAL) { printf("ILLEGAL\n"); continue; }
    printf("%s %u %u %u %u %u %u %lld\n", rv_op_name[in.op], in.rd, in.rs1, in.rs2, in.rs3, in.rm, in.len, (long long)in.imm);
  }
  return 0;
}
