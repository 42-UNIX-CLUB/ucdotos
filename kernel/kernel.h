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

/* vfs */
struct inode {
  u32 ino, mode, size, cap, nlink;
  u32 refs; /* open files, cwds and bound sockets; freed when this and nlink are both 0 */
  u8 *data;
  u8 owned; /* data was kmalloc'ed (not a slice of the initrd) */
  int dev;  /* for S_IFCHR */
  s64 mtime;
  struct inode *parent, *child, *next;
  void *priv; /* a bound socket, for S_IFSOCK */
  char name[64];
};
extern struct inode *vfs_root;
void vfs_init(void);
int vfs_load_tar(u8 *tar, u32 len);
struct inode *vfs_lookup(struct inode *cwd, const char *path);
struct inode *vfs_create(struct inode *cwd, const char *path, u32 mode, int *err);
int vfs_unlink(struct inode *cwd, const char *path, int rmdir);
int vfs_rename(struct inode *from_base, const char *from, struct inode *to_base, const char *to);
int vfs_path(struct inode *ino, char *buf, u32 cap);
void vfs_mkdev(const char *path, int dev);
void inode_get(struct inode *i);
void inode_put(struct inode *i);

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
enum { DEV_NULL = 1, DEV_ZERO, DEV_CONSOLE, DEV_RANDOM, DEV_FB, DEV_INPUT, DEV_PTMX, DEV_DSP, DEV_PTS0 = 100 };
struct file *pty_open_master(struct inode *ino, int flags);
struct file *pty_open_slave(struct inode *ino, int flags);
struct file *fb_open(struct inode *ino, int flags);
struct file *ev_open(struct inode *ino, int flags);
struct file *dsp_open(struct inode *ino, int flags);
void audio_init(void);

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
