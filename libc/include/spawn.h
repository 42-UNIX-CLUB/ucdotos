// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SPAWN_H
#define _SPAWN_H
/* posix_spawn, on top of the private spawn() call (libc/src/exec.c). Supported: file
 * actions open, close, dup2; attributes SETPGROUP, SETSIGMASK, SETSIGDEF. */
#include <stddef_extra.h>
#include <signal.h>
struct __psfa;
typedef struct { int n; struct __psfa *acts; } posix_spawn_file_actions_t;
typedef struct { short flags; pid_t pgroup; sigset_t mask, def; } posix_spawnattr_t;
#define POSIX_SPAWN_RESETIDS 1
#define POSIX_SPAWN_SETPGROUP 2
#define POSIX_SPAWN_SETSIGDEF 4
#define POSIX_SPAWN_SETSIGMASK 8
int posix_spawn(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const[], char *const[]);
int posix_spawnp(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const[], char *const[]);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *, int, const char *, int, mode_t);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *, int);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *, int, int);
int posix_spawnattr_init(posix_spawnattr_t *);
int posix_spawnattr_destroy(posix_spawnattr_t *);
int posix_spawnattr_setflags(posix_spawnattr_t *, short);
int posix_spawnattr_getflags(const posix_spawnattr_t *, short *);
int posix_spawnattr_setpgroup(posix_spawnattr_t *, pid_t);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *, pid_t *);
int posix_spawnattr_setsigmask(posix_spawnattr_t *, const sigset_t *);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *, sigset_t *);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *, const sigset_t *);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *, sigset_t *);
#endif
