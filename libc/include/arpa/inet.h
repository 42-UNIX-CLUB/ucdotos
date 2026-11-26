// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <netinet/in.h>
int inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
in_addr_t inet_addr(const char *s);
int inet_aton(const char *s, struct in_addr *out);
char *inet_ntoa(struct in_addr a);
#endif
