# UC.OS architecture

How the system works, layer by layer. Everything here describes the code as it is on
2026-11-26. If you change behaviour, change this file in the same piece of work.

Contents: 1 the big picture, 2 the host, 3 the system call path, 4 blocking and
scheduling, 5 processes, 6 memory, 7 files and devices, 8 terminals, 9 sockets,
10 screen and input, 11 the desktop stack, 12 the C library, 13 the toolchain,
14 limits and what does not exist, 15 sound.

## 1. The big picture

A browser executes only JavaScript and WebAssembly. Nothing can run "below" WebAssembly
in a page. UC.OS therefore compiles the kernel itself to WebAssembly and makes every
program a separate WebAssembly module.

```
+--------------------------------------------------------------------------------+
| page: boot/index.html + boot/page.mjs                                          |
|   a <canvas>, key/mouse listeners, a boot log; fetches kernel.wasm and the     |
|   image (initrd.tar.gz, unpacked with DecompressionStream, else initrd.tar)    |
+--------------------------------------------------------------------------------+
| host: boot/host.mjs  ("firmware")                                              |
|   instantiates the kernel; instantiates programs when the kernel asks;         |
|   parks and resumes program stacks (JSPI); clock, random, console log          |
+--------------------------------------------------------------------------------+
| kernel.wasm (kernel/*.c, about 42 KB)            one instance, its own memory   |
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
   | `fb_dirty()` | the framebuffer was written since the host last took the damage box (`fb_damage`); show it on the next animation frame. Called once per frame, not per write |
   | `audio_rate audio_open audio_write audio_close audio_volume audio_reset` | the host half of `/dev/dsp` (`boot/audio.mjs`): samples go to the mixer on the browser's audio thread, or to a real-time stand-in under Node. See section 15 |

3. Copies the initrd (a ustar archive) and the command line into kernel memory with the
   kernel export `boot_alloc`, calls `fb_resize(w, h)` if there is a screen, then
   `kmain(initrd, len, cmdline)`.

Kernel command line: `init=/path/to/program -- arg1 arg2`. Default init is `/bin/init`.

Kernel exports the host uses: `kmain boot_alloc syscall_pid proc_attach proc_state
proc_crashed proc_account proc_resumed block_timeout console_input input_event fb_resize
fb_addr fb_damage fb_width fb_height audio_played audio_underrun memory __indirect_function_table`. (`set_current` still exists and is
harmless; the kernel no longer depends on it.)

The object `boot()` returns: `k` (kernel exports), `memory`, `procs`, `frame()`,
`damage()` (the rectangle written since the last call), `resize(w, h)`,
`input(type, code, value)`, `consoleInput(str)`, `consoleEOF()`.

The host import `now` is `performance.now.bind(performance)`, not an arrow function:
Chromium calls a bound function from wasm about 2.5 times faster (PERFORMANCE.md).

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

- No JavaScript runs on this path. Measured cost of the cheapest call (`getppid`): see
  PERFORMANCE.md section 1 (about 11 to 12 ns warm under Node, 14 to 16 ns in Chromium at
  moderate machine load, 100 to 400 ns for the first cold calls). About a third is the call, a third the
  trampoline hop, a third kernel work.
- `syscall_pid` dispatches the four hottest calls (`getppid`, `getpid`, `read`, `write`)
  itself and has no stack frame; every other call goes to `syscall_slow`. Keep large
  locals out of `syscall_pid`: one big buffer in any inlined case gives every call a frame.
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
- **Wait queues are never cleaned.** A pid stays in a queue until the queue is woken, even
  after that process died and the pid went to a new process. The new process then gets a
  spurious wake-up, which is harmless (see above).
- **Every return that ends a call must also end its timeout**: clear `proc->sleep_until`
  on every path that does not block, not only the normal one, or the next `nanosleep` or
  `ppoll` of that process inherits a stale deadline.
- **A call can kill its own caller** (Ctrl-C typed by a process at its own terminal, or a
  hangup when it closes the last master of a terminal whose foreground group it is in).
  The caller is then a zombie for the rest of that call: its descriptors are already
  closed, `fd_alloc`, `dup3` and `spawn` refuse it, and code that continues after such a
  point must not touch objects the caller's descriptors held (see `ptm_write`).
- Timeouts longer than 2^31 - 1 ms (about 24.8 days) are handed to the host as that
  maximum (`block_timeout`), because browser and Node timers fire at once beyond it.

Ending a process: `exit` marks the process a zombie and returns `-ERESTARTSYS`. The
wrapper calls `host.block()`, the host sees the process is no longer running and throws
`EXIT`, which unwinds the wasm stack. Killing a parked process is the same: the kernel
marks it and calls `host.wake`, and the resumed `block()` throws.

### Time slicing

Nothing can interrupt running WebAssembly, so the kernel takes the CPU back at system
call boundaries. Every 256th call (counted by the process's system call counter, `nsys`)
it reads the clock (about 120 ns in Chrome, 60 ns under Node); if the process has run longer than
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

- Table of `NPROC` = 64 slots; pid = slot index; pid 1 is init; pids are reused at once,
  except that a number still used as the process group id or session id of a live (or
  zombie) process is skipped, as on Linux, so a new process can never land in a group or
  session it did not join. If every free slot is such a number, the lowest free slot is
  used anyway.
- `struct proc`: state, pid, ppid, pgid, sid, exit code, 64 file descriptors (`NFD`) with
  close-on-exec flags, cwd, the three shim function pointers, the argument blob, wait
  bookkeeping, counters.
- **spawn**: the path must be a regular file starting with the wasm magic (`-ENOEXEC`
  otherwise). The child inherits cwd, pgid, sid and every descriptor not marked
  close-on-exec. `fdmap[3]` names the parent descriptors that become the child's 0, 1, 2
  (-1 = inherit as is). Arguments and environment travel as one blob of at most 64 KiB:
  `u32 argc, u32 envc`, then the strings. `libc/crt/crt0.c` fetches it with `getargs`.
- **exit and wait**: exit marks the process a zombie first (so a chain of hangups started
  by closing its descriptors can never come back to it), hands children to init (or to
  nobody when init itself is gone; such children are freed when they exit), closes the
  descriptors and wakes the parent. If nobody can wait for it, the slot is freed at once.
  `wait4` takes a pid, -1 (any child), 0 (any child in the caller's process group) or
  -pgid, and `WNOHANG`; other option bits are ignored and `rusage` is not filled.
  Status encoding as on Linux: `(code & 0xff) << 8` for an exit, the signal number for a
  kill or crash.
- **No signals.** `kill(pid, sig)` terminates the target (status = sig). Signal 0 only
  tests. A signal number outside 0..64 is `EINVAL`. `pid` must be positive: process
  groups (`kill(0)`, `kill(-pgid)`) are not supported and give `ESRCH`. The libc's
  `signal()` records handlers that are never called.
- **Job control, minimal**: `setpgid`, `getpgid`, `setsid`, and the tty's foreground
  group (section 8). When the last process of a group is freed, any terminal whose
  foreground group it was forgets it (foreground group 0).
- There are no users or permissions. `getuid` returns 1000.

## 6. Memory

- The kernel has one linear memory. Its heap (`kernel/klib.c`) is a first-fit free list
  with coalescing, grown 1 MiB at a time with `memory.grow`. `kmalloc kzalloc krealloc
  kfree`. wasm memory never shrinks.
- Each process has its own linear memory, exported as `memory`. Its `malloc`
  (`libc/src/malloc.c`) is the same allocator and grows its own memory with no system
  call. A request above 0x7fffffff bytes (PTRDIFF_MAX, glibc's limit too), a `calloc` whose
  product does not fit, or memory that cannot grow, returns NULL with `errno` ENOMEM. The program stack is 1 MiB, placed first in memory (`--stack-first`), so a stack
  overflow traps instead of corrupting data.
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
  Creating a name longer than 63 bytes is `ENAMETOOLONG`. An empty path names nothing
  (`ENOENT`), as on Linux; a relative path with a directory descriptor that is not a
  directory is `ENOTDIR`.
- **Inode lifetime.** An inode has a name (`nlink`) and references (`refs`: open files,
  working directories, a bound socket). Unlinking takes the name away; the inode and its
  data are freed when both are gone. An unlinked file stays readable through open
  descriptors. An unlinked directory becomes its own parent: `..` inside it stays put,
  nothing can be created in it (`ENOENT`), `getcwd` there is `ENOENT`, and removing or
  renaming it again is `ENOENT`.
- Directories are linked lists: looking up, creating and listing cost time linear in
  the number of entries, so filling one directory with n entries costs n^2. Measured on
  2026-11-26 under Node: creating one file in a directory of 2,500 entries takes about
  80 us, in one of 40,000 about 1.6 ms; listing 40,000 entries with 1 KiB `getdents64`
  buffers takes 0.76 s (each call walks the list from the start).
- At boot the kernel walks the initrd (ustar) and creates inodes whose data pointers
  point into the archive. A file is copied to the heap the first time it is written.
  The loader stops at the first entry whose size runs past the end of the archive, and
  ignores `.` and `..` path components.
- File sizes and offsets are 32-bit. `lseek` refuses results above 2^31 - 1 (`EINVAL`),
  and the heap refuses any single block above 2 GiB, so a write or `ftruncate` that
  would need more fails with `ENOSPC`. Up to that, a file takes as much kernel memory as
  its size (sparse regions are real zeros), and kernel memory never shrinks.
- **Nothing persists.** A reload starts from the same image.
- `struct file` has reference counts, flags, an offset and a `struct fops`
  (`read write poll close ioctl`). Regular files, directories (`getdents64`), devices,
  pipes, sockets and ptys all go through it. `poll(file, add_waiter)` returns the ready
  mask and optionally registers the caller on the right wait queue.
- `getdents64` with a buffer too small for the next entry is `EINVAL`, as on Linux.
  `renameat` honours both directory descriptors; renaming onto a name `.` or `..` is
  `EINVAL`. `pread64`/`pwrite64` check the access mode like `read`/`write`.
  `ftruncate` needs a descriptor open for writing and a non-negative size (`EINVAL`).
  A write of 0 bytes never extends a file.
- Pipes: 64 KiB ring buffer, blocking both ways, short writes, `-EPIPE` with no reader.
  A read of 0 bytes (on a pipe, socket, pty or the console) returns 0 at once.

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

`TIOCSPGRP` needs a positive id of a group that exists (zombies count): `EINVAL` or
`ESRCH` otherwise. It does not check that the group is in the caller's session, as
Linux does.

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
the peer is gone, `-ECONNREFUSED` with no listener (and when the backlog is full). A bound
socket keeps its inode alive: unlinking the path while the socket is open is safe. No datagrams, no `socketpair`, no
`accept4` (set close-on-exec with `fcntl` after `accept`), no networking of any kind.

## 10. Screen and input (`kernel/fb.c`, `boot/page.mjs`, `boot/keys.mjs`)

**Framebuffer.** `/dev/fb0` is `width * height * 4` bytes in kernel memory, bytes in
order R, G, B, A (what a canvas `ImageData` wants). `ioctl(fd, 0x4600, ...)` is Linux's
`FBIOGET_VSCREENINFO`. Write with `lseek` + `write` or `pwrite64`. The kernel keeps the
bounding box of every write since the last frame (rows from the offset, columns too when a
write stays within one row) and calls `host.fb_dirty()` on the first write after the host
took the box. On the next animation frame the page takes the box with `fb_damage()` and
copies only that rectangle to the canvas; a resize marks the whole new buffer. The page
sizes the framebuffer to the window times the device pixel ratio (capped at 2; `?scale=1`
forces 1). `lseek` works on `/dev/fb0` (`SEEK_END` counts from 0, because the device node
has no size).

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
mouse move/down/up, wheel, resize, focus, blur, close, wm-state, zoom (the user asked for
bigger, smaller or normal content: `wheel` is +1, -1 or 0; a client with no use for it
ignores it; the terminal changes its font size by 2 px per step, between 8 and 72 px).

Wire format: 8-byte header `u16 type, u16 flags, u32 len`, then the payload. Client to
manager: `HELLO TITLE PRESENT COMMAND GETSTATE`. Manager to client: `CONFIGURE EVENT
STATE REPLY`. `PRESENT` carries the client's buffer size, a rectangle and only that
rectangle's pixels. The manager never blocks on a client, drops clients that send
nonsense, and drops events for a client that stops reading. At most 24 clients.

**A theme change is delivered as a resize event with the unchanged size.** Every client
must call `ui_theme_load()` again on every `UI_EV_RESIZE` and redraw.

**wm** (`bin/wm/`): master-and-stack tiling, gaps, rounded anti-aliased borders (2 px
accent on the focused window, 1 px grey on the others), fullscreen, floating windows,
workspaces, damage-based compositing at most once per 15 ms, zero work when idle.
Commands (bindable keys and `ui_wm_command`): `exec CMD`, `workspace N`, `move N`,
`focus next|prev`, `swap next|prev|master`, `master grow|shrink`, `fullscreen`, `float`,
`kill`, `theme next|NAME`, `reload`, `exit`, `stats`, `bench`, `trace on|off` (print, for each
`PRESENT`, the time from its first byte to the framebuffer write; used by the latency
probe), `animation MS`,
`zoom in|out|reset` (sends the zoom event to the focused window; bound by default to
Ctrl+= with or without Shift, Ctrl+- and Ctrl+0, which therefore no longer reach programs.
Bindings are by key position, so on a non-US keyboard these are the keys in the US
positions of = and -, and the numeric keypad cannot be bound).

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

**sh** (`bin/sh/`, about 4,500 lines) is a POSIX shell for a system without `fork`.
External programs are started with `spawn`. Builtins, functions, compound commands and
subshells (`( list )`, `$(list)`, every stage of a multi-stage pipeline) run in the shell's own
process: what a subshell changes is put back when it ends (variables, functions and
aliases through a journal, positional parameters, options, traps, `$!`, and the umask,
directory and descriptors, saved only when the subshell is about to change them).
`exit`, `break`, `continue` and `return` in a subshell end only the subshell. `umask` is
kept by the shell and passed to the kernel when the kernel has the call. `exec CMD` calls `execve`; on a kernel without it (`ENOSYS`, today's `main`) and
inside a subshell, `CMD` runs as a child and the shell exits with its status (`exec_cmd()`
in `bin/sh/exec.c`). `trap` installs a handler that only marks the signal; the action runs
between commands. Job control is detected once with `rt_sigprocmask` (`jobs_init()` in
`bin/sh/exec.c`): with kernel support, an interactive shell turns on `set -m`, sets VSUSP
to Ctrl-Z for the commands it runs, gives every pipeline its own process group, hands the
terminal to the foreground group and waits with `WUNTRACED`. Without it (today's `main`),
`set -m` is refused with a message and the rest behaves as before.

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

- POSIX regular expressions (`libc/src/regex.c`, `regex.h`: `regcomp`, `regexec`,
  `regerror`, `regfree`, basic and extended syntax, leftmost-longest, the GNU escapes
  `\w \s \b \< \>` and `\| \+ \?` in basic syntax, `REG_STARTEND`). A pattern compiles to a
  small program. Without back-references a Pike VM runs it in time proportional to
  program size times text length; with back-references a backtracking search runs with a
  budget of 20 million steps, then returns `REG_ESPACE`. It yields to the kernel every
  262,144 steps. `fnmatch()` is in `libc/src/fnmatch.c`. grep still has its own matcher.
- `lib/` is shared with the kernel: `string.c`, `fmt.c` (`vsnprintf` with exact `%e`/`%g`).
  `memcpy`/`memmove`/`memset`: the kernel uses `mem.s` (single bulk-memory instructions);
  the libc uses `umem.s` (the same above 16 bytes, plain loads and stores up to 16, because
  V8 turns a variable-length `memory.copy` into a call out of the generated code; the
  kernel's copies are larger, and there the bigger functions measured slower).
- stdio with buffering, the `scanf` family, `getline`, `popen`/`system` (they spawn
  `/bin/sh -c`), a full `math.h` written here (within 1 to 3 ulp of glibc), `qsort`,
  `strtod`, `getopt`, termios wrappers, `select` on top of `ppoll`.
- `spawn()` and `spawnp()` in `unistd.h` are the process-creation API.
- **Missing on purpose:** `setjmp`/`longjmp` (impossible in plain C on wasm), `fork`,
  `exec*`, threads. **Missing:** `long double` arithmetic and `%Lf`, `getopt_long`,
  `erf`/`gamma`, wide characters, locales other than "C", time zones (all times are UTC).
- `make` (section 13) uses only what is here; it needed no new libc function.
- `libc/crt/crt0.c` is built twice (`crt0_2.o`, `crt0_3.o`) because `main` may take two
  or three parameters and wasm checks signatures.
- Each program is statically linked. A minimal program is about 1.6 KB; `sh` about 100 KB.

## 13. The toolchain

- `clang --target=wasm32-unknown-unknown -O2 -ffreestanding -nostdlib -fvisibility=hidden`
  and `wasm-ld`. No Emscripten, no wasi-sdk, no sysroot. Inside UC.OS the same job is
  done by our own `cc` and `ld` (13.1).
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

### 13.1 The compiler inside UC.OS: `cc` and `ld` (`bin/cc/`, `bin/ld/`)

A C compiler and a linker written for this project, so that a C program can be written,
compiled and run inside UC.OS with no server. Both are ordinary UC.OS programs (built by
clang like everything else) and also build with the host `cc` for development.

```
cc file.c -o prog
  pp.c      preprocessor (hide-set macro expansion, include guards detected, #pragma once)
  parse.c   C11 front end: typed syntax tree, constant folding, initialisers
  gen.c     tree -> WebAssembly function bodies (each function generated twice: the
            first pass learns the frame size and the state numbers of forward gotos)
  wasm.c    relocatable object in the tool-conventions "linking" format
  main.c    gcc-style driver; one translation unit per process; runs ld to link
ld --crt0=/usr/lib/crt0_2.o,/usr/lib/crt0_3.o objs... -L/usr/lib -lc
  reads objects and ar archives (from cc or clang), resolves symbols, drops unreachable
  functions and data, applies relocations, writes the program host.mjs expects
```

- **Object format.** cc writes the same relocatable WebAssembly objects as clang
  (`linking` and `reloc.*` custom sections, padded LEB fields). wasm-ld links cc's
  objects; our ld links clang's objects and the clang-built `libc.a` unchanged, so code
  built by the two compilers mixes freely (tested: `bin/cc/tests/mix/`).
- **ABI.** The same as clang on wasm32 (tool-conventions BasicCABI): ILP32, 8-byte
  `long long` and `double`, signed `char`, 4-byte `wchar_t`, `size_t` is `unsigned long`.
  A struct holding one scalar is passed and returned as that scalar; other structs and
  unions go by pointer to a copy (caller-made) and come back through a hidden first
  pointer argument; empty structs are dropped. Variadic arguments are stored in an area
  of the caller's frame, each at its natural alignment (aggregates as a pointer to a
  copy), and a pointer to it is the hidden last argument; `va_list` is a `char *`.
  `long double` is binary128 in memory and passed as two `i64`, as with clang.
- **Frames.** Scalars whose address is never taken live in wasm locals. Arrays, structs
  and address-taken locals live in a frame on the linear-memory stack (`__stack_pointer`,
  growing down, 16-byte aligned). Leaf functions do not move the stack pointer.
- **Control flow.** `if`, loops and `switch` map to block/loop/br/br_table (dense switch
  values use br_table, sparse ones a chain of br_if). `goto` has no wasm equivalent: a
  function with goto labels, or with a switch whose case labels sit inside nested
  statements (Duff's device), is flattened. Only the statements that contain a jump
  target are cut into numbered states, dispatched by one loop around a br_table; the rest
  stays structured. A goto costs a local.set, a br and a br_table dispatch.
- **main.** `int main(void)` is emitted as `__original_main` plus a two-argument `main`
  that calls it, as clang does. ld picks `crt0_3.o` when `main` takes three arguments.
- **In the image** (Makefile rule `cc-sysroot.stamp`, only when `cc` is in `PROGS`): libc,
  libui and libgfx headers in `/usr/include` (`sys/syscall.h` is installed with its
  include of `abi.h` rewritten to `<ucos/abi.h>`), cc's freestanding headers (`stddef.h`,
  `stdint.h`, `stdarg.h`, `stdbool.h`, `limits.h`, `float.h`, `stdalign.h`, ...) in
  `/usr/lib/cc/include`, and `libc.a`, `libui.a`, `libgfx.a`, `crt0_2.o`, `crt0_3.o` in
  `/usr/lib`. `--sysroot=DIR` makes cc look under DIR instead of `/`.
- **Not supported:** `_Complex`, `__int128`, `_BitInt`; inline assembly other than an
  empty `asm("")` barrier (and `.s` files: there is no assembler); computed goto and
  `&&label`; nested functions; `__attribute__((constructor, destructor))` (warned and
  ignored) and `alias`; `long double` arithmetic and conversions (they call the
  compiler-rt helpers `__addtf3`, `__extenddftf2` and so on, which our libc does not
  have: the same link error as with clang); multidimensional variable length arrays,
  pointers to VLA types and `sizeof` of a VLA type that is not a variable; VLA and
  `alloca` memory is released when the function returns, not at the end of the block;
  `_Atomic` and `_Thread_local` are accepted and mean nothing (one thread); trigraphs;
  `#line` is accepted and ignored; calls to one unprototyped function with different
  argument types in one file; bit-fields that cross their container in a packed struct.
  Errors stop at the first one in most cases.

### 13.2 make (`bin/make/`)

`/bin/make` is our own make, written to run the makefiles people write for GNU Make 4.4.
It is plain C (`make.h`, `util.c` for strings, hash tables and globbing, and these):

| File | What |
|---|---|
| `read.c` | logical lines, comments, conditionals, `define`, `include`, assignments, rules |
| `var.c` | variables, scopes (global, target, pattern, inherited from the parent target), `$(...)` expansion |
| `func.c` | the built-in functions, `$(shell)` |
| `rule.c` | the file table, time stamps (cached `stat`), `vpath`, pattern and suffix rules, the implicit rule search |
| `run.c` | the update walk, recipes, `-j N`, the child environment |
| `main.c` | options, `MAKEFLAGS`, remaking makefiles, goals, messages |
| `builtin.c` | built-in variables and suffix rules (the C, C++, assembler, lex and yacc ones), `CC = cc` |
| `sys.c` | the only file that talks to the system: one half for UC.OS (`spawn`), one for POSIX (`fork`/`exec`), so the same logic is tested natively |

How it maps onto UC.OS:
- A recipe line with no shell syntax is started directly with `spawn` (as GNU Make does
  with `execvp`); anything else is `$(SHELL) -c line`, `/bin/sh` by default. The child gets
  an environment built from the exported variables, `MAKEFLAGS` and `MAKELEVEL`+1.
- `-C dir` is a `chdir` before reading; children inherit the directory. `$(shell)` uses a
  pipe from `pipe2` as the child's standard output.
- `-j N` keeps up to N children and waits with `wait4(-1)`. There is no jobserver: a
  sub-make given `-jN` runs its own N jobs. With one CPU thread, `-j` gives no speed-up.
- A makefile that was remade is re-read by starting make again with `spawn` and waiting
  for it (there is no `exec`).
- Time stamps: UC.OS keeps whole seconds, so a target written in the same second as its
  prerequisite counts as up to date (GNU Make's rule: rebuild only if strictly newer).
- Ctrl-C: the kernel kills make together with its recipe, so a half-written target is
  not deleted. The native build does delete it, as GNU Make does.

## 14. Limits, and what does not exist

| Limit | Value | Where |
|---|---|---|
| processes | 64 | `NPROC`, tied to the 64-bit wait queue mask |
| descriptors per process | 64 | `NFD` |
| descriptors per `ppoll` | 64 | |
| path / file name | 255 / 63 bytes | `PATH_MAX`, `struct inode` |
| file size, file offset | just under 2 GiB, 2^31 - 1 | `kmalloc`, `sys_lseek` |
| one `getrandom` call | 33,554,431 bytes (Linux's figure) | `sys_getrandom` |
| argv + environment | 64 KiB | `sys_spawn` |
| one `malloc` in a program | 0x7fffffff bytes | `MALLOC_MAX` in `libc/src/malloc.c` |
| ptys | 16 | `NPTY` |
| pipe buffer / socket buffer | 64 KiB / 512 KiB per direction | |
| sound streams (`/dev/dsp` opens) / buffer per stream | 8 / 512 bytes to 256 KiB (default 16 KiB) | `NSTREAM`, `BUF_MAX` in `audio.c` |
| window manager clients | 24 | |
| time slice | 8 ms | `SLICE_MS` |

Not built: the Worker half of the hybrid process model (true preemption, multi-core),
`fork`, signals, threads, shared memory, persistence, networking, sound input (recording), `/proc`
contents, users and permissions, time zones, a clipboard, dynamic linking for UC.OS
programs. Unmodified Linux binaries run through an ordinary program,
`bin/lx/` (a riscv64 user-mode engine: interpreter, ELF loader, Linux system call layer
on top of this kernel); its design is in `docs/LINUX-BINARIES.md`. It needs nothing from
the kernel that other programs do not have, and lists what it would like in section 7.3
of that file.

## 15. Sound (`kernel/audio.c`, `boot/audio.mjs`, `boot/audio-worklet.js`, `bin/audiod.c`)

DECISIONS.md 20; what works and what is missing is SOUND.md.

**`/dev/dsp`**, in the shape of OSS. Each open descriptor is one stream (8 at most). One
format: 16-bit little-endian stereo at the host's rate. Calls: `write` (blocking, or
`EAGAIN` with `O_NONBLOCK`), `poll` (writable when a fragment is free), and the ioctls
`SPEED SETFMT GETFMTS CHANNELS STEREO SETFRAGMENT GETBLKSIZE GETOSPACE GETODELAY
SETPLAYVOL GETERROR RESET SYNC`, with Linux's numbers. `SPEED`, `SETFMT` and `CHANNELS`
answer with what the device does, as OSS drivers may. No reading (recording), no `mmap`.

The kernel does not mix. `write` copies the samples out of the process into a static
16 KiB buffer and calls `host.audio_write` for each piece; the host copies them at once.
Per stream the kernel keeps only `queued`, the bytes written and not yet played; the
stream's buffer size (`SETFRAGMENT`: fragments of 256 B to 64 KiB, 256 KiB in all) caps
it, so it is also the stream's delay. The host calls `audio_played(id, frames)` as sound
plays, which lowers `queued` and wakes writers and `poll`; `audio_underrun(id)` counts a
gap.

**The host.** In a browser, `boot/audio.mjs` posts each write to an AudioWorklet node
(`boot/audio-worklet.js`) with the buffer transferred. The worklet, on the audio thread,
sums every stream with its volume into the output (float), every 4 render quanta
(~11 ms) posts how many frames of each stream played, and reports a stream that ran dry
while it was being fed: more data arriving within 0.5 s of running out. The page creates
the AudioContext at boot and resumes it on the first click or key press (the browser's
rule). Under Node, or without Web Audio, a stand-in consumes each stream in real time on a
10 ms timer, only while something is queued, and can write the integer mix to a file
(`--audio-capture=FILE` in `tools/run.mjs` and `tools/shot.mjs`).

**audiod**, the sound server for everything but games, a user program started by `init`.
It listens on `/run/audiod.sock`. A client sends a 20-byte header (`"UCAU"`, rate, channels,
bits, volume 0 to 100, total bytes or 0) and samples: 8-bit unsigned or 16-bit, mono or
stereo, 4 to 192 kHz. Each client is converted with linear interpolation in integers
(16.16 positions, 15-bit fractions; a client at the device rate skips interpolation),
mixed with a 32-bit sum and clamped, in 20 ms chunks into audiod's own `/dev/dsp` stream
(16 x 4 KiB, ~340 ms). It reads a client only while that client's 64 KiB buffer has room,
so a fast client waits in `write`. When a client's `total` bytes are mixed it gets one
byte back and the connection is closed. With no clients, audiod closes `/dev/dsp`.

**oplplay** (`bin/oplplay/`), Doom's music (DECISIONS.md 21). `opl.c` is our OPL2
synthesizer: 9 two-operator channels, the chip's registers, computed like the chip in the
log domain (a 256-entry log-sine table and a 256-entry exponential table: output =
exp(logsin(phase) + attenuation)), a 9-bit envelope with the chip's rate scheme, four
waveforms, feedback, FM and additive connection, key scaling, tremolo and vibrato.
Rendered at 48 kHz directly (phase and envelope steps scaled from the chip's 49716 Hz);
a channel whose voices have finished is skipped. `main.c` reads the GENMIDI instruments
(a file or straight from a WAD), a MUS or MIDI song (format 0 or 1, tempo map; events
sorted stably), and drives the 9 voices as Doom's music driver did: percussion on MIDI
channel 10 from instruments 128 to 174 at fixed pitches, double-voice instruments with
their fine detune, the oldest voice stolen when all are busy, volume and expression and
pan per channel, pitch bend of 2 semitones. It sends 16-bit stereo to audiod, rendering
at most ~60 ms ahead of real time; with `-c` it takes `pause`, `resume`, `volume N` and
`stop` on stdin. Doom's music module (`bin/doom/i_ucossound.c`) writes the song and
GENMIDI to `/tmp/doom/` and runs it.

