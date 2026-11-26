// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _NETDB_H
#define _NETDB_H
#include <netinet/in.h>
/* Names are looked up in /etc/hosts ("localhost" always works), then by a DNS query to
 * the server in /etc/resolv.conf: the router, bin/routerd, when it runs. Only IPv4 (A)
 * records. Services must be numeric ports. */
struct addrinfo {
  int ai_flags, ai_family, ai_socktype, ai_protocol;
  socklen_t ai_addrlen;
  struct sockaddr *ai_addr;
  char *ai_canonname;
  struct addrinfo *ai_next;
};
#define AI_PASSIVE 1
#define AI_CANONNAME 2
#define AI_NUMERICHOST 4
#define AI_NUMERICSERV 0x400
#define EAI_BADFLAGS (-1)
#define EAI_NONAME (-2)
#define EAI_AGAIN (-3)
#define EAI_FAIL (-4)
#define EAI_FAMILY (-6)
#define EAI_SOCKTYPE (-7)
#define EAI_SERVICE (-8)
#define EAI_MEMORY (-10)
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int code);
struct hostent {
  char *h_name, **h_aliases;
  int h_addrtype, h_length;
  char **h_addr_list;
};
#define h_addr h_addr_list[0]
struct hostent *gethostbyname(const char *name);
#endif
