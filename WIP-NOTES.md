# jit: machine code to WebAssembly translator, and the D versus C measurement

- **Branch / worktree:** `feat/binary-translator` in `.worktrees/jit` (done, read only now); current work: translator inside lx in `.worktrees/lxjit` (detached at main 3e5abe7, integrated tree staged)
- **Owner:** agent of uc-os-dev
- **State:** PAUSED 13:55 by uc-os-dev (gate). Translator inside lx works on real Debian programs; timings partly measured (below); nothing running.
- **Updated:** 2026-11-26 13:58
- **Next step:** on resume: finish the timings (second half of runs), Chromium runs, memory (step 5), region-size experiment for short commands, report
- **Waiting on:** nothing

## Done so far
- libjit/: own RV64GC decoder (I M A F D C Zicsr), wasm emitter, translator (block / dispatch / structured from the dominator tree with dispatch fallback, guest calls as wasm calls), trivial interpreter, prototype engine (region cache, table slots, tiering, lockstep checker), exact software FMA and directed rounding.
- Mechanism measured (Node 26, Chromium 158): compile+instantiate+first call of a 100 B module about 45 us (Node) / 65 us (Chromium); Liftoff code about 1.8x slower than optimised; no OSR: a loop entered once stays in baseline code.
- Correctness: every translated config ends in the same final state (exit, output, registers, memory) as the interpreter on the programs checked so far; lockstep finds no difference (full matrix running).
- FP soft-float: 6.5 million checks against x86 hardware in 4 rounding modes, 0 failures.

## FINAL: D (translated) against C (interpreted), inside UC.OS (Node 26), quiet machine
Measured 2026-11-26 08:20 to 10:00, load average 2.2 to 3.0, nothing else of mine running. C = lx, the core engine's final interpreter (built with its own Makefile from .worktrees/lx, run in my image). D = bin/rvjit (struct regions, calls, chaining, learned targets, masked bounds). MIPS = interpreter-counted guest instructions / time. C: best of 2 whole-process wall times (boot and start included, about 25 ms). D first run: translation and baseline tier included; D warm: best of runs 2 and 3 in the same engine. Whole command: wall of one full run of each.

| program | guest insns | C: lx | D first run | D warm | D/C first | D/C warm | whole command lx/rvjit |
|---|---|---|---|---|---|---|---|
| sieve | 247M | 151 | 870 | 1377 | 5.8x | 9.1x | 4.6x |
| sort | 166M | 182 | 739 | 1121 | 4.1x | 6.2x | 3.0x |
| sha256 | 277M | 304 | 2338 | 4923 | 7.7x | 16.2x | 4.9x |
| fib | 74M | 176 | 811 | 1443 | 4.6x | 8.2x | 2.7x |
| matmul | 89M | 163 | 847 | 2084 | 5.2x | 12.8x | 3.0x |
| strloop | 70M | 128 | 737 | 2412 | 5.8x | 18.8x | 3.2x |
| indirect | 133M | 156 | 333 | 634 | 2.1x | 4.1x | 1.8x |
| switch | 348M | 242 | 1873 | 3073 | 7.7x | 12.7x | 5.3x |
| hello (start-up) | 4K | 27.7 to 35.9 ms wall | | | | | rvjit 67 to 69 ms with 64 MiB guest memory, 30 to 32 ms with 4 MiB (boot + /bin/true alone: 25 ms) |

Standalone (Node) and Chromium tables: docs/BINARY-TRANSLATION.md sections 3.2 and 3.3 in the worktree. Ceiling (C compiled straight to wasm, Node): 1,650 to 10,900 MIPS on these programs; translated reaches 30 to 100 percent of it on loops, 15 to 45 percent on call/switch-heavy code. Chromium: translated code within about 20 percent of Node; interpreters 1.3 to 2.2x slower there.
Reproduce: `sh tools/jit/quiet-bench.sh build-NAME` (waits for load < 3 by itself).

## Documentation notes
(For uc-os-docs. Facts the shared documentation should state because of this stream. Nothing is integrated into the default boot path.)
- New, research code on branch `feat/binary-translator` (worktree `.worktrees/jit`), uncommitted:
  - `libjit/`: RISC-V RV64GC to WebAssembly translator and prototype engine (decoder, wasm emitter, structuring, interpreter, F/D software arithmetic). Builds natively, as standalone wasm32 and as a UC.OS program.
  - `bin/rvjit/` (only a `CFLAGS` file listing libjit sources; links with `--export-table --growable-table`): runs a static RV64 ELF in UC.OS. Needs the host extension.
  - `boot/jit-proto/ext.mjs`, `host-ext.mjs`, `jit.html`: host extension through `boot({ extend })`, adds the program import `jit.load(ptr, len)`; boot/host.mjs unchanged.
  - `tools/jit/`: `jit.mk` (builds; `make -f tools/jit/jit.mk B=build-NAME`), `run.mjs`, `bench.mjs`, `bench.html`, `check.mjs`, `chrome.mjs`, `mech.mjs`/`mech.html`, `xltest.c`, `validate.mjs`, `fptest.c`, `ucos-run.mjs`, guest test programs in `tools/jit/guest/`.
  - `docs/BINARY-TRANSLATION.md`: the stream's own design document (all numbers and methods there).
- Edit already made to `docs/STATUS.md` (section "Not built", the Linux programs bullet): mentions the translator as research code. Left as is per the new rule; uc-os-docs to reconcile.
- Final D vs C table (top of this file) and docs/BINARY-TRANSLATION.md sections 1 and 3: the facts for PERFORMANCE.md about Linux binaries under translation.
- Facts for PERFORMANCE.md (measured 2026-11-26, i5-8250U, load about 2; `node tools/jit/mech.mjs`, Chromium via `node tools/jit/chrome.mjs --page=tools/jit/mech.html`):
  - compile + instantiate + first call of a 105-byte generated module: about 45 us Node 26, 65 us Chromium 158; 1.3 MiB module: `new WebAssembly.Module` 2.2 ms (Node) / 2.9 ms (Chromium), first call of every function 15 / 19 ms (lazy Liftoff compile, about 70 to 90 MB/s).
  - Chromium refuses synchronous `new WebAssembly.Module` above 8 MB on the main thread (confirmed with a 13 MiB module).
  - V8 baseline (Liftoff) code about 1.8x slower than optimised for i64 loop code; optimised code used from about the second call after 1 ms of execution; a loop entered once stays in baseline code (no OSR), confirmed again.
- DEVELOPMENT.md candidates: the translator's test commands (section 10 of BINARY-TRANSLATION.md); guest programs need `ld.lld` and the riscv64 target of clang (both present).
- DECISIONS.md candidates (assumptions made, for owners): run-time translation of hot regions as the design continued; guest address space below 4 GiB in the engine memory; default bounds mode "masked"; translate threshold 50 block entries; fflags not computed. Details in BINARY-TRANSLATION.md sections 4, 5, 9.

- (lxjit, afternoon) For LINUX-BINARIES.md / STATUS.md when the translator is merged: lx has a switch `--jit=off|on|lockstep` / `LX_JIT` (default off), options `--jit-opts` / `LX_JIT_OPTS`; files libjit/jitcore.{c,h}, bin/lx/jit.c; hooks in bin/lx rv_interp.c (heat counter, LX_STOP_JIT, rv_flush_code), main.c, arch.h, rv_arch.c, sys_proc.c (__fork_child), sys.c (stats), CFLAGS (libjit sources, -DJIT_RENAME, --export-table --growable-table). New program bin/jtime.c (wall time of a command). Host side only through boot/jit-proto/ext.mjs; build-patches/host-jit.patch (not applied) is the permanent form. Verified: see the 12:05 and 13:55 log entries; numbers in docs/BINARY-TRANSLATION.md section 11 (worktree lxjit).

## Needs from others
- lx: a way to run a static RV64 ELF under their optimised interpreter and an instruction count, for case (2). Their CPU state layout and memory access API (registers must be addressable at fixed offsets by generated code).

## Assumptions and questions for the owners
- Assumed (reversible): guest address space below 4 GiB inside the engine's memory, guest base as a constant offset; default bounds mode "masked" (cannot leave guest memory, faults not precise). Alternatives: unchecked (fastest, wrong addresses trap or alias), checked (precise SIGSEGV, slower). Numbers to come.
- fflags (FP exception flags) are not computed, in translated code or in my interpreter. Documented deviation.

## Preliminary numbers (superseded by FINAL above; Node 26, standalone engine, load 5 to 9)
Guest MIPS = instructions counted by the interpreter / time; best (median) of 2 rounds x 5 runs; first run includes translation.
(1) = my trivial interpreter, (3) = translated, structured + calls (JIT), also AOT, (5) = same C compiled to wasm, (4) native x86 -O2. (2) not yet measured.

| program | insns | (1) trivial interp | (3) block | (3) dispatch | (3) struct | (3) struct+calls | (3) AOT | (5) wasm direct | (4) native |
|---|---|---|---|---|---|---|---|---|---|
| sieve | 247M | 37 | 445 | 584 | 1265 | 1401 | 2222 | 2510 | 2559 |
| sort | 166M | 19 | 249 | 648 | 1118 | 884 | 1166 | 1659 | 1635 |
| sha256 | 277M | 27 | 4300 | 2834 | 4428 | 4537 | 4518 | 8237 | 7372 |
| fib | 74M | 22 | 252 | 454 | 478 | 1321 | 1617 | 8053 | 8438 |
| matmul | 89M | 20 | 1609 | 613 | 2176 | 1630 | 1835 | 4184 | 8379 |
| strloop | 70M | 28 | 215 | 1278 | 2192 | 2755 | 2396 | 3590 | 5634 |
| indirect | 133M | 28 | 210 | 289 | 286 | 424 | 398 | 1370 | 1102 |
| switch | 348M | 20 | 246 | 626 | 643 | 594 | 595 | 6896 | 5093 |

Earlier, on a quiet machine (load 1.4): fib interp 43, struct+calls 1855, wasm direct 10161; switch interp 31, struct+calls 859.

## Log
### 2026-11-26 07:02
Phase 1 working end to end under Node. Preliminary table above. Indirect jumps and calls exit to the driver today, which is why indirect and switch lag; region chaining by wasm tail calls is being built. Translator output validates for all 18 strategy/bounds/call combinations.
### 2026-11-26 07:12
Region chaining by wasm tail calls (return_call_indirect) and learned indirect-jump targets (retranslate a region with the targets seen so far as in-region branches) built. Preliminary, Node, load 7 to 9: switch best 480 ms -> 137 ms (348M insns: about 700 -> 2500 MIPS); indirect 520 -> 308 ms. Found by reading: a translator bug (an empty block when the instruction limit is hit falls into itself: endless loop), fixed, not yet re-tested. Machine-load rules from the coordinator now followed: nothing started while loadavg > 5, nice 19, check runs split under 60 s. Final D vs C table waits for load < 3.
### 2026-11-26 07:50
- Case (2) now possible: lx (core engine, snapshot of `.worktrees/lx/bin/lx` copied 07:3x into build-jit/jit/lx-src, built by me) runs my test ELFs with the right checksums, natively and as a UC.OS program.
- Preliminary (load 4.7 to 5): lx native: fib 74M insns 0.43 s (172 MIPS), sieve 247M 1.14 s (216 MIPS). lx inside UC.OS under Node (my build against this tree's libc): fib 1.66 to 1.71 s wall including boot (about 45 MIPS). rvjit inside UC.OS (translated): fib 100 to 102 ms warm, 180 ms first run (about 730 MIPS warm), same final state as the standalone engine.
- Finding for the lx stream (not verified on their side): their wasm build ran about 4x slower than their native build here; their file says the two are about equal. Possible causes: build flags, their libc changes (lib/umem.s) missing in my tree, or the run loop staying in baseline code (no OSR). Worth checking before the owners compare C and D.
- Translator: CPU state addresses are now parameters (`xlat_env`), so the translator can target lx's `struct rv_cpu` (pc 264, f 272, fcsr 528) without code changes. Fixed the empty-block loop; short check runs pass (fib-gc, fpmix-gc, 5 configs incl. lockstep).
### 2026-11-26 08:40 (quiet machine, load 2.6 to 3.0, final method)
sieve (247M guest instructions):
- Node, standalone engine, MIPS best (median): trivial interp 44 (44), predecode interp 95 (95), block 982, dispatch 696, struct 1199, struct+calls 1390 (1281), aot 2078 (1979), C compiled to wasm 3047 (2853), native 3285 to 4172.
- Chromium 158: trivial interp 20, predecode 46, aot 2491 (2287), wasm direct 3770 (struct+calls lost to a harness bug, fixed).
- Inside UC.OS under Node, one boot per run: rvjit translated 218 ms warm / 465 ms first run (1131 / 530 MIPS); lx (core engine interpreter, case 2) 3.47 s wall incl. boot (71 MIPS); rvjit trivial interp 10.1 s; predecode 2.95 s. D vs C (lx) on sieve: 16x warm, 7.5x first run.
### 2026-11-26 09:25 (disk and /tmp)
- Removed my single files from the shared scratchpad (test ELFs, copies of headers, a mutated rvfp.c, small binaries; under 1 MB total).
- Left behind, directories I would like removed (the owner's call, never rm -rf by me): /tmp/ucos-jit-* (7 Chromium profiles from tools/jit/chrome.mjs, about 3 MB each, 21 MB total). chrome.mjs now puts profiles in build-chrome-profiles/ inside the worktree.
- On the real disk in my worktree: build-jit/ (builds, images, results; about 60 MB), build-chrome-profiles/ (new).
### 2026-11-26 10:05
Final D vs C table measured on a quiet machine (see FINAL at the top). Bug found by the run and fixed: dropping a translation (learned-target retranslation, fence.i, frm change) called a wrong table slot (the support module wrote only its first slot); only the calls=0 configurations on indirect and switch hit it; rerun clean. Fork/migrate: ecall now flushes all registers; engine_after_copy() re-creates translations lazily in a cloned or migrated instance (docs/BINARY-TRANSLATION.md 4.10). Host must give cloned instances the jit.load import too.
### 2026-11-26 10:50
- Bounds and budget (Node, quiet, struct+calls, best warm ms, two rounds): sieve masked 169/153, unchecked 160/157, checked 151/169, no budget checks 71/86; sort 132/127, 111/123, 127/143, 94/98; fib 48/48, 44/44, 52/51, 42/49; strloop 24/24, 26/26, 29/31, 23/23. Budget checks cost up to 2x on the tightest loop; bounds modes differ by 0 to 15 percent. Re-entry on budget (yield_exit=1) no gain.
- Correctness sweep after the last code change: 20 programs x 12 configs (incl. lockstep, copytest, tiny regions, cache bound) = 240 runs, 0 failed.
- No background jobs of mine left running. Fork call numbers noted as 520/521/522.
### 2026-11-26 11:10 (step 1 of the integration, .worktrees/lxjit)
- libjit/jitcore.{c,h}: the translator as an engine component (plain-C interface). bin/lx/jit.c: lx's side. Hooks in bin/lx: a heat counter and a stop at block entry (rv_interp.c), LX_STOP_JIT in the main loop, flush_code also drops translations, frm writes notify, __fork_child calls the after-copy path, stats line. lx links with the translator (270 KB). Switch: --jit=off|on|lockstep or LX_JIT (default off); options --jit-opts / LX_JIT_OPTS.
- Host: boot/jit-proto/ext.mjs now hands the loader to the engine through a slot in its own table right after the instance is created (no import, so the same lx binary interprets without the extension). Permanent form written as build-patches/host-jit.patch (17 added lines in instantiate()), NOT applied.
- Translator default inside lx: bounds=checked (faults where the interpreter faults; the window rule a - g_lo <= g_span).
### 2026-11-26 12:05 (step 2, correctness on real programs)
- Debian programs under lx inside UC.OS (Node), interpreter vs translator vs lockstep: hello, M2 (busybox sh, 37 checks: 37/37), M34 (dash, coreutils, bash: 10/10), mawk (2 programs), dash pipelines and command substitution, ls -lR, sort -k2n, sha256sum of perl and bash, perl -e (hash, atan2), gzip -9 / -d round trip of bash (cmp equal), busybox xz -dc of a real Packages.xz (sha256 compared), dpkg-deb --info, bash associative arrays. All 117 output lines byte-identical in the three modes; lockstep: 0 differences.
- Bugs found and fixed on the way: (1) lockstep yield did not make translated code leave at back edges, (2) translator defaults overrode user options, (3) a callee reached from translated code was never counted as hot (the interpreter's first block of a run was not counted): perl made 10 M lookups, all missed; after the fix 2,488 missed.
- Not an engine difference: `dpkg-deb -c` hangs under the interpreter too with lx's default emulated fork (parent and child cannot run at once); to be retried with the real clone build.
- Rough, load 4.4 to 4.9, one run each: perl sum loop whole command 3.6 to 3.7 s interpreter, 2.8 to 3.1 s translator. Pending proper timing.
### 2026-11-26 13:55 PAUSED (uc-os-dev, gate)
Paused in the middle of the whole-command timings (tools/jit/timing.sh, quiet lock released, loop stopped). Nothing of mine is running.
State summary (translator inside the core engine, .worktrees/lxjit, nothing committed):
- Runs translated, output byte-identical to the interpreter (and lockstep 0 differences where run): Debian hello, mawk, dash pipelines and command substitution, coreutils (ls -lR, sort, sha256sum, wc, cmp, cp), perl -e, gzip round trip, busybox xz -d, dpkg-deb --info, bash, M2 37/37, M34 10/10; the 17 maintainer-script reinstalls (all exit 0, /etc files identical); with the real-clone build (-DLX_HAVE_PCLONE): forkt 10/10, a bash loop forking 100 subshells, exec from bash, dpkg-deb -c, signal traps (TERM to self, CHLD); kernel INT/TERM traps do not run in either mode (known lx limitation).
- Differences from the interpreter found: none in output. One false alarm (gzip header timestamp of a freshly created input file). Three bugs found and fixed in my code: lockstep yield, option order, callees from translated code never counted hot.
- Partial timings, inside UC.OS under Node, whole command (jtime: spawn to exit), 8 runs per side for the first four, 4 for the rest, load 2.7 to 3.4 except where noted, best (median) ms: hello 6.2 (9.2) interpreter / 16.8 (24.4) translator / 12.0 (13.4) translator threshold 500; busybox sh -c true 4.8 / 7.1 / 5.2; dpkg -l 64 / 141 / 94; ls -lR /usr 342 / 601 / 647 (load up to 4.7); sha256sum 10 MB 4064 / 217 / 226 (load rose to 17.5 at the end of that boot: recheck); gzip -n -c 7.5 MB 5781 / 823 / 762; busybox xz -dc 2857 / 343 / 362; perl loop 2028 / 557 / 478; sort -n 100,000 lines 1733 / 745 / 972; mawk same file 1597 / 415 / 442; apt reinstall debianutils (scripts under lx) 1609 / 3217 / 2266. Translation wins 2 to 19x on computation and loses on short, start-up dominated commands and on fork-heavy work (forked children re-translate: the after-copy policy).
- Left: second half of the timing runs, Chromium, memory footprint (step 5), smaller regions for short commands, the report.
- 2026-11-26 13:58 read the opt-engines finding (Chromium renderer: indirect branch speculation force-disabled; br_table and varying call_indirect mispredict). Plan on resume: timings in Chromium first; review indirect calls/jumps/chaining for varying targets (per-target call_indirect sites from learned targets), measure both engines, keep only what is no slower in either.
- 2026-11-26 14:00 while paused, code only (not built, not run): forked children drop decoded blocks too (translate only what gets hot in them); indirect guest calls now learn their targets, and option split=1 (default off until measured in Node and Chromium) gives each known target its own call_indirect with a constant slot; Chromium page and driver for the timings (tools/jit/os/page.html, tools/jit/timing-chrome.sh, persistent profile under build-chrome-profiles/).
- 2026-11-26 14:41 PAUSED again (uc-os-dev, pull request builds before the demo). Stopped before the first Chromium timing run (the Chromium store install had finished: 45 packages, 72 s). Nothing of mine running.
- 2026-11-26 20:03 released bench.lock on uc-os-dev's request (Chromium timing driver stopped before the sha256,xz group). Chromium group 1 (hello, busybox sh -c true, dpkg -l; 16 runs per side, control included) and lsR (4 per side) measured. No lock until c9q/build-vfy-log/log.txt says done.
### 2026-11-26 20:07 (time corrected; first written by hand as 20:30)
- Chromium (headless, sandboxed renderer), whole command, 16 runs per side, ABBA with a control (the interpreter twice): hello 5.5 / 5.4 ms best (interpreter, control) against 17.8 translated and 7.2 at threshold 500; busybox sh -c true 6.0 / 6.3 against 7.8 and 7.0; dpkg -l 99 / 84 against 400 and 156. The control differs by up to 15% at the best: smaller differences are not findings. Translation overhead on short commands is larger in Chromium than under Node.
- Region size (Node, single runs, indicative): ls -lR /usr default regions 500 regions 20.2 MB wasm 3.2 s peak RSS 336 MiB; maxinsns=128 maxblocks=32: 675 regions 5.3 MB 1.24 s 233 MiB; interpreter 485 ms 208 MiB. Output identical with small regions.
- Cache bound: perl loop with cache=1024 KiB thrashes (7,264 regions re-translated, 8.6 s); with small regions it fits (707 regions, 1.65 s). Output correct.
- Final timing sessions queued (tools/jit/timing*.sh): they wait for uc-os-dev's verification log to say done, take the lock for at most one page or boot at a time, with 60 s gaps.
- 2026-11-26 20:10 the Chromium page 'sha256,xz' timed 20:08 to 20:10 is DISTURBED (uc-os-dev ran a job in that window): excluded from results, to be repeated after the queued sessions.
### 2026-11-26 21:14 FINAL NOTES (laptop shutting down; all my runs stopped, no lock held)
State: libjit linked into bin/lx in .worktrees/lxjit (uncommitted). Switch --jit=off|on|lockstep or LX_JIT (default OFF = interpreter only); --jit-opts / LX_JIT_OPTS (threshold, bounds [default checked inside lx], strategy, calls, chain, learn, ic, split, abi, fp, cache KiB, maxblocks, maxinsns, verbose). Host: boot/jit-proto/ext.mjs hands the loader through a table slot (jit_attach) after each instance is created; without it lx interprets. Permanent form: build-patches/host-jit.patch (not applied).
Passes (interpreter vs translator vs lockstep, byte-identical output): real-program script 113 lines (hello, mawk x2, dash, ls -lR, sort, sha256sum, perl, gzip round trip, busybox xz -d, dpkg-deb --info, bash) all 3 modes, lockstep 0 differences; M2 37/37 and M34 10/10 (also in lockstep); the timed commands' outputs (ls -lR /usr, dpkg -l, sort/mawk 100k lines, sha256 10 MB, gzip -n, xz -dc, perl) identical; 17/17 maintainer-script reinstalls exit 0 with identical /etc results; with -DLX_HAVE_PCLONE: forkt 10/10, 100 forks, exec, dpkg-deb -c, TERM/CHLD traps identical in both modes. split=1 and small regions (maxinsns=128 maxblocks=32): identical output.
Timings, whole command (jtime, spawn to exit), ms best / q1 / median (n); off2 = interpreter again (control); on500 threshold 500; onsmall maxinsns=128 maxblocks=32; onsmall500 both; onsplit split=1; on2 = translator again (control). The Chromium page 'sha256,xz' at 20:08 was disturbed and is excluded (not repeated yet).
Chromium:
| test | off best / q1 / median (n) | off2 best / q1 / median (n) | on best / q1 / median (n) | on500 best / q1 / median (n) | onsmall best / q1 / median (n) | onsmall500 best / q1 / median (n) | onsplit best / q1 / median (n) | on2 best / q1 / median (n) | load |
|---|---|---|---|---|---|---|---|---|---|
| hello | 4.6 / 5.7 / 7.5 (24) | 4.7 / 4.9 / 6.6 (24) | 17.8 / 20.0 / 26.2 (16) | 7.2 / 9.2 / 15.7 (16) | 12.7 / 13.6 / 23.2 (8) | 5.6 / 5.7 / 6.4 (8) |  |  | 2.82 to 4.58 |
| bbtrue | 5.8 / 6.1 / 6.7 (24) | 5.8 / 6.3 / 6.8 (24) | 7.8 / 9.7 / 11.7 (16) | 7.0 / 8.6 / 9.0 (16) | 7.2 / 7.9 / 10.2 (8) | 5.6 / 6.0 / 7.3 (8) |  |  | 2.82 to 4.58 |
| dpkgl | 62.1 / 72.7 / 133.5 (24) | 65.0 / 83.9 / 98.6 (24) | 399.6 / 1038.7 / 1189.2 (16) | 156.2 / 166.5 / 212.5 (16) | 246.3 / 290.8 / 303.8 (8) | 91.0 / 99.0 / 124.9 (8) |  |  | 2.82 to 4.58 |
| lsR | 364.2 / 379.1 / 397.6 (16) | 350.0 / 401.1 / 464.4 (16) | 1327.3 / 2120.2 / 3512.3 (8) |  | 411.4 / 446.1 / 888.0 (8) | 289.7 / 304.4 / 383.4 (8) |  |  | 2.69 to 5.13 |
| perl | 5345.7 / 5418.2 / 5540.2 (8) |  | 999.0 / 1189.0 / 1963.0 (12) |  | 882.1 / 882.1 / 939.9 (4) |  | 1097.8 / 1106.3 / 2019.1 (8) | 961.7 / 1049.9 / 1090.5 (8) | 1.39 to 4.37 |
| mawk | 2526.1 / 2526.1 / 2788.8 (4) |  | 1034.2 / 1034.2 / 1117.1 (4) |  |  |  |  |  | 1.39 to 3.48 |
| sort | 4716.2 / 4792.1 / 5246.4 (8) |  | 1087.0 / 1087.0 / 1557.1 (4) |  | 914.4 / 914.4 / 1074.2 (4) |  |  |  | 1.79 to 2.45 |
| gzip | 19462.0 / 19462.0 / 19588.5 (4) |  | 940.3 / 940.3 / 972.1 (4) |  |  |  |  |  | 1.79 to 2.45 |
| apt | 11.7 / 11.7 / 13.2 (4) 3 failed |  | 12.0 / 12.0 / 15.5 (4) 3 failed |  |  |  |  |  | 2.91 to 3.52 |
| sha256 | 8145.7 / 8145.7 / 8277.7 (4) |  |  |  | 410.1 / 410.1 / 426.2 (4) |  |  |  | 1.99 to 2.24 |
Node (final session, cut off after dpkgl; earlier partial Node table of 13:55 in the 13:55 entry, without control):
| test | off best / q1 / median (n) | off2 best / q1 / median (n) | on best / q1 / median (n) | on500 best / q1 / median (n) | onsmall best / q1 / median (n) | onsmall500 best / q1 / median (n) | load |
|---|---|---|---|---|---|---|---|
| hello | 4.2 / 4.8 / 5.4 (16) | 4.3 / 4.8 / 5.3 (16) | 9.5 / 10.8 / 12.5 (16) | 5.8 / 6.1 / 6.9 (16) | 9.2 / 10.0 / 11.4 (16) | 5.2 / 6.0 / 6.7 (16) | 2.38 to 2.38 |
| bbtrue | 6.5 / 8.9 / 9.8 (16) | 8.5 / 9.1 / 9.9 (16) | 10.3 / 12.6 / 13.2 (16) | 9.5 / 9.8 / 11.9 (16) | 11.2 / 12.2 / 14.6 (16) | 9.3 / 10.0 / 11.1 (16) | 2.81 to 4.11 |
| dpkgl | 39.3 / 51.0 / 67.1 (16) | 40.0 / 45.2 / 52.4 (16) | 101.9 / 121.2 / 161.1 (16) | 54.4 / 64.8 / 74.4 (16) | 85.5 / 96.1 / 115.5 (16) | 60.4 / 67.8 / 84.5 (16) | 2.88 to 3.30 |
Files outside bin/lx the work needs: libjit/ (all; new jitcore.c/h), bin/jtime.c, boot/jit-proto/ext.mjs (+ host-ext.mjs, jit.html from before), tools/jit/ (ucos-run.mjs, chrome.mjs, timing.sh, timing-chrome.sh, timing-sum.mjs, os/*.sh, os/page.html, plus the earlier prototype tools), docs/BINARY-TRANSLATION.md (section 11 added), build-patches/host-jit.patch (in build-patches/, git-ignored: copy it if needed). bin/lx changes: CFLAGS, arch.h, main.c, rv_interp.c, rv_arch.c, sys.c, sys_proc.c, new jit.c.
Half done: nothing broken in the tree. Not done: Node timings after dpkgl (lsR, sha256, gzip, xz, perl, sort, mawk, apt, perl split control); the disturbed Chromium sha256,xz page; apt reinstall in Chromium fails after its first run (exit 100, not traced); fork-heavy timing after the child-cold change; bin/lx is based on the integrated tree (main 6f121c3 lacks the stat/utimensat fixes, no conflict with my hooks).
Remaining, in order: 1 finish Node timings with control; 2 repeat Chromium sha256,xz; 3 decide defaults with numbers (small regions + threshold 500 look best for short commands, no worse for long ones in the runs so far); 4 fork-heavy and apt timings; 5 report to uc-os-dev via SendMessage (not yet sent).
