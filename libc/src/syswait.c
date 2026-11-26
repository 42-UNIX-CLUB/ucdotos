// SPDX-License-Identifier: GPL-2.0-or-later
/* The slow half of every system call: the kernel said -ERESTARTSYS. Park until woken (the
 * host may also return at once), then run the handlers of signals the kernel has for us,
 * then the caller makes its call again. Kept out of line, so the inline fast path in
 * sys/syscall.h is one compare. __sig_hook is set by sigaction() when the program installs
 * its first handler (libc/src/signal.c); a program without handlers never pays for them. */
#include <sys/syscall.h>
void (*__sig_hook)(void);
void __syscall_wait(void) {
  __host_block();
  if (__sig_hook) __sig_hook();
}
