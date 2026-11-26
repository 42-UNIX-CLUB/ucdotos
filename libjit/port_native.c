// SPDX-License-Identifier: GPL-2.0-or-later
// Port for the native (host cc) build: interpreter runs and unit tests. It cannot run
// WebAssembly, so jit_host_load fails and the engine interprets.
#include "port.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "engine.h"

void *jit_malloc(size_t n) { return malloc(n); }
void *jit_realloc(void *p, size_t n) { return realloc(p, n); }
void jit_free(void *p) { free(p); }
void *jit_alloc_guest(size_t n) { return calloc(1, n); }
void jit_print(const char *s, size_t n) { fwrite(s, 1, n, stdout); fflush(stdout); }
double jit_now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
void jit_exit(int code) { exit(code); }
int jit_host_load(const void *p, size_t len) { (void)p; (void)len; return -1; }
void port_guest_write(int fd, const void *p, size_t n) { if (write(fd, p, n) < 0) {} }
void port_timeslice(void) {}
