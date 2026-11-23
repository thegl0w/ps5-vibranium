/* SPDX-License-Identifier: MIT */
/* The few libdrm declarations Mesa's AMD Vulkan driver uses, for Windows on the PS5 (HAVE_PS5GPU). Same names and
 * field names as libdrm's xf86drm.h; implemented in ac_ps5_drm.cpp where a function is needed at all. */
#ifndef PS5_SHIM_XF86DRM_H
#define PS5_SHIM_XF86DRM_H
#include <stdint.h>
#include <stddef.h>
#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER 2
#define DRM_NODE_MAX 3

#define DRM_BUS_PCI 0
#define DRM_BUS_USB 1
#define DRM_BUS_PLATFORM 2
#define DRM_BUS_HOST1X 3

typedef struct _drmPciBusInfo {
   uint16_t domain;
   uint8_t bus;
   uint8_t dev;
   uint8_t func;
} drmPciBusInfo, *drmPciBusInfoPtr;

typedef struct _drmPciDeviceInfo {
   uint16_t vendor_id;
   uint16_t device_id;
   uint16_t subvendor_id;
   uint16_t subdevice_id;
   uint8_t revision_id;
} drmPciDeviceInfo, *drmPciDeviceInfoPtr;

typedef struct _drmDevice {
   char **nodes;
   int available_nodes;
   int bustype;
   union {
      drmPciBusInfoPtr pci;
   } businfo;
   union {
      drmPciDeviceInfoPtr pci;
   } deviceinfo;
} drmDevice, *drmDevicePtr;

typedef struct _drmVersion {
   int version_major;
   int version_minor;
   int version_patchlevel;
   int name_len;
   char *name;
   int date_len;
   char *date;
   int desc_len;
   char *desc;
} drmVersion, *drmVersionPtr;

int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
void drmFreeDevice(drmDevicePtr *device);
int drmIoctl(int fd, unsigned long request, void *arg);

#ifdef __cplusplus
}
#endif
#endif
