# Performance

What has been measured, how, and what the numbers do and do not mean. Quote from this
file, with the caveats, or measure again. Do not round the caveats away.

## 1. System calls

Measured on one laptop (Intel i5-8250U) on 2026-11-26. The machine was heavily loaded
during most runs (several compilers running), so the UC.OS figures are pessimistic.

| Operation | UC.OS warm (Node 26) | UC.OS warm (Chromium 158) | UC.OS first cold batch | Native Linux, same laptop | unkernel (published) | Native Linux (unkernel's machine, published) |
|---|---|---|---|---|---|---|
| `getppid` | 11 to 13 ns | 14 to 18 ns | 100 to 400 ns | 884 ns | 172 ns | 110 ns |
| read 64 B from `/dev/zero` | 44 to 55 ns | 73 to 91 ns | about 900 ns | 1,005 ns | 172 ns | 161 ns |
| write 64 B to `/dev/null` | 21 to 24 ns | 22 to 28 ns | about 450 ns | 962 ns | | |
| pipe write + read, 64 B | 85 to 108 ns | 135 to 152 ns | about 1,000 ns | 2,016 ns | 482 ns | 417 ns |
| open + close | 139 to 176 ns | 177 to 217 ns | about 800 ns | 3,310 ns | | |
| `stat` | 125 to 150 ns | 140 to 182 ns | 600 to 1,100 ns | 1,559 ns | | |
| `clock_gettime` | 100 to 158 ns | 350 to 450 ns | about 1,000 ns | | | |
| start a process and wait | 71 to 85 us | 120 to 220 us | 1.3 to 1.6 ms | 251 us (fork), 627 us (spawn `/bin/true`) | 57 us (fork) | 73 us (fork) |

Other figures: the kernel is 37 KB and is up in 0.4 to 0.9 ms. A process that only
writes (`yes > /dev/null`) sustains about 27 million system calls per second in Chromium
while the desktop stays responsive.

### How to read this

- **Warm versus cold.** "Warm" is the best batch after the browser engine has optimised
  the code. Until then, calls cost 0.1 to 1.6 microseconds. A program that makes a call
  only a few times pays the cold price. Both columns are true; say which one you quote.
- **This is not a like-for-like comparison with Linux.** A UC.OS call is a function call
  with no privilege switch, and the kernel does far less per call: no permissions, no
  locks, no security checks. This laptop's native figures are also unusually slow
  (CPU vulnerability mitigations), so the unkernel machine's native column is the fairer
  reference for "native".
- **The unkernel and "its machine" columns are that project's published figures**, not
  measured here, on different hardware.
- **Where UC.OS is behind:** starting a process (each start instantiates a wasm module
  through the browser), and anything that needs the time (it comes from JavaScript).
- **User code speed has not been measured.** Programs run as compiled wasm, so they
  should be near what the browser gives any wasm program, but there is no number.
- The Chromium column is three runs of `sysbench --json` in headless Chromium 158 on the same laptop (load average about 3). Reading the clock costs about 400 ns there (it is a call into JavaScript), which is why the kernel only looks at it on every 256th system call: at every 64th, `getppid` measured 22 to 26 ns. The rows are timed on the process CPU clock (`CLOCK_PROCESS_CPUTIME_ID`), spawn on the wall clock. Chromium's clock ticks every
  0.1 ms. Firefox and Safari have not been measured.

## 2. Mechanisms, measured in isolation

Small experiments run before the design was fixed (Node 26, same laptop):

| Mechanism | Cost |
|---|---|
| direct wasm-to-wasm call into another instance | 4 to 7 ns |
| the same call routed through a JavaScript function | 47 to 65 ns |
| a blocking call: JSPI suspend plus resume | about 190 ns |
| copy 64 bytes between two memories with a wasm shim (called from JS) | 38 ns |
| the same copy through a JavaScript typed array | 129 ns |
| a countdown check in every loop iteration (rejected design) | +51% on an empty 1 ns loop |

The real system's `getppid` is higher than the 4 to 7 ns toy because it adds the
trampoline hop, the dispatch, counters, and the time-slice check every 256th call.

## 3. Graphics and programs

All from headless runs under Node at 1280x720 unless noted; reported by the component
authors.

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

## 4. Memory

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
kernel keeps the initrd (7.6 MiB, 5.5 of it fonts), the framebuffer (3.6 MiB) and two 512 KiB
buffers for each window's socket.

## 5. How to measure properly

1. Put the timed loop in a `noinline` function and call it many times. V8 does not
   replace a running wasm loop with optimised code, so a loop entered once measures
   baseline code forever. `bin/kbench.c` is the minimal example; `bin/sysbench.c` is the
   full tool (`--json` for machine-readable output).
2. Report the best batch and the first batch.
3. Make each batch much longer than the clock's resolution. Under Node the clock is
   fine; in Chrome it ticks every 0.1 ms and in Firefox about every 1 ms.
4. Say where you measured (Node or which browser), on what machine, and how loaded it was.
5. The kernel forces a yield every 8 ms; a batch that spans one is a little slower. The
   best-batch figure hides that.
6. Commands: `node tools/run.mjs --build=B --init=/bin/sysbench` and, in a browser,
   type `sysbench` in a terminal.
