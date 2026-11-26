// SPDX-License-Identifier: GPL-2.0-or-later
/* riscv64 parts of the Linux ABI that are not instructions: the signal frame
 * (struct rt_sigframe: siginfo, then ucontext with the register state, as in Linux's
 * arch/riscv uapi headers), riscv_hwprobe and riscv_flush_icache. */
#include "lx.h"
#include "rv.h"
#include "arch.h"
#include "abi_lx.h"

extern struct rv_cpu rv_cpu;
void rv_flush(uint64_t addr, uint64_t len);

/* Frame layout (bytes from the frame start), derived from the uapi structures:
 *   0     siginfo (128)
 *   128   ucontext: uc_flags 8, uc_link 8, uc_stack 24, uc_sigmask 8, padding 120
 *   304   uc_mcontext (16-aligned): sc_regs[32] (pc, x1..x31), then the FP state
 *   560   f[32] (64-bit each), 816 fcsr; the FP union is 528 bytes (Q-sized); its last
 *         header word pair at 1080 is the extension terminator (magic 0, size 0)
 *   1088  end */
#define FR_INFO 0
#define FR_UC 128
#define FR_STACK (FR_UC + 16)
#define FR_MASK (FR_UC + 40)
#define FR_REGS 304
#define FR_FP 560
#define FR_FCSR 816
#define FR_SIZE 1088

static uint64_t sigtramp; /* guest address of "li a7, 139; ecall" */
void rv_exec_reset(void) { sigtramp = 0; }

static void put64(uint64_t a, uint64_t v) { memcpy(g_mem + a, &v, 8); }
static void put32(uint64_t a, uint32_t v) { memcpy(g_mem + a, &v, 4); }
static uint64_t get64(uint64_t a) { uint64_t v; memcpy(&v, g_mem + a, 8); return v; }
static uint32_t get32(uint64_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }

extern uint64_t lx_altstack_sp, lx_altstack_size;
extern int lx_altstack_flags;

int rv_sig_setup(int sig, uint64_t handler, uint64_t flags, uint64_t restorer, uint64_t mask_old,
                 uint64_t sp, const struct lx_fault *info, int sicode) {
  (void)restorer;
  if (!sigtramp) {
    int64_t a = mem_map(0, LX_PAGE, LX_PROT_READ | LX_PROT_EXEC, LX_MAP_PRIVATE | LX_MAP_ANON, -1, 0);
    if (a < 0) return (int)a;
    sigtramp = (uint64_t)a;
    put32(sigtramp, 0x08b00893u); /* addi a7, zero, 139 (rt_sigreturn) */
    put32(sigtramp + 4, 0x00000073u); /* ecall */
  }
  uint64_t fr = (sp - FR_SIZE) & ~15ull;
  if (!g_okrange(fr, FR_SIZE)) return -EFAULT;
  memset(g_mem + fr, 0, FR_SIZE);
  put32(fr + 0, (uint32_t)sig);
  put32(fr + 8, (uint32_t)sicode);
  if (info && (sig == LX_SIGSEGV || sig == LX_SIGBUS || sig == LX_SIGILL || sig == LX_SIGFPE || sig == LX_SIGTRAP))
    put64(fr + 16, info->addr);
  else if (info) { put32(fr + 16, (uint32_t)info->addr); put32(fr + 20, (uint32_t)lx_uid()); }
  put64(FR_STACK + fr, lx_altstack_sp);
  put32(FR_STACK + fr + 8, (uint32_t)lx_altstack_flags);
  put64(FR_STACK + fr + 16, lx_altstack_size);
  put64(fr + FR_MASK, mask_old);
  put64(fr + FR_REGS, rv_cpu.pc);
  for (int i = 1; i < 32; i++) put64(fr + FR_REGS + 8 * (uint64_t)i, rv_cpu.x[i]);
  for (int i = 0; i < 32; i++) put64(fr + FR_FP + 8 * (uint64_t)i, rv_cpu.f[i]);
  put32(fr + FR_FCSR, rv_cpu.fcsr);
  rv_cpu.x[RV_REG_SP] = fr;
  rv_cpu.x[RV_REG_A0] = (uint64_t)sig;
  rv_cpu.x[RV_REG_A0 + 1] = fr + FR_INFO;
  rv_cpu.x[RV_REG_A0 + 2] = fr + FR_UC;
  rv_cpu.x[RV_REG_RA] = sigtramp;
  rv_cpu.pc = handler;
  (void)flags;
  return 0;
}

int rv_sig_return(uint64_t *mask_out) {
  uint64_t fr = rv_cpu.x[RV_REG_SP];
  if (!g_okrange(fr, FR_SIZE)) return -EFAULT;
  *mask_out = get64(fr + FR_MASK);
  rv_cpu.pc = get64(fr + FR_REGS);
  for (int i = 1; i < 32; i++) rv_cpu.x[i] = get64(fr + FR_REGS + 8 * (uint64_t)i);
  for (int i = 0; i < 32; i++) rv_cpu.f[i] = get64(fr + FR_FP + 8 * (uint64_t)i);
  rv_cpu.fcsr = get32(fr + FR_FCSR) & 0xff;
  return 0;
}

/* riscv_hwprobe(pairs, count, cpusetsize, cpus, flags): answer what this interpreter
 * really implements. Misaligned scalar accesses cost the same as aligned ones here. */
int64_t rv_sys_arch(int nr, uint64_t a[6]) {
  if (nr == LX_SYS_riscv_flush_icache) { rv_flush(a[0], a[1] - a[0]); return 0; }
  if (nr != LX_SYS_riscv_hwprobe) return -ENOSYS;
  uint64_t pairs = a[0], n = a[1];
  if (a[4] & ~1ull) return -EINVAL;
  if (n && !g_okrange(pairs, n * 16)) return -EFAULT;
  for (uint64_t i = 0; i < n; i++) {
    int64_t key = (int64_t)get64(pairs + 16 * i);
    uint64_t v = 0;
    switch (key) {
    case 0: case 1: case 2: v = 0; break;               /* vendor, arch, impl ids */
    case 3: v = 1; break;                               /* BASE_BEHAVIOR_IMA */
    case 4: {                                           /* IMA_EXT_0 */
      extern unsigned rv_decode_ext;
      v = 1 | 2;                                        /* FD, C */
      if (rv_decode_ext & RV_EXT_ZBA) v |= 1ull << 3;
      if (rv_decode_ext & RV_EXT_ZBB) v |= 1ull << 4;
      if (rv_decode_ext & RV_EXT_ZBS) v |= 1ull << 5;
      v |= 1ull << 43; /* ZCA: the C instructions other than FP loads/stores */
      v |= 1ull << 45; /* ZCD: compressed double loads/stores */
      break;
    }
    case 5: v = 3; break;                               /* CPUPERF_0: misaligned fast */
    case 7: v = g_size - 1; break;                      /* highest user virtual address */
    case 8: v = 10000000; break;                        /* time CSR frequency (see csr_read) */
    case 9: v = 3; break;                               /* misaligned scalar: fast */
    case 10: v = 4; break;                              /* misaligned vector: unsupported */
    default: key = -1; v = 0; break;
    }
    put64(pairs + 16 * i, (uint64_t)key);
    put64(pairs + 16 * i + 8, v);
  }
  return 0;
}
