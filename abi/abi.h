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
#define HNIOC_CAPS 0x4801 /* arg: u32 *, bit (1 << HN_x) set if the host can do HN_x */
#define HNIOC_OPEN 0x4802 /* arg: struct hn_open * */
#define HNIOC_DONE 0x4803 /* the request (or our half of the stream) is complete */
struct hn_open { int op, port; char host[256]; };

#endif
