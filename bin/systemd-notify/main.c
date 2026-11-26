// SPDX-License-Identifier: GPL-2.0-or-later
/* systemd-notify: tell the service manager about a service's state (READY=1 and the
 * rest of sd_notify's keys). UC.OS's NOTIFY_SOCKET is a stream socket (the kernel has
 * no datagrams), and the message carries SENDER_PID= because the kernel cannot tell
 * the manager who is on the other end (docs/SERVICES.md, "Readiness").
 *   systemd-notify [--ready] [--reloading] [--stopping] [--status=TEXT] [--pid=PID] [VAR=VALUE...] */
#include "../../libsvc/os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  char msg[2048];
  int n = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--ready")) n += snprintf(msg + n, sizeof msg - n, "READY=1\n");
    else if (!strcmp(a, "--reloading")) n += snprintf(msg + n, sizeof msg - n, "RELOADING=1\n");
    else if (!strcmp(a, "--stopping")) n += snprintf(msg + n, sizeof msg - n, "STOPPING=1\n");
    else if (!strncmp(a, "--status=", 9)) n += snprintf(msg + n, sizeof msg - n, "STATUS=%s\n", a + 9);
    else if (!strncmp(a, "--pid=", 6)) n += snprintf(msg + n, sizeof msg - n, "MAINPID=%s\n", a + 6);
    else if (!strcmp(a, "--pid")) n += snprintf(msg + n, sizeof msg - n, "MAINPID=%d\n", getppid());
    else if (!strcmp(a, "--booted")) { return access("/run/systemd/system", 0) == 0 ? 0 : 1; }
    else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { printf("systemd-notify [--ready] [--status=TEXT] [--pid=PID] [VAR=VALUE...]\n"); return 0; }
    else if (strchr(a, '=') && a[0] != '-') n += snprintf(msg + n, sizeof msg - n, "%s\n", a);
    if (n >= (int)sizeof msg - 64) break;
  }
  n += snprintf(msg + n, sizeof msg - n, "SENDER_PID=%d\nSENDER_PPID=%d\n", getpid(), getppid());
  const char *sock = getenv("NOTIFY_SOCKET");
  if (!sock || !*sock) { fprintf(stderr, "systemd-notify: No status data could be sent: $NOTIFY_SOCKET was not set\n"); return 1; }
  int fd = svc_connect_unix(sock);
  if (fd < 0) { fprintf(stderr, "systemd-notify: cannot reach %s\n", sock); return 1; }
  int ok = write(fd, msg, n) == n;
  close(fd);
  return ok ? 0 : 1;
}
