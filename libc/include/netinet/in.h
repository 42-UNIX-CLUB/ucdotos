// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _NETINET_IN_H
#define _NETINET_IN_H
#include <sys/socket.h>
/* struct in_addr and struct sockaddr_in are in abi/abi.h. */
typedef unsigned in_addr_t;
typedef unsigned short in_port_t;
#define INADDR_ANY ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE ((in_addr_t)0xffffffff)
#define INET_ADDRSTRLEN 16
#define IPPROTO_RAW 255
static inline unsigned short htons(unsigned short v) { return (unsigned short)(v << 8 | v >> 8); }
static inline unsigned short ntohs(unsigned short v) { return (unsigned short)(v << 8 | v >> 8); }
static inline unsigned htonl(unsigned v) { return __builtin_bswap32(v); }
static inline unsigned ntohl(unsigned v) { return __builtin_bswap32(v); }
#endif
