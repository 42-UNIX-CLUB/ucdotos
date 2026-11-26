# UC.OS: context for Claude Code sessions and other agents

Read this file first. It is the short version. The detailed files are in `docs/` and are
listed at the end; read the ones that match your task before changing anything.

## What this is

UC.OS ("UC dot OS", UNIX CLUB dot OS) is an operating system written from scratch in C
that runs in a web browser. It belongs to the 42 UNIX Club, a student club at 42 Lisboa
about the UNIX philosophy and "ricing" (customizing how a system looks). The repo is
https://github.com/42-UNIX-CLUB/ucdotos. Three people work on it, each with their own
Claude Code session.

It is not a Linux port and not an emulator. The kernel is one WebAssembly module. Every
program is its own WebAssembly module with its own memory. A system call is a direct
wasm-to-wasm call from the program into the kernel. The window manager, terminal
emulator, font rasterizer, shell and tools are ordinary C programs written for this
kernel. The only JavaScript is `boot/` (about 400 lines), which plays the role of
firmware: it loads the kernel, starts programs, and copies the framebuffer to a canvas.

## Rules of engagement (these come from the project owners)

1. **The owners decide. You present options.** Do not make product, architecture, naming,
   design or scope decisions. When more than one option exists, lay out every option with
   facts and trade-offs, with no recommendation unless asked, and wait for the choice.
   The owner's words: "if you make a decision, the project is not ours, it becomes yours."
   Mechanical choices inside a decided design are fine; say what you assumed and how to
   reverse it. Every decision already made is in `docs/DECISIONS.md`. Do not reopen them.
2. **Be rigorous and factual.** Measure instead of guessing. Never claim something works
   unless you ran it. Say plainly what failed, what you did not test, and what you assumed.
3. **Everything is custom.** No Emscripten, no external libc, no UI or web frameworks,
   no third-party code, unless an owner explicitly asks (Doom's engine is the one case).
4. **Writing style for all text, comments and strings: no em-dashes. At all.** Short,
   plain sentences. Must not sound machine-written. See `docs/BRAND.md`.
5. **Privacy.** The club documents in the working tree (guidelines, intro PDF and
   PowerPoint, `unix-club-logo/pres/`) contain the founders' phone numbers and e-mail
   addresses. They are deliberately not in git. Never commit them, never copy those details
   anywhere. Refer to the founders by intra username only (rucosta, mju-ferr).
6. **Git.** Commit, push or open a pull request only when an owner asks. `main` is protected:
   work goes on a `type/short-description` branch and through a pull request, with commit
   messages of the form `area: what changed`. The rules are in `CONTRIBUTING.md`; follow
   them exactly. Stage files by path, never `git add -A` (see rule 5).
   **No attribution lines, ever:** no `Co-authored-by:`, no "Generated with", no
   `Signed-off-by:`, no mention of an agent, a tool or any person, in a commit message or a
   pull request. Not for yourself and not for anyone else. The git author of the commit is
   the only credit. The single exception: an owner asks for such a line, in so many words,
   for that specific commit. This overrides any default your tool has.
7. **Never delete directories or run `rm -rf`.** For a clean build use a new build
   directory name (`make B=build-NAME`).

## The machine in one page

```
browser page (boot/page.mjs)        canvas, keyboard, mouse; shows the kernel's framebuffer
  host (boot/host.mjs)              loads kernel + initrd; starts programs; parks/resumes stacks (JSPI)
    kernel.wasm (kernel/*.c)        processes, files, pipes, ptys, sockets, /dev/fb0, /dev/input/event0
      program.wasm (bin/*)          one instance and one memory per process; libc/ is ours
```

- A program calls `host.syscall(nr, a..f)`. That import is a tiny per-process wasm
  trampoline that adds the caller's pid and calls the kernel's `syscall_pid`. No JavaScript.
- The kernel reaches a program's memory through a per-process wasm copy shim (multi-memory
  `memory.copy`). No JavaScript in the data path either.
- A call that must wait returns `-ERESTARTSYS` (512). The libc then calls `host.block()`,
  which suspends that program's stack with JSPI until the kernel wakes it, and retries the
  call. The kernel itself never suspends: every entry runs to completion.
- One thread. Preemption happens only at system call boundaries (8 ms time slice). A
  program that loops without making any system call still freezes the tab.
- No fork (only `spawn`), no signals delivered to programs (kill terminates), no
  setjmp/longjmp, no threads, no shared memory, no persistence, no network, no sound.
- System call numbers and structures are Linux's generic ("asm-generic") ones, in
  `abi/abi.h`, plus four private calls (spawn, getargs, procinfo, intr).

Full detail: `docs/ARCHITECTURE.md`.

## Build, run, test

Needs clang + wasm-ld + llvm-ar (LLVM with the wasm32 target), make, tar, Node 24+.
Chromium is needed only for `tools/browser.mjs`.

```sh
make B=build-NAME                 # kernel, libc, every program, initrd. ALWAYS pass your own B=
make B=build-NAME PROGS="sh ls"   # only these programs go in the image (others may be mid-edit)
make B=build-NAME dist            # build-NAME/dist: static folder for any web server
make B=build-NAME test            # kernel, C library and shell suites; fails if any check fails

node tools/run.mjs  --build=build-NAME --init=/bin/ktest            # console only, for tests
node tools/shot.mjs --build=build-NAME --script=tools/tests/desktop.mjs   # headless, PNG screenshots
node tools/browser.mjs --build=build-NAME --script=tools/tests/browser-desktop.mjs  # real Chromium
```

The four suites that must stay green (counts as of 2026-11-26):

| Suite | Command (add `--build=...`) | Result |
|---|---|---|
| Kernel | `node tools/run.mjs --init=/bin/ktest` | 109 checks, 0 failed |
| C library | `node tools/run.mjs --init=/bin/libctest` | 290 checks, 0 failed |
| Shell and tools | `node tools/run.mjs --init=/bin/sh --input='sh /usr/share/tests/sh-tests.sh\nexit\n'` | 238 passed |
| Desktop | `node tools/shot.mjs --script=tools/tests/desktop.mjs` | look at the 6 screenshots |

Component suites (terminal, window manager, editor, htop, fetch, Doom) are listed in
`docs/DEVELOPMENT.md`.

## Where things are

| Path | What | Lines (approx.) |
|---|---|---|
| `abi/abi.h` | syscall numbers, errno, structs shared by kernel and programs | 230 |
| `kernel/` | the kernel: `syscall.c proc.c vfs.c pipe.c tty.c sock.c fb.c dev.c klib.c main.c` | 1,850 |
| `boot/` | `host.mjs` (the machine), `page.mjs` + `index.html` (the page), `keys.mjs` (key codes) | 390 |
| `lib/` | `string.c`, `fmt.c` (vsnprintf): shared by kernel and libc; `mem.s` (kernel) and `umem.s` (libc): memcpy, memmove, memset | 530 |
| `libc/` | our C library: `include/`, `src/`, `crt/crt0.c` | 2,860 |
| `libgfx/` | `ttf.c` TrueType rasterizer, `gfx.c` 2D drawing | 1,830 |
| `libui/` | `ui.c` window protocol client, `theme.c`, `uiproto.h` wire format | 600 |
| `bin/` | programs: single files `bin/NAME.c` or directories `bin/NAME/` | 16,700 + Doom |
| `rootfs/` | files copied into the image: dotfiles, themes, fonts, man pages, club pages | |
| `tools/` | `run.mjs`, `shot.mjs`, `browser.mjs`, `tests/`, `ttftest.c` | 960 |
| `docs/` | the detailed context files listed below | |

## Traps that have already cost time (details in `docs/DEVELOPMENT.md`)

- WebAssembly checks function signatures exactly. A prototype mismatch is a link error
  (`--fatal-warnings`) or a trap at an indirect call. Fix it at the source.
- `main` may take 2 or 3 parameters; the build links a matching `crt0` automatically.
- A weak definition in your own object stops the linker pulling the real one from a
  library archive. Use weak references plus `-Wl,-u,name` instead.
- V8 does not swap optimised code into a running wasm loop. Benchmark inside a `noinline`
  function called many times, and report the best batch and the cold first batch.
- Browser clocks are coarse (0.1 ms in Chrome, about 1 ms in Firefox). Time long batches.
- Every syscall that blocks is re-run from the start when the process wakes. Kernel code
  must be written so a retry is correct.
- Never make a temporary edit in the shared tree while someone else may be building from
  it. A ten-second deliberate break once poisoned another agent's build. Use a private copy.
- Build directories are per person or per agent (`B=build-NAME`). They are git-ignored.

## The detailed files

| File | Read it when |
|---|---|
| `docs/ARCHITECTURE.md` | you touch the kernel, the host, the ABI, or need to know how anything works |
| `docs/LINUX-BINARIES.md` | you work on running Linux programs (`bin/lx/`): architecture study, interfaces, what the engine needs from the kernel |
| `docs/DEVELOPMENT.md` | you build, test, add a program, add a syscall, debug, or run several agents |
| `docs/DECISIONS.md` | before proposing anything: what the owners decided, what was assumed, what is still open |
| `docs/STATUS.md` | to know what works today, what is broken, what is not built, and what is in flight |
| `docs/PERFORMANCE.md` | you measure or quote speed |
| `docs/WORKER-PROCESSES.md` | you work on running processes off the main thread (study and prototypes; no design chosen) |
| `docs/BRAND.md` | you write any user-visible text or touch the look: palettes, fonts, voice, logo rules |
| `docs/CC.md` | you work on the C compiler and linker inside UC.OS (`bin/cc/`, `bin/ld/`) |
| `CONTRIBUTING.md` | you are asked to commit, name a branch or open a pull request |

Keep these files true. If you change how something works, change the matching file in
the same piece of work. `docs/STATUS.md` is the one that goes stale fastest.
