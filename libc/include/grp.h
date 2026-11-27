// SPDX-License-Identifier: GPL-2.0-or-later
/* The group database: /etc/group, read as on Linux (libc/src/pwd.c). */
#ifndef _GRP_H
#define _GRP_H
#include <sys/types.h>
#include <stddef_extra.h>
struct group {
  char *gr_name, *gr_passwd;
  gid_t gr_gid;
  char **gr_mem;
};
struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
int getgrnam_r(const char *name, struct group *gr, char *buf, size_t len, struct group **out);
int getgrgid_r(gid_t gid, struct group *gr, char *buf, size_t len, struct group **out);
struct group *getgrent(void);
void setgrent(void);
void endgrent(void);
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups);
int initgroups(const char *user, gid_t group);
#endif
