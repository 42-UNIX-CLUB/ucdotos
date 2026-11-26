// SPDX-License-Identifier: GPL-2.0-or-later
/* Placeholders for the build-time stack transform (tools/wasmfork.c). In a program that went
 * through it, the transform replaces these three bodies; otherwise they stay as they are and
 * fork() fails with ENOSYS. They live alone in this file so that the compiler cannot see
 * their bodies from their callers and fold them away. */
static volatile int opaque;
/* the transform's state: ready?, unwinding/rewinding, frame pointer, result (libc/src/fork.c) */
__attribute__((noinline)) int __fork_ctl(int op, int val) { return opaque ? op + val : 0; }
/* where fork happens: the transform makes it unwind the stack (and, rewinding, return the result) */
__attribute__((noinline)) long __fork_point(void) { return opaque ? opaque : -38 /* ENOSYS */; }
/* runs fn; the transform makes it catch the unwinding, call __fork_unwound (which clones),
 * and rewind. The call below only keeps __fork_unwound in the program for the transform. */
void __fork_unwound(void);
__attribute__((noinline)) int __fork_catch(int (*fn)(void), int rewind) {
  if (opaque > 1) __fork_unwound();
  return opaque ? rewind : fn();
}
