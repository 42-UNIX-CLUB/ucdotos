// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _UNISTD_H
#define _UNISTD_H
#include <stddef_extra.h>
#include <sys/syscall.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
int close(int);
off_t lseek(int, off_t, int);
int dup(int);
int dup2(int, int);
int pipe(int[2]);
int pipe2(int[2], int);
int chdir(const char *);
char *getcwd(char *, size_t);
int unlink(const char *);
int rmdir(const char *);
int access(const char *, int);
int isatty(int);
int ftruncate(int, off_t);
pid_t getpid(void);
pid_t getppid(void);
uid_t getuid(void);
unsigned sleep(unsigned);
int usleep(unsigned);
void _exit(int) __attribute__((noreturn));
/* spawn: this system has no fork yet. fdmap[i] (i = 0..2) is the parent's fd that becomes the child's fd i, or -1. */
pid_t spawn(const char *path, char *const argv[], char *const envp[], const int fdmap[3]);
pid_t spawnp(const char *file, char *const argv[], const int fdmap[3]); /* searches $PATH */
extern char **environ;
/* process groups and sessions (the kernel may answer ENOSYS) */
int setpgid(pid_t, pid_t);
pid_t getpgid(pid_t);
pid_t getpgrp(void);
pid_t setsid(void);
pid_t tcgetpgrp(int);
int tcsetpgrp(int, pid_t);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int gethostname(char *, size_t);
long sysconf(int);
int getpagesize(void);
#define _SC_ARG_MAX 0
#define _SC_CHILD_MAX 1
#define _SC_CLK_TCK 2
#define _SC_OPEN_MAX 4
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE 30
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES 85
/* getopt (POSIX; a leading ':' in optstring reports a missing argument as ':') */
int getopt(int, char *const[], const char *);
extern char *optarg;
extern int optind, opterr, optopt;
/* owners, sync, terminal names (libc/src/tools_compat.c; the kernel may answer ENOSYS) */
int chown(const char *, uid_t, gid_t);
int lchown(const char *, uid_t, gid_t);
int fchown(int, uid_t, gid_t);
int fsync(int);
void sync(void);
char *ttyname(int);
/* links and owners (libc/src/arc_compat.c; the kernel may answer ENOSYS) */
int symlink(const char *, const char *);
int symlinkat(const char *, int, const char *);
int link(const char *, const char *);
int linkat(int, const char *, int, const char *, int);
ssize_t readlink(const char *, char *, size_t);
ssize_t readlinkat(int, const char *, char *, size_t);
int fchownat(int, const char *, uid_t, gid_t, int);
#endif
