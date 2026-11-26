// SPDX-License-Identifier: GPL-2.0-or-later
// A program that does almost nothing: for start-up cost (a short command), not speed.
#define REPS 1
#include "rt.h"
uint64_t work(void) { uint64_t s = 0; for (int i = 0; i < 100; i++) s += (uint64_t)i * i; return s; }
