# UC.OS architecture

How the system works, layer by layer. Everything here describes the code as it is on
2026-11-26. If you change behaviour, change this file in the same piece of work.

Contents: 1 the big picture, 2 the host, 3 the system call path, 4 blocking and
scheduling, 5 processes, 6 memory, 7 files and devices, 8 terminals, 9 sockets,
10 screen and input, 11 the desktop stack, 12 the C library, 13 the toolchain,
14 limits and what does not exist.

## 1. The big picture

A browser executes only JavaScript and WebAssembly. Nothing can run "below" WebAssembly
in a page. UC.OS therefore compiles the kernel itself to WebAssembly and makes every
program a separate WebAssembly module.

```
+--------------------------------------------------------------------------------+
| page: boot/index.html + boot/page.mjs                                          |
|   a <canvas>, key/mouse listeners, a boot log; fetches kernel.wasm, initrd.tar |
+--------------------------------------------------------------------------------+
| host: boot/host.mjs  ("firmware")                                              |
|   instantiates the kernel; instantiates programs when the kernel asks;         |
|   parks and resumes program stacks (JSPI); clock, random, console log          |
+--------------------------------------------------------------------------------+
| kernel.wasm (kernel/*.c, about 37 KB)            one instance, its own memory   |
|   processes, fds, ramfs, pipes, ptys, unix sockets, /dev/fb0, /dev/input/event0 |
+----------------+----------------+----------------+----------------+------------+
| init (pid 1)   | wm             | bar, launcher  | term           | sh, tools  |
| one wasm instance and one linear memory per process; libc is linked statically |
+--------------------------------------------------------------------------------+
```

Design choices behind this (all made by the owners, see DECISIONS.md):
- kernel compiled straight to wasm, not a kernel for a real CPU inside an emulator;
- one thread, direct wasm-to-wasm system calls, JSPI for blocking ("hybrid": a Worker
  path for CPU-heavy programs is planned and not built);
- a separate memory per process, bridged by a wasm copy shim;
- `spawn` instead of `fork`, with fork kept possible for later;
- Linux-shaped system calls;
- the window manager is an ordinary program, the kernel only exposes devices.

## 2. The host (`boot/host.mjs`)

`boot({ kernel, initrd, cmdline, screen, log, onIdle, onFrame, debug, extend })` does this
(`extend` is a research hook, off by default, used only by the Worker prototype in
`boot/worker-proto/`; see WORKER-PROCESSES.md):

1. Compiles two hand-assembled modules that are byte arrays in the source: `SHIM` and
   `TRAMPOLINE` (see sections 3 and 6).
2. Instantiates the kernel with the import module `host`:

   | Import | Meaning |
   |---|---|
   | `log(ptr, len)` | console output (kprintf, and writes to /dev/console) |
   | `spawn(pid, ptr, len, cacheable)` | instantiate the wasm program stored at ptr..ptr+len in kernel memory as process pid |
   | `wake(pid)` | resume a process parked in `block()` |
   | `reap(pid)` | the process slot was freed; drop its instance |
   | `now()` | monotonic milliseconds (`performance.now()`) |
   | `epoch()` | milliseconds since 1970 |
   | `random()` | 32 random bits |
   | `memory()` | bytes of memory the device reports (default 4 GiB) |
   | `fb_dirty()` | the framebuffer changed; show it on the next animation frame |

3. Copies the initrd (a ustar archive) and the command line into kernel memory with the
   kernel export `boot_alloc`, calls `fb_resize(w, h)` if there is a screen, then
   `kmain(initrd, len, cmdline)`.

Kernel command line: `init=/path/to/program -- arg1 arg2`. Default init is `/bin/init`.

Kernel exports the host uses: `kmain boot_alloc syscall_pid proc_attach proc_state
proc_crashed proc_account block_timeout console_input input_event fb_resize fb_addr
fb_width fb_height memory __indirect_function_table`. (`set_current` still exists and is
harmless; the kernel no longer depends on it.)

The object `boot()` returns: `k` (kernel exports), `memory`, `procs`, `frame()`,
`resize(w, h)`, `input(type, code, value)`, `consoleInput(str)`, `consoleEOF()`.

`checkSupport()` reports what the browser lacks: JSPI (`WebAssembly.Suspending`) and
multi-memory. Browser floor: Chrome/Edge 137+, Firefox 153+, Safari 27+ (Safari is
unverified by us).

### Starting a process

When the kernel calls `host.spawn(pid, ptr, len, cacheable)`:

1. Compile the module (cached by address and length when the file is an unmodified
   initrd file, which is what `cacheable` means).
2. Instantiate a `TRAMPOLINE` with `{ k: { sys: kernel.syscall_pid }, p: { pid } }`.
3. Instantiate the program with `{ host: { syscall: trampoline.c, block } }`.
   `block` is a `WebAssembly.Suspending` function created for this process.
4. Instantiate a `SHIM` with the kernel's memory and the program's exported memory, put
   its three functions into the kernel's function table (three slots, recycled), and
   tell the kernel with `proc_attach(pid, slot, slot+1, slot+2)`.
5. Call `WebAssembly.promising(program._start)()`.

When that promise ends: a thrown `EXIT` symbol is the normal way out (see section 4).
Anything else is a wasm trap (out-of-bounds access, `unreachable`, stack overflow, bad
indirect call): the host calls `proc_crashed(pid, 11)` and the kernel records death by
signal 11. A module that fails to compile or instantiate is `proc_crashed(pid, 4)`.

## 3. The system call path

```
program:    __syscall6(nr, a, b, c, d, e, f)          libc/include/sys/syscall.h (inline)
              -> import host.syscall                   = trampoline export "c" (wasm)
trampoline:   k.sys(pid, nr, a, b, c, d, e, f)        pid is an immutable global set by the host
kernel:       syscall_pid(pid, nr, ...)               kernel/syscall.c: sets `cur`, dispatches
```

- No JavaScript runs on this path. Measured cost of the cheapest call (`getppid`): about
  11 to 13 ns warm under Node and 14 to 18 ns in Chromium, 100 to 400 ns for the first cold
  calls. See PERFORMANCE.md.
- The pid comes from the trampoline, not from the program, so a program cannot
  impersonate another process, and the kernel never depends on the order in which the
  host resumes stacks. (An earlier design set `cur` from JavaScript on resume; two
  processes woken in the same instant then got each other's identity. `bin/ktest.c` has
  regression tests for this.)
- All arguments and the result are 32-bit (`long` is 32 bits on wasm32). Negative
  results in the range -1..-4095 are `-errno`, as on Linux. `__sysret()` in the libc turns
  them into `-1` plus `errno`.
- The result `-ERESTARTSYS` (512) means "park me, then call again". The inline wrapper
  loops: `while ((r = host.syscall(...)) == -512) host.block();`.

Call numbers follow Linux's generic table (`include/uapi/asm-generic/unistd.h`), the
one every new Linux architecture uses. Implemented today (47 cases):

`getcwd dup dup3 fcntl ioctl mkdirat unlinkat renameat utimensat ftruncate chdir openat
close pipe2 getdents64 lseek read write pread64 pwrite64 ppoll newfstatat fstat exit
exit_group nanosleep clock_gettime sched_yield kill uname getpid getppid getuid sysinfo
setpgid getpgid setsid socket bind listen accept connect wait4 getrandom`

Private calls (Linux has no equivalent):

| Number | Name | Meaning |
|---|---|---|
| 500 | `spawn(path, argv, envp, fdmap)` | start a program; returns the pid |
| 501 | `getargs(buf, cap)` | the argument and environment blob for `_start` |
| 502 | `procinfo(index, struct procinfo *)` | one row of the process table; `-ESRCH` past the end |
| 503 | `intr()` | 1 if Ctrl-C was pressed for this process since the last call, then clears it |

Anything else returns `-ENOSYS`.

### Adding a system call

1. Add the number (Linux's, if Linux has the call) and any struct to `abi/abi.h`.
2. Add a `case` in `syscall_pid` and a `static long sys_xxx(...)` in `kernel/syscall.c`.
   Take user pointers as `u32` and use `copy_from_user`, `copy_to_user`, `str_from_user`.
3. If it can wait: register on a wait queue and `return block(timeout_ms)`. Remember the
   whole call runs again on wake-up (section 4).
4. Add the wrapper in `libc/src/` and its declaration in `libc/include/`.
5. Add checks to `bin/ktest.c`.

## 4. Blocking, waking and time slicing

The kernel never suspends. Every entry (a system call, or a host call such as
`input_event`) runs to completion on the kernel's single stack. Waiting is done by the
caller, outside the kernel:

1. Kernel code that must wait adds the current process to a wait queue (`wq_add(&q)`)
   and returns `block(timeout_ms)`, which is `-ERESTARTSYS`. A wait queue (`waitq`) is a
   64-bit mask of pids, which is why `NPROC` is 64.
2. The libc wrapper calls `host.block()`. The host asks the kernel for the timeout
   (`block_timeout(pid)`: negative = until woken, 0 = just yield, positive = milliseconds)
   and awaits a promise. JSPI suspends that program's whole wasm stack.
3. Something changes: kernel code calls `wq_wake(&q)`, which calls `host.wake(pid)` for
   every pid in the mask. The host resolves the promise; the stack resumes later.
4. The wrapper calls the same system call again, from the start.

Consequences every kernel author must respect:
- **A blocking call is restarted, not continued.** Do nothing irreversible before you
  know you will not block, or make the partial work visible as a short count (pipes,
  sockets and ttys return short writes; the libc's `write()` loops).
- **Timeouts survive the restart in `proc->sleep_until`.** `nanosleep`, `ppoll` and the
  tty's timed read set it on first entry and clear it when they finish.
- **Spurious wake-ups are normal.** The call simply re-checks and blocks again.
- **A wake-up for a process that is not parked is ignored**, which is correct because it
  will re-check on its next call.

Ending a process: `exit` marks the process a zombie and returns `-ERESTARTSYS`. The
wrapper calls `host.block()`, the host sees the process is no longer running and throws
`EXIT`, which unwinds the wasm stack. Killing a parked process is the same: the kernel
marks it and calls `host.wake`, and the resumed `block()` throws.

### Time slicing

Nothing can interrupt running WebAssembly, so the kernel takes the CPU back at system
call boundaries. Every 256th call it reads the clock (reading it costs about 400 ns in Chrome, so not more often); if the process has run longer than
`SLICE_MS` (8 ms, `kernel/kernel.h`) since it last blocked, the call returns `block(0)`
before doing anything. The host resumes the process after one turn of the event loop,
using a `MessageChannel` task (not `setTimeout(0)`, which browsers clamp to 4 ms). On
every such yield the host first wakes sleeping processes whose deadline has passed,
because some event loops serve a stream of messages ahead of timers.

Verified in Chromium: with `yes > /dev/null` running (about 27 million system calls per
second), new windows open, other shells work and Ctrl-C kills it.

**Not covered:** a program that loops without making any system call. The shell is
being changed to call `intr()` in its loops for this reason. A pure CPU loop in a C
program still freezes the tab. The planned fix is the Worker half of the hybrid model.

### CPU accounting

The host tells the kernel how long a process ran each time it parks
(`proc_account(pid, ms)`); `procinfo.cpu_us` reports it. `procinfo.state` is 1 running,
2 zombie, 3 sleeping.

## 5. Processes (`kernel/proc.c`)

- Table of `NPROC` = 64 slots; pid = slot index; pid 1 is init; pids are reused at once.
- `struct proc`: state, pid, ppid, pgid, sid, exit code, 64 file descriptors (`NFD`) with
  close-on-exec flags, cwd, the three shim function pointers, the argument blob, wait
  bookkeeping, counters.
- **spawn**: the path must be a regular file starting with the wasm magic (`-ENOEXEC`
  otherwise). The child inherits cwd, pgid, sid and every descriptor not marked
  close-on-exec. `fdmap[3]` names the parent descriptors that become the child's 0, 1, 2
  (-1 = inherit as is). Arguments and environment travel as one blob of at most 64 KiB:
  `u32 argc, u32 envc`, then the strings. `libc/crt/crt0.c` fetches it with `getargs`.
- **exit and wait**: exit closes descriptors, hands children to init, wakes the parent.
  If nobody can wait for it, the slot is freed at once. `wait4` supports a pid or -1 and
  `WNOHANG`. Status encoding as on Linux: `(code & 0xff) << 8` for an exit, the signal
  number for a kill or crash.
- **No signals.** `kill(pid, sig)` terminates the target (status = sig). Signal 0 only
  tests. The libc's `signal()` records handlers that are never called.
- **Job control, minimal**: `setpgid`, `getpgid`, `setsid`, and the tty's foreground
  group (section 8).
- There are no users or permissions. `getuid` returns 1000.

## 6. Memory

- The kernel has one linear memory. Its heap (`kernel/klib.c`) is a first-fit free list
  with coalescing, grown 1 MiB at a time with `memory.grow`. `kmalloc kzalloc krealloc
  kfree`. wasm memory never shrinks.
- Each process has its own linear memory, exported as `memory`. Its `malloc`
  (`libc/src/malloc.c`) is the same allocator and grows its own memory with no system
  call, at least one 64 KiB page at a time. The program stack is 1 MiB, placed first in
  memory (`--stack-first`), so a stack overflow traps instead of corrupting data.
- Because memory never shrinks, code that replaces a big buffer frees the old one before
  allocating the new one (window pixels in libui and the wm, the kernel framebuffer, the
  wallpaper). Allocating first would leave both sizes claimed for good.
- **User memory access.** The kernel cannot address another instance's memory directly.
  Per process, the host instantiates `SHIM`, a module that imports two memories (kernel
  `k`, process `p`) and exports `i(kdst, usrc, len)`, `o(udst, ksrc, len)` and `s()`
  (process size in pages). The kernel calls them through function pointers:
  `copy_from_user`, `copy_to_user`, `str_from_user` in `kernel/proc.c`. Each checks the
  range against `s()` first and returns `-EFAULT`, because a trap inside the kernel
  would leave its stack pointer wrong.
- A buggy program cannot corrupt the kernel or another program. Hostile code is not a
  design goal; nobody has audited it for that.
- There is no shared memory between processes. Pixels and data move by copying through
  sockets and pipes.

## 7. Files and devices (`kernel/vfs.c`, `kernel/dev.c`)

- One in-memory tree of `struct inode` (name up to 63 bytes, children as a linked list).
  Paths up to 255 bytes. No symlinks, no hard links, no permissions, no mount points.
- At boot the kernel walks the initrd (ustar) and creates inodes whose data pointers
  point into the archive. A file is copied to the heap the first time it is written.
- **Nothing persists.** A reload starts from the same image.
- `struct file` has reference counts, flags, an offset and a `struct fops`
  (`read write poll close ioctl`). Regular files, directories (`getdents64`), devices,
  pipes, sockets and ptys all go through it. `poll(file, add_waiter)` returns the ready
  mask and optionally registers the caller on the right wait queue.
- Pipes: 64 KiB ring buffer, blocking both ways, short writes, `-EPIPE` with no reader.

| Path | What |
|---|---|
| `/dev/null`, `/dev/zero`, `/dev/urandom` | the usual |
| `/dev/console` | writes go to the host log; reads come from `console_input` (tests) |
| `/dev/ptmx`, `/dev/pts/N` | pseudo-terminals (section 8) |
| `/dev/fb0` | the screen (section 10) |
| `/dev/input/event0` | keyboard and pointer (section 10) |
| `/tmp`, `/run`, `/proc` | empty directories created at boot; `/proc` has nothing in it |

`sysinfo`: `totalram` = what the device reports, capped at 4 GiB; `freeram` = that minus
everything the kernel and all processes have claimed; `bufferram` = kernel heap in use.

## 8. Terminals (`kernel/tty.c`)

Up to 16 ptys. `open("/dev/ptmx")` gives a master; `ioctl(TIOCGPTN)` gives N; the slave
is `/dev/pts/N`. Layouts and constants are Linux's (`struct termios`, `struct winsize`).

Line discipline: `ICRNL`, `OPOST|ONLCR`, `ICANON` (line editing with UTF-8-aware
erase, `VKILL`, `VEOF`), `ECHO`, `ISIG`, raw mode with `VMIN`/`VTIME` (a timed read
when `VMIN` = 0). Ioctls: `TCGETS TCSETS TCSETSW TCSETSF TIOCGWINSZ TIOCSWINSZ
TIOCGPGRP TIOCSPGRP TIOCGPTN TCFLSH`.

Ctrl-C (and Ctrl-\\): every process in the tty's foreground group is killed, except a
session leader, which instead gets a pending flag it reads with `intr()`. That is how a
shell survives Ctrl-C and can stop its own loops. A pty starts with foreground group 0,
so whoever starts the shell must `setpgid` the child and `TIOCSPGRP` it (`term` does).

Hangup: when the master closes, every process in the foreground job's session is
killed, the shell included. When the last slave closes, the master reads end-of-file
and polls `POLLHUP`; a terminal emulator closes its window then.

There is no `SIGWINCH`: programs re-read the size when they redraw.

## 9. Sockets (`kernel/sock.c`)

`AF_UNIX`, `SOCK_STREAM` only: `socket bind listen accept connect`, then `read`,
`write`, `ppoll`. 512 KiB buffer per direction, backlog 16, short writes, `-EPIPE` when
the peer is gone, `-ECONNREFUSED` with no listener. No datagrams, no `socketpair`, no
`accept4` (set close-on-exec with `fcntl` after `accept`), no networking of any kind.

## 10. Screen and input (`kernel/fb.c`, `boot/page.mjs`, `boot/keys.mjs`)

**Framebuffer.** `/dev/fb0` is `width * height * 4` bytes in kernel memory, bytes in
order R, G, B, A (what a canvas `ImageData` wants). `ioctl(fd, 0x4600, ...)` is Linux's
`FBIOGET_VSCREENINFO`. Write with `lseek` + `write` or `pwrite64`; every write calls
`host.fb_dirty()`, and the page copies the whole buffer to the canvas on the next
animation frame. The page sizes the framebuffer to the window times the device pixel
ratio (capped at 2; `?scale=1` forces 1).

**Input.** `/dev/input/event0` delivers Linux evdev records
`{ s64 sec; s64 usec; u16 type; u16 code; s32 value; }` (24 bytes):

| Event | Records |
|---|---|
| key press | `(EV_KEY=1, Linux key code, 1)`; auto-repeat has value 2 |
| ...that typed a character | followed by `(EV_MSC=4, 0x10, Unicode code point)`: the browser already applied the keyboard layout |
| key release | `(EV_KEY, code, 0)` |
| pointer | `(EV_ABS=3, 0, x)`, `(EV_ABS, 1, y)` in framebuffer pixels |
| buttons | `EV_KEY` with `0x110` left, `0x111` right, `0x112` middle |
| wheel | `(EV_REL=2, 8, +1 or -1)` |
| screen resized | `(EV_MSC, 0x11, (w << 16) | h)` |
| end of a batch | `(EV_SYN=0, 0, 0)` |

Use the code point for text and the key code for shortcuts. Text from dead keys and
input methods arrives as key 240 with a code point. The page keeps F5, F11, F12 and
Ctrl+Shift+I/J for the browser. Browsers never deliver Ctrl+W, Ctrl+T, Ctrl+N or the
Super key reliably, which is why the window manager's modifier is Alt.

Only one program should read the input device and write the framebuffer: the window
manager, or `term --fb` when there is none.

## 11. The desktop stack

```
init -> wm (owns /dev/fb0 and /dev/input/event0, listens on /run/wm.sock)
          +- bar        (role UI_BAR)       workspaces, title, clock, mem / procs / syscalls per second
          +- launcher   (role UI_FLOATING)  on Alt+d
          +- term ...   (role UI_NORMAL)    each owns a pty and a shell
          +- doom, uidemo, any libui client
```

**init** (`bin/init.c`) tries `/bin/wm`, then `/bin/term --fb`, then `/bin/sh` on the
console, restarting each up to three times. `init PROGRAM ARGS` runs one program.

**libui** (`libui/ui.h`, `ui.c`, `uiproto.h`). The client API: `ui_open(title, app_id,
w, h, role)`, `ui_pixels`, `ui_width`, `ui_height`, `ui_present(x, y, w, h)`, `ui_fd`,
`ui_poll_event`, `ui_wait_event`, `ui_set_title`, `ui_wm_state`, `ui_wm_command`,
`ui_close`. Events: key down/up (Linux key code + modifier bits), text (code point),
mouse move/down/up, wheel, resize, focus, blur, close, wm-state.

Wire format: 8-byte header `u16 type, u16 flags, u32 len`, then the payload. Client to
manager: `HELLO TITLE PRESENT COMMAND GETSTATE`. Manager to client: `CONFIGURE EVENT
STATE REPLY`. `PRESENT` carries the client's buffer size, a rectangle and only that
rectangle's pixels. The manager never blocks on a client, drops clients that send
nonsense, and drops events for a client that stops reading. At most 24 clients.

**A theme change is delivered as a resize event with the unchanged size.** Every client
must call `ui_theme_load()` again on every `UI_EV_RESIZE` and redraw.

**wm** (`bin/wm/`): master-and-stack tiling, gaps, rounded anti-aliased borders (2 px
accent on the focused window, 1 px grey on the others), fullscreen, floating windows,
workspaces, damage-based compositing at most once per 15 ms, zero work when idle. It keeps
no full-screen buffers: damaged rectangles are painted a band of rows at a time into a
static 256 KiB strip and each band is written to `/dev/fb0`. The wallpaper is made again
for each row from two ready-made rows (plain, and with the dot grid) and a small picture of
the logo. Its memory is about 1.2 MiB plus a copy of every window's pixels.
Commands (bindable keys and `ui_wm_command`): `exec CMD`, `workspace N`, `move N`,
`focus next|prev`, `swap next|prev|master`, `master grow|shrink`, `fullscreen`, `float`,
`kill`, `theme next|NAME`, `reload`, `exit`, `stats`, `bench`, `animation MS`.

A workspace switch slides: for `animation_ms` milliseconds (160 by default, 0 for none) the
old and the new workspace pan across the screen, eased out. The slide keeps no pictures of
either workspace. Every frame, `compose_anim()` in `compose.c` draws the old workspace's
windows shifted one way and the new one's shifted the other, each clipped to its side of the
seam, over the wallpaper panned by the same amount, so it needs no extra memory at any
screen size and windows keep updating while they move. The wallpaper is not copied where a
window is about to cover it. The bar stays in place, unless one of the two workspaces hides
it (a fullscreen window): then it travels with the workspace that shows it. Switching back
to the workspace the slide came from continues from where the picture is. Any other switch
during a slide cuts the first one short (the picture jumps to its end) and starts a new
one. During a slide the whole screen is written to `/dev/fb0` on every frame (15 ms apart).

Configuration: `~/.config/wm/config` (in the image: `rootfs/home/user/.config/wm/config`),
i3 style: `set mod Alt`, `gaps`, `border`, `border_thin`, `radius`, `workspace_count`,
`animation_ms`, `master_ratio`, `bind Mod+KEY command`, `exec` (autostart). Default autostart: `exec bar`
and `exec term -e sh /etc/welcome` (runs `fetch`, then a shell).

**Themes** (`libui/theme.h`). The active name is the first word of
`~/.config/ucos/theme`. A theme is `/usr/share/themes/NAME.theme`, lines of
`key = value`: colours `bg fg accent border_focus border_normal border_float bar_bg
bar_fg panel dim desktop`, `color0..color15`, `font`, `font_bold`, `font_size`, `gap`,
`border`, `radius`. Two ship: `deck` (Gruvbox + Fira Code) and `brand` (dark green +
JetBrains Mono). See BRAND.md.

**bar** (`bin/bar/`), **launcher** (`bin/launcher/`): entries are
`/usr/share/applications/*.app` files (`name=`, `exec=`, `comment=`), then every other
file in `/bin`.

**term** (`bin/term/`): `vt.c` (escape parser and screen model, VT500 state machine,
5000 lines of scrollback), `render.c` (draws changed cells into a surface), `keys.c`
(key presses to xterm sequences), `be_ui.c` (window back end), `be_fb.c` (bare
framebuffer back end, `term --fb`), `main.c`. Options: `--fb`, `-e cmd args`, `-T title`,
`--font-size N`, `--hold`, `--stats`. It sets `TERM=xterm-256color`,
`COLORTERM=truecolor`, `THEME`. Test hook: Ctrl+Alt+Shift+F12 prints the visible text to
the console log.

**libgfx** (`libgfx/`): `ttf.c` parses TrueType (`glyf` outlines, cmap formats 4 and 12,
composite glyphs) and rasterizes with exact area coverage; a glyph cache stretches
box-drawing, block and powerline glyphs to fill the cell. `gfx.c`: surfaces with
clipping, fills, anti-aliased rounded rectangles and circles, lines, blits, text.
Pixels are `unsigned int` with bytes R, G, B, A in memory (`GFX_HEX(0xRRGGBB)`).
No hinting, kerning, ligatures, CFF fonts or colour glyphs.

## 12. The C library (`libc/`, `lib/`)

Written for this system. Headers in `libc/include/`: `alloca assert ctype dirent errno
fcntl inttypes libgen limits locale math poll signal stdio stdlib string strings termios
time unistd` and `sys/{ioctl,select,stat,syscall,sysinfo,time,types,utsname,wait}.h`.
`stddef.h`, `stdint.h`, `stdarg.h`, `stdbool.h` come from clang.

- `lib/` is shared with the kernel: `string.c`, `fmt.c` (`vsnprintf` with exact `%e`/`%g`),
  `mem.s` (`memcpy`/`memmove`/`memset` as single bulk-memory instructions).
- stdio with buffering, the `scanf` family, `getline`, `popen`/`system` (they spawn
  `/bin/sh -c`), a full `math.h` written here (within 1 to 3 ulp of glibc), `qsort`,
  `strtod`, `getopt`, termios wrappers, `select` on top of `ppoll`.
- `spawn()` and `spawnp()` in `unistd.h` are the process-creation API.
- **Missing on purpose:** `setjmp`/`longjmp` (impossible in plain C on wasm), `fork`,
  `exec*`, threads. **Missing:** `long double` arithmetic and `%Lf`, `getopt_long`,
  `erf`/`gamma`, wide characters, locales other than "C", time zones (all times are UTC).
- `libc/crt/crt0.c` is built twice (`crt0_2.o`, `crt0_3.o`) because `main` may take two
  or three parameters and wasm checks signatures.
- Each program is statically linked. A minimal program is about 1.6 KB; `sh` about 100 KB.

## 13. The toolchain

- `clang --target=wasm32-unknown-unknown -O2 -ffreestanding -nostdlib -fvisibility=hidden`
  and `wasm-ld`. No Emscripten, no wasi-sdk, no sysroot.
- Kernel link: `--no-entry --export-table --growable-table -z stack-size=262144
  --stack-first`. Programs: `--entry=_start -z stack-size=1048576 --stack-first
  --fatal-warnings`.
- Exports are marked `__attribute__((export_name("x")))` (`EXPORT(x)` in the kernel);
  imports `__attribute__((import_module("host"), import_name("x")))` (`HOST(x)`).
- Inline assembly exists but it is WebAssembly, not x86. clang accepts wasm instructions
  in `__asm__` and in `.s` files (`lib/mem.s` is one). `__builtin_wasm_memory_grow`,
  `__builtin_wasm_memory_size`, and `<wasm_simd128.h>` work. x86 or ARM assembly cannot
  run in a browser at all.
- The browser's engine compiles wasm to machine code. You do not control registers or
  native instructions. What you control: how often you cross into JavaScript, memory
  layout, bulk-memory and SIMD instructions, and algorithmic cost.

## 14. Limits, and what does not exist

| Limit | Value | Where |
|---|---|---|
| processes | 64 | `NPROC`, tied to the 64-bit wait queue mask |
| descriptors per process | 64 | `NFD` |
| descriptors per `ppoll` | 64 | |
| path / file name | 255 / 63 bytes | `PATH_MAX`, `struct inode` |
| argv + environment | 64 KiB | `sys_spawn` |
| ptys | 16 | `NPTY` |
| pipe buffer / socket buffer | 64 KiB / 512 KiB per direction | |
| window manager clients | 24 | |
| time slice | 8 ms | `SLICE_MS` |

Not built: the Worker half of the hybrid process model (true preemption, multi-core),
`fork`, signals, threads, shared memory, persistence, networking, sound, `/proc`
contents, users and permissions, time zones, a clipboard, dynamic linking, running
Linux binaries (see DECISIONS.md for the three "rungs").
