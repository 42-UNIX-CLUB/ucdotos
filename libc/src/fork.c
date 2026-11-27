// SPDX-License-Identifier: GPL-2.0-or-later
/* fork(), vfork() and fork_call(), on top of the clone primitive (SYS_pclone).
 *
 * The clone primitive copies memory and globals into a new instance, but not the engine's
 * call stack, which nothing can read. The child starts fresh at the export __fork_child.
 *  - fork_call(fn, arg): the child runs fn(arg) and exits. Works in every program.
 *  - fork(), vfork(): need the program to have gone through the build-time stack transform
 *    (USE_FORK, tools/wasmfork.c). fork() then unwinds the stack to __fork_catch, saving
 *    each frame's locals into `frames`, clones, and both processes rewind their stacks from
 *    the saved frames: the parent with the child's pid, the child with 0.
 *    Untransformed, they fail with ENOSYS. vfork() is fork() (a copy, not shared memory). */
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int __fork_ctl(int op, int val);
long __fork_point(void);
int __fork_catch(int (*fn)(void), int rewind);
/* __fork_ctl operations, as tools/wasmfork.c implements them */
enum { FC_READY, FC_GET_STATE, FC_SET_STATE, FC_GET_FP, FC_SET_FP, FC_GET_RESULT, FC_SET_RESULT, FC_SET_END };
enum { ST_NORMAL, ST_UNWIND, ST_REWIND };

static char frames[65536] __attribute__((aligned(8))); /* saved frames while forking */
static int saved_fp;
static int (*saved_main)(void);
static enum { M_NONE, M_FORK, M_CALL, M_MIGRATE } mode;
int __migrate_ranges(unsigned *out, int cap, unsigned low);
extern void (*__migrate_hook)(void);
static unsigned mranges[2 * 1025];
static int (*call_fn)(void *);
static void *call_arg;

/* crt0 runs main through here when this file is linked (it refers to it weakly) */
int __fork_main(int (*fn)(void)) {
  saved_main = fn;
  return __fork_catch(fn, 0);
}

pid_t fork(void) {
  if (!__fork_ctl(FC_READY, 0)) { errno = ENOSYS; return -1; }
  mode = M_FORK;
  __fork_ctl(FC_SET_FP, (int)(long)frames);
  __fork_ctl(FC_SET_END, (int)(long)(frames + sizeof frames));
  return __sysret(__fork_point());
}
pid_t vfork(void) { return fork(); }

/* migrate(): move to a fresh memory holding only live data (libc/src/migrate.c) and carry
 * on right here, with the call stack rebuilt. 0 after the move, -1 if it failed. */
static void migrate_resume(void) {
  __fork_ctl(FC_SET_FP, saved_fp);
  __fork_ctl(FC_SET_RESULT, 0);
  __fork_ctl(FC_SET_STATE, ST_REWIND);
  exit(__fork_catch(saved_main, 1));
}
int migrate(void) {
  if (!__fork_ctl(FC_READY, 0)) { errno = ENOSYS; return -1; }
  mode = M_MIGRATE;
  __migrate_hook = migrate_resume;
  __fork_ctl(FC_SET_FP, (int)(long)frames);
  __fork_ctl(FC_SET_END, (int)(long)(frames + sizeof frames));
  long r = __fork_point();
  __migrate_hook = 0;
  mode = M_NONE;
  return __sysret(r);
}

/* Called by the transformed __fork_catch once the stack is unwound to it. */
void __fork_unwound(void) {
  int fp = __fork_ctl(FC_GET_FP, 0);
  if (fp > (int)(long)(frames + sizeof frames)) { /* the frames did not fit: the stack is lost */
    static const char msg[] = "fork: call stack too deep to fork (frame buffer full)\n";
    write(2, msg, sizeof msg - 1);
    _exit(127);
  }
  saved_fp = fp; /* the child reads it from its copy of memory */
  long r;
  if (mode == M_MIGRATE) { /* the frames are saved in memory: move, and the new instance rewinds them */
    volatile char here;
    int n = __migrate_ranges(mranges, 1025, ((unsigned)(unsigned long)&here - 256) & ~15u);
    r = __syscall3(SYS_pmigrate, mranges, n, 0); /* returns only if it failed */
  } else r = __syscall1(SYS_pclone, 0);
  __fork_ctl(FC_SET_RESULT, r);
  __fork_ctl(FC_SET_STATE, ST_REWIND);
}

/* fork_call: the child runs fn(arg) and exits with its result; the parent gets the pid. */
pid_t fork_call(int (*fn)(void *), void *arg) {
  mode = M_CALL;
  call_fn = fn;
  call_arg = arg;
  long r = __syscall1(SYS_pclone, 0);
  mode = M_NONE;
  return __sysret(r);
}

/* The clone primitive's entry point in the child (see ARCHITECTURE.md section 5). */
__attribute__((export_name("__fork_child"))) void __fork_child(void) {
  if (mode == M_CALL) { mode = M_NONE; exit(call_fn(call_arg)); }
  if (mode != M_FORK) _exit(127);
  __fork_ctl(FC_SET_FP, saved_fp);
  __fork_ctl(FC_SET_RESULT, 0);
  __fork_ctl(FC_SET_STATE, ST_REWIND);
  exit(__fork_catch(saved_main, 1));
}
