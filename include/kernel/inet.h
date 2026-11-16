/* include/kernel/inet.h -- AF_INET and AF_INET6 sockets for system calls (net/socket.c)
 *
 * SOCK_STREAM (TCP), SOCK_DGRAM (UDP, and ICMP "ping" sockets with
 * IPPROTO_ICMP) and SOCK_RAW with IPPROTO_ICMP. The calls mirror the
 * AF_UNIX ones in socket.h: kernel buffers in, the system call layer copies
 * to and from user memory.
 *
 * AF_INET6 sockets (TCP and UDP) are dual-stack sockets on the IPv4 stack:
 * they take and give IPv4-mapped addresses (::ffff:a.b.c.d), "::" for any
 * address, and "::1" for the loopback (127.0.0.1 underneath). Any other
 * IPv6 address is unreachable. */
#ifndef KESTREL_INET_H
#define KESTREL_INET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AF_UNSPEC       0
#define AF_INET         2
#define AF_INET6        10
#define AF_NETLINK      16
#define AF_PACKET       17
#define SOCK_RAW        3
#define INADDR_ANY      0u

#define SOL_IP          0
#define SOL_TCP         6
#define SOL_UDP         17
#define SOL_IPV6        41
/* SOL_SOCKET options (socket.h has SO_TYPE, SO_ERROR, SO_SNDBUF, SO_RCVBUF, SO_PEERCRED) */
#define SO_DEBUG        1
#define SO_REUSEADDR    2
#define SO_DONTROUTE    5
#define SO_BROADCAST    6
#define SO_KEEPALIVE    9
#define SO_OOBINLINE    10
#define SO_LINGER       13
#define SO_REUSEPORT    15
#define SO_PASSCRED     16
#define SO_RCVLOWAT     18
#define SO_SNDLOWAT     19
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_BINDTODEVICE 25
#define SO_TIMESTAMP    29
#define SO_ACCEPTCONN   30
#define SO_SNDBUFFORCE  32
#define SO_RCVBUFFORCE  33
#define SO_PROTOCOL     38
#define SO_DOMAIN       39
/* SOL_IP */
#define IP_TOS          1
#define IP_TTL          2
#define IP_HDRINCL      3
#define IP_RECVERR      11
#define IP_MTU_DISCOVER 10
#define IP_MULTICAST_TTL 33
#define IP_MULTICAST_LOOP 34
/* SOL_IPV6 */
#define IPV6_UNICAST_HOPS   16
#define IPV6_MULTICAST_IF   17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_MTU_DISCOVER   23
#define IPV6_RECVERR        25
#define IPV6_V6ONLY         26
#define IPV6_RECVPKTINFO    49
#define IPV6_DONTFRAG       62
#define IPV6_RECVTCLASS     66
#define IPV6_TCLASS         67
#define IPV6_ADDR_PREFERENCES 72
/* SOL_TCP */
#define TCP_NODELAY     1
#define TCP_MAXSEG      2
#define TCP_CORK        3
#define TCP_KEEPIDLE    4
#define TCP_KEEPINTVL   5
#define TCP_KEEPCNT     6
#define TCP_INFO        11
#define TCP_QUICKACK    12
#define TCP_USER_TIMEOUT 18
#define TCP_FASTOPEN    23

#define MSG_OOB         0x1
#define MSG_DONTROUTE   0x4
#define MSG_EOR         0x80
#define MSG_MORE        0x8000

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;                  /* network order */
    uint32_t sin_addr;                  /* network order */
    uint8_t  sin_zero[8];
};

struct sockaddr_in6 {
    uint16_t sin6_family;
    uint16_t sin6_port;                 /* network order */
    uint32_t sin6_flowinfo;
    uint8_t  sin6_addr[16];
    uint32_t sin6_scope_id;
};

/* an address handed back to user space: v4.sin_family says which */
union inet_sa {
    struct sockaddr_in  v4;
    struct sockaddr_in6 v6;
};
static inline uint32_t inet_sa_len(const union inet_sa *a)
{ return a->v4.sin_family == AF_INET6 ? (uint32_t)sizeof(struct sockaddr_in6) : (uint32_t)sizeof(struct sockaddr_in); }

struct file;

bool  inet_is(struct file *f);
int   inet_create(int domain, int type, int protocol, struct file **out);   /* AF_INET, AF_INET6 */
int   inet_bind(struct file *f, const void *addr, uint32_t len);
int   inet_listen(struct file *f, int backlog);
int   inet_connect(struct file *f, const void *addr, uint32_t len);
int   inet_accept(struct file *f, int flags, struct file **out, union inet_sa *peer);
/* to: NULL for the connected peer. from: filled for datagrams (may be NULL). */
long  inet_sendto(struct file *f, const void *buf, size_t len, int flags, const void *to, uint32_t tolen);
long  inet_recvfrom(struct file *f, void *buf, size_t len, int flags, union inet_sa *from, int *msg_flags);
int   inet_shutdown(struct file *f, int how);
int   inet_name(struct file *f, bool peer, union inet_sa *out);
int   inet_setsockopt(struct file *f, int level, int opt, const void *val, uint32_t len);
int   inet_getsockopt(struct file *f, int level, int opt, void *val, uint32_t *len);
int   inet_type(struct file *f);       /* SOCK_* */
/* Interface ioctls (SIOCGIFCONF, SIOCGIFADDR, ...), any socket: net/socket.c */
int   net_ioctl(unsigned long req, void *arg);
bool  net_ioctl_is(unsigned long req);
size_t net_ioctl_size(unsigned long req, const void *karg);     /* bytes to copy in/out */

#endif
