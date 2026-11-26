# Binary translation: Linux machine code to WebAssembly

Living design document of the translator stream (branch `feat/binary-translator`). It
answers one question for the owners, "option D if this would run better than option C":
how much faster guest machine code runs when it is translated to WebAssembly than when it
is interpreted, and what the translation costs. It lays out facts and options and makes
no choice for the owners. Everything here was measured on 2026-11-26 on the project laptop
(Intel i5-8250U, 4 cores, 8 threads) while other people's builds were running; the load
average is given with every table. Nothing here is wired into the default boot path.

Contents: 1 the answer in short, 2 the mechanism (how a running program gets new code),
3 benchmarks, 4 how the translator works, 5 run time, install time or hybrid, 6 the host
interface and its security, 7 what the core engine must provide, 8 amd64 and arm64,
9 limits, deviations and what is not verified, 10 files and how to reproduce.

## 1. The answer in short

Measured on a quiet machine (load average 2.2 to 3.0, nothing else of this stream
running), 2026-11-26 08:20 to 10:00. Eight test programs (section 3.1), RISC-V RV64IM.

**D against C inside UC.OS** (Node 26, one boot per run, the programs started as init):
C is the core engine's interpreter (`lx`, branch `feat/linux-binaries`, final version,
built with its own Makefile, switch dispatch, block cache); D is the translator
(`bin/rvjit`, structured regions, guest calls as wasm calls, chaining, learned indirect
targets, masked bounds). Million guest instructions per second (MIPS) = instructions
counted by an interpreter / time.

| program | guest insns | C: lx | D: first run | D: warm | D/C first run | D/C warm | whole command, lx / rvjit wall |
|---|---|---|---|---|---|---|---|
| sieve | 247M | 151 | 870 | 1377 | 5.8x | 9.1x | 4.6x |
| sort | 166M | 182 | 739 | 1121 | 4.1x | 6.2x | 3.0x |
| sha256 | 277M | 304 | 2338 | 4923 | 7.7x | 16.2x | 4.9x |
| fib | 74M | 176 | 811 | 1443 | 4.6x | 8.2x | 2.7x |
| matmul | 89M | 163 | 847 | 2084 | 5.2x | 12.8x | 3.0x |
| strloop | 70M | 128 | 737 | 2412 | 5.8x | 18.8x | 3.2x |
| indirect | 133M | 156 | 333 | 634 | 2.1x | 4.1x | 1.8x |
| switch | 348M | 242 | 1873 | 3073 | 7.7x | 12.7x | 5.3x |
| hello (start-up only) | 4K | 27.7 to 35.9 ms | | | | | 67 to 69 ms (64 MiB guest memory), 30 to 32 ms (4 MiB) |

How to read it:
- C's figure is the best of two runs of the whole process (wall time from the host,
  boot and start included, about 25 ms; the core engine's author reports 250 to 380 MIPS
  for its own kernels, consistent with these). D "first run" is the guest program's first
  run inside a fresh engine, translation and the browser's baseline tier included; "warm" is
  the best of the next two runs in the same engine. "Whole command" compares the wall time
  of one complete run of each engine, start-up included.
- **For computation, D is 4 to 19 times faster than C once warm, 2 to 8 times on a first
  run, 1.8 to 5.3 times for the whole command.** Worst: `indirect` (calls through a jump
  table into small functions: every guest call goes through memory). Best: straight loops
  (`strloop`, `sha256`).
- **For a trivial command D is not faster:** its start-up was 35 ms longer, all of it the
  engine clearing 64 MiB of guest memory; with 4 MiB it starts as fast as C. Nothing was
  translated (the program never reached the threshold), so translation added nothing.
- The C compiled straight to wasm (the ceiling) runs at 1,650 to 10,900 MIPS on the same
  programs (section 3.2): translated code reaches 30 to 100 percent of it on loops, 15 to 45
  percent on call- and switch-heavy code.

Other facts:
- Translation costs per region: 0.1 to 0.3 ms of our code generation plus 0.08 to 0.2 ms
  for the browser to compile and instantiate a module of 2 to 20 KiB. Against C at about
  250 MIPS (4 ns per instruction) and D at about 1 ns, a region pays for itself after
  roughly 50,000 to 100,000 executed instructions.
- Chromium: translated code ran at about the same speed as under Node (within 20 percent);
  the interpreters ran 1.3 to 2.2 times slower there (section 3.3). The Chromium slowdown
  the core engine reports for its interpreter does not show in translated code.
- The browser compiles in two tiers; a loop entered once keeps its first, slower code
  (about 1.8x) until it is entered again. This is most of the difference between "first
  run" and "warm".

## 2. The mechanism: how a running program gets new code

A UC.OS program is one wasm module compiled once. A translator inside a program has to
produce new wasm at run time and run it.

### 2.1 What was built

The engine (here a UC.OS program, `bin/rvjit`, or the same code as a standalone module)
writes the bytes of a module into its own memory and calls one host import,
`jit.load(ptr, len)`. The host compiles the bytes and instantiates them with exactly one
import namespace: **the calling program's own exports** (its memory, its function table,
and the helper functions it exports, such as `jit_ecall`). The generated module puts its
functions into the program's table with its own active element segments, at slots the
engine chose. The engine then calls a region with `call_indirect` through a C function
pointer. Translated code reads and writes guest registers and guest memory directly in the
engine's memory, with constant offsets. A second tiny generated module, made once at
start, exports `grow(n)` (grow the table, filled with a stub) and `stub(slot)` (put the
stub back into a slot, to drop a translation), so the engine manages its table without
JavaScript.

Host side today: `boot/jit-proto/ext.mjs`, plugged in through the `extend` option of
`boot()`; `boot/host.mjs` is unchanged. Because host.mjs has no hook for extra imports of
a program, the prototype starts such programs itself with a copy of host.mjs's `start()`.
The permanent version is about ten lines inside host.mjs (section 6).

### 2.2 Compile and instantiate cost by module size

`tools/jit/mech.mjs` (Node) and `tools/jit/mech.html` (Chromium, through
`tools/jit/chrome.mjs`). Modules of generated loop code; each sample is a batch of fresh
modules with different bytes (V8 caches identical bytes); per module, best / median of 7.
Load average about 2.

| size | functions | `new WebAssembly.Module` | `new Instance` | first call of one function | first call of every function | async compile + instantiate |
|---|---|---|---|---|---|---|
| Node 26: 105 B | 1 | 31 / 33 us | 5 / 5 us | 7 / 10 us | 8 / 10 us | 87 / 100 us |
| 11 KiB | 8 | 76 / 91 us | 10 / 14 us | 22 / 23 us | 163 / 176 us | 135 / 194 us |
| 131 KiB | 10 | 270 / 315 us | 11 / 19 us | 135 / 223 us | 1.3 / 1.5 ms | 0.46 / 0.90 ms |
| 1.3 MiB | 100 | 2.2 / 2.7 ms | 0.14 / 0.16 ms | 0.19 / 0.21 ms | 14.7 / 16.2 ms | 3.3 / 4.0 ms |
| 13 MiB | 800 | 17 / 25 ms | 0.9 / 12 ms | 0.24 / 0.26 ms | 160 / 165 ms | 22 / 25 ms |
| Chromium 158: 105 B | 1 | 52 / 75 us | 5 / 7 us | 8 / 10 us | 8 / 10 us | 126 / 135 us |
| 11 KiB | 8 | 126 / 143 us | 10 / 11 us | 29 / 32 us | 196 / 244 us | 214 / 243 us |
| 131 KiB | 10 | 467 / 507 us | 7 / 13 us | 193 / 200 us | 1.9 / 2.0 ms | 0.57 / 0.66 ms |
| 1.3 MiB | 100 | 2.9 / 3.1 ms | 0.1 / 0.1 ms | 0.2 / 0.2 ms | 19.3 / 19.4 ms | 3.5 / 3.9 ms |
| 13 MiB | 800 | refused | | | | not measured |

What this shows:
- V8 compiles functions lazily: `new Module` only validates (about 1 ms per 600 KiB);
  each function is compiled by the baseline compiler (Liftoff) on its first call, at about
  70 to 90 MB/s of wasm. A big ahead-of-time module costs only for the functions run.
- A small module costs about 45 us under Node and 65 us in Chromium from bytes to first
  result. That is the floor for one region.
- Chromium refuses synchronous compilation above 8 MB on the main thread (the message
  points to `--enable-features=WebAssemblyUnlimitedSyncCompilation`). Regions are far
  below that; a whole-program module might not be (section 5).
- Asynchronous compilation is slower for small modules (it goes through a task) and only
  pays off from about 1 MiB.

### 2.3 Tiering

Same tools, one function with a loop of 20 i64 operations per iteration.

| | Node 26 | Chromium 158 |
|---|---|---|
| called repeatedly, 200,000 iterations per call: ns per iteration, call 1 / best | 6.7 / 3.5 | 6.5 / 3.5 |
| first call within 1.25x of the best | call 2 (after 1.3 ms) | call 2 (after 1.3 ms) |
| one call of 30 million iterations on a fresh module | 6.2 ns (1.8x the best) | 6.4 ns (1.8x) |
| one long call after the function was warmed by other calls | 3.6 ns | 4.0 ns |
| short calls (100 iterations): first batch within 1.25x of the best | after about 1,200 calls (1 ms) | (clock too coarse in this run) |

So: baseline code is about 1.8 times slower than optimised code for this kind of code;
the optimising compiler takes over after about a millisecond of execution, but only for
calls that start after that. A loop entered once never gets faster (no OSR). In the
translator this shows as the first run of a program being slower than later runs (the
"first" column in section 3), and it is why the engine's driver loop is itself split into
functions that are re-entered (`run_some`), and why the budget helper can make a long
translated loop leave and re-enter (option `yield_exit=1`).

## 3. Benchmarks

### 3.1 Method

- Eight small C programs in `tools/jit/guest/` (sieve, quicksort, sha256, recursive
  Fibonacci, integer matrix multiply, byte-wise string loops, an indirect-call program
  that clang turned into a jump table plus direct calls, and a bytecode interpreter with a
  dense switch), plus two floating-point programs (n-body, a float/double mix). One source
  builds as a riscv64 ELF (`-march=rv64im`, and rv64gc for the `-gc` variants), as wasm32
  (case 5, the ceiling) and natively (case 4). Build command in `tools/jit/jit.mk`.
- Guest instructions are counted by the interpreter; "MIPS" for every case is that count
  divided by the time, so cases 4 and 5 are "guest instructions per second equivalent".
- Each translated configuration runs in a fresh engine, 5 runs per engine, 2 engines,
  configurations alternated; best (and median) of the runs after the first; the first run
  of an engine includes translation and the browser's tiering.
- Every translated run must end in the same final state as the interpreter (exit code,
  output, all registers, memory below the break and the stack): it did in every run
  reported here.
- The standalone engine (`tools/jit/sa/`) runs outside UC.OS, to isolate the
  translator. UC.OS figures are in section 3.4.

### 3.2 Node, standalone engine (quiet machine, load 2.2 to 3.0)

Guest MIPS, best (median). (1) my trivial interpreter (decodes every instruction);
(1b) my predecoding interpreter (decodes each block once). (3) translated: `block` one
region per basic block; `dispatch` whole region in a loop around a br_table; `struct`
structured control flow; `+calls` guest calls become wasm calls; `aot` every function of
the file in one module. All (3) columns with chaining and learned targets. (5) the same C
compiled to wasm (best of the three runs of each program, they vary by up to 20 percent).
(4) native x86 -O2 (range of three runs).

| program | insns | (1) | (1b) | block | dispatch | struct | dispatch+calls | struct+calls | aot | (5) wasm | (4) native |
|---|---|---|---|---|---|---|---|---|---|---|---|
| sieve | 247M | 44 | 95 | 982 (895) | 696 (637) | 1199 (1132) | 667 (642) | 1390 (1281) | 2078 (1979) | 3357 | 3285 to 4172 |
| sort | 166M | 24 | 46 | 553 (489) | 836 (721) | 1356 (1269) | 732 (692) | 1344 (1273) | 1331 (1228) | 2111 | 1689 to 2383 |
| sha256 | 277M | 16 | 41 | 4741 (4302) | 2315 (1858) | 3695 (2807) | 2808 (2166) | 4762 (4279) | 4724 (4312) | 8709 | 7098 to 9399 |
| fib | 74M | 34 | 77 | 765 (751) | 1260 (1148) | 1257 (1178) | 1382 (1284) | 1582 (1421) | 1525 (1389) | 9097 | 7329 to 8953 |
| matmul | 89M | 29 | 96 | 2201 (2109) | 929 (840) | 2628 (2203) | 778 (666) | 2353 (1746) | 1934 (1540) | 8660 | 12223 to 18315 |
| strloop | 70M | 31 | 90 | 632 (581) | 1844 (1536) | 2448 (2222) | 1560 (1440) | 2344 (2153) | 2450 (2195) | 4232 | 5680 to 6080 |
| indirect | 133M | 41 | 111 | 236 (217) | 478 (432) | 501 (479) | 560 (454) | 772 (628) | 780 (543) | 2278 | 1382 to 1992 |
| switch | 348M | 26 | 79 | 436 (416) | 2312 (2099) | 4334 (3512) | 1938 (1832) | 4061 (3931) | 4378 (4239) | 10918 | 8252 to 10438 |

Every translated run ended in the interpreter's final state (no MISMATCH in any run).
Results: `build-NAME/jit/results/quiet-node*.txt` (`tools/jit/quiet-bench.sh`).

What it says:
- Structured beats dispatch on loop code (sieve 1199 against 696, sort 1356 against 836,
  switch 4334 against 2312); `block` wins only where the hot loop is one basic block
  (sha256, matmul).
- Guest calls as wasm calls help call-heavy code a little more than expected once
  chaining exists (fib 1257 to 1582; indirect 501 to 772).
- `aot` (one module for the whole file) is close to run-time translation once warm; it
  helps where the first region entries would otherwise wait for the threshold (sieve).
- Learned indirect targets turned `switch` from about 600 MIPS (earlier, without them,
  load 5 to 9) into 4,000 to 4,400: 40 percent of the C compiled to wasm.
- Earlier runs under load (5 to 9) gave the same picture with 10 to 40 percent lower
  figures; they are superseded by this table.

### 3.3 Chromium 158 (quiet machine)

Same engine and pages (`tools/jit/bench.html` through `tools/jit/chrome.mjs`), one round of
5 runs. Guest MIPS, best (median).

| program | (1) | (1b) | struct+calls | aot | (5) wasm |
|---|---|---|---|---|---|
| sieve | 23 | 46 | 1432 (1377) | 2196 (2124) | 3186 |
| sort | 19 | 48 | 1238 (1170) | 1261 (1249) | 2182 |
| sha256 | 21 | 48 | 4505 (4184) | 4602 (4483) | 9312 |
| fib | 19 | 42 | 1312 (1255) | 1296 (1236) | 6790 |
| matmul | 18 | 40 | 2389 (1835) | 2126 (1387) | 7149 |
| strloop | 18 | 39 | 2114 (1659) | 2114 (2088) | 4202 |
| indirect | 18 | 38 | 645 (606) | 615 (440) | 1695 |
| switch | 22 | 47 | 4741 (4573) | 5140 (4894) | 2121 |

Translated code: within about 20 percent of Node. Interpreters: 1.3 to 2.2 times slower
than under Node. The direct-wasm `switch` ran 4.5 times slower in Chromium than under Node
in this run (not investigated; a single round).

### 3.4 Inside UC.OS

Section 1's table. Commands: `make -f tools/jit/jit.mk B=build-NAME ucos` (image with
`rvjit`, `lx` if built, and the test programs in `/usr/share/jit/`), then
`node tools/jit/ucos-run.mjs --build=build-NAME --init=/bin/rvjit --args='repeat=3
/usr/share/jit/fib.elf' --stats`. Translated code inside UC.OS ran at 80 to 100 percent of
the standalone engine's speed (sieve 1377 against 1390 MIPS warm). My own interpreters
inside UC.OS: (1) 24 to 33 MIPS, (1b) 69 to 105.

### 3.5 Bounds checking, budget checks, re-entry, translation cost

Node, standalone engine, struct+calls, two alternating rounds of 5 runs, load 2.6 to 3.0
(one sort run at 5.2). Best warm run in ms (first run in brackets), both rounds.

| program | masked (default) | unchecked | checked | no budget checks | yield_exit=1 |
|---|---|---|---|---|---|
| sieve | 169, 153 (276, 250) | 160, 157 (261, 261) | 151, 169 (304, 357) | 71, 86 (198, 250) | 138, 165 (214, 281) |
| sort | 132, 127 (165, 150) | 111, 123 (137, 155) | 127, 143 (150, 198) | 94, 98 (119, 125) | 128, 130 (155, 166) |
| fib | 48, 48 (60, 64) | 44, 44 (55, 54) | 52, 51 (55, 68) | 42, 49 (58, 61) | 48, 54 (57, 74) |
| strloop | 24, 24 (39, 38) | 26, 26 (41, 60) | 29, 31 (46, 56) | 23, 23 (43, 39) | 24, 24 (38, 43) |

- **Bounds**: masked costs about the same as unchecked here; checked (precise faults)
  adds 0 to 15 percent.
- **Budget checks are expensive on tiny loops**: sieve's inner loop (a store and an add)
  runs about twice as fast without them, sort 25 percent faster, the others within 10
  percent. They exist only because a UC.OS process must reach the kernel regularly
  (CLAUDE.md: a pure loop freezes the tab). Options, for the owners: keep them (correct,
  costs up to 2x on the tightest loops); count less often (for example only in loops that
  contain no inner loop, plus calls; same guarantee as today for every loop nest, not
  built); or run translated programs in a Worker (docs/WORKER-PROCESSES.md), where no
  budget is needed at all.
- **Re-entry on budget** (`yield_exit=1`, leave and re-enter so the optimised code is
  picked up) did not help warm runs and made no clear difference to first runs.
- **Translation cost** (from the runs of section 3.2): our code generation 0.1 to 0.3 ms
  per region, the browser's compile and instantiate 0.08 to 0.2 ms per module (median per
  module), modules of 2 to 20 KiB; 3 to 25 regions per program.

## 4. How the translator works

`libjit/`. Plain C that builds three ways: natively (unit tests), as a standalone wasm32
module (measurements), and inside a UC.OS program (`bin/rvjit/CFLAGS`).

### 4.1 Layers

| File | What |
|---|---|
| `wasm.h`, `wasm.c` | binary emitter: buffers, LEB128, opcodes, sections |
| `rv.h`, `rvdec.c` | RV64GC decoder (I, M, A, F, D, Zicsr, Zifencei, C), CPU state layout |
| `xlat.h`, `xgen.h`, `xlat.c` | architecture-neutral: region discovery, control flow graph, dominators, structuring, register caching, exits, calls, chaining, module assembly |
| `rvsem.c` | RISC-V instruction semantics in wasm (the architecture half) |
| `rvfp.c` | F and D: wasm where exact, software (exact) for the rest; also the interpreter's F/D |
| `rvinterp.c` | simple interpreter and a predecoding one |
| `engine.c`, `engine.h` | prototype engine: driver loop, region cache, table slots, tiering, helpers, lockstep |
| `port_*.c`, `fmt.c`, `main.c` | what each build provides |

The architecture-specific part is `rvsem.c`, `rvfp.c` and `rvdec.c`, behind the functions
declared at the end of `xlat.h` (`arch_decode`, `arch_regs`, `arch_emit`,
`arch_emit_cond`, `arch_emit_link`, `arch_emit_indirect_target`, helpers).

### 4.2 Regions and structuring

A region is everything reachable from one entry address by direct branches and
fall-through (not into calls), up to 512 blocks and 4096 instructions. Three ways to turn
it into wasm, all implemented and measured:
- `block`: one region per basic block; the engine dispatches between them.
- `dispatch`: all blocks in one function: `loop { block ... block { br_table } ... }`,
  a branch sets the next block number and jumps to the loop; fall-through to the next block
  is free.
- `struct`: structured code from the dominator tree, after N. Ramsey, "Beyond Relooper"
  (ICFP 2022): loop headers become `loop`, blocks with two or more forward predecessors
  follow a wasm `block` that their predecessors leave with `br`, all other blocks are
  placed inline. It needs a reducible graph; an irreducible region falls back to
  `dispatch` (none of the test programs had one).

### 4.3 Registers and memory

- Guest integer registers are wasm i64 locals, floating-point registers f64 locals with
  the raw bits. A region loads the registers it uses at entry and stores the ones it
  writes at every exit (one shared exit block). x0 is a constant.
- Guest memory is one block of the engine's memory; guest address `a` is at
  `base + a`, the base folded into the load's constant offset. The guest address space
  must therefore be below 4 GiB (an assumption, section 7). Three disciplines:
  `unchecked` (32-bit wrap; out of range traps or aliases), `masked` (AND with size - 1:
  never leaves guest memory, a wild access is not reported), `checked` (compare; a fault
  leaves with the exact state, which is precise because no register is written before the
  access). Default: masked (an assumption).

### 4.4 Exits, system calls, time slices

- A region returns a reason: branch out of the region, return, "interpret this
  instruction" (anything the translator does not handle: CSR writes, `fence.i`, illegal
  instructions), fault, exit.
- `ecall` calls the engine's `jit_ecall` in place (registers a0 to a7 flushed first); a
  blocking system call suspends translated frames with JSPI like any wasm frame.
- Budget: a counter (a local) is decremented at every loop back edge, every function entry
  and every chained jump; at zero, `jit_yield` runs (in UC.OS a `getppid`, which lets the
  kernel's time-slice check run every 256 calls) and returns the new budget, or a negative
  number to make the region leave. Default budget 4096.

### 4.5 Calls

`jal ra, f` becomes a wasm call: the caller stores the registers it changed, calls the
callee (a direct `call` when both are in the same module, otherwise `call_indirect`
through the callee's table slot, which holds a stub until the callee is translated),
reloads what the callee may have changed, and checks that the callee returned with
`pc == return address` (otherwise it leaves: longjmp-like control flow stays correct). Guest
recursion deeper than 256 calls leaves to the engine instead of growing the wasm stack.
For a callee that is a translated leaf (no calls, only returns), only the registers it
reads are stored and only the ones it writes are reloaded. An option `abi=1` trusts the
RISC-V calling convention (callee-saved registers survive); it is a deviation (hand-written
assembly may break the convention) and measured no clear gain under load, so it is off.

What would close more of the gap on call-heavy code (not built): pass the callee's
registers as wasm parameters and results (multi-value) between functions of one module,
so calls stop going through memory.

### 4.6 Indirect jumps and chaining

- `chain`: a region that leaves to a known address tail-calls the next region
  (`return_call_indirect`, wasm tail calls) instead of returning to the engine; an indirect
  jump looks its target up (`jit_lookup`) and tail-calls it. At depth 0 a return does the
  same.
- `ic`: a one-entry inline cache (target, slot) per indirect site, as module globals.
- `learn`: an indirect jump records (site, target) when it misses; after 256 misses the
  regions that contain the site are retranslated with the targets seen so far (up to 16)
  as ordinary successors, so a switch becomes a chain of compares and branches inside the
  region. Preliminary, Node, load 7 to 9: `switch` from about 700 to about 2500 MIPS.

### 4.7 Tiering, cache, invalidation

Interpret first; count block entries per address; translate a region at 50 entries (an
assumption: the break-even estimate in section 1 says 10,000 instructions, and hot
blocks run many instructions per entry). Regions live in a hash table with a table slot
each. `fence.i` and the `riscv_flush_icache` system call drop every translation (RISC-V
requires one of them after code is modified, so self-modifying and generated guest code
is handled without watching stores). A write to `frm` drops every translation (code
translated for round-to-nearest-even assumed it). No size bound on the cache yet.

### 4.8 Floating point

Exact in wasm with round-to-nearest-even, after replacing NaN results by RISC-V's
canonical NaN: add, sub, mul, div, sqrt, conversions, comparisons. Inline with fix-ups:
min/max (RISC-V's NaN rules), float-to-integer (saturating truncation plus the NaN case),
NaN-boxing of single-precision values. Through an exact software routine: fused
multiply-add (wasm has none; the routine is integer arithmetic), every other rounding
mode, `fclass`. Verified natively against x86 hardware: 6.5 million operations in four
rounding modes, no difference; on the n-body and fp-mix programs the RISC-V results
match native x86 bit for bit, with contraction off against plain x86 code and with
contraction on against x86 hardware FMA. Not implemented: the exception flags (fflags).

### 4.9 Correctness checks

- `tools/jit/check.mjs`: every program under every configuration must end in the
  interpreter's final state. Last full sweep (2026-11-26, after the last code change):
  20 programs (the 8 benchmarks as rv64im and rv64gc, nbody, fpmix, hello) x 12
  configurations (struct+calls, dispatch, block, aot, checked bounds, calls=0 with learned
  targets, tiny regions, budget 7 with re-entry, copytest=50, an 8 KiB code cache, abi=1,
  lockstep): 240 runs, 0 failed.
- Lockstep (`lockstep=1`): each region execution runs first under the interpreter (stores
  logged), is undone, then runs translated; registers, pc, instruction count, fcsr and the
  logged memory are compared, and the run stops at the first difference. Translated code
  in this mode leaves at every back edge, call and indirect jump, so each comparison is
  short. It does not catch a translated store to an address the interpreter did not write.
- `tools/jit/xltest.c` + `validate.mjs`: every function of a program, every strategy and
  bounds mode, validated and instantiated.
- `tools/jit/fptest.c`: the software floating point against the host.

### 4.10 Fork and migration (memory copied into a fresh instance)

The kernel's clone call (fork stream, `SYS_pclone`, private call 520) and the migration
(`SYS_pmigrate`, 522) copy the engine's linear memory and exported globals into a new instance
of the same module and enter `__fork_child` or `__migrate_resume` on a fresh stack. Tables
are not copied, and generated modules are separate instances that do not exist in the new
one. What this means for translated code, and what is built:

- **All guest state must be in memory at the system call.** Translated code keeps
  registers in wasm locals; at `ecall` it now stores the pc and every register the region
  changed, and reloads everything after the call (built; it also makes signal frames,
  `rt_sigreturn` and `execve` correct, which replace the whole state). The engine can answer
  "state replaced, continue at cpu->pc" and the region leaves.
- **The region cache is in memory and comes along, but its slot numbers do not mean
  anything in the new table.** `engine_after_copy()` (built, `libjit/engine.c`), called
  first thing in `__fork_child` / `__migrate_resume`: makes the support module again,
  resets the slot allocator, and marks every translated region cold one entry short of
  the threshold. Hot code is translated again the first time it runs (about 0.2 to 0.5 ms
  per region measured above); cold code costs nothing; learned indirect targets and
  counters survive. Tested by `copytest=N` (acts as if a copy happened every N region
  runs) in `tools/jit/check.mjs`.
- Faster alternative, not built: keep the generated bytes of each module in memory (they
  are a few KiB each) and, after the copy, grow the table to the old size and load the same
  bytes again (same slot numbers in their element segments): about 50 to 70 us per region
  instead of a translation, at the price of the bytes kept.
- **Host requirement**: the clone and migrate paths in `boot/host.mjs` must give the new
  instance the same `jit.load` import as the original (section 6).

## 5. Run time, install time, or hybrid

| | run-time translation (built) | install-time, whole file (built as `mode=aot`, prototype) | hybrid |
|---|---|---|---|
| start-up | interpreter from the first instruction; hot regions translated after 50 entries, about 0.2 to 0.5 ms each | the module must be compiled before the program runs: validation about 1 ms per 600 KiB, baseline compile only for functions that run (lazy) | as install time, plus run-time translation for what the file module does not cover |
| code quality | per region; calls between regions through the table | whole file: direct calls between all functions, callee summaries known | as install time |
| indirect jumps and calls | lookup at run time, learned targets | a lookup at run time is still needed for computed targets | same |
| code discovered at run time (JITs in the guest, `dlopen` of unknown code, self-modifying code) | handled | not covered: needs the interpreter or run-time translation anyway | handled |
| position-independent libraries | translated where loaded | needs translation for an unknown base: pc constants become base-relative (an add per `auipc`/link), or translate again per load address | same |
| size | only hot code (a few KiB to tens of KiB per program here) | about 8 to 19 bytes of wasm per guest instruction measured here; a 2 MiB libc text would be roughly 8 to 16 MiB (estimate from these ratios, not measured): above Chromium's 8 MB synchronous limit, so async compile or several modules | as install time |
| browser code cache | none (bytes made in memory) | none either inside UC.OS: V8 caches compiled code for modules fetched from the network, not for bytes from our file system | none |
| persistence | could cache translations by content hash once files persist | the module is a file next to the binary | same |

Measured in section 3.2: `aot` matched or beat `struct+calls` (sieve 2222 against 1401,
sort 1166 against 884, fib 1617 against 1321), mostly because calls between functions of one
module are direct and the first region entry does not wait for the threshold.

Assumption for continuing (for the owners to confirm or change): **run-time translation
of hot regions, with the interpreter for everything else**, because it is the only one of
the three that handles all code without an install step, and it costs only what is hot.
Install-time modules stay possible on top (the same translator emits them; `mode=aot`).

## 6. The host interface and its security

(Revised in section 11.3: inside the core engine the loader is handed over through a slot in
the program's own table after instantiation, `jit_attach(slot)`, instead of an import, so a
program that may use it still runs on a host without it. Everything below about what the
loader may reach is unchanged.)

Proposed permanent form: a host import that a program declares if it wants it.

```
(import "jit" "load" (func (param i32 i32) (result i32)))   ; ptr, len -> 0 | -1 bad module | -2 too large
```

- The host compiles `len` bytes at `ptr` in the **calling program's own memory**
  (synchronously; at most 4 MiB, under Chromium's 8 MB limit) and instantiates them with
  one import object, `{ e: <that program's own exports> }`.
- The new module reaches only what the program itself exports: its memory, its table, its
  own functions (and its imports re-exported, which are the pid-bound system call
  trampoline). It cannot name the kernel's memory or table, or another process's: those
  are not in its import object, and wasm has no other way to reach them.
- Its functions enter the program's table through the module's own element segments, so
  the host does not need a second call. When the process dies, its table and instances go
  with it.
- In `boot/host.mjs` this is about ten lines in `start()`: pass `jit: { load }` when the
  module imports `jit.load` (a check of `WebAssembly.Module.imports`). No JavaScript runs on
  the path of a translated instruction or a system call.
- A cloned or migrated instance (fork stream) must get the same `jit.load` import.
- Limits to consider: a per-process byte or count quota (a process could make the host
  compile without end; today the same process could just allocate memory without end).
- Alternative not built: an asynchronous variant through JSPI for modules above 1 MiB.

## 7. Meeting the core engine (lx)

The translator was built against a small engine of its own because the core engine's
interfaces did not exist yet. The core engine (`.worktrees/lx`, docs/LINUX-BINARIES.md
section 3 there) is now final. How the two fit, checked against its interface document:

| Need of the translator | What lx provides | Fit |
|---|---|---|
| CPU state at fixed addresses | `struct rv_cpu rv_cpu`: `x[33]` at 0 (x[32] a sink), pc 264, f 272, fcsr 528, res_addr 536 | yes: the translator takes every field's address as a parameter (`xlat_env`); its own bookkeeping (budget, counters) lives in a separate small struct |
| guest memory as base + address | `g_mem + a`, valid when `a - g_lo <= g_span` | yes: same model; the `checked` mode must compare `a - g_lo` instead of `a` (a two-line change, not made); `masked` needs a power-of-two window |
| invalidation | `arch_cur->flush_code(addr, len)` is the single entry point (fence.i, flush_icache, munmap/mprotect of code, execve) | yes: call the translator's flush from there (today: drop everything) |
| system call from translated code | the interpreter stops with `LX_STOP_SYSCALL`; the main loop calls `lx_syscall()` and `lx_signals()` | yes: the translator's `jit_ecall` becomes: (registers are already stored) set pc, `lx_syscall()`, `lx_signals()` if pending, answer "state replaced" when the pc is not the one after the ecall |
| interpreter for cold code | block-cached interpreter, `arch_cur->run()` | yes: the driver loop calls it where mine calls `rv_interp_block` |
| time slice | lx calls `getppid` every 4,000 guest instructions | the translator's budget helper does the same |
| fork | lx clones through `SYS_pclone` (520) and resumes in `__fork_child` | `engine_after_copy()` (section 4.10) goes at the top of `__fork_child`; ecall already leaves all state in memory |
| exports | the lx program must export its table and the helpers | link flags `--export-table --growable-table` and seven `export_name` functions |

What would make translated code slower if it were different (and is not): registers
reached through a pointer (would cost a load per access), a guest window that moves
after translation, a memory check that needs more than one compare.

Measured here (section 1): lx's final interpreter inside UC.OS runs these programs at
128 to 304 MIPS; natively an older snapshot of it ran 172 to 216.

## 8. amd64 and arm64

Estimates, grounded in what RISC-V showed; none of this is measured.

- **What carries over unchanged**: the emitter, region discovery, structuring, register
  locals, exits, chaining, learned targets, the host interface, the engine loop.
- **arm64**: fixed 4-byte instructions (decoding as simple as RISC-V's). Condition flags
  NZCV: set by `adds/subs/cmp/tst`, read by `b.cond/csel/cset/adc`. In compiler output a
  flag-setting instruction is almost always followed by its one consumer in the same
  block, so a liveness pass per region turns `cmp; b.cond` into one wasm compare and branch
  (as RISC-V's `blt` is now); flags live across blocks or calls need to be materialised
  (four bits stored, or the operands kept). 31 general registers fit as locals like RISC-V's.
  NEON is part of the base ABI and glibc's string functions use it: they need wasm SIMD128
  translations (128 bits map one to one for most integer operations) or helpers. Expected
  quality: close to RISC-V's for integer code after the flag pass; string functions depend
  on the SIMD work.
- **amd64**: variable-length decoding (prefixes, ModRM, SIB; a big decoder but a solved
  problem). Almost every ALU instruction writes flags: the same liveness pass, with more
  cases (partial flag updates of `inc/dec`, shifts by zero). Partial registers (AL, AH, AX,
  32-bit writes zeroing the top) cost masks and merges. Memory operands with
  base + index * scale + displacement cost a few more wasm operations per access. SSE
  scalar maps to f32/f64 like RISC-V's F/D; x87 (80-bit `long double`) has no wasm
  equivalent and needs a software implementation (slow, rare in 64-bit code). The largest
  difference is self-modifying code: x86 needs no instruction to announce modified code,
  so the engine must notice stores to translated pages itself: a check on every store (a
  bitmap lookup, a few wasm operations) or write protection the engine emulates; RISC-V's
  `fence.i` rule avoids this entirely. Expected quality: lower than RISC-V's because of
  flags and partial registers; how much lower is not known without building it.

## 9. Limits, deviations, not verified

- Floating-point exception flags are not computed (translated code and interpreter).
- `masked` bounds (the default) does not report wild accesses; `unchecked` can alias
  above 4 GiB; only `checked` is precise.
- Single-threaded semantics for the A extension (no other hart exists).
- One guest per engine; no size bound on the region cache; no multi-value calling
  convention between translated functions.
- Lockstep does not catch translated stores to addresses the interpreter did not write.
- Everything depends on V8's behaviour as measured: lazy compilation, tiering after about
  1 ms, no OSR, tail calls, the 8 MB synchronous compile limit. Firefox and Safari are not
  installed here and were not measured.
- Real Linux programs (busybox and others) were not run under translation: that needs the
  translator linked into the core engine (section 7), which was not done. So the start-up
  of `ls` or `busybox sh -c true` under D is not measured; the closest fact is the `hello`
  row of section 1 (D starts as fast as C when its guest memory is small, and translates
  nothing in a short run).
- Budget checks cost up to 2x on the tightest loops (section 3.5).

## 10. Files and how to reproduce

```sh
make -f tools/jit/jit.mk B=build-NAME                       # engine.wasm, native tools, test programs
./build-NAME/jit/xltest build-NAME/jit/guest/sort.elf DIR && node tools/jit/validate.mjs DIR
node tools/jit/run.mjs --build=build-NAME build-NAME/jit/guest/fib.elf stats=1 repeat=3
node tools/jit/check.mjs --build=build-NAME --programs=fib,switch-gc
node tools/jit/bench.mjs --build=build-NAME --programs=fib --rounds=2 --reps=5
node tools/jit/chrome.mjs --page=tools/jit/bench.html --query='build=build-NAME&programs=fib'
node tools/jit/mech.mjs ; node tools/jit/chrome.mjs --page=tools/jit/mech.html
make -f tools/jit/jit.mk B=build-NAME ucos                  # UC.OS image with rvjit and the test programs
node tools/jit/ucos-run.mjs --build=build-NAME --init=/bin/rvjit --args='stats=1 /usr/share/jit/fib.elf'
make B=$PWD/build-lxb PROGS='init lx' -C ../lx && cp build-lxb/bin/lx build-NAME/jit/lx.wasm   # case (2), then `ucos` again
sh tools/jit/quiet-bench.sh build-NAME                     # every table of section 1 and 3; waits for load < 3
```

On a shared machine: `nice -n 19`, one program per invocation, and check the load first.

## 11. Inside the core engine (lx)

Work of 2026-11-26 afternoon, in `.worktrees/lxjit` (the integrated tree: kernel with real
clone, file system, package manager, lx). Nothing committed.

### 11.1 What was built

| Piece | What |
|---|---|
| `libjit/jitcore.c`, `jitcore.h` | the translator as an engine component with a plain-C interface (region cache, table slots, translation, helpers, flush, after-copy, lockstep). Knows the translator's headers, not lx's |
| `bin/lx/jit.c` | lx's side: the switch, system calls from translated code (`lx_syscall`, then `lx_signals`), yield (`hb_tick`, leave when another guest thread or a signal waits), code fetch, running translated code from the main loop; native stubs |
| `bin/lx/rv_interp.c` | a heat counter and a translated flag per decoded block; at every block entry the interpreter stops with `LX_STOP_JIT` when the block is translated or just became hot (one predictable branch when the switch is off); the first block of a run is counted but not stopped at; `rv_flush_code` drops translations as well (fence.i, flush_icache, munmap/mprotect of code, execve); writes to frm notify the translator |
| `bin/lx/main.c`, `arch.h`, `rv_arch.c`, `sys_proc.c`, `sys.c`, `CFLAGS` | the options, `LX_STOP_JIT` in the main loop, flush_icache through `rv_flush_code`, `__fork_child` calls the after-copy path, the statistics line, the libjit sources and `--export-table --growable-table` |
| `boot/jit-proto/ext.mjs` | host side, through the tools' `extend` hook (see 11.3) |
| `build-patches/host-jit.patch` | the same as a change to `boot/host.mjs` (17 added lines); NOT applied |
| `bin/jtime.c` | runs a command and prints its wall time (UC.OS has no `time`) |
| `tools/jit/os/` | image, in-OS test and timing scripts; `tools/jit/timing.sh`, `timing-sum.mjs` |

lx's own decoder and interpreter stay the engine's; the translator keeps its own decoder
(renamed at link time, `-DJIT_RENAME`), which costs about 15 KB of code. Using lx's
decoder instead is possible (same instruction set) but means renaming every opcode in the
translator.

### 11.2 The switch

`lx --jit=off|on|lockstep` or the environment variable `LX_JIT` (the option wins; the
variable is inherited, so it also covers engines started by `spawn`, by dpkg's
`script-runner` and by the libc's ELF dispatch). **Default: off** (interpreter only, the
behaviour of pull request #14) until the owners decide. Translator options:
`--jit-opts='k=v ...'` or `LX_JIT_OPTS` (`threshold`, `bounds`, `strategy`, `calls`,
`chain`, `learn`, `ic`, `abi`, `fp`, `cache` in KiB, `maxblocks`, `maxinsns`, `verbose`).
Every assumption of question 29 stays a switch: `bounds=masked|checked|unchecked` (inside lx
the default is **checked**, so faults happen where the interpreter faults), `fp=0` makes
every floating-point instruction go to lx's interpreter.

### 11.3 Host side

The engine exports `jit_attach(slot)` and its function table. Right after every program
instance is created (spawn, exec, clone, migrate all use `new WebAssembly.Instance` in the
integrated `boot/host.mjs`), the prototype extension makes the loader function, wraps it
in a two-instruction wasm function, puts that into the engine's own table and calls
`jit_attach(slot)` before `_start` or `__fork_child` runs. No import is involved, so the
same lx binary runs on a host without the extension: it then has no loader and interprets
(one line on stderr with `verbose=1`). The prototype replaces the global
`WebAssembly.Instance` constructor with a subclass while the page lives; the permanent
form is the patch above, inside `instantiate()`. The security argument of section 6 is
unchanged: the loader compiles bytes from the calling instance's own memory and
instantiates them against that instance's own exports only.

### 11.4 Correctness on real programs

All inside UC.OS under Node, Debian 13 riscv64 packages installed by apt from a local
repository (45 packages), outputs compared byte for byte between `LX_JIT=off`, `on` and
`lockstep`:

- hello; mawk (two programs); dash pipelines and command substitution; coreutils `ls -lR`,
  `sort -k2n`, `sha256sum`, `wc`, `cmp`, `cp`; perl -e (hash, atan2, a loop); gzip -9 and -d
  round trip of bash; busybox xz -dc of a real Packages.xz; dpkg-deb --info; bash
  associative arrays; M2 (busybox sh, 37 checks) 37/37; M34 (dash, coreutils, bash) 10/10;
  `ls -lR /usr`, `dpkg -l`, sort and mawk of 100,000 lines, sha256sum of 10 MB, gzip -n of
  7.5 MB, xz -dc of perl-base's data.tar.xz: identical outputs.
- The 17 maintainer-script reinstalls (hello base-files base-passwd dash debianutils
  perl-base mawk coreutils sed grep findutils diffutils gzip tar bash libc-bin dpkg, scripts
  run by Debian's dash under lx): all exit 0 with the translator; /etc/shells, passwd, group,
  the /etc listing and the dpkg states identical to the interpreter run.
- With lx built for the kernel's clone (`-DLX_HAVE_PCLONE`): the integration's fork checks
  10/10, a bash loop forking 100 subshells, exec from bash, `dpkg-deb -c` (which hangs with
  the default emulated fork, interpreter or not), signal traps for TERM sent to itself and
  CHLD; identical in both modes. Traps for INT/TERM sent by the kernel do not run in either
  mode (known lx limitation).
- Lockstep: 0 differences over the script above and M2/M34 (thousands of region runs).
- Bugs found on the way, all in my code, fixed: lockstep's yield did not leave at back
  edges; defaults overrode user options; a callee reached from translated code was never
  counted as hot (perl made 10 million lookups, all missed; after the fix 2,488).

### 11.5 Whole-command times (partial: the run was paused)

Inside UC.OS under Node, `jtime` from spawn to exit (engine start-up included), alternating
interpreter / translator / translator with `threshold=500`, quiet lock held, load 2.7 to
3.4 unless noted. 8 runs per side for the first four rows, 4 for the others. Best (median) ms.

| command | interpreter | translator | translator, threshold 500 |
|---|---|---|---|
| `hello` | 6.2 (9.2) | 16.8 (24.4) | 12.0 (13.4) |
| `busybox sh -c true` | 4.8 (5.8) | 7.1 (9.7) | 5.2 (6.5) |
| `dpkg -l` | 64 (76) | 141 (177) | 94 (104) |
| `ls -lR /usr` (load up to 4.7) | 342 (448) | 601 (674) | 647 (835) |
| `sha256sum` of 10 MB (load rose to 17.5 at the end: recheck) | 4064 (4722) | 217 (251) | 226 (287) |
| `gzip -n -c` of 7.5 MB | 5781 (6168) | 823 (963) | 762 (778) |
| `busybox xz -dc` of 1.6 MB | 2857 (3002) | 343 (382) | 362 (381) |
| `perl -e` loop of 1,000,000 | 2028 (2268) | 557 (725) | 478 (1156) |
| `sort -n` of 100,000 lines | 1733 (2013) | 745 (746) | 972 (983) |
| `mawk` on the same file | 1597 (1669) | 415 (471) | 442 (817) |
| `apt reinstall -y debianutils` (scripts under lx; load up to 7) | 1609 (1734) | 3217 (3530) | 2266 (2292) |

Pending: the second half of these runs, the same in Chromium, and the memory figures.

What it says so far: translated, the long computations are 2.3 to 19 times faster as whole
commands; start-up dominated commands are slower (hello +11 ms, `dpkg -l` +77 ms, `ls -lR`
+260 ms at threshold 50), because every engine process translates its hot code anew (0.5 to
1 ms per region, regions of about 20 KB of wasm); the maintainer-script run, which starts
many short engines and forks, is twice as slow. Levers: a higher threshold (helps short
commands, costs long ones a little), smaller regions, a cache of generated code shared
between processes (the same libc regions are translated in every process; not built), and
forked children that translate only what gets hot in them (built after this run, not yet
measured).

### 11.6 Region size, memory and the cache bound

Single runs inside UC.OS under Node at load below 5 (indicative, not timings), `LX_STATS=1`
for the translator's own counts, `ucos-run.mjs --stats` for the Node process's peak RSS
(the whole system: kernel, 48 MB image, store, every engine).

`ls -lR /usr`, interpreter: 485 ms, peak RSS 208 MiB.

| translator, regions limited to | regions | wasm generated | generation + host compile | whole command | peak RSS |
|---|---|---|---|---|---|
| 4096 instructions, 512 blocks (default) | 500 | 20.2 MB | 374 + 417 ms | 3.2 s | 336 MiB |
| 512 instructions | 513 | 12.5 MB | 217 + 251 ms | 2.1 s | 291 MiB |
| 128 instructions, 32 blocks | 675 | 5.3 MB | 172 + 228 ms | 1.24 s | 233 MiB |
| default, threshold 500 | 415 | 15.3 MB | 216 + 220 ms | 1.8 s | 306 MiB |

- The generated code is large: about 94 bytes of wasm per guest instruction with the default
  regions (every call site stores and reloads registers; every checked access carries its
  own fault exit). Smaller regions cut both the code and the time to make it; the same
  output (the real-program script, byte for byte).
- Memory that can be measured: the bytes of wasm generated and kept (`live` in the
  statistics line; bounded by `cache=`, default 32 MiB per engine), the region table (a few
  dozen bytes per region), and under Node the process's RSS, which includes V8's compiled
  code: +128 MiB for 20 MB of wasm, +26 MiB for 5.3 MB in the runs above. Browsers do not
  report the memory of compiled modules to the page; in Chromium it can only be seen from
  outside (the renderer process's PSS), which was not done.
- The table grows by 1,024 slots at a time and slots are never reused (a dropped region's
  slot gets the stub back).
- At the bound, every translation is dropped (the simplest policy). When the working set
  does not fit, it thrashes: perl's loop with `cache=1024` (1 MiB) re-translated 7,264
  regions (180 MB generated over the run, 8.6 s against 3.6 s unbounded in the same
  conditions); with small regions the same 1 MiB holds the working set (707 regions, 641 KB
  live, 1.65 s). Output correct in all three. A finer policy (drop the least used) is not
  built.
- Forked children: after a copy every region starts cold and the child's decoded blocks are
  dropped too, so a child that execs soon translates nothing (built after the fork test of
  11.4; the fork-heavy maintainer-script timing is to be repeated).
