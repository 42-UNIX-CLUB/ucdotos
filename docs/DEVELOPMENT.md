# Developing UC.OS

How to build, run, test, extend and debug the system, and how to work in the tree when
several people or agents are in it at once.

## 1. Tools you need

| Tool | Used for | Version known to work |
|---|---|---|
| clang, wasm-ld, llvm-ar, llvm-objdump | everything | LLVM 23.1 |
| make, tar | the build | GNU |
| Node.js | the test runners | 26 (needs JSPI: 24+) |
| Chromium | `tools/browser.mjs` only | 158 |
| host `cc`, Python 3 with PIL | native unit tests of some components | |

No Emscripten, no wasi-sdk, no npm packages.

## 2. Building

```sh
make B=build-NAME                    # everything
make B=build-NAME PROGS="sh ls cat"  # put only these programs in the image
make B=build-NAME dist               # also assemble build-NAME/dist for a web server
make B=build-NAME WADS=DIR           # take Doom's data files from DIR (default bin/doom/wads)
```

- **Always pass your own `B=`.** The default is `build/`. Build directories are
  git-ignored. Each person and each agent uses a distinct one so that parallel builds
  cannot corrupt each other.
- `PROGS=` exists because every `bin/*.c` and `bin/*/` is a program, and someone else's
  half-written program would otherwise break your build.
- Outputs: `kernel.wasm`, `libc.a`, `libgfx.a`, `libui.a`, `libarc.a`, `libpkg.a`, `bin/NAME` (wasm files with no
  extension), `root/` (the staged filesystem), `initrd.tar`, and with `dist`: `initrd.tar.gz`
  (the same archive, `gzip -9`) and `dist/`, which holds both. The page fetches
  `initrd.tar.gz` and unpacks it with the browser's `DecompressionStream`, and falls back to
  `initrd.tar` when there is no `.gz`; the Node tools read `initrd.tar`. Whether to ship the
  `.gz` is an open owner decision.
- Every `make` runs `tools/remote-files.sh`, which writes `B/files/` (the remote files, a
  `.gz` copy where that is smaller, Freedoom's licence, small test files) and the list
  `B/root/etc/remote-files` (section "Remote files" in ARCHITECTURE section 7). `make dist`
  copies `B/files/` to `dist/files/`. Doom's data comes from `WADS=DIR`, default
  `bin/doom/wads/`, which is git-ignored and empty in a fresh clone: then the build still
  succeeds and Doom says in its window that it has no data. `sh tools/get-doom-data.sh
  [DIR]` downloads both free data files and checks them against pinned SHA-256 sums
  (Freedoom 0.13.0 from the project's release; the shareware `doom1.wad` from Debian's
  `doom-wad-shareware` package, since id Software no longer hosts it).
- `tools/run.mjs` and `tools/shot.mjs` serve remote files from the build directory
  (`tools/remote-node.mjs`); `--remote-rate=BYTES_PER_SECOND` slows them down and
  `--remote-off` makes every fetch fail, to test progress and errors.
- There is no `clean` target on purpose. Use a new `B=` name. Do not `rm -rf`.
- The initrd is `rootfs/` plus the programs under `/bin`, as a plain ustar archive.
  The staging directory `root/` is only added to, never emptied: a file you removed from
  `rootfs/` stays in an old build directory's image. Another reason to use a fresh `B=`.

### What a program is

| Form | Rule |
|---|---|
| `bin/NAME.c` | one file, linked with `libc.a` |
| `bin/NAME/*.c` | every `.c` directly in the directory is compiled and linked. Subdirectories are not picked up (put tests there) |
| `bin/NAME/CFLAGS` | optional file: extra compiler and linker flags for that program |
| `bin/NAME/USE_GFX` | optional empty file: also link `libui.a` and `libgfx.a` |
| `bin/NAME/USE_ARC` | optional empty file: also link `libarc.a` (compression, tar, ar, checksums; `libarc/arc.h`) |
| `bin/NAME/USE_PKG` | optional empty file: also link `libpkg.a` and then `libarc.a` (the package tools use it) |
| `bin/NAME/ALIASES` | optional: more names, one per line, under which the same program is copied into `/bin` (there are no links in the file system yet). The program looks at `argv[0]`. `bin/gzip/ALIASES` gives `gunzip`, `zcat`, `xz`, `xzcat`, ... |

`main` may be `main(void)`, `main(int, char **)` or `main(int, char **, char **)`.
The link tries `crt0_2.o` first and falls back to `crt0_3.o`.

To add a file to the image, put it under `rootfs/` at the path it should have.

## 3. Running

Three runners, all in `tools/`. All take `--build=build-NAME`.

**`run.mjs`: console only.** The program's stdin/stdout/stderr are `/dev/console`.

```sh
node tools/run.mjs --build=B --init=/bin/ktest
node tools/run.mjs --build=B --init=/bin/sh --input='echo hi | cat\nexit\n'
node tools/run.mjs --build=B --init=/bin/sysbench --args="--json"
```

`--input` types text and then sends end-of-file (the console buffer holds 1024 bytes, so
put long scripts in a file inside the image). `--args` are passed to the program.
`--debug` prints trap messages from the host. The console is not a terminal: terminal
ioctls return `ENOTTY` there. A program started as pid 1 has no environment.

**`shot.mjs`: headless with a screen.** Boots under Node with a framebuffer, drives the
input device with scripted events, writes PNG screenshots to `B/shots/`.

```sh
node tools/shot.mjs --build=B --script=tools/tests/desktop.mjs [--size=1280x720] [--init=/bin/wm] [--quiet]
```

A script's default export is `async (t) => {}` with `t.sleep t.shot t.key t.tap t.type
t.move t.click t.wheel t.resize t.pixel t.log t.sys` (documented at the top of the file).
`t.log` is everything printed to the console; the terminal's Ctrl+Alt+Shift+F12 hook
prints its visible text there, which is how scripts assert on what a terminal shows.

**`browser.mjs`: real Chromium.** Serves `B/dist` itself, launches headless Chromium and
drives it through the DevTools protocol with real key and mouse events. Same script API
plus `t.eval('js')`. Run `make B=... dist` first.

```sh
node tools/browser.mjs --build=B --script=tools/tests/browser-desktop.mjs
```

Do not set `TMPDIR` to a long path for it: Chromium refuses long socket paths.

**By hand:** `cd B/dist && python3 -m http.server 8000`, then open the page. URL
parameters: `?scale=1` (one framebuffer pixel per CSS pixel), `?cmdline=init=/bin/fbdemo`
(kernel command line), `?debug` (trap messages in the log), `?base=URL` (where to fetch
`kernel.wasm` and the image), `?stats` (frame counters in `window.ucosStats`, for
`tools/bench/frames.mjs`). The page records boot phase times in `window.ucosBoot`.

## 4. Tests

Run the system suites before you say something works: `make B=build-NAME test` runs the
first six and fails if any check fails. Counts as of 2026-11-26.

| Suite | Command | Passes at |
|---|---|---|
| kernel | `node tools/run.mjs --build=B --init=/bin/ktest` | `ktest: 124 checks, 0 failed` |
| C library | `node tools/run.mjs --build=B --init=/bin/libctest` | `libctest: 298 checks, 0 failed` (292 if `/bin/sh` is not in the image) |
| shell and tools | `node tools/run.mjs --build=B --init=/bin/sh --input='sh /usr/share/tests/sh-tests.sh\nexit\n'` | `sh-tests: 238 checks, 238 passed, 0 failed` |
| sound | `node tools/tests/audio.mjs --build=B` | `audiotest: 37 checks, 0 failed` |
| network | `node tools/run.mjs --build=B --init=/bin/nettest` | `nettest: 89 checks, 0 failed` |
| router (HTTP path, offline) | `node tools/tests/router-http.mjs --build=B` | `routertest: 24 checks, 0 failed` |
| desktop | `node tools/shot.mjs --build=B --script=tools/tests/desktop.mjs` | six screenshots; look at them |
| browser | `node tools/browser.mjs --build=B --script=tools/tests/browser-desktop.mjs` | four screenshots; look at them |

The network and router suites are also in the image as `~/net-tests.sh` (see NETWORKING.md);
`node tools/tests/router-http.mjs --build=B --home-script` runs that script with the test server.

Component suites:

| Component | Where | How | Last reported |
| kernel fuzzer | `bin/kfuzz.c` | `node tools/run.mjs --build=B --init=/bin/kfuzz --args="regress"` replays the kept seeds, then runs ktest; `--args="SEED CALLS"` for a new seed | 7 kept seeds pass; one unexplained trap on seed 306, not reproduced |
|---|---|---|---|
| terminal parser | `bin/term/tests/test_vt.c`, `fuzz_vt.c` | native, `bin/term/tests/run_tests.sh` | 315 checks; 30 MB fuzz clean |
| terminal vs reference | `bin/term/tests/conformance.py` | native, needs `pyte` in a venv | 0 cell differences |
| terminal in the OS | `bin/term/tests/os/*.mjs` | `shot.mjs` | 68 checks |
| window manager | `bin/wm/tests/*.mjs` | `sh bin/wm/tests/run.sh build-NAME` (build with `PROGS="wm bar launcher uidemo term sh"`: `zoom.mjs` needs `term`) | 9 scripts, 142 checks (2026-11-26) |
| shell line editor | `bin/sh/test/le_test.c` | native: `cc bin/sh/test/le_test.c bin/sh/lineedit.c` | 42 checks |
| core tools | `tools/tests/coretools/` | native, against the host's tools: `node tools/tests/coretools/run.mjs [TOOL...]` (`--regen` rewrites expectations from the host, `--emit-os` writes the in-OS script); in the OS: `sh /usr/share/tests/coretools.sh [-v] [TOOL...]`; fuzzing: `node tools/tests/coretools/fuzz.mjs sed\|diff SECONDS` and `tools/tests/coretools/fuzzre.c`. On this machine the host's grep is ugrep and its find is bfs, not GNU | 19,072 native, 1,755 in the OS (2026-11-26; cases marked `@noos` run natively only) |
| interactive shell in the desktop | `bin/sh/test/interactive.mjs`, `welcome.mjs` | `shot.mjs` | 33 + 2 checks |
| shell conformance | `tools/tests/sh-posix.cases` (edit these), `rootfs/usr/share/tests/sh-posix.sh` (generated) | `node tools/tests/sh-posix.mjs --build=B` runs the cases in UC.OS in chunks of 60 and exits 1 on a failure; `--gen` regenerates the expected values with a host shell (set `TMPDIR` to a `build-*` directory: `/tmp` is a RAM disk here), `--host` runs them on the host; in the OS by hand: `sh /usr/share/tests/sh-posix.sh [FROM [TO]]` | 858 of 859; the known failure is umask-effect (no file modes on `main`) |
| shell job control | `bin/sh/test/jobs.mjs` | `shot.mjs` | 10 checks on a kernel with signals, 2 without |
| font rasterizer | `tools/ttftest.c` (+ `ttftest_ft.c` against FreeType) | native; build line in its header | all fonts, 6,000 mutated fonts clean |
| Doom | `bin/doom/tests/*.mjs`, `mkimage.sh` | `shot.mjs`; needs the data files in the build (`make WADS=DIR`, or `tools/get-doom-data.sh` first). `mkimage.sh` no longer puts a data file in the image: it arrives as a remote file. `launcher.mjs`, `message.mjs`, `import.mjs` and `browser.mjs` cover the launcher entries, the messages, a dropped file and Chromium | play, demo, window tests pass |
| Linux programs (`lx`) | `bin/lx/tests/` | native: `sh bin/lx/tests/run.sh` (add `LX_SCRATCH=DIR` after `LX_SCRATCH=DIR sh bin/lx/tests/fetch-debian.sh`, which downloads Debian riscv64 packages outside the repository, for the Debian milestones); in UC.OS: `sh bin/lx/tests/mkimage.sh BUILD FILE:PATH...` then for example `node tools/run.mjs --build=BUILD --init=/bin/lx --args="/usr/bin/busybox sh /lx/m2.sh /usr/bin/busybox /tmp"` (busybox script) or `--args="/usr/bin/dash /lx/m34.sh"` (dynamic programs), with the small Debian root from `bin/lx/tests/mkimage-debian.sh`. The scripts need exactly these arguments: without them the busybox script reports 29 failures that are not real. Full commands in `docs/LINUX-BINARIES.md` section 11 | 23 of 23 native; in the OS 37 of 37 (busybox) and 10 of 10 (dynamic programs), 2026-11-26 |
| libarc and the archive tools | `libarc/tests/run.sh` | native, one command: `bash libarc/tests/run.sh` (add `ARC_DEBS=dir` to also compare real `.deb` files with GNU tar and bsdtar). Needs host gzip, xz, zstd, GNU tar, bsdtar, binutils ar, python3 | see STATUS.md |
| libarc fuzzing | `libarc/tests/fuzz.c` | native, build line in its header; `./fuzz SECONDS seed...` | see STATUS.md |
| archive tools in the OS | `rootfs/usr/share/tests/arc-tests.sh` | `node tools/run.mjs --build=B --init=/bin/sh --input='sh /usr/share/tests/arc-tests.sh\nexit\n'` (needs `sh echo cat cp ls mkdir rm head seq wc gzip tar ar sha256sum` in the image) | `arc-tests: 52 checks, 52 passed` |
| real packages in the OS | `libarc/tests/os-debs.sh` | `sh libarc/tests/os-debs.sh BUILD DEB...`: extracts each `.deb` inside UC.OS with `ar x` and `tar xf` and compares the file contents with the host's GNU tar | 7 of 7 identical (2026-11-26) |
| vi, htop, fetch | native builds driven through a pty from Python | scripts were kept in a scratch directory and are NOT in the repo | vi 129, htop 72, fetch 64 |
| C compiler and linker | `bin/cc/tests/` | `sh bin/cc/tests/build.sh B` once, then `sh bin/cc/tests/run.sh B` | 37: 19 programs compared with clang, 3 mixed links, 15 diagnostics (2026-11-26) |
| cc random expressions | `bin/cc/tests/fuzz.py`, `fuzz.sh` | `sh bin/cc/tests/fuzz.sh B SEED COUNT` (keep runs under a minute) | 6,750 expressions, 0 differences |
| programs built by cc, in the OS | `bin/cc/tests/os.sh`, `buildall.sh` | `sh bin/cc/tests/os.sh B "prog ..."` | see STATUS.md |

| make vs GNU Make | `bin/make/tests/cases/*.mk`, `expected/` | native: `node bin/make/tests/run.mjs` (builds make with `cc` and ASan; compares with the host's GNU Make 4.x, or with the stored outputs if there is none) | see STATUS.md |
| make in the OS | `bin/make/tests/os/cases.mk` -> `rootfs/usr/share/tests/make/` | `sh bin/make/tests/os/run.sh build-NAME` (builds the image and runs the cases inside UC.OS) | see STATUS.md |
| shell features recipes use | `rootfs/usr/share/tests/make/shell-survey.sh` | `node tools/run.mjs --build=B --init=/bin/sh --input='sh /usr/share/tests/make/shell-survey.sh\nexit\n'` | see STATUS.md |
| make speed | `bin/make/tests/perf/gen.mk` (also in the image as `/usr/share/tests/make/perf-gen.mk`) | instructions at the top of the file | PERFORMANCE.md |
| package manager | `libpkg/tests/run.sh` | native, no network: `sh libpkg/tests/run.sh [OUTDIR]` (builds with ASan and UBSan, makes .deb fixtures from text with tar and ar) | 272 checks (235 unit checks inside) |
| package manager in the OS | `libpkg/tests/os-test.sh`, `os/pkg-os.sh` | `sh libpkg/tests/os-test.sh build-NAME` (builds an image, stages fixtures, boots under Node) | 37 checks |
| package manager on real repositories | `libpkg/tests/real-repos.sh`, `check-plan.py` | optional, DOWNLOADS about 75 MB of Debian and Ubuntu indexes; resolves packages per distribution and architecture | 48 resolutions; plans checked by `check-plan.py` |
| maintainer script survey | `libpkg/tests/survey-scripts.py` | optional, DOWNLOADS the first few KiB of about 50 .debs | |
| full index inside the OS | `libpkg/tests/os-bigindex.sh` | optional: `sh libpkg/tests/os-bigindex.sh PACKAGES_FILE [BUILD] [PKG...]` with a plain `Packages` or a `Packages.xz`; runs `apt update` on it inside UC.OS and samples apt's memory with `ps` | about 35 MB peak for Debian 13 `main` amd64 (68,829 stanzas), 2026-11-26 |
| real Debian packages installed in the OS | `libpkg/tests/os-real.sh` | optional: `sh libpkg/tests/os-real.sh BUILD DEBIAN_DEB... -- UBUNTU_DEB...` (the .deb files are not in the repository): `apt update`, `apt install -y hello`, `dpkg -l/-L/-V`, remove, purge, autoremove, the name-length limit, inside UC.OS | passes with Debian hello and its three dependencies (2026-11-26) |

Gap to know about: the vi/htop/fetch test scripts are not in the repository.

### Testing make

- A case file holds several cases, each starting with `##### name`, then `--- Makefile`,
  `--- file NAME` and `--- run` sections. Every `run` line is a shell command run in a
  scratch directory; standard output, standard error, the exit status and every file left
  behind are compared. `node bin/make/tests/run.mjs NAME` runs the files or cases whose
  name contains NAME; `-v` lists passes; `--keep` keeps the scratch directories.
- After adding or changing cases, refresh the stored outputs with `--update` (needs GNU
  Make 4.x) so the suite still runs on a machine without GNU Make.
- Use `touch -d '2001-01-01' file` for old files instead of `sleep`: the host file system
  has nanosecond times. In-OS cases must use `sleep 1`: UC.OS keeps whole seconds.
- After editing `bin/make/tests/os/cases.mk`, run `node bin/make/tests/os/gen.mjs`: it
  writes the case tree into `rootfs/usr/share/tests/make/` with GNU Make's output.

### Writing a kernel test

`bin/ktest.c` is one program that re-runs itself as helper children
(`ktest child-NAME`). Add a child mode at the top if you need a second process, add
`CHECK("what", condition)` lines, keep each check independent of timing where you can.
It must pass when started directly as pid 1.

### Working on the C compiler (`bin/cc/`, `bin/ld/`)

`sh bin/cc/tests/build.sh build-NAME` builds `cc` and `ld` with the host compiler (with
AddressSanitizer and UBSan; set `ASAN_OPTIONS=detect_leaks=0`, the compiler never frees
memory on purpose) into `build-NAME/native/`, plus `build-NAME/sysroot/`, a copy of what
the image ships under `/usr`. Then `run.sh` builds every program in `bin/cc/tests/c/`
with clang and with cc against the same libc, runs both under `bin/cc/tests/run.mjs` (a
small stand-in for the kernel: write, read, files, exit, clock) and compares output and
exit status; `mix/` links half clang, half cc objects; `err/` checks diagnostics. Pass
`wasm-ld` as the second argument to link cc's objects with wasm-ld instead of our ld.
`cc -S` writes a readable listing of the generated code.

## 5. Common tasks

**Add a program.** Create `bin/NAME.c`. Include the libc headers you need. It appears
as `/bin/NAME`. For a graphical program make `bin/NAME/` with a `USE_GFX` file and use
`libui/ui.h`; `bin/uidemo/` is the smallest example, `bin/fbdemo/` draws without a window
manager.

**Add a system call.** See ARCHITECTURE.md section 3.

**Add a device.** Give it an id in the `DEV_*` enum (`kernel/kernel.h`), create the node
with `vfs_mkdev()` in an init function called from `kmain`, return a `struct file` for it
from `dev_open()` in `kernel/main.c`, and implement `struct fops`.

**Add a launcher entry.** `rootfs/usr/share/applications/NAME.app` with `name=`, `exec=`,
`comment=`. Use `exec=term -e PROGRAM` for terminal programs.

**Add a theme.** `rootfs/usr/share/themes/NAME.theme` (format in `libui/theme.h`).
`Alt+t` cycles through the themes that exist.

**Change a default dotfile.** They are in `rootfs/etc/` and `rootfs/home/user/`. They are
written to teach: keep every line commented.

**Work on the package manager.** The logic is plain C in `libpkg/` and builds for the
host: `sh libpkg/tests/build-native.sh OUTDIR` gives `OUTDIR/bin/dpkg`, `apt` and the
rest. A host build refuses to touch `/`: always pass `--root=DIR` (dpkg) or `-o Dir=DIR`
(apt), or set `DPKG_ROOT`. `UCOS_PKG_CONFDIR` points dpkg at another `dpkg.cfg`
directory (tests use an empty one). Maintainer scripts in another root need
`--force-script-chrootless`. The archive formats come from the real `libarc`, natively as well (`build-native.sh`
compiles `libpkg/*.c` and `libarc/*.c`).

**Start a process from C.** `spawn(path, argv, envp, fdmap)` or `spawnp(file, argv,
fdmap)`. For a pipeline: create pipes with `pipe2(p, O_CLOEXEC)`, spawn each stage with
an `fdmap`, close your copies, `waitpid`. `bin/ktest.c` and `bin/sh/exec.c` show it.

## 6. Debugging

- `--debug` (runners) or `?debug` (page) prints why a process died: `memory access out
  of bounds`, `unreachable`, `indirect call signature mismatch`, `null function`.
- `kprintf()` in the kernel and `fprintf(stderr, ...)` in programs both reach the host
  console (the terminal emulator does not show the kernel console).
- A process "killed by signal 11" trapped. Signal 4 means its module failed to load.
- `llvm-objdump -d file` disassembles wasm objects and linked programs.
- A hang is usually a missed wake-up: something changed state without `wq_wake` on the
  queue a sleeper registered on. `ps` and `htop` show state `S` for sleeping.
- If results look impossible, suspect a stale `kernel.wasm` in your build directory and
  rebuild into a fresh `B=`.

## 7. Traps

1. **Exact signatures.** wasm type-checks calls. Old C that calls a function through
   the wrong prototype, or registers `int f(void)` as a `void (*)(void)`, links with a
   warning (fatal here) or traps at run time. Fix the declaration or add a wrapper.
2. **`main` is not special under `-ffreestanding`.** That is why two `crt0` objects exist.
3. **Weak definitions and archives.** If your object file defines a symbol weakly, the
   linker considers it resolved and never pulls the real definition out of `libc.a` or
   `libui.a`. For an optional dependency, declare the function weak, test its address at
   run time, and force the archive member with `-Wl,-u,symbol` in the program's `CFLAGS`
   (see `bin/doom/CFLAGS`).
4. **Restarted system calls.** See ARCHITECTURE.md section 4. In user code this shows
   up as short writes: use the libc's `write()`, which loops.
5. **One thread, cooperative.** Idle programs must block in `poll`, `read` or
   `nanosleep`. A loop that makes system calls is preempted every 8 ms; a loop that makes
   none freezes everything. In long pure computations, make a cheap call now and then.
6. **Benchmarks.** V8 has no on-stack replacement for wasm: a loop that is entered once
   stays in baseline code forever. Put the timed loop in a `noinline` function, call it
   many times, report the best batch and the first (cold) batch. Browser clocks tick
   every 0.1 ms (Chrome) or about 1 ms (Firefox): batches must be much longer than that.
7. **No `setjmp`.** Code that needs it has to be restructured.
8. **32-bit `long`.** `long` and pointers are 32 bits. File offsets are 32 bits.
9. **Text encoding.** The host decodes console output as UTF-8 in streaming mode.
   `printf("%-10s")` pads by bytes, not by characters.
10. **Wide tables in narrow terminals.** A tiled terminal can be 60 columns wide. Read
    the width with `TIOCGWINSZ` and adapt.
11. **Themes at run time.** Programs must not cache the theme from `$THEME`; the file
    `~/.config/ucos/theme` is the source of truth and can change while they run.
12. **The safety check on shell commands** (Claude Code specific): commands with complex
    here-documents, or `rm -rf` on a variable path, are blocked. Write files with the
    file tools and run plain commands.

## 8. Code conventions

Branch names, commit messages and pull requests: see `CONTRIBUTING.md`.


- C11, compact, readable; comments say why, not what. Two-space indent as in the tree.
- Every source file of ours starts with `// SPDX-License-Identifier: GPL-2.0-or-later`
  (`#` for make, shell, Python and assembly). Upstream Doom files keep their own headers.
- Kernel code takes user pointers as `u32` and never dereferences them.
- No em-dashes in comments, strings, dotfiles or docs.
- User-visible text follows BRAND.md: calm, plain, short.
- Programs must work both on a terminal and when their output is a pipe or the console.

## 9. Several people and agents in one tree

This project was built by one coordinating session plus parallel subagents. What made
that work, and what broke:

- **Strict file ownership.** Each agent owns named paths and edits nothing else. If it
  needs something outside them, it reports it instead of editing. Give every agent the
  list in its brief.
- **One build directory each** (`make B=build-NAME`).
- **Interfaces first.** The coordinator writes the shared headers (`abi/abi.h`,
  `libui/ui.h`, `libui/theme.h`) before agents start, so they can work against a contract
  while the implementation is still being written.
- **Each agent tests natively where it can** (the API is Linux-shaped, so terminal
  programs compile with the host `cc` behind a small `#ifdef __wasm__` layer) and in the
  OS with the runners.
- **Briefs state the hard facts**: no fork, no signals, single-threaded and cooperative,
  the console is not a terminal, exact signatures, no em-dashes, never delete directories,
  report only what you ran.
- **When the kernel or the host changes, tell every running agent to rebuild**, because
  `kernel.wasm` and `boot/host.mjs` must match.
- **Never put a deliberately broken line in the shared tree**, not even for ten seconds.
  It was done once to prove a regression test; another agent's build read the file in
  that window and then looked up to date. Copy the tree somewhere private instead.
- **Reports must be factual**: what was built, test counts, what was not tested, what
  was worked around. Verify the important claims yourself before repeating them.

When you (an agent) are started by a coordinator, your brief overrides nothing in
CLAUDE.md; if they conflict, stop and ask.
