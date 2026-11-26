// SPDX-License-Identifier: GPL-2.0-or-later
// rvjit: run a static RV64 ELF file with the prototype engine (native and UC.OS builds).
//   rvjit [key=value ...] file.elf
// Options: see engine_parse_opts() in engine.c (mode=interp|jit|aot strategy=block|dispatch|struct
// calls=0|1 bounds=unchecked|masked|checked budget=N threshold=N lockstep=1 repeat=N stats=1 ...).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

int main(int argc, char **argv) {
  engine_defaults();
  const char *file = 0;
  for (int i = 1; i < argc; i++) {
    if (strchr(argv[i], '=')) { if (engine_parse_opts(argv[i])) return 2; }
    else file = argv[i];
  }
  if (!file) { fprintf(stderr, "usage: rvjit [key=value ...] file.elf\n"); return 2; }
  FILE *f = fopen(file, "rb");
  if (!f) { fprintf(stderr, "rvjit: cannot open %s\n", file); return 2; }
  size_t cap = 1 << 20, n = 0;
  unsigned char *buf = malloc(cap);
  for (;;) {
    size_t got = fread(buf + n, 1, cap - n, f);
    n += got;
    if (n < cap) break;
    cap *= 2;
    buf = realloc(buf, cap);
  }
  fclose(f);
  if (engine_init() || engine_load_elf(buf, n)) return 3;
  int code = 0;
  double best = 1e30;
  for (int r = 0; r < E.repeat; r++) {
    if (r) engine_reset_guest();
    double t0 = jit_now_ms();
    code = engine_run();
    double t = jit_now_ms() - t0;
    if (t < best) best = t;
    jit_printf("@@RUN %d %.3f\n", r, t);
  }
  engine_report();
  return code;
}
