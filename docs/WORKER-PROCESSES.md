# Worker processes: design study

The missing half of decision 5 (the "hybrid" process model): how a UC.OS process could run
off the page's main thread. This file lays out the options with facts and measurements so
that the owners can choose. It makes no recommendation. Written 2026-11-26 on branch
`feat/worker-processes`; nothing here is integrated, the default path is unchanged.

Contents: 1 the problem, 2 what was built to find out, 3 the approaches (A, B, C, D and
others), 4 measurements, 5 findings that apply to every Worker approach, 6 fork and several
cores, 7 open decisions for the owners, 8 what was not verified, 9 how to reproduce, 10 sources.

## 1. The problem

Nothing can interrupt running WebAssembly on a thread. The kernel takes the CPU back only
at system call boundaries (ARCHITECTURE.md section 4). A program that computes without
system calls (a tight C loop, `bc` on a huge number) therefore freezes the whole tab: no
input, no screen updates, no way to kill it. Measured here in headless Chromium: a 1.5 s
pure loop in an ordinary program stops every animation frame for 1.46 to 1.73 s; in the real
desktop a 3 s loop froze the page for 2.8 to 3.6 s.

Two more gaps a Worker path could close: using more than one core, and real preemption.

## 2. What was built to find out

All of it is separate from the default build and off by default.

| Piece | What it does |
|---|---|
| `boot/host.mjs` | one new option, `extend` (default `null`): a function that receives the kernel's host imports and returns the ones to use. 5 lines. With `extend` unset nothing changes. |
| `boot/worker-proto/ext.mjs` | the main-thread side. Wraps the host imports `spawn`, `wake`, `reap`. A program of the right kind is started in its own Worker instead of on the main thread; its system calls are served on the main thread by the real kernel. The kernel is not changed and does not know. |
| `boot/worker-proto/worker.mjs` | the Worker side, one process per Worker, approach A or B. |
| `boot/worker-proto/marshal.mjs` | approach B's table of the bytes each system call reads. |
| `boot/worker-proto/wasm.mjs` | builds the copy-shim variants; tells A, B and ordinary programs apart from the raw bytes. |
| `boot/worker-proto/test.html`, `test.mjs` | browser test page: runs a scenario, measures animation-frame gaps per phase. |
| `boot/worker-proto/desktop.html`, `host-ext.mjs` | the real desktop with the extension on: an import map points `host.mjs` at a wrapper, so `page.mjs` runs unchanged. |
| `boot/worker-proto/d.html`, `d.mjs`, `d-worker.mjs` | approach D: the whole machine in one Worker, an OffscreenCanvas, a heartbeat and a watchdog. |
| `boot/worker-proto/terminate.*` | measures how long `Worker.terminate()` takes to stop a computing Worker. |
| `tools/worker-proto/proto.mk` | builds the variants by re-running the default Makefile into other directories with other `CC`/`ULDFLAGS`. No default rule changed. |
| `tools/worker-proto/wproto.c` | the test program (system calls, benchmarks, pure loops, start-up and kill timing). |
| `tools/worker-proto/yield-cc.sh`, `yield-rt.s`, `yield-slow.c` | approach C with clang only. |
| `tools/worker-proto/mark.c` | approach B's mark: an export named `ucos_worker`. |
| `tools/worker-proto.mjs` | runner: under Node like `tools/run.mjs`; with `--browser`, its own static server (with or without the two isolation headers) and headless Chromium. |

No file in `kernel/` or `libc/` was changed.

How a program is chosen for a Worker in the prototype (one possible answer to an open
question, section 7): a module whose own memory is shared goes to approach A; a module that
exports `ucos_worker` goes to approach B; anything else takes the default path. The check is
a walk over the module's sections, before compiling.

## 3. The approaches

Each one is described by the same questions: how a system call travels, how the kernel
reaches the process's memory, blocking and wake-up, kill, file descriptors and `spawn`,
what the browser and the hosting must provide, what changes in the build, costs, risks.
Numbers are in section 4.

### A. Shared memory (prototyped, works)

- **System call.** The program is built with a shared memory. In its Worker, `host.syscall`
  is a JavaScript function that writes the call number and six arguments into a 64-byte
  SharedArrayBuffer, sets a state word, `Atomics.notify`, and sleeps in `Atomics.wait`. The
  main thread waits with `Atomics.waitAsync` (it may not block), runs
  `kernel.syscall_pid(pid, ...)` and wakes the Worker. Optional: either side spins a little
  before sleeping, which turns 12 to 20 us into 0.4 to 2.3 us per call.
- **Memory.** The main thread instantiates a copy shim (the same three functions as the
  default `SHIM`) that imports the process's shared memory. The kernel's `copy_from_user`
  and `copy_to_user` work unchanged, directly on the program's memory: no copying beyond
  the one the kernel does anyway. Memory growth in the Worker is seen by the shim
  (verified: a 32 MiB `malloc` after start, then reads and writes at its top).
- **Blocking.** A call returns -ERESTARTSYS; the libc calls `host.block`, which is another
  request through the same control block. The main thread parks the process exactly as
  `host.mjs` does (timeouts, yields, `wake`) and answers when it may run again, or that it
  is dead (then the Worker throws and unwinds). See section 5 for the one subtle point.
- **Kill.** The kernel's kill path calls `host.wake(pid)`. The extension sees the process
  is no longer running and calls `Worker.terminate()`. The process is a zombie for the
  kernel at once (parent reaps it within 0.5 to 2.9 ms), but see section 5: Chromium lets the
  thread run for about 2 more seconds.
- **File descriptors, spawn.** Nothing changes: they live in the kernel on the main thread.
  `spawn` from inside a Worker process works (tested: `/bin/echo` through a pipe), and the
  child can be ordinary or another Worker process.
- **Browser and hosting.** SharedArrayBuffer and shared WebAssembly memory exist only on a
  cross-origin isolated page: the document must be served with
  `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`
  (or `credentialless`). Also needed: Workers, `Atomics.waitAsync` on the main thread.
  What the headers constrain:
  - COEP `require-corp` blocks cross-origin subresources that do not opt in with CORS or a
    `Cross-Origin-Resource-Policy` header. UC.OS loads only same-origin files today; the
    page's `?base=URL` option would need CORS on the other origin.
  - COOP `same-origin` cuts the link between the page and windows it opens or that opened
    it. UC.OS uses none.
  - Static hosts. GitHub Pages cannot send custom headers (GitHub community discussion
    13309). GitLab.com Pages has no per-project headers (gitlab-pages issue 50; self-managed
    instances can set them server-wide). Cloudflare Pages documents a `_headers` file.
    Netlify (`_headers`, `netlify.toml`), Vercel (`vercel.json`), Firebase Hosting
    (`firebase.json`) and S3 with CloudFront (response-headers policy) document custom
    headers; none of these hosts was tested here. Any server we configure ourselves
    (nginx, Caddy) can send them.
  - The `coi-serviceworker` workaround: a same-origin script registers a service worker
    that adds the two headers to every response. Costs, from its README: the page reloads
    once on the first visit; it needs HTTPS or localhost; the file must be served from our
    origin (not bundled, not from a CDN), next to the page or above it; by default it uses
    COEP `credentialless`. It is third-party code (MIT); rule 3 of CLAUDE.md would need an
    owner's yes, or we write the 30-odd lines ourselves. Not tested here.
- **Build.** Every program that runs this way, and the libc it links, must be compiled
  with `-matomics -mbulk-memory` and linked with `--shared-memory --max-memory=N` (a shared
  memory must declare a maximum; the prototype uses 1 GiB). `proto.mk` builds the whole
  tree this way, Doom included, with no source change. It is a second build of the libc
  and of each program; one binary cannot be both.
- **Costs.** 12 to 20 us per call in Chromium without spinning, 0.55 to 2.3 us with;
  14 to 28 ms to start (Worker creation dominates); 2.8 to 2.9 MiB per process; calls wait
  for the main thread (8.7 ms per call while a main-thread process is CPU-bound).
- **Risks.** Hosting depends on headers (above). tombl/linux keeps each process's memory
  inside its own Worker because, in its words, passing Memory objects between workers
  "exposes browser-specific wrapper refresh and lifetime behavior, especially after memory
  growth"; growth worked here in Chromium and Node but Firefox and Safari were not tried.
  Once a process has threads, another thread could change a buffer while the kernel copies
  it; with one thread per process, as now, the process is asleep during its own call.

### B. No shared memory (prototyped, works)

- **System call.** An ordinary build, unchanged code, plus the `ucos_worker` mark. In the
  Worker, `host.syscall` is a `WebAssembly.Suspending` (JSPI) function: it gathers the
  bytes the call will read (the marshalling table), posts one message, suspends the wasm
  stack, and on the reply writes back the bytes the kernel wrote and returns the result.
- **Memory.** The main thread keeps a **proxy memory** per process: an ordinary
  WebAssembly.Memory grown to the same size as the process's memory (the size travels with
  every call), into which the sent bytes are written at their real addresses. The kernel's
  shim reads that memory. The shim's `copy_to_user` half also calls a small import that
  records each range the kernel wrote; only those ranges travel back. So only reads need a
  table. Untouched proxy pages are not committed by the OS.
- **The table** (`marshal.mjs`): 15 entries covering 17 of the 51 call numbers the kernel
  implements: `write pwrite64 openat newfstatat mkdirat unlinkat utimensat chdir renameat
  nanosleep ppoll bind connect spawn` and `ioctl` for `TCSETS/TCSETSW/TCSETSF TIOCSWINSZ
  TIOCSPGRP`. `spawn` is the only deep one (argv and envp are arrays of string pointers).
  77 lines with comments. A checking mode makes the shim's read half report any kernel read
  outside what was sent (a NUL inside the sent part ends a string read). With every process
  of the C library, kernel and shell suites running in Workers in checking mode, it reported
  0 misses; with `write` and `openat` deliberately left out it reported each miss and the
  console received NUL bytes, which is what a table bug does silently without the check.
  Every new system call or ioctl that reads user memory needs an entry.
- **Blocking.** As in A: the main thread parks the process when the call returns
  -ERESTARTSYS and later posts "run again" or "dead"; the Worker's `host.block` (also JSPI)
  waits for that message.
- **Kill.** As in A (`Worker.terminate()`, same 2 s in Chromium).
- **File descriptors, spawn.** As in A.
- **Browser and hosting.** No special headers (verified with the headers off: `crossOriginIsolated=false`,
  `SharedArrayBuffer` undefined, B still works). Needs JSPI inside Workers.
- **Build.** None for the program beyond the mark (or any other way of choosing). The same
  binary could run either way.
- **Costs.** 46 to 122 us per call in Chromium, 43 to 60 us under Node (two message hops
  and a JSPI suspend each call); 14 to 36 ms to start; 3.9 to 4.0 MiB per process.
- **Risks.** The table must stay complete (a miss is silently wrong data, not an error).
  Each call allocates messages; during B phases the main thread's frame gaps and timer
  lateness were worse than in A phases (frames 24 to 70 ms against 17 to 37 ms; a 10 ms
  sleeper woke up to 28 to 50 ms late against 1 to 5 ms); the cause was not established.
  In Chromium the first 100 ms or so of a B process ran 3 to 10 times slower than later
  (54 to 197 million loop iterations per second against 450 to 600); not investigated.

### C. No Worker: yield points inserted at build time (prototyped as a comparison)

The owners did not choose "loop checks" as the model; this is only a comparison point.

- **How, with clang only.** Three ways were tried:
  1. `yield-cc.sh` compiles C to WebAssembly assembly (`clang -S`), inserts a counter check
     right after every `loop` instruction (every loop iteration passes there), and
     assembles the result (`clang -c file.s`). The check is a decrement of a wasm global
     and a branch to a slow path every 65,536 iterations. The slow path reads the clock
     (one system call) and, if 4 ms have passed since the last yield, makes 256 `getppid`
     calls so that the kernel's existing slice check (every 256th call) runs: the process
     yields, and a killed process unwinds there. No kernel change. (One kernel line, letting
     `sched_yield` yield when the slice is over, would replace the 256 calls.) Works for the
     whole tree, libc included.
  2. `-fsanitize-coverage=trace-pc-guard` (works on wasm32): a hook call on every edge of
     the control-flow graph. Much more expensive (below).
  3. `-finstrument-functions` does not compile for wasm32 (it needs
     `__builtin_return_address`). `-Xclang -finstrument-function-entry-bare` does compile
     (a call at every function entry), but it misses loops, so it can only be a complement
     (for recursion without loops).
  A source-level macro (for example a `YIELD()` the programmer writes in long loops) works
  too and costs nothing elsewhere, but covers only the loops someone remembered.
- **Guarantees.** Responsiveness and Ctrl-C at yield points, if every piece of code that
  can run long was compiled this way. Not covered: code built without it; a single long
  bulk instruction (`memory.copy`/`memory.fill` of hundreds of MiB is one instruction);
  loops whose iterations are slow (the count is iterations, not time: at 1 us per iteration
  the clock is read every 65 ms). It gives no extra core and no true preemption.
- **Costs.** On real programs under Node (best of 7, best of 3 runs, machine loaded):
  `bc` 2^60000 +79%, `bc` sqrt(2) to 1200 digits +76%, `sort` of 40,000 lines +58%,
  `grep` +26%, a tight loop -21% throughput; code size `bc` 44 to 51 KiB. With
  `-fsanitize-coverage`: +49%, +476%, +143%, +42%; `bc` 72 KiB. In Chromium the page kept
  presenting frames while such a program computed on the main thread (longest gap 26 to
  29 ms) and a 10 ms sleeper woke 13 to 19 ms late at worst.
- **Build.** Every program and the libc through the wrapper; a third build of the tree.

### D. The whole OS in one Worker (prototyped)

- `d-worker.mjs` runs `boot/host.mjs` (kernel and all one-thread processes) in a Worker.
  The page transfers an OffscreenCanvas to it (the Worker presents frames itself with its
  own `requestAnimationFrame`) and would forward input as messages. The Worker posts a
  heartbeat every 50 ms.
- A process that computes on the machine's thread stops the whole machine, but not the
  page: the page kept drawing (longest frame gap 17 to 21 ms) while the machine was stuck.
  A watchdog on the page terminates the machine's Worker when the heartbeat stops for
  longer than a limit (1 s in the test; it fired 2 to 13 ms after the limit) and boots a new
  one: a working shell 47 to 97 ms later. **All state is lost** (every process, every file
  written, since nothing persists), and in Chromium the old Worker keeps running for about
  2 s (section 5).
- **D with A or B**: the extension also works inside the machine's Worker (nested
  Workers). A spinning A or B process was killed by the kernel without a reboot; the
  machine's heartbeat never stopped longer than 61 to 79 ms.
- Boot in the Worker took 13 to 23 ms (78 to 88 ms with the Worker's own start).
- Costs and changes: `page.mjs` would be split into a page part (canvas, input, watchdog) and
  a machine part. The ARCHITECTURE assumption "the kernel runs on the page's main thread"
  (DECISIONS.md section 2) would be reversed. No change to programs or the kernel. Headers
  are needed only if combined with A.
- What D alone gives: the page stays alive and can offer a reset. What it does not: it
  cannot kill one process, and it does not add cores.

### Other approaches seen in comparable projects (not prototyped)

- **Kernel in a shared memory, every task in its own Worker, the kernel entered from every
  Worker under locks.** linux-wasm (Joel Severin): one Web Worker per task, `shared: true`
  memory, Atomics-based locks between Workers, `terminate()` for dead tasks; Linux is built
  NOMMU and programs `-fPIC -shared` into one address space; its dev server sends COOP and
  COEP. tombl/linux: kernel threads mapped to Workers, context switch by atomic notify
  between Workers, each process's own memory owned by its Worker and reached through
  requests that the owner Worker serves. BrowserPod (Leaning Technologies): "One is spawned
  for every process or thread", the kernel is a wasm module "available in every Worker",
  a new process gets a fresh memory and a thread reuses its process's memory. For UC.OS
  this would mean a kernel built with a shared memory and made safe for several CPUs
  (locks around every shared structure): system calls would stay direct wasm calls (fast)
  from every Worker and the kernel would scale across cores. It is a large kernel change and
  needs cross-origin isolation.
- **Kernel in a dedicated Worker, each process in its own Worker, system calls through
  shared memory.** Automattic's kandelo ("Each process runs in its own worker"; "Kernel
  (Rust to Wasm, in a dedicated worker)"; "system calls through shared memory"). This is A
  with D's placement of the kernel.
- **WASIX (Wasmer)**: threads on a pool of Web Workers sharing one SharedArrayBuffer; its
  browser SDK requires COOP and COEP; `fork`/`vfork` are listed as supported and
  setjmp/longjmp go through Asyncify (a Binaryen transform, which our rules exclude).
- **Synchronous requests from a Worker to a service worker** as the blocking primitive
  without isolation headers is a known technique; it was not researched or measured here.
- unkernel: not found under that name during this work; its figures in DECISIONS.md
  section 4 come from the earlier survey.

## 4. Measurements

Machine: the same laptop as PERFORMANCE.md (Intel i5-8250U, 8 threads), **heavily loaded by
other engineers' builds (load average 3.5 to 7.5 during every run)**. Node 26.13, headless
Chromium 158. Each figure is the best of several runs unless it says otherwise; ranges are
across runs. Benchmarks follow PERFORMANCE.md section 4 (timed loops in `noinline`
functions, batches of at least 20 ms, best of 16 batches, the first batch reported too).
The main-thread column is the same test program on the default path, measured in the same
runs; it agrees with PERFORMANCE.md.

### System calls (best batch, per call)

| | main thread (default) | B | A | A, both sides spin |
|---|---|---|---|---|
| `getppid`, Chromium | 18.8 to 20.8 ns | 46 to 122 us | 12.2 to 20.4 us | 0.62 to 2.3 us |
| `write` 64 B to /dev/null, Chromium | 27.9 to 29.1 ns | 62 to 168 us | 12.7 to 19.7 us | 0.55 to 0.67 us |
| pipe write + read 64 B, Chromium | 156 to 178 ns | 110 to 262 us | 24.6 to 42.1 us | 2.9 to 4.9 us |
| `getppid`, Node | 17.0 ns | 59.6 us | 17.2 us | 0.40 to 0.53 us |
| `write` 64 B, Node | 29.4 ns | 60.7 us | 16.1 us | 0.46 to 0.78 us |
| `getppid` while a main-thread process is CPU-bound (`yes > /dev/null`), Chromium | | 8.7 to 8.9 ms | 8.7 to 8.8 ms (some batches 12 to 18 us) | 0.5 to 2.3 us (best batch) |

"Spin" means the Worker spins up to 200,000 times before `Atomics.wait` and the main thread
spins up to 50 us before `Atomics.waitAsync` (and gives the event loop a turn at least every
4 ms, without which the page froze). Under Node, spinning on either side alone gave the
same 0.5 us; several runs with both sides spinning gave 12 to 18 us instead, which is
consistent with the machine's load; a standalone channel benchmark gave 490 ns.

### Start, kill, memory

| | main thread | B | A |
|---|---|---|---|
| spawn + wait of a program that exits at once, Chromium, best (mean) | 0.10 to 0.32 ms (0.36 to 0.95) | 13.6 to 35.8 ms (15.7 to 49.6) | 14.2 to 28.1 ms (16.9 to 46.6) |
| the same under Node, best | 0.14 ms | 24.0 ms | 23.9 ms |
| `kill` of a spinning process until its parent has reaped it, Chromium | | 0.5 to 1.1 ms | 2.0 to 2.9 ms (includes a 1.5 MiB memory snapshot the measuring code takes) |
| until the Worker thread really stops, Chromium | | about 2 s (not observable from the page) | 2,013 to 2,025 ms (its memory stops changing) |
| until the Worker thread really stops, Node (`exit` event) | | 1.0 to 7.6 ms | 0.9 to 4.7 ms |
| memory per process, Chromium (PSS of all Chromium processes, 8 sleeping processes, delta / 8) | 0.15 to 0.5 MiB | 3.9 to 4.0 MiB | 2.8 to 2.9 MiB |

### Responsiveness while a process computes for 4 s without system calls (Chromium)

A main-thread process sleeps 10 ms 350 times and paints a stripe on `/dev/fb0` each time;
the page presents the framebuffer on every animation frame.

| Where the loop runs | page: longest gap between animation frames | frames presented in about 3.6 s | the 10 ms sleeper, worst lateness |
|---|---|---|---|
| main thread, ordinary build (1.5 s loop) | 1,464 to 1,733 ms (no frame at all) | 0 | |
| Worker, A | 16.9 to 37.4 ms | 215 to 219 | 1.0 to 4.8 ms |
| Worker, B | 24 to 70 ms | 217 to 223 | 7.7 to 50 ms |
| main thread, C (yield points) | 26.2 to 29.5 ms | 261 to 266 in about 4.4 s | 12.6 to 18.6 ms |

In the real desktop (`desktop.html`): a 20 s loop in a terminal, a second terminal opened
and ran `ps` (state R for the looping program), Ctrl-C ended it, longest frame gap 31 to
40 ms for A and B; the ordinary build's 3 s loop froze the page for 2.8 to 3.6 s. Screenshots:
`build-NAME/shots/wp-d0*.png`.

### Correctness: the existing suites with every process in a Worker

Images where every program is the A build (or the B build, in checking mode):

| Suite | A | B |
|---|---|---|
| C library (`libctest`) | 290 of 290 | 290 of 290 |
| shell and tools (`sh-tests`) | 238 of 238 | 238 of 238 |
| kernel (`ktest`) | 65 of 66 | 65 of 66 |

The one ktest failure, in both: "a spinning process is preempted" expects the child to make
1,000 system calls within the parent's 40 ms sleep. A Worker child spends 15 to 25 ms just
starting and each call costs 13 to 60 us, so it cannot; the parent did wake on time, which
is what the check protects. ktest's last informational loop (5 million `getppid`) takes
about 65 s in A and 215 s in B.

## 5. Findings that apply to every Worker approach

1. **Lost wake-ups.** On one thread, `host.block()` runs in the same JavaScript turn as the
   -ERESTARTSYS return, so nothing can happen in between. With a Worker there is a gap
   between the reply and the Worker's block request. A `wake` that lands in it finds the
   process not parked and is ignored (correct on one thread, ARCHITECTURE.md section 4),
   and the process then sleeps forever. This hung ktest's pipe tests in the first version.
   Fix (in the prototype): the main thread parks the process at the moment the call returns
   -ERESTARTSYS, and the Worker's block request only waits for that park to end.
2. **`Worker.terminate()` in Chromium waits about 2 seconds before it stops a Worker that
   is computing.** Measured 2,011 to 2,025 ms in every run, for a wasm loop and for a
   JavaScript loop alike (`terminate.html`). A Web Platform Tests file states that
   "Chromium forcibly terminates a worker after 2 seconds". For the kernel the process is
   gone at once (it cannot make another call: nobody serves it), but the thread keeps a core
   busy for 2 s, and its memory is freed only then. Under Node the thread stops in 1 to 8 ms.
   Firefox and Safari were not measured.
3. **Every Worker process's system calls wait for the main thread.** While a main-thread
   process uses its whole 8 ms slice, each call from a Worker waits for the next slice
   boundary (8.7 to 8.9 ms per call measured, for A and B). Spinning on the main thread
   (A only) helps but spends main-thread time.
4. **CPU accounting.** The host accounts CPU time only when a process parks. A Worker
   process that never parks shows 0 CPU in `ps` and `htop` (seen in the desktop
   screenshot). It would need the Worker side to report its time.
5. **Start-up is about 100 times slower than on the main thread** (Worker creation). A pool
   of idle Workers would hide part of it; not built.
6. **Time slicing still runs** for Worker processes (the kernel's check every 256th call):
   harmless, one extra round trip every 8 ms.

## 6. Relation to `fork` (decision 7) and to several cores

- **fork.** Decision 7 keeps fork possible through a compile-time stack transform. In A the
  parent's memory is a SharedArrayBuffer the main thread can read: the child is a new shared
  memory filled with one copy, plus a new Worker. In B the main thread cannot see the
  parent's memory: fork means asking the Worker to send its whole memory (a large message)
  first. C and D change nothing for fork. In every Worker approach a forked child costs a
  Worker start (14 to 36 ms here) on top of the copy.
- **Threads.** Only A can grow into threads (several Workers on one shared memory). That
  also needs thread-local storage, atomics in the libc and a futex-like system call. B
  cannot have threads that share memory.
- **Several cores.** A and B run each Worker process's user code on its own core. The
  kernel stays one thread on the main thread, so system-call-heavy work does not scale and
  every call waits for the main thread (finding 3). Making the kernel itself run on several
  cores is the shared-kernel model of linux-wasm, tombl/linux and BrowserPod (section 3,
  others): a kernel with a shared memory and locks. C gives no extra core. D moves
  everything to one other core.
- **Preemption.** None of A, B, C or D preempts a process in the sense of stopping it
  and resuming it later: a Worker process simply runs in parallel; C yields at points it
  chose. Stopping a Worker temporarily (SIGSTOP-like) is not possible from JavaScript;
  only termination is.

## 7. Open decisions for the owners

1. **Which approach or combination.**
   | Option | What it costs |
   |---|---|
   | A | isolation headers (hosting); a second build of the libc and of each Worker program; 12 to 20 us per call (0.5 to 2.3 us with spinning, which costs main-thread time) |
   | B | no headers, no rebuild; 46 to 122 us per call; a marshalling table to keep complete for every new call that reads user memory |
   | A where the page is isolated, B where it is not | both sets of costs; two code paths to test |
   | C | 26% to 79% slower programs (real ones measured), a third build of the tree; no extra core; no guarantee for code built without it |
   | D alone | page split in two; the machine still freezes (the page offers a reset that loses everything) |
   | D with A or B | the costs of both |
   | shared kernel on several cores (not prototyped) | a kernel made safe for several CPUs; isolation headers |
2. **Are cross-origin isolation headers acceptable**, given that hosting is still open
   (DECISIONS.md section 3)? Choices: only hosts that can send headers (rules out GitHub
   Pages and GitLab.com Pages); the service-worker workaround (one reload on first visit,
   HTTPS, third-party code or our own version); no headers, so no A.
3. **How a program is marked to run in a Worker.**
   | Option | Cost |
   |---|---|
   | a flag at spawn (a new `spawn` flag or call) | ABI change; every caller decides |
   | something in the file (an export, as in the prototype, or a custom section) | decided at build time; works for any caller |
   | a shell builtin or prefix (like `nice`) | only from the shell |
   | automatic after N ms without a system call | a running main-thread process cannot be moved to a Worker (its stack and memory are in the main thread's wasm instance); this could only restart it or apply to the next run |
   | the A build itself is the mark (a shared memory means a Worker) | ties marking to the build |
4. **What happens when the browser lacks the features** (no isolation, no JSPI in Workers,
   no `Atomics.waitAsync`): run the program on the main thread anyway (freezes again);
   refuse to start it (`-ENOEXEC` or a new error); fall back from A to B.
5. **Whether a Worker pool** (idle Workers kept ready) is wanted, trading memory (about
   3 to 4 MiB each) for start-up time.
6. **What to do about Chromium's 2 s termination delay**: accept it; or limit how many
   killed Workers may be still dying at once.

## 8. What was not verified

- Firefox and Safari: not installed here. SharedArrayBuffer, `Atomics.waitAsync`, JSPI
  inside Workers, nested Workers, OffscreenCanvas in Workers and the termination delay are
  all unmeasured there.
- Real hosting with the headers, and the service-worker workaround: not tried.
- Input forwarding in D: the D prototype drives the shell through the console only.
- A Worker pool, fork, threads, the shared-kernel model: not built.
- The cause of B's worse frame gaps and timer lateness, and of its slow first 100 ms in
  Chromium: not established.
- All numbers come from a loaded machine; they are best-of figures, not typical ones.

## 9. How to reproduce

```sh
make B=build-NAME dist                                    # the default build
make -f tools/worker-proto/proto.mk B=build-NAME          # all variants, images, web folder
node tools/worker-proto.mjs --build=build-NAME/proto/run --init=/bin/sh \
  --input='echo hi | wproto-b basic -\necho hi | wproto-a basic -\nwproto-a bench\nexit\n' --check
node tools/worker-proto.mjs --build=build-NAME/proto/run-a --init=/bin/libctest   # every process in a Worker (A)
node tools/worker-proto.mjs --build=build-NAME/proto/run-b --init=/bin/libctest --check
node tools/worker-proto.mjs --build=build-NAME/proto/run --init=/bin/sh --input='sh /usr/share/tests/wp-cpu.sh coi\nexit\n'
node tools/worker-proto.mjs --browser --build=build-NAME                 # scenario, isolated page
node tools/worker-proto.mjs --browser --build=build-NAME --no-coi        # scenario without the headers (B only)
node tools/worker-proto.mjs --browser --build=build-NAME --query='mem=8&kind=a'   # memory sampling (main, a, b)
node tools/worker-proto.mjs --browser --build=build-NAME --test=desktop  # the real desktop, screenshots
node tools/worker-proto.mjs --browser --build=build-NAME --test=d        # approach D
node tools/worker-proto.mjs --browser --build=build-NAME --test=terminate
```

Runner options: `--check` and `--drop=NR,...` (B's checking mode and a test that it catches
misses), `--spin-a=N`, `--main-spin=US`, `--stats`; in the browser `--query='spina=N&mainspin=US'`
and `--query='cmd=...'` (shell lines instead of the scenario).

## 10. Sources (read during this work)

- linux-wasm, Joel Severin: https://github.com/joelseverin/linux-wasm (README and
  `runtime/linux.js`, `runtime/linux-worker.js`, `runtime/server.py`)
- tombl/linux: https://github.com/tombl/linux (README; `arch/wasm/kernel/process.c`,
  `arch/wasm/kernel/remote_vm.c`, branch `wasm`)
- BrowserPod deep dive: https://labs.leaningtech.com/blog/browserpod-deep-dive
- kandelo: https://github.com/Automattic/kandelo
- WASIX features: https://wasix.org/docs/explanation/features ; Wasmer SDK troubleshooting
  (COOP/COEP): https://docs.wasmer.io/sdk/wasmer-js/explainers/troubleshooting
- coi-serviceworker: https://github.com/gzuidhof/coi-serviceworker
- GitHub Pages headers: https://github.com/orgs/community/discussions/13309
- GitLab Pages headers: https://gitlab.com/gitlab-org/gitlab-pages/-/issues/50
- Cloudflare Pages headers: https://developers.cloudflare.com/pages/configuration/headers/
- Chromium's forced worker termination after 2 s (test comment):
  https://chromium.googlesource.com/external/w3c/web-platform-tests/+/refs/heads/DOMParser-interface/workers/Worker-terminate-forever-during-evaluation.html ;
  the change that added the delayed forced termination: https://codereview.chromium.org/2490113002
