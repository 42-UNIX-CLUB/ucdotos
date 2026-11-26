// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KERNEL_H
#define KERNEL_H
#include "../abi/abi.h"

#define OS_NAME "UC.OS" /* UNIX CLUB dot OS */
#define OS_RELEASE "0.1.0"
#define NPROC 64 /* wait queues are a 64-bit mask of pids, so keep <= 64 */
#define NFD 64
#define PATH_MAX 256
#define NULL ((void *)0)

/* The "hardware": functions the page (or Node) provides. Kept as small as possible. */
#define HOST(name) __attribute__((import_module("host"), import_name(#name)))
#define EXPORT(name) __attribute__((export_name(#name)))
HOST(log) void host_log(const char *p, u32 len);        /* serial console out */
HOST(spawn) void host_spawn(int pid, const u8 *wasm, u32 len, int cacheable); /* instantiate a program */
HOST(wake) void host_wake(int pid);                      /* resume a blocked process */
HOST(reap) void host_reap(int pid);                      /* drop a dead process's instance */
HOST(now) double host_now(void);                         /* monotonic milliseconds */
HOST(epoch) double host_epoch(void);                     /* milliseconds since 1970 */
HOST(random) u32 host_random(void);
HOST(memory) double host_memory(void);                   /* bytes of memory the device reports */
#define SLICE_MS 8.0 /* longest a process runs before the kernel makes it yield at a system call */
HOST(fb_dirty) void host_fb_dirty(void);                 /* framebuffer changed, present it */

/* klib */
void *memcpy(void *, const void *, usize);
void *memmove(void *, const void *, usize);
void *memset(void *, int, usize);
int memcmp(const void *, const void *, usize);
usize strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, usize);
usize strlcpy(char *, const char *, usize);
char *strrchr(const char *, int);
int snprintf(char *, usize, const char *, ...);
int vsnprintf(char *, usize, const char *, __builtin_va_list);
void kprintf(const char *fmt, ...);
void panic(const char *msg);
void *kmalloc(usize n);
void *kzalloc(usize n);
void *krealloc(void *p, usize n);
void kfree(void *p);
u32 kmem_used(void);

/* wait queues: a set of pids to wake when something changes */
typedef u64 waitq;
void wq_add(waitq *q);  /* add the current process */
void wq_wake(waitq *q); /* wake everyone and clear */

/* vfs. A directory entry (struct dentry: a name in one directory) and an inode (the
 * object: mode, owner, times, data) are separate, so a file can have several names. */
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define VFS_PATH_MAX 4096 /* longest path the file system calls accept, as Linux */
#define VFS_NAME_MAX 255
struct dentry {
  struct dentry *hnext;       /* hash chain in the directory */
  struct dentry *next, *prev; /* directory order, for listings */
  struct inode *ino, *dir;    /* what it names, and where */
  u32 hash, seq;              /* seq: position in the directory, for getdents offsets */
  u16 nlen;
  char name[];
};
struct dirdata {              /* only directories have one */
  struct inode *parent;       /* ".."; the directory itself once it is removed */
  struct dentry **tab;        /* hash buckets (mask + 1 of them), NULL when empty */
  u32 mask, count, nextseq, gen; /* gen changes whenever an entry goes away */
  struct dentry *first, *last;
};
struct lock;
struct sdirty;
struct inode {
  u32 ino, mode, uid, gid, nlink, size, cap;
  u32 refs;     /* open files, cwds, bound sockets; freed when this and nlink are both 0 */
  u8 *data;     /* file contents, or a symbolic link's target */
  u8 owned;     /* data was kmalloc'ed (not a slice of the initrd) */
  u8 kind;      /* 0, or a /proc or magic-link kind (procfs.c) */
  u8 sflags;    /* persistence state (store.c) */
  u8 spare;
  int dev;      /* S_IFCHR: device id; procfs kinds: their argument */
  s64 atime, mtime, ctime; /* nanoseconds since 1970 */
  struct dirdata *dir;
  struct dentry *alias; /* one of its names, for paths in /proc and getcwd; NULL if unknown */
  void *priv;   /* a bound socket, for S_IFSOCK */
  struct lock *locks;
  u32 sid;      /* id in the persistent store, 0 = none */
  struct sdirty *sd; /* changes not yet handed to the store (store.c), or NULL */
};
extern struct inode *vfs_root;
void vfs_init(void);
int vfs_load_tar(u8 *tar, u32 len);
struct inode *vfs_lookup(struct inode *cwd, const char *path);
struct inode *vfs_lookup_err(struct inode *cwd, const char *path, int *err); /* NULL with *err = -errno */
struct inode *vfs_create(struct inode *cwd, const char *path, u32 mode, int *err);
int vfs_path(struct inode *ino, char *buf, u32 cap);
void vfs_mkdev(const char *path, int dev);
void inode_get(struct inode *i);
void inode_put(struct inode *i);

/* ---- file system calls and their helpers (vfs.c, fs_sys.c, procfs.c) ---- */
/* Permission enforcement, one switch. 0: none, as before (modes are stored and reported
 * but nothing is refused); 1: also refuse to run a file that has no execute bit at all, as
 * Linux does even for root (this breaks scripts run as ./name without chmod +x);
 * 2: full owner/group/other checks. An owner decision; see docs/DECISIONS.md. Build a
 * kernel with another level for tests: make CFLAGS="... -DVFS_ENFORCE_DEFAULT=2". */
#ifndef VFS_ENFORCE_DEFAULT
#define VFS_ENFORCE_DEFAULT 0
#endif
extern int vfs_enforce;
struct fscred {               /* per process, indexed by pid */
  u32 uid, euid, suid, fsuid, gid, egid, sgid, fsgid;
  u32 umask, gen, ngroups;
  u32 groups[16];
  struct inode *root, *exe;   /* chroot directory (NULL = /), the program it runs */
  u32 sync_target;            /* fsync in progress: the change number it waits for */
  int lockwait;               /* F_SETLKW in progress: the pid it waits for, for EDEADLK */
  u64 lockwait_nsys;          /* the caller's system call count when it started that wait */
};
extern struct fscred fscred[NPROC];
void vfs_proc_start(int pid, int parent, struct inode *exe); /* proc_spawn: inherit credentials */
void vfs_proc_end(int pid);           /* proc_free: the slot is free again */
void vfs_proc_exec(int pid, struct inode *exe); /* execve: new program, same credentials and locks */
void locks_proc_end(void);           /* proc_exit: drop record locks of processes that are gone */
void procfs_forget(int pid);
void inode_drop(struct inode *i);     /* free an inode that is in no directory (procfs) */
int vfs_may_exec(struct inode *i);   /* 0 or -EACCES, by the enforcement switch */
struct nameidata {
  struct inode *dir, *ino;    /* the directory of the last component, and what it names (or NULL) */
  struct dentry *de;          /* the entry, when the last component is a real name */
  struct file *file;          /* the path ended at /proc/N/fd/M of a pipe or socket */
  const char *last;
  u32 lastlen;
  u8 trailing, dot;           /* the path ended with '/'; the last component is "." (1) or ".." (2) */
  char buf[2][VFS_PATH_MAX];  /* symbolic link expansion */
};
#define W_FOLLOW 1            /* follow a symbolic link in the last component */
#define W_NOTRAIL 2           /* ...not even with a trailing slash (unlink, rmdir, rename act on the link) */
int vfs_walk(struct inode *base, const char *path, int flags, struct nameidata *nd);
struct inode *vfs_mknod_at(struct nameidata *nd, u32 mode, int *err);
int vfs_unlink_at(struct nameidata *nd, int rmdir);
int vfs_link_at(struct inode *i, struct nameidata *nd);
int vfs_rename_nd(struct nameidata *from, struct nameidata *to, int flags);
int vfs_symlink_target(struct inode *i, char *buf, u32 cap);
int vfs_may(struct inode *i, int mask); /* mask: 4 read, 2 write, 1 search/execute; 0 or -EACCES */
struct inode *vfs_cred_root(void);
s64 vfs_now(void);
int path_from_user(char *kdst, u32 usrc); /* copies at most VFS_PATH_MAX, 256 bytes at a time */
int reg_truncate(struct inode *i, u32 size);
void vfs_touch(struct inode *i, int what); /* 1 atime, 2 mtime, 4 ctime: set to now */
void vfs_changed(struct inode *i);         /* metadata changed: tell the store */
long fs_syscall(long nr, long a, long b, long c, long d, long e); /* -ENOSYS if not a file system call */
long fs_openat(int dirfd, u32 upath, int flags, int mode); /* the hottest ones, called from syscall_slow directly */
long fs_fstat(int fd, u32 ust);
long fs_newfstatat(int dirfd, u32 upath, u32 ust, int flags);
void fs_close_fd(struct file *f);           /* close(2) of one descriptor: drop the caller's record locks */
long fs_fcntl_lock(struct file *f, int cmd, u32 uarg);
struct inode *procfs_lookup(struct inode *dir, const char *name, u32 n);
struct inode *procfs_follow(struct inode *link, struct file **file);
int procfs_readlink(struct inode *link, char *buf, u32 cap);
int procfs_list(struct inode *dir, u32 idx, char *name, struct inode **ino); /* 1 = got one */
struct file *procfs_open(struct inode *i, int flags);
void procfs_init(void);
void store_apply(void);              /* kmain: after the image, before init */
void store_statfs(u64 *total, u64 *free);
long store_need_data(struct inode *i);
#define S_LAZY_BIT 8 /* inode->sflags: contents still in the store (store.c) */
#define DEV_TTY 50 /* /dev/tty: opens the caller's terminal */
int vfs_symlink_at(const char *target, struct nameidata *nd);
struct dentry *vfs_dir_seek(struct file *f, u32 off);
void vfs_dir_remember(struct file *f, struct dentry *next, u32 off);
enum { K_NONE, K_PROCROOT, K_PIDDIR, K_FDDIR, K_GEN, K_SELF, K_MAGIC = 16, K_FDLINK = K_MAGIC, K_EXE, K_CWD, K_TTY };

/* files */
struct file;
struct fops {
  long (*read)(struct file *, u32 ubuf, u32 len);
  long (*write)(struct file *, u32 ubuf, u32 len);
  int (*poll)(struct file *, int add_waiter); /* ready mask; optionally register the current process */
  void (*close)(struct file *);
  long (*ioctl)(struct file *, u32 cmd, u32 arg);
};
struct file {
  int refs, flags;
  const struct fops *ops;
  struct inode *ino;
  u32 off;
  void *priv;
};
struct file *file_new(const struct fops *ops, struct inode *ino, int flags, void *priv);
void file_put(struct file *f);
extern const struct fops reg_fops, dir_fops;
const struct fops *dev_fops(int dev);
enum { DEV_NULL = 1, DEV_ZERO, DEV_CONSOLE, DEV_RANDOM, DEV_FB, DEV_INPUT, DEV_PTMX, DEV_PTS0 = 100 };
struct file *pty_open_master(struct inode *ino, int flags);
struct file *pty_open_slave(struct inode *ino, int flags);
struct file *fb_open(struct inode *ino, int flags);
struct file *ev_open(struct inode *ino, int flags);

/* processes */
typedef void (*copy_fn)(u32 dst, u32 src, u32 len);
typedef u32 (*size_fn)(void);
enum { P_FREE, P_RUN, P_ZOMBIE };
struct proc {
  int state, pid, ppid, exit_code;
  int pgid, sid; /* process group and session, for terminal job control */
  struct file *fd[NFD];
  u8 cloexec[NFD];
  struct inode *cwd;
  copy_fn copy_in, copy_out; /* the process's multi-memory shim */
  size_fn mem_pages;
  u8 *args;
  u32 args_len;
  double block_ms;   /* timeout for the next block; <0 = forever */
  double sleep_until; /* nanosleep / poll deadline; 0 = none */
  u64 nsys;
  double cpu_ms;
  double slice_start; /* when the current run began (0 = not yet sampled); for time slicing */
  double resumed_at;  /* host clock when this process last started or resumed running */
  u32 slice_calls;
  u8 sleeping, intr;  /* blocked in a system call; Ctrl-C pending */
  waitq child_wq;
  char name[32];
  /* fork, exec and signals (kernel/fork.c, kernel/signal.c) */
  u64 sig_pending, sig_blocked, sig_saved_mask; /* bit n-1 = signal n; saved_mask: rt_sigsuspend */
  struct k_sigaction sa[65];
  s32 sig_pid[65], sig_code[65], sig_status[65]; /* who sent each pending signal, for siginfo */
  u32 kick_skew;      /* added to nsys to reach the next slow-path check; taken back there */
  long kick_ret;      /* what the kicked system call returns instead of running */
  u8 sig_kick;        /* check signals at the next system call entry */
  u8 stopped, stop_sig, wait_report; /* job control; wait_report: 1 stop, 2 continue not yet reported */
  u8 blk_signal, blk_norestart; /* a blocked call was interrupted by a signal; it never restarts */
  u8 in_delivery, want_eintr, eintr_next, in_sigsuspend;
  double alarm_at, alarm_ival; /* ITIMER_REAL, host clock in ms; 0 = off */
};
extern struct proc procs[NPROC];
extern struct proc *cur;
int copy_from_user(void *kdst, u32 usrc, u32 len);
int copy_to_user(u32 udst, const void *ksrc, u32 len);
int str_from_user(char *kdst, u32 usrc, u32 cap);
int user_ok(u32 addr, u32 len); /* the range lies inside the current process's memory */
int fd_alloc(struct proc *p, struct file *f, int from);
struct file *fd_get(int fd);
int proc_spawn(struct proc *parent, const char *path, u8 *args, u32 args_len, const int *fdmap);
void proc_exit(struct proc *p, int code);
long block(double timeout_ms); /* returns -ERESTARTSYS for the syscall to return */

/* subsystems */
/* fork, exec and signals (kernel/fork.c, kernel/signal.c) */
HOST(clone) int host_clone(int parent, int child);       /* new instance with a copy of parent's memory; 0 or -errno */
HOST(exec) void host_exec(int pid, const u8 *wasm, u32 len, int cacheable); /* replace pid's program */
HOST(migrate) int host_migrate(int pid, u32 uranges, u32 n); /* move pid into a fresh memory; 0 or -errno */
long sys_pmigrate(u32 uranges, u32 n, int flags);
int pid_alloc(void);
long block_raw(double timeout_ms); /* block() without the signal check (time slicing, stopped processes) */
long block_nr(double timeout_ms); /* like block(), but a signal handler makes the call fail with EINTR even with SA_RESTART */
void sig_send(struct proc *p, int sig, int code, int from_pid, int status);
void sig_kick(struct proc *p);    /* make p check its signals at its next system call */
int sig_kicked(long nr);          /* at a system call entry: 1 = return cur->kick_ret instead of running the call */
long sig_interrupt(int norestart); /* from block(): -ERESTARTSYS without waiting, so the libc runs handlers; 0 = no signal */
void sig_child_exited(struct proc *c);
void sig_exec_reset(struct proc *p);
void sig_fork_copy(struct proc *child, struct proc *parent);
void alarm_check(double now);
long sig_pipe(void); /* a write found no reader: SIGPIPE to the writer; returns -EPIPE (or -ERESTARTSYS if it died) */
long sys_pclone(int flags);
long sys_execve(u32 upath, u32 uargv, u32 uenvp);
long sys_signal_call(long nr, long a, long b, long c, long d);
long sys_kill2(int pid, int sig);
int tty_sig_leader(struct proc *p, int sig); /* Ctrl-C to a session leader with the default action: set intr instead */
int pipe_create(struct file **r, struct file **w);
void console_init(void);
long sys_socket(int domain, int type, int proto);
long sys_bind(int fd, u32 uaddr, u32 len);
long sys_listen(int fd, int backlog);
long sys_accept(int fd);
long sys_connect(int fd, u32 uaddr, u32 len);
void fb_init(void);
void input_init(void);
void pty_init(void);
void tty_pgrp_gone(int pgid); /* no process is left in this group: no terminal may name it */

#endif
