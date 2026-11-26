# Running Linux binaries

The living design document for the engine that runs Linux programs from Debian and Ubuntu
packages on UC.OS. It is also how the engine (interpreter, loader, system call layer) and
the binary translator coordinate. Keep it true: change it in the same piece of work as the
code. Dates are 2026-11-26 unless stated.

The engine's name is a placeholder: `lx` (`LX_NAME` in `bin/lx/lx.h`, and the directory
`bin/lx/`). Naming it is the owners' call.

Contents: 1 decisions and assumptions, 2 the guest architecture (facts), 3 module map and
interfaces, 4 what runs, 5 speed, 6 memory, 7 system calls (table from real programs, and
what the engine needs from the kernel), 8 processes: fork, exec, ELF dispatch, 9 signals,
10 limits, 11 how to test, 12 open decisions, 13 sources.

## 1. Decisions and assumptions

- **Decided by the owners:** install `.deb` packages from Debian or Ubuntu and run what is
  inside, as the highest priority. Direction: translate guest code to WebAssembly (option
  D) "if this would run better than option C" (a user-mode interpreter). A translator needs
  an interpreter anyway (reference, cold code, fallback), so both are built: this engine
  (C) and a translator (option D, a separate piece of work, not merged yet).
- **Assumed, for the owners to confirm or overturn:**

| Assumption | Why | How to reverse |
|---|---|---|
| First guest architecture: **riscv64** | smallest job with an official Debian port; its system call numbers are UC.OS's (section 2) | a second front end implements `struct lx_arch` (`bin/lx/arch.h`) |
| Name `lx` | a placeholder | `LX_NAME`, directory name |
| Guest root: `/debian` if that directory exists, else `/` | the package manager installs under `/debian` (its placeholder) | `--root=DIR` or `LX_ROOT` |
| What `uname` shows the guest: sysname `Linux`, release `6.12.0-lx`, machine `riscv64`, node name from the host | programs check sysname and parse the release; 6.12 is Debian 13's kernel | `sys_uname()` in `bin/lx/sys.c` |
| Guest user id: the host's (UC.OS: 1000) unless `LX_UID` is set | truthful by default | `lx_uid()` in `bin/lx/sys.c` |
| Memory protection recorded, not enforced per access | cost (section 6) | build with `-DLX_MEMCHECK=2` |
| Fork on UC.OS emulated until the kernel has a clone primitive | section 8 | |

## 2. The guest architecture: facts for the owners' choice

This is an open owner decision. The engine starts with riscv64 as the measurement vehicle.

### 2.1 What the mirrors carry

Binary packages, counted from the real `Packages` indices on 2026-11-26 (Debian 13.8
"trixie" `main`; Ubuntu 26.04 "resolute" release pocket; archive.ubuntu.com for amd64 and
i386, ports.ubuntu.com for the others). "Arch-specific" excludes `Architecture: all`
packages, which are listed under every architecture.

| | amd64 | arm64 | riscv64 | i386 | armhf |
|---|---|---|---|---|---|
| Debian 13 main, all | 68,829 | 68,200 | 66,169 | 67,030 | 65,295 |
| Debian 13 main, arch-specific | 35,135 | 34,506 | 32,474 | 33,336 | 31,601 |
| Debian port status | official | official | official (new in 13) | official (13 keeps it for multiarch; no installer) | official |
| Ubuntu 26.04 main, arch-specific | 3,848 | 3,798 | 3,297 | 2,034 | 3,234 |
| Ubuntu 26.04 universe, arch-specific | 33,550 | 32,756 | 30,592 | 3,930 | 29,366 |
| Ubuntu 26.04 also offers | `amd64v3` variant (33,513 universe) | | | partial (multiarch only) | |

Debian 13 required set (Priority: required, without Recommends), from the package manager
stream's resolver against the same indices: packages / download MB / installed MB: amd64
89 / 35.4 / 132, arm64 89 / 34.0 / 156, riscv64 88 / 34.8 / 116. bash: 13 / 7.2 / 28,
13 / 6.7 / 40, 13 / 6.8 / 23. gcc: 38 / 56.5 / 211, 37 / 49.3 / 212, 36 / 64.6 / 305.
python3-minimal: 19 / 12.5 / 48, 19 / 12.1 / 59, 19 / 12.0 / 41.

### 2.2 Baseline instruction set per release

| | Debian 13 | Ubuntu 26.04 LTS |
|---|---|---|
| amd64 | x86-64 baseline (SSE2) | x86-64 baseline; an extra `amd64v3` archive (AVX2, BMI, FMA) is listed in the Release file |
| arm64 | ARMv8.0-A; FP and Advanced SIMD (128-bit) are part of the ABI; glibc picks string routines by HWCAP | same |
| riscv64 | **RV64GC**: measured, the ELF attribute of Debian's busybox and libc.so.6 is `rv64i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_zicsr2p0_zifencei2p0_zmmul1p0_zaamo1p0_zalrsc1p0` | **RVA23**: measured, Ubuntu 26.04's busybox-static carries `..._b1p0_v1p0_..._zicond..._zfa..._zcb..._zba_zbb_zbs..._zvbb..._zvkb...`, and contains 4,895 vector instructions; the engine stops on the first one with SIGILL today |

Consequence: Debian riscv64 needs only RV64GC (done). Ubuntu 26.04 riscv64 would need the
vector extension (V, scalable vectors) plus about fifteen smaller extensions.

### 2.3 Size of the job: what real binaries use

Measured with `bin/lx/tests/isa_census.sh` on 2026-11-26: llvm-objdump over Debian 13's
busybox (static), bash and `libc.so.6` for each architecture (a sample, to keep the run
short on a shared machine). "Forms" are mnemonic plus operand kinds (register class,
memory, immediate): for x86 a lower bound on the distinct encodings to handle, since it
does not separate prefixes, operand sizes and addressing modes. RISC-V and arm64 are
counted without aliases (`-M no-aliases`), so RISC-V compressed forms count separately
(`c.ld`, `c.mv`, ...) although they decode to the same base instructions.

| | amd64 | arm64 | riscv64 |
|---|---|---|---|
| `.text` bytes of the three files | 3,888,779 | 3,576,492 | 2,669,638 |
| static instructions | 963,567 | 883,337 | 927,682 |
| average instruction size | 4.04 bytes | 4 bytes | 2.88 bytes |
| distinct mnemonics | 405 | 204 | 175 (about 110 base instructions plus compressed forms) |
| distinct forms | 754 | 456 | 537 |
| mnemonics covering 99% / 99.9% of instructions | 139 / 257 | 56 / 105 | 68 / 103 |
| integer / scalar FP / vector | 95.5% / 0.1% / 4.5% | 99.1% / 0.2% / 0.7% | 99.9% / 0.1% / 0.0% |
| most frequent | movq 17.4%, movl 9.9%, callq 5.7%, je 5.0%, leaq 4.7% | ldr 12.0%, orr (mov) 10.1%, add 8.5%, mov 7.3%, bl 6.3% | c.mv 8.9%, c.ldsp 8.0%, jal 6.8%, c.li 6.8%, c.sdsp 6.8% |

The x86 vector share is mostly SSE2 in glibc's string functions (selected at run time;
the AVX2 and AVX-512 variants are in the same `libc.so.6`). The whole RV64GC instruction
set the engine implements is about 200 operations including FP and atomics, all of it
needed: the decoder has no "rare" tail to skip.

### 2.4 Decoding and translating to WebAssembly

| | amd64 | arm64 | riscv64 |
|---|---|---|---|
| Instruction length | 1 to 15 bytes, prefixes, ModRM/SIB | fixed 4 bytes | 4 bytes, or 2 with C (compressed) |
| Condition flags | 6 flags written by most ALU instructions; an interpreter or translator must compute them lazily | NZCV, written only by `S` forms and compares | none (compare-and-branch) |
| Addressing | base + index*scale + disp, memory operands on most instructions | load/store architecture, rich addressing modes, pre/post index | load/store, base + 12-bit offset only |
| Vectors in baseline | SSE2 128-bit: maps onto wasm SIMD 128; AVX2 (v3) 256-bit must be split | Advanced SIMD 128-bit: maps onto wasm SIMD 128; SVE is not in the baseline | none in Debian (RV64GC); V (scalable, RVA23) in Ubuntu 26.04: vl/vtype/masks/register grouping are a large job |
| Self-modifying code | caches are coherent, no instruction marks the change: a translator must notice writes to code pages | explicit `DC CVAU / IC IVAU / ISB` (`__clear_cache`) | explicit `fence.i` or the `riscv_flush_icache` system call |
| Thread pointer | `fs` base, set by `arch_prctl` | `TPIDR_EL0`, set by the program itself | register `tp` (x4) |
| System call | `syscall`; nr in rax, args rdi rsi rdx r10 r8 r9 | `svc #0`; nr in x8, args x0..x5 | `ecall`; nr in a7, args a0..a5 |
| Atomics | `lock` prefix, cmpxchg, TSO memory model | LDXR/STXR, LSE atomics (glibc selects by HWCAP), weak memory model | LR/SC and AMO, weak memory model (RVWMO) |
| Floating point | SSE scalar (x87 for long double) | scalar FP in SIMD registers | F and D registers, 5 rounding modes in each instruction |
| Endianness | little | little | little |
| Page size | 4 KiB | 4 KiB on Debian kernels; binaries are linked for up to 64 KiB | 4 KiB |

Guest threads run one at a time in one engine (section 8), so the memory models do not
matter yet; they would with true parallel threads.

### 2.5 System call numbering and structure layouts

| | amd64 | arm64 | riscv64 | i386 | armhf |
|---|---|---|---|---|---|
| Table | its own (`syscall_64.tbl`) | generic (`asm-generic/unistd.h`), same as UC.OS | generic, same as UC.OS (plus `riscv_hwprobe` 258, `riscv_flush_icache` 259) | its own | its own (EABI) |
| struct stat | 144 bytes, x86 order | generic 64-bit, 128 bytes | generic 64-bit, 128 bytes | stat64 | stat64 |
| Pointers and long | 64-bit | 64-bit | 64-bit | 32-bit | 32-bit |

The UC.OS kernel uses the generic numbers with wasm32 layouts (32-bit `long`, its own
`struct stat` without nanoseconds, `timespec` with a 32-bit `tv_nsec`), so even for riscv64
the engine converts structures (`bin/lx/sys.c`); only the numbers coincide.

### 2.6 A 64-bit guest in a 32-bit WebAssembly memory

The engine places every guest mapping itself (section 6), so a 64-bit guest is kept inside
a window of the engine's own memory and a guest address is an offset into it. The cost per
access is one unsigned compare (`a - g_lo <= g_span`) before a plain wasm load; the wasm
engine's own bounds check is free on 64-bit hosts (guard pages). Measured cost of that
compare: section 6. With the WebAssembly memory64 proposal (shipped in Chrome 133 and
Firefox 134) the engine could use more than 4 GiB, but every wasm access then needs an
explicit bounds check: Mozilla reports a slowdown "from just 10% to over 100%" for
memory64 code. The practical guest limit in 32-bit wasm: the window (default 1 GiB,
`--mem=MIB`) plus the engine must fit in 4 GiB, so about 3 GiB.

### 2.7 Other differences

- Third-party `.deb` files outside the mirrors (browsers, editors, games, vendor tools) are
  overwhelmingly amd64, some arm64, almost none riscv64. Not measured; general knowledge.
- Code density: section 2.3 has `.text` sizes of the same programs.

## 3. Module map and interfaces

```
bin/lx/
  rv.h rv_decode.c rv_disasm.c   riscv64 front end: CPU state, decoder (pure)      <- translator reuses
  softfp.c softfp.h              IEEE 754 binary32/64 in software (RISC-V rules)   <- translator reuses
  rv_fp.c rv_fp.h                host-float fast paths, proven equal to softfp     <- translator reuses
  rv_interp.c rv_ops.inc         reference interpreter: block cache, two dispatch modes
  rv_arch.c                      riscv64 signal frame, riscv_hwprobe, riscv_flush_icache
  arch.h                         the architecture interface (struct lx_arch)
  mem.c                          guest address space inside the engine's memory    <- translator reuses
  elf.c                          ELF64 loader, initial stack, auxiliary vector
  path.c path.h                  guest root: path translation (a chroot in user space)
  sys.c sys_sig.c sys_proc.c     Linux system call layer: files, memory, time / signals / processes
  sys.h abi_lx.h sysnames.h      guest ABI (numbers, structures), call names (generated)
  host.h                         the lowest level, swappable:
  host_linux.c                     the host's Linux (native builds: a user-mode emulator)
  host_ucos.c                      UC.OS kernel calls (the wasm build)
  main.c                         command line, main loop
  CFLAGS                         extra flags for the UC.OS build (-mtail-call, -ffp-contract=off, -fno-math-errno)
  tests/                         native tests and guest test programs (not built into UC.OS)
```

### 3.1 Decoder (`rv.h`)

```c
int rv_decode(uint32_t raw, struct rv_insn *out);   /* returns length 2 or 4 */
struct rv_insn { uint16_t op; uint8_t rd, rs1, rs2, rs3, rm, len; int64_t imm; };
```

- Pure: no memory, no state. Pass the 32 bits at pc; if the low two bits are not `11` only
  the low 16 bits are used (at the end of a mapping a caller may pass 0 as the high half
  after checking `rv_insn_len()`).
- Compressed instructions come out **expanded**: `op` is always a base opcode and `len`
  says 2. A consumer handles base opcodes only.
- `op`: `enum rv_op`, generated from the `RV_OPS` X-macro, with names (`rv_op_name[]`) and
  flags (`rv_op_flags[]`: jump, branch, load, store, system, csr, atomic, fp, bitmanip) and
  a format letter (used by the disassembler and the decoder test).
- `imm` is sign-extended as the ISA says. LUI/AUIPC: the shifted 32-bit value. Shifts: the
  amount. CSR ops: the CSR number; the `*I` forms carry their 5-bit immediate in `rs1`.
  Atomics: `rm` holds aq (bit 1) and rl (bit 0). FP: `rm` is the rounding mode field (7 =
  dynamic).
- `RV_ILLEGAL` for anything outside user-mode RV64GC (or an enabled extension).
- `rv_decode_ext` enables Zba, Zbb, Zbs (off by default: Debian's baseline is RV64GC).
- Tested against LLVM's disassembler and assembler (section 11): every compressed encoding
  and 30,000 random 32-bit words, 0 mismatches.

### 3.2 CPU state (`struct rv_cpu` in `rv.h`)

Fixed layout, so generated code can address it directly. The interpreter's instance is the
global `struct rv_cpu rv_cpu`.

| Offset | Field | Notes |
|---|---|---|
| 0 | `uint64_t x[33]` | `x[0]` is always 0; `x[32]` is the sink for writes to x0 |
| 264 | `uint64_t pc` | valid when the interpreter is stopped |
| 272 | `uint64_t f[32]` | raw bits; single precision values are NaN-boxed |
| 528 | `uint32_t fcsr` | frm in bits 7:5, fflags in 4:0 (same bit order as `FF_*` in `softfp.h`) |
| 536 | `uint64_t res_addr` | LR/SC reservation, `~0` when none (cleared at every stop) |
| 544 | `uint64_t res_val` | unused today |
| 552 | `uint64_t instret` | retired instructions (charged per block) |

### 3.3 Guest memory (`lx.h`, `mem.c`)

- The guest sees `[0, g_size)`; guest address `a` is host address `g_mem + a`.
- A valid access satisfies `a - g_lo <= g_span` (unsigned): `g_lo` is the 64 KiB null
  guard, the top page is never mapped, so one compare covers accesses of up to 8 bytes.
- In WebAssembly `g_mem` is a 32-bit pointer into the engine's linear memory; after the
  check, `g_mem + (uint32_t)a` is a plain wasm load or store. A translator can keep
  `g_mem`, `g_lo`, `g_span` in locals.
- Self-modifying code: the RISC-V rule. The guest runs `fence.i` or calls
  `riscv_flush_icache` before executing code it wrote; munmap/mprotect of executable
  memory, mmap with PROT_EXEC and execve also flush. `arch_cur->flush_code(addr, len)` is
  the single entry point (today it drops all cached code).
- Mapping API: `mem_map`, `mem_unmap`, `mem_protect`, `mem_remap`, `mem_brk`,
  `mem_is_mapped`, `mem_sync` (Linux semantics, `-errno` on failure).

### 3.4 System call entry

The interpreter returns `LX_STOP_SYSCALL` with `pc` at the `ecall`; the main loop calls
`lx_syscall()`, which reads the number and arguments through `arch_cur->sys_nr/sys_args`,
writes the result with `sys_ret` and steps past the instruction with `sys_skip`
(`rt_sigreturn`, a successful `execve`, an emulated fork child finishing, and a signal
delivered by `rt_sigsuspend` replace the pc instead). A translator does the same: store
the registers it holds in locals back into `rv_cpu`, set `pc`, call `lx_syscall()`, call
`lx_signals()` if `lx_sig_pending || hb_pending_signal`, reload, continue at `rv_cpu.pc`.

```c
for (;;) {                                     /* main.c */
  int stop = arch_cur->run(&fault);
  if (stop == LX_STOP_SYSCALL) lx_syscall();
  else if (stop == LX_STOP_BUDGET) hb_tick();  /* lets the UC.OS kernel preempt and kill */
  else lx_trap(stop, &fault);                  /* SIGSEGV, SIGILL, SIGBUS, SIGTRAP */
  if (lx_sig_pending || hb_pending_signal) lx_signals();
}
```

### 3.5 Floating point (`softfp.h`, `rv_fp.h`)

`sf64_*` / `sf32_*`: exact IEEE 754 with RISC-V semantics (5 rounding modes, the 5 flags,
canonical NaN, tininess after rounding, saturating conversions, RISC-V min/max, NV for
inf*0+qNaN). `fpd_*` / `fps_*` (add, sub, mul, div, sqrt, fma): the same results from the
host's floating point when the rounding mode is RNE and operands and result are far from
the edges of the range. The inexact flag is decided exactly (TwoSum / Dekker products), or
is free when already set (flags are sticky); fma uses the round-to-odd algorithm of Boldo
and Melquiond and is taken only when NX is already set. Everything else goes to the
reference. Tested against the x86 FPU and against each other (section 11).

### 3.6 The architecture interface (`arch.h`)

Everything architecture-specific the engine needs: ELF machine, AT_HWCAP, AT_PLATFORM,
uname machine, register reset, the run function, the system call convention, pc/sp/tp
access, signal frame setup and return, code flush, architecture-specific calls
(`riscv_hwprobe`), whole-state save/load (fork emulation). A second front end is a new
`struct lx_arch` plus its decoder and interpreter; loader, memory, paths and the system
call layer are shared. The loader applies no relocations (static PIE relocates itself;
ld.so relocates everything else), so it needs nothing per architecture beyond
`e_machine`. An amd64 front end would also need: its own system call number table mapped
to the generic one, `struct stat` and other layouts written in x86 order, `arch_prctl`.

## 4. What runs

| Milestone | Test | Native (host Linux) | UC.OS (Node, `tools/run.mjs`) |
|---|---|---|---|
| M1 static hello (own assembly) | `tests/guest/hello.S` | runs | runs |
| M2 busybox-static script: sh, ls, cat, grep, sort, awk, sed, sha256sum, md5sum, gzip, tar, find, xargs, dc, ... | `tests/guest/m2.sh` (37 checks) | 37/37, with the host's fork and with the emulated fork | 37/37 (emulated fork), 2.4 s |
| M2 busybox `vi` in a terminal | `tests/os/vi.mjs` (3 checks: text typed in insert mode shows, `~` markers drawn, `ggdd:wq` saves the edited file) | not tried | 3/3 in `term --fb` |
| M3 dynamically linked programs with Debian's glibc and ld.so | `tests/guest/m34.sh` (10 checks: dash, coreutils echo/ls/cat/sort, bash, exit status, pipes) | 10/10 (both fork modes) | 10/10, under root `/debian` |
| M4 in Chromium | `tests/os/browser.mjs` (bash in `term --fb`, then the M3/M4 script) | | bash prompt and commands work; m34 9/10 in one run: the check `printf ... \| sort \| busybox tr` gave empty output in Chromium while it passes under Node; not explained yet |
| M4 dash and bash, coreutils | same; and `tests/os/interactive.mjs` (6 checks, typed into UC.OS's terminal emulator: prompt, arithmetic, `ls`, a loop into a pipe, dash inside bash, exit back) | non-interactive passes; interactive not tried | non-interactive passes; interactive bash in `term --fb`: 6/6 |
| M5 `dpkg --version` | `run.sh` | runs | runs |
| M5 `dpkg-deb -c` on a package | `run.sh` (Debian's `bc` .deb) | runs (needs threads: liblzma always starts a worker; green threads, section 8) | not tried |
| M5 `apt-get --version` | `run.sh` | runs | not tried (libstdc++ and libapt-pkg add about 5 MB to the image) |
| M5 `perl -e` | `run.sh` | runs | runs (`perl -e print(6*7)` prints 42) |
| M5 `python3.13 -c 'print(1)'` | `run.sh` (needs libpython3.13-stdlib) | runs | not tried (the standard library is too big for the RAM image) |
| M6 minimal Debian root running maintainer scripts | `tests/m6.sh`: in a root unpacked by `fetch-debian.sh` plus an empty dpkg database, the root's own dpkg (`LX_UID=0`) installs libgcc-s1, libc6, libdebconfclient0, base-passwd, base-files, debianutils, bc; their preinst/postinst run under the root's `/bin/sh` (dash), `update-passwd` writes `/etc/passwd`, `update-shells` writes `/etc/shells` | 7 of 7 packages in state `ii`, installed bc runs; 0 failed. The root was unpacked on the host first (not by debootstrap), and dependencies were forced in a fixed order (no apt) | not tried (needs a larger image or persistence) |
| Ubuntu 26.04 riscv64 binaries | | stop at the first vector instruction (RVA23, section 2.2) | |

The UC.OS test image is made by `tests/mkimage-debian.sh` (a small Debian root at
`/debian`: dash, bash, coreutils subset, busybox, dpkg, perl and their libraries; links
copied as files because UC.OS has no symbolic links yet): 17 MB with the desktop's 5.5 MB
of fonts, so it is a test image, not a default one.

## 5. Speed

Measured 2026-11-26 on the shared laptop (i5-8250U), `bin/lx/tests/guest/bench.c`
built for riscv64 (`-O2 -march=rv64gc`, run by the engine) and natively (`cc -O2`, the
reference). Best of 3 runs per kernel inside each process; native engine variants run
alternately by `tests/bench.sh`; UC.OS runs under Node 26 with `tools/run.mjs`, two
alternating rounds. **The machine load was 2.6 to 4.7 during these runs (other engineers'
work), and the same build varied by up to 2x between rounds in Node, so treat
differences under about 30% as noise.**

Chromium 158 (headless, `tools/browser.mjs`, the engine started as init so no shell or
fork is involved, two runs each, load about 4 to 5): fib 68.7 ms, sieve 528 ms, sort 869
ms, sha256 12.3 s, checksums right. That is 2.5 to 3.5 times slower than the same build
under Node on the same machine. Not explained yet; candidates to check on a quiet
machine: V8 tiering differences in the browser, the 1 GiB memory window (whether
Chromium falls back to explicit bounds checks for this instance), headless throttling.

Guest instructions per kernel (engine's `--stats`, set-up subtracted): sieve 43.2 M, sort
62.5 M, sha256 (16 MiB) 1,307 M, float (Mandelbrot with fused multiply-add) 51.2 M, fib(27)
6.9 M.

| Kernel | native x86 code | engine natively, tail-call dispatch | engine natively, switch | engine in UC.OS (Node), switch | same, tail-call | Node switch / native code |
|---|---|---|---|---|---|---|
| sieve | 10.3 ms | 111 ms (389 M instr/s) | 134 ms (323) | 170 ms (254) | 342 ms | 16.6x |
| sort | 33.3 ms | 169 ms (370) | 183 ms (341) | 236 ms (265) | 607 ms | 7.1x |
| sha256 16 MiB | 111 ms | 2,505 ms (522) | 2,831 ms (462) | 3,458 to 3,693 ms (354 to 378) | 11,001 ms | 31 to 33x |
| float | 20.3 ms | 453 ms (113) | 425 ms (120) | 1,135 ms (45) | 1,157 ms | 56x |
| fib | 0.79 ms | 17.1 ms (402) | 19.4 ms (353) | 24.8 ms (277) | 60.7 ms | 31x |

What the numbers say:
- **Interpreter speed in UC.OS: roughly 250 to 380 million guest instructions per second
  for integer code, 45 million for floating point heavy code; 7 to 33 times slower than
  the same C compiled natively (56 times for the FP kernel).** This is the C side of the
  owners' C-versus-D comparison.
- **Dispatch:** under V8 the tail-call threaded dispatch (`return_call_indirect`) is 1.4
  to 3 times slower than the switch (`br_table`); natively it is about 10% faster. The
  wasm build therefore defaults to the switch (`--dispatch=` overrides).
- **Superinstructions** (`--no-fuse` off): the three fusions in the tree (li, mv,
  auipc+addi) show no gain above the noise natively; a pair profile (`-DLX_PROFILE`) is
  in place to choose better ones. Not pursued further while the machine is loaded.
- **Floating point:** the host fast paths make the float kernel 1.6 times faster in Node
  than soft-float alone (1,135 ms against 1,811 ms).
- **Budget and yield:** the engine calls `getppid` every 4,000 guest instructions (about
  11 to 16 microseconds of guest work at these speeds) so the kernel can preempt it
  (every 256th call reads the clock, so a time slice is noticed within about 4 ms) and kill
  it. At PERFORMANCE.md's 11 to 15 ns per `getppid` that is under 0.15% of run time
  (computed, not measured separately).
- Cold start: the first run of each kernel in Node was 1 to 1.5 times the best.

How to repeat: `sh bin/lx/tests/bench.sh OUTDIR 3` natively (OUTDIR with `bench-native`
and `bench-rv`); in UC.OS add `bench-rv` to the image (`tests/mkimage.sh`) and run
`node tools/run.mjs --build=B --init=/bin/lx --args="[--dispatch=tail] /lx/bench KERNEL 3"`.

## 6. Memory

- Layout (low to high): 64 KiB never mapped (null guard); the program at its link address
  (PIE at 0x100000) and the dynamic linker 16 MiB above it; brk after that; mmap
  allocations top down from below the stack; an 8 MiB stack ending one page below the top
  of the window; the top page never mapped.
- The window is allocated once at start (`--mem`, default 1 GiB). In WebAssembly it is one
  `memory.grow` at the end of the engine's memory; the browser commits pages only when they
  are touched. Natively it is one `mmap` with MAP_NORESERVE.
- Released pages: natively `madvise(MADV_DONTNEED)` gives them back; WebAssembly memory
  never shrinks, so a released page is cleared when it is handed out again.
- File mappings are read in. `MAP_SHARED` writable file mappings are written back at
  `msync`, `munmap` and exit (never past the end of the file). Two processes do not share
  a mapping (no shared memory in UC.OS).
- `MADV_DONTNEED` clears the range, as Linux does for private anonymous memory.
- Protection is recorded but not enforced on each access; an access to an unmapped page
  inside the window is not caught. Cost of the checks, measured 2026-11-26 with three
  builds (`-DLX_MEMCHECK=0` no check, `1` the window compare (default), `2` the window
  compare plus the page's protection from the page table): natively, no difference
  between 0 and 1 above the noise (sieve 108 against 111 ms, sha256 2,823 against 2,505
  ms, the default being faster in some runs); in Node, two alternating rounds at load 3.2
  to 4.4, best of each: sieve 148 / 170 / 212 ms, sort 298 / 236 / 306 ms, sha256 4,385 /
  3,693 / 3,458 ms, fib 29.5 / 24.8 / 33.7 ms for variants 0 / 1 / 2. The spread between
  rounds of the same build (up to 2x) is larger than any difference between variants, so
  the cost of the window check and of enforcing protections could not be resolved on this
  machine today; the enforcing variant ran bash and busybox correctly. To repeat when
  the machine is quiet: `tests/bench.sh` builds variants 0 and 2 natively.

## 7. System calls

### 7.1 Table from real programs

Counts from running each program natively with `LX_STATS=1` (`tests/syscall_table.sh`),
summed over all engine processes it started.

Programs: busybox-m2 = `busybox sh m2.sh` (37 checks, 86 processes, most of them
emulated or real forks); dash and bash = a short script that runs `ls`; ls = `ls -la
/usr/bin`; dpkg = `dpkg --version`; apt-get = `apt-get --version`; perl = `perl -e`;
python3 = `python3.13 -c 'print(1)'`. Measured 2026-11-26 with the host's fork.

| call (nr) | busybox-m2 | dash | bash | ls | dpkg | apt-get | perl | python3 |
|---|---|---|---|---|---|---|---|---|
| brk (214) | 690 | 9 | 9 | 3 | 3 | 4 | 4 | 9 |
| chdir (49) | 92 |  |  |  |  |  |  |  |
| clock_gettime (113) | 2 |  | 6 | 1 |  |  | 1 | 98 |
| clone (220) | 1998 | 2 | 2 |  |  |  |  |  |
| close (57) | 4098 | 15 | 13 | 12 | 7 | 21 | 6 | 14 |
| connect (203) |  |  |  | 4 |  |  |  |  |
| dup3 (24) | 588 | 3 | 1 |  |  |  |  |  |
| execve (221) | 52 | 1 | 1 |  |  |  |  |  |
| exit_group (94) | 86 | 2 | 2 | 1 | 1 | 1 | 1 | 1 |
| faccessat (48) | 1 | 4 | 8 | 2 | 2 | 1 | 1 | 1 |
| fcntl (25) | 399 | 4 |  |  |  |  | 5 | 4 |
| fstat (80) | 145 | 9 | 13 | 7 | 8 | 18 | 4 | 22 |
| futex (98) |  |  |  |  |  | 1 |  |  |
| getcwd (17) | 86 | 2 | 2 |  |  |  |  | 1 |
| getdents64 (61) | 4 | 2 | 2 | 2 | 2 | 2 |  | 8 |
| getegid (177) |  | 2 | 6 |  |  |  | 3 | 1 |
| geteuid (175) |  | 4 | 6 |  |  |  | 3 | 1 |
| getgid (176) | 138 | 2 | 6 |  |  |  | 3 | 1 |
| getpeername (205) |  |  | 2 |  |  |  |  |  |
| getpgid (155) |  |  | 2 |  |  |  |  |  |
| getpid (172) | 86 | 2 | 7 |  |  |  |  |  |
| getppid (173) | 86 | 2 | 6 |  |  |  |  |  |
| getrandom (278) | 138 | 3 | 3 | 1 | 1 | 1 | 1 | 3 |
| gettid (178) |  |  |  |  |  |  |  | 1 |
| getuid (174) | 141 | 2 | 6 |  | 1 |  | 3 | 1 |
| ioctl (29) | 5 | 2 | 11 | 2 | 2 | 1 | 4 | 11 |
| llistxattr (12) |  |  |  | 168 |  |  |  |  |
| lseek (62) | 12 |  |  |  |  |  | 4 | 15 |
| mmap (222) | 2 | 20 | 26 | 12 | 12 | 43 | 10 | 16 |
| mprotect (226) | 138 | 12 | 14 | 6 | 6 | 18 | 5 | 6 |
| munmap (215) |  |  |  |  |  |  |  | 1 |
| newfstatat (79) | 245 | 12 | 20 | 3 |  | 10 | 10 | 50 |
| openat (56) | 334 | 13 | 16 | 11 | 9 | 22 | 7 | 31 |
| pipe2 (59) | 1729 |  |  |  |  |  |  |  |
| prctl (167) | 138 | 6 | 6 | 6 |  | 6 |  |  |
| prlimit64 (261) | 140 | 3 | 5 | 1 | 1 | 1 | 1 | 1 |
| read (63) | 3403 | 8 | 10 | 6 | 8 | 23 | 5 | 15 |
| readlinkat (78) | 138 |  |  | 3 |  |  | 1 | 1 |
| riscv_hwprobe (258) | 138 | 8 | 10 | 4 | 4 | 14 | 3 | 2 |
| rseq (293) | 138 | 3 | 3 | 1 | 1 | 1 | 1 | 1 |
| rt_sigaction (134) | 601 | 14 | 36 |  |  |  | 67 | 66 |
| rt_sigprocmask (135) | 3996 | 4 | 24 |  |  |  | 4 | 2 |
| rt_sigreturn (139) | 1829 | 1 | 1 |  |  |  |  |  |
| sendfile (71) | 5 |  |  |  |  |  |  |  |
| set_robust_list (99) | 269 | 3 | 4 | 1 | 1 | 1 | 1 | 1 |
| set_tid_address (96) | 138 | 3 | 3 | 1 | 1 | 1 | 1 | 1 |
| setgid (144) | 138 |  |  |  |  |  |  |  |
| setuid (146) | 138 |  |  |  |  |  |  |  |
| socket (198) |  |  |  | 4 |  |  |  |  |
| statfs (43) |  | 2 | 2 | 2 | 2 |  |  |  |
| statx (291) |  | 1 | 1 | 168 |  |  |  |  |
| umask (166) |  |  |  |  | 1 |  |  |  |
| uname (160) | 86 |  | 2 |  |  |  |  |  |
| unlinkat (35) | 1 |  |  |  |  |  |  |  |
| wait4 (260) | 3634 | 2 | 2 |  |  |  |  |  |
| write (64) | 193 | 7 | 3 | 3 | 2 | 17 | 1 | 1 |

What it shows: a few dozen calls cover these programs. Every one of them is answered
today (natively; on UC.OS the same calls go to the kernel or are emulated, section 7.2).
The busybox column is dominated by fork, pipes, wait4 and signal masks (the shell); the
others make mostly memory, file and identity calls. `rseq` gets -ENOSYS and glibc carries
on. None of these programs used threads, epoll, eventfd or sockets beyond `getpeername`
(bash checks whether stdin is a socket).

### 7.2 What the engine does with each call

- Passed to the host with pointer translation only: read, write, pread64, pwrite64,
  openat, close, lseek, getdents64, dup, dup3, pipe2, ioctl (terminal requests), fcntl,
  mkdirat, unlinkat, renameat2 (flags 0), ftruncate, chdir, getcwd, uname (then rewritten),
  getrandom, wait4, kill (other processes), getpid, getppid, setpgid, getpgid, setsid,
  socket, bind, listen, accept, connect, sched_yield.
- Converted: newfstatat/fstat/statx (from a neutral stat), clock_gettime, nanosleep,
  clock_nanosleep, ppoll, pselect6 (on ppoll), sysinfo, utimensat, readv/writev/preadv/
  pwritev (loops), gettimeofday, times, getrusage, prlimit64.
- Emulated in the engine: brk, mmap, munmap, mprotect, mremap, madvise, msync, mincore,
  mlock*; rt_sigaction, rt_sigprocmask, rt_sigreturn, sigaltstack, rt_sigpending,
  rt_sigsuspend, kill/tgkill/tkill to itself; execve, clone/clone3 (section 8); umask;
  set_tid_address, set_robust_list, prctl (name, a few flags), sched_getaffinity (one CPU),
  getcpu, riscv_hwprobe (answers what the interpreter implements: IMA, FD, C, ZCA, ZCD;
  misaligned access fast), riscv_flush_icache, memfd_create (an unlinked file in /tmp),
  sendfile and copy_file_range (copy loops), statfs (a fixed RAM file system), flock and
  fcntl record locks (always granted: one process), futex (queues between green threads,
  section 8), /proc/self/exe and /proc/self/fd/N.
- Identity: getuid family returns `lx_uid()`; set*id succeed only as root.
- Returns -ENOSYS: rseq (glibc copes), epoll_*, eventfd2, signalfd4, inotify_init1,
  socketpair, sendmsg/recvmsg and the other socket calls, setitimer with a timer.

### 7.3 What the engine needs from the kernel, in priority order

1. A clone (fork) primitive (section 8). Without it, a guest parent and child cannot run at
   the same time.
2. Signals delivered to a program instead of termination, at least the way to receive
   SIGINT, SIGCHLD, SIGWINCH, SIGPIPE and SIGTERM (the engine then raises them in the
   guest). Today Ctrl-C kills the engine with the guest.
3. Symbolic links: `readlinkat`, `symlinkat`, and `newfstatat` honouring
   AT_SYMLINK_NOFOLLOW (Debian's root is full of symlinks: `/bin -> usr/bin`, the dynamic
   linker, alternatives). In flight in the vfs stream.
4. Hard links (`linkat`), modes and owners (`fchmod*`, `fchown*`), `faccessat`: dpkg uses
   them. In flight in the vfs stream.
5. `F_DUPFD_CLOEXEC` (the engine emulates it with two calls), `getsid`, `utimensat` with
   explicit times (dpkg and tar restore times).
6. File offsets and sizes above 2 GiB, eventually (Linux programs use 64-bit offsets).
7. `socketpair`, then `sendmsg`/`recvmsg` (apt uses them between its methods).

From the libc: nothing (the engine calls the kernel with `__syscall6`).

## 8. Processes: fork, exec, and starting guest programs

- **execve** is done inside the engine: it drops the guest image, loads the new one, keeps
  the descriptors, closes close-on-exec ones, resets handled signals to default. `#!`
  scripts are followed (up to 5 levels). A native program (a UC.OS wasm file, or natively a
  host binary) is started instead: natively by the host's execve; on UC.OS by spawn, then
  the engine waits and exits with its status.
- **fork / vfork / clone** without CLONE_VM: natively the engine forks with the host.
  On UC.OS (no fork yet) the engine emulates fork + exec and fork + exit: at clone it saves
  the registers, every writable guest page that is not all zeros, the memory map, the
  signal state, the working directory and the descriptor table (each open descriptor is
  duplicated, close-on-exec, to a number at 40 and above); it then runs the guest as the
  child; when the child calls execve the target starts as a new process (another engine
  with `--root` and `--argv0`, or a UC.OS program) with the descriptors the child set up;
  when it exits its status is kept under a made-up pid (100000 and up) for wait4; then the
  parent is restored exactly and clone returns the pid. Limits: the child runs before the
  parent continues, so a child that waits for its parent (a pipe the parent must fill)
  hangs; the descriptor save needs free slots (UC.OS has 64 per process). Two
  adjustments make the common shells work: in an emulated child a read on an empty pipe
  returns end-of-file instead of waiting for the stopped parent (bash's child waits on a
  pipe until its parent closes it, PGRP_PIPE; that end-of-file is what it waits for), and
  the child's setpgid/setsid/TIOCSPGRP on itself are not applied (they would move the
  parent shell; the parent does the same for the real child after exec).
  `LX_FORK=emulate` selects the emulation natively, for testing.
- **Threads** (clone or clone3 with CLONE_VM, without CLONE_VFORK): green threads inside
  one engine (`sys_thread.c`). A thread is a saved register set; one runs at a time; the
  engine switches at a futex wait, at a thread's exit (CLONE_CHILD_CLEARTID word cleared
  and woken), and when the instruction budget runs out (round robin, so spin loops make
  progress). futex WAIT, WAKE, the BITSET forms (with absolute timeouts), REQUEUE,
  CMP_REQUEUE and WAKE_OP; not the priority-inheritance locks. Up to 64 threads; thread
  ids 200000 and up. With one host thread every access is sequentially consistent, so the
  weak memory model never shows. Limits: a host call that blocks (read on an empty pipe)
  stops all guest threads; the signal mask is per process. Needed in practice: dpkg-deb
  (liblzma's decoder always starts a worker thread, so every .deb needs it), python's
  `threading` (tested: four threads joined), and apt's download methods (not tried).

### 8.1 The contract needed from the kernel's clone primitive

1. A system call (number of the owners' choosing; Linux `clone` 220 with flags SIGCHLD
   is fine) creating a new process whose wasm memory is a byte copy of the caller's whole
   linear memory at the moment of the call, with the same module, the same mutable
   globals (in particular `__stack_pointer`), a copy of the descriptor table (shared open
   files and offsets, close-on-exec flags), the same cwd, pgid and sid.
2. The wasm call stack cannot be copied, so the child must not resume mid-call: the child
   instance starts by calling a program export (proposal: `__fork_child`), memory and
   globals already copied. The engine forks only from its system call handler, with all
   guest state in linear memory; `__fork_child` returns to its main loop with the guest's
   result 0. (Proposal: the engine writes a flag in its memory before the call, so code in
   `__fork_child` knows where to continue.)
3. The parent gets the child's pid or -errno; wait4 works as for any process; a child exit
   should raise SIGCHLD once signals exist.
4. Copy cost: the engine's memory is the engine (about 1 MB) plus its guest window (1 GiB
   reserved by default, mostly never touched); copying only what was touched, or only up
   to the highest page in use, would matter. If the kernel can only copy up to
   `memory.size`, a smaller `--mem` keeps it cheap.

Status: the fork stream has built this primitive (`SYS_pclone`, private call 520; not in
this tree yet; 504 to 506 went to networking, and the ranges of private call numbers are
an open question for the owners). Its contract matches points 1 to 3. The engine's side is written behind
`-DLX_HAVE_PCLONE`: `hb_fork()` calls it; the clone request is kept in memory; the export
`__fork_child` applies the child's half of clone (stack, thread pointer, child tid
words), makes the guest's clone return 0 and enters the main loop. Not run yet. Open on
point 4: the primitive copies the whole memory size (zero pages skipped, but read), so a
1 GiB guest window means about 1 GiB scanned per fork; a variant taking the live ranges
(as the fork stream's `SYS_pmigrate` does) would fit the engine, which knows every mapped
page. Until the trees merge the emulated fork stays.

### 8.2 Starting a guest program from the UC.OS shell (ELF dispatch)

Today: explicitly, `lx PROGRAM ARGS` (guest path, resolved inside the root). Hook needed
for automatic dispatch: the kernel's spawn refuses a file that does not start with the wasm
magic (`-ENOEXEC`). Proposal, the smallest change: on `-ENOEXEC`, if the file starts with
`\177ELF` and its machine is one the engine runs, spawn `/bin/lx` with the original path
prepended to argv (like Linux's binfmt_misc). This can live in the kernel's spawn (one
check), or in the libc's `spawn()`/`spawnp()` and the shell (no kernel change, but then
only programs using the libc get it). The reverse direction (a guest running a UC.OS
program) already works: the engine sees the wasm magic and spawns it.

## 9. Signals

The engine owns handlers, mask, alternate stack and pending set, and delivers between
guest instructions (at every stop: system calls, faults, and every instruction budget) by
building the riscv64 `rt_sigframe` (siginfo, ucontext with the registers, as in Linux's
uapi headers). The return path is a two-instruction trampoline the engine maps (`li a7,
139; ecall`), because there is no vDSO. Sources today: the guest itself (raise, abort,
kill of its own pid or group, faults: SIGSEGV, SIGBUS for misaligned atomics, SIGILL,
SIGTRAP, and SIGPIPE on EPIPE) and, natively, the host's signals (forwarded). SA_RESTART
works by re-running the system call after the handler. An unhandled fatal signal ends the
engine as killed by that signal (UC.OS: `kill(self, sig)`). Entry point for asynchronous
signals once the kernel has them: set a bit in `lx_sig_pending`.

## 10. Limits

- Guest threads are green threads in one engine (section 8): one runs at a time; a
  blocking host call stops them all. No shared memory between guest processes.
- Fork on UC.OS: emulated (section 8).
- Protection not enforced; unmapped pages inside the window readable.
- Debian riscv64 only (RV64GC); Ubuntu 26.04 riscv64 needs V and more.
- File times on UC.OS: only "now" can be set.
- No vDSO: glibc falls back to system calls for the clock (fine).

## 11. How to test

```sh
sh bin/lx/tests/run.sh                         # from a fresh clone: unit tests and own guest programs
LX_SCRATCH=/dir sh bin/lx/tests/fetch-debian.sh   # download and unpack Debian riscv64 packages (outside the repo)
LX_SCRATCH=/dir sh bin/lx/tests/run.sh         # also the Debian milestones
make B=build-lx PROGS="sh ls cat echo true lx"
sh bin/lx/tests/mkimage.sh build-lx /dir/root/usr/bin/busybox:/lx/bin/busybox bin/lx/tests/guest/m2.sh:/lx/m2.sh
node tools/run.mjs --build=build-lx --init=/bin/lx --args="/lx/bin/busybox sh /lx/m2.sh /lx/bin/busybox /tmp"
```

| Test | What | Result |
|---|---|---|
| `fp_test.c` | soft-float against the x86 FPU in 4 rounding modes with flags; RISC-V specific vectors; every fast path against soft-float bit for bit | 11.5 million checks at 300,000 iterations, 0 failed |
| `decode_test.py` | decoder against LLVM's disassembler and assembler | 59,560 encodings, 0 mismatches, 0 validity disagreements |
| `path_test.c` | path translation against a fake file system | 29/29 |
| `guest/ops.c` | the same C program natively and for riscv64 at -O0, -O1, -O2, rv64g, -Os: hashes over integer, memory, double, float, control, atomic operations | identical |
| `guest/insn.c` | fcsr, static and dynamic rounding, flags, NaN boxing, LR/SC, AMO, fence.i on rewritten code, mulh, division edge cases | 44/44 |
| `guest/sig.c` | raise, masks, pending, SIGSEGV and SIGILL handlers that fix the context, sigaltstack, SA_RESETHAND, deferral | 13/13 |
| `guest/m2.sh` | M2 | 37/37 |

## 12. Open decisions for the owners

| Topic | Options |
|---|---|
| Guest architecture | riscv64 (now: smallest, Debian official, numbers match; Ubuntu needs V); arm64 (fixed length, NZCV, NEON fits wasm SIMD; most popular after amd64; numbers match); amd64 (where third-party software is; variable length, lazy flags, own call table); several (the interface allows it) |
| Engine name | anything; `lx` is a placeholder |
| Debian files: own root or merged into `/` | own root (now: `/debian` placeholder, path translation per call); merged (no translation, Debian files mix with UC.OS's) |
| Guest uid | the host's (now); 0 (fake root, like fakeroot, needed for dpkg); configurable per root |
| uname contents | as now (Linux 6.12.0-lx riscv64); something else |
| Memory protection | not enforced (now); enforced per access (cost in section 6) |
| Threads | green threads in the engine (now, assumed: needed by dpkg-deb); one Worker per thread later (true parallelism, needs shared memory) |

## 13. Sources

- RISC-V Unprivileged ISA manual (RV64I, M, A, F, D, C, Zicsr, Zifencei, Zba/Zbb/Zbs).
- RISC-V ELF psABI; System V gABI.
- Linux: `scripts/syscall.tbl`, `include/uapi/asm-generic/unistd.h`, `arch/riscv/include/uapi/asm/{hwprobe,sigcontext,ucontext}.h`, `arch/riscv/kernel/signal.c` (frame layout), `kernel/fork.c` and `arch/riscv/Kconfig` (riscv selects CLONE_BACKWARDS).
- Debian and Ubuntu `Release` and `Packages` indices (deb.debian.org, archive.ubuntu.com, ports.ubuntu.com), fetched 2026-11-26.
- IEEE 754-2008. S. Boldo, G. Melquiond, "Emulation of FMA and correctly rounded sums: proved algorithms using rounding to odd", IEEE Trans. Computers, 2008.
- Memory64: [Is Memory64 actually worth using?](https://spidermonkey.dev/blog/2025/01/15/is-memory64-actually-worth-using.html) (SpiderMonkey blog, 2025: "10% to over 100%", Firefox 134, Chrome 133); [Mozilla bug 1761439](https://bugzilla.mozilla.org/show_bug.cgi?id=1761439) (about 17% on one benchmark); [v8-dev thread](https://mail-archive.com/v8-dev@googlegroups.com/msg162075.html) on 32-bit bounds checks in V8.
