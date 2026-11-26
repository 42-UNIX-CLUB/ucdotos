# Status

Last updated: 2026-11-26, about 02:00 (Lisbon time). This file goes stale fastest:
update it whenever you finish, break or start something.

## 1. What works today

One image (`make B=... dist`, 50 programs, 7.8 MB of which 5.8 MB is fonts; 3.8 MB as the
`initrd.tar.gz` the page loads when it is there) boots
in Chromium to a tiling desktop.

| Area | State | Evidence |
|---|---|---|
| Kernel | processes, files, pipes, poll, ptys with line discipline and Ctrl-C, unix sockets, framebuffer, input, time slicing, CPU accounting; audited and fuzzed on 2026-11-26 (branch `fix/kernel-hardening`, not merged yet): inodes are freed, many crash, hang and leak bugs fixed | `ktest`: 109 of 109; `kfuzz` (system call fuzzer, `--init=/bin/kfuzz --args="SEED ITERATIONS"`, or `--args=regress` for the kept seeds): 82 campaign runs, 5.6 million calls; one trap inside the kernel seen once and not reproduced (see below) |
| Host and page | boots in Chromium 158 and under Node 26; real keyboard and mouse verified through the DevTools protocol | `tools/tests/browser-desktop.mjs` |
| C library | stdio, scanf, math, time, termios, getopt and the rest | `libctest`: 290 of 290 |
| Shell and 30 tools, `bc` | pipelines, redirections, control flow, functions, substitution, globbing; line editor, history, Tab completion, Ctrl-C (also stops shell loops) and Ctrl-D verified in the desktop | `sh-tests`: 238 of 238; `bin/sh/test/interactive.mjs`: 33 of 33 |
| Font rasterizer, drawing | bitmaps match FreeType's size and position; fuzzed | `tools/ttftest.c` |
| Window manager, bar, launcher | tiling, workspaces with a slide (`animation_ms`, 160 by default, 0 for none; up: picture moves left; down: right; drawn live each frame, so it uses no extra memory and windows keep updating; the bar stays fixed unless a fullscreen window hides it on one side, then it travels with the workspace that shows it; switching back mid-slide continues smoothly, any other switch mid-slide jumps), fullscreen, floating, themes, bad clients survive; zoom keys (Ctrl+= or Ctrl++, Ctrl+-, Ctrl+0) are taken by the manager and sent to the focused window as `UI_EV_ZOOM` | `bin/wm/tests/run.sh`: 8 of 8 scripts, 126 checks (90 older + 20 in `slide.mjs` + 16 in `zoom.mjs`). The tests close the welcome terminal at boot because they expect an empty desktop; `zoom.mjs` keeps it and needs `term` in the image |
| Terminal emulator | xterm-compatible; matches a reference emulator cell for cell on real program output. On `UI_EV_ZOOM` the font size moves 2 px per step (8 to 72 px), sharp, and the grid reflows | 315 + 68 checks; zoom in `bin/wm/tests/zoom.mjs`. `bin/term/tests/os/window.mjs` fails 1 of 4 ("second window is tiled narrower") without this change too: it predates the master/stack layout |
| vi, htop, fetch, sysbench | work in the desktop at one, two and three tiled windows, in both themes, and follow a theme switch live; sysbench and fetch adapt to coarse browser clocks | 360 native checks (those scripts are not in the repo); desktop scripts `bin/htop/shots.mjs`, `bin/vi/shots.mjs`, `bin/fetch/shots.mjs`, `bin/fetch/sysbench-shots.mjs` |
| Doom | runs full-screen and in a window, 35 fps, saves and loads | needs a WAD: none is in the image |
| Club content | the intro deck as files in `~/unix_club`, `man unix-club`, commented dotfiles | |
| Licence | GPL-2.0-or-later: `LICENSE`, `README.md`, SPDX tags | |

Committed: one commit on `main` (the first), pushed by an owner. Everything after it is uncommitted in the working tree: time slicing, `intr`, the process CPU clock, new `sysinfo` meaning, the browser test driver, the `crt0` argument fix, round 2 of the shell and the apps, and the context files.

## 2. Known problems

| Problem | Detail |
|---|---|
| Large directories are slow | A directory is a linked list. Creating a file in a directory of 40,000 entries takes about 1.6 ms (80 us at 2,500); listing it takes 0.76 s. 100,000 entries would take minutes to create. Options are in the kernel hardening report. |
| One call can grow kernel memory by up to 2 GiB | `ftruncate` or a write at a large offset makes a file that big for real (no sparse files), and wasm memory never shrinks. Whether to cap file size is an open question for the owners. |
| A trap inside the kernel looks like a program crash | The host reports it as the calling process dying with signal 11; the kernel's state after it is unknown. Nothing says so loudly. |
| One kernel trap from kfuzz is not understood | Seed 306 (4 workers, 20,000 calls) once trapped in `copy_from_user` with "function signature mismatch": the calling process's copy functions were not attached (a null table entry). Not reproduced in 3 reruns or 41 further runs with lifecycle checks on the host calls. |
| ktest's preemption check is sensitive to load | "a spinning process is preempted" failed after 5 fuzz runs out of about 120 (the parent slept 150 to 257 ms instead of about 40), always while several fuzz runs shared the machine; the same seeds passed when rerun. Cause not proven. |
| A pure CPU loop freezes the tab | Time slicing only works at system calls. Shell loops are covered (the shell calls `intr()` in every loop iteration, and Ctrl-C stops them). A C program that computes without calling the kernel still freezes everything. |
| Ctrl-C in a non-interactive script is untested | The code path exists (exit status 130); the test runners cannot send Ctrl-C to it. Also untested: Ctrl-C while the `read` builtin waits. |
| Speed figures in a browser are noisier and higher than under Node | Reading the clock costs about 120 ns in Chrome (330 ns before the performance pass) and it ticks every 0.1 ms. sysbench reports the resolution, shows `n/a` for the cold column there, and times rows on the process CPU clock. Run-to-run spread in Chromium is about 20%. See PERFORMANCE.md. |
| Doom has no game data in the image | The launcher entry starts it and it exits with "No IWAD file was found". Waits for an owner decision. |
| Doom has no sound | No audio device in the kernel. |
| Process start is slow | About 72 to 80 microseconds warm under Node (125 to 190 in Chromium), over 1 ms cold: every start instantiates a module and allocates its memory through the browser (PERFORMANCE.md section 4; the ideas tried there did not help in Chromium). |
| `clock_gettime` costs about 90 to 100 ns under Node and 190 to 220 ns in Chromium | The time comes from JavaScript (it was 100 to 160 and 350 to 450 ns before the performance pass). |
| `malloc`/`free` are slow under churn | About 55 times slower than glibc in `ubench` (first-fit list, every `free` walks it). See PERFORMANCE.md section 3. |
| The login message wraps in a 60-column terminal | `rootfs/etc/motd` lines are up to 68 columns. |
| Nothing persists | A reload starts from the image. |
| Times are UTC | No time zone data. |
| Window manager reaps children late | There is no child-exit notification; it reaps when it next wakes. |
| Terminal: no clipboard, italic drawn upright, mouse click reporting untested | |
| vi | every character is one column wide; with wrapped lines H, M, L and Ctrl-F/B count buffer lines, not screen rows |
| fetch measures `getppid` in 40 ms | too short for the engine to finish optimising, so it reads 15 to 40 ns where sysbench reports the warm figure |
| sysbench in a half-height window | its first lines scroll off; the table and footnote stay visible |
| Shell, without fork | `&` on a builtin runs in the foreground; a builtin at the end of a pipeline runs in the shell; functions defined inside `$(...)` stay defined |
| Safari is untested | JSPI support there rests on other projects' reports. |
| Firefox is untested | Only Chromium and Node were run. |
| Test scripts for vi, htop, fetch are not in the repo | They were written in a scratch directory. |
| Stray build directories | `build-*` directories in the working tree are git-ignored and can be deleted by an owner. |

## 3. Not built

- The Worker half of the hybrid process model (real preemption, several cores). A design
  study with working prototypes (shared memory, messages, yield points, the whole OS in a
  Worker) and measurements is in `docs/WORKER-PROCESSES.md`; nothing is integrated, and the
  choice is waiting for the owners.
- `fork`, signals, threads, shared memory.
- Persistence, networking, sound.
- `/proc` contents, users and permissions, time zones, a clipboard.
- The minishell port (waiting for the repo from an owner).
- Running Linux programs: no libc port for recompiling them, no binary emulation.
- Unpacking the compressed image while it downloads (the page unpacks `initrd.tar.gz` after the download; whether to ship the `.gz` at all is open, see PERFORMANCE.md section 6).

## 4. Waiting for an owner

See DECISIONS.md section 3: Doom game data, Doom A/D keys, hosting, persistence,
networking, when to build the Worker path, sound, the minishell repo, a linking
exception for the libraries, the copyright holder line, the brand theme's placeholder
colours, clearing public use of the 42 glyphs, and the browser floor.

## 5. How this was built, in order

1. Research of every way to run an OS in a browser; the owners chose to build their own.
2. Kernel core, libc core, boot loader, Node test runner: first boot, self-test green.
3. Seven parallel subagents: font rasterizer and drawing; C library; shell and tools;
   editor, htop, fetch, speed demo; window manager, libui, bar, launcher; terminal
   emulator; Doom. Meanwhile the kernel gained ptys, sockets, framebuffer and input.
4. Two kernel bugs found by subagents and fixed: wrong process identity when two
   processes woke in the same instant (fixed with the per-process trampoline); a stale
   build caused by a temporary edit in the shared tree (process lesson, see
   DEVELOPMENT.md section 9).
5. Integration: one image, booted in Chromium. Licence applied. First commit.
6. Time slicing, Ctrl-C for the shell, real-browser test driver. Round 2 of fixes.
