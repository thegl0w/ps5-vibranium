// SPDX-License-Identifier: MIT
// Mesa's kernel interface (ac_drm_*, a few drm* helpers, a sync provider) on libps5gpu, for Windows on the PS5.
// Compiled into Mesa when -Dps5gpu=true (src/amd/common/meson.build).
//
// One GPU client process at a time, one device per process (ac_drm_device_initialize hands out the same device,
// reference counted, as libdrm does for one fd). Every buffer lives in VRAM, whatever domain was asked for.
// Fences are sequence numbers on the one graphics ring; a sync object is "signalled", "empty", or "sequence N".
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <map>
#include <mutex>
#include <vector>
#include <windows.h>

#include "common/ac_linux_drm.h"

// The process environment as it is now (a module's C runtime may have copied it before our user-mode driver added
// variables from C:\ps5\gpu\umd-env.txt). Each name gets its own buffer, kept for the life of the process.
// At load: copy RADV_*, ACO_*, MESA_* and PS5GPU_* from the live process environment into this module's C runtime,
// so that options our user-mode driver set (umd-env.txt) reach Mesa's own getenv calls.
static struct P5EnvSync {
   P5EnvSync()
   {
      char *block = GetEnvironmentStringsA();
      for (char *e = block; e && *e; e += strlen(e) + 1)
         if (!strncmp(e, "RADV_", 5) || !strncmp(e, "ACO_", 4) || !strncmp(e, "MESA_", 5) || !strncmp(e, "PS5GPU_", 7)) _putenv(e);
      if (block) FreeEnvironmentStringsA(block);
   }
} g_p5_env_sync;

static const char *p5env(const char *name)
{
   char buf[512];
   DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
   if (n == 0 || n >= sizeof buf) return getenv(name);
   return _strdup(buf);
}
#include "common/ac_gpu_info.h"
#include "common/ac_debug.h"
#include "util/os_time.h"
#include "util/u_sync_provider.h"

#include "../lib/ps5drm.h"
#include "../tool/backend.h"
#include "../tool/regs.h"

struct amdgpu_bo {
   uint32_t handle;
   uint64_t size;
   void *cpu;
   uint32_t preferred_heap;
   uint64_t flags;
   uint64_t phys_alignment;
};

struct amdgpu_va {
   ac_drm_device *dev;
   uint64_t base, size;
   int range;
};

namespace {

// A fence: either "already signalled" or "done when submission number seq has completed".
struct Fence {
   bool signaled = false, has_fence = false;
   uint64_t seq = 0;
   bool exists() const { return signaled || has_fence; }
};
// One point of a timeline sync object (what the kernel keeps as a dma_fence_chain link).
struct TimelinePoint {
   uint64_t value;
   Fence fence;
};
// A sync object has a binary state (signaled / has_fence / seq) and, when used as a timeline, a list of points in
// ascending order. Point 0 always means the binary state.
struct SyncObj {
   bool used = false, signaled = false, has_fence = false;
   uint64_t seq = 0;
   std::vector<TimelinePoint> chain;
};

struct Provider {
   struct util_sync_provider base;
   ac_drm_device *dev;
};

// GPU virtual address ranges handed to Mesa: a 32-bit one and the rest.
const uint64_t kVaLowStart = 0x200000ull, kVaLowEnd = 0x100000000ull;
// The upper range ends at bit 46: Mesa's workaround for scalar loads from unbound sparse pages ("SMEM with NULL PRT")
// wants one address bit that is 0 in every ordinary allocation; it sets that bit itself for the sparse buffers it
// emulates, and its shaders mask the bit out of buffer addresses. See ac_drm_query_sw_info.
const unsigned kPrtControlBit = 46;
const uint64_t kVaHighStart = 0x100000000ull, kVaHighEnd = 1ull << kPrtControlBit;

}

struct ac_drm_device {
   p5_dev *p5 = nullptr;
   int refs = 0;
   std::mutex lock;
   std::map<uint32_t, amdgpu_bo *> bos;
   std::vector<SyncObj> syncs;
   std::map<uint64_t, uint64_t> va_free[2];       // start -> size
   uint64_t vram_used = 0;
   struct drm_amdgpu_info_device info = {};
   uint32_t gb_addr_config = 0;
   Provider provider = {};
};

namespace {

std::mutex g_lock;
ac_drm_device *g_dev;

// PS5GPU_TRACE=<file>: one line per buffer, address-space and submission call, for finding out afterwards what was
// (or was not) mapped at an address the GPU faulted on. Flushed at every submission.
FILE *g_trace;
std::mutex g_trace_lock;
void trace(const char *fmt, ...)
{
   if (!g_trace) return;
   va_list ap;
   va_start(ap, fmt);
   std::lock_guard<std::mutex> g(g_trace_lock);
   vfprintf(g_trace, fmt, ap);
   va_end(ap);
}

void sleep_a_bit(uint64_t waited_us)
{
   if (waited_us < 200)
      YieldProcessor();
   else if (waited_us < 4000)
      SwitchToThread();
   else
      Sleep(1);
}

uint32_t rd(uint32_t reg)
{
   uint32_t v = 0;
   be::rd(reg, v);
   return v;
}

// What Linux's AMDGPU_INFO_DEV_INFO returns for this chip. The fixed numbers are the ones the PS5 kernel patch
// hard-codes (amdgpu_device.c: no IP discovery table on the console); the harvest-dependent ones come from the
// library (lib/ps5drm.cpp, readHwConfig).
void fill_device_info(ac_drm_device *dev)
{
   struct drm_amdgpu_info_device &d = dev->info;
   const uint32_t max_se = 2, max_sh_per_se = 2, max_cu_per_sh = 10;
   p5_info pi;
   p5_get_info(dev->p5, &pi);

   d.device_id = 0x13FC;
   d.chip_rev = 3;                        // as Linux reports on this console (gpu-info/linux-20261120/drm-info.txt)
   d.external_rev = 0x84;                 // nv.c with the PS5 patch: the BC-250's id, which Mesa knows as GFX 10.1.3
   d.pci_rev = 0;
   d.family = AMDGPU_FAMILY_NV;
   d.num_shader_engines = max_se;
   d.num_shader_arrays_per_engine = max_sh_per_se;
   d.gpu_counter_freq = 100000;           // kHz
   d.num_hw_gfx_contexts = 8;
   d.ids_flags = 0;                       // not FUSION: all our memory is "dedicated", CPU-visible VRAM
   d.virtual_address_offset = kVaLowStart;
   d.virtual_address_max = kVaHighEnd;
   d.virtual_address_alignment = 4096;
   d.pte_fragment_size = 4096;
   d.gart_page_size = 4096;
   d.ce_ram_size = 0x10000;
   d.vram_type = AMDGPU_VRAM_TYPE_GDDR6;
   d.vram_bit_width = 128;
   d.gc_double_offchip_lds_buf = 1;
   d.wave_front_size = 32;
   d.num_shader_visible_vgprs = 1024;
   d.num_cu_per_sh = max_cu_per_sh;
   d.num_tcc_blocks = 16;
   d.gs_vgt_table_depth = 32;
   d.gs_prim_buffer_depth = 1792;
   d.max_gs_waves_per_vgt = 32;
   d.high_va_offset = 0;
   d.high_va_max = 0;
   d.pa_sc_tile_steering_override = 0x122000;   // Linux's value here; Mesa writes it to PA_SC_TILE_STEERING_OVERRIDE
   d.max_engine_clock = d.max_memory_clock = 200000;
   d.min_engine_clock = d.min_memory_clock = 200000;
   d.tcc_disabled_mask = 0;

   // Which render backends and compute units exist is read by the library once, while the GPU is idle (it needs
   // GRBM_GFX_INDEX, which must not change under another program's running work).
   uint32_t active_rbs = pi.rb_mask, cu_total = pi.cu_count;
   for (uint32_t se = 0; se < max_se; ++se)
      for (uint32_t sh = 0; sh < max_sh_per_se; ++sh) d.cu_bitmap[se][sh] = pi.cu_bitmap[se * max_sh_per_se + sh];
   // Linux does not read this register on Cyan Skillfish: it reports CYAN_SKILLFISH_GB_ADDR_CONFIG_GOLDEN (gfx_v10_0.c).
   // The register itself read 0x44 under Windows; Mesa on Linux lays images out for the golden value.
   const uint32_t gb_live = rd(GC(mmGB_ADDR_CONFIG));
   dev->gb_addr_config = 0x00100044;
   d.enabled_rb_pipes_mask = active_rbs;
   d.num_rb_pipes = (uint32_t)__builtin_popcount(active_rbs);
   d.cu_active_number = cu_total;
   d.cu_ao_mask = 0xffffffff;
   if (p5env("PS5GPU_DEBUG"))
      fprintf(stderr, "ps5gpu: rb mask 0x%x (%u), cu %u [%x %x %x %x], GB_ADDR_CONFIG 0x%08x, heap %llu MiB\n", active_rbs, d.num_rb_pipes, cu_total,
              d.cu_bitmap[0][0], d.cu_bitmap[0][1], d.cu_bitmap[1][0], d.cu_bitmap[1][1], gb_live, (unsigned long long)(pi.heap_bytes >> 20));
}

// ---- debugging aid: where are we? (PS5GPU_DEBUG) ----
// Prints the call chain as offsets into this DLL; resolve them on the build machine with
//   x86_64-w64-mingw32-addr2line -f -e vulkan_radeon.dll <image base + offset>   (or nm, sorted)
#ifdef _WIN64
static void print_chain(const char *why, CONTEXT ctx)
{
   HMODULE self = nullptr;
   GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&print_chain, &self);
   fprintf(stderr, "ps5gpu: %s; call chain (offsets in the DLL):", why);
   if (!ctx.Rip && !IsBadReadPtr((void *)ctx.Rsp, 8)) {      // a call through a null pointer: the caller is on the stack
      fprintf(stderr, " (call to address 0 from)");
      ctx.Rip = *(DWORD64 *)ctx.Rsp;
      ctx.Rsp += 8;
   }
   for (int depth = 0; depth < 24 && ctx.Rip; depth++) {
      HMODULE mod = nullptr;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ctx.Rip, &mod);
      if (mod == self) fprintf(stderr, " +0x%llx", (unsigned long long)(ctx.Rip - (uintptr_t)self));
      else if (mod) {                           // another module: its file name and the offset in it
         char path[MAX_PATH] = "";
         GetModuleFileNameA(mod, path, sizeof path);
         const char *name = strrchr(path, '\\');
         fprintf(stderr, " %s+0x%llx", name ? name + 1 : path, (unsigned long long)(ctx.Rip - (uintptr_t)mod));
      } else fprintf(stderr, " [%p]", (void *)ctx.Rip);
      DWORD64 base = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
      if (!fn) {                               // a leaf: the return address is on top of the stack
         if (IsBadReadPtr((void *)ctx.Rsp, 8)) break;
         ctx.Rip = *(DWORD64 *)ctx.Rsp;
         ctx.Rsp += 8;
         continue;
      }
      void *handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
   }
   fprintf(stderr, "\n");
   fflush(stderr);
}
#else
// 32-bit: instruction pointer, then return addresses along the frame-pointer chain (as far as it is readable).
static void print_chain(const char *why, CONTEXT ctx)
{
   HMODULE self = nullptr;
   GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&print_chain, &self);
   fprintf(stderr, "ps5gpu: %s; call chain (offsets in the DLL):", why);
   uintptr_t ip = ctx.Eip, bp = ctx.Ebp;
   for (int depth = 0; depth < 24 && ip; depth++) {
      HMODULE mod = nullptr;
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ip, &mod);
      if (mod == self) fprintf(stderr, " +0x%lx", (unsigned long)(ip - (uintptr_t)self));
      else fprintf(stderr, " [%p]", (void *)ip);
      if (!bp || IsBadReadPtr((void *)bp, 8)) break;
      ip = ((uintptr_t *)bp)[1];
      bp = ((uintptr_t *)bp)[0];
   }
   fprintf(stderr, "\n");
   fflush(stderr);
}
#endif

// Removed again when vulkan_radeon.dll is unloaded: a handler left behind in freed code is called on the next exception
// anywhere in the process (Control Flow Guard fast fail).
static PVOID g_crash_handler;
static LONG CALLBACK crash_printer(EXCEPTION_POINTERS *info);
__attribute__((destructor)) static void crash_printer_unload(void)
{
   if (g_crash_handler) { RemoveVectoredExceptionHandler(g_crash_handler); g_crash_handler = nullptr; }
}

static LONG CALLBACK crash_printer(EXCEPTION_POINTERS *info)
{
   DWORD code = info->ExceptionRecord->ExceptionCode;
   if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
       code != EXCEPTION_INT_DIVIDE_BY_ZERO)
      return EXCEPTION_CONTINUE_SEARCH;
   char why[96];
   snprintf(why, sizeof why, "exception 0x%08lx, address 0x%llx", (unsigned long)code,
            (unsigned long long)(info->ExceptionRecord->NumberParameters >= 2 ? info->ExceptionRecord->ExceptionInformation[1] : 0));
   print_chain(why, *info->ContextRecord);
   return EXCEPTION_CONTINUE_SEARCH;
}

// ---- sync objects ----

bool sync_done(ac_drm_device *dev, const SyncObj &s)
{
   return s.signaled || (s.has_fence && p5_completed(dev->p5) >= s.seq);
}

bool fence_done(ac_drm_device *dev, const Fence &f)
{
   return f.signaled || (f.has_fence && p5_completed(dev->p5) >= f.seq);
}

// The fence that stands for `point` of a sync object: the binary state for point 0, otherwise the first timeline
// point at or after it (as dma_fence_chain_find_seqno). False while nothing has been submitted for it yet.
bool sync_fence(const SyncObj &s, uint64_t point, Fence *out)
{
   if (point == 0) {
      out->signaled = s.signaled;
      out->has_fence = s.has_fence;
      out->seq = s.seq;
      return out->exists();
   }
   for (const TimelinePoint &t : s.chain) {
      if (t.value >= point) {
         *out = t.fence;
         return true;
      }
   }
   return false;
}

// Adds a point; finished points before the newest finished one are dropped (only the last one matters for queries).
void sync_add_point(ac_drm_device *dev, SyncObj &s, uint64_t value, const Fence &f)
{
   auto it = s.chain.begin();
   while (it != s.chain.end() && it->value < value) ++it;
   if (it != s.chain.end() && it->value == value) it->fence = f;
   else s.chain.insert(it, TimelinePoint{value, f});
   size_t done = 0;
   while (done < s.chain.size() && fence_done(dev, s.chain[done].fence)) ++done;
   if (done > 1) s.chain.erase(s.chain.begin(), s.chain.begin() + (done - 1));
}

SyncObj *sync_get(ac_drm_device *dev, uint32_t handle)
{
   if (handle == 0 || handle > dev->syncs.size() || !dev->syncs[handle - 1].used) return nullptr;
   return &dev->syncs[handle - 1];
}

int sync_create(ac_drm_device *dev, uint32_t flags, uint32_t *handle)
{
   std::lock_guard<std::mutex> g(dev->lock);
   uint32_t i = 0;
   while (i < dev->syncs.size() && dev->syncs[i].used) ++i;
   if (i == dev->syncs.size()) dev->syncs.emplace_back();
   dev->syncs[i] = SyncObj();
   dev->syncs[i].used = true;
   dev->syncs[i].signaled = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0;
   *handle = i + 1;
   return 0;
}

int sync_destroy(ac_drm_device *dev, uint32_t handle)
{
   std::lock_guard<std::mutex> g(dev->lock);
   SyncObj *s = sync_get(dev, handle);
   if (!s) return -EINVAL;
   s->used = false;
   return 0;
}

// DRM_IOCTL_SYNCOBJ_WAIT: the timeout is an absolute time on the monotonic clock (Mesa's os_time_get_nano).
int sync_wait(ac_drm_device *dev, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled,
              const uint64_t *points = nullptr)
{
   uint64_t start = (uint64_t)os_time_get_nano();
   for (;;) {
      unsigned done = 0;
      int first = -1;
      {
         std::lock_guard<std::mutex> g(dev->lock);
         for (unsigned i = 0; i < count; ++i) {
            SyncObj *s = sync_get(dev, handles[i]);
            if (!s) return -EINVAL;
            Fence f;
            if (!sync_fence(*s, points ? points[i] : 0, &f)) {
               if (!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)) return -EINVAL;
               continue;
            }
            // WAIT_AVAILABLE: a fence being attached is enough.
            if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) || fence_done(dev, f)) {
               ++done;
               if (first < 0) first = (int)i;
            }
         }
      }
      if (count == 0 || ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) ? done == count : done > 0)) {
         if (first_signaled) *first_signaled = first < 0 ? 0 : (uint32_t)first;
         return 0;
      }
      int64_t now = os_time_get_nano();
      if (timeout_nsec <= now) return -ETIME;
      sleep_a_bit(((uint64_t)now - start) / 1000);
   }
}

int sync_reset_or_signal(ac_drm_device *dev, const uint32_t *handles, uint32_t count, bool signal)
{
   std::lock_guard<std::mutex> g(dev->lock);
   for (uint32_t i = 0; i < count; ++i) {
      SyncObj *s = sync_get(dev, handles[i]);
      if (!s) return -EINVAL;
      s->signaled = signal;
      s->has_fence = false;
      s->seq = 0;
      s->chain.clear();
   }
   return 0;
}

// DRM_IOCTL_SYNCOBJ_TRANSFER: the fence of (src, src_point) becomes dst's binary fence (dst_point 0) or a new point
// of dst's timeline.
int sync_transfer(ac_drm_device *dev, uint32_t dst, uint32_t src, uint64_t dst_point = 0, uint64_t src_point = 0)
{
   std::lock_guard<std::mutex> g(dev->lock);
   SyncObj *d = sync_get(dev, dst), *s = sync_get(dev, src);
   if (!d || !s) return -EINVAL;
   Fence f;
   if (!sync_fence(*s, src_point, &f)) return -EINVAL;
   if (dst_point == 0) {
      d->signaled = f.signaled;
      d->has_fence = f.has_fence;
      d->seq = f.seq;
   } else {
      sync_add_point(dev, *d, dst_point, f);
   }
   return 0;
}

// DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL: each point becomes an already-signalled point (point 0: the binary state).
int sync_timeline_signal(ac_drm_device *dev, const uint32_t *handles, const uint64_t *points, uint32_t count)
{
   std::lock_guard<std::mutex> g(dev->lock);
   for (uint32_t i = 0; i < count; ++i) {
      SyncObj *s = sync_get(dev, handles[i]);
      if (!s) return -EINVAL;
      Fence f;
      f.signaled = true;
      if (!points || points[i] == 0) {
         s->signaled = true;
         s->has_fence = false;
         s->seq = 0;
      } else {
         sync_add_point(dev, *s, points[i], f);
      }
   }
   return 0;
}

// DRM_IOCTL_SYNCOBJ_QUERY: the last finished point of each timeline (or, with LAST_SUBMITTED, the last point added).
int sync_query(ac_drm_device *dev, const uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags)
{
   std::lock_guard<std::mutex> g(dev->lock);
   for (uint32_t i = 0; i < count; ++i) {
      SyncObj *s = sync_get(dev, handles[i]);
      if (!s) return -EINVAL;
      uint64_t v = 0;
      if (flags & 1 /* DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED */) {
         if (!s->chain.empty()) v = s->chain.back().value;
      } else {
         for (const TimelinePoint &t : s->chain) {
            if (!fence_done(dev, t.fence)) break;
            v = t.value;
         }
      }
      points[i] = v;
   }
   return 0;
}

ac_drm_device *pdev(struct util_sync_provider *p) { return ((Provider *)p)->dev; }

int prov_create(struct util_sync_provider *p, uint32_t flags, uint32_t *handle) { return sync_create(pdev(p), flags, handle); }
int prov_destroy(struct util_sync_provider *p, uint32_t handle) { return sync_destroy(pdev(p), handle); }
int prov_no_fd_out(struct util_sync_provider *, uint32_t, int *out) { if (out) *out = -1; errno = ENOSYS; return -1; }
int prov_fd_to_handle(struct util_sync_provider *, int, uint32_t *) { errno = ENOSYS; return -1; }
int prov_import_sync_file(struct util_sync_provider *, uint32_t, int) { errno = ENOSYS; return -1; }
int prov_wait(struct util_sync_provider *p, uint32_t *handles, unsigned n, int64_t timeout, unsigned flags, uint32_t *first)
{
   int r = sync_wait(pdev(p), handles, n, timeout, flags, first);
   if (r < 0) { errno = -r; return -1; }
   return 0;
}
int prov_reset(struct util_sync_provider *p, const uint32_t *handles, uint32_t n) { return sync_reset_or_signal(pdev(p), handles, n, false); }
int prov_signal(struct util_sync_provider *p, const uint32_t *handles, uint32_t n) { return sync_reset_or_signal(pdev(p), handles, n, true); }
int prov_transfer(struct util_sync_provider *p, uint32_t dst, uint64_t dst_point, uint32_t src, uint64_t src_point, uint32_t)
{
   int r = sync_transfer(pdev(p), dst, src, dst_point, src_point);
   if (r < 0) { errno = -r; return -1; }
   return 0;
}
int prov_timeline_signal(struct util_sync_provider *p, const uint32_t *handles, uint64_t *points, uint32_t n)
{
   int r = sync_timeline_signal(pdev(p), handles, points, n);
   if (r < 0) { errno = -r; return -1; }
   return 0;
}
int prov_timeline_wait(struct util_sync_provider *p, uint32_t *handles, uint64_t *points, unsigned n, int64_t timeout, unsigned flags, uint32_t *first)
{
   int r = sync_wait(pdev(p), handles, n, timeout, flags, first, points);
   if (r < 0) { errno = -r; return -1; }
   return 0;
}
int prov_query(struct util_sync_provider *p, uint32_t *handles, uint64_t *points, uint32_t n, uint32_t flags)
{
   int r = sync_query(pdev(p), handles, points, n, flags);
   if (r < 0) { errno = -r; return -1; }
   return 0;
}
void prov_finalize(struct util_sync_provider *p)
{
   if (p != &pdev(p)->provider.base) free(p);
}
struct util_sync_provider *prov_clone(struct util_sync_provider *p)
{
   Provider *c = (Provider *)malloc(sizeof(Provider));
   *c = *(Provider *)p;
   return &c->base;
}

void provider_init(ac_drm_device *dev)
{
   Provider &p = dev->provider;
   memset(&p, 0, sizeof p);
   p.dev = dev;
   p.base.create = prov_create;
   p.base.destroy = prov_destroy;
   p.base.handle_to_fd = prov_no_fd_out;
   p.base.fd_to_handle = prov_fd_to_handle;
   p.base.import_sync_file = prov_import_sync_file;
   p.base.export_sync_file = prov_no_fd_out;
   p.base.wait = prov_wait;
   p.base.reset = prov_reset;
   p.base.signal = prov_signal;
   p.base.transfer = prov_transfer;
   p.base.finalize = prov_finalize;
   p.base.clone = prov_clone;
   p.base.timeline_signal = prov_timeline_signal;
   p.base.timeline_wait = prov_timeline_wait;
   p.base.query = prov_query;
}

// ---- address ranges ----

void va_release(ac_drm_device *dev, int range, uint64_t base, uint64_t size)
{
   auto &m = dev->va_free[range];
   auto next = m.lower_bound(base);
   if (next != m.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second == base) {
         base = prev->first;
         size += prev->second;
         m.erase(prev);
      }
   }
   if (next != m.end() && base + size == next->first) {
      size += next->second;
      m.erase(next);
   }
   m[base] = size;
}

}   // namespace

extern "C" {

// ---- libdrm helpers Mesa calls directly ----

int drmGetCap(int, uint64_t capability, uint64_t *value)
{
   *value = (capability == DRM_CAP_SYNCOBJ || capability == DRM_CAP_SYNCOBJ_TIMELINE) ? 1 : 0;
   return 0;
}

int drmGetDevice2(int, uint32_t, drmDevicePtr *device)
{
   struct Block {
      drmDevice dev;
      drmPciBusInfo bus;
      drmPciDeviceInfo info;
   };
   Block *b = (Block *)calloc(1, sizeof(Block));
   b->bus.domain = 0;
   b->bus.bus = 0x20;
   b->bus.dev = 0;
   b->bus.func = 0;
   b->info.vendor_id = 0x1002;
   b->info.device_id = 0x13FC;
   b->info.subvendor_id = 0x1022;
   b->info.subdevice_id = 0;
   b->dev.bustype = DRM_BUS_PCI;
   b->dev.available_nodes = 1 << DRM_NODE_RENDER;
   b->dev.businfo.pci = &b->bus;
   b->dev.deviceinfo.pci = &b->info;
   *device = &b->dev;
   return 0;
}

void drmFreeDevice(drmDevicePtr *device)
{
   free(*device);
   *device = NULL;
}

int drmIoctl(int, unsigned long, void *)
{
   errno = ENOSYS;
   return -1;
}

// ---- device ----

int ac_drm_device_initialize(int, bool, uint32_t *major_version, uint32_t *minor_version, ac_drm_device **device_handle)
{
   std::lock_guard<std::mutex> g(g_lock);
   if (!g_dev) {
      static bool hooked;
      if (!hooked && p5env("PS5GPU_DEBUG")) {
         g_crash_handler = AddVectoredExceptionHandler(1, crash_printer);
         hooked = true;
      }
      if (!g_trace && p5env("PS5GPU_TRACE")) {
         g_trace = fopen(p5env("PS5GPU_TRACE"), "w");
         if (g_trace) setvbuf(g_trace, nullptr, _IOFBF, 1 << 20);
      }
      p5_dev *p5 = nullptr;
      int r = p5_open(&p5);
      if (r) {
         fprintf(stderr, "ps5gpu: cannot open the GPU (%d)%s\n", r,
                 r == -ENXIO ? ": it has not been started (ps5gpu2 run)" : r == -EBUSY ? ": 15 programs are using it already" : r == -ENODEV ? ": ps5gpu.sys is not loaded" : "");
         return r;
      }
      ac_drm_device *dev = new ac_drm_device;
      dev->p5 = p5;
      dev->va_free[0][kVaLowStart] = kVaLowEnd - kVaLowStart;
      dev->va_free[1][kVaHighStart] = kVaHighEnd - kVaHighStart;
      provider_init(dev);
      fill_device_info(dev);
      g_dev = dev;
   }
   g_dev->refs++;
   *major_version = 3;
   *minor_version = 64;                   // what Linux 7.2 reports on the console
   *device_handle = g_dev;
   return 0;
}

void ac_drm_device_deinitialize(ac_drm_device *dev)
{
   std::lock_guard<std::mutex> g(g_lock);
   if (--dev->refs > 0) return;
   p5_close(dev->p5);
   for (auto &b : dev->bos) delete b.second;
   delete dev;
   g_dev = nullptr;
}

struct util_sync_provider *ac_drm_device_get_sync_provider(ac_drm_device *dev) { return &dev->provider.base; }
uintptr_t ac_drm_device_get_cookie(ac_drm_device *dev) { return (uintptr_t)dev; }
int ac_drm_device_get_fd(ac_drm_device *) { return -1; }

// ---- full-screen presentation by scan-out flip (called from Mesa's Windows window-system code, wsi_common_win32.cpp) ----
//
// A swapchain's frames end up as plain BGRA pixels in one of our buffers (Mesa copies each finished frame there with
// the GPU, then used to copy it again with the CPU into a bitmap for the window). When the window covers the whole
// screen and the frame has the screen's size and pitch, that buffer can be shown as it is: the display is pointed at
// it (libps5gpu, p5_scanout_flip) and nothing is copied. The desktop underneath keeps running unseen and comes back
// when the swapchain stops qualifying, is destroyed, or the program ends (libps5gpu does that for dead programs).
extern "C" {

// The screen as the display shows it: size and pitch in pixels. 0 on success. Cheap: asked of the hardware once a second.
int ps5gpu_scanout_size(uint32_t *width, uint32_t *height, uint32_t *pitch_pixels)
{
   static std::mutex m;
   static p5_scanout so;
   static uint64_t when;
   static bool have;
   std::lock_guard<std::mutex> g(m);
   uint64_t now = be::nowUs();
   if (!have || now - when > 1000000) {
      std::lock_guard<std::mutex> g2(g_lock);
      if (!g_dev || p5_scanout_query(g_dev->p5, &so)) return -1;
      have = true;
      when = now;
   }
   *width = so.width;
   *height = so.height;
   *pitch_pixels = so.pitch_pixels;
   return 0;
}

// Shows the picture that starts at `pixels` (a pointer into one of our mapped buffers). wait != 0: returns when the
// display has switched to it, i.e. after the next vertical blank (at most 100 ms). 0 on success.
int ps5gpu_scanout_present(void *pixels, int wait)
{
   ac_drm_device *dev = g_dev;
   if (!dev) return -1;
   uint32_t handle = 0;
   uint64_t offset = 0;
   {
      std::lock_guard<std::mutex> g(dev->lock);
      static uint32_t last;                              // the same few buffers come round again and again
      auto hit = [&](amdgpu_bo *b) { return b->cpu && (uint8_t *)pixels >= (uint8_t *)b->cpu && (uint8_t *)pixels < (uint8_t *)b->cpu + b->size; };
      auto it = dev->bos.find(last);
      if (it == dev->bos.end() || !hit(it->second)) {
         it = dev->bos.begin();
         while (it != dev->bos.end() && !hit(it->second)) ++it;
      }
      if (it == dev->bos.end()) return -1;
      last = handle = it->first;
      offset = (uint64_t)((uint8_t *)pixels - (uint8_t *)it->second->cpu);
   }
   int r = p5_scanout_flip(dev->p5, handle, offset);
   trace("scanout flip h%u +0x%llx -> %d\n", handle, (unsigned long long)offset, r);
   if (r) return r;
   if (wait) {
      uint64_t t0 = be::nowUs();
      while (p5_scanout_pending(dev->p5)) {
         uint64_t waited = be::nowUs() - t0;
         if (waited > 100000) break;
         sleep_a_bit(waited);
      }
   }
   return 0;
}

void ps5gpu_scanout_release(void)
{
   ac_drm_device *dev = g_dev;
   if (dev) p5_scanout_restore(dev->p5);
}

}

// ---- buffers ----

int ac_drm_bo_alloc(ac_drm_device *dev, struct amdgpu_bo_alloc_request *req, ac_drm_bo *bo)
{
   uint32_t handle = 0;
   int r = p5_bo_alloc(dev->p5, req->alloc_size, req->phys_alignment, &handle);
   if (r && p5env("PS5GPU_DEBUG")) {
      CONTEXT here;
      RtlCaptureContext(&here);
      char why[96];
      snprintf(why, sizeof why, "buffer of %llu bytes refused (%d)", (unsigned long long)req->alloc_size, r);
      print_chain(why, here);
   }
   if (r) return r;
   amdgpu_bo *b = new amdgpu_bo();
   b->handle = handle;
   b->preferred_heap = req->preferred_heap;
   b->flags = req->flags;
   b->phys_alignment = req->phys_alignment;
   p5_bo_query(dev->p5, handle, &b->size, &b->cpu);
   trace("alloc h%u %llu flags 0x%llx\n", handle, (unsigned long long)b->size, (unsigned long long)req->flags);
   std::lock_guard<std::mutex> g(dev->lock);
   dev->bos[handle] = b;
   dev->vram_used += b->size;
   bo->abo = b;
   return 0;
}

int ac_drm_bo_free(ac_drm_device *dev, ac_drm_bo bo)
{
   amdgpu_bo *b = bo.abo;
   {
      std::lock_guard<std::mutex> g(dev->lock);
      dev->bos.erase(b->handle);
      dev->vram_used -= b->size;
   }
   trace("free h%u (completed %llu)\n", b->handle, (unsigned long long)p5_completed(dev->p5));
   int r = p5_bo_free(dev->p5, b->handle);
   delete b;
   return r;
}

int ac_drm_bo_export(ac_drm_device *, ac_drm_bo bo, enum amdgpu_bo_handle_type type, uint32_t *shared_handle)
{
   if (type != amdgpu_bo_handle_type_kms && type != amdgpu_bo_handle_type_kms_noimport) return -ENOSYS;
   *shared_handle = bo.abo->handle;
   return 0;
}

int ac_drm_bo_import(ac_drm_device *, enum amdgpu_bo_handle_type, uint32_t, struct ac_drm_bo_import_result *) { return -ENOSYS; }
int ac_drm_create_bo_from_user_mem(ac_drm_device *, void *, uint64_t, ac_drm_bo *) { return -ENOSYS; }

int ac_drm_bo_cpu_map(ac_drm_device *, ac_drm_bo bo, void **cpu)
{
   *cpu = bo.abo->cpu;
   return 0;
}

int ac_drm_bo_cpu_unmap(ac_drm_device *, ac_drm_bo) { return 0; }

int ac_drm_bo_set_metadata(ac_drm_device *, uint32_t, struct amdgpu_bo_metadata *) { return 0; }

int ac_drm_bo_query_info(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_info *info)
{
   std::lock_guard<std::mutex> g(dev->lock);
   auto it = dev->bos.find(bo_handle);
   if (it == dev->bos.end()) return -ENOENT;
   memset(info, 0, sizeof *info);
   info->alloc_size = it->second->size;
   info->phys_alignment = it->second->phys_alignment;
   info->preferred_heap = it->second->preferred_heap;
   info->alloc_flags = it->second->flags;
   return 0;
}

int ac_drm_bo_wait_for_idle(ac_drm_device *dev, ac_drm_bo, uint64_t timeout_ns, bool *busy)
{
   // Buffers are not tracked per submission: idle means everything submitted so far has finished.
   p5_info pi;
   p5_get_info(dev->p5, &pi);
   *busy = p5_wait(dev->p5, pi.submitted, timeout_ns) != 0;
   return 0;
}

int ac_drm_bo_va_op_raw(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr, uint64_t flags, uint32_t ops)
{
   size = (size + 4095) & ~4095ull;
   trace("va op %u h%u off 0x%llx size 0x%llx at 0x%llx flags 0x%llx (completed %llu)\n", ops, bo_handle, (unsigned long long)offset, (unsigned long long)size,
         (unsigned long long)addr, (unsigned long long)flags, (unsigned long long)p5_completed(dev->p5));
   // Sparse "no page here" bindings are not supported yet; leaving the range unmapped is the closest thing.
   if (ops == AMDGPU_VA_OP_MAP && (bo_handle == 0 || (flags & AMDGPU_VM_PAGE_PRT))) ops = AMDGPU_VA_OP_CLEAR;
   if (ops == AMDGPU_VA_OP_REPLACE && (bo_handle == 0 || (flags & AMDGPU_VM_PAGE_PRT))) ops = AMDGPU_VA_OP_CLEAR;
   uint32_t f = (uint32_t)flags & (AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE);
   return p5_va_op(dev->p5, bo_handle, offset, size, addr, f, ops);
}

int ac_drm_bo_va_op(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr, uint64_t flags, uint32_t ops)
{
   if (ops != AMDGPU_VA_OP_UNMAP && ops != AMDGPU_VA_OP_CLEAR)
      flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE;
   return ac_drm_bo_va_op_raw(dev, bo_handle, offset, size, addr, flags, ops);
}

int ac_drm_bo_va_op_raw2(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr, uint64_t flags, uint32_t ops, uint32_t,
                         uint64_t, uint64_t, uint32_t)
{
   return ac_drm_bo_va_op_raw(dev, bo_handle, offset, size, addr, flags, ops);
}

// ---- address ranges ----

int ac_drm_va_range_alloc(ac_drm_device *dev, enum amdgpu_gpu_va_range, uint64_t size, uint64_t alignment, uint64_t base_required, uint64_t *va_base_allocated,
                          amdgpu_va_handle *va_range_handle, uint64_t flags)
{
   std::lock_guard<std::mutex> g(dev->lock);
   int range = (flags & AMDGPU_VA_RANGE_32_BIT) ? 0 : 1;
   if (alignment < 4096) alignment = 4096;
   size = (size + 4095) & ~4095ull;
   auto &m = dev->va_free[range];
   for (auto it = m.begin(); it != m.end(); ++it) {
      uint64_t start = base_required ? base_required : (it->first + alignment - 1) / alignment * alignment, end = it->first + it->second;
      if (start < it->first || start >= end || end - start < size) continue;
      uint64_t first = it->first;
      m.erase(it);
      if (start > first) m[first] = start - first;
      if (start + size < end) m[start + size] = end - (start + size);
      amdgpu_va *va = new amdgpu_va{dev, start, size, range};
      trace("range alloc 0x%llx + 0x%llx\n", (unsigned long long)start, (unsigned long long)size);
      *va_base_allocated = start;
      *va_range_handle = va;
      return 0;
   }
   return -ENOMEM;
}

int ac_drm_va_range_free(amdgpu_va_handle va)
{
   if (!va) return 0;
   trace("range free 0x%llx + 0x%llx\n", (unsigned long long)va->base, (unsigned long long)va->size);
   {
      std::lock_guard<std::mutex> g(va->dev->lock);
      va_release(va->dev, va->range, va->base, va->size);
   }
   delete va;
   return 0;
}

int ac_drm_va_range_query(ac_drm_device *, enum amdgpu_gpu_va_range, uint64_t *start, uint64_t *end)
{
   *start = kVaLowStart;
   *end = kVaHighEnd;
   return 0;
}

// ---- contexts and submission ----

int ac_drm_cs_ctx_create2(ac_drm_device *dev, uint32_t, uint32_t *ctx_id) { return p5_ctx_create(dev->p5, ctx_id); }
int ac_drm_cs_ctx_free(ac_drm_device *dev, uint32_t ctx_id) { return p5_ctx_free(dev->p5, ctx_id); }
int ac_drm_cs_ctx_stable_pstate(ac_drm_device *, uint32_t, uint32_t, uint32_t, uint32_t *out_flags)
{
   if (out_flags) *out_flags = 0;
   return 0;
}

int ac_drm_cs_query_reset_state2(ac_drm_device *, uint32_t, uint64_t *flags)
{
   *flags = 0;
   return 0;
}

int ac_drm_cs_query_fence_status(ac_drm_device *dev, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t fence_seq_no, uint64_t timeout_ns, uint64_t flags,
                                 uint32_t *expired)
{
   if (flags & AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE) {
      uint64_t now = (uint64_t)os_time_get_nano();
      timeout_ns = timeout_ns > now ? timeout_ns - now : 0;
   }
   *expired = p5_wait(dev->p5, fence_seq_no, timeout_ns) == 0;
   return 0;
}

void ac_drm_cs_chunk_fence_info_to_data(uint32_t bo_handle, uint64_t offset, struct drm_amdgpu_cs_chunk_data *data)
{
   data->fence_data.handle = bo_handle;
   data->fence_data.offset = offset * sizeof(uint64_t);
}

int ac_drm_cs_submit_raw2(ac_drm_device *dev, uint32_t ctx_id, uint32_t, int num_chunks, struct drm_amdgpu_cs_chunk *chunks, uint64_t *seq_no)
{
   std::vector<p5_ib> ibs;
   std::vector<uint32_t> out_syncs;
   std::vector<std::pair<uint32_t, uint64_t>> out_points;      // timeline sync objects to signal: handle, point
   uint32_t fence_handle = 0;
   uint64_t fence_offset = 0;
   const uint64_t forever = 60ull * 1000000000ull;

   for (int i = 0; i < num_chunks; ++i) {
      void *data = (void *)(uintptr_t)chunks[i].chunk_data;
      switch (chunks[i].chunk_id) {
      case AMDGPU_CHUNK_ID_IB: {
         struct drm_amdgpu_cs_chunk_ib *ib = (struct drm_amdgpu_cs_chunk_ib *)data;
         if (ib->ip_type != AMDGPU_HW_IP_GFX) {
            fprintf(stderr, "ps5gpu: submission for ip type %u; only the graphics ring exists\n", ib->ip_type);
            return -EINVAL;
         }
         p5_ib p = { ib->va_start, ib->ib_bytes, 0 };
         if (ib->flags & AMDGPU_IB_FLAG_CE) p.flags |= P5_IB_CE;
         if (ib->flags & AMDGPU_IB_FLAG_PREAMBLE) p.flags |= P5_IB_PREAMBLE;
         ibs.push_back(p);
         break;
      }
      case AMDGPU_CHUNK_ID_FENCE: {
         struct drm_amdgpu_cs_chunk_fence *f = (struct drm_amdgpu_cs_chunk_fence *)data;
         fence_handle = f->handle;
         fence_offset = f->offset;
         break;
      }
      case AMDGPU_CHUNK_ID_DEPENDENCIES:
      case AMDGPU_CHUNK_ID_SCHEDULED_DEPENDENCIES: {
         struct drm_amdgpu_cs_chunk_dep *deps = (struct drm_amdgpu_cs_chunk_dep *)data;
         unsigned n = chunks[i].length_dw * 4 / sizeof(*deps);
         for (unsigned k = 0; k < n; ++k)
            if (p5_wait(dev->p5, deps[k].handle, forever)) return -ETIME;
         break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_IN: {
         // The kernel would hold the job back until these are done. One ring, submissions in order: wait here.
         struct drm_amdgpu_cs_chunk_sem *sems = (struct drm_amdgpu_cs_chunk_sem *)data;
         unsigned n = chunks[i].length_dw * 4 / sizeof(*sems);
         for (unsigned k = 0; k < n; ++k) {
            uint64_t seq = 0;
            {
               std::lock_guard<std::mutex> g(dev->lock);
               SyncObj *s = sync_get(dev, sems[k].handle);
               if (!s || (!s->signaled && !s->has_fence)) return -EINVAL;
               if (s->signaled) continue;
               seq = s->seq;
            }
            if (p5_wait(dev->p5, seq, forever)) return -ETIME;
         }
         break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_OUT: {
         struct drm_amdgpu_cs_chunk_sem *sems = (struct drm_amdgpu_cs_chunk_sem *)data;
         unsigned n = chunks[i].length_dw * 4 / sizeof(*sems);
         for (unsigned k = 0; k < n; ++k) out_syncs.push_back(sems[k].handle);
         break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT: {
         // As SYNCOBJ_IN, for a point of a timeline.
         struct drm_amdgpu_cs_chunk_syncobj *sems = (struct drm_amdgpu_cs_chunk_syncobj *)data;
         unsigned n = chunks[i].length_dw * 4 / sizeof(*sems);
         for (unsigned k = 0; k < n; ++k) {
            Fence f;
            {
               std::lock_guard<std::mutex> g(dev->lock);
               SyncObj *s = sync_get(dev, sems[k].handle);
               if (!s || !sync_fence(*s, sems[k].point, &f)) return -EINVAL;
            }
            if (!f.signaled && p5_wait(dev->p5, f.seq, forever)) return -ETIME;
         }
         break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL: {
         struct drm_amdgpu_cs_chunk_syncobj *sems = (struct drm_amdgpu_cs_chunk_syncobj *)data;
         unsigned n = chunks[i].length_dw * 4 / sizeof(*sems);
         for (unsigned k = 0; k < n; ++k) {
            if (sems[k].point == 0) out_syncs.push_back(sems[k].handle);
            else out_points.emplace_back(sems[k].handle, sems[k].point);
         }
         break;
      }
      case AMDGPU_CHUNK_ID_BO_HANDLES:
         break;
      default:
         fprintf(stderr, "ps5gpu: submission chunk 0x%x is not supported\n", chunks[i].chunk_id);
         return -EINVAL;
      }
   }
   if (ibs.empty()) return -EINVAL;
   // PS5GPU_DUMP=1: every submission; PS5GPU_DUMP=a-b: submissions number a to b only (counted from 0).
   static unsigned dump_n, dump_first, dump_last = ~0u;
   static const char *dump_env = p5env("PS5GPU_DUMP");
   if (dump_env && dump_n == 0 && strchr(dump_env, '-')) sscanf(dump_env, "%u-%u", &dump_first, &dump_last);
   const unsigned dump_this = dump_n++;
   if (dump_env && dump_this >= dump_first && dump_this <= dump_last) {
      // What Mesa is about to hand the GPU, decoded with Mesa's own parser.
      const unsigned n = dump_this;
      FILE *out = g_trace ? g_trace : stderr;      // with PS5GPU_TRACE the dump goes into the trace file
      for (size_t i = 0; i < ibs.size(); ++i) {
         // A buffer can end with a jump to its continuation (INDIRECT_BUFFER with the chain bit); follow those.
         uint64_t va = ibs[i].va;
         uint32_t dws = ibs[i].bytes / 4;
         for (unsigned part = 0; part < 256; ++part) {
            uint32_t *words = (uint32_t *)p5_va_cpu(dev->p5, va);
            fprintf(out, "ps5gpu: submission %u, buffer %u of %u, part %u: va 0x%llx, %u dwords, flags 0x%x%s\n", n, (unsigned)i, (unsigned)ibs.size(), part,
                    (unsigned long long)va, dws, ibs[i].flags, words ? "" : " (NOT MAPPED)");
            if (!words) break;
            struct ac_ib_parser parser = {};
            parser.f = out;
            parser.ib = words;
            parser.num_dw = dws;
            parser.gfx_level = GFX10;
            parser.family = CHIP_NAVI10;
            parser.ip_type = AMD_IP_GFX;
            ac_parse_ib(&parser, "ps5gpu");
            if (dws < 4 || words[dws - 4] != 0xc0023f00 || !(words[dws - 1] & (1u << 20))) break;
            va = words[dws - 3] | ((uint64_t)words[dws - 2] << 32);
            dws = words[dws - 1] & 0xFFFFF;
         }
      }
      // PS5GPU_PEEK=<gpu address>: the 16 dwords found there at this moment (a descriptor, a constant block).
      if (const char *peek = p5env("PS5GPU_PEEK")) {
         uint64_t va = strtoull(peek, nullptr, 0);
         uint32_t *w = (uint32_t *)p5_va_cpu(dev->p5, va);
         fprintf(out, "ps5gpu: peek 0x%llx:", (unsigned long long)va);
         if (!w) fprintf(out, " not mapped");
         else for (int k = 0; k < 16; ++k) fprintf(out, " %08x", w[k]);
         fprintf(out, "\n");
      }
      fflush(out);
   }
   uint64_t seq = 0;
   int r = p5_submit(dev->p5, ctx_id, ibs.data(), (uint32_t)ibs.size(), fence_handle, fence_offset, &seq);
   if (g_trace) {
      trace("submit ctx %u seq %llu r %d completed %llu ibs %u first 0x%llx + %u\n", ctx_id, (unsigned long long)seq, r, (unsigned long long)p5_completed(dev->p5),
            (unsigned)ibs.size(), (unsigned long long)ibs[0].va, ibs[0].bytes);
      std::lock_guard<std::mutex> g(g_trace_lock);
      fflush(g_trace);
   }
   if (r) return r;
   {
      std::lock_guard<std::mutex> g(dev->lock);
      for (uint32_t h : out_syncs) {
         SyncObj *s = sync_get(dev, h);
         if (!s) continue;
         s->signaled = false;
         s->has_fence = true;
         s->seq = seq;
      }
      for (const auto &hp : out_points) {
         SyncObj *s = sync_get(dev, hp.first);
         if (!s) continue;
         Fence f;
         f.has_fence = true;
         f.seq = seq;
         sync_add_point(dev, *s, hp.second, f);
      }
   }
   if (seq_no) *seq_no = seq;
   return 0;
}

// ---- sync objects through the ac_drm interface (the provider above is the same thing) ----

int ac_drm_cs_create_syncobj2(ac_drm_device *dev, uint32_t flags, uint32_t *handle) { return sync_create(dev, flags, handle); }
int ac_drm_cs_destroy_syncobj(ac_drm_device *dev, uint32_t handle) { return sync_destroy(dev, handle); }
int ac_drm_cs_syncobj_wait(ac_drm_device *dev, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   return sync_wait(dev, handles, num_handles, timeout_nsec, flags, first_signaled);
}
int ac_drm_cs_syncobj_query2(ac_drm_device *dev, uint32_t *handles, uint64_t *points, unsigned count, uint32_t flags)
{
   return sync_query(dev, handles, points, count, flags);
}
int ac_drm_cs_import_syncobj(ac_drm_device *, int, uint32_t *) { return -ENOSYS; }
int ac_drm_cs_syncobj_export_sync_file(ac_drm_device *, uint32_t, int *) { return -ENOSYS; }
int ac_drm_cs_syncobj_import_sync_file(ac_drm_device *, uint32_t, int) { return -ENOSYS; }
int ac_drm_cs_syncobj_export_sync_file2(ac_drm_device *, uint32_t, uint64_t, uint32_t, int *) { return -ENOSYS; }
int ac_drm_cs_syncobj_transfer(ac_drm_device *dev, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point, uint32_t)
{
   return sync_transfer(dev, dst_handle, src_handle, dst_point, src_point);
}
// The LUID Windows gave our display adapter (the one with a display source; Microsoft's Basic Render Driver has none).
// Vulkan reports it as deviceLUID so that DXVK/vkd3d adapters and Windows' own DXGI adapters are the same object to a
// program: without it DXVK invented a LUID, could not tie the monitor to its adapter ("monitors not associated"), and
// dgVoodoo2 could not find the screen of the device it had created (SetCooperativeLevel failed, SCP:CB 2026-11-21).
int ac_ps5_adapter_luid(uint8_t luid[8])
{
   struct KmtAdapterInfo { UINT hAdapter; LUID AdapterLuid; ULONG NumOfSources; BOOL bPrecisePresentRegionsPreferred; };
   struct KmtEnumAdapters2 { ULONG NumAdapters; KmtAdapterInfo *pAdapters; };
   struct KmtCloseAdapter { UINT hAdapter; };
   typedef LONG (APIENTRY *EnumFn)(KmtEnumAdapters2 *);
   typedef LONG (APIENTRY *CloseFn)(const KmtCloseAdapter *);
   static int state;                       // 0 not looked yet, 1 found, -1 none
   static uint8_t cached[8];
   if (state == 0) {
      state = -1;
      HMODULE gdi = LoadLibraryA("gdi32.dll");
      EnumFn enumAdapters = gdi ? (EnumFn)GetProcAddress(gdi, "D3DKMTEnumAdapters2") : nullptr;
      CloseFn closeAdapter = gdi ? (CloseFn)GetProcAddress(gdi, "D3DKMTCloseAdapter") : nullptr;
      KmtEnumAdapters2 e = {};
      if (enumAdapters && enumAdapters(&e) >= 0 && e.NumAdapters) {
         std::vector<KmtAdapterInfo> list(e.NumAdapters);
         e.pAdapters = list.data();
         if (enumAdapters(&e) >= 0) {
            for (ULONG i = 0; i < e.NumAdapters; ++i) {
               if (state < 0 && list[i].NumOfSources > 0) {
                  memcpy(cached, &list[i].AdapterLuid, 8);
                  state = 1;
               }
               if (closeAdapter) {
                  KmtCloseAdapter c = { list[i].hAdapter };
                  closeAdapter(&c);
               }
            }
         }
      }
   }
   if (state > 0) memcpy(luid, cached, 8);
   return state > 0 ? 0 : -1;
}

int ac_ps5_syncobj_accumulate(ac_drm_device *dev, uint32_t dst, const uint32_t *srcs, const uint64_t *points, uint32_t count)
{
   std::lock_guard<std::mutex> g(dev->lock);
   SyncObj *d = sync_get(dev, dst);
   if (!d) return -EINVAL;
   // Everything runs on one ring in submission order, so "all of them done" is "the newest of them done".
   uint64_t newest = d->has_fence ? d->seq : 0;
   bool any = d->has_fence;
   for (uint32_t i = 0; i < count; ++i) {
      SyncObj *s = sync_get(dev, srcs[i]);
      Fence f;
      if (!s || !sync_fence(*s, points ? points[i] : 0, &f)) return -EINVAL;
      if (f.has_fence && !f.signaled) {
         any = true;
         if (f.seq > newest) newest = f.seq;
      }
   }
   d->has_fence = any;
   d->signaled = !any;
   d->seq = newest;
   return 0;
}

int ac_drm_cs_syncobj_timeline_wait(ac_drm_device *dev, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout_nsec, unsigned flags,
                                    uint32_t *first_signaled)
{
   return sync_wait(dev, handles, count, timeout_nsec, flags, first_signaled, points);
}

// ---- queries ----

int ac_drm_query_info(ac_drm_device *dev, unsigned info_id, unsigned size, void *value)
{
   p5_info pi;
   p5_get_info(dev->p5, &pi);
   switch (info_id) {
   case AMDGPU_INFO_DEV_INFO:
      memset(value, 0, size);
      memcpy(value, &dev->info, size < sizeof dev->info ? size : sizeof dev->info);
      return 0;
   case AMDGPU_INFO_MEMORY: {
      struct drm_amdgpu_memory_info m = {};
      m.vram.total_heap_size = m.vram.usable_heap_size = m.vram.max_allocation = pi.heap_bytes;
      m.vram.heap_usage = pi.heap_bytes - pi.heap_free;
      m.cpu_accessible_vram = m.vram;
      // Nothing is ever placed in system memory; a nominal size keeps Mesa's heap arithmetic happy.
      m.gtt.total_heap_size = m.gtt.usable_heap_size = m.gtt.max_allocation = 256ull << 20;
      memset(value, 0, size);
      memcpy(value, &m, size < sizeof m ? size : sizeof m);
      return 0;
   }
   case AMDGPU_INFO_VRAM_USAGE:
   case AMDGPU_INFO_VIS_VRAM_USAGE: {
      uint64_t v = pi.heap_bytes - pi.heap_free;
      memcpy(value, &v, size < 8 ? size : 8);
      return 0;
   }
   case AMDGPU_INFO_GTT_USAGE: {
      uint64_t v = 0;
      memcpy(value, &v, size < 8 ? size : 8);
      return 0;
   }
   default:
      return -EINVAL;
   }
}

int ac_drm_read_mm_registers(ac_drm_device *dev, unsigned dword_offset, unsigned count, uint32_t, uint32_t, uint32_t *values)
{
   for (unsigned i = 0; i < count; ++i) {
      if (dword_offset + i == GC(mmGB_ADDR_CONFIG)) {
         values[i] = dev->gb_addr_config;
         continue;
      }
      if (!be::rd(dword_offset + i, values[i])) return -EINVAL;
   }
   return 0;
}

int ac_drm_query_hw_ip_count(ac_drm_device *, unsigned type, uint32_t *count)
{
   *count = type == AMDGPU_HW_IP_GFX ? 1 : 0;
   return 0;
}

int ac_drm_query_hw_ip_info(ac_drm_device *, unsigned type, unsigned, struct drm_amdgpu_info_hw_ip *info)
{
   memset(info, 0, sizeof *info);
   if (type != AMDGPU_HW_IP_GFX) return -EINVAL;      // the compute and SDMA rings are not running
   info->hw_ip_version_major = 10;
   info->hw_ip_version_minor = 0;         // Linux: 10.0, discovery version 10.1.3
   info->ib_start_alignment = 32;
   info->ib_size_alignment = 32;
   info->available_rings = 1;
   info->ip_discovery_version = (10 << 16) | (1 << 8) | 3;
   return 0;
}

int ac_drm_query_firmware_version(ac_drm_device *, unsigned fw_type, unsigned, unsigned, uint32_t *version, uint32_t *feature)
{
   *version = 0;
   *feature = 0;
   switch (fw_type) {      // amdgpu_firmware_info on the console
   case AMDGPU_INFO_FW_GFX_ME: *version = 0x63; *feature = 32; break;
   case AMDGPU_INFO_FW_GFX_PFP: *version = 0x94; *feature = 32; break;
   case AMDGPU_INFO_FW_GFX_CE: *version = 0x25; *feature = 32; break;
   case AMDGPU_INFO_FW_GFX_MEC: *version = 0x90; *feature = 32; break;
   case AMDGPU_INFO_FW_GFX_RLC: *version = 0x0d; break;
   case AMDGPU_INFO_FW_SDMA: *version = 0x34; *feature = 50; break;
   default: break;
   }
   return 0;
}

int ac_drm_query_uq_fw_area_info(ac_drm_device *, unsigned, unsigned, struct drm_amdgpu_info_uq_metadata *) { return -EINVAL; }

int ac_drm_query_gpu_info(ac_drm_device *dev, struct amdgpu_gpu_info *info)
{
   const struct drm_amdgpu_info_device &d = dev->info;
   memset(info, 0, sizeof *info);
   info->asic_id = d.device_id;
   info->chip_rev = d.chip_rev;
   info->chip_external_rev = d.external_rev;
   info->family_id = d.family;
   info->ids_flags = d.ids_flags;
   info->max_engine_clk = d.max_engine_clock;
   info->max_memory_clk = d.max_memory_clock;
   info->num_shader_engines = d.num_shader_engines;
   info->num_shader_arrays_per_engine = d.num_shader_arrays_per_engine;
   info->num_hw_gfx_contexts = d.num_hw_gfx_contexts;
   info->rb_pipes = d.num_rb_pipes;
   info->enabled_rb_pipes_mask = d.enabled_rb_pipes_mask;
   info->gpu_counter_freq = d.gpu_counter_freq;
   info->gb_addr_cfg = dev->gb_addr_config;
   info->cu_active_number = d.cu_active_number;
   info->cu_ao_mask = d.cu_ao_mask;
   memcpy(info->cu_bitmap, d.cu_bitmap, sizeof info->cu_bitmap);
   info->vram_type = d.vram_type;
   info->vram_bit_width = d.vram_bit_width;
   info->ce_ram_size = d.ce_ram_size;
   info->pci_rev_id = d.pci_rev;
   return 0;
}

int ac_drm_query_heap_info(ac_drm_device *dev, uint32_t heap, uint32_t, struct amdgpu_heap_info *info)
{
   p5_info pi;
   p5_get_info(dev->p5, &pi);
   memset(info, 0, sizeof *info);
   if (heap == AMDGPU_GEM_DOMAIN_VRAM) {
      info->heap_size = info->max_allocation = pi.heap_bytes;
      info->heap_usage = pi.heap_bytes - pi.heap_free;
   } else if (heap == AMDGPU_GEM_DOMAIN_GTT) {
      info->heap_size = info->max_allocation = 256ull << 20;
   } else {
      return -EINVAL;
   }
   return 0;
}

int ac_drm_query_sensor_info(ac_drm_device *, unsigned, unsigned, void *) { return -EINVAL; }
int ac_drm_query_video_caps_info(ac_drm_device *, unsigned, unsigned, void *) { return -EINVAL; }
int ac_drm_query_gpuvm_fault_info(ac_drm_device *, unsigned, void *) { return -EINVAL; }
int ac_drm_vm_reserve_vmid(ac_drm_device *, uint32_t) { return 0; }
int ac_drm_vm_unreserve_vmid(ac_drm_device *, uint32_t) { return 0; }
const char *ac_drm_get_marketing_name(ac_drm_device *) { return "AMD Oberon (PlayStation 5)"; }

int ac_drm_query_sw_info(ac_drm_device *, enum amdgpu_sw_info info, void *value)
{
   // The 32-bit address range really is below 4 GiB here.
   if (info == amdgpu_sw_info_address32_hi) {
      *(uint32_t *)value = 0;
      return 0;
   }
   // The address bit Mesa's shaders clear before scalar buffer loads. It must be a bit no ordinary allocation has
   // set. Answering 0 here (until 2026-11-20) made the shaders clear bit 32 instead (the code shifts by "bit - 32"),
   // so a storage buffer above 4 GiB was read 4 GiB too low: a GPU fault and a hang in the first game that used one
   // from a shader that way (DXVK's frame-rate display).
   if (info == amdgpu_sw_info_address_prt_wa_control_bit) {
      *(uint8_t *)value = kPrtControlBit;
      return 0;
   }
   return -EINVAL;
}

int ac_drm_create_userqueue(ac_drm_device *, uint32_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, void *, uint32_t, uint32_t *) { return -ENOSYS; }
int ac_drm_free_userqueue(ac_drm_device *, uint32_t) { return -ENOSYS; }
int ac_drm_userq_signal(ac_drm_device *, struct drm_amdgpu_userq_signal *) { return -ENOSYS; }
int ac_drm_userq_wait(ac_drm_device *, struct drm_amdgpu_userq_wait *) { return -ENOSYS; }

int ac_drm_query_pci_bus_info(ac_drm_device *, struct radeon_info *info)
{
   info->pci.domain = 0;
   info->pci.bus = 0x20;
   info->pci.dev = 0;
   info->pci.func = 0;
   info->pci.valid = true;
   return 0;
}

void ac_drm_query_has_vm_always_valid(ac_drm_device *, struct radeon_info *info) { info->has_vm_always_valid = true; }

}   // extern "C"
