// SPDX-License-Identifier: GPL-2.0-or-later
// Port for UC.OS (bin/rvjit). Generated code is loaded through the host import jit.load,
// which the prototype extension boot/jit-proto/ext.mjs provides.
#include "port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "engine.h"

__attribute__((import_module("jit"), import_name("load"))) int ucos_jit_load(const void *p, size_t len);

void *jit_malloc(size_t n) { return malloc(n); }
void *jit_realloc(void *p, size_t n) { return realloc(p, n); }
void jit_free(void *p) { free(p); }
void *jit_alloc_guest(size_t n) {
  char *p = malloc(n + 65536);
  if (!p) return 0;
  p = (char *)(((uintptr_t)p + 65535) & ~(uintptr_t)65535);
  memset(p, 0, n);
  return p;
}
void jit_print(const char *s, size_t n) { fwrite(s, 1, n, stdout); fflush(stdout); }
double jit_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
void jit_exit(int code) { exit(code); }
int jit_host_load(const void *p, size_t len) { return ucos_jit_load(p, len); }
void port_guest_write(int fd, const void *p, size_t n) { fflush(stdout); if (write(fd, p, n) < 0) {} }
// A cheap system call: the kernel checks the time slice on every 256th call and parks us if
// our 8 ms are up. With the default budget that is about every million back edges.
void port_timeslice(void) { getppid(); }
