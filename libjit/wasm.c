// SPDX-License-Identifier: GPL-2.0-or-later
// WebAssembly binary emitter: buffers and LEB128.
#include "wasm.h"
#include "port.h"

void wb_init(wbuf *b) { b->p = 0; b->n = b->cap = 0; }
void wb_free(wbuf *b) { jit_free(b->p); wb_init(b); }

void wb_reserve(wbuf *b, uint32_t extra) {
  if (b->n + extra <= b->cap) return;
  uint32_t cap = b->cap ? b->cap : 256;
  while (cap < b->n + extra) cap *= 2;
  b->p = jit_realloc(b->p, cap);
  b->cap = cap;
}

void wb_bytes(wbuf *b, const void *p, uint32_t n) {
  wb_reserve(b, n);
  memcpy(b->p + b->n, p, n);
  b->n += n;
}

void wb_uleb(wbuf *b, uint64_t v) {
  wb_reserve(b, 10);
  do {
    uint8_t c = v & 0x7f;
    v >>= 7;
    if (v) c |= 0x80;
    b->p[b->n++] = c;
  } while (v);
}

void wb_sleb(wbuf *b, int64_t v) {
  wb_reserve(b, 10);
  for (;;) {
    uint8_t c = v & 0x7f;
    v >>= 7;                       // arithmetic shift: clang and gcc define it for signed values
    if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) { b->p[b->n++] = c; return; }
    b->p[b->n++] = c | 0x80;
  }
}

void wb_name(wbuf *b, const char *s) {
  uint32_t n = 0;
  while (s[n]) n++;
  wb_uleb(b, n);
  wb_bytes(b, s, n);
}

void wb_vec(wbuf *b, uint32_t count, const wbuf *items) {
  wb_uleb(b, count);
  wb_bytes(b, items->p, items->n);
}

void wb_section(wbuf *mod, int id, const wbuf *body) {
  wb_byte(mod, (uint8_t)id);
  wb_uleb(mod, body->n);
  wb_bytes(mod, body->p, body->n);
}
