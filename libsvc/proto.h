// SPDX-License-Identifier: GPL-2.0-or-later
/* How systemctl and journalctl talk to the manager: an AF_UNIX stream socket and lines
 * of text. A request is one line of words separated by single spaces (unit names
 * contain no spaces). The answer is any number of lines, then "=rc N" where N is the
 * exit status the client should return. Lines that start with "!" are errors meant
 * for stderr. A "journal -f" request keeps the connection and streams records.
 *
 * Paths are placeholders until the owners name them (docs/SERVICES.md). */
#ifndef SVC_PROTO_H
#define SVC_PROTO_H
#define SVC_CONTROL "/run/svcd/control"   /* $SVCD_SOCKET overrides it (tests) */
#define SVC_NOTIFY "/run/systemd/notify"  /* NOTIFY_SOCKET for services (stream, not datagram) */
#define SVC_RUNNING "/run/systemd/system"  /* exists while the manager runs: how Debian scripts test for systemd */
#endif
