// SPDX-License-Identifier: GPL-2.0-or-later
/* Everything in the engine that depends on the guest architecture goes through this
 * interface. riscv64 implements it in rv_arch.c. A second front end (amd64, arm64) is a
 * new struct lx_arch, not a change to the loader, memory or system call layer. */
#ifndef LX_ARCH_H
#define LX_ARCH_H
#include <stdint.h>

/* Why arch->run returned. */
enum lx_stop { LX_STOP_SYSCALL = 1, LX_STOP_FAULT, LX_STOP_ILLEGAL, LX_STOP_BREAK, LX_STOP_BUDGET, LX_STOP_MISALIGNED };

struct lx_fault { uint64_t pc, addr; int kind; };

struct lx_arch {
  const char *name;           /* "riscv64" */
  int e_machine;              /* ELF e_machine accepted by the loader */
  uint64_t hwcap;             /* AT_HWCAP */
  const char *platform;       /* AT_PLATFORM string, or 0 for none */
  const char *uname_machine;  /* utsname.machine */
  /* Reset registers for a fresh image: pc = entry, sp = initial stack. */
  void (*reset)(uint64_t entry, uint64_t sp);
  /* Run guest code until something needs the engine. */
  int (*run)(struct lx_fault *f);
  /* System call convention: number and six arguments in, one result out. */
  int (*sys_nr)(void);
  void (*sys_args)(uint64_t a[6]);
  void (*sys_ret)(int64_t v);
  void (*sys_skip)(void);     /* step past the system call instruction */
  uint64_t (*get_pc)(void);
  uint64_t (*get_instret)(void);
  void (*set_pc)(uint64_t pc);
  uint64_t (*get_sp)(void);
  void (*set_tp)(uint64_t tp);
  void (*set_sp)(uint64_t sp);
  /* Signals: build a frame on the guest stack and point the CPU at the handler; and
   * undo it for rt_sigreturn. Return 0 or -errno (no room on the stack). */
  int (*sig_setup)(int sig, uint64_t handler, uint64_t flags, uint64_t restorer, uint64_t mask_old,
                   uint64_t sp, const struct lx_fault *info, int sicode);
  int (*sig_return)(uint64_t *mask_out);
  /* Drop every cached decoding of guest code (fence.i, munmap of code, execve). */
  void (*flush_code)(uint64_t addr, uint64_t len);
  /* Architecture-specific system calls (riscv_hwprobe, riscv_flush_icache). -ENOSYS if not ours. */
  int64_t (*sys_arch)(int nr, uint64_t a[6]);
  /* Save and restore the whole register state (fork emulation, signals). */
  unsigned state_size;
  void (*save)(void *buf);
  void (*load)(const void *buf);
};
extern const struct lx_arch *arch_cur;
extern const struct lx_arch arch_riscv64;

/* Instructions to run between two hb_tick() calls (see docs/LINUX-BINARIES.md). */
extern uint64_t lx_budget;
uint32_t lx_uid(void);
uint32_t lx_gid(void);

#endif
