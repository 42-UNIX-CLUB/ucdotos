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
#define SYS_getsockname 204
#define SYS_getpeername 205
#define SYS_sendto 206
#define SYS_recvfrom 207
#define SYS_setsockopt 208
#define SYS_getsockopt 209
#define SYS_shutdown 210
#define SYS_wait4 260
#define SYS_getrandom 278
/* Private call numbers (Linux has none of these). Ranges in use, so that streams do not collide:
 *   500-503  base: spawn, getargs, procinfo, intr (below)
 *   504-519  networking (netstat 504, ifinfo 505, ifctl 506 in the network stack; room to grow)
 *   520-539  processes and memory: pclone 520, sigtake 521, pmigrate 522 (further down)
 * The ranges are an assumption made at integration, reversible; see docs/DECISIONS.md. */
/* --- private calls: Linux has no spawn syscall and passes argv on the stack --- */
#define SYS_spawn 500    /* (path, argv, envp, fdmap[3] or 0) -> pid */
#define SYS_getargs 501  /* (buf, cap) -> bytes: argc, envc, then NUL-terminated strings */
#define SYS_procinfo 502 /* (index, struct procinfo*) -> 0, or -ESRCH past the end */
#define SYS_intr 503     /* () -> 1 if Ctrl-C was pressed for this process since the last call (and clears it), else 0 */
#define SYS_netstat 504  /* (index, struct sockinfo*) -> 0, or -ESRCH past the end: every TCP connection and socket */
#define SYS_ifinfo 505   /* (index, struct ifinfo*) -> 0, or -ESRCH past the end: network interfaces */
#define SYS_ifctl 506    /* (index, op, value) -> 0: change an interface (IFCTL_*) */

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
#define EDESTADDRREQ 89
#define EMSGSIZE 90
#define EPROTOTYPE 91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETDOWN 100
#define ENETUNREACH 101
#define ECONNABORTED 103
#define ECONNRESET 104
#define ENOBUFS 105
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EALREADY 114
#define EINPROGRESS 115

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

struct timespec {
  s64 tv_sec;
  long tv_nsec;
};
/* Times have nanoseconds (st_atim, st_mtim, st_ctim), as Linux; st_atime and friends are the
 * seconds, through the same macros as glibc. Changed 2026-11-26 (was three s64 seconds):
 * every program must be rebuilt with this header. */
struct stat {
  u64 st_dev, st_ino;
  u32 st_mode, st_nlink, st_uid, st_gid;
  u64 st_rdev;
  s64 st_size;
  s32 st_blksize;
  s64 st_blocks;
  struct timespec st_atim, st_mtim, st_ctim;
};
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
struct dirent64 {
  u64 d_ino;
  s64 d_off;
  u16 d_reclen;
  u8 d_type;
  char d_name[];
};
struct utsname {
  char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
struct pollfd {
  int fd;
  short events, revents;
};
#define POLLIN 1
#define POLLPRI 2
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
#define EVIOCGRAB 0x40044590 /* /dev/input/event0, arg int: 1 = capture the mouse (the page locks the pointer and sends REL_X/REL_Y), 0 = release */
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
/* --- links, permissions, owners, special files (added for tar; the kernel may answer ENOSYS) --- */
#ifndef SYS_mknodat
#define SYS_mknodat 33
#endif
#ifndef SYS_symlinkat
#define SYS_symlinkat 36
#endif
#ifndef SYS_linkat
#define SYS_linkat 37
#endif
#ifndef SYS_fchmod
#define SYS_fchmod 52
#endif
#ifndef SYS_fchmodat
#define SYS_fchmodat 53
#endif
#ifndef SYS_fchownat
#define SYS_fchownat 54
#endif
#ifndef SYS_fchown
#define SYS_fchown 55
#endif
#ifndef SYS_readlinkat
#define SYS_readlinkat 78
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif
#ifndef S_IFBLK
#define S_IFBLK 0060000
#endif
#ifndef UTIME_NOW
#define UTIME_NOW ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
#endif
#ifndef EXDEV
#define EXDEV 18
#endif
#ifndef ELOOP
#define ELOOP 40
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif
/* --- sockets (Linux values) --- */
#define AF_UNIX 1
#define AF_INET 2
#define AF_PACKET 17
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_NONBLOCK O_NONBLOCK
#define SOCK_CLOEXEC O_CLOEXEC
#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_LINGER 13
#define IP_TTL 2
#define IP_TRANSPARENT 19 /* a listener that accepts connections to any address (the router) */
#define IP_TRANSPARENT_PORT 0x5501 /* UC.OS: a transparent listener takes only this destination port (0: all) */
#define TCP_NODELAY 1
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#define MSG_PEEK 2
#define MSG_DONTWAIT 0x40
#define ETH_P_ALL 3
#define ETH_P_IP 0x0800
#define PACKET_HOST 0
#define PACKET_OUTGOING 4
struct linger { int l_onoff, l_linger; }; /* l_onoff 1 and l_linger 0: close() resets the connection */
struct in_addr { u32 s_addr; }; /* network byte order */
struct sockaddr_in { u16 sin_family, sin_port; struct in_addr sin_addr; u8 sin_zero[8]; };
struct sockaddr_ll {
  u16 sll_family, sll_protocol; /* protocol in network byte order */
  int sll_ifindex;
  u16 sll_hatype;
  u8 sll_pkttype, sll_halen, sll_addr[8];
};
/* SYS_netstat: one entry per TCP connection (listeners, children waiting for accept,
 * connections left to finish after close) and per UDP, raw or packet socket.
 * Addresses and ports in host byte order. */
#define TCP_ESTABLISHED 1 /* states use Linux's numbering (include/net/tcp_states.h) */
#define TCP_SYN_SENT 2
#define TCP_SYN_RECV 3
#define TCP_FIN_WAIT1 4
#define TCP_FIN_WAIT2 5
#define TCP_TIME_WAIT 6
#define TCP_CLOSE 7
#define TCP_CLOSE_WAIT 8
#define TCP_LAST_ACK 9
#define TCP_LISTEN 10
#define TCP_CLOSING 11
struct sockinfo {
  u8 proto, state; /* proto: IPPROTO_TCP, IPPROTO_UDP, IPPROTO_ICMP (raw) or 255 (packet) */
  u16 lport, rport;
  u16 owner;       /* pid that opened the socket, 0 once closed */
  u32 laddr, raddr;
  u32 rx_queue, tx_queue; /* bytes waiting to be read; bytes not yet acknowledged */
  u32 cwnd, ssthresh, snd_wnd, mss; /* TCP only: congestion window, threshold, peer's window, segment size */
  u32 srtt_us, rto_ms, retransmits;
};
/* SYS_ifinfo */
#define IFF_UP 1
#define IFF_LOOPBACK 8
#define IFF_DEFAULT 0x10000 /* UC.OS: the default route leaves through this interface */
struct ifinfo {
  char name[16];
  u32 flags, mtu, addr, mask; /* addr, mask in host byte order */
  u64 rx_packets, rx_bytes, tx_packets, tx_bytes, dropped;
  u32 loss_permille, delay_ms; /* simulated faults, for tests and teaching */
};
/* SYS_ifctl ops */
#define IFCTL_UP 1    /* value: 0 down, 1 up */
#define IFCTL_MTU 2   /* value: bytes, 68 to 65535 */
#define IFCTL_LOSS 3  /* value: packets dropped per thousand */
#define IFCTL_DELAY 4 /* value: milliseconds added to every packet */
#define IFCTL_ADDR 5  /* value: IPv4 address, host byte order */
#define IFCTL_MASK 6  /* value: netmask, host byte order */
#define IFCTL_DEFAULT 7 /* value: 1 = the default route uses this interface, 0 = not */

/* /dev/hostnet: a program asks the host (the browser or Node) to do network work the
 * page can do. Each open descriptor is one request. Only the router uses it. */
#define HN_RESOLVE 1  /* host = a name; read gives one IPv4 address per line, then end of file */
#define HN_HTTP 2     /* write a whole HTTP/1.x request, then HNIOC_DONE; read gives the response */
#define HN_TCP 3      /* host:port; then a byte stream both ways; HNIOC_DONE ends our side */
#define HNCAP_TEST (1 << 4) /* HNIOC_CAPS: the host is in test mode (tools/tests/router-http.mjs) */
#define HNCAP_RELAY (1 << 5) /* HN_TCP goes through the platform's relay: ports 80 and 443 only */
#define HNCAP_PROXY (1 << 6) /* HN_HTTP may go through the platform's proxy (the hosts on its list) */
#define HNIOC_CAPS 0x4801 /* arg: u32 *, bit (1 << HN_x) set if the host can do HN_x */
#define HNIOC_OPEN 0x4802 /* arg: struct hn_open * */
#define HNIOC_DONE 0x4803 /* the request (or our half of the stream) is complete */
struct hn_open { int op, port; char host[256]; };
/* --- added for the package manager (durability, the file creation mask); the
 * kernel may answer ENOSYS until it implements them --- */
#ifndef SYS_fsync
#define SYS_fsync 82
#endif
#ifndef SYS_fdatasync
#define SYS_fdatasync 83
#endif
#ifndef SYS_umask
#define SYS_umask 166
#endif
#ifndef ENOTSUP
#define ENOTSUP EOPNOTSUPP
#endif

/* --- processes and signals (added for fork, exec and signal delivery) --- */
#define SYS_execve 221
#define SYS_setitimer 103
#define SYS_getitimer 102
#define SYS_tgkill 131
#define SYS_rt_sigsuspend 133
#define SYS_rt_sigaction 134
#define SYS_rt_sigprocmask 135
#define SYS_rt_sigpending 136
#define SYS_pclone 520  /* (flags = 0) -> child pid; clones the caller: memory, exported globals, descriptors.
                         * The child starts at the program's export __fork_child. See ARCHITECTURE.md 5. */
#define SYS_pmigrate 522 /* (ranges, n, flags = 0): move the caller into a fresh memory holding only these ranges
                          * (u32 start, u32 length pairs); same pid. Never returns on success: the new instance
                          * starts at the export __migrate_resume. See ARCHITECTURE.md 5. */
#define SYS_sigtake 521 /* (struct sigtake *) -> next signal to run a handler for (its bit is cleared), or 0 */
#define WUNTRACED 2
#define E2BIG 7
#define WCONTINUED 8
/* rt_sigaction's structure, Linux generic layout on a 32-bit machine (sigsetsize must be 8) */
struct k_sigaction {
  u32 handler; /* 0 = SIG_DFL, 1 = SIG_IGN, else a function pointer of the program */
  u32 flags;
  u32 restorer;
  u32 pad;
  u64 mask;    /* bit n-1 = signal n */
};
/* what SYS_sigtake reports: the handler to call and the mask to restore after it */
struct sigtake {
  u32 handler, flags;
  u64 oldmask;
  s32 signo, code, pid, status; /* code: Linux's si_code: 0 kill, 0x80 kernel, CLD_* for SIGCHLD */
};
struct k_itimerval {
  s64 interval_sec;
  long interval_usec;
  s64 value_sec;
  long value_usec;
};
#define SIGHUP 1
#define SIGQUIT 3
#define SIGPIPE 13
#define SIGALRM 14
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGWINCH 28
#define VSUSP 10
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO 4
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000

/* --- file system: links, modes, owners, times, locks, statx (Linux generic values) --- */
#define SYS_flock 32
#define SYS_mknodat 33
#define SYS_symlinkat 36
#define SYS_linkat 37
#define SYS_statfs 43
#define SYS_fstatfs 44
#define SYS_truncate 45
#define SYS_fallocate 47
#define SYS_faccessat 48
#define SYS_fchdir 50
#define SYS_chroot 51
#define SYS_fchmod 52
#define SYS_fchmodat 53
#define SYS_fchownat 54
#define SYS_fchown 55
#define SYS_readv 65
#define SYS_writev 66
#define SYS_preadv 69
#define SYS_pwritev 70
#define SYS_sendfile 71
#define SYS_readlinkat 78
#define SYS_sync 81
#define SYS_fsync 82
#define SYS_fdatasync 83
#define SYS_setregid 143
#define SYS_setgid 144
#define SYS_setreuid 145
#define SYS_setuid 146
#define SYS_setresuid 147
#define SYS_getresuid 148
#define SYS_setresgid 149
#define SYS_getresgid 150
#define SYS_setfsuid 151
#define SYS_setfsgid 152
#define SYS_getgroups 158
#define SYS_setgroups 159
#define SYS_umask 166
#define SYS_geteuid 175
#define SYS_getgid 176
#define SYS_getegid 177
#define SYS_syncfs 267
#define SYS_renameat2 276
#define SYS_copy_file_range 285
#define SYS_statx 291
#define SYS_faccessat2 439
#define SYS_fchmodat2 452

#define ELOOP 40
#define EXDEV 18
#define ENXIO 6
#define EBUSY 16
#define EROFS 30
#define EMLINK 31
#define EOPNOTSUPP 95
#define EDQUOT 122
#define ENODATA 61
#define EOVERFLOW 75
#define ETXTBSY 26
#define ENODEV 19
#define EFBIG 27
#define ENOLCK 37
#define EDEADLK 35
#define EWOULDBLOCK EAGAIN

#define S_IFLNK 0120000
#define S_IFBLK 0060000
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define DT_UNKNOWN 0
#define DT_BLK 6
#define DT_LNK 10

#define O_NOCTTY 0400
#define O_DSYNC 010000
#define O_LARGEFILE 0100000
#define O_NOFOLLOW 0400000
#define O_NOATIME 01000000
#define O_SYNC 04010000
#define O_PATH 010000000
#define O_TMPFILE 020200000 /* includes O_DIRECTORY, as on Linux */

#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EACCESS 0x200 /* faccessat; same value as AT_REMOVEDIR, as on Linux */
#define AT_SYMLINK_FOLLOW 0x400
#define AT_NO_AUTOMOUNT 0x800
#define AT_EMPTY_PATH 0x1000
#define AT_STATX_SYNC_TYPE 0x6000
#define RENAME_NOREPLACE 1
#define RENAME_EXCHANGE 2
#define RENAME_WHITEOUT 4
#define UTIME_NOW ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)

/* flock(2) and fcntl(2) record locks */
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_GETLK64 12
#define F_SETLK64 13
#define F_SETLKW64 14
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38
#define F_DUPFD_CLOEXEC 1030
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
struct flock {      /* off_t is 32 bits here, as on 32-bit Linux */
  short l_type, l_whence;
  long l_start, l_len;
  int l_pid;
};
struct flock64 {
  short l_type, l_whence;
  s64 l_start, l_len;
  int l_pid;
};

struct iovec {
  void *iov_base;
  usize iov_len;
};

/* statx(2): the same layout on every Linux architecture */
struct statx_timestamp {
  s64 tv_sec;
  u32 tv_nsec;
  s32 __reserved;
};
struct statx {
  u32 stx_mask, stx_blksize;
  u64 stx_attributes;
  u32 stx_nlink, stx_uid, stx_gid;
  u16 stx_mode, __spare0;
  u64 stx_ino, stx_size, stx_blocks, stx_attributes_mask;
  struct statx_timestamp stx_atime, stx_btime, stx_ctime, stx_mtime;
  u32 stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
  u64 stx_mnt_id;
  u32 stx_dio_mem_align, stx_dio_offset_align;
  u64 __spare3[12];
};
#define STATX_TYPE 0x1
#define STATX_MODE 0x2
#define STATX_NLINK 0x4
#define STATX_UID 0x8
#define STATX_GID 0x10
#define STATX_ATIME 0x20
#define STATX_MTIME 0x40
#define STATX_CTIME 0x80
#define STATX_INO 0x100
#define STATX_SIZE 0x200
#define STATX_BLOCKS 0x400
#define STATX_BASIC_STATS 0x7ff
#define STATX_BTIME 0x800
#define STATX_MNT_ID 0x1000

/* statfs(2): Linux's statfs64 layout for 32-bit architectures */
struct statfs {
  u32 f_type, f_bsize;
  u64 f_blocks, f_bfree, f_bavail, f_files, f_ffree;
  int f_fsid[2];
  u32 f_namelen, f_frsize, f_flags, f_spare[4];
};
#define UCOSFS_MAGIC 0x858458f6 /* RAMFS_MAGIC: the tree lives in memory */
#define PROC_SUPER_MAGIC 0x9fa0

/* --- sockets: send and receive with flags (added for SIGPIPE on sockets) --- */
#define SYS_sendmsg 211  /* (fd, struct msghdr *, flags) */
#define SYS_recvmsg 212  /* (fd, struct msghdr *, flags) */
#ifndef EMSGSIZE
#define EMSGSIZE 90
#endif
#ifndef EISCONN
#define EISCONN 106
#endif
#ifndef ENOTCONN
#define ENOTCONN 107
#endif
#define MSG_WAITALL 0x100
#define MSG_NOSIGNAL 0x4000 /* a send to a peer that is gone gives EPIPE without SIGPIPE */
struct k_msghdr {        /* Linux's struct msghdr on a 32-bit machine */
  u32 name, namelen, iov, iovlen, control, controllen, flags;
};

#endif
