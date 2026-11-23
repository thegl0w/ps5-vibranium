/* SPDX-License-Identifier: MIT */
/* Shim for Mesa's drm.h on Windows (its "one of the BSDs" branch wants this header). The ioctl numbers are never
 * used: there is no DRM file descriptor here, the calls go to libps5gpu. */
#ifndef PS5_SHIM_IOCCOM_H
#define PS5_SHIM_IOCCOM_H
#define _IOC(dir, group, nr, size) ((unsigned long)(((dir) << 30) | (((size) & 0x1fff) << 16) | ((group) << 8) | (nr)))
#define _IO(g, n) _IOC(0, (g), (n), 0)
#define _IOR(g, n, t) _IOC(2, (g), (n), sizeof(t))
#define _IOW(g, n, t) _IOC(1, (g), (n), sizeof(t))
#define _IOWR(g, n, t) _IOC(3, (g), (n), sizeof(t))
#endif
