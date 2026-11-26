# Decisions

What the owners decided, what was assumed on their behalf and can be overturned, and
what is still open. Before proposing a change, check it against this file. Do not reopen
a decision unless an owner asks.

How decisions are made here: an agent lays out every option with facts and no
recommendation (unless asked), and an owner chooses. Add each new decision to this file
with its date, the options that were on the table, and who chose.

## 1. Decided by the owners

| # | Date | Decision | Options that were considered |
|---|---|---|---|
| 1 | 2026-11-25 | The project is a real OS that runs in the browser, for the club's showcase on 2026-11-26 and beyond. Time is explicitly not a constraint. | |
| 2 | 2026-11-25 | No web frameworks. | |
| 3 | 2026-11-25 | **Build everything ourselves** ("family D"): a custom kernel, libc, tools and desktop, in C, for maximum control. | A whole-machine emulators (v86, JSLinux, QEMU-wasm), B syscall emulators (CheerpX), C kernels compiled to wasm (unkernel, linux-wasm), D write our own, E simulated web desktops, F streamed real VMs |
| 4 | 2026-11-25 | The kernel is compiled **straight to WebAssembly**; programs are separate wasm modules written for it. Linux binaries are a later goal, not an immediate one. | kernel for a real CPU inside our own emulator; inside v86 |
| 5 | 2026-11-25 | **Process model: hybrid.** One thread with direct wasm-to-wasm system calls and JSPI for blocking by default; CPU-heavy programs may run in their own Worker. | one thread only; one thread plus compiler-inserted loop checks; a Worker per process |
| 6 | 2026-11-25 | **Memory: separate memory per process, copied with a wasm shim** (multi-memory). | one shared memory; separate memories copied through JavaScript |
| 7 | 2026-11-25 | **`fork`: spawn only now, keep fork possible later** through a compile-time stack transform. | spawn only forever; real fork now |
| 8 | 2026-11-25 | **System calls are Linux-shaped** (numbers and semantics), so recompiling Linux programs later means implementing more calls, not redesigning. | our own interface; WASI |
| 9 | 2026-11-25 | **The window manager is an ordinary user program.** The kernel only exposes devices. | window manager inside the kernel; JavaScript draws the screen |
| 10 | 2026-11-25 | **Two themes, switchable**: the deck look (Gruvbox + Fira Code) and the brand look (dark green + JetBrains Mono). The target layout is slide 26 of the club's intro deck. | deck only; brand only |
| 11 | 2026-11-25 | **Text is drawn by our own TrueType rasterizer** in C. | glyph atlas baked at build time; a classic bitmap font |
| 12 | 2026-11-25 | **Programs for the showcase**: shell and core tools, fetch (with a 3D animated logo) and the club pages as files, a vi-like editor, a live system call speed demo, a version of minishell (repo to be provided), Doom, htop, bc. | |
| 13 | 2026-11-25 | **Goal for speed**: system calls faster than unkernel's published figures and as close to native Linux as possible; lean use of WebAssembly. | |
| 14 | 2026-11-25 | Work is split across **parallel Opus subagents** coordinated by one session. | |
| 15 | 2026-11-26 | **Name: UC.OS** ("UC dot OS", UNIX CLUB dot OS). The earlier working name DuxOS is dropped. | EpochOS and a long list of others |
| 16 | 2026-11-26 | **Repository**: https://github.com/42-UNIX-CLUB/ucdotos, branch `main`. The owners run git themselves. | |
| 17 | 2026-11-26 | **Licence: GPL-2.0-or-later.** | GPL-3.0 |

Notes that go with these:

- On 4: an owner first asked for a layer "better than WebAssembly". That is not possible
  in a browser: only JavaScript and WebAssembly execute there, so any layer we add sits
  on top of wasm and is slower. The decision was then to use wasm directly and well.
  "C with inline assembly" means WebAssembly instructions here, not x86 (see
  ARCHITECTURE.md section 13).
- On 5: only the one-thread half is built. See STATUS.md.
- On 8, the three rungs of Linux compatibility that were explained: (1) our own programs,
  which is where we are; (2) Linux programs recompiled from source against our libc, which
  needs more system calls; (3) unmodified Linux binaries, which needs a CPU emulator with
  a JIT plus most of Linux's roughly 350 calls and brings back emulation cost.
- On 17, what was explained before the choice: GPL-2.0-or-later lets us take code from
  GPL-2.0-only projects (Linux, BusyBox) and from GPL-3.0 projects; GPL-3.0 would have
  given an explicit patent grant and anti-lockdown terms but excluded GPL-2.0-only code.
  Serving the site counts as distributing binaries, so the source must be offered (the
  login message names the repo).

## 2. Assumptions made by agents (reversible; an owner can overturn any of them)

| Assumption | Why | How to reverse |
|---|---|---|
| "Linux-shaped" means Linux's **generic** call table (the one aarch64 and riscv use), not x86-64's. | It is what a new Linux architecture gets. | One header: `abi/abi.h`. |
| The kernel runs on the page's **main thread**. | Direct canvas access, simplest. | Move `boot()` into a Worker with an OffscreenCanvas. |
| The C compiler inside UC.OS writes clang-compatible relocatable objects and has its own linker (option A of `docs/CC.md`); the tools are called `cc` and `ld` with no `gcc` alias; headers in `/usr/include` and `/usr/lib/cc/include`, libraries in `/usr/lib`; `long double` is binary128 as with clang; `-O` levels are accepted and change nothing; the compiler makes a cheap system call per statement and per 64 tokens so a long compile can be time-sliced. C++ is out of scope. | Options and reasons in `docs/CC.md` section 3. | `docs/CC.md` section 3 says where each lives. |
| The window manager's modifier key is **Alt**. | Browsers and host systems take the Super key. | `set mod` in `~/.config/wm/config`. |
| Tiling layout is **master and stack**, 4 workspaces, 12 px gaps, radius 8. | To match slide 26. | The same config file. |
| The first terminal runs `sh /etc/welcome` (fetch, then a shell). | So the desktop greets with fetch like the slide and stays open. | The `exec` line at the end of the wm config. |
| Time slice is **8 ms**, with the clock read on every 256th system call. | Keeps input and a 60 Hz screen alive. Reading the clock costs about 400 ns in Chrome; at every 64th call it doubled the cost of the cheapest calls. The price: a process making slow calls (0.1 ms each) is only checked every 25 ms or so. | `SLICE_MS` in `kernel/kernel.h`; the mask in `syscall_pid`. |
| Limits: 64 processes, 64 descriptors, 16 ptys, 24 window clients. | Simple fixed tables. | See ARCHITECTURE.md section 14. |
| In the `brand` theme, blue, purple and aqua (and their bright versions) are **placeholders**. | The club's brand defines only 7 colours; a terminal needs 16. | `rootfs/usr/share/themes/brand.theme`. |
| Default theme is `deck`. | The deck is what the showcase audience has seen. | `rootfs/home/user/.config/ucos/theme`. |
| Doom: A/D **turn** (like the arrows); the game's memory zone is 16 MiB; no sound. | Stated in the port's brief. | `bin/doom/doomgeneric_ucos.c`, `i_system.c`. |
| Source files carry SPDX tags and **no copyright holder line**. | Whose name goes there is the owners' call. | Add a line to the headers and README. |
| `.gitignore` excludes build output and `bin/doom/wads/`. | Game data licence is undecided. | Edit `.gitignore`. |
| The club's source documents (guidelines, PDF, PowerPoint, `unix-club-logo/`) are **not in git**. | They contain phone numbers and e-mail addresses. | An owner can add redacted versions. |
| Shell: `~name` becomes `/home/name` without a user database; exit status 2 for expansion errors and special-builtin errors (and a `.` file not found in a script), 127 for `command -v` of an unknown name (as dash); `umask` is kept by the shell and succeeds, passed to the kernel when it has the call (the POSIX-faithful choice, after the owner asked which that was); a stopped foreground job does not stop the rest of the line; no warning when exiting with stopped jobs; `kill` is also a builtin; `exec CMD` inside a subshell runs `CMD` as a child; there is no `/bin/[` (a program cannot be named `[` under `bin/`), only `/bin/test`. | Choices made while closing POSIX gaps (2026-11-26). | `bin/sh/`. |
| Regular expressions and `fnmatch` live in the C library (`libc/src/regex.c`, `fnmatch.c`), not inside the tools. | They are POSIX C library functions other programs can use. | Move the files. |
| Linux programs: riscv64 as the first guest architecture; the engine is called `lx`; the guest root is `/debian` if it exists, else `/`; guests see `uname` Linux 6.12.0-lx riscv64; the guest user id is the host's unless `LX_UID` is set; memory protection is recorded, not enforced; `fork` in a guest is emulated until the kernel has a clone call. | Listed with reasons in `docs/LINUX-BINARIES.md` section 1. | The same section says where each one lives. |
| OS identity strings: `uname` reports `UC.OS`, release `0.1.0`, machine `wasm32`, host name `42lisboa`, user `user`. | Needed something. | `kernel/kernel.h`, `kernel/syscall.c`, `bin/init.c`. |

## 3. Open: an owner needs to choose

| Topic | Options | Blocks |
|---|---|---|
| **Shell: reference for conformance** | bash 5.3 in POSIX mode with 26 overrides written from knowledge of dash (now; dash is not installed on the build machine), or dash itself (`node tools/tests/sh-posix.mjs --gen --shell=dash`) | |
| **Core tools in the image** | 28 separate programs, 759 KB together: keep all in the default image (now); one multi-call program under many names once the kernel has links (saves about 12 KB per tool); leave rarely used tools out of the default image | Image size |
| **`/etc/passwd` and `/etc/group`** | none (now: `whoami` fails, `id` prints numbers); add files naming uid 1000 `user`; make `whoami` fall back to `$USER`; wait for Debian's base-passwd from the package manager | |
| **Where the core tools' test data lives** | in the image under `/usr/share/tests` (now, about 298 KB); in a separate test image | Image size |
| **Compressed image** | `make dist` writes `initrd.tar.gz` and the page prefers it (built, in the tree): ship it as built; drop it and let the web server compress; plain tar only. It saves 4 MB of download (3.2 s at 10 Mbit/s) and costs 135 to 175 ms of unpacking (PERFORMANCE.md section 6) | Hosting |
| **`-Oz` instead of `-O2`** | Kernel 21% and programs 26% smaller, image 2.6% smaller; speed the same within noise except `fib` (faster) and 16-byte `memset` (slower); not measured in Chromium (PERFORMANCE.md section 7) | |
| **Time-slice check cost** | The check costs about 2.5 ns per `getppid` in Chromium, not explained: keep it (now); skip it for trivial calls (a program looping only on those would never yield); a clock in shared memory written by a Worker (needs cross-origin isolation headers) | |
| **Guest architecture for Linux programs** | amd64, arm64 or riscv64 (now: riscv64, chosen by an agent as the measurement vehicle). Debian 13 riscv64 programs run; Ubuntu 26.04 riscv64 programs need vector instructions; the package counts, instruction census and costs per architecture are in `docs/LINUX-BINARIES.md` section 2. Also open there: guest root layout, guest user id (dpkg wants 0), threads model | Which Debian and Ubuntu programs can run |
| **Doom game data** | Shareware `doom1.wad` (4.2 MB; free to distribute unmodified, proprietary content) or `freedoom1.wad` (28.8 MB, BSD). Both are in `bin/doom/wads/` on the build machine, neither is in git or in the image. | Doom being playable in the hosted build |
| **Doom A/D keys** | turn (now) or strafe | |
| **Hosting** for dux.ensui.dev and the colleagues' subdomains | Any static host works (`make dist`). Each domain could be an independent copy, redirect to one canonical domain, or show a different machine. No special headers are needed today. | Going live |
| **Persistence** | none (now): every reload starts fresh; keep files per browser origin (IndexedDB or OPFS); export and import | |
| **Networking** | none (now); HTTP only through `fetch`; outbound TCP through a relay server someone runs | |
| **Worker half of the hybrid model** | when, and which programs run there | Freeze-proofing against pure CPU loops |
| **Sound** | a kernel audio device feeding Web Audio | Doom sound |
| **minishell** | the owner will provide the repo; it must be adapted from `fork`+`execve` to `spawn` | |
| **Linking exception** | Every program links our GPL libc statically, so third-party programs must be GPL-compatible unless the libc (and libgfx, libui) get a linking exception. | Non-GPL programs for UC.OS |
| **Copyright holder** | the three authors, or the club | Licence headers |
| **Brand theme colours** | approve or replace the placeholder blue, purple and aqua | |
| **The 42 glyphs in public** | The club's own brief says to clear public use of the 42 logo with the campus. The mark (which contains the 42 glyphs) is drawn by fetch and the wallpaper. | Public launch |
| **Browser floor** | JSPI needs Chrome/Edge 137+, Firefox 153+, Safari 27+. Accept, or add a fallback (a compile-time stack transform) for older browsers. | |

## 4. Background: the research that led here

Before any code, the options for "an OS in a browser" were surveyed (six families, about
25 engines). The points that shaped the decisions:

- unkernel (a Linux kernel compiled to wasm that emulates aarch64 user programs) showed
  that a wasm-native kernel makes system calls nearly free compared with emulating a
  whole PC: it publishes 172 ns for `getppid` against 1,662 ns under qemu-system.
- The catch in that family is user programs: wasm has no MMU, so either every program is
  rebuilt for wasm (no real `fork`) or the user CPU is emulated (slow user code). UC.OS
  takes the first answer.
- A browser cannot open raw network sockets, so any networking needs a relay server.
- Browser storage is per origin: files saved on one domain do not exist on another.
