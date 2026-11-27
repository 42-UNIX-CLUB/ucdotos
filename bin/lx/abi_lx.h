// SPDX-License-Identifier: GPL-2.0-or-later
/* The guest's Linux ABI: generic system call numbers (riscv64 uses the generic table),
 * errno values, flags, and the 64-bit structure layouts a riscv64 guest uses. Every
 * structure here is the guest's view, written with fixed-size types so it means the
 * same on wasm32 and on a 64-bit host. Source: Linux include/uapi/asm-generic/. */
#ifndef LX_ABI_H
#define LX_ABI_H
#include <stdint.h>
#include <errno.h>

/* errno values the UC.OS headers do not name (Linux values) */
#ifndef ENXIO
#define ENXIO 6
#endif
#ifndef E2BIG
#define E2BIG 7
#endif
#ifndef ENOTBLK
#define ENOTBLK 15
#endif
#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef EXDEV
#define EXDEV 18
#endif
#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef ETXTBSY
#define ETXTBSY 26
#endif
#ifndef EFBIG
#define EFBIG 27
#endif
#ifndef EROFS
#define EROFS 30
#endif
#ifndef EMLINK
#define EMLINK 31
#endif
#ifndef EDOM
#define EDOM 33
#endif
#ifndef EDEADLK
#define EDEADLK 35
#endif
#ifndef ENOLCK
#define ENOLCK 37
#endif
#ifndef ELOOP
#define ELOOP 40
#endif
#ifndef ENODATA
#define ENODATA 61
#endif
#ifndef EOVERFLOW
#define EOVERFLOW 75
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT 97
#endif
#ifndef ENOTCONN
#define ENOTCONN 107
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif
#ifndef ENOTSUP
#define ENOTSUP EOPNOTSUPP
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT 93
#endif

/* System call numbers (generic table) */
enum {
  LX_SYS_io_setup = 0, LX_SYS_getxattr = 8, LX_SYS_lgetxattr = 9, LX_SYS_fgetxattr = 10,
  LX_SYS_listxattr = 11, LX_SYS_llistxattr = 12, LX_SYS_flistxattr = 13,
  LX_SYS_getcwd = 17, LX_SYS_eventfd2 = 19, LX_SYS_epoll_create1 = 20, LX_SYS_epoll_ctl = 21,
  LX_SYS_epoll_pwait = 22, LX_SYS_dup = 23, LX_SYS_dup3 = 24, LX_SYS_fcntl = 25,
  LX_SYS_inotify_init1 = 26, LX_SYS_ioctl = 29, LX_SYS_flock = 32, LX_SYS_mknodat = 33,
  LX_SYS_mkdirat = 34, LX_SYS_unlinkat = 35, LX_SYS_symlinkat = 36, LX_SYS_linkat = 37,
  LX_SYS_renameat = 38, LX_SYS_umount2 = 39, LX_SYS_mount = 40, LX_SYS_statfs = 43, LX_SYS_fstatfs = 44,
  LX_SYS_truncate = 45, LX_SYS_ftruncate = 46, LX_SYS_fallocate = 47, LX_SYS_faccessat = 48,
  LX_SYS_chdir = 49, LX_SYS_fchdir = 50, LX_SYS_chroot = 51, LX_SYS_fchmod = 52, LX_SYS_fchmodat = 53,
  LX_SYS_fchownat = 54, LX_SYS_fchown = 55, LX_SYS_openat = 56, LX_SYS_close = 57,
  LX_SYS_pipe2 = 59, LX_SYS_getdents64 = 61, LX_SYS_lseek = 62, LX_SYS_read = 63, LX_SYS_write = 64,
  LX_SYS_readv = 65, LX_SYS_writev = 66, LX_SYS_pread64 = 67, LX_SYS_pwrite64 = 68,
  LX_SYS_preadv = 69, LX_SYS_pwritev = 70, LX_SYS_sendfile = 71, LX_SYS_pselect6 = 72,
  LX_SYS_ppoll = 73, LX_SYS_signalfd4 = 74, LX_SYS_splice = 76, LX_SYS_readlinkat = 78,
  LX_SYS_newfstatat = 79, LX_SYS_fstat = 80, LX_SYS_sync = 81, LX_SYS_fsync = 82,
  LX_SYS_fdatasync = 83, LX_SYS_utimensat = 88, LX_SYS_capget = 90, LX_SYS_exit = 93,
  LX_SYS_exit_group = 94, LX_SYS_waitid = 95, LX_SYS_set_tid_address = 96, LX_SYS_futex = 98,
  LX_SYS_set_robust_list = 99, LX_SYS_get_robust_list = 100, LX_SYS_nanosleep = 101,
  LX_SYS_getitimer = 102, LX_SYS_setitimer = 103, LX_SYS_clock_settime = 112,
  LX_SYS_clock_gettime = 113, LX_SYS_clock_getres = 114, LX_SYS_clock_nanosleep = 115,
  LX_SYS_sched_setparam = 118, LX_SYS_sched_setscheduler = 119, LX_SYS_sched_getscheduler = 120,
  LX_SYS_sched_getparam = 121, LX_SYS_sched_setaffinity = 122, LX_SYS_sched_getaffinity = 123,
  LX_SYS_sched_yield = 124, LX_SYS_sched_get_priority_max = 125, LX_SYS_sched_get_priority_min = 126,
  LX_SYS_kill = 129, LX_SYS_tkill = 130, LX_SYS_tgkill = 131, LX_SYS_sigaltstack = 132,
  LX_SYS_rt_sigsuspend = 133, LX_SYS_rt_sigaction = 134, LX_SYS_rt_sigprocmask = 135,
  LX_SYS_rt_sigpending = 136, LX_SYS_rt_sigtimedwait = 137, LX_SYS_rt_sigqueueinfo = 138,
  LX_SYS_rt_sigreturn = 139, LX_SYS_setpriority = 140, LX_SYS_getpriority = 141,
  LX_SYS_setregid = 143, LX_SYS_setgid = 144, LX_SYS_setreuid = 145, LX_SYS_setuid = 146,
  LX_SYS_setresuid = 147, LX_SYS_getresuid = 148, LX_SYS_setresgid = 149, LX_SYS_getresgid = 150,
  LX_SYS_setfsuid = 151, LX_SYS_setfsgid = 152, LX_SYS_times = 153, LX_SYS_setpgid = 154,
  LX_SYS_getpgid = 155, LX_SYS_getsid = 156, LX_SYS_setsid = 157, LX_SYS_getgroups = 158,
  LX_SYS_setgroups = 159, LX_SYS_uname = 160, LX_SYS_sethostname = 161, LX_SYS_getrlimit = 163,
  LX_SYS_setrlimit = 164, LX_SYS_getrusage = 165, LX_SYS_umask = 166, LX_SYS_prctl = 167,
  LX_SYS_getcpu = 168, LX_SYS_gettimeofday = 169, LX_SYS_getpid = 172, LX_SYS_getppid = 173,
  LX_SYS_getuid = 174, LX_SYS_geteuid = 175, LX_SYS_getgid = 176, LX_SYS_getegid = 177,
  LX_SYS_gettid = 178, LX_SYS_sysinfo = 179, LX_SYS_socket = 198, LX_SYS_socketpair = 199,
  LX_SYS_bind = 200, LX_SYS_listen = 201, LX_SYS_accept = 202, LX_SYS_connect = 203,
  LX_SYS_getsockname = 204, LX_SYS_getpeername = 205, LX_SYS_sendto = 206, LX_SYS_recvfrom = 207,
  LX_SYS_setsockopt = 208, LX_SYS_getsockopt = 209, LX_SYS_shutdown = 210, LX_SYS_sendmsg = 211,
  LX_SYS_recvmsg = 212, LX_SYS_readahead = 213, LX_SYS_brk = 214, LX_SYS_munmap = 215,
  LX_SYS_mremap = 216, LX_SYS_clone = 220, LX_SYS_execve = 221, LX_SYS_mmap = 222,
  LX_SYS_fadvise64 = 223, LX_SYS_mprotect = 226, LX_SYS_msync = 227, LX_SYS_mlock = 228,
  LX_SYS_munlock = 229, LX_SYS_mlockall = 230, LX_SYS_munlockall = 231, LX_SYS_mincore = 232, LX_SYS_madvise = 233,
  LX_SYS_accept4 = 242, LX_SYS_riscv_hwprobe = 258, LX_SYS_riscv_flush_icache = 259,
  LX_SYS_wait4 = 260, LX_SYS_prlimit64 = 261, LX_SYS_renameat2 = 276, LX_SYS_seccomp = 277,
  LX_SYS_getrandom = 278, LX_SYS_memfd_create = 279, LX_SYS_membarrier = 283, LX_SYS_copy_file_range = 285,
  LX_SYS_execveat = 281, LX_SYS_statx = 291, LX_SYS_rseq = 293, LX_SYS_pidfd_open = 434, LX_SYS_clone3 = 435,
  LX_SYS_close_range = 436, LX_SYS_openat2 = 437, LX_SYS_faccessat2 = 439, LX_SYS_epoll_pwait2 = 441,
  LX_SYS_fchmodat2 = 452,
  /* UC.OS private calls (abi/abi.h) */
  LX_SYS_spawn = 500, LX_SYS_getargs = 501, LX_SYS_procinfo = 502, LX_SYS_intr = 503,
  LX_NSYS = 512
};

/* open flags (generic values; riscv64 uses them) */
#define LX_O_ACCMODE 3
#define LX_O_CREAT 0100
#define LX_O_EXCL 0200
#define LX_O_NOCTTY 0400
#define LX_O_TRUNC 01000
#define LX_O_APPEND 02000
#define LX_O_NONBLOCK 04000
#define LX_O_DIRECTORY 0200000
#define LX_O_NOFOLLOW 0400000
#define LX_O_CLOEXEC 02000000
#define LX_O_PATH 010000000
#define LX_O_TMPFILE 020200000
#define LX_AT_FDCWD (-100)
#define LX_AT_SYMLINK_NOFOLLOW 0x100
#define LX_AT_REMOVEDIR 0x200
#define LX_AT_SYMLINK_FOLLOW 0x400
#define LX_AT_EACCESS 0x200
#define LX_AT_EMPTY_PATH 0x1000

#define LX_S_IFMT 0170000
#define LX_S_IFLNK 0120000
#define LX_S_IFREG 0100000
#define LX_S_IFDIR 0040000
#define LX_S_IFCHR 0020000

/* struct stat for riscv64 and every other generic 64-bit port (asm-generic/stat.h) */
struct g_stat {
  uint64_t st_dev, st_ino;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, __pad1;
  int64_t st_size;
  int32_t st_blksize, __pad2;
  int64_t st_blocks;
  /* Linux names these st_atime, st_atime_nsec...; renamed here because UC.OS's <sys/stat.h>
   * defines st_atime as a macro (st_atim.tv_sec), as glibc does */
  int64_t g_atime, g_atime_nsec, g_mtime, g_mtime_nsec, g_ctime, g_ctime_nsec;
  uint32_t __unused4, __unused5;
};
_Static_assert(sizeof(struct g_stat) == 128, "g_stat");

struct g_statx_ts { int64_t tv_sec; uint32_t tv_nsec; int32_t __res; };
struct g_statx {
  uint32_t stx_mask, stx_blksize;
  uint64_t stx_attributes;
  uint32_t stx_nlink, stx_uid, stx_gid;
  uint16_t stx_mode, __spare0;
  uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
  struct g_statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
  uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
  uint64_t stx_mnt_id;
  uint32_t stx_dio_mem_align, stx_dio_offset_align;
  uint64_t __spare3[12];
};
_Static_assert(sizeof(struct g_statx) == 256, "g_statx");

struct g_timespec { int64_t tv_sec, tv_nsec; };
struct g_timeval { int64_t tv_sec, tv_usec; };
struct g_iovec { uint64_t base, len; };
struct g_rlimit { uint64_t cur, max; };
struct g_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
struct g_sysinfo {
  int64_t uptime;
  uint64_t loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
  uint16_t procs, pad;
  uint32_t pad2;
  uint64_t totalhigh, freehigh;
  uint32_t mem_unit;
  char _f[4];
};
_Static_assert(sizeof(struct g_sysinfo) == 112, "g_sysinfo");
struct g_rusage {
  struct g_timeval ru_utime, ru_stime;
  int64_t rest[14];
};
struct g_tms { int64_t utime, stime, cutime, cstime; };
struct g_sigaction {          /* the kernel's struct sigaction on riscv64 (no sa_restorer) */
  uint64_t handler;
  uint64_t flags;
  uint64_t mask;
};
struct g_stack { uint64_t sp; int32_t flags, pad; uint64_t size; };

/* signals */
#define LX_SIGHUP 1
#define LX_SIGINT 2
#define LX_SIGQUIT 3
#define LX_SIGILL 4
#define LX_SIGTRAP 5
#define LX_SIGABRT 6
#define LX_SIGBUS 7
#define LX_SIGFPE 8
#define LX_SIGKILL 9
#define LX_SIGUSR1 10
#define LX_SIGSEGV 11
#define LX_SIGUSR2 12
#define LX_SIGPIPE 13
#define LX_SIGALRM 14
#define LX_SIGTERM 15
#define LX_SIGCHLD 17
#define LX_SIGCONT 18
#define LX_SIGSTOP 19
#define LX_SIGTSTP 20
#define LX_SIGTTIN 21
#define LX_SIGTTOU 22
#define LX_SIGWINCH 28
#define LX_SIGSYS 31
#define LX_NSIG 64
#define LX_SA_NOCLDSTOP 1
#define LX_SA_NOCLDWAIT 2
#define LX_SA_SIGINFO 4
#define LX_SA_ONSTACK 0x08000000
#define LX_SA_RESTART 0x10000000
#define LX_SA_NODEFER 0x40000000
#define LX_SA_RESETHAND 0x80000000
#define LX_SS_ONSTACK 1
#define LX_SS_DISABLE 2

/* clone flags */
#define LX_CLONE_VM 0x00000100
#define LX_CLONE_FS 0x00000200
#define LX_CLONE_FILES 0x00000400
#define LX_CLONE_SIGHAND 0x00000800
#define LX_CLONE_PIDFD 0x00001000
#define LX_CLONE_VFORK 0x00004000
#define LX_CLONE_PARENT 0x00008000
#define LX_CLONE_THREAD 0x00010000
#define LX_CLONE_SETTLS 0x00080000
#define LX_CLONE_PARENT_SETTID 0x00100000
#define LX_CLONE_CHILD_CLEARTID 0x00200000
#define LX_CLONE_CHILD_SETTID 0x01000000

/* auxiliary vector */
#define LX_AT_NULL 0
#define LX_AT_PHDR 3
#define LX_AT_PHENT 4
#define LX_AT_PHNUM 5
#define LX_AT_PAGESZ 6
#define LX_AT_BASE 7
#define LX_AT_FLAGS 8
#define LX_AT_ENTRY 9
#define LX_AT_UID 11
#define LX_AT_EUID 12
#define LX_AT_GID 13
#define LX_AT_EGID 14
#define LX_AT_PLATFORM 15
#define LX_AT_HWCAP 16
#define LX_AT_CLKTCK 17
#define LX_AT_SECURE 23
#define LX_AT_BASE_PLATFORM 24
#define LX_AT_RANDOM 25
#define LX_AT_HWCAP2 26
#define LX_AT_EXECFN 31
#define LX_AT_MINSIGSTKSZ 51

#endif
