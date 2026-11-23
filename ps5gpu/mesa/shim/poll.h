/* SPDX-License-Identifier: MIT */
/* Shim for Mesa's util/libsync.h on Windows: there are no sync files here, so poll() always fails. */
#ifndef PS5_SHIM_POLL_H
#define PS5_SHIM_POLL_H
#include <errno.h>
struct pollfd {
   int fd;
   short events;
   short revents;
};
#define POLLIN 0x1
#define POLLERR 0x8
#define POLLNVAL 0x20
static inline int poll(struct pollfd *fds, unsigned long nfds, int timeout)
{
   (void)fds; (void)nfds; (void)timeout;
   errno = ENOSYS;
   return -1;
}
#endif
