// SPDX-License-Identifier: GPL-2.0-or-later
// What the translator needs from wherever it runs. Three builds use it:
//   native (host cc)          unit tests of the decoder, the structuring and the emitter
//   standalone wasm32         measurements under Node and in Chromium (tools/jit/)
//   UC.OS program             bin/rvjit, linked with our libc
// Each provides these few functions (port_native.c, port_sa.c, port_ucos.c).
#ifndef JIT_PORT_H
#define JIT_PORT_H
#include <stddef.h>
#include <stdint.h>

void *jit_malloc(size_t n);
void *jit_realloc(void *p, size_t n);
void jit_free(void *p);
// Zeroed memory, aligned to 64 KiB, that the caller keeps forever (the guest's memory).
void *jit_alloc_guest(size_t n);
void jit_print(const char *s, size_t n);   // to standard output
double jit_now_ms(void);                    // monotonic milliseconds
void jit_exit(int code);
// Compile and instantiate `len` bytes of WebAssembly at `p` against this program's own
// exports (memory, function table, helpers). Returns 0, or a negative error.
// The native build cannot run wasm: it returns -1 there.
int jit_host_load(const void *p, size_t len);
// Run a function pointer obtained from the table (a translated region): (depth) -> reason.
static inline int jit_call(uint32_t slot, int depth) { return ((int (*)(int))(uintptr_t)slot)(depth); }

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);

// A small printf for all builds (the standalone build has no libc).
int jit_printf(const char *fmt, ...);
int jit_snprintf(char *buf, size_t cap, const char *fmt, ...);
#endif
