// SPDX-License-Identifier: MIT
// Host test for CyanDirectLoad: synthetic firmware through parse(), and every load sequence checked access by access
// against a recording Io. Build: clang++ -std=c++20 -Wall -Wextra -I. test_directload.cpp CyanDirectLoad.cpp
#include "CyanDirectLoad.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace CyanDirectLoad;

static int gChecks, gFailures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++gChecks;                                                                                                     \
        if (!(cond)) {                                                                                                 \
            ++gFailures;                                                                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
        }                                                                                                              \
    } while (0)

// ---------------------------------------------------------------------------------------------------------------
// Synthetic firmware files

static void put32(std::vector<uint8_t>& b, size_t at, uint32_t v)
{
    if (b.size() < at + 4) b.resize(at + 4);
    for (int i = 0; i < 4; ++i) b[at + i] = (uint8_t)(v >> (8 * i));
}

// One PSP container: 0x100 header (magic, inner size), inner bytes, 0x100 signature.
static std::vector<uint8_t> box(const std::vector<uint8_t>& inner, uint32_t magic = 0x31535024, int sizeSkew = 0)
{
    std::vector<uint8_t> b(0x100, 0xEE);
    put32(b, 0x10, magic);
    put32(b, 0x14, (uint32_t)(inner.size() + sizeSkew));
    b.insert(b.end(), inner.begin(), inner.end());
    b.insert(b.end(), 0x100, 0x5A);
    return b;
}

static std::vector<uint8_t> pattern(size_t bytes, uint32_t seed)
{
    std::vector<uint8_t> v(bytes);
    for (size_t i = 0; i < bytes; i += 4) put32(v, i, seed + (uint32_t)(i / 4) * 0x01010101u);
    return v;
}

struct FileSpec {
    uint32_t version = 0x1234;
    uint32_t jtOffsetDw = 0, jtSizeDw = 0;
    uint32_t payloadAt = 0x100;
};

static std::vector<uint8_t> makeFile(const std::vector<uint8_t>& payload, const FileSpec& fs)
{
    std::vector<uint8_t> f(fs.payloadAt, 0);
    f.insert(f.end(), payload.begin(), payload.end());
    put32(f, 0x00, (uint32_t)f.size());
    put32(f, 0x10, fs.version);
    put32(f, 0x14, (uint32_t)payload.size());
    put32(f, 0x18, fs.payloadAt);
    put32(f, 0x24, fs.jtOffsetDw);
    put32(f, 0x28, fs.jtSizeDw);
    return f;
}

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// ---------------------------------------------------------------------------------------------------------------
// Recording Io with a tiny register model

struct Access {
    char op; // 'R', 'W', 'D'
    uint32_t reg, value;
    bool operator==(const Access& o) const { return op == o.op && reg == o.reg && value == o.value; }
};

struct Sim {
    std::map<uint32_t, uint32_t> regs;
    std::vector<Access> log;
    int cpStatBusyReads = 0;       // CP_STAT reads that return busy before 0
    int icDoneAfterReads = 1;      // IC_OP_CNTL reads after an invalidate until the done bit shows; <0 never
    std::map<uint32_t, int> icPending;
};

static bool isIcOp(uint32_t reg)
{
    return reg == Reg::Pfp.icOpCntl || reg == Reg::Ce.icOpCntl || reg == Reg::Me.icOpCntl || reg == Reg::Mec.icOpCntl;
}

static uint32_t simRead(void* ctx, uint32_t reg)
{
    Sim& s = *(Sim*)ctx;
    uint32_t v = s.regs[reg];
    if (reg == Reg::CpStat) {
        v = s.cpStatBusyReads > 0 ? 0x80008000u : 0;
        if (s.cpStatBusyReads > 0) --s.cpStatBusyReads;
    } else if (isIcOp(reg) && s.icPending.count(reg)) {
        int& left = s.icPending[reg];
        if (left == 0) v |= Reg::kIcInvalidateDone;
        else if (left > 0) --left;
    }
    s.log.push_back({ 'R', reg, v });
    return v;
}

static void simWrite(void* ctx, uint32_t reg, uint32_t value)
{
    Sim& s = *(Sim*)ctx;
    s.regs[reg] = value;
    if (isIcOp(reg) && (value & Reg::kIcInvalidate)) s.icPending[reg] = s.icDoneAfterReads;
    s.log.push_back({ 'W', reg, value });
}

static void simDelay(void* ctx, uint32_t us)
{
    ((Sim*)ctx)->log.push_back({ 'D', 0, us });
}

static Io ioFor(Sim& s) { return Io{ &s, simRead, simWrite, simDelay }; }

// Expected-sequence builder. Values are written out explicitly per test rather than re-derived from the code.
struct Expect {
    std::vector<Access> a;
    Expect& R(uint32_t reg, uint32_t v) { a.push_back({ 'R', reg, v }); return *this; }
    Expect& W(uint32_t reg, uint32_t v) { a.push_back({ 'W', reg, v }); return *this; }
    Expect& D(uint32_t us) { a.push_back({ 'D', 0, us }); return *this; }
};

static bool sameLog(const std::vector<Access>& got, const std::vector<Access>& want, const char* what)
{
    size_t n = got.size() < want.size() ? got.size() : want.size();
    for (size_t i = 0; i < n; ++i) {
        if (!(got[i] == want[i])) {
            printf("  %s: access %zu differs: got %c 0x%05x 0x%08x, want %c 0x%05x 0x%08x\n", what, i, got[i].op, got[i].reg,
                   got[i].value, want[i].op, want[i].reg, want[i].value);
            return false;
        }
    }
    if (got.size() != want.size()) {
        printf("  %s: %zu accesses, want %zu\n", what, got.size(), want.size());
        return false;
    }
    return true;
}

// Literal register numbers, typed again from the spec table so a typo in the header shows up here.
enum : uint32_t {
    GRBM_SOFT_RESET = 0x1260 + 0x0DA8,
    CP_MEC_CNTL = 0x1260 + 0x0E2D,
    CP_STAT = 0x1260 + 0x0F40,
    CP_ME_CNTL = 0x1260 + 0x0F56,
    RLC_ADDR = 0xA000 + 0x5B61,
    RLC_DATA = 0xA000 + 0x5B62,
    CPG_PSP = 0xA000 + 0x5C10,
    CPC_PSP = 0xA000 + 0x5C11,
    S0_CNTL = 0x1260 + 0x1C,
    S0_F32 = 0x1260 + 0x2A,
    S0_RB = 0x1260 + 0x80,
    S0_IB = 0x1260 + 0x8A,
    S1_CNTL = S0_CNTL + 0x600,
    S1_F32 = S0_F32 + 0x600,
    S1_RB = S0_RB + 0x600,
    S1_IB = S0_IB + 0x600,
    S0_UADDR = 0xA000 + 0x5880,
    S0_UDATA = 0xA000 + 0x5881,
    S1_UADDR = S0_UADDR + 0x20,
    S1_UDATA = S0_UDATA + 0x20,
};
struct Eng {
    uint32_t lo, hi, cntl, op, addr, data;
};
static const Eng PFP = { 0xA000 + 0x5840, 0xA000 + 0x5841, 0xA000 + 0x5842, 0xA000 + 0x5843, 0xA000 + 0x5814, 0xA000 + 0x5815 };
static const Eng ME = { 0xA000 + 0x5844, 0xA000 + 0x5845, 0xA000 + 0x5846, 0xA000 + 0x5847, 0xA000 + 0x5816, 0xA000 + 0x5817 };
static const Eng CE = { 0xA000 + 0x5848, 0xA000 + 0x5849, 0xA000 + 0x584A, 0xA000 + 0x584B, 0xA000 + 0x5818, 0xA000 + 0x5819 };
static const Eng MEC = { 0xA000 + 0x584C, 0xA000 + 0x584D, 0xA000 + 0x584E, 0xA000 + 0x584F, 0xA000 + 0x581A, 0xA000 + 0x581B };

// SDMA stop and halt (quiesce step 4) with every register starting at `init[...]`.
static void expectSdmaStop(Expect& e, uint32_t rb0, uint32_t ib0, uint32_t rb1, uint32_t ib1, uint32_t f0, uint32_t f1)
{
    e.R(S0_RB, rb0).W(S0_RB, rb0 & ~1u).R(S0_IB, ib0).W(S0_IB, ib0 & ~1u);
    e.R(S1_RB, rb1).W(S1_RB, rb1 & ~1u).R(S1_IB, ib1).W(S1_IB, ib1 & ~1u);
    e.R(S0_F32, f0).W(S0_F32, f0 | 1).R(S1_F32, f1).W(S1_F32, f1 | 1);
}

// ---------------------------------------------------------------------------------------------------------------
// parse()

struct Fw {
    std::vector<uint8_t> file;
    Image im;
};

static std::vector<uint8_t> simpleFile(size_t codeBytes, uint32_t seed, uint32_t version, uint32_t jtOffDw = 0, uint32_t jtDw = 0)
{
    FileSpec fs;
    fs.version = version;
    fs.jtOffsetDw = jtOffDw;
    fs.jtSizeDw = jtDw;
    return makeFile(box(pattern(codeBytes, seed)), fs);
}

// MEC: [box(code)][box(jt)], jt_offset = 0x200 + code bytes (in dwords), jt_size header field `hdrJt`.
static std::vector<uint8_t> mecFile(size_t codeBytes, size_t jtBytes, uint32_t version, uint32_t hdrJt)
{
    std::vector<uint8_t> payload = box(pattern(codeBytes, 0x30000000));
    std::vector<uint8_t> second = box(pattern(jtBytes, 0x3F000000));
    payload.insert(payload.end(), second.begin(), second.end());
    FileSpec fs;
    fs.version = version;
    fs.jtOffsetDw = (uint32_t)((0x200 + codeBytes) / 4);
    fs.jtSizeDw = hdrJt;
    return makeFile(payload, fs);
}

static void testParse()
{
    Image im;
    // Good RLC / SDMA.
    {
        std::vector<uint8_t> f = simpleFile(0x400, 0x10000000, 0x77);
        CHECK(parse(f.data(), f.size(), Kind::Rlc, im));
        CHECK(im.code == f.data() + 0x100 + 0x100);
        CHECK(im.codeBytes == 0x400);
        CHECK(im.version == 0x77);
        CHECK(im.jt == nullptr && im.jtDwords == 0 && im.jtDwordsHdr == 0);
        CHECK(rd32(im.code) == 0x10000000);
        CHECK(parse(f.data(), f.size(), Kind::Sdma, im));
    }
    // Good CP gfx with jump table inside the code.
    {
        std::vector<uint8_t> f = simpleFile(0x800, 0x20000000, 0x99, 0x100, 0x40);
        CHECK(parse(f.data(), f.size(), Kind::CpGfx, im));
        CHECK(im.codeBytes == 0x800);
        CHECK(im.jt == im.code + 0x400 && im.jtDwords == 0x40 && im.jtDwordsHdr == 0x40);
        // Jump table ending exactly at the end of the code is fine; one dword more is not.
        std::vector<uint8_t> g = simpleFile(0x800, 0x20000000, 0x99, 0x1C0, 0x40);
        CHECK(parse(g.data(), g.size(), Kind::CpGfx, im));
        g = simpleFile(0x800, 0x20000000, 0x99, 0x1C0, 0x41);
        CHECK(!parse(g.data(), g.size(), Kind::CpGfx, im));
        g = simpleFile(0x800, 0x20000000, 0x99, 0xFFFFFFFF, 0x2);
        CHECK(!parse(g.data(), g.size(), Kind::CpGfx, im));
        // Empty jump table (jt_size 0) is refused wherever it points.
        g = simpleFile(0x800, 0x20000000, 0x99, 0x100, 0);
        CHECK(!parse(g.data(), g.size(), Kind::CpGfx, im));
        g = simpleFile(0x800, 0x20000000, 0x99, 0, 0);
        CHECK(!parse(g.data(), g.size(), Kind::CpGfx, im));
        // One-dword jump table is the smallest accepted.
        g = simpleFile(0x800, 0x20000000, 0x99, 0x1FF, 1);
        CHECK(parse(g.data(), g.size(), Kind::CpGfx, im) && im.jtDwords == 1);
        // The same empty-jt file is still a fine RLC/SDMA image (they have no jump table).
        g = simpleFile(0x800, 0x20000000, 0x99, 0, 0);
        CHECK(parse(g.data(), g.size(), Kind::Rlc, im) && parse(g.data(), g.size(), Kind::Sdma, im));
    }
    // Good MEC (two containers).
    {
        std::vector<uint8_t> f = mecFile(0x1000, 0x80, 0xAB, 0x7FF);
        CHECK(parse(f.data(), f.size(), Kind::CpMec, im));
        const uint8_t* payload = f.data() + 0x100;
        CHECK(im.code == payload + 0x100);
        CHECK(im.codeBytes == 0x1000 + 0x200 + 0x80);
        CHECK(im.jt == payload + 0x200 + 0x1000 + 0x100);
        CHECK(im.jt == im.code + (0x200 + 0x1000));
        CHECK(im.jtDwords == 0x20);
        CHECK(im.jtDwordsHdr == 0x7FF);
        CHECK(im.version == 0xAB);
        CHECK(rd32(im.jt) == 0x3F000000);
        // A MEC file is not a valid single-container image (inner size != payload - 0x200).
        CHECK(!parse(f.data(), f.size(), Kind::CpGfx, im));
        CHECK(!parse(f.data(), f.size(), Kind::Rlc, im));
    }
    // Bad MEC variants.
    {
        std::vector<uint8_t> f = mecFile(0x1000, 0x80, 0xAB, 0x20);
        std::vector<uint8_t> g = f;
        put32(g, 0x100 + 0x200 + 0x1000 + 0x10, 0x31535025); // second magic
        CHECK(!parse(g.data(), g.size(), Kind::CpMec, im));
        g = f;
        put32(g, 0x24, (0x200 + 0x1000) / 4 + 1); // jt_offset mismatch
        CHECK(!parse(g.data(), g.size(), Kind::CpMec, im));
        g = f;
        put32(g, 0x100 + 0x200 + 0x1000 + 0x14, 0x84); // second inner size too big for the payload
        CHECK(!parse(g.data(), g.size(), Kind::CpMec, im));
        g = f;
        put32(g, 0x100 + 0x200 + 0x1000 + 0x14, 0x7C); // second inner size too small (payload has slack)
        CHECK(!parse(g.data(), g.size(), Kind::CpMec, im));
        g = f;
        put32(g, 0x100 + 0x14, 0x100000); // first inner size runs past the payload
        CHECK(!parse(g.data(), g.size(), Kind::CpMec, im));
        g = f;
        put32(g, 0x100 + 0x10, 0); // first magic
        CHECK(!parse(g.data(), g.size(), Kind::CpMec, im));
        // Second container with an empty jump table (inner size 0), otherwise consistent.
        g = mecFile(0x1000, 0, 0xAB, 0);
        CHECK(!parse(g.data(), g.size(), Kind::CpMec, im));
        // Smallest accepted second container: one dword.
        g = mecFile(0x1000, 4, 0xAB, 1);
        CHECK(parse(g.data(), g.size(), Kind::CpMec, im) && im.jtDwords == 1);
    }
    // Bad common-header cases.
    {
        std::vector<uint8_t> f = simpleFile(0x400, 1, 1);
        std::vector<uint8_t> g = f;
        CHECK(!parse(g.data(), g.size() - 4, Kind::Rlc, im)); // size field != file size
        put32(g, 0x00, (uint32_t)g.size() + 4);
        CHECK(!parse(g.data(), g.size(), Kind::Rlc, im));
        g = f;
        put32(g, 0x14, 0x602); // payload not a multiple of 4
        CHECK(!parse(g.data(), g.size(), Kind::Rlc, im));
        g = f;
        put32(g, 0x18, 0x104); // payload runs past the end
        CHECK(!parse(g.data(), g.size(), Kind::Rlc, im));
        g = f;
        put32(g, 0x18, 0xFFFFFF00); // offset overflow
        CHECK(!parse(g.data(), g.size(), Kind::Rlc, im));
        g = f;
        put32(g, 0x100 + 0x10, 0x31535024 ^ 1); // magic
        CHECK(!parse(g.data(), g.size(), Kind::Rlc, im));
        g = makeFile(box(pattern(0x400, 1), 0x31535024, 4), FileSpec()); // inner size != payload - 0x200
        CHECK(!parse(g.data(), g.size(), Kind::Sdma, im));
        g = makeFile(box({}), FileSpec()); // payload exactly 0x200: not larger
        CHECK(!parse(g.data(), g.size(), Kind::Rlc, im));
        std::vector<uint8_t> tiny(0x2B, 0);
        put32(tiny, 0, 0x2B);
        CHECK(!parse(tiny.data(), tiny.size(), Kind::Rlc, im));
        CHECK(!parse(nullptr, 0, Kind::Rlc, im));
        // Smallest acceptable: payload 0x204, code 4 bytes, header-only file of 0x2C + payload.
        FileSpec fs;
        fs.payloadAt = 0x2C;
        g = makeFile(box(pattern(4, 9)), fs);
        CHECK(parse(g.data(), g.size(), Kind::Rlc, im) && im.codeBytes == 4);
    }
    // A failed parse leaves `out` all zero.
    {
        std::vector<uint8_t> good = simpleFile(0x400, 1, 1, 0, 1);
        Image old;
        CHECK(parse(good.data(), good.size(), Kind::CpGfx, old) && old.code && old.jt);
        std::vector<uint8_t> f = good;
        put32(f, 0x100 + 0x10, 0);
        CHECK(!parse(f.data(), f.size(), Kind::CpGfx, old));
        CHECK(!old.code && !old.codeBytes && !old.jt && !old.jtDwords && !old.jtDwordsHdr && !old.version);
        old.version = 7;
        CHECK(!parse(nullptr, 0, Kind::Rlc, old) && old.version == 0);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Sequences

static void testSnapshot()
{
    Sim s;
    Io io = ioFor(s);
    s.regs[0xA000 + 0x4C00] = 0x11;
    s.regs[S1_RB] = 0x2222;
    s.regs[MEC.cntl] = 0x33;
    Snapshot snap;
    snapshot(io, snap);
    CHECK(snap.rlcCntl == 0x11 && snap.sdmaRbCntl[1] == 0x2222 && snap.cpcIcBaseCntl == 0x33);
    bool allReads = true;
    for (const Access& a : s.log) allReads &= a.op == 'R';
    CHECK(allReads);
    CHECK(s.log.size() == 28);
}

static void testDisableGpa()
{
    Sim s;
    Io io = ioFor(s);
    s.regs[CPC_PSP] = 0x1;
    s.regs[CPG_PSP] = 0x100;
    disableGpaMode(io);
    Expect e;
    e.R(CPC_PSP, 0x1).W(CPC_PSP, 0x9).R(CPG_PSP, 0x100).W(CPG_PSP, 0x108);
    CHECK(sameLog(s.log, e.a, "disableGpaMode"));
}

static void testQuiesce(bool reset)
{
    Sim s;
    Io io = ioFor(s);
    s.cpStatBusyReads = 2;
    s.regs[CP_ME_CNTL] = 0x00000003;
    s.regs[S0_CNTL] = 0x00040001;
    s.regs[S1_CNTL] = 0x000C0000;
    s.regs[S0_RB] = 0x101;
    s.regs[S0_IB] = 0x1;
    s.regs[S1_RB] = 0x0;
    s.regs[S1_IB] = 0x3;
    s.regs[S0_F32] = 0x4000;
    s.regs[S1_F32] = 0x1;
    s.regs[GRBM_SOFT_RESET] = 0x10;
    quiesce(io, reset);

    Expect e;
    e.R(CP_ME_CNTL, 0x3).W(CP_ME_CNTL, 0x15000003);
    e.R(CP_STAT, 0x80008000).D(1).R(CP_STAT, 0x80008000).D(1).R(CP_STAT, 0);
    e.W(CP_MEC_CNTL, 0x50000000).D(50);
    e.R(S0_CNTL, 0x00040001).W(S0_CNTL, 0x1).R(S1_CNTL, 0x000C0000).W(S1_CNTL, 0x00080000);
    expectSdmaStop(e, 0x101, 0x1, 0x0, 0x3, 0x4000, 0x1);
    if (reset) {
        e.R(GRBM_SOFT_RESET, 0x10).W(GRBM_SOFT_RESET, 0x00800010).R(GRBM_SOFT_RESET, 0x00800010).D(50);
        e.W(GRBM_SOFT_RESET, 0x10).R(GRBM_SOFT_RESET, 0x10);
        e.R(GRBM_SOFT_RESET, 0x10).W(GRBM_SOFT_RESET, 0x01000010).R(GRBM_SOFT_RESET, 0x01000010).D(50);
        e.W(GRBM_SOFT_RESET, 0x10).R(GRBM_SOFT_RESET, 0x10);
        expectSdmaStop(e, 0x100, 0x0, 0x0, 0x2, 0x4001, 0x1);
    }
    CHECK(sameLog(s.log, e.a, reset ? "quiesce(reset)" : "quiesce"));
    // Nothing touches the RLC.
    bool rlc = false;
    for (const Access& a : s.log) rlc |= a.reg == 0xA000 + 0x4C00;
    CHECK(!rlc);
}

static void testQuiesceHaltTimeout()
{
    Sim s;
    Io io = ioFor(s);
    s.cpStatBusyReads = 1 << 30;
    quiesce(io, false);
    size_t reads = 0, delays = 0;
    for (const Access& a : s.log) {
        if (a.op == 'R' && a.reg == CP_STAT) ++reads;
        if (a.op == 'D' && a.value == 1) ++delays;
    }
    CHECK(reads == kHaltTimeoutUs && delays == kHaltTimeoutUs); // timeout rounds of read + 1 us
    CHECK(s.log[2].op == 'R' && s.log[2].reg == CP_STAT && s.log[3].op == 'D');
    CHECK(s.regs[CP_MEC_CNTL] == 0x50000000); // carried on after the timeout
}

static void testLoadRlc()
{
    std::vector<uint8_t> f = simpleFile(0x10, 0xA0000000, 0x5555);
    Image im;
    CHECK(parse(f.data(), f.size(), Kind::Rlc, im));
    Sim s;
    Io io = ioFor(s);
    Report r = loadRlc(io, im);
    CHECK(r.status == Status::Ok && r.step == 0 && r.dataWrites == 4);
    Expect e;
    e.W(RLC_ADDR, 0x2000).W(RLC_DATA, 0xA0000000).W(RLC_DATA, 0xA1010101).W(RLC_DATA, 0xA2020202).W(RLC_DATA, 0xA3030303);
    e.W(RLC_ADDR, 0x5555);
    CHECK(sameLog(s.log, e.a, "loadRlc"));

    Sim s2;
    Io io2 = ioFor(s2);
    Image bad = {};
    Report rb = loadRlc(io2, bad);
    CHECK(rb.status == Status::BadImage && rb.step == 0 && s2.log.empty());
}

static void testLoadSdma()
{
    std::vector<uint8_t> f0 = simpleFile(0x8, 0xB0000000, 0x51);
    std::vector<uint8_t> f1 = simpleFile(0xC, 0xC0000000, 0x52);
    Image a, b;
    CHECK(parse(f0.data(), f0.size(), Kind::Sdma, a));
    CHECK(parse(f1.data(), f1.size(), Kind::Sdma, b));
    Sim s;
    Io io = ioFor(s);
    s.regs[S0_RB] = 0x1;
    s.regs[S1_IB] = 0x1;
    Report r = loadSdma(io, a, b);
    CHECK(r.status == Status::Ok && r.step == 0 && r.dataWrites == 5);
    Expect e;
    expectSdmaStop(e, 0x1, 0, 0, 0x1, 0, 0);
    e.W(S0_UADDR, 0).W(S0_UDATA, 0xB0000000).W(S0_UDATA, 0xB1010101).W(S0_UADDR, 0x51);
    e.W(S1_UADDR, 0).W(S1_UDATA, 0xC0000000).W(S1_UDATA, 0xC1010101).W(S1_UDATA, 0xC2020202).W(S1_UADDR, 0x52);
    CHECK(sameLog(s.log, e.a, "loadSdma"));

    Sim s2;
    Io io2 = ioFor(s2);
    Image bad = {};
    Report r2 = loadSdma(io2, a, bad);
    CHECK(r2.status == Status::BadImage && r2.step == 0 && s2.log.empty());
}

struct CpSet {
    std::vector<uint8_t> files[4];
    CpImages im;
};

static void makeCp(CpSet& c)
{
    // PFP 0x1100 bytes (rounds to 0x2000), CE 0x800, ME 0x1000, MEC 0x1000 code + 0x10 jt.
    c.files[0] = simpleFile(0x1100, 0x01000000, 0x101, 0x400, 2);
    c.files[1] = simpleFile(0x800, 0x02000000, 0x102, 0x10, 1);
    c.files[2] = simpleFile(0x1000, 0x03000000, 0x103, 0x0, 3);
    c.files[3] = mecFile(0x1000, 0x10, 0x104, 0x99);
    CHECK(parse(c.files[0].data(), c.files[0].size(), Kind::CpGfx, c.im.pfp));
    CHECK(parse(c.files[1].data(), c.files[1].size(), Kind::CpGfx, c.im.ce));
    CHECK(parse(c.files[2].data(), c.files[2].size(), Kind::CpGfx, c.im.me));
    CHECK(parse(c.files[3].data(), c.files[3].size(), Kind::CpMec, c.im.mec));
}

static void expectEngine(Expect& e, const Eng& g, uint32_t opInit, uint32_t cntlInit, uint32_t cntlNew, bool writeCntl, uint64_t addr,
                         const Image& im, int icReadsBeforeDone)
{
    e.R(g.op, opInit).W(g.op, opInit | 1);
    for (int i = 0; i < icReadsBeforeDone; ++i) e.R(g.op, opInit | 1).D(1);
    e.R(g.op, opInit | 3);
    e.R(g.cntl, cntlInit);
    if (writeCntl) e.W(g.cntl, cntlNew);
    e.W(g.lo, (uint32_t)addr & 0xFFFFF000u).W(g.hi, (uint32_t)(addr >> 32));
    e.W(g.addr, 0);
    for (uint32_t i = 0; i < im.jtDwords; ++i) e.W(g.data, rd32(im.jt + 4 * i));
    e.W(g.addr, im.version);
}

static void testCp()
{
    CpSet c;
    makeCp(c);
    CHECK(storeBytesNeeded(c.im) == 0x2000 + 0x1000 + 0x1000 + 0x2000); // MEC code 0x1210 rounds to 0x2000

    std::vector<uint8_t> mem(0x8000, 0xCC);
    Store st = { mem.data(), 0x0000008012340000ull, mem.size() };
    CHECK(stageCp(c.im, st) == Status::Ok);
    CHECK(memcmp(mem.data(), c.im.pfp.code, c.im.pfp.codeBytes) == 0);
    CHECK(memcmp(mem.data() + 0x2000, c.im.ce.code, c.im.ce.codeBytes) == 0);
    CHECK(memcmp(mem.data() + 0x3000, c.im.me.code, c.im.me.codeBytes) == 0);
    CHECK(memcmp(mem.data() + 0x4000, c.im.mec.code, c.im.mec.codeBytes) == 0);
    CHECK(mem[0x1100] == 0xCC); // padding untouched
    CHECK(storeIntact(c.im, st));
    mem[0x3004] ^= 1;
    CHECK(!storeIntact(c.im, st));

    // Store errors, all without register access (stageCp has no Io at all; loadCp must not touch registers).
    Store small = st;
    small.bytes = 0x5FFF;
    CHECK(stageCp(c.im, small) == Status::StoreTooSmall);
    Store unaligned = st;
    unaligned.gpu += 0x800;
    CHECK(stageCp(c.im, unaligned) == Status::StoreTooSmall);
    Store noCpu = st;
    noCpu.cpu = nullptr;
    CHECK(stageCp(c.im, noCpu) == Status::StoreTooSmall);
    CHECK(!storeIntact(c.im, unaligned) && !storeIntact(c.im, noCpu));
    Store exact = st;
    exact.bytes = 0x6000;
    CHECK(stageCp(c.im, exact) == Status::Ok);
    CpImages badIm = c.im;
    badIm.ce.code = nullptr;
    CHECK(stageCp(badIm, st) == Status::BadImage);
    // A CP image with an empty or missing jump table is not usable, for any of the four engines.
    for (int which = 0; which < 4; ++which) {
        for (int kind = 0; kind < 2; ++kind) {
            CpImages e = c.im;
            Image* im[4] = { &e.pfp, &e.ce, &e.me, &e.mec };
            if (kind == 0) im[which]->jtDwords = 0;
            else im[which]->jt = nullptr;
            CHECK(stageCp(e, st) == Status::BadImage);
            CHECK(!storeIntact(e, st));
            Sim s;
            Io io = ioFor(s);
            Report r = loadCp(io, e, st);
            CHECK(r.status == Status::BadImage && r.step == 0 && s.log.empty());
        }
    }
    {
        Sim s;
        Io io = ioFor(s);
        CHECK(loadCp(io, c.im, small).status == Status::StoreTooSmall && s.log.empty());
        Report ru = loadCp(io, c.im, unaligned);
        CHECK(ru.status == Status::StoreTooSmall && ru.step == 0 && s.log.empty());
        CHECK(loadCp(io, c.im, noCpu).status == Status::StoreTooSmall && s.log.empty());
        CHECK(loadCp(io, badIm, st).status == Status::BadImage && s.log.empty());
    }

    // Full load.
    Sim s;
    Io io = ioFor(s);
    s.cpStatBusyReads = 3;
    s.icDoneAfterReads = 2;
    s.regs[CP_ME_CNTL] = 0x00000100;
    s.regs[PFP.op] = 0x0;
    s.regs[CE.op] = 0x10;
    s.regs[PFP.cntl] = 0x0380000F; // VMID 0xF, cache policy, exe disable
    s.regs[CE.cntl] = 0x03800005;
    s.regs[ME.cntl] = 0x00000003;
    s.regs[MEC.cntl] = 0x0280000A;
    std::fill(mem.begin(), mem.end(), 0);
    Report r = loadCp(io, c.im, st);
    CHECK(r.status == Status::Ok && r.step == 0);
    CHECK(r.cpStatAfterHalt == 0 && r.haltWaitUs == 3);
    CHECK(r.icacheWaitUs[0] == 2 && r.icacheWaitUs[1] == 2 && r.icacheWaitUs[2] == 2 && r.icacheWaitUs[3] == 2);
    CHECK(r.dataWrites == 2 + 1 + 3 + 4);
    CHECK(storeIntact(c.im, st));

    const uint64_t g = st.gpu;
    Expect e;
    e.R(CP_ME_CNTL, 0x100).W(CP_ME_CNTL, 0x15000100);
    e.R(CP_STAT, 0x80008000).D(1).R(CP_STAT, 0x80008000).D(1).R(CP_STAT, 0x80008000).D(1).R(CP_STAT, 0);
    expectEngine(e, PFP, 0x0, 0x0380000F, 0x00000010, true, g + 0x0000, c.im.pfp, 2);
    expectEngine(e, CE, 0x10, 0x03800005, 0, false, g + 0x2000, c.im.ce, 2);
    expectEngine(e, ME, 0x0, 0x00000003, 0, false, g + 0x3000, c.im.me, 2);
    e.W(CP_MEC_CNTL, 0x50000000).D(50);
    expectEngine(e, MEC, 0x0, 0x0280000A, 0x0000001A, true, g + 0x4000, c.im.mec, 2);
    CHECK(sameLog(s.log, e.a, "loadCp"));
    // Jump tables come from the right place: MEC's is the second container.
    CHECK(rd32(c.im.mec.jt) == 0x3F000000 && c.im.mec.jtDwords == 4);
    CHECK(rd32(c.im.pfp.jt) == 0x01000000 + 0x400 * 0x01010101u);

    // High GPU address bits go to IC_BASE_HI, and LO is masked to 4 KiB.
    CHECK(s.regs[MEC.hi] == 0x80 && s.regs[MEC.lo] == 0x12344000);

    // Icache timeout on PFP: step 1, nothing after it.
    Sim t;
    Io iot = ioFor(t);
    t.icDoneAfterReads = -1;
    Report rt = loadCp(iot, c.im, st);
    CHECK(rt.status == Status::IcacheTimeout && rt.step == 1);
    CHECK(rt.icacheWaitUs[0] == kIcacheTimeoutUs);
    size_t icReads = 0;
    for (const Access& a : t.log) icReads += a.op == 'R' && a.reg == PFP.op;
    CHECK(icReads == 1 + kIcacheTimeoutUs); // the read-modify-write's read, then timeout polling reads
    CHECK(t.log.back().op == 'D' && t.log[t.log.size() - 2].op == 'R' && t.log[t.log.size() - 2].reg == PFP.op);

    // Timeout on MEC only.
    Sim m;
    Io iom = ioFor(m);
    // Done bit for PFP/CE/ME at once, never for MEC: pre-arm by forcing MEC's pending counter negative via its write.
    struct MecNever {
        static void write(void* ctx, uint32_t reg, uint32_t value)
        {
            Sim& s = *(Sim*)ctx;
            simWrite(ctx, reg, value);
            if (reg == Reg::Mec.icOpCntl) s.icPending[reg] = -1;
        }
    };
    iom.write = MecNever::write;
    m.icDoneAfterReads = 0;
    Report rm = loadCp(iom, c.im, st);
    CHECK(rm.status == Status::IcacheTimeout && rm.step == 4 && rm.dataWrites == 6);
}

int main()
{
    testParse();
    testSnapshot();
    testDisableGpa();
    testQuiesce(false);
    testQuiesce(true);
    testQuiesceHaltTimeout();
    testLoadRlc();
    testLoadSdma();
    testCp();
    printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
