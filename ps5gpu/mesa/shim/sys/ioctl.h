/* SPDX-License-Identifier: MIT */
/* Shim for Mesa's util/libsync.h on Windows: no file descriptors to ioctl; every call fails. */
#ifndef PS5_SHIM_SYS_IOCTL_H
#define PS5_SHIM_SYS_IOCTL_H
#include <errno.h>
#include <sys/ioccom.h>
static inline int ioctl(int fd, unsigned long request, ...)
{
   (void)fd; (void)request;
   errno = ENOSYS;
   return -1;
}
#endif
