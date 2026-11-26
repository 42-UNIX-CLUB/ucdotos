// SPDX-License-Identifier: GPL-2.0-or-later
// Port for the standalone wasm32 build of the engine, used for measurements outside UC.OS
// (tools/jit/run.mjs under Node, tools/jit/bench.html in Chromium). No libc: a size-class
// allocator on memory.grow, and four imports from the host page.
#include "port.h"
#include "engine.h"

__attribute__((import_module("env"), import_name("jit_load"))) int env_jit_load(const void *p, size_t len);
__attribute__((import_module("env"), import_name("print"))) void env_print(const char *p, size_t len);
__attribute__((import_module("env"), import_name("now"))) double env_now(void);
__attribute__((import_module("env"), import_name("exit"))) void env_exit(int code);

// size classes 16 B .. 2 GiB; each block has an 8-byte header holding its class
static void *freel[32];
static char *arena, *arena_end;
static void *more(size_t n) {
  if (arena + n > arena_end) {
    size_t pages = (n + 65535) / 65536 + 16;
    long old = __builtin_wasm_memory_grow(0, pages);
    if (old < 0) return 0;
    if ((char *)(old * 65536) != arena_end) arena = (char *)(old * 65536);
    arena_end = (char *)((old + (long)pages) * 65536);
  }
  void *p = arena;
  arena += n;
  return p;
}
void *jit_malloc(size_t n) {
  int c = 4;
  while (((size_t)1 << c) < n + 8) c++;
  void *p = freel[c];
  if (p) freel[c] = *(void **)p;
  else { p = more((size_t)1 << c); if (!p) return 0; }
  *(uint64_t *)p = (uint64_t)c;
  return (char *)p + 8;
}
void jit_free(void *q) {
  if (!q) return;
  void *p = (char *)q - 8;
  int c = (int)*(uint64_t *)p;
  *(void **)p = freel[c];
  freel[c] = p;
}
void *jit_realloc(void *q, size_t n) {
  if (!q) return jit_malloc(n);
  int c = (int)*(uint64_t *)((char *)q - 8);
  if (n + 8 <= ((size_t)1 << c)) return q;
  void *r = jit_malloc(n);
  memcpy(r, q, ((size_t)1 << c) - 8);
  jit_free(q);
  return r;
}
void *jit_alloc_guest(size_t n) {
  long old = __builtin_wasm_memory_grow(0, (n + 65535) / 65536);
  return old < 0 ? 0 : (void *)(old * 65536);
}
int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *p = a, *q = b;
  for (size_t i = 0; i < n; i++) if (p[i] != q[i]) return p[i] - q[i];
  return 0;
}
void jit_print(const char *s, size_t n) { env_print(s, n); }
double jit_now_ms(void) { return env_now(); }
void jit_exit(int code) { env_exit(code); }
int jit_host_load(const void *p, size_t len) { return env_jit_load(p, len); }
void port_guest_write(int fd, const void *p, size_t n) { (void)fd; env_print(p, n); }
void port_timeslice(void) {}

// ---- the host's entry points ----
__attribute__((export_name("sa_alloc"))) void *sa_alloc(size_t n) { return jit_malloc(n); }
__attribute__((export_name("sa_main"))) int sa_main(const uint8_t *elf, size_t len, const char *opts, int quiet) {
  static int inited;
  engine_defaults();
  if (engine_parse_opts(opts)) return 2;
  if (!inited) { if (engine_init()) return 3; inited = 1; }
  if (engine_load_elf(elf, len)) return 3;
  int code = 0;
  for (int r = 0; r < E.repeat; r++) {
    if (r) engine_reset_guest();
    double t0 = jit_now_ms();
    code = engine_run();
    jit_printf("@@RUN %d %.3f\n", r, jit_now_ms() - t0);
  }
  (void)quiet;
  engine_report();
  return code;
}
