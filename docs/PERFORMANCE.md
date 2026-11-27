# Performance

What has been measured, how, and what the numbers do and do not mean. Quote from this
file, with the caveats, or measure again. Do not round the caveats away.

All figures were measured on one laptop (Intel i5-8250U, 4 cores, 8 threads) with Node 26
and headless Chromium 158. During the 2026-11-26 performance pass other engineers were
compiling and testing on the same machine (load average 2 to 19), so absolute numbers are
noisy and pessimistic. Every claim of "faster" below comes from an A/B comparison: both
builds run alternately (A, B, A, B, ...) at least 10 times each, and the best and the
median of the runs are compared. A difference smaller than the spread is reported as "no
measurable difference".

Sizes in this file were measured on the tree of the performance pass itself (commit
e733652 plus its changes). The kernel hardening merged since (`ddff3da`) made the kernel
and the image larger: on `ddff3da` plus this pass, `kernel.wasm` is 42,144 bytes,
`initrd.tar` 7,813,120 bytes and `initrd.tar.gz` 3,778,902 bytes (measured 2026-11-26 from
`make B=... dist`). The speed figures were not re-measured on that tree.

## 1. System calls

`sysbench --json` (best batch of each run; "best" is the best run, "median" the median run).
"Before" is the tree before the performance pass (commit e733652), "after" is the tree with
the changes listed in section 9. Rows are timed on the process CPU clock, spawn on the wall
clock. Chromium's clock ticks every 0.1 ms, so its rows use batches of 20 ms or more.

| Operation | Node before | Node after | Chromium before | Chromium after | Difference |
|---|---|---|---|---|---|
| `getppid` | 11.8 / 13.8 ns | 10.6 / 12.2 ns | 17.6 / 19.1 ns | 13.9 / 15.4 ns | -12% Node, -19% Chromium |
| read 64 B from `/dev/zero` | 43.9 / 51.1 ns | 45.2 / 50.4 ns | 81.1 / 90.0 ns | 80.8 / 87.0 ns | none measurable |
| write 64 B to `/dev/null` | 20.0 / 23.0 ns | 18.3 / 20.2 ns | 26.4 / 30.7 ns | 23.1 / 25.2 ns | -12% Node, -18% Chromium |
| pipe write + read, 64 B | 84.6 / 96.6 ns | 84.0 / 97.3 ns | 145 / 165 ns | 133 / 142 ns | none in Node, -14% Chromium |
| open + close | 135 / 165 ns | 150 / 172 ns | 197 / 221 ns | 210 / 224 ns | medians +1 to +4%; best 7 to 13% higher in three runs (see below) |
| `stat` | 122 / 145 ns | 124 / 136 ns | 176 / 196 ns | 184 / 200 ns | none measurable |
| `clock_gettime` | 102 / 120 ns | 89 / 100 ns | 384 / 443 ns | 193 / 210 ns | -17% Node, -53% Chromium |
| spawn + wait `/bin/true` | 72.5 / 79.3 us | 74.1 / 79.3 us | 156 / 172 us | 125 / 172 us | none measurable |
| spawn + wait `sh -c :` (100 KB program) | 96 / 103 us | 98 / 105 us | 141 / 171 us | 142 / 186 us | none measurable |

These rows were measured by the engineer who made the pass. A second, independent Node
A/B of the same two builds (8 alternating runs, best / median, load average 4 to 8) found
`getppid` 11.3 / 14.4 to 10.7 / 13.1 ns, write 21.2 / 24.8 to 17.8 / 22.4 ns,
`clock_gettime` 109 / 130 to 89 / 113 ns, and no measurable difference on read, pipe,
open + close, `stat` and spawn: the same direction, slightly smaller gains. The Chromium
columns have not been repeated independently; one run of the "after" build in Chromium gave
`getppid` 12.9 ns and `clock_gettime` 201 ns.

Each cell is best / median of 10 alternating runs (Node at load average 3 to 6, Chromium
at 3 to 7). The cold first batch of 200 calls is unchanged by this pass: 160 to 500 ns for
the cheap calls, about 1 us for pipe and open, 1 to 3 ms for the first spawn. One earlier
Chromium A/B of the same two builds, run while the machine was overloaded, showed the
"after" side twice as slow on open, stat and spawn (medians only; best values equal) and is
not used. The open + close cost: `openat` and `close` now go through `syscall_slow`, one
more function call each; that is the likely cause of the higher best values.

Other figures: the kernel (39 KB then, 42 KB on `ddff3da`) is up in 0.4 to 0.9 ms. A process that only
writes (`yes > /dev/null`) sustains about 27 million system calls per second in Chromium
while the desktop stays responsive (measured before this pass).

Reference figures, not measured in this pass: native Linux on this laptop (`sysbench` built
with `cc -O2`, measured earlier): `getppid` 884 ns, read 1,005 ns, write 962 ns, pipe
2,016 ns, open + close 3,310 ns, `stat` 1,559 ns, spawn `/bin/true` 627 us (fork 251 us).
unkernel's published figures (other hardware): `getppid` 172 ns, read 172 ns, pipe 482 ns,
fork 57 us; native Linux on unkernel's machine: 110, 161, 417 ns, 73 us.

### How to read this

- **Warm versus cold.** "Warm" is the best batch after the browser engine has optimised
  the code. Until then calls cost 0.1 to 1.6 microseconds. A program that makes a call only
  a few times pays the cold price. Say which one you quote.
- **This is not a like-for-like comparison with Linux.** A UC.OS call is a function call
  with no privilege switch, and the kernel does far less per call: no permissions, no locks,
  no security checks. This laptop's native figures are also unusually slow (CPU
  vulnerability mitigations), so unkernel's machine is the fairer reference for "native".
- **Where UC.OS is behind:** starting a process (each start instantiates a wasm module and
  allocates its memory through the browser), and anything that needs the time (it comes
  from JavaScript).
- Firefox and Safari have not been measured.

## 2. Where the time of a system call goes

`tools/bench/callcost.mjs` (Node only) times a wasm loop calling the real `kernel.wasm`
entry point, directly and through the real per-process trampoline, against a stand-in
callee that only returns. `getppid`, 9 rounds of best-of-5 batches of a million calls:

| Path | ns per call, one run (best / median) | earlier runs (medians) |
|---|---|---|
| program to a callee that returns, one cross-instance call | 4.7 / 4.7 | 3.9 to 4.3 |
| the same through the trampoline (two cross-instance calls) | 8.8 / 9.0 | 7.7 to 8.4 |
| program to the real kernel through the trampoline, before | 13.6 / 13.8 | 11.3 to 12.4 |
| the same, after this pass | 11.1 / 11.5 | 9.8 to 10.4 (an intermediate version) |
| the kernel called directly, without the trampoline: before / after | 9.7 / 7.6 (medians) | |

So about a third of a warm `getppid` is the call itself, a third is the trampoline hop
(the price of an unforgeable pid, which stays), and a third was kernel work; this pass took
the kernel's share from about 4.8 to about 2.6 ns. What the
compiler emitted before the pass (`llvm-objdump -d kernel.wasm`): `syscall_pid` set up a
784-byte stack frame on every call (the path buffers of `openat`, `stat` and others were
inlined into it), kept two counters (`nsys` and `slice_calls`), and the clock check sat in
line with its floating point. After: the four hot calls (`getppid`, `getpid`, `read`,
`write`) are dispatched in `syscall_pid`, which has no stack frame; everything else is in
`syscall_slow`; the time slice uses the `nsys` counter; the clock check is out of line.

Measured and found to make no measurable difference (A/B, both Node and Chromium unless
noted): passing 6 instead of 8 arguments from the trampoline to the kernel (the program
side cannot change without a libc change); clearing the `sleeping` flag outside the hot
path (Node); moving the slice check body out of line on its own; dispatching through a
local pointer instead of `cur`; reading the clock every 1024th call instead of every 256th.

One cost remains that is not understood: with the slice check removed entirely (only as an
experiment), `getppid` in Chromium measured 12.9 to 16.0 ns median against 15.4 to 18.7 ns
with it, in four separate A/B runs (the experimental kernel also took 6 arguments, which on
its own made no difference); `read`, `write` and pipe showed no measurable
difference. Preemption itself is not the cause (an experiment with an effectively infinite
slice measured the same as the normal kernel). The check is three wasm instructions on the
hot path. Removing it would break the guarantee that a process making system calls yields
within about 8 ms, so it stays.

### The clock

Reading the time is a call from wasm into JavaScript. `tools/bench/clockcost.mjs`, ns per
call of an imported `now()`, best / median of 15 batches:

| Import | Node | Chromium |
|---|---|---|
| a wasm function (the floor) | 3.2 / 3.7 | 3.5 / 3.8 |
| an empty JavaScript function | 8.0 / 8.5 | 24.7 / 25.3 |
| `() => performance.now()` (before) | 71.5 / 75.0 | 316 / 331 |
| `performance.now.bind(performance)` (after) | 58.7 / 63.1 | 120 / 122 |
| `Date.now` (millisecond resolution, not monotonic) | 60.1 / 75.1 | 76 / 77 |

Passing the bound function instead of an arrow function halves `clock_gettime` in
Chromium (section 1). Same values, same resolution, same monotonicity.

### Copies between memories

Every `copy_from_user` and `copy_to_user` makes two cross-instance calls: `s()` for the
process size (the range check) and the copy. An experiment (not in the tree: it changes
`kernel/proc.c` and `kernel/kernel.h`) moved the range check into the shim, so each copy is
one call that returns 0 or `-EFAULT`. Node, 10 alternating runs, median: pipe write + read
107 to 89 ns, read 46.6 to 43.4 ns, `clock_gettime` 127 to 113 ns, `stat` no measurable
difference. All four test suites passed with it. Not measured in Chromium.

## 3. User code

`bin/ubench.c`: CPU-bound kernels that make no system call while timed, each in a
`noinline` function called 5 to 9 times; milliseconds per run on the process CPU clock.
The same file compiled natively with `gcc 16 -O2` on the same laptop, runs interleaved
(10 of each, machine heavily loaded, so compare the ratio, not the absolute numbers):

| Kernel | native, best / median | UC.OS Node, best / median | Node / native (medians) | UC.OS Chromium, best / median | Chromium / native | Chromium cold first run |
|---|---|---|---|---|---|---|
| sieve to 4 million | 12.1 / 15.3 | 9.8 / 18.3 | 1.19 | 11.8 / 14.8 | 0.97 | 47.6 |
| quicksort 1M ints (same code) | 90.5 / 107 | 87.6 / 118 | 1.11 | 95.1 / 122 | 1.14 | 153 |
| libc `qsort` 1M ints | 171 / 198 | 176 / 224 | 1.13 | 257 / 280 | 1.41 | 487 |
| `memcpy` 16 B x 4M | 15.3 / 17.4 | 26.9 / 34.6 | 1.99 | 30.0 / 34.4 | 1.98 | 55.3 |
| `memcpy` 4 KiB x 16K | 3.35 / 4.15 | 3.00 / 3.83 | 0.92 | 3.40 / 4.40 | 1.06 | 5.05 |
| `memcpy` 1 MiB x 64 | 2.58 / 5.56 | 2.54 / 3.16 | 0.57 | 2.60 / 3.55 | 0.64 | 4.00 |
| `memset` 16 B x 4M | 16.4 / 19.7 | 21.0 / 27.0 | 1.37 | 24.7 / 31.4 | 1.60 | 52.2 |
| `memset` 4 KiB x 16K | 1.96 / 2.42 | 1.85 / 2.43 | 1.01 | 2.00 / 2.60 | 1.08 | 3.35 |
| Mandelbrot 600x400, doubles | 41.8 / 56.8 | 40.9 / 49.6 | 0.87 | 43.7 / 54.7 | 0.96 | 81.2 |
| recursive `fib(30)` | 2.95 / 4.28 | 4.93 / 6.32 | 1.48 | 6.00 / 7.45 | 1.74 | 10.1 |
| `snprintf`/`strlen`/`strtol` x 100K | 62.9 / 78.5 | 67.7 / 81.2 | 1.03 | 83.2 / 103 | 1.31 | 139 |
| `malloc`/`free` 200K blocks | 3.45 / 4.19 | 173 / 234 | 56 | 175 / 230 | 55 | 268 |
| CRC-32 over 8 MiB | 20.9 / 27.8 | 22.5 / 30.1 | 1.08 | 23.8 / 29.8 | 1.07 | 37.6 |

Milliseconds per run. These UC.OS figures already include the `lib/umem.s` change below.
The native `memcpy` is glibc's; the 1 MiB native median is inflated by load.

- Plain loops, integer and floating point arithmetic, our own sort and large copies run at
  native speed or within 20%, under Node and in Chromium.
- Code that goes through our libc (`qsort` with a callback, `snprintf` and friends): 1.03 to
  1.13 times native under Node, 1.31 to 1.41 in Chromium.
- Function calls are slower: `fib` 1.5 times native under Node, 1.7 in Chromium.
- `malloc`/`free` churn is about 55 times slower than glibc: `libc/src/malloc.c` is a
  first-fit free list with address-ordered insertion, so every `free` walks the list. This
  is the algorithm, not WebAssembly (see section 9, proposals).
- Small `memcpy`/`memset` were 1.6 to 1.7 times native under Node before the pass (and
  much worse in Chromium): V8 compiles a `memory.copy` or `memory.fill` of unknown length
  into a call out of the generated code. The libc's versions (`lib/umem.s`) now copy up to
  16 bytes with plain loads and stores. 10 alternating runs, median (best):

  | 16-byte kernel, 4M calls | Node before | Node after | Chromium before | Chromium after |
  |---|---|---|---|---|
  | `memcpy` | 38.8 ms (33.5) | 26.9 ms (23.7) | 172 ms (147) | 60 ms (35) |
  | `memset` | 32.5 ms (28.4) | 21.8 ms (18.9) | 177 ms (118) | 57 ms (30) |

  4 KiB copies, the string kernel and the sort showed no measurable difference. Small
  copies and fills are still 1.4 to 2.0 times native after the change. The same
  change in the kernel made `read` 4 to 12% slower (median, five A/Bs): the kernel's calls
  are 64 bytes or more, and the larger function is no longer inlined by V8. With the kernel
  back on the one-instruction `lib/mem.s`, read measured 13% and pipe 27% faster (median)
  than with the short path in both. So the kernel keeps `lib/mem.s`.
- The first (cold) run is 1.5 to 3 times slower than the best: code starts in V8's baseline
  tier.

## 4. Starting a process

Steps of `start()` in `boot/host.mjs`, each repeated 300 times in isolation with the real
trampoline, shim and program (`tools/bench/spawncost.mjs`), microseconds per step:

| Step | Node, `true` | Chromium, `true` | Chromium, `sh` (100 KB) |
|---|---|---|---|
| compile the program (once, then cached) | 417 | 300 | 1,400 |
| `new WebAssembly.Suspending` | 0.9 | 1.0 | 1.3 |
| instantiate the trampoline | 13 | 13 | 13 to 20 |
| instantiate the program | 124 | 122 | 150 to 159 |
| for comparison: allocating an 18-page (1.1 MiB) memory alone | | 46 | 56 |
| instantiate the shim | 9 to 12 | 12 to 16 | 14 to 23 |
| `WebAssembly.promising(_start)` | 1.0 | 2.0 | 2.3 |

The isolated figures for instantiating the program include allocation pressure (instances
are kept until garbage collection), so they overstate it; the end-to-end `spawn_wait` row
of section 1 is the reliable number. The program instance, mostly its memory, dominates.
Synchronous and asynchronous instantiation cost the same per step.

Ideas measured (A/B, 10 runs each):

| Idea | Node | Chromium | Verdict |
|---|---|---|---|
| one trampoline per pid, made once and reused | not separately | no measurable difference | rejected |
| synchronous `new WebAssembly.Instance` for trampoline, program and shim, no awaits | `true` 114 to 98 us, `sh` 148 to 136 us (median) | `true` 175 to 159 us, `sh` 183 to 213 us (median, reproduced twice) | rejected: slower for `sh` in Chromium |

Chromium 158 refuses both synchronous `new WebAssembly.Module` and synchronous
`new WebAssembly.Instance` (even of an already compiled module) on the main thread when the
module is larger than 8 MB (`RangeError`, probed with generated modules of 8,000 and
8,200 KiB). The largest program, `doom`, is 445 KB.

**The kernel layers and process start (2026-11-27).** No timed comparison of the three
kernel layers (fork and signals, the file system with persistence, users and permissions)
against the main before them exists: the machine never had a quiet window. What exists is a
count of user-space instructions per process start under Node, through `init`, with `xargs`
as the parent (`seq N | xargs -n 1 PROG`; the count at N = 10,000 minus the count at
N = 2,000, divided by 8,000). Eight passes, median against the main before the layers
(`1a934da`):

| Side | `/bin/true` | `sh -c :` |
|---|---|---|
| control (the same main again) | 1.0014 | 1.0027 |
| fork and signals | 1.0115 | 1.0159 |
| plus the file system | 1.0136 | 1.0117 |
| plus users and permissions, as root | 1.0142 | 1.0179 |
| plus users and permissions, as the desktop user | 1.0172 | 1.0156 |

The last two rows were counted on an earlier head of the users layer; four more passes on
the head that was merged gave 1.0172 and 1.0136 for `/bin/true` (root, user) and 1.0111 and
1.0157 for `sh -c :`, the same within what the count resolves. A side's own passes spread
0.1 to 2.8% (5.9% once, from a single low pass on the file system row with `sh`), so the
count resolves about 1%, not finer. It counts instructions, not time:
it says each layer adds about 1 to 2% to the instructions of a process start, and nothing
about memory stalls or the browser.

## 5. Presenting the screen

The kernel keeps the bounding box of every write to `/dev/fb0` since the last frame and
tells the host once per frame; the page copies only that rectangle to the canvas
(`putImageData` with a dirty rectangle). Time the page spends per presented frame in
headless Chromium (software canvas), median of 5 alternating runs (`tools/bench/frames-ab.mjs`):

| Scenario | 1280x720 before | after | 2560x1440 (`?scale=2`) before | after |
|---|---|---|---|---|
| whole screen changes every frame (`fbdemo`) | 0.71 ms | 0.72 ms | 2.10 ms | 2.17 ms |
| one terminal cell per key (typing at the shell) | 0.72 ms | 0.042 ms | 2.57 ms | 0.035 ms |
| `fetch -a` animation, 30 frames per second | 0.64 ms | 0.089 ms | 3.67 ms | 0.20 ms |

Pixels copied per frame when typing: 921,600 before, about 1,100 after (1,900 at scale 2).
Correctness: `tools/bench/present-check.mjs` compares every canvas pixel with the kernel's
framebuffer after typing, during the animation, after it, and after two window resizes, at
scale 1 and 2: 0 differing pixels in every step. A single box means that two small changes
far apart (the bar clock and a terminal cell in the same frame) upload the rectangle that
spans both.

## 6. Download and boot

The image was 7.7 MB as a plain tar (5.8 MB of it fonts) and 3.7 MB gzipped (`gzip -9`);
7.8 MB and 3.8 MB on `ddff3da`.
`make dist` now also writes `initrd.tar.gz`; the page fetches it and unpacks it with the
browser's `DecompressionStream`, and falls back to `initrd.tar` when there is no `.gz`.
The Node tools still read the plain tar.

Boot phases in headless Chromium served from localhost, milliseconds since navigation,
best / median of 10 alternating runs (`tools/bench/boot-ab.mjs`, load average about 19):

| Phase | plain tar | gzip |
|---|---|---|
| kernel and image fetched (from page script start) | 145 / 230 | 280 / 405 |
| kernel started (`kmain` returned) | 23 / 43 | 24 / 41 |
| from kernel up to the first frame | 51 / 91 | 57 / 103 |
| first frame, since navigation | 414 / 737 | 625 / 1,021 |

On localhost the transfer takes no time, so this shows only the cost of unpacking: about
135 to 175 ms on this loaded machine. The transfer saved is 3.99 MB: about 3.2 s at
10 Mbit/s, 0.64 s at 50 Mbit/s, 0.16 s at 200 Mbit/s. Break-even is roughly 200 to
240 Mbit/s. A web server that compresses on the fly gives the same saving without the
`.gz` file. Not built: unpacking while downloading (streaming), which would hide most of
the unpacking time on a slow network.

Not done, with the reason: `WebAssembly.instantiateStreaming` for the kernel (39 to 42 KB; the
whole kernel start, including copying 7.7 MB into kernel memory and unpacking the archive,
is 23 to 43 ms); compiling the boot programs in parallel (the host does not know which
programs the desktop will start; doing it needs the host to read the archive itself).

## 7. Compiler flags

Whole tree built with each flag set (kernel, libc, every program); all three suites pass
with every one. Sizes in bytes:

| Flags | kernel.wasm | all programs | `sh` | `doom` | initrd.tar | initrd.tar.gz |
|---|---|---|---|---|---|---|
| `-O2` (current) | 39,084 | 1,775,489 | 103,574 | 445,287 | 7,731,200 | 3,745,231 |
| `-O3` | 41,719 | 1,884,605 | 118,314 | 471,124 | 7,833,600 | 3,763,580 |
| `-Os` | 32,247 | 1,429,076 | 81,711 | 384,475 | 7,383,040 | 3,670,493 |
| `-Oz` | 31,037 | 1,307,395 | 74,233 | 377,600 | 7,260,160 | 3,646,382 |
| `-O2 -flto` | 50,372 | 2,091,114 | 156,556 | 574,023 | 8,048,640 | 3,799,391 |
| `-O2 -msimd128` | 39,299 | 1,802,991 | 104,337 | 452,621 | 7,751,680 | 3,757,276 |

Speed, Node only, 10 alternating runs against `-O2` (sysbench rows and every ubench
kernel), measured while the machine was heavily loaded (load average 8 to 19), so only
large effects count:

- `-O3`: no measurable difference on any row.
- `-Os` and `-Oz`: `fib` 1.7 to 1.9 times faster, 16-byte `memset` 1.2 to 1.3 times
  slower (with the previous `lib/mem.s`), everything else within the noise. `-Oz` read and
  write measured 14 to 15% faster in one A/B; not repeated, not measured in Chromium.
- `-O2 -flto`: no row faster beyond the noise; most ubench medians were 4 to 30% slower.
- `-O2 -msimd128`: no row faster beyond the noise; sieve and sort medians were 1.2 and 1.5
  times slower in one A/B (not repeated).

wasm-opt was not used. Chromium was not measured for the flags.

## 8. Mechanisms measured in isolation

Small experiments run before the design was fixed (Node 26, same laptop):

| Mechanism | Cost |
|---|---|
| direct wasm-to-wasm call into another instance | 4 to 7 ns |
| the same call routed through a JavaScript function | 47 to 65 ns |
| a blocking call: JSPI suspend plus resume | about 190 ns |
| copy 64 bytes between two memories with a wasm shim (called from JS) | 38 ns |
| the same copy through a JavaScript typed array | 129 ns |
| a countdown check in every loop iteration (rejected design) | +51% on an empty 1 ns loop |

## 9. What the performance pass changed, and what it proposes

Kept in the tree (each measured, section numbers above):

| Change | Where | Effect |
|---|---|---|
| hot dispatch without a stack frame, one counter, clock check out of line | `kernel/syscall.c` | section 1 |
| clock import passed bound | `boot/host.mjs` | `clock_gettime` halved in Chromium |
| framebuffer damage box, partial canvas upload, one host notification per frame | `kernel/fb.c`, `boot/host.mjs`, `boot/page.mjs` | section 5 |
| libc `memcpy`/`memmove`/`memset` up to 16 bytes without bulk-memory instructions | `lib/umem.s` (new), `Makefile` | 16-byte copies and fills 1.5 times faster under Node, about 3 times in Chromium (section 3) |
| gzipped image with fallback | `Makefile`, `boot/page.mjs` | section 6; whether to ship it is an open owner decision (`docs/DECISIONS.md`, open items) |
| boot phase marks (`window.ucosBoot`), `?stats` frame counters | `boot/page.mjs` | measurement only |

Proposals outside the files this pass could change (measured where stated):

- Bounds check inside the copy shim (section 2): pipe round trip 17% faster under Node.
- `libc/src/malloc.c`: about 55 times slower than glibc on allocation churn (section 3).
  Options that remove the list walk on `free`: size classes (segregated free lists), or a
  boundary-tag allocator that merges neighbours without searching. Not tried.
- `kernel/tty.c` `pts_write` copies through a 512-byte buffer and handles every byte with
  `out_char`; `kernel/dev.c` `/dev/zero` zeroes a 256-byte buffer and copies it, and
  `/dev/urandom` calls into JavaScript for every 4 bytes. Not measured.
- `struct proc.slice_calls` (`kernel/kernel.h`) is no longer used.

## 10. Graphics and programs

All from headless runs under Node at 1280x720 unless noted; reported by the component
authors before this pass.

| What | Figure |
|---|---|
| window manager, full-screen composite | 1.0 to 2.0 ms (2026-11-26, painted in bands: 0.95, 1.08, 1.99 ms in three runs of `perf.mjs`; 1.58 ms twice with the old full-screen buffer) |
| a 600x400 window's `ui_present` until its pixels are in the framebuffer | about 1.1 ms (median) |
| window manager when idle | 0 framebuffer writes per second with the bar hidden; 1 frame per second for the clock |
| terminal, full repaint of 160x45 cells (native build) | 2.9 ms |
| terminal while a child floods output (in the OS) | about 1 ms per frame; 13 to 15 MB/s through the pty |
| terminal parser (native build) | 80 to 90 MB/s |
| fetch 3D logo, one frame (in the OS) | 0.14 to 0.35 ms |
| font rasterizer (native build) | 140,000 to 340,000 glyphs per second at 16 px |
| cached text drawing (native build) | 1.1 to 2.1 million glyphs per second |
| Doom, engine time per frame | 1.1 to 1.5 ms in game; timedemo 680 to 850 fps |
| Doom memory after loading a level | 22 MiB (25 MiB in a window) |

### make

Measured 2026-11-26 by the author of `make`, on the 8-thread build laptop with a load
average of 7 to 11 from other builds, so every figure is noisy; not repeated by anyone else. `bin/make/tests/perf/gen.mk N=10000` writes a makefile
with 10,000 objects from 10,000 sources through a pattern rule, 10,000 `+=` lines, 1,000
target-specific variables and a 1,000-step chain (12,000 lines). Native figures are wall
time of `/usr/bin/time`; UC.OS figures come from time stamps on the console under Node
(`bin/make/tests/perf/stamp.mjs`).

| What | GNU Make 4.4.1, native | ours, native | ours, in UC.OS |
|---|---|---|---|
| read and evaluate big.mk (`parse-only`) | 0.77 to 0.86 s | 0.02 to 0.04 s | 0.04 s |
| no-op build, sources spread over small directories (`DEEP=1`) | 1.31 to 1.38 s | 0.18 to 0.24 s | 0.19 to 0.20 s |
| no-op build, 10,000 sources in one directory | 0.53 to 0.61 s | 0.35 to 0.43 s (before the `+=` fix) | 6.6 s, 95% in the kernel |
| `make -t`, create all 22,000 files, one directory | 1.9 s | 1.1 s | 30.7 s, 95% in the kernel |
| `make -t`, `DEEP=1` | | | 0.52 s |

- Both makes do about 21,000 `stat` calls for the no-op build.
- GNU Make's parse time comes from appending to a simple variable 10,000 times (each `+=`
  copies the whole value). Ours appends in place. Before that fix our parse took 2.0 s in
  UC.OS, 86% of it in `strlen`.
- With 10,000 files in one directory, UC.OS spends its time in the kernel's `walk` and
  `vfs_create`, which search a directory's linked list of entries (CPU profile with
  `node --cpu-prof`). Directory size, not make, dominates there.

## 11. Memory

Headless under Node at 1280x720, 2026-11-26: the default desktop, then two more terminals
opened with Mod+Return, then `ps` in the last one. Each figure is the process's whole wasm
memory (`ps` MEM); two runs each. The kernel's own memory was 16.3 MiB both times.

| Process | Before | After | Why |
|---|---|---|---|
| wm | 14.9 MiB | 6.4 MiB | no full-screen back buffer or wallpaper; window buffers freed before the new size is allocated |
| bar | 1.9 MiB | 1.9 MiB | |
| welcome terminal | 10.1 MiB | 8.3 to 8.4 MiB | window buffer freed before the new size is allocated |
| second terminal | 6.9 MiB | 6.1 MiB | same |
| third terminal | 5.4 MiB | 2.9 MiB | same, and the Nerd fallback font is not read until a glyph needs it |
| init, sh, ps, each | 1.4 MiB | 1.1 MiB | the heap grows one page at a time; the argument buffer is 4 KiB, not 64 |
| all processes | 47.5 MiB | 32.4 to 32.6 MiB | |

After `perf.mjs` opens and closes 20 windows three times, the wm holds 91 pages (5.7 MiB)
instead of 215 (13.4 MiB).

What is left: every program reserves a 1 MiB stack (the deepest use measured in the test
suites is 54 KiB, in `sh`); every font a program uses is read whole into its memory; the
kernel keeps the initrd (7.6 MiB then, 9.8 MiB on `35050ca`; 5.5 MiB of it fonts), the
framebuffer (3.6 MiB) and two 512 KiB buffers for each window's socket.

## 12. Sound (2026-11-26)

The design (DECISIONS.md 20, SOUND.md) was chosen from benchmarks of each piece, compiled
to WebAssembly the same way as the real code, median of 7 batches, this machine:

| Work | Node | Chromium |
|---|---|---|
| Doom's mix: 8 channels, resample 11025 to 48000 Hz, one frame of sound (1,371 frames) | 28.0 µs | 29.6 µs |
| Sum 2 streams, one Doom frame of sound (what kernel mixing would cost) | 9.2 µs | 10.6 µs |
| Sum 4 / 8 streams, 20 ms of sound (audiod) | 7.0 / 10.1 µs | 7.6 / 10.7 µs |
| `postMessage` of one Doom frame of sound (11 KB, copied) | 3.3 µs | 5.2 µs |
| Convert one Doom frame of sound from 16-bit to float | 3.8 µs | not run |

A Doom frame at 35 fps lasts 28,571 µs: the whole game path is about 0.15% of it. That is
why the kernel does not mix (it would add ~11 µs per frame on the main thread) and why
conversion happens on the audio thread.

Measured on the built system:

- **Doom in headless Chromium** (Freedoom 0.13, its demo, 15 s of presented frames timed in
  the page, 3 alternating runs each way):

  | | fps | median frame | 95th pct | 99th pct | sd | underruns |
  |---|---|---|---|---|---|---|
  | sound | 34.81 / 34.81 / 34.79 | 32.7 / 33.1 / 31.9 ms | 33.7 / 33.6 / 33.6 | 34.0 / 33.8 / 33.8 | 7.05 / 7.51 / 6.46 | 0 / 0 / 0 |
  | `-nosound` | 34.81 / 34.81 / 34.81 | 33.2 / 33.2 / 32.1 ms | 33.6 / 33.6 / 33.6 | 33.9 / 33.9 / 34.0 | 7.59 / 7.40 / 7.10 | |

  No difference beyond run-to-run noise.
- **Doom `-timedemo demo1` under Node** (as fast as it can): 1,779 / 1,683 / 1,730 fps with
  sound, 1,730 / 1,695 / 1,683 without. Sound work is tied to real time (the device takes
  only so much per second), not to the frame rate, so a faster game does not pay more.
- **Doom, 20 s in real time under Node**, user CPU of the whole Node process: 2.23 to 2.37 s with
  sound, 2.02 to 2.06 s without (3 runs each). Most of the difference is the stand-in's own
  10 ms clock, which exists only under Node; at 5 ms it was 2.42 to 2.50 s. In a browser
  that work is the worklet's, on the audio thread.
- **audiod**, two clients for about 1.3 s (one resampled from 24 kHz): 8 to 10 ms of CPU,
  down from 12.6 ms before its loop decoded each input frame once and dropped the division
  per sample.
- **Music** (DECISIONS.md 21, `oplplay` through audiod). Rendering inside UC.OS under Node:
  30 s of Freedoom's E1M1 in about 285 to 295 ms (the difference of 31 s and 1 s runs, 3
  each), about 10 ms of CPU per second of music, ~1% of the main thread; 10 s of a 9-voice
  chord in 82 to 109 ms. Doom on the desktop in headless Chromium, 3 alternating runs each
  way, 15 s of presented frames: with music 35.83 / 35.81 / 35.81 fps, 95th percentile
  33.7 / 33.7 / 34.1 ms, 99th 35.1 / 34.5 / 35.6 ms; with `-nomusic` 35.82 / 35.80 / 35.82
  fps, 33.7 / 33.6 / 33.6 ms, 34.0 / 35.0 / 33.9 ms; 0 underruns either way. The 99th
  percentile is about 0.8 ms higher on average with music, inside the spread between runs.

## 13. Decompression (libarc)

Output throughput in MB/s (10^6 bytes of decompressed data per second), on the same
laptop, single thread. "Native" is libarc compiled with the host `cc -O2`; the host
tools are GNU gzip 1.15, xz 5.8.4 (`-T1`), zstd 1.5.7.

Baseline, before any tuning (2026-11-26 06:28, load average 1.9), whole-process time,
best of 3, on the data member of Debian's `libllvm19` package (a 124 MB tar; xz as
shipped, gzip -6 and zstd -19 made from the same tar):

| Format | libarc native | host tool |
|---|---|---|
| gzip | 288 | 240 |
| xz | 60.5 | 101.5 |
| zstd | 231 | 486 |

After the decoders were tuned, and inside UC.OS: not measured yet (the machine stayed too
loaded for a valid benchmark on 2026-11-26).

How: `libarc/tests/arcbench.c` decompresses a file from memory several times and prints
the first (cold) run and the best run; it builds natively and for UC.OS, and
`libarc/tests/os-bench.sh BUILD FILE...` runs it inside UC.OS under Node.

## 14. How to measure properly

1. Put the timed loop in a `noinline` function and call it many times. V8 does not
   replace a running wasm loop with optimised code, so a loop entered once measures
   baseline code forever. `bin/kbench.c` is the minimal example; `bin/sysbench.c` is the
   full tool (`--json`, `--only NAME,NAME`), `bin/ubench.c` the user-code one.
2. Report the best batch and the first batch.
3. Make each batch much longer than the clock's resolution. Under Node the clock is fine;
   in Chrome it ticks every 0.1 ms and in Firefox about every 1 ms.
4. Compare A/B, never against a number from another day: `tools/bench/ab.mjs --a=TREE
   --b=TREE --mode=node|browser` runs two builds alternately and prints best, median and
   worst of each row. Keep the baseline in a separate copy of the tree.
5. Say where you measured (Node or which browser), on what machine, and how loaded it was.
6. The kernel forces a yield every 8 ms; a batch that spans one is a little slower. The
   best-batch figure hides that.
7. The other tools in `tools/bench/`: `callcost.mjs` (where a call's time goes),
   `clockcost.mjs` (the clock import), `spawncost.mjs` (process start, step by step),
   `frames-ab.mjs` and `present-check.mjs` (the screen), `boot-ab.mjs` (boot phases).
