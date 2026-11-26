// SPDX-License-Identifier: GPL-2.0-or-later
/* Shared kernel/user ABI. Linux-shaped: numbers follow Linux's generic table
 * (include/uapi/asm-generic/unistd.h), the one every new Linux port uses. */
#ifndef ABI_H
#define ABI_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef short s16;
typedef int s32;
typedef long long s64;
typedef unsigned long usize;
typedef long ssize;

/* --- syscall numbers (Linux generic) --- */
#define SYS_getcwd 17
#define SYS_dup 23
#define SYS_dup3 24
#define SYS_fcntl 25
#define SYS_ioctl 29
#define SYS_mkdirat 34
#define SYS_unlinkat 35
#define SYS_utimensat 88
#define SYS_ftruncate 46
#define SYS_chdir 49
#define SYS_openat 56
#define SYS_close 57
#define SYS_pipe2 59
#define SYS_getdents64 61
#define SYS_lseek 62
#define SYS_read 63
#define SYS_write 64
#define SYS_pread64 67
#define SYS_pwrite64 68
#define SYS_ppoll 73
#define SYS_newfstatat 79
#define SYS_fstat 80
#define SYS_exit 93
#define SYS_exit_group 94
#define SYS_nanosleep 101
#define SYS_clock_gettime 113
#define SYS_sched_yield 124
#define SYS_kill 129
#define SYS_uname 160
#define SYS_getpid 172
#define SYS_getppid 173
#define SYS_getuid 174
#define SYS_sysinfo 179
#define SYS_socket 198
#define SYS_bind 200
#define SYS_listen 201
#define SYS_accept 202
#define SYS_connect 203
#define SYS_wait4 260
#define SYS_getrandom 278
/* --- private calls: Linux has no spawn syscall and passes argv on the stack --- */
#define SYS_spawn 500    /* (path, argv, envp, fdmap[3] or 0) -> pid */
#define SYS_getargs 501  /* (buf, cap) -> bytes: argc, envc, then NUL-terminated strings */
#define SYS_procinfo 502 /* (index, struct procinfo*) -> 0, or -ESRCH past the end */
#define SYS_intr 503     /* () -> 1 if Ctrl-C was pressed for this process since the last call (and clears it), else 0 */

/* kernel -> user: "sleep until woken, then retry the call" (Linux's ERESTARTSYS) */
#define ERESTARTSYS 512

/* --- errno (Linux values) --- */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define ENOSPC 28
#define ESPIPE 29
#define EPIPE 32
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define ENOTSOCK 88
#define EADDRINUSE 98
#define ECONNREFUSED 111

/* --- open flags (Linux generic) --- */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 3
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_DIRECTORY 0200000
#define O_CLOEXEC 02000000
#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* --- file types --- */
#define S_IFMT 0170000
#define S_IFSOCK 0140000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_REG 8
#define DT_SOCK 12

struct stat {
  u64 st_dev, st_ino;
  u32 st_mode, st_nlink, st_uid, st_gid;
  u64 st_rdev;
  s64 st_size;
  s32 st_blksize;
  s64 st_blocks;
  s64 st_atime, st_mtime, st_ctime;
};
struct dirent64 {
  u64 d_ino;
  s64 d_off;
  u16 d_reclen;
  u8 d_type;
  char d_name[];
};
struct timespec {
  s64 tv_sec;
  long tv_nsec;
};
struct utsname {
  char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
struct pollfd {
  int fd;
  short events, revents;
};
#define POLLIN 1
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
struct procinfo {
  int pid, ppid, state; /* state: 1 running, 2 zombie, 3 sleeping (blocked in a system call) */
  u32 mem_pages; /* 64 KiB wasm pages */
  u64 syscalls;
  char name[32];
  u64 cpu_us; /* time spent running, in microseconds */
};
/* --- terminals (Linux values and layouts) --- */
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPTN 0x80045430 /* pty master: get the number N of /dev/pts/N */
#define NCCS 19
struct termios {
  u32 c_iflag, c_oflag, c_cflag, c_lflag;
  u8 c_line;
  u8 c_cc[NCCS];
};
struct winsize {
  u16 ws_row, ws_col, ws_xpixel, ws_ypixel;
};
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define ICRNL 0x100
#define OPOST 0x1
#define ONLCR 0x4
#define ISIG 0x1
#define ICANON 0x2
#define ECHO 0x8
#define ECHOE 0x10
#define ECHOK 0x20
#define ECHONL 0x40
#define IEXTEN 0x8000
struct sysinfo {
  long uptime;
  unsigned long loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
  u16 procs, pad;
  unsigned long totalhigh, freehigh;
  u32 mem_unit;
  char _f[8];
};
#define SYS_renameat 38
/* job control, minimal: a tty has one foreground process group; Ctrl-C kills it */
#define SYS_setpgid 154
#define SYS_getpgid 155
#define SYS_setsid 157
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define SIGINT 2
#define SIGKILL 9
#define SIGSEGV 11
#define SIGTERM 15

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2 /* time this process has spent running */
#define WNOHANG 1

/* --- sound: OSS, Linux's original sound interface (include/uapi/linux/soundcard.h) ---
 * /dev/dsp: each open descriptor is one stream. One format: 16-bit little-endian
 * stereo at the rate the host plays at (SNDCTL_DSP_SPEED reports it). The streams are
 * mixed by the host, off the kernel's thread. */
#define SNDCTL_DSP_RESET 0x5000
#define SNDCTL_DSP_SYNC 0x5001
#define SNDCTL_DSP_SPEED 0xC0045002
#define SNDCTL_DSP_STEREO 0xC0045003
#define SNDCTL_DSP_GETBLKSIZE 0xC0045004
#define SNDCTL_DSP_SETFMT 0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006
#define SNDCTL_DSP_SETFRAGMENT 0xC004500A /* arg: (count << 16) | log2(size) */
#define SNDCTL_DSP_GETFMTS 0x8004500B
#define SNDCTL_DSP_GETOSPACE 0x8010500C   /* struct audio_buf_info */
#define SNDCTL_DSP_GETODELAY 0x80045017   /* bytes written and not yet played */
#define SNDCTL_DSP_SETPLAYVOL 0xC0045018  /* left | (right << 8), 0 to 100 */
#define SNDCTL_DSP_GETERROR 0x80685019    /* struct audio_errinfo */
#define AFMT_U8 0x08
#define AFMT_S16_LE 0x10
struct audio_buf_info { int fragments, fragstotal, fragsize, bytes; };
struct audio_errinfo {
  int play_underruns, rec_overruns; /* play_underruns: times the stream ran dry while it was being fed */
  unsigned play_ptradjust, rec_ptradjust;
  int play_errorcount, rec_errorcount, play_lasterror, rec_lasterror;
  long play_errorparm, rec_errorparm;
  int filler[16];
};

#endif
