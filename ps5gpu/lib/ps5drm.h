/* SPDX-License-Identifier: MIT */
/* libps5gpu: what a program (Mesa) needs from a kernel graphics driver, in user mode on top of ps5gpu.sys.
 *
 * The GPU must have been started first (ps5gpu2 run: microcode, ring). Up to 15 clients at a time (programs, or
 * several p5_open in one program): each gets its own GPU address space (VMID 1-15); the buffer heap, the page-table
 * pool and the command ring are shared, with their bookkeeping in VRAM under a system-wide lock. The first client
 * (none attached) starts from an empty heap.
 *
 * Memory model: every buffer is a piece of the VRAM carve-out (which is ordinary RAM that the CPU can map), so there
 * is no system-memory domain and no GART. GPU addresses of buffers are whatever the client maps them at in its own address space.
 */
#ifndef PS5DRM_H
#define PS5DRM_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct p5_dev p5_dev;

#define P5_VA_MAP 1u
#define P5_VA_UNMAP 2u
#define P5_VA_CLEAR 3u      /* same as unmap here: no sparse "PRT" pages yet */
#define P5_VA_REPLACE 4u

#define P5_VA_READ 0x2u     /* AMDGPU_VM_PAGE_READABLE, WRITEABLE, EXECUTABLE */
#define P5_VA_WRITE 0x4u
#define P5_VA_EXEC 0x8u

#define P5_IB_CE 0x1u       /* AMDGPU_IB_FLAG_CE */
#define P5_IB_PREAMBLE 0x2u /* AMDGPU_IB_FLAG_PREAMBLE */

typedef struct p5_ib {
    uint64_t va;            /* in the client's address space */
    uint32_t bytes;
    uint32_t flags;
} p5_ib;

typedef struct p5_info {
    uint64_t vram_bytes;    /* the whole carve-out */
    uint64_t heap_bytes;    /* what buffers can use */
    uint64_t heap_free;
    uint64_t va_start, va_end;   /* usable GPU virtual addresses */
    uint64_t submitted, completed;   /* this client's last sequence number; the last one the GPU finished */
    uint32_t buffers;       /* this client's */
    uint32_t pt_pages;
    uint32_t vmid, clients; /* this client's GPU address space (1-15); programs attached right now */
    uint32_t rb_mask, cu_count, cu_bitmap[4];   /* active render backends and compute units (read once, GPU idle) */
} p5_info;

/* 0 on success, negative errno-style values otherwise. */
int p5_open(p5_dev **dev);
void p5_close(p5_dev *dev);
int p5_get_info(p5_dev *dev, p5_info *info);
/* How many programs are attached right now (without attaching); negative: error. */
int p5_clients(void);

/* Showing a buffer on the screen directly ("scan-out flip"), for full-screen programs: no copy, taken by the display at
 * its next frame. The buffer must hold a picture of exactly the screen's size as 32-bit BGRA with the screen's pitch
 * (p5_scanout_query), from `offset` on. One program at a time has the screen (-EBUSY for the others); it gives it back
 * with p5_scanout_restore or by closing, and if it dies the next p5_open or p5_housekeeping puts the desktop back. */
typedef struct p5_scanout {
    uint32_t width, height, pitch_pixels;
    uint32_t flipped_by_pid;        /* 0: the desktop is showing */
    uint64_t phys_now, phys_desktop;
} p5_scanout;
int p5_scanout_query(p5_dev *dev, p5_scanout *out);
int p5_scanout_flip(p5_dev *dev, uint32_t handle, uint64_t offset);
/* 1 while the display has not yet switched to the buffer of the last p5_scanout_flip (it does at its next frame). */
int p5_scanout_pending(p5_dev *dev);
int p5_scanout_restore(p5_dev *dev);
/* Saves what the display is showing right now (whoever owns the screen) as a 32-bit BMP file. Needs no client. */
int p5_scanout_snapshot(const char *path);
/* Cleans up after programs that died: gives the screen back to the desktop, frees their memory. Needs no client.
 * Returns bit 0: screen restored, bit 1: clients removed; negative: error. */
int p5_housekeeping(void);
/* For a watchdog, without attaching: 0 fine, 1 GPU not started, 2 command processor halted, 3 work waiting (stuck if
 * *fence does not move between two calls), negative on errors. */
int p5_health(uint64_t* seq, uint64_t* fence);
/* Process ids of the programs that have the GPU open; returns how many (at most max). */
int p5_client_pids(uint32_t* pids, int max);
/* For `ps5gpu2 run` only (administrator), right before it restarts the GPU: makes this Windows start's session id and
 * stamps the shared state with it, and empties the client table (nobody may be attached: run checks p5_clients first).
 * After a warm Restart the table of the Windows before then counts as gone everywhere. 0 or negative errno. */
int p5_run_begin(void);
/* The shared state as text (clients, their pids and memory, heap), without attaching. For debugging. */
int p5_state_text(char* out, int size);
/* Debugging: map the shared area and pool (and the whole heap if `heap`) like p5_open, hold them `ms`, unmap; no
 * client, no state change. Returns how long the mapping took in microseconds. */
int p5_map_probe(int heap, int ms);

int p5_bo_alloc(p5_dev *dev, uint64_t bytes, uint64_t alignment, uint32_t *handle);
int p5_bo_free(p5_dev *dev, uint32_t handle);
int p5_bo_query(p5_dev *dev, uint32_t handle, uint64_t *bytes, void **cpu);

/* Where the CPU sees the memory mapped at a GPU virtual address (NULL if nothing is mapped there). For debugging. */
void *p5_va_cpu(p5_dev *dev, uint64_t va);

int p5_va_op(p5_dev *dev, uint32_t handle, uint64_t offset, uint64_t bytes, uint64_t va, uint32_t flags, uint32_t op);

int p5_ctx_create(p5_dev *dev, uint32_t *ctx);
int p5_ctx_free(p5_dev *dev, uint32_t ctx);

/* Submits the buffers in order on the graphics ring. If fence_handle is not 0, the 64-bit sequence number is also
 * written to that buffer at fence_offset when the work is done (the "user fence"). */
int p5_submit(p5_dev *dev, uint32_t ctx, const p5_ib *ibs, uint32_t count, uint32_t fence_handle, uint64_t fence_offset, uint64_t *seq);
/* 0 = done, 1 = timed out. */
int p5_wait(p5_dev *dev, uint64_t seq, uint64_t timeout_ns);
uint64_t p5_completed(p5_dev *dev);

#ifdef __cplusplus
}
#endif
#endif
