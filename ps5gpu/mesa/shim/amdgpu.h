/* SPDX-License-Identifier: MIT */
/* The libdrm_amdgpu types Mesa's AMD Vulkan driver uses, for Windows on the PS5 (HAVE_PS5GPU). Written from the
 * names Mesa uses (and the definitions Mesa itself carries for its Windows build in ac_linux_drm.h); both sides of
 * every call are compiled against this file, so only the names have to agree with libdrm, not the layout. */
#ifndef PS5_SHIM_AMDGPU_H
#define PS5_SHIM_AMDGPU_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMDGPU_TIMEOUT_INFINITE 0xffffffffffffffffull
#define AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE (1 << 0)

#define AMDGPU_VA_RANGE_32_BIT 0x1
#define AMDGPU_VA_RANGE_HIGH 0x2
#define AMDGPU_VA_RANGE_REPLAYABLE 0x4

enum amdgpu_bo_handle_type {
   amdgpu_bo_handle_type_gem_flink_name = 0,
   amdgpu_bo_handle_type_kms = 1,
   amdgpu_bo_handle_type_dma_buf_fd = 2,
   amdgpu_bo_handle_type_kms_noimport = 3,
};

enum amdgpu_gpu_va_range {
   amdgpu_gpu_va_range_general = 0
};

enum amdgpu_sw_info {
   amdgpu_sw_info_address32_hi = 0,
   amdgpu_sw_info_address_prt_wa_control_bit = 1,
};

typedef struct amdgpu_device *amdgpu_device_handle;
typedef struct amdgpu_context *amdgpu_context_handle;
typedef struct amdgpu_bo *amdgpu_bo_handle;
typedef struct amdgpu_va *amdgpu_va_handle;

struct amdgpu_bo_alloc_request {
   uint64_t alloc_size;
   uint64_t phys_alignment;
   uint32_t preferred_heap;
   uint64_t flags;
};

struct amdgpu_bo_metadata {
   uint64_t flags;
   uint64_t tiling_info;
   uint32_t size_metadata;
   uint32_t umd_metadata[64];
};

struct amdgpu_bo_info {
   uint64_t alloc_size;
   uint64_t phys_alignment;
   uint32_t preferred_heap;
   uint64_t alloc_flags;
   struct amdgpu_bo_metadata metadata;
};

struct amdgpu_bo_import_result {
   amdgpu_bo_handle buf_handle;
   uint64_t alloc_size;
};

struct amdgpu_heap_info {
   uint64_t heap_size;
   uint64_t heap_usage;
   uint64_t max_allocation;
};

struct amdgpu_cs_fence {
   amdgpu_context_handle context;
   uint32_t ip_type;
   uint32_t ip_instance;
   uint32_t ring;
   uint64_t fence;
};

struct amdgpu_cs_fence_info {
   amdgpu_bo_handle handle;
   uint64_t offset;
};

struct amdgpu_gpu_info {
   uint32_t asic_id;
   uint32_t chip_rev;
   uint32_t chip_external_rev;
   uint32_t family_id;
   uint64_t ids_flags;
   uint64_t max_engine_clk;
   uint64_t max_memory_clk;
   uint32_t num_shader_engines;
   uint32_t num_shader_arrays_per_engine;
   uint32_t avail_quad_shader_pipes;
   uint32_t max_quad_shader_pipes;
   uint32_t cache_entries_per_quad_pipe;
   uint32_t num_hw_gfx_contexts;
   uint32_t rb_pipes;
   uint32_t enabled_rb_pipes_mask;
   uint32_t gpu_counter_freq;
   uint32_t backend_disable[4];
   uint32_t mc_arb_ramcfg;
   uint32_t gb_addr_cfg;
   uint32_t gb_tile_mode[32];
   uint32_t gb_macro_tile_mode[16];
   uint32_t pa_sc_raster_cfg[4];
   uint32_t pa_sc_raster_cfg1[4];
   uint32_t cu_active_number;
   uint32_t cu_ao_mask;
   uint32_t cu_bitmap[4][4];
   uint32_t vram_type;
   uint32_t vram_bit_width;
   uint32_t ce_ram_size;
   uint32_t vce_harvest_config;
   uint32_t pci_rev_id;
};

#ifdef __cplusplus
}
#endif
#endif
