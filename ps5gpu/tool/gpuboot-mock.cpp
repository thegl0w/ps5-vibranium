// SPDX-License-Identifier: MIT
// Host test of the boot-time GPU start (wddm/gpuboot.cpp, the code ps5wddm.sys runs in DriverEntry) against the same
// register/memory model stage2 is tested with (backend_mock.cpp). build-gpuboot-mock.sh compares its register writes
// with stage2's.
//   gpuboot-mock <fw dir> [draw] [clients] [budget=<ms>]
//     draw     also step 7 (the on-screen rectangle), so the write trace can be compared with `stage2-mock run <fw> 8`
//     clients  pretend libps5gpu has a program attached and ask for the live-restart check (must refuse)
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "backend.h"
#include "../wddm/gpuboot.h"

static bool mrd(void*, uint32_t reg, uint32_t* v) { return be::rd(reg, *v); }
static bool mwr(void*, uint32_t reg, uint32_t v) { return be::wr(reg, v); }
static void mdelay(void*, uint32_t us) { be::delayUs(us); }
static uint64_t mnow(void*) { return be::nowUs(); }
static int gMaps;
static uint8_t* mmap_(void*, uint64_t phys, uint64_t bytes) { uint8_t* p = (uint8_t*)be::map(phys, bytes, true); if (p) ++gMaps; return p; }
static void munmap_(void*, uint8_t* va, uint64_t) { if (va) --gMaps; }
static bool mdoor(void*, uint32_t off, uint64_t v) { return be::doorbell64(off, v); }
static void mflush(void*) { be::flushWrites(); }
static void mline(void*, const char* t) { printf("  %s\n", t); }
static void mstep(void*, uint32_t n, const char* name) { printf("[step %u: %s]\n", n, name); }
static int mfmt(char* out, size_t n, const char* fmt, va_list ap) { return vsnprintf(out, n, fmt, ap); }

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: gpuboot-mock <fw dir> [draw] [clients] [budget=<ms>]\n"); return 2; }
    gpuboot::Options opt = { false, false, 5000 };
    bool fakeClient = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "draw")) opt.drawTest = true;
        else if (!strcmp(argv[i], "clients")) { opt.refuseIfClients = true; fakeClient = true; }
        else if (!strncmp(argv[i], "budget=", 7)) opt.budgetMs = (uint32_t)strtoul(argv[i] + 7, NULL, 0);
    }
    static std::vector<uint8_t> file[gpuboot::kFwCount];
    gpuboot::Files files = {};
    for (int i = 0; i < gpuboot::kFwCount; ++i) {
        std::string path = std::string(argv[1]) + "/" + gpuboot::kFwNames[i];
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) { printf("cannot open %s\n", path.c_str()); return 1; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        file[i].resize(n > 0 ? (size_t)n : 0);
        if (n <= 0 || fread(file[i].data(), 1, (size_t)n, f) != (size_t)n) { printf("cannot read %s\n", path.c_str()); return 1; }
        fclose(f);
        files.data[i] = file[i].data();
        files.bytes[i] = file[i].size();
    }
    if (!be::open() || !be::arm(true)) return 2;
    if (fakeClient) {
        // libps5gpu's state at area + 10 MiB (area = top of VRAM - 512 MiB; the model: 4 GiB of VRAM): magic, 1 live.
        uint32_t* st = (uint32_t*)(be::vram() + 0x100000000ull - 0x20000000ull + 0xA00000);
        st[0] = 0x33533550;
        st[1] = 1;
    }
    gpuboot::Env env = { nullptr, mrd, mwr, mdelay, mnow, mmap_, munmap_, mdoor, mflush, mline, mstep, mfmt };
    char reason[200];
    gpuboot::Result r = gpuboot::run(env, files, opt, reason, sizeof reason);
    be::report();
    bool clean = be::clean();
    printf("gpuboot result %u: %s; mappings left %d; model %s\n", (unsigned)r, reason, gMaps, clean ? "clean" : "NOT clean");
    if (fakeClient) return r == gpuboot::kClients && gMaps == 0 ? 0 : 1;
    return r == gpuboot::kOk && clean && gMaps == 0 ? 0 : 1;
}
