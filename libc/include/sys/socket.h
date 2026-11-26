// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
#include <stddef_extra.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
/* Families, types, options and struct sockaddr_in come from abi/abi.h (Linux values). */
typedef unsigned socklen_t;
typedef unsigned short sa_family_t;
struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_storage { sa_family_t ss_family; char __ss_pad[126]; };
struct msghdr {
  void *msg_name;
  socklen_t msg_namelen;
  struct iovec *msg_iov;
  size_t msg_iovlen;
  void *msg_control;
  size_t msg_controllen;
  int msg_flags;
};
#define AF_UNSPEC 0
#define AF_LOCAL AF_UNIX
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX AF_UNIX
#define PF_INET AF_INET
#define PF_PACKET AF_PACKET
#define SOMAXCONN 64
int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t alen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *alen);
int setsockopt(int fd, int level, int opt, const void *val, socklen_t len);
int getsockopt(int fd, int level, int opt, void *val, socklen_t *len);
int shutdown(int fd, int how);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int fd, struct msghdr *msg, int flags);
#endif
