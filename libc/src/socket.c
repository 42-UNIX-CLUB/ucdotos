// SPDX-License-Identifier: GPL-2.0-or-later
/* Socket calls: sendmsg and recvmsg (connected AF_UNIX streams, kernel/sock.c). The other
 * socket wrappers are in libc/src/net.c. */
#include <sys/socket.h>
#include <errno.h>
ssize_t sendmsg(int fd, const struct msghdr *m, int fl) { return __sysret(__syscall3(SYS_sendmsg, fd, m, fl)); }
ssize_t recvmsg(int fd, struct msghdr *m, int fl) { return __sysret(__syscall3(SYS_recvmsg, fd, m, fl)); }
