// SPDX-License-Identifier: GPL-2.0-or-later
/* The user database: /etc/passwd, read as on Linux (libc/src/pwd.c). */
#ifndef _PWD_H
#define _PWD_H
#include <sys/types.h>
#include <stddef_extra.h>
struct passwd {
  char *pw_name, *pw_passwd;
  uid_t pw_uid;
  gid_t pw_gid;
  char *pw_gecos, *pw_dir, *pw_shell;
};
struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);
int getpwnam_r(const char *name, struct passwd *pw, char *buf, size_t len, struct passwd **out);
int getpwuid_r(uid_t uid, struct passwd *pw, char *buf, size_t len, struct passwd **out);
struct passwd *getpwent(void);
void setpwent(void);
void endpwent(void);
#endif
