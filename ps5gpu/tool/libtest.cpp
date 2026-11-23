// SPDX-License-Identifier: MIT
// Test of libps5gpu: buffers, mappings, many submissions with fences, a remap, ring wrap-around.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../lib/ps5drm.h"
#include "regs.h"
#include "backend.h"

static uint32_t buildFill(uint32_t* ib, uint64_t dstVa, uint32_t bytes, uint32_t value)
{
    uint32_t n = 0;
    ib[n++] = PACKET3(PACKET3_DMA_DATA, 5);
    ib[n++] = PACKET3_DMA_DATA_ENGINE(0) | PACKET3_DMA_DATA_DST_SEL(0) | PACKET3_DMA_DATA_SRC_SEL(2) | PACKET3_DMA_DATA_CP_SYNC;
    ib[n++] = value;
    ib[n++] = 0;
    ib[n++] = (uint32_t)dstVa;
    ib[n++] = (uint32_t)(dstVa >> 32);
    ib[n++] = bytes;
    while (n & 7) ib[n++] = PACKET3(PACKET3_NOP, 0x3FFF);
    return n * 4;
}

int libtest(unsigned rounds)
{
    p5_dev* d = nullptr;
    p5_info info;
    int r = p5_open(&d);
    if (r) { printf("libtest: p5_open failed: %d (%s)\n", r, r == -6 ? "the GPU has not been started: ps5gpu2 run first" : r == -16 ? "another client owns the GPU" : "see above"); return 1; }
    p5_get_info(d, &info);
    printf("libtest: VRAM %llu MiB, heap %llu MiB free, last sequence %llu / done %llu\n", (unsigned long long)(info.vram_bytes >> 20),
        (unsigned long long)(info.heap_free >> 20), (unsigned long long)info.submitted, (unsigned long long)info.completed);

    const uint64_t ibVa = 0x100000000ull, dataVa = 0x110000000ull, pages = 256;
    uint32_t ctx = 0, ibBo = 0, dataBo = 0, otherBo = 0, fenceBo = 0;
    void *ibCpu = nullptr, *dataCpu = nullptr, *otherCpu = nullptr, *fenceCpu = nullptr;
    bool ok = true;
    ok = ok && !p5_ctx_create(d, &ctx);
    ok = ok && !p5_bo_alloc(d, 0x10000, 0x1000, &ibBo) && !p5_bo_alloc(d, pages * 0x1000, 0x10000, &dataBo) && !p5_bo_alloc(d, pages * 0x1000, 0x1000, &otherBo) &&
         !p5_bo_alloc(d, 0x1000, 0x1000, &fenceBo);
    ok = ok && !p5_bo_query(d, ibBo, nullptr, &ibCpu) && !p5_bo_query(d, dataBo, nullptr, &dataCpu) && !p5_bo_query(d, otherBo, nullptr, &otherCpu) &&
         !p5_bo_query(d, fenceBo, nullptr, &fenceCpu);
    ok = ok && !p5_va_op(d, ibBo, 0, 0x10000, ibVa, P5_VA_READ | P5_VA_EXEC, P5_VA_MAP) && !p5_va_op(d, dataBo, 0, pages * 0x1000, dataVa, P5_VA_READ | P5_VA_WRITE, P5_VA_MAP);
    if (!ok) { printf("libtest: setup failed\n"); p5_close(d); return 1; }
    volatile uint32_t* data = (volatile uint32_t*)dataCpu;
    volatile uint32_t* other = (volatile uint32_t*)otherCpu;
    volatile uint64_t* userFence = (volatile uint64_t*)fenceCpu;
    if (data[0] != 0 || data[pages * 1024 - 1] != 0) { printf("libtest: new buffer is not zero\n"); ok = false; }

    // Many small submissions: each fills one page with its own value. 64 command slots are reused, so we never
    // overwrite a command buffer the GPU may still be reading: wait for the submission that last used the slot.
    uint64_t seqOfSlot[64] = {0}, seq = 0, firstSeq = 0, t0 = be::nowUs();
    uint32_t lastValue[256] = {0};
    for (unsigned i = 0; ok && i < rounds; ++i) {
        unsigned slot = i % 64, page = (i * 7) % pages;
        if (seqOfSlot[slot] && p5_wait(d, seqOfSlot[slot], 2000000000ull)) { printf("libtest: timeout waiting for sequence %llu\n", (unsigned long long)seqOfSlot[slot]); ok = false; break; }
        uint32_t value = 0xA5000000u + i;
        p5_ib ib = { ibVa + slot * 0x100, buildFill((uint32_t*)ibCpu + slot * 0x40, dataVa + (uint64_t)page * 0x1000, 0x1000, value), 0 };
        r = p5_submit(d, ctx, &ib, 1, fenceBo, 8, &seq);
        if (r) { printf("libtest: submit %u failed: %d\n", i, r); ok = false; break; }
        if (!firstSeq) firstSeq = seq;
        seqOfSlot[slot] = seq;
        lastValue[page] = value;
    }
    if (ok && p5_wait(d, seq, 2000000000ull)) { printf("libtest: timeout at the end\n"); ok = false; }
    unsigned wrong = 0;
    for (unsigned p = 0; ok && p < pages; ++p) {
        if (!lastValue[p]) continue;
        if (data[p * 1024] != lastValue[p] || data[p * 1024 + 1023] != lastValue[p]) ++wrong;
    }
    printf("libtest: %.2f s for the submissions\n", (be::nowUs() - t0) / 1e6);
    printf("libtest: %u submissions (sequence %llu..%llu), %u wrong pages, user fence %llu, completed %llu\n", rounds, (unsigned long long)firstSeq,
        (unsigned long long)seq, wrong, (unsigned long long)userFence[1], (unsigned long long)p5_completed(d));
    if (wrong || userFence[1] != seq) ok = false;

    // Remap: the same virtual address now leads to the other buffer. The stale translation must be flushed.
    if (ok) {
        ok = !p5_va_op(d, 0, 0, pages * 0x1000, dataVa, 0, P5_VA_UNMAP) && !p5_va_op(d, otherBo, 0, pages * 0x1000, dataVa, P5_VA_READ | P5_VA_WRITE, P5_VA_MAP);
        p5_ib ib = { ibVa, buildFill((uint32_t*)ibCpu, dataVa, 0x1000, 0x0E0E0E0E), 0 };
        uint32_t before = data[0];
        ok = ok && !p5_submit(d, ctx, &ib, 1, 0, 0, &seq) && !p5_wait(d, seq, 2000000000ull);
        bool good = other[0] == 0x0E0E0E0E && other[1023] == 0x0E0E0E0E && data[0] == before;
        printf("libtest: after the remap the fill landed in %s (new buffer %08x, old buffer %08x)\n", good ? "the new buffer" : "THE WRONG PLACE", (uint32_t)other[0], (uint32_t)data[0]);
        ok = ok && good;
    }
    // Scan-out: show one of our buffers on the screen and give the screen back. LIBTEST_FLIP=1 on the console (the
    // picture is visibly replaced by a grey gradient for half a second); always in the simulator.
#ifndef PS5GPU_MOCK
    if (ok && getenv("LIBTEST_FLIP"))
#else
    if (ok)
#endif
    {
        p5_scanout so, so2;
        uint32_t scanBo = 0;
        void* scanCpu = nullptr;
        ok = !p5_scanout_query(d, &so);
        printf("libtest: screen %ux%u, pitch %u, picture at 0x%llx (desktop 0x%llx), flipped by pid %u\n", so.width, so.height, so.pitch_pixels,
            (unsigned long long)so.phys_now, (unsigned long long)so.phys_desktop, so.flipped_by_pid);
        uint64_t bytes = (uint64_t)so.pitch_pixels * 4 * so.height;
        ok = ok && so.phys_now == so.phys_desktop && !p5_bo_alloc(d, bytes, 0x10000, &scanBo) && !p5_bo_query(d, scanBo, nullptr, &scanCpu);
        if (ok) {
            uint32_t* px = (uint32_t*)scanCpu;
            for (uint32_t y = 0; y < so.height; ++y)
                for (uint32_t x = 0; x < so.width; ++x) px[(uint64_t)y * so.pitch_pixels + x] = 0xFF000000u | ((x * 255 / so.width) * 0x010101u);
            uint64_t t = be::nowUs();
            int r1 = p5_scanout_flip(d, scanBo, 0);
            while (!r1 && p5_scanout_pending(d) && be::nowUs() - t < 500000) be::delayUs(200);
            uint64_t took = be::nowUs() - t;
            p5_scanout_query(d, &so2);
            bool shown = !r1 && !p5_scanout_pending(d) && so2.phys_now != so.phys_desktop && so2.flipped_by_pid != 0;
            printf("libtest: flip %s (%d), taken after %llu us, picture now at 0x%llx\n", shown ? "taken" : "NOT TAKEN", r1, (unsigned long long)took, (unsigned long long)so2.phys_now);
#ifndef PS5GPU_MOCK
            be::delayUs(500000);
#endif
            int r2 = p5_scanout_restore(d);
            p5_scanout_query(d, &so2);
            bool back = !r2 && so2.phys_now == so.phys_desktop && so2.flipped_by_pid == 0;
            printf("libtest: desktop %s (picture at 0x%llx)\n", back ? "back" : "NOT BACK", (unsigned long long)so2.phys_now);
            ok = shown && back;
        } else printf("libtest: scan-out setup failed\n");
        if (scanBo) p5_bo_free(d, scanBo);
    }
    p5_va_op(d, 0, 0, pages * 0x1000, dataVa, 0, P5_VA_UNMAP);
    p5_va_op(d, 0, 0, 0x10000, ibVa, 0, P5_VA_UNMAP);
    p5_bo_free(d, ibBo); p5_bo_free(d, dataBo); p5_bo_free(d, otherBo); p5_bo_free(d, fenceBo);
    uint64_t freeBefore = info.heap_free;
    p5_get_info(d, &info);
    printf("libtest: heap free again %llu MiB (%s), page-table pages used %u\n", (unsigned long long)(info.heap_free >> 20), info.heap_free == freeBefore ? "all returned" : "LEAK",
        info.pt_pages);
    ok = ok && info.heap_free == freeBefore;
    p5_ctx_free(d, ctx);
    p5_close(d);
    printf(ok ? "LIBTEST RESULT: PASS\n" : "LIBTEST RESULT: FAIL\n");
    return ok ? 0 : 1;
}

// Several clients at once: each has its own address space, so all of them use the SAME GPU virtual addresses for
// different buffers; submissions are interleaved. Then one leaves (its memory must come back), one "dies" without
// closing (the next attach must clean up after it), and a new one takes a freed address space.
namespace
{
    struct Client {
        p5_dev* d = nullptr;
        uint32_t ctx = 0, ibBo = 0, dataBo = 0;
        void *ibCpu = nullptr, *dataCpu = nullptr;
        uint64_t seq = 0;
        p5_info info;
    };
    const uint64_t kIbVa = 0x100000000ull, kDataVa = 0x110000000ull, kPages = 64;

    bool attach(Client& c, const char* name)
    {
        int r = p5_open(&c.d);
        if (r) { printf("libtest2: %s: p5_open failed: %d\n", name, r); return false; }
        bool ok = !p5_ctx_create(c.d, &c.ctx) && !p5_bo_alloc(c.d, 0x10000, 0x1000, &c.ibBo) && !p5_bo_alloc(c.d, kPages * 0x1000, 0x1000, &c.dataBo) &&
                  !p5_bo_query(c.d, c.ibBo, nullptr, &c.ibCpu) && !p5_bo_query(c.d, c.dataBo, nullptr, &c.dataCpu) &&
                  !p5_va_op(c.d, c.ibBo, 0, 0x10000, kIbVa, P5_VA_READ | P5_VA_EXEC, P5_VA_MAP) &&
                  !p5_va_op(c.d, c.dataBo, 0, kPages * 0x1000, kDataVa, P5_VA_READ | P5_VA_WRITE, P5_VA_MAP);
        p5_get_info(c.d, &c.info);
        printf("libtest2: %s attached: address space %u, %u client(s), heap free %llu MiB%s\n", name, c.info.vmid, c.info.clients, (unsigned long long)(c.info.heap_free >> 20), ok ? "" : ", SETUP FAILED");
        return ok;
    }

    // One fill of page `page` with `value`, through command slot `slot`.
    bool fill(Client& c, unsigned slot, unsigned page, uint32_t value)
    {
        p5_ib ib = { kIbVa + slot * 0x100, buildFill((uint32_t*)c.ibCpu + slot * 0x40, kDataVa + (uint64_t)page * 0x1000, 0x1000, value), 0 };
        return p5_submit(c.d, c.ctx, &ib, 1, 0, 0, &c.seq) == 0;
    }

    unsigned wrongPages(Client& c, uint32_t base, unsigned rounds)
    {
        unsigned wrong = 0;
        volatile uint32_t* data = (volatile uint32_t*)c.dataCpu;
        for (unsigned p = 0; p < kPages; ++p) {
            uint32_t want = 0;
            for (unsigned i = 0; i < rounds; ++i)
                if (i % kPages == p) want = base + i;
            if (want && (data[p * 1024] != want || data[p * 1024 + 1023] != want)) ++wrong;
        }
        return wrong;
    }
}

int libtest2(unsigned rounds)
{
    Client a, b, c;
    int others = p5_clients();          // programs already on the GPU: then counts are relative and the heap moves under us
    if (others < 0) others = 0;
    const uint32_t base = (uint32_t)others;
    bool ok = attach(a, "A") && attach(b, "B") && attach(c, "C");
#ifdef PS5GPU_MOCK
    uint32_t pidC = be::lastPid();
#endif
    if (ok && (a.info.vmid == b.info.vmid || b.info.vmid == c.info.vmid || a.info.vmid == c.info.vmid || c.info.clients != base + 3)) { printf("libtest2: clients share an address space or are miscounted\n"); ok = false; }
    uint64_t freeWithThree = c.info.heap_free;
    // Interleaved: A, B, C, A, B, C ... each waits for its own previous use of a command slot (16 slots).
    uint64_t slotSeq[3][16] = {};
    Client* cl[3] = { &a, &b, &c };
    for (unsigned i = 0; ok && i < rounds; ++i) {
        for (unsigned k = 0; ok && k < 3; ++k) {
            unsigned slot = i % 16;
            if (slotSeq[k][slot] && p5_wait(cl[k]->d, slotSeq[k][slot], 2000000000ull)) { printf("libtest2: timeout\n"); ok = false; break; }
            ok = fill(*cl[k], slot, i % kPages, 0xA0000000u + (k << 24) + i);
            slotSeq[k][slot] = cl[k]->seq;
        }
    }
    for (unsigned k = 0; ok && k < 3; ++k) {
        if (p5_wait(cl[k]->d, cl[k]->seq, 2000000000ull)) { printf("libtest2: timeout at the end\n"); ok = false; break; }
        unsigned wrong = wrongPages(*cl[k], 0xA0000000u + (k << 24), rounds);
        printf("libtest2: client %c: %u submissions, last sequence %llu, %u wrong pages\n", 'A' + k, rounds, (unsigned long long)cl[k]->seq, wrong);
        if (wrong) ok = false;
    }

    // B leaves properly. Its memory comes back; A and C keep working.
    uint32_t vmidB = b.info.vmid;
    if (ok) {
        p5_close(b.d);
        b.d = nullptr;
        p5_get_info(a.d, &a.info);
        uint64_t per = 0x10000 + kPages * 0x1000;
        printf("libtest2: B closed: %u clients, heap free %llu KiB more than with three\n", a.info.clients, (unsigned long long)((a.info.heap_free - freeWithThree) >> 10));
        if (a.info.clients != base + 2 || (!base && a.info.heap_free != freeWithThree + per)) { printf("libtest2: B's memory did not come back exactly\n"); ok = false; }
        ok = ok && fill(a, 0, 1, 0x11111111) && fill(c, 0, 1, 0x33333333) && !p5_wait(a.d, a.seq, 2000000000ull) && !p5_wait(c.d, c.seq, 2000000000ull);
        if (ok && (((volatile uint32_t*)a.dataCpu)[1024] != 0x11111111 || ((volatile uint32_t*)c.dataCpu)[1024] != 0x33333333)) { printf("libtest2: A or C broke when B left\n"); ok = false; }
    }
#ifdef PS5GPU_MOCK
    // C "dies" without closing (only the simulator can pretend that). The next attach finds out and cleans up.
    if (ok) {
        p5_scanout so;
        uint32_t scanBo = 0;
        ok = !p5_scanout_query(c.d, &so) && !p5_bo_alloc(c.d, (uint64_t)so.pitch_pixels * 4 * so.height, 0x10000, &scanBo) && !p5_scanout_flip(c.d, scanBo, 0);
        int busy = ok ? p5_scanout_flip(a.d, a.dataBo, 0) : 0;      // A may not take the screen from a living C (and its buffer is too small anyway)
        printf("libtest2: C took the screen: %s; A trying meanwhile: %d\n", ok ? "yes" : "NO", busy);
        if (busy == 0) ok = false;
        be::killPid(pidC);
        c.d = nullptr;                 // leaked on purpose, like a killed program
    }
#else
    if (ok) { p5_close(c.d); c.d = nullptr; }
#endif
    Client e;
    if (ok) {
        ok = attach(e, "D");
#ifdef PS5GPU_MOCK
        if (ok) {
            p5_scanout so;
            p5_scanout_query(e.d, &so);
            bool back = so.phys_now == so.phys_desktop && so.flipped_by_pid == 0;
            printf("libtest2: after C died holding the screen, the desktop is %s\n", back ? "back" : "NOT BACK");
            ok = back;
        }
#endif
        uint64_t per = 0x10000 + kPages * 0x1000;
        if (ok && (e.info.clients != base + 2 || (!base && e.info.heap_free != freeWithThree + per))) { printf("libtest2: after C went, clients %u and heap do not add up\n", e.info.clients); ok = false; }
        if (ok && e.info.vmid != vmidB) printf("libtest2: note: D got address space %u (B had %u)\n", e.info.vmid, vmidB);
        // D reuses an address space someone else had: its first submission must install its own tables.
        ok = ok && fill(e, 0, 2, 0x44444444) && fill(a, 1, 2, 0x12121212) && !p5_wait(e.d, e.seq, 2000000000ull) && !p5_wait(a.d, a.seq, 2000000000ull);
        if (ok && (((volatile uint32_t*)e.dataCpu)[2 * 1024] != 0x44444444 || ((volatile uint32_t*)a.dataCpu)[2 * 1024] != 0x12121212)) { printf("libtest2: D's fill landed in the wrong place\n"); ok = false; }
    }
    if (e.d) p5_close(e.d);
    if (a.d) p5_close(a.d);
    if (b.d) p5_close(b.d);
#ifndef PS5GPU_MOCK
    if (c.d) p5_close(c.d);
#endif
    printf("libtest2: %d client(s) left (%u were there before)\n", p5_clients(), base);
    if (p5_clients() != (int)base) ok = false;
    printf(ok ? "LIBTEST2 RESULT: PASS\n" : "LIBTEST2 RESULT: FAIL\n");
    return ok ? 0 : 1;
}
