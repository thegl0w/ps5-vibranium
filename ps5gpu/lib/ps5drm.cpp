// SPDX-License-Identifier: MIT
// libps5gpu: see ps5drm.h. Follows Linux's amdgpu where it matters (page-table format, submission order, fences).
#include "ps5drm.h"
#include <cstdlib>
#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include "../tool/backend.h"
#include "../tool/regs.h"
#include "../tool/layout.h"

using namespace p5layout;

namespace
{
    const uint64_t kPage = 0x1000;
    const uint64_t kPteValid = 1, kPteExecutable = 1 << 4, kPteReadable = 1 << 5, kPteWriteable = 1 << 6;
    const uint64_t kPteAddr = 0x0000FFFFFFFFF000ull;
    const uint64_t kVaStart = 0x200000ull, kVaEnd = 0x800000000000ull;

    // ---- state shared by every program that uses the GPU ----
    // It lives in our VRAM area (so every process, 32- or 64-bit, sees the same bytes) and is only touched while the
    // backend's lock (a named mutex) is held. No pointers in it. One client = one p5_open = one GPU address space
    // (VMID 1-15). Plain data and fixed arrays, so the same code can move into the kernel driver later.
    const uint64_t kStateOff = 0xA00000;                 // area + 10 MiB .. 16 MiB is free (layout.h)
    const uint32_t kStateMagic = 0x33533550;             // "P5S3"
    const uint32_t kMaxBos = 16384, kMaxCtx = 256, kPoolPages = (uint32_t)(kPtPoolBytes / kPage), kNone = 0xFFFFFFFF;
    enum : uint32_t { kFree = 0, kLive = 1, kZombie = 2, kPending = 2 };

    struct Range { uint64_t off, bytes; };
    struct Bo {
        uint64_t off, bytes, freeSeq;                    // VRAM offset; freeSeq: given back once this sequence number is done
        uint32_t next;                                   // free-slot list or the pending-free queue
        uint8_t owner, state, pad[2];                    // state: kFree, kLive, kPending
    };
    struct Client {
        uint32_t state, pid, root, tlbDirty, bos, ptPages;   // root: pool page of its top-level page table
        uint64_t lastSeq;
    };
    struct Ctx { uint32_t id, owner, preamble, pad; };
    struct State {
        uint32_t magic, live, nextCtx, lastCtx, nfree, boFree, pendHead, pendTail, poolHint, hwValid, rbMask, cuCount, cu[4];
        uint64_t freeBytes, pendingBytes, rptrKnown;
        Client cl[16];
        Ctx ctx[kMaxCtx];
        uint8_t poolOwner[kPoolPages];
        Range freeList[kMaxBos + 8];
        Bo bo[kMaxBos];
    };
    static_assert(sizeof(State) <= kAreaBytes - kStateOff, "state does not fit behind the area");
    static_assert(kUserOff + kUserBytes <= kStateOff, "state overlaps the step-8 memory");

    // Which Windows start the client table belongs to. A warm Restart keeps VRAM, so the table of the Windows before is
    // still there and valid-looking; its pids mean nothing now, and Windows starts its programs in the same order every
    // time, so the same numbers are often running again (2026-11-25: the old compositor's pid was this start's dwm ->
    // `ps5gpu2 run` refused, the watchdog's recovery ended that program). `run` writes this record with the session id
    // (be::sessionId: volatile registry, gone after any restart). No record = a tool from before it: trust the pids.
    const uint64_t kRunRecOff = 0xFF0000;
    const uint32_t kRunRecMagic = 0x52523550;            // "P5RR"
    struct RunRec { uint32_t magic, pad; uint64_t session; };
    static_assert(kStateOff + sizeof(State) <= kRunRecOff, "state overlaps the run record");
    static_assert(kRunRecOff + sizeof(RunRec) <= kAreaBytes, "run record outside the area");

    // false: the table (clients, their memory) is from an earlier Windows start; every client in it is gone.
    bool tableCurrent(const uint8_t* area)
    {
        const RunRec* r = (const RunRec*)(area + kRunRecOff);
        if (r->magic != kRunRecMagic) return true;
        uint64_t id = be::sessionId(false);
        return id != 0 && id == r->session;
    }

    struct Lock {
        Lock() { be::lock(); }
        ~Lock() { be::unlock(); }
    };
}

struct p5_dev {
    bool lazy = false;                    // buffers are mapped one by one (32-bit clients)
    uint32_t vmid = 0, pid = 0;           // pid: what be::pid() said when this client attached
    uint64_t carvePhys = 0, mcBase = 0, vramBytes = 0, areaOff = 0, heapEnd = 0;
    uint8_t *area = nullptr, *heap = nullptr, *pool = nullptr;
    Shared* shared = nullptr;
    State* st = nullptr;
    volatile uint64_t* fence = nullptr;
    uint32_t* ring = nullptr;
    uint32_t ringDw = (uint32_t)(kRingBytes / 4);
    std::mutex lock;                      // cpu
    std::map<uint32_t, uint8_t*> cpu;     // lazy mode: handle -> this process's mapping of that buffer
    bool failed = false;
    uint32_t scanPipes = 0;               // set while this client has flipped the screen to one of its buffers
    uint64_t scanPhys = 0;
    uint64_t txWptr = 0;                  // where the submission being put together will end (ringWrite / ringCommit)

    uint64_t areaMc() const { return mcBase + areaOff; }
    uint64_t poolPhys() const { return carvePhys + areaOff + kAreaBytes; }
    Client& me() { return st->cl[vmid]; }
};

namespace
{
    uint32_t R(p5_dev* d, uint32_t reg)
    {
        uint32_t v = 0;
        if (!be::rd(reg, v)) {
            d->failed = true;
            return 0xFFFFFFFF;
        }
        return v;
    }

    uint64_t ringRptr64(p5_dev* d)
    {
        uint32_t reg = R(d, GC(mmCP_RB0_RPTR)) & (d->ringDw - 1);
        uint64_t w = d->shared->wptr;
        return w - ((w - reg) & (d->ringDw - 1));
    }

    // A submission is written behind the shared write pointer first (the GPU does not look there) and only then made
    // known, in one go (ringCommit). A program killed half-way through leaves nothing behind: the next submission
    // simply overwrites what it wrote. Before 2026-11-21 the shared pointer moved with every word, and a `taskkill`
    // at the wrong moment left half a submission for the next program's doorbell to hand to the command processor,
    // which halted on it (CP_ME_CNTL 0x15000000, everything stuck until `ps5gpu2 run`).
    void ringWrite(p5_dev* d, uint32_t v)
    {
        d->ring[d->txWptr & (d->ringDw - 1)] = v;
        ++d->txWptr;
    }

    // Is there room for `dwords` (plus padding)? Padding can add up to 255 dwords; one more block stays free so the
    // pointers never meet. Never waits: the caller lets go of the lock and tries again.
    bool ringRoom(p5_dev* d, uint32_t dwords)
    {
        if (d->shared->wptr - d->st->rptrKnown + dwords + 512 <= d->ringDw) return true;
        d->st->rptrKnown = ringRptr64(d);
        return d->shared->wptr - d->st->rptrKnown + dwords + 512 <= d->ringDw;
    }

    // Order matters for a program that dies in here: what it "has submitted" (lastSeq, which keeps its memory alive
    // until the GPU is past it) and which context ran last are recorded before the GPU can start on the work; the
    // sequence number and the write pointer become shared just before the doorbell. Whatever point it dies at, the
    // others see either nothing of this submission or all of it.
    void ringCommit(p5_dev* d, uint64_t seq, uint32_t ctx)
    {
        while (d->txWptr & 0xFF) ringWrite(d, PACKET3(PACKET3_NOP, 0x3FFF));
        be::flushWrites();
        d->me().lastSeq = seq;
        d->st->lastCtx = ctx;
        d->shared->seq = seq;
        d->shared->wptr = d->txWptr;
        *(volatile uint64_t*)(d->area + kWptrOff) = d->shared->wptr;
        be::flushWrites();
        if (!be::doorbell64(kDoorbellIndex * 4, d->shared->wptr)) d->failed = true;
    }

    // gfx_v10_0_ring_emit_wreg on the gfx ring.
    void emitWreg(p5_dev* d, uint32_t reg, uint32_t value)
    {
        ringWrite(d, PACKET3(PACKET3_WRITE_DATA, 3));
        ringWrite(d, WRITE_DATA_ENGINE_SEL(1) | WR_CONFIRM);
        ringWrite(d, reg);
        ringWrite(d, 0);
        ringWrite(d, value);
    }

    // gmc_v10_0_emit_flush_gpu_tlb + PFP_SYNC_ME (gfx_v10_0_ring_emit_vm_flush), with the write-then-wait fallback of
    // amdgpu_ring_emit_reg_write_reg_wait_helper. The gfx ring's invalidation engine is 0. Also (re)writes the
    // page-table base of the client's VMID, which is how a new client's address space is installed: through the ring,
    // in order with everyone's work, never by a register write while the GPU is busy.
    void emitTlbFlush(p5_dev* d)
    {
        uint32_t vmid = d->vmid;
        uint64_t root = (d->poolPhys() + (uint64_t)d->me().root * kPage) | kPteValid;
        uint32_t req = ((1u << vmid) << GCVM_INVALIDATE_ENG0_REQ__PER_VMID_INVALIDATE_REQ__SHIFT) | GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PTES_MASK |
                       GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE0_MASK | GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE1_MASK |
                       GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE2_MASK | GCVM_INVALIDATE_ENG0_REQ__INVALIDATE_L1_PTES_MASK;
        emitWreg(d, GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32) + 2 * vmid, (uint32_t)root);
        emitWreg(d, GC(mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32) + 2 * vmid, (uint32_t)(root >> 32));
        emitWreg(d, GC(mmGCVM_INVALIDATE_ENG0_REQ), req);
        ringWrite(d, PACKET3(PACKET3_WAIT_REG_MEM, 5));
        ringWrite(d, WAIT_REG_MEM_MEM_SPACE(0) | WAIT_REG_MEM_OPERATION(0) | WAIT_REG_MEM_FUNCTION(3) | WAIT_REG_MEM_ENGINE(0));
        ringWrite(d, GC(mmGCVM_INVALIDATE_ENG0_ACK));
        ringWrite(d, 0);
        ringWrite(d, 1u << vmid);
        ringWrite(d, 1u << vmid);
        ringWrite(d, 0x20);
        ringWrite(d, PACKET3(PACKET3_PFP_SYNC_ME, 0));
        ringWrite(d, 0);
    }

    // gfx_v10_0_ring_emit_fence, 64-bit, no interrupt. `mc` is a VMID 0 address.
    // gfx_v10_0_emit_mem_sync: write back and invalidate the GPU's caches (GL2, GLM, GL1, GLV, GLK, GLI).
    void emitMemSync(p5_dev* d)
    {
        const uint32_t gcr = PACKET3_ACQUIRE_MEM_GCR_CNTL_GL2_INV(1) | PACKET3_ACQUIRE_MEM_GCR_CNTL_GL2_WB(1) | PACKET3_ACQUIRE_MEM_GCR_CNTL_GLM_INV(1) |
                             PACKET3_ACQUIRE_MEM_GCR_CNTL_GLM_WB(1) | PACKET3_ACQUIRE_MEM_GCR_CNTL_GL1_INV(1) | PACKET3_ACQUIRE_MEM_GCR_CNTL_GLV_INV(1) |
                             PACKET3_ACQUIRE_MEM_GCR_CNTL_GLK_INV(1) | PACKET3_ACQUIRE_MEM_GCR_CNTL_GLI_INV(1);
        ringWrite(d, PACKET3(PACKET3_ACQUIRE_MEM, 6));
        ringWrite(d, 0);
        ringWrite(d, 0xFFFFFFFFu);
        ringWrite(d, 0xFFFFFFu);
        ringWrite(d, 0);
        ringWrite(d, 0);
        ringWrite(d, 0x0000000Au);
        ringWrite(d, gcr);
    }

    void emitFence(p5_dev* d, uint64_t mc, uint64_t seq)
    {
        ringWrite(d, PACKET3(PACKET3_RELEASE_MEM, 6));
        ringWrite(d, PACKET3_RELEASE_MEM_GCR_SEQ | PACKET3_RELEASE_MEM_GCR_GL2_WB | PACKET3_RELEASE_MEM_GCR_GLM_INV | PACKET3_RELEASE_MEM_GCR_GLM_WB |
                     PACKET3_RELEASE_MEM_CACHE_POLICY(3) | PACKET3_RELEASE_MEM_EVENT_TYPE(0x14 /* CACHE_FLUSH_AND_INV_TS_EVENT */) | PACKET3_RELEASE_MEM_EVENT_INDEX(5));
        // PS5GPU_FENCE_IRQ=1: also raise the end-of-pipe interrupt (for testing the display driver's interrupt ring).
        static const bool fenceIrq = getenv("PS5GPU_FENCE_IRQ") != nullptr;
        ringWrite(d, PACKET3_RELEASE_MEM_DATA_SEL(2) | PACKET3_RELEASE_MEM_INT_SEL(fenceIrq ? 2 : 0));
        ringWrite(d, (uint32_t)mc);
        ringWrite(d, (uint32_t)(mc >> 32));
        ringWrite(d, (uint32_t)seq);
        ringWrite(d, (uint32_t)(seq >> 32));
        ringWrite(d, 0);
    }

    // ---- page-table pool: 4 KiB pages, each owned by one client ----

    uint32_t poolAlloc(p5_dev* d)
    {
        State* s = d->st;
        for (uint32_t n = 0; n < kPoolPages; ++n) {
            uint32_t i = (s->poolHint + n) % kPoolPages;
            if (s->poolOwner[i]) continue;
            s->poolOwner[i] = (uint8_t)d->vmid;
            s->poolHint = i + 1;
            ++d->me().ptPages;
            d->me().tlbDirty = 1;                        // a new table (maybe another client's before): flush before use
            memset(d->pool + (uint64_t)i * kPage, 0, kPage);
            return i;
        }
        return kNone;
    }

    // The last-level table (512 entries) that holds the entry for `va`, making the tables on the way if asked.
    // On a miss without `create`, *missShift is the level that was absent (its entry spans 1 << shift bytes).
    uint64_t* leafTable(p5_dev* d, uint64_t va, bool create, int* missShift)
    {
        uint64_t* table = (uint64_t*)(d->pool + (uint64_t)d->me().root * kPage);
        for (int shift = 39; shift > 12; shift -= 9) {
            uint64_t& e = table[(va >> shift) & 511];
            uint64_t v = e;
            if (!(v & kPteValid)) {
                if (missShift) *missShift = shift;
                if (!create) return nullptr;
                uint32_t page = poolAlloc(d);
                if (page == kNone) return nullptr;
                v = (d->poolPhys() + (uint64_t)page * kPage) | kPteValid;
                e = v;
            }
            uint64_t off = (v & kPteAddr) - d->poolPhys();
            if (off >= kPtPoolBytes || d->st->poolOwner[off / kPage] != d->vmid) {      // not one of ours: the state is damaged
                d->failed = true;
                if (missShift) *missShift = shift;
                return nullptr;
            }
            table = (uint64_t*)(d->pool + off);
        }
        return table;
    }

    // ---- buffer heap: free ranges as a sorted array ----

    void freeRange(p5_dev* d, uint64_t off, uint64_t bytes)
    {
        State* s = d->st;
        uint32_t i = 0;
        while (i < s->nfree && s->freeList[i].off < off) ++i;
        s->freeBytes += bytes;
        bool prev = i > 0 && s->freeList[i - 1].off + s->freeList[i - 1].bytes == off;
        bool next = i < s->nfree && off + bytes == s->freeList[i].off;
        if (prev && next) {
            s->freeList[i - 1].bytes += bytes + s->freeList[i].bytes;
            memmove(&s->freeList[i], &s->freeList[i + 1], (s->nfree - i - 1) * sizeof(Range));
            --s->nfree;
        } else if (prev) {
            s->freeList[i - 1].bytes += bytes;
        } else if (next) {
            s->freeList[i].off = off;
            s->freeList[i].bytes += bytes;
        } else {
            memmove(&s->freeList[i + 1], &s->freeList[i], (s->nfree - i) * sizeof(Range));
            s->freeList[i].off = off;
            s->freeList[i].bytes = bytes;
            ++s->nfree;
        }
    }

    bool takeRange(p5_dev* d, uint64_t bytes, uint64_t alignment, uint64_t* out)
    {
        State* s = d->st;
        for (uint32_t i = 0; i < s->nfree; ++i) {
            uint64_t first = s->freeList[i].off, end = first + s->freeList[i].bytes;
            uint64_t start = (first + alignment - 1) & ~(alignment - 1);
            if (start >= end || end - start < bytes) continue;
            if (start > first && start + bytes < end) {
                s->freeList[i].bytes = start - first;
                memmove(&s->freeList[i + 2], &s->freeList[i + 1], (s->nfree - i - 1) * sizeof(Range));
                s->freeList[i + 1].off = start + bytes;
                s->freeList[i + 1].bytes = end - (start + bytes);
                ++s->nfree;
            } else if (start > first) {
                s->freeList[i].bytes = start - first;
            } else if (start + bytes < end) {
                s->freeList[i].off = start + bytes;
                s->freeList[i].bytes = end - (start + bytes);
            } else {
                memmove(&s->freeList[i], &s->freeList[i + 1], (s->nfree - i - 1) * sizeof(Range));
                --s->nfree;
            }
            s->freeBytes -= bytes;
            *out = start;
            return true;
        }
        return false;
    }

    void slotFree(State* s, uint32_t i)
    {
        s->bo[i].state = kFree;
        s->bo[i].owner = 0;
        s->bo[i].next = s->boFree;
        s->boFree = i;
    }

    // Buffers that were freed while GPU work was still queued go back to the heap once that work is done (as Linux
    // keeps a buffer alive until the jobs that may use it have finished).
    void reclaim(p5_dev* d)
    {
        State* s = d->st;
        uint64_t done = *d->fence;
        while (s->pendHead != kNone && s->bo[s->pendHead].freeSeq <= done) {
            uint32_t i = s->pendHead;
            s->pendHead = s->bo[i].next;
            s->pendingBytes -= s->bo[i].bytes;
            freeRange(d, s->bo[i].off, s->bo[i].bytes);
            slotFree(s, i);
        }
        if (s->pendHead == kNone) s->pendTail = kNone;
    }

    Bo* bo(p5_dev* d, uint32_t handle)
    {
        if (handle == 0 || handle > kMaxBos) return nullptr;
        Bo* b = &d->st->bo[handle - 1];
        return b->state == kLive && b->owner == d->vmid ? b : nullptr;
    }

    // Everything client `vmid` owns goes back. Only when the GPU has finished its last submission.
    void release(p5_dev* d, uint32_t vmid)
    {
        State* s = d->st;
        for (uint32_t i = 0; i < kMaxBos; ++i) {
            if (s->bo[i].state != kLive || s->bo[i].owner != vmid) continue;
            freeRange(d, s->bo[i].off, s->bo[i].bytes);
            slotFree(s, i);
        }
        for (uint32_t i = 0; i < kPoolPages; ++i)
            if (s->poolOwner[i] == vmid) s->poolOwner[i] = 0;
        for (Ctx& c : s->ctx)
            if (c.owner == vmid) c = Ctx{};
        if (s->cl[vmid].state == kLive) --s->live;
        s->cl[vmid] = Client{};
    }

    // A client goes away: at once if its work is done, otherwise it stays as a "zombie" (owning its memory and address
    // space) until the GPU has caught up.
    void detach(p5_dev* d, uint32_t vmid)
    {
        State* s = d->st;
        if (*d->fence >= s->cl[vmid].lastSeq) {
            release(d, vmid);
        } else if (s->cl[vmid].state == kLive) {
            s->cl[vmid].state = kZombie;
            --s->live;
        }
    }

    // Clients whose program ended without closing (crash, taskkill), and zombies whose work has finished.
    void reap(p5_dev* d)
    {
        State* s = d->st;
        bool current = tableCurrent(d->area);
        for (uint32_t v = 1; v < 16; ++v) {
            if (s->cl[v].state == kLive && (!current || !be::pidAlive(s->cl[v].pid))) detach(d, v);
            if (s->cl[v].state == kZombie && *d->fence >= s->cl[v].lastSeq) release(d, v);
        }
    }

    // gfx_v10_0_setup_rb / gfx_v10_0_get_cu_info: which render backends and compute units this chip has. Selecting a
    // shader engine goes through GRBM_GFX_INDEX, which must not change under running work, so it is read once, here,
    // while no client exists and the ring is empty; clients get the result from p5_get_info.
    void readHwConfig(p5_dev* d)
    {
        State* s = d->st;
        const uint32_t max_se = 2, max_sh_per_se = 2, max_cu_per_sh = 10, rb_width = 4;
        s->rbMask = s->cuCount = 0;
        for (uint32_t se = 0; se < max_se; ++se) {
            for (uint32_t sh = 0; sh < max_sh_per_se; ++sh) {
                be::wr(GC(mmGRBM_GFX_INDEX), GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK | (se << GRBM_GFX_INDEX__SE_INDEX__SHIFT) | (sh << GRBM_GFX_INDEX__SA_INDEX__SHIFT));
                uint32_t rb = R(d, GC(mmCC_RB_BACKEND_DISABLE)) | R(d, GC(mmGC_USER_RB_BACKEND_DISABLE));
                rb = (rb & CC_RB_BACKEND_DISABLE__BACKEND_DISABLE_MASK) >> CC_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT;
                s->rbMask |= (~rb & ((1u << rb_width) - 1)) << ((se * max_sh_per_se + sh) * rb_width);
                uint32_t disabled = ~((1u << (max_cu_per_sh >> 1)) - 1);
                disabled |= (R(d, GC(mmCC_GC_SHADER_ARRAY_CONFIG)) & CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK) >> CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS__SHIFT;
                disabled |= (R(d, GC(mmGC_USER_SHADER_ARRAY_CONFIG)) & GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK) >> GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_WGPS__SHIFT;
                uint32_t wgp = ~disabled, cu = 0;
                for (uint32_t w = 0; w < 16; ++w)
                    if (wgp & (1u << w)) cu |= 3u << (2 * w);
                s->cu[se * max_sh_per_se + sh] = cu;
                for (uint32_t w = 0; w < 32; ++w) s->cuCount += (cu >> w) & 1;
            }
        }
        be::wr(GC(mmGRBM_GFX_INDEX), GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK | GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK | GRBM_GFX_INDEX__SA_BROADCAST_WRITES_MASK);
        s->hwValid = 1;
    }

    // No client exists: start from a clean heap, pool and tables. The ring must be empty.
    int rebuild(p5_dev* d)
    {
        State* s = d->st;
        if (ringRptr64(d) != d->shared->wptr) return -EAGAIN;
        if (d->failed) return -EIO;
        *d->fence = d->shared->seq;
        uint32_t nextCtx = s->magic == kStateMagic ? s->nextCtx : 0;
        memset(s, 0, sizeof *s);
        s->nextCtx = nextCtx;
        s->rptrKnown = d->shared->wptr;
        s->pendHead = s->pendTail = kNone;
        for (uint32_t i = 0; i < kMaxBos; ++i) s->bo[i].next = i + 1 < kMaxBos ? i + 1 : kNone;
        // Heap: everything from kHeapStart to our area, minus the scan-out image if it happens to be in there.
        uint64_t lo = d->heapEnd, hi = d->heapEnd;
        uint64_t surf = ((uint64_t)(R(d, DMU(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH)) & 0xFFFF) << 32) | R(d, DMU(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS));
        uint64_t surfOff = surf >= d->carvePhys && surf < d->carvePhys + d->vramBytes ? surf - d->carvePhys : surf >= d->mcBase && surf < d->mcBase + d->vramBytes ? surf - d->mcBase : ~0ull;
        if (surfOff != ~0ull && surfOff + 0x4000000 > kHeapStart && surfOff < d->heapEnd) {
            lo = surfOff & ~0xFFFFFull;                      // 64 MiB covers any mode up to 4K
            hi = lo + 0x4000000;
            if (lo < kHeapStart) lo = kHeapStart;
            if (hi > d->heapEnd) hi = d->heapEnd;
        }
        if (lo > kHeapStart) freeRange(d, kHeapStart, lo - kHeapStart);
        if (hi < d->heapEnd) freeRange(d, hi, d->heapEnd - hi);
        if (!be::arm(true)) return -EIO;
        readHwConfig(d);
        be::flushWrites();
        s->magic = kStateMagic;
        return d->failed ? -EIO : 0;
    }
}

// ---- scan-out: showing one of our buffers on the screen directly ----
//
// The display hardware reads the picture from a physical address in the carve-out; the desktop's picture is where the
// launch left it. Writing another address ("flip") makes the display read from there from the next frame on, with no
// copy: Linux's hubp2_program_surface_flip_and_addr inside the timing generator's update lock, exactly as
// `ps5gpu2 flip` proved (2026-11-19). Nothing else about the display is touched: no mode, no format, no pitch, so a
// buffer can only be shown if it is the screen's size, 32-bit BGRA, with the screen's pitch.
//
// Who flipped and where the desktop's picture is must outlive any one program (and a rebuild of State), so it is kept
// in a small record of its own in our area, stamped with this Windows start so a stale one is never believed.
namespace
{
    const uint64_t kScanRecOff = kSharedOff + 0x100;
    const uint32_t kScanRecMagic = 0x4F533550;           // "P5SO"
    struct ScanRec {
        uint32_t magic, flipPid;
        uint64_t bootStamp, desktopPhys, flipPhys;
    };
    struct ScanPipe { uint32_t addrLo, addrHi, inUseLo, inUseHi, vpDim, vpStart, cntl; };
#define P5_PIPE(n) { DMU(mmHUBPREQ##n##_DCSURF_PRIMARY_SURFACE_ADDRESS), DMU(mmHUBPREQ##n##_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH), \
                     DMU(mmHUBPREQ##n##_DCSURF_SURFACE_EARLIEST_INUSE), DMU(mmHUBPREQ##n##_DCSURF_SURFACE_EARLIEST_INUSE_HIGH), \
                     DMU(mmHUBP##n##_DCSURF_PRI_VIEWPORT_DIMENSION), DMU(mmHUBP##n##_DCSURF_PRI_VIEWPORT_START), DMU(mmHUBP##n##_DCHUBP_CNTL) }
    const ScanPipe kScanPipes[4] = { P5_PIPE(0), P5_PIPE(1), P5_PIPE(2), P5_PIPE(3) };
    const uint32_t kUpdateLockStatus = 0x100;            // OTG_MASTER_UPDATE_LOCK.UPDATE_LOCK_STATUS
    const uint32_t kHubpBlankEn = 0x1;                   // DCHUBP_CNTL.HUBP_BLANK_EN

    struct ScanView {                                    // what the display shows right now, read from its registers
        uint32_t pipes = 0, width = 0, height = 0, pitch = 0;
        uint64_t phys = 0;
        bool ok = false;
    };

    bool rdReg(uint32_t reg, uint32_t& v) { return be::rd(reg, v); }

    ScanView scanLook()
    {
        ScanView v;
        uint32_t lo = 0, hi = 0, pitch = 0;
        if (!rdReg(kScanPipes[0].addrLo, lo) || !rdReg(kScanPipes[0].addrHi, hi) || !rdReg(DMU(mmHUBPREQ0_DCSURF_SURFACE_PITCH), pitch)) return v;
        // The address every showing pipe agrees on: the first unblanked pipe's.
        for (uint32_t n = 0; n < 4; ++n) {
            const ScanPipe& p = kScanPipes[n];
            uint32_t dim = 0, start = 0, cntl = 0, alo = 0, ahi = 0;
            if (!rdReg(p.vpDim, dim) || !rdReg(p.vpStart, start) || !rdReg(p.cntl, cntl) || !rdReg(p.addrLo, alo) || !rdReg(p.addrHi, ahi)) return v;
            if ((cntl & kHubpBlankEn) || !(dim & 0x3FFF) || !((dim >> 16) & 0x3FFF)) continue;
            uint64_t addr = ((uint64_t)(ahi & 0xFFFF) << 32) | alo;
            if (!v.pipes) v.phys = addr;
            else if (addr != v.phys) return v;           // the pipes disagree: someone is in the middle of something
            v.pipes |= 1u << n;
            if ((start & 0x3FFF) + (dim & 0x3FFF) > v.width) v.width = (start & 0x3FFF) + (dim & 0x3FFF);
            if (((dim >> 16) & 0x3FFF) > v.height) v.height = (dim >> 16) & 0x3FFF;
        }
        v.pitch = (pitch & 0x3FFF) + 1;                  // the register holds pitch - 1 (0x9FF for 2560)
        v.ok = v.pipes != 0 && v.width != 0 && v.height != 0;
        return v;
    }

    bool scanTaken(uint32_t pipes, uint64_t phys)
    {
        for (uint32_t n = 0; n < 4; ++n) {
            if (!(pipes & (1u << n))) continue;
            uint32_t lo = 0, hi = 0;
            if (!rdReg(kScanPipes[n].inUseLo, lo) || !rdReg(kScanPipes[n].inUseHi, hi)) return false;
            if ((((uint64_t)(hi & 0xFFFF) << 32) | lo) != phys) return false;
        }
        return true;
    }

    // The flip itself. Caller holds the lock.
    bool scanWrite(uint32_t pipes, uint64_t phys)
    {
        if (!be::arm(true)) return false;
        bool ok = be::wr(DMU(mmOTG0_OTG_MASTER_UPDATE_LOCK), 1);
        uint64_t t0 = be::nowUs();
        for (;;) {                                       // the lock is confirmed within microseconds; go on regardless after 10 ms
            uint32_t v = 0;
            if (!rdReg(DMU(mmOTG0_OTG_MASTER_UPDATE_LOCK), v) || (v & kUpdateLockStatus) || be::nowUs() - t0 > 10000) break;
        }
        for (uint32_t n = 0; n < 4; ++n) {
            if (!(pipes & (1u << n))) continue;
            ok = be::wr(kScanPipes[n].addrHi, (uint32_t)(phys >> 32)) && ok;
            ok = be::wr(kScanPipes[n].addrLo, (uint32_t)phys) && ok;
        }
        ok = be::wr(DMU(mmOTG0_OTG_MASTER_UPDATE_LOCK), 0) && ok;
        return ok;
    }

    // The record, made valid for this Windows start. `now` = what the display shows at this moment.
    ScanRec* scanRec(uint8_t* area, const ScanView& now)
    {
        ScanRec* r = (ScanRec*)(area + kScanRecOff);
        uint64_t stamp = be::bootStamp();
        if (r->magic != kScanRecMagic || r->bootStamp != stamp) {
            r->magic = kScanRecMagic;
            r->bootStamp = stamp;
            r->flipPid = 0;
            r->flipPhys = 0;
            r->desktopPhys = now.phys;                   // nobody of ours has flipped since this start: this is the desktop
        }
        return r;
    }

    // If the program that flipped is gone, the desktop comes back. Caller holds the lock.
    bool scanReclaim(uint8_t* area)
    {
        ScanView now = scanLook();
        if (!now.ok) return false;
        ScanRec* r = scanRec(area, now);
        if (!r->flipPid || be::pidAlive(r->flipPid)) return false;
        bool ok = now.phys == r->desktopPhys || scanWrite(now.pipes, r->desktopPhys);
        r->flipPid = 0;
        r->flipPhys = 0;
        return ok;
    }
}

extern "C" {

int p5_open(p5_dev** out)
{
    *out = nullptr;
    if (!be::open()) return -ENODEV;
    if (!be::lockReady()) return -EACCES;                    // no shared-state lock: no GPU (never work on the state unlocked)
    p5_dev* d = new p5_dev;
    uint32_t fbBase = R(d, GC(mmGCMC_VM_FB_LOCATION_BASE)), fbTop = R(d, GC(mmGCMC_VM_FB_LOCATION_TOP)), fbOffset = R(d, GC(mmGCMC_VM_FB_OFFSET));
    d->mcBase = (uint64_t)(fbBase & 0xFFFFFF) << 24;
    d->vramBytes = (((uint64_t)(fbTop & 0xFFFFFF) << 24) | 0xFFFFFF) - d->mcBase + 1;
    d->carvePhys = (uint64_t)fbOffset << 24;
    if (d->failed || d->carvePhys < 0x100000000ull || d->vramBytes < 0x80000000ull || d->vramBytes > 0x400000000ull) { delete d; return -ENODEV; }
    d->areaOff = d->vramBytes - kAreaBelowTop;
    d->heapEnd = d->areaOff;
    // A 64-bit client maps the whole buffer heap once. A 32-bit client cannot (3 GiB does not fit in its address
    // space), so it maps each buffer when its memory is first needed ("lazy"; PS5GPU_LAZYMAP=1 forces this mode in a
    // 64-bit client, for testing).
    d->lazy = sizeof(void*) == 4 || getenv("PS5GPU_LAZYMAP") != nullptr;
    d->area = (uint8_t*)be::map(d->carvePhys + d->areaOff, kAreaBytes, true);
    d->pool = d->area ? (uint8_t*)be::map(d->poolPhys(), kPtPoolBytes, true) : nullptr;
    if (d->pool && !d->lazy) d->heap = (uint8_t*)be::map(d->carvePhys + kHeapStart, d->heapEnd - kHeapStart, true);
    auto fail = [&](int r) {
        if (d->heap) be::unmap(d->heap);
        if (d->pool) be::unmap(d->pool);
        if (d->area) be::unmap(d->area);
        delete d;
        return r;
    };
    if (!d->pool || (!d->lazy && !d->heap)) return fail(-ENOMEM);
    d->shared = (Shared*)(d->area + kSharedOff);
    d->st = (State*)(d->area + kStateOff);
    d->fence = (volatile uint64_t*)(d->area + kFenceOff);
    d->ring = (uint32_t*)(d->area + kRingOff);

    // Wait up to 2 s for a place: with no other client the ring has to run empty first (what a program that just
    // ended left behind).
    uint64_t start = be::nowUs();
    for (;;) {
        int r = 0;
        {
            Lock g;
            // The GPU must be running our ring (ps5gpu2 run, step 6 passed).
            uint32_t me = R(d, GC(mmCP_ME_CNTL));
            if (d->failed) r = -EIO;
            else if (d->shared->magic != kSharedMagic || (me & (CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__CE_HALT_MASK))) r = -ENXIO;
            else if (!tableCurrent(d->area)) r = -ENXIO;     // the ring of the Windows before a warm Restart: not started yet
            State* s = d->st;
            if (!r) scanReclaim(d->area);                // before the heap is laid out around the scan-out image
            if (!r && s->magic == kStateMagic) reap(d);
            if (!r && (s->magic != kStateMagic || s->live == 0)) r = rebuild(d);
            if (!r) {
                uint32_t v = 1;
                while (v < 16 && s->cl[v].state != kFree) ++v;
                // The last kReserve free slots (of VMIDs 1-15) are kept for the compositor: with every program allowed
                // on the GPU, background programs must never lock the desktop out. Others get -EBUSY (Windows then
                // gives them its software renderer).
                const uint32_t kReserve = 3;
                uint32_t freeSlots = 0;
                for (uint32_t k = 1; k < 16; ++k) freeSlots += s->cl[k].state == kFree;
                if (v == 16) r = -EBUSY;
                else if (freeSlots <= kReserve && !be::isCompositor()) r = -EBUSY;
                else {
                    d->vmid = v;
                    s->cl[v] = Client{};
                    s->cl[v].state = kLive;
                    d->pid = be::pid();
                    s->cl[v].pid = d->pid;
                    s->cl[v].tlbDirty = 1;                   // the first submission installs the page-table base
                    ++s->live;
                    uint32_t root = poolAlloc(d);
                    if (root == kNone) { release(d, v); d->vmid = 0; r = -ENOMEM; }
                    else s->cl[v].root = root;
                    be::flushWrites();
                }
            }
        }
        if (r == 0) break;
        if (r != -EAGAIN) return fail(r);
        if (be::nowUs() - start > 2000000) return fail(-EIO);
        be::delayUs(100);
    }
    *out = d;
    return 0;
}

void p5_close(p5_dev* d)
{
    if (!d) return;
    if (d->scanPipes) p5_scanout_restore(d);
    p5_wait(d, d->me().lastSeq, 2000000000ull);
    {
        Lock g;
        detach(d, d->vmid);
        be::flushWrites();
    }
    for (auto& c : d->cpu) be::unmap(c.second);
    if (d->heap) be::unmap(d->heap);
    be::unmap(d->pool);
    be::unmap(d->area);
    delete d;
}

int p5_clients(void)
{
    p5_dev* d = nullptr;
    // Looks without attaching: the state is only meaningful once the ring has been started.
    if (!be::open()) return -ENODEV;
    uint32_t fbTop = 0, fbBase = 0, fbOffset = 0;
    if (!be::rd(GC(mmGCMC_VM_FB_LOCATION_BASE), fbBase) || !be::rd(GC(mmGCMC_VM_FB_LOCATION_TOP), fbTop) || !be::rd(GC(mmGCMC_VM_FB_OFFSET), fbOffset)) return -EIO;
    uint64_t mcBase = (uint64_t)(fbBase & 0xFFFFFF) << 24, vram = (((uint64_t)(fbTop & 0xFFFFFF) << 24) | 0xFFFFFF) - mcBase + 1;
    uint64_t carve = (uint64_t)fbOffset << 24;
    if (carve < 0x100000000ull || vram < 0x80000000ull || vram > 0x400000000ull) return -ENODEV;
    uint8_t* area = (uint8_t*)be::map(carve + vram - kAreaBelowTop, kAreaBytes, true);
    if (!area) return -ENOMEM;
    (void)d;
    int n = 0;
    {
        Lock g;
        State* s = (State*)(area + kStateOff);
        if (((Shared*)(area + kSharedOff))->magic == kSharedMagic && s->magic == kStateMagic && tableCurrent(area))
            for (uint32_t v = 1; v < 16; ++v) n += s->cl[v].state == kLive && be::pidAlive(s->cl[v].pid);
    }
    be::unmap(area);
    return n;
}

// For the watchdog, without attaching: 0 = started and nothing waiting, 1 = not started, 2 = the command processor
// is halted (it stops itself when it is fed something it cannot parse), 3 = work is waiting (seq > fence: normal for a
// moment; stuck if the fence does not move between two looks).
int p5_health(uint64_t* seq, uint64_t* fence)
{
    *seq = *fence = 0;
    if (!be::open()) return -ENODEV;
    uint32_t fbTop = 0, fbBase = 0, fbOffset = 0, me = 0;
    if (!be::rd(GC(mmGCMC_VM_FB_LOCATION_BASE), fbBase) || !be::rd(GC(mmGCMC_VM_FB_LOCATION_TOP), fbTop) || !be::rd(GC(mmGCMC_VM_FB_OFFSET), fbOffset) ||
        !be::rd(GC(mmCP_ME_CNTL), me)) return -EIO;
    uint64_t mcBase = (uint64_t)(fbBase & 0xFFFFFF) << 24, vram = (((uint64_t)(fbTop & 0xFFFFFF) << 24) | 0xFFFFFF) - mcBase + 1;
    uint64_t carve = (uint64_t)fbOffset << 24;
    if (carve < 0x100000000ull || vram < 0x80000000ull || vram > 0x400000000ull) return -ENODEV;
    uint8_t* area = (uint8_t*)be::map(carve + vram - kAreaBelowTop, kAreaBytes, true);
    if (!area) return -ENOMEM;
    int r = 0;
    Shared* sh = (Shared*)(area + kSharedOff);
    if (sh->magic != kSharedMagic) r = 1;
    else {
        *seq = sh->seq;
        *fence = *(volatile uint64_t*)(area + kFenceOff);
        if (me & (CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__CE_HALT_MASK)) r = 2;
        else if (*seq > *fence) {
            // Only work of a library client counts: right after `ps5gpu2 run` the two counters differ by the tool's
            // own test submissions until the first client attaches (2026-11-21: that read as "stuck").
            Lock g;
            State* st = (State*)(area + kStateOff);
            if (st->magic == kStateMagic && tableCurrent(area))
                for (uint32_t v = 1; v < 16; ++v)
                    if (st->cl[v].state == kLive && st->cl[v].lastSeq > *fence && be::pidAlive(st->cl[v].pid)) r = 3;
        }
    }
    be::unmap(area);
    return r;
}

// The programs that have the GPU open (process ids), for the recovery script. Returns how many.
int p5_client_pids(uint32_t* pids, int max)
{
    if (!be::open()) return -ENODEV;
    uint32_t fbTop = 0, fbBase = 0, fbOffset = 0;
    if (!be::rd(GC(mmGCMC_VM_FB_LOCATION_BASE), fbBase) || !be::rd(GC(mmGCMC_VM_FB_LOCATION_TOP), fbTop) || !be::rd(GC(mmGCMC_VM_FB_OFFSET), fbOffset)) return -EIO;
    uint64_t mcBase = (uint64_t)(fbBase & 0xFFFFFF) << 24, vram = (((uint64_t)(fbTop & 0xFFFFFF) << 24) | 0xFFFFFF) - mcBase + 1;
    uint64_t carve = (uint64_t)fbOffset << 24;
    if (carve < 0x100000000ull || vram < 0x80000000ull || vram > 0x400000000ull) return -ENODEV;
    uint8_t* area = (uint8_t*)be::map(carve + vram - kAreaBelowTop, kAreaBytes, true);
    if (!area) return -ENOMEM;
    int n = 0;
    {
        Lock g;
        State* s = (State*)(area + kStateOff);
        if (((Shared*)(area + kSharedOff))->magic == kSharedMagic && s->magic == kStateMagic && tableCurrent(area))
            for (uint32_t v = 1; v < 16 && n < max; ++v)
                if (s->cl[v].state == kLive && be::pidAlive(s->cl[v].pid)) pids[n++] = s->cl[v].pid;
    }
    be::unmap(area);
    return n;
}

int p5_map_probe(int heap, int ms)
{
    if (!be::open()) return -ENODEV;
    uint32_t fbTop = 0, fbBase = 0, fbOffset = 0;
    if (!be::rd(GC(mmGCMC_VM_FB_LOCATION_BASE), fbBase) || !be::rd(GC(mmGCMC_VM_FB_LOCATION_TOP), fbTop) || !be::rd(GC(mmGCMC_VM_FB_OFFSET), fbOffset)) return -EIO;
    uint64_t mcBase = (uint64_t)(fbBase & 0xFFFFFF) << 24, vram = (((uint64_t)(fbTop & 0xFFFFFF) << 24) | 0xFFFFFF) - mcBase + 1;
    uint64_t carve = (uint64_t)fbOffset << 24, areaOff = vram - kAreaBelowTop;
    uint64_t t0 = be::nowUs();
    void* area = be::map(carve + areaOff, kAreaBytes, true);
    void* pool = be::map(carve + areaOff + kAreaBytes, kPtPoolBytes, true);
    void* h = heap ? be::map(carve + kHeapStart, areaOff - kHeapStart, true) : nullptr;
    uint64_t t1 = be::nowUs();
    be::delayUs((uint64_t)ms * 1000);
    if (h) be::unmap(h);
    if (pool) be::unmap(pool);
    if (area) be::unmap(area);
    return (int)(t1 - t0);
}

int p5_state_text(char* out, int size)
{
    if (!out || size < 64) return -EINVAL;
    out[0] = 0;
    if (!be::open()) return -ENODEV;
    uint32_t fbTop = 0, fbBase = 0, fbOffset = 0;
    if (!be::rd(GC(mmGCMC_VM_FB_LOCATION_BASE), fbBase) || !be::rd(GC(mmGCMC_VM_FB_LOCATION_TOP), fbTop) || !be::rd(GC(mmGCMC_VM_FB_OFFSET), fbOffset)) return -EIO;
    uint64_t mcBase = (uint64_t)(fbBase & 0xFFFFFF) << 24, vram = (((uint64_t)(fbTop & 0xFFFFFF) << 24) | 0xFFFFFF) - mcBase + 1;
    uint64_t carve = (uint64_t)fbOffset << 24;
    if (carve < 0x100000000ull || vram < 0x80000000ull || vram > 0x400000000ull) return -ENODEV;
    uint8_t* area = (uint8_t*)be::map(carve + vram - kAreaBelowTop, kAreaBytes, true);
    if (!area) return -ENOMEM;
    int n = 0;
    {
        Lock g;
        State* s = (State*)(area + kStateOff);
        if (((Shared*)(area + kSharedOff))->magic != kSharedMagic || s->magic != kStateMagic) {
            n = snprintf(out, size, "no state\n");
        } else {
            uint64_t perClient[16] = {}, pending = 0;
            for (uint32_t i = 0; i < kMaxBos; ++i) {
                if (s->bo[i].state == kLive && s->bo[i].owner < 16) perClient[s->bo[i].owner] += s->bo[i].bytes;
                if (s->bo[i].state == kPending) pending += s->bo[i].bytes;
            }
            n = snprintf(out, size, "live %u, heap free %llu MiB, pending %llu MiB (%llu MiB in pending buffers), free ranges %u\n", s->live,
                         (unsigned long long)(s->freeBytes >> 20), (unsigned long long)(s->pendingBytes >> 20), (unsigned long long)(pending >> 20), s->nfree);
            // Overlap check: every live or pending buffer against every other, and against the free list.
            std::vector<std::pair<uint64_t, uint64_t>> used;
            for (uint32_t i = 0; i < kMaxBos; ++i)
                if (s->bo[i].state == kLive || s->bo[i].state == kPending) used.emplace_back(s->bo[i].off, s->bo[i].off + s->bo[i].bytes);
            uint32_t overlaps = 0, freeOverlaps = 0;
            std::sort(used.begin(), used.end());
            for (size_t i = 1; i < used.size(); ++i)
                if (used[i].first < used[i - 1].second) ++overlaps;
            for (uint32_t f = 0; f < s->nfree; ++f) {
                uint64_t a0 = s->freeList[f].off, a1 = a0 + s->freeList[f].bytes;
                auto it = std::lower_bound(used.begin(), used.end(), std::make_pair(a1, (uint64_t)0));
                if (it != used.begin() && (it - 1)->second > a0) ++freeOverlaps;
            }
            n += snprintf(out + n, size - n, "buffers %zu, overlapping pairs %u, free ranges overlapping buffers %u\n", used.size(), overlaps, freeOverlaps);
            // Page tables: walk every client's tree. Each table page must be owned by that client and reached from one
            // tree only; each valid leaf entry must point into a buffer that client owns.
            uint8_t* pool = (uint8_t*)be::map(carve + vram - kAreaBelowTop + kAreaBytes, kPtPoolBytes, true);
            if (pool) {
                struct Owned { uint64_t off, end; uint32_t owner; };
                std::vector<Owned> bos;
                for (uint32_t i = 0; i < kMaxBos; ++i)
                    if (s->bo[i].state == kLive) bos.push_back({s->bo[i].off, s->bo[i].off + s->bo[i].bytes, s->bo[i].owner});
                std::sort(bos.begin(), bos.end(), [](const Owned& x, const Owned& y) { return x.off < y.off; });
                std::vector<uint8_t> seen(kPoolPages, 0);
                const uint64_t poolBase = carve + vram - kAreaBelowTop + kAreaBytes;
                for (uint32_t v = 1; v < 16 && n < size - 200; ++v) {
                    if (s->cl[v].state == kFree) continue;
                    uint32_t pages = 0, shared = 0, notOwned = 0, outside = 0, ptes = 0, foreign = 0, stray = 0;
                    char ex[160] = "";
                    struct Item { uint32_t page; int level; uint64_t va; };
                    std::vector<Item> stack{{s->cl[v].root, 0, 0}};
                    while (!stack.empty()) {
                        Item it = stack.back();
                        stack.pop_back();
                        if (it.page >= kPoolPages) { ++outside; continue; }
                        ++pages;
                        if (seen[it.page] && seen[it.page] != v) ++shared;
                        seen[it.page] = (uint8_t)v;
                        if (s->poolOwner[it.page] != v) ++notOwned;
                        uint64_t tbl[512];
                        memcpy(tbl, pool + (uint64_t)it.page * kPage, sizeof tbl);
                        int shift = 39 - 9 * it.level;
                        for (uint32_t e = 0; e < 512; ++e) {
                            if (!(tbl[e] & kPteValid)) continue;
                            uint64_t va = it.va | ((uint64_t)e << shift);
                            uint64_t phys = tbl[e] & kPteAddr;
                            if (it.level < 3) {
                                if (phys < poolBase || phys >= poolBase + kPtPoolBytes) { ++outside; continue; }
                                stack.push_back({(uint32_t)((phys - poolBase) / kPage), it.level + 1, va});
                            } else {
                                ++ptes;
                                uint64_t off = phys - carve;
                                auto bit = std::upper_bound(bos.begin(), bos.end(), off, [](uint64_t o, const Owned& x) { return o < x.off; });
                                if (bit == bos.begin() || (bit - 1)->end <= off) {
                                    if (!stray && !foreign) snprintf(ex, sizeof ex, " (first: va 0x%llx -> heap 0x%llx, no buffer)", (unsigned long long)va, (unsigned long long)off);
                                    ++stray;
                                } else if ((bit - 1)->owner != v) {
                                    if (!stray && !foreign) snprintf(ex, sizeof ex, " (first: va 0x%llx -> heap 0x%llx, buffer of client %u)", (unsigned long long)va, (unsigned long long)off, (bit - 1)->owner);
                                    ++foreign;
                                }
                            }
                        }
                    }
                    n += snprintf(out + n, size - n, "client %u page tables: %u pages (%u shared with another client, %u not owned, %u outside the pool), %u mappings: %u into another client's buffer, %u into no buffer%s\n",
                                  v, pages, shared, notOwned, outside, ptes, foreign, stray, ex);
                }
                be::unmap(pool);
            }
            for (uint32_t v = 1; v < 16 && n < size - 120; ++v)
                if (s->cl[v].state != kFree)
                    n += snprintf(out + n, size - n, "client %u: %s pid %u (%s) buffers %u = %llu MiB, page-table pages %u, last seq %llu\n", v,
                                  s->cl[v].state == kLive ? "live" : "zombie", s->cl[v].pid, !tableCurrent(area) ? "earlier Windows start" : be::pidAlive(s->cl[v].pid) ? "running" : "gone", s->cl[v].bos,
                                  (unsigned long long)(perClient[v] >> 20), s->cl[v].ptPages, (unsigned long long)s->cl[v].lastSeq);
        }
    }
    be::unmap(area);
    return n;
}

int p5_scanout_query(p5_dev* d, p5_scanout* out)
{
    Lock g;
    ScanView now = scanLook();
    memset(out, 0, sizeof *out);
    if (!now.ok) return -EIO;
    ScanRec* r = scanRec(d->area, now);
    out->width = now.width;
    out->height = now.height;
    out->pitch_pixels = now.pitch;
    out->phys_now = now.phys;
    out->phys_desktop = r->desktopPhys;
    out->flipped_by_pid = r->flipPid;
    return 0;
}

int p5_scanout_flip(p5_dev* d, uint32_t handle, uint64_t offset)
{
    Lock g;
    ScanView now = scanLook();
    if (!now.ok) return -EIO;
    Bo* b = bo(d, handle);
    uint64_t bytes = (uint64_t)now.pitch * 4 * now.height;
    if (!b || (offset & (kPage - 1)) || offset > b->bytes || bytes > b->bytes - offset) return -EINVAL;
    ScanRec* r = scanRec(d->area, now);
    if (r->flipPid && r->flipPid != d->pid && be::pidAlive(r->flipPid)) return -EBUSY;      // another program has the screen
    uint64_t phys = d->carvePhys + b->off + offset;
    be::flushWrites();
    if (!scanWrite(now.pipes, phys)) return -EIO;
    r->flipPid = d->pid;
    r->flipPhys = phys;
    d->scanPipes = now.pipes;
    d->scanPhys = phys;
    return 0;
}

int p5_scanout_pending(p5_dev* d)
{
    if (!d->scanPipes) return 0;
    return scanTaken(d->scanPipes, d->scanPhys) ? 0 : 1;
}

int p5_scanout_restore(p5_dev* d)
{
    Lock g;
    ScanView now = scanLook();
    if (!now.ok) return -EIO;
    ScanRec* r = scanRec(d->area, now);
    d->scanPipes = 0;
    if (r->flipPid != d->pid) return 0;                  // not ours to undo
    bool ok = now.phys == r->desktopPhys || scanWrite(now.pipes, r->desktopPhys);
    r->flipPid = 0;
    r->flipPhys = 0;
    // The buffer may be freed right after this: wait (briefly) until the display has really let go of it.
    uint64_t t0 = be::nowUs();
    while (ok && !scanTaken(now.pipes, r->desktopPhys) && be::nowUs() - t0 < 100000) be::delayUs(500);
    return ok ? 0 : -EIO;
}

int p5_scanout_snapshot(const char* path)
{
    // What the display is reading right now, as a 32-bit BMP (bottom-up rows, as the format wants). Works whoever
    // owns the screen, so a full-screen program's real output can be looked at (a GDI screenshot shows the desktop).
    if (!be::open()) return -ENODEV;
    ScanView now = scanLook();
    if (!now.ok || (now.phys & 0xFFFF)) return -EIO;
    uint64_t bytes = (((uint64_t)now.pitch * 4 * now.height) + 0xFFFF) & ~0xFFFFull;
    const uint8_t* px = (const uint8_t*)be::map(now.phys, bytes, true);
    if (!px) return -ENOMEM;
    FILE* f = fopen(path, "wb");
    int r = f ? 0 : -EIO;
    if (f) {
        uint32_t row = now.width * 4, size = row * now.height;
        uint8_t h[54] = { 'B', 'M' };
        uint32_t v[] = { 54 + size, 0, 54, 40, now.width, now.height, 1 | (32u << 16), 0, size, 2835, 2835, 0, 0 };
        memcpy(h + 2, v, sizeof v);
        fwrite(h, 1, 54, f);
        std::vector<uint8_t> line(row);
        for (uint32_t y = now.height; y-- > 0;) {
            memcpy(line.data(), px + (uint64_t)y * now.pitch * 4, row);
            fwrite(line.data(), 1, row, f);
        }
        fclose(f);
    }
    be::unmap((void*)px);
    return r;
}

int p5_run_begin(void)
{
    if (!be::open()) return -ENODEV;
    uint32_t fbTop = 0, fbBase = 0, fbOffset = 0;
    if (!be::rd(GC(mmGCMC_VM_FB_LOCATION_BASE), fbBase) || !be::rd(GC(mmGCMC_VM_FB_LOCATION_TOP), fbTop) || !be::rd(GC(mmGCMC_VM_FB_OFFSET), fbOffset)) return -EIO;
    uint64_t mcBase = (uint64_t)(fbBase & 0xFFFFFF) << 24, vram = (((uint64_t)(fbTop & 0xFFFFFF) << 24) | 0xFFFFFF) - mcBase + 1;
    uint64_t carve = (uint64_t)fbOffset << 24;
    if (carve < 0x100000000ull || vram < 0x80000000ull || vram > 0x400000000ull) return -ENODEV;
    uint64_t id = be::sessionId(true);
    if (!id) return -EACCES;
    uint8_t* area = (uint8_t*)be::map(carve + vram - kAreaBelowTop, kAreaBytes, true);
    if (!area) return -ENOMEM;
    {
        Lock g;
        // Every client of the old ring is gone (run refuses while one is running): empty the table so that the first
        // program to attach rebuilds the heap, whatever library version it carries (older ones only check pids).
        State* s = (State*)(area + kStateOff);
        if (s->magic == kStateMagic) {
            for (uint32_t v = 1; v < 16; ++v) s->cl[v].state = kFree;
            s->live = 0;
        }
        RunRec* r = (RunRec*)(area + kRunRecOff);
        r->session = id;
        r->pad = 0;
        r->magic = kRunRecMagic;
        be::flushWrites();
    }
    be::unmap(area);
    return 0;
}

int p5_housekeeping(void)
{
    if (!be::open()) return -ENODEV;
    uint32_t fbTop = 0, fbBase = 0, fbOffset = 0;
    if (!be::rd(GC(mmGCMC_VM_FB_LOCATION_BASE), fbBase) || !be::rd(GC(mmGCMC_VM_FB_LOCATION_TOP), fbTop) || !be::rd(GC(mmGCMC_VM_FB_OFFSET), fbOffset)) return -EIO;
    uint64_t mcBase = (uint64_t)(fbBase & 0xFFFFFF) << 24, vram = (((uint64_t)(fbTop & 0xFFFFFF) << 24) | 0xFFFFFF) - mcBase + 1;
    uint64_t carve = (uint64_t)fbOffset << 24;
    if (carve < 0x100000000ull || vram < 0x80000000ull || vram > 0x400000000ull) return -ENODEV;
    p5_dev tmp;                                          // enough of a device for reap(): the area, nothing else
    tmp.carvePhys = carve; tmp.mcBase = mcBase; tmp.vramBytes = vram;
    tmp.areaOff = vram - kAreaBelowTop; tmp.heapEnd = tmp.areaOff;
    tmp.area = (uint8_t*)be::map(carve + tmp.areaOff, kAreaBytes, true);
    if (!tmp.area) return -ENOMEM;
    tmp.shared = (Shared*)(tmp.area + kSharedOff);
    tmp.st = (State*)(tmp.area + kStateOff);
    tmp.fence = (volatile uint64_t*)(tmp.area + kFenceOff);
    int did = 0;
    {
        Lock g;
        if (tmp.shared->magic == kSharedMagic) {
            if (scanReclaim(tmp.area)) did |= 1;
            if (tmp.st->magic == kStateMagic) {
                uint32_t before = tmp.st->live;
                reap(&tmp);
                if (tmp.st->live != before) did |= 2;
            }
            be::flushWrites();
        }
    }
    be::unmap(tmp.area);
    return did;
}

int p5_get_info(p5_dev* d, p5_info* info)
{
    Lock g;
    State* s = d->st;
    reclaim(d);
    memset(info, 0, sizeof *info);
    info->vram_bytes = d->vramBytes;
    info->heap_bytes = d->heapEnd - kHeapStart;
    info->heap_free = s->freeBytes + s->pendingBytes;
    info->va_start = kVaStart;
    info->va_end = kVaEnd;
    info->submitted = d->me().lastSeq;
    info->completed = *d->fence;
    info->buffers = d->me().bos;
    info->pt_pages = d->me().ptPages;
    info->vmid = d->vmid;
    info->clients = s->live;
    info->rb_mask = s->rbMask;
    info->cu_count = s->cuCount;
    for (int i = 0; i < 4; ++i) info->cu_bitmap[i] = s->cu[i];
    return 0;
}

int p5_bo_alloc(p5_dev* d, uint64_t bytes, uint64_t alignment, uint32_t* handle)
{
    if (bytes == 0 || bytes > d->heapEnd) return -EINVAL;
    bytes = (bytes + kPage - 1) & ~(kPage - 1);
    if (alignment < kPage) alignment = kPage;
    if ((alignment & (alignment - 1)) || alignment > 0x40000000ull) return -EINVAL;
    uint64_t start = 0, t0 = be::nowUs();
    uint32_t slot = kNone;
    for (;;) {
        bool pending;
        {
            Lock g;
            State* s = d->st;
            reclaim(d);
            if (s->boFree == kNone) return -ENOMEM;
            bool ok = takeRange(d, bytes, alignment, &start);
            if (!ok && s->pendHead == kNone) {               // last resort: what dead programs still hold
                reap(d);
                ok = takeRange(d, bytes, alignment, &start);
            }
            if (ok) {
                slot = s->boFree;
                s->boFree = s->bo[slot].next;
                s->bo[slot] = Bo{ start, bytes, 0, kNone, (uint8_t)d->vmid, (uint8_t)kLive, {0, 0} };
                ++d->me().bos;
                break;
            }
            pending = s->pendHead != kNone;
        }
        // Memory that is only waiting for the GPU to finish will come back: wait for it, up to 2 s.
        if (!pending || be::nowUs() - t0 > 2000000) return -ENOMEM;
        be::delayUs(200);
    }
    // Linux hands out cleared memory too. Lazy mode: map, clear, unmap (most buffers are never looked at by the CPU).
    if (!d->lazy) {
        memset(d->heap + (start - kHeapStart), 0, bytes);
    } else {
        void* tmp = be::map(d->carvePhys + start, bytes, true);
        if (!tmp) {
            Lock g;
            freeRange(d, start, bytes);
            slotFree(d->st, slot);
            --d->me().bos;
            return -ENOMEM;
        }
        memset(tmp, 0, bytes);
        be::unmap(tmp);
    }
    *handle = slot + 1;
    return 0;
}

int p5_bo_free(p5_dev* d, uint32_t handle)
{
    {
        std::lock_guard<std::mutex> l(d->lock);
        auto it = d->cpu.find(handle);
        if (it != d->cpu.end()) {
            be::unmap(it->second);
            d->cpu.erase(it);
        }
    }
    Lock g;
    State* s = d->st;
    Bo* b = bo(d, handle);
    if (!b) return -ENOENT;
    --d->me().bos;
    uint32_t i = handle - 1;
    if (d->shared->seq <= *d->fence) {
        freeRange(d, b->off, b->bytes);
        slotFree(s, i);
    } else {
        b->state = kPending;
        b->freeSeq = d->shared->seq;
        b->next = kNone;
        if (s->pendTail == kNone) s->pendHead = i;
        else s->bo[s->pendTail].next = i;
        s->pendTail = i;
        s->pendingBytes += b->bytes;
    }
    return 0;
}

int p5_bo_query(p5_dev* d, uint32_t handle, uint64_t* bytes, void** cpu)
{
    uint64_t off, size;
    {
        Lock g;
        Bo* b = bo(d, handle);
        if (!b) return -ENOENT;
        off = b->off;
        size = b->bytes;
    }
    if (bytes) *bytes = size;
    if (cpu) {
        if (!d->lazy) *cpu = d->heap + (off - kHeapStart);
        else {
            std::lock_guard<std::mutex> l(d->lock);
            uint8_t*& p = d->cpu[handle];
            if (!p) p = (uint8_t*)be::map(d->carvePhys + off, size, true);
            *cpu = p;
            if (!p) { d->cpu.erase(handle); return -ENOMEM; }
        }
    }
    return 0;
}

int p5_va_op(p5_dev* d, uint32_t handle, uint64_t offset, uint64_t bytes, uint64_t va, uint32_t flags, uint32_t op)
{
    if ((va | bytes | offset) & (kPage - 1) || bytes == 0 || va < kVaStart || va + bytes > kVaEnd || va + bytes < va) return -EINVAL;
    Lock g;
    uint64_t end = va + bytes;
    if (op == P5_VA_UNMAP || op == P5_VA_CLEAR) {
        while (va < end) {
            int miss = 12;
            uint64_t* leaf = leafTable(d, va, false, &miss);
            if (d->failed) return -EIO;
            if (!leaf) {                                     // nothing mapped under this entry: skip what it spans
                va = (va & ~((1ull << miss) - 1)) + (1ull << miss);
                continue;
            }
            for (uint64_t i = (va >> 12) & 511; i < 512 && va < end; ++i, va += kPage) leaf[i] = 0;
        }
        d->me().tlbDirty = 1;
        return 0;
    }
    if (op != P5_VA_MAP && op != P5_VA_REPLACE) return -EINVAL;
    Bo* b = bo(d, handle);
    if (!b || offset > b->bytes || bytes > b->bytes - offset) return -EINVAL;
    uint64_t bits = kPteValid | ((flags & P5_VA_READ) ? kPteReadable : 0) | ((flags & P5_VA_WRITE) ? kPteWriteable : 0) | ((flags & P5_VA_EXEC) ? kPteExecutable : 0);
    uint64_t phys = d->carvePhys + b->off + offset;
    bool dirty = op == P5_VA_REPLACE;
    while (va < end) {
        uint64_t* leaf = leafTable(d, va, true, nullptr);
        if (!leaf) return d->failed ? -EIO : -ENOMEM;
        for (uint64_t i = (va >> 12) & 511; i < 512 && va < end; ++i, va += kPage, phys += kPage) {
            if (leaf[i] & kPteValid) dirty = true;           // an existing translation changes
            leaf[i] = phys | bits;
        }
    }
    if (dirty) d->me().tlbDirty = 1;
    return 0;
}

void* p5_va_cpu(p5_dev* d, uint64_t va)
{
    uint64_t phys;
    {
        Lock g;
        uint64_t* leaf = leafTable(d, va, false, nullptr);
        if (!leaf) return nullptr;
        uint64_t e = leaf[(va >> 12) & 511];
        if (!(e & kPteValid)) return nullptr;
        phys = (e & kPteAddr) + (va & (kPage - 1));
    }
    if (phys < d->carvePhys + kHeapStart || phys >= d->carvePhys + d->heapEnd) return nullptr;
    if (!d->lazy) return d->heap + (phys - d->carvePhys - kHeapStart);
    uint64_t off = phys - d->carvePhys;          // lazy mode: only buffers that are mapped already can be looked at
    std::lock_guard<std::mutex> l(d->lock);
    for (auto& c : d->cpu) {
        Bo& b = d->st->bo[c.first - 1];
        if (b.state == kLive && b.owner == d->vmid && off >= b.off && off < b.off + b.bytes) return c.second + (off - b.off);
    }
    return nullptr;
}

int p5_ctx_create(p5_dev* d, uint32_t* ctx)
{
    Lock g;
    State* s = d->st;
    for (Ctx& c : s->ctx) {
        if (c.id) continue;
        if (++s->nextCtx == 0) ++s->nextCtx;                 // ids are never reused: "same context as last time" must not match a stranger's
        c = Ctx{ s->nextCtx, d->vmid, 0, 0 };
        *ctx = c.id;
        return 0;
    }
    return -ENOMEM;
}

int p5_ctx_free(p5_dev* d, uint32_t ctx)
{
    Lock g;
    for (Ctx& c : d->st->ctx) {
        if (c.id != ctx || c.owner != d->vmid || !ctx) continue;
        c = Ctx{};
        return 0;
    }
    return -ENOENT;
}

int p5_submit(p5_dev* d, uint32_t ctx, const p5_ib* ibs, uint32_t count, uint32_t fence_handle, uint64_t fence_offset, uint64_t* seq)
{
    if (count == 0 || count > 32) return -EINVAL;             // a submission is one block that must fit the ring at once
    bool preamble = false;
    for (uint32_t i = 0; i < count; ++i) {
        if ((ibs[i].va & 3) || (ibs[i].bytes & 3) || ibs[i].bytes == 0 || ibs[i].bytes / 4 > 0xFFFFF) return -EINVAL;
        if (ibs[i].flags & P5_IB_PREAMBLE) preamble = true;
    }
    be::flushWrites();                                        // buffer contents out of the WC buffers
    uint64_t t0 = be::nowUs();
    for (;;) {
        {
            Lock g;
            State* st = d->st;
            Ctx* c = nullptr;
            for (Ctx& k : st->ctx)
                if (k.id == ctx && k.owner == d->vmid && ctx) c = &k;
            if (!c) return -EINVAL;
            uint64_t userFenceMc = 0;
            if (fence_handle) {
                Bo* b = bo(d, fence_handle);
                if (!b || (fence_offset & 7) || fence_offset + 8 > b->bytes) return -EINVAL;
                userFenceMc = d->mcBase + b->off + fence_offset;
            }
            if (ringRoom(d, 64 + 4 * count)) {
                d->txWptr = d->shared->wptr;
                // A packet must not straddle the end of the ring: if this submission would, start it at offset 0.
                if ((d->txWptr & (d->ringDw - 1)) + 64 + 4 * count > d->ringDw)
                    while (d->txWptr & (d->ringDw - 1)) ringWrite(d, PACKET3(PACKET3_NOP, 0x3FFF));

                // amdgpu_ib_schedule without preemption: address-space flush if a mapping went away, context control,
                // the buffers, the fence(s).
                be::flushWrites();                            // page tables out of the WC buffers
                // amdgpu_ib_schedule: a job from another context than the one that ran last first waits until everything
                // before it on the ring has finished (need_ctx_switch -> gfx_v10_0_ring_emit_pipeline_sync, a
                // WAIT_REG_MEM on the ring's fence, PFP, "equal"). Without it two programs' work overlapped on the GPU and
                // the compositor's textures got another program's data (2026-11-21).
                static const bool serial = be::envFlag("PS5GPU_SERIAL");   // debugging: every submission waits for all before it
                if ((ctx != st->lastCtx || serial) && st->lastCtx) {
                    uint64_t fenceMc = d->areaMc() + kFenceOff;
                    ringWrite(d, PACKET3(PACKET3_WAIT_REG_MEM, 5));
                    ringWrite(d, WAIT_REG_MEM_MEM_SPACE(1) | WAIT_REG_MEM_OPERATION(0) | WAIT_REG_MEM_FUNCTION(3) | WAIT_REG_MEM_ENGINE(1));
                    ringWrite(d, (uint32_t)fenceMc);
                    ringWrite(d, (uint32_t)(fenceMc >> 32));
                    ringWrite(d, (uint32_t)d->shared->seq);
                    ringWrite(d, 0xFFFFFFFFu);
                    ringWrite(d, 4);
                }
                static const bool paranoid = be::envFlag("PS5GPU_PARANOID");   // debugging: caches and translations flushed every time
                if (paranoid) {
                    emitMemSync(d);
                    d->me().tlbDirty = 1;
                }
                if (d->me().tlbDirty) {
                    emitTlbFlush(d);
                    d->me().tlbDirty = 0;
                }
                bool ctxSwitch = ctx != st->lastCtx, preambleFirst = preamble && !c->preamble;
                if (preamble) c->preamble = 1;
                uint32_t dw2 = 0x80000000;                    // gfx_v10_0_ring_emit_cntxcntl
                if (ctxSwitch) dw2 |= 0x8001 | 0x01000000 | 0x10002 | (preamble ? 0x10000000 : 0);
                else if (preambleFirst) dw2 |= 0x10000000;
                ringWrite(d, PACKET3(PACKET3_CONTEXT_CONTROL, 1));
                ringWrite(d, dw2);
                ringWrite(d, 0);
                for (uint32_t i = 0; i < count; ++i) {
                    // A preamble is skipped when the same context ran last and has presented it before.
                    if ((ibs[i].flags & P5_IB_PREAMBLE) && !ctxSwitch && !preambleFirst) continue;
                    ringWrite(d, PACKET3((ibs[i].flags & P5_IB_CE) ? PACKET3_INDIRECT_BUFFER_CNST : PACKET3_INDIRECT_BUFFER, 2));
                    ringWrite(d, (uint32_t)ibs[i].va);
                    ringWrite(d, (uint32_t)(ibs[i].va >> 32));
                    ringWrite(d, (ibs[i].bytes / 4) | (d->vmid << 24));
                }
                uint64_t s = d->shared->seq + 1;
                emitFence(d, d->areaMc() + kFenceOff, s);
                if (userFenceMc) emitFence(d, userFenceMc, s);
                ringCommit(d, s, ctx);
                if (seq) *seq = s;
                return d->failed ? -EIO : 0;
            }
        }
        // The ring is full: let the others in and try again; give up if the GPU stopped consuming it.
        if (d->failed || be::nowUs() - t0 > 5000000) return -EIO;
        be::delayUs(50);
    }
}

uint64_t p5_completed(p5_dev* d) { return *d->fence; }

int p5_wait(p5_dev* d, uint64_t seq, uint64_t timeout_ns)
{
    uint64_t start = be::nowUs(), limit = timeout_ns / 1000;
    for (;;) {
        if (*d->fence >= seq) return 0;
        uint64_t waited = be::nowUs() - start;
        if (waited >= limit) return 1;
        be::delayUs(waited < 2000 ? 20 : 200);
    }
}

}
