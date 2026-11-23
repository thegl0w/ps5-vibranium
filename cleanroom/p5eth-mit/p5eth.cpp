// SPDX-License-Identifier: MIT
//
// p5eth.sys - NDIS 6.30 miniport for the PS5 wired Ethernet controller
// (Sony "Salina" gigabit MAC, PCI 40:00.2, 104d:9104).
//
// Copyright (c) 2026 ps5win contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Design (clean-room, written from windows-handoff/cleanroom/spec-p5eth.md):
//  * Root-enumerated miniport (root\p5eth). The controller's bus (0x40) is not
//    visible to Windows, so the chip is reached at fixed physical addresses and
//    the upstream root port (00:01.1) is checked/fixed via config mechanism 1.
//  * SAFETY: bus 0x40 also hosts the console's internal NVMe at 40:00.0. The
//    only bus-0x40 config accesses this driver can make are to function 2
//    (enforced in CfgAllowed()), and the only MMIO it maps is the Ethernet BAR0
//    page and the MAC-address page of the glue function.
//  * No interrupt: one system thread polls link, RX, TX and TX completion.

#include <ndis.h>
#include <intrin.h>

// Exported by ntoskrnl; declared in ntifs.h, which this driver does not include.
extern "C" NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);

// ---------------------------------------------------------------------------
// Constants from the spec
// ---------------------------------------------------------------------------

#define P5_TAG                  '5hte'      // shows as "eth5" in pool tools

// Root port 00:01.1 (1022:13DF), forwards bus 0x40.
#define RP_BUS                  0
#define RP_DEV                  1
#define RP_FN                   1
#define RP_WANT_BUSNUM          0x00404000u // reg 0x18, low 24 bits
#define RP_WANT_MEMWIN          0xC4308000u // reg 0x20
#define RP_WANT_CMDBITS         0x0007u     // I/O, memory, bus master

// Ethernet function 40:00.2.
#define ETH_BUS                 0x40
#define ETH_DEV                 0
#define ETH_FN                  2
#define ETH_ID                  0x9104104Du
#define ETH_BAR0_PHYS           0x85000000u
#define ETH_BAR0_SIZE           0x1000u

// MAC address bytes in the glue function's BAR4 (+0x3000).
#define MACROM_PHYS             0x85403000u

// Reserved RAM used for DMA (Ethernet's share).
#define DMA_PHYS                0x6F300000u
#define DMA_SIZE                0x00225000u
#define DMA_OFF_TXRING          0x00000000u
#define DMA_OFF_RXRING          0x00001000u
#define DMA_OFF_TXBUF           0x00005000u
#define DMA_OFF_RXBUF           0x000A5000u

#define TX_COUNT                256u
#define RX_COUNT                1024u
#define TX_SLOT                 0x800u      // 128-byte aligned, 256 * 0x800 = 0x80000 <= 0xA0000
#define RX_BUFSZ                0x600u      // 1024 * 0x600 = 0x180000
#define FRAME_MIN               60u
#define FRAME_MAX_IND           1514u
#define FRAME_MAX_HW            0x600u

C_ASSERT(TX_COUNT * 16 <= DMA_OFF_RXRING - DMA_OFF_TXRING);
C_ASSERT(RX_COUNT * 16 <= DMA_OFF_TXBUF - DMA_OFF_RXRING);
C_ASSERT(TX_COUNT * TX_SLOT <= DMA_OFF_RXBUF - DMA_OFF_TXBUF);
C_ASSERT(DMA_OFF_RXBUF + RX_COUNT * RX_BUFSZ <= DMA_SIZE);

// Register offsets in BAR0.
#define R_LINK                  0x004
#define R_CFG08                 0x008
#define R_CFG0C                 0x00C
#define R_RXCFG                 0x010
#define R_MACHI                 0x014
#define R_MACLO                 0x018
#define R_MCHASH                0x01C
#define R_CFG30                 0x030
#define R_TXCTL                 0x034
#define R_RXCTL                 0x038
#define R_TXBASE_A              0x03C
#define R_RXBASE_A              0x040
#define R_TXBASE_B              0x044
#define R_RXBASE_B              0x048
#define R_ISR                   0x050
#define R_IMR                   0x054
#define R_CFG74                 0x074
#define R_CFG9C                 0x09C
#define R_MACRESET              0x0AC
#define R_STAT0                 0x110
#define R_STAT1                 0x114
#define R_COALESCE              0x204

#define CTL_ENABLE              0x1u
#define CTL_STOP                0x2u
#define CTL_KICK                0x4u
#define IMR_ALL                 0x007FFFFFu
#define ISR_RX_STARVED          0x22u       // bits 1 and 5
#define RXCFG_PROMISC           0x10u

#define DESC_OWN_DONE           0x80000000u // bit 31
#define DESC_WRAP               0x40000000u // bit 30
#define TXD_FLAGS               0x30000000u
#define TXD_W2_SUBMIT           4u
#define TXD_W2_RECLAIMED        0xFFFF0000u
#define RXD_LEN_MASK            0x7FFu

// Windows-side numbers.
#define P5_MTU                  1500u
#define P5_LINK_SPEED_BPS       1000000000ull
#define P5_MCAST_MAX            32u
#define P5_RX_PER_PASS          256u
#define P5_LINK_POLL_100NS      (200 * 10000)     // 200 ms
#define P5_DIAG_POLL_100NS      (5000 * 10000)    // 5 s

#define P5_PACKET_FILTERS (NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | \
                           NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST | \
                           NDIS_PACKET_TYPE_PROMISCUOUS)

// ---------------------------------------------------------------------------
// Adapter context
// ---------------------------------------------------------------------------

struct P5Desc {
    volatile ULONG w0;
    volatile ULONG w1;
    volatile ULONG w2;
    volatile ULONG w3;
};
C_ASSERT(sizeof(P5Desc) == 16);

enum P5RunState : LONG {
    RunPaused = 0,
    RunRunning = 1,
    RunPausing = 2,
};

struct P5TxInfo {
    USHORT Length;
    UCHAR Kind;     // 0 unicast, 1 multicast, 2 broadcast
};

struct P5Adapter {
    NDIS_HANDLE MiniportHandle;

    // Root port bookkeeping.
    BOOLEAN RootPortChanged;
    ULONG RpOrigBusNum;     // reg 0x18
    ULONG RpOrigMemWin;     // reg 0x20
    USHORT RpOrigCommand;   // reg 0x04 low 16

    // Mappings.
    volatile UCHAR *Regs;
    PUCHAR Dma;
    P5Desc *TxRing;
    P5Desc *RxRing;
    PUCHAR TxBuf;
    PUCHAR RxBuf;
    ULONG DmaPhys;

    UCHAR PermanentMac[6];
    UCHAR CurrentMac[6];

    // Ring indices (thread-owned).
    ULONG TxHead;           // next descriptor to fill
    ULONG TxTail;           // oldest not yet reclaimed
    ULONG RxNext;
    P5TxInfo TxInfo[TX_COUNT];

    // Receive indication resources.
    NDIS_HANDLE NblPool;
    PNET_BUFFER_LIST RxNbl;
    PMDL RxMdl;
    PUCHAR RxCopy;

    // Send queue.
    NDIS_SPIN_LOCK QueueLock;
    BOOLEAN QueueLockInit;
    PNET_BUFFER_LIST QHead;
    PNET_BUFFER_LIST QTail;

    // State.
    volatile LONG RunState;
    volatile LONG Busy;         // thread work that must finish before pause completes
    volatile LONG LinkUp;
    volatile LONG Ready;        // InitializeEx finished
    ULONG PacketFilter;
    ULONG Lookahead;
    ULONG McastCount;
    UCHAR McastList[P5_MCAST_MAX][6];

    // Worker thread.
    KEVENT WakeEvent;
    PKTHREAD Thread;
    volatile LONG StopThread;

    // Statistics.
    ULONG64 RxFrames, RxUcast, RxMcast, RxBcast, RxBytes, RxErrors, RxDiscards;
    ULONG64 TxFrames, TxUcast, TxMcast, TxBcast, TxBytes;
    volatile LONG64 TxErrors, TxDiscards;
};

static NDIS_HANDLE g_DriverHandle;

static const NDIS_OID g_SupportedOids[] = {
    OID_GEN_SUPPORTED_LIST,
    OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,
    OID_GEN_PHYSICAL_MEDIUM,
    OID_GEN_MAXIMUM_LOOKAHEAD,
    OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_VENDOR_DRIVER_VERSION,
    OID_GEN_DRIVER_VERSION,
    OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS,
    OID_GEN_MAXIMUM_SEND_PACKETS,
    OID_GEN_XMIT_OK,
    OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR,
    OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER,
    OID_GEN_STATISTICS,
    OID_GEN_INTERRUPT_MODERATION,
    OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_RCV_ERROR_ALIGNMENT,
    OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS,
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static inline ULONG RegRead(P5Adapter *a, ULONG off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(a->Regs + off));
}

static inline void RegWrite(P5Adapter *a, ULONG off, ULONG v)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(a->Regs + off), v);
}

static inline void RegOr(P5Adapter *a, ULONG off, ULONG bits)
{
    RegWrite(a, off, RegRead(a, off) | bits);
}

static inline void RegAndNot(P5Adapter *a, ULONG off, ULONG bits)
{
    RegWrite(a, off, RegRead(a, off) & ~bits);
}

static void SleepMicro(ULONG us)
{
    if (KeGetCurrentIrql() <= APC_LEVEL) {
        NdisMSleep(us);
    } else {
        KeStallExecutionProcessor(us);
    }
}

// Poll a register until (value & mask) == want. stepUs/limitUs in microseconds.
static BOOLEAN PollReg(P5Adapter *a, ULONG off, ULONG mask, ULONG want, ULONG stepUs, ULONG limitUs, BOOLEAN mayStall)
{
    for (ULONG waited = 0;; waited += stepUs) {
        if ((RegRead(a, off) & mask) == want) {
            return TRUE;
        }
        if (waited >= limitUs) {
            return FALSE;
        }
        if (mayStall) {
            KeStallExecutionProcessor(stepUs);
        } else {
            SleepMicro(stepUs);
        }
    }
}

static void DiagDword(PCWSTR name, ULONG value)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"p5eth", name, REG_DWORD, &value, sizeof(value));
}

static void DiagString(PCWSTR name, PCWSTR text)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    SIZE_T chars = 0;
    while (text[chars] != 0) {
        chars++;
    }
    RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"p5eth", name, REG_SZ, (PVOID)text,
                          (ULONG)((chars + 1) * sizeof(WCHAR)));
}

static void DiagStage(ULONG stage, PCWSTR text)
{
    DiagDword(L"Stage", stage);
    DiagString(L"State", text);
}

// ---------------------------------------------------------------------------
// PCI configuration mechanism 1 (ports 0xCF8 / 0xCFC)
// ---------------------------------------------------------------------------

// Only the root port 00:01.1 and the Ethernet function 40:00.2 may be touched.
// In particular 40:00.0 (internal NVMe) is never addressed.
static BOOLEAN CfgAllowed(ULONG bus, ULONG dev, ULONG fn)
{
    if (bus == RP_BUS && dev == RP_DEV && fn == RP_FN) {
        return TRUE;
    }
    if (bus == ETH_BUS && dev == ETH_DEV && fn == ETH_FN) {
        return TRUE;
    }
    return FALSE;
}

// clang-cl has no _disable/_enable builtins; use cli/sti directly.
static inline void CpuIrqOff() { __asm__ __volatile__("cli" ::: "memory"); }
static inline void CpuIrqOn() { __asm__ __volatile__("sti" ::: "memory"); }

static inline ULONG CfgAddress(ULONG bus, ULONG dev, ULONG fn, ULONG reg)
{
    return 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xFCu);
}

static ULONG CfgRead32(ULONG bus, ULONG dev, ULONG fn, ULONG reg)
{
    if (!CfgAllowed(bus, dev, fn)) {
        return 0xFFFFFFFFu;
    }
    ULONG64 flags = __readeflags();
    CpuIrqOff();
    ULONG saved = __indword(0xCF8);
    __outdword(0xCF8, CfgAddress(bus, dev, fn, reg));
    ULONG v = __indword(0xCFC);
    __outdword(0xCF8, saved);
    if (flags & 0x200) {
        CpuIrqOn();
    }
    return v;
}

static void CfgWrite32(ULONG bus, ULONG dev, ULONG fn, ULONG reg, ULONG v)
{
    if (!CfgAllowed(bus, dev, fn)) {
        return;
    }
    ULONG64 flags = __readeflags();
    CpuIrqOff();
    ULONG saved = __indword(0xCF8);
    __outdword(0xCF8, CfgAddress(bus, dev, fn, reg));
    __outdword(0xCFC, v);
    __outdword(0xCF8, saved);
    if (flags & 0x200) {
        CpuIrqOn();
    }
}

static void CfgWrite16(ULONG bus, ULONG dev, ULONG fn, ULONG reg, USHORT v)
{
    if (!CfgAllowed(bus, dev, fn)) {
        return;
    }
    ULONG64 flags = __readeflags();
    CpuIrqOff();
    ULONG saved = __indword(0xCF8);
    __outdword(0xCF8, CfgAddress(bus, dev, fn, reg));
    __outword((USHORT)(0xCFC + (reg & 2)), v);
    __outdword(0xCF8, saved);
    if (flags & 0x200) {
        CpuIrqOn();
    }
}

static void RootPortPrepare(P5Adapter *a)
{
    ULONG busnum = CfgRead32(RP_BUS, RP_DEV, RP_FN, 0x18);
    ULONG memwin = CfgRead32(RP_BUS, RP_DEV, RP_FN, 0x20);
    USHORT cmd = (USHORT)(CfgRead32(RP_BUS, RP_DEV, RP_FN, 0x04) & 0xFFFF);

    DiagDword(L"RpBusNum", busnum);
    DiagDword(L"RpMemWin", memwin);
    DiagDword(L"RpCommand", cmd);

    BOOLEAN ok = ((busnum & 0x00FFFFFFu) == RP_WANT_BUSNUM) &&
                 (memwin == RP_WANT_MEMWIN) &&
                 ((cmd & RP_WANT_CMDBITS) == RP_WANT_CMDBITS);
    if (ok) {
        return;
    }

    a->RpOrigBusNum = busnum;
    a->RpOrigMemWin = memwin;
    a->RpOrigCommand = cmd;
    a->RootPortChanged = TRUE;

    CfgWrite32(RP_BUS, RP_DEV, RP_FN, 0x18, (busnum & 0xFF000000u) | RP_WANT_BUSNUM);
    CfgWrite32(RP_BUS, RP_DEV, RP_FN, 0x20, RP_WANT_MEMWIN);
    CfgWrite16(RP_BUS, RP_DEV, RP_FN, 0x04, (USHORT)(cmd | RP_WANT_CMDBITS));
    DiagDword(L"RpChanged", 1);
}

static void RootPortRestore(P5Adapter *a)
{
    if (!a->RootPortChanged) {
        return;
    }
    CfgWrite16(RP_BUS, RP_DEV, RP_FN, 0x04, a->RpOrigCommand);
    CfgWrite32(RP_BUS, RP_DEV, RP_FN, 0x20, a->RpOrigMemWin);
    CfgWrite32(RP_BUS, RP_DEV, RP_FN, 0x18, a->RpOrigBusNum);
    a->RootPortChanged = FALSE;
}

static NDIS_STATUS EthFunctionCheck()
{
    ULONG id = CfgRead32(ETH_BUS, ETH_DEV, ETH_FN, 0x00);
    ULONG bar0 = CfgRead32(ETH_BUS, ETH_DEV, ETH_FN, 0x10);
    DiagDword(L"PciId", id);
    DiagDword(L"PciBar0", bar0);
    if (id != ETH_ID || (bar0 & 0xFFFFF000u) != ETH_BAR0_PHYS) {
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }
    USHORT cmd = (USHORT)(CfgRead32(ETH_BUS, ETH_DEV, ETH_FN, 0x04) & 0xFFFF);
    if ((cmd & 0x6) != 0x6) {
        CfgWrite16(ETH_BUS, ETH_DEV, ETH_FN, 0x04, (USHORT)(cmd | 0x6));
    }
    return NDIS_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// MAC address
// ---------------------------------------------------------------------------

static BOOLEAN MacUsable(const UCHAR *m)
{
    if (m[0] & 1) {
        return FALSE;
    }
    BOOLEAN allZero = TRUE, allOnes = TRUE;
    for (int i = 0; i < 6; i++) {
        if (m[i] != 0x00) allZero = FALSE;
        if (m[i] != 0xFF) allOnes = FALSE;
    }
    return !(allZero || allOnes);
}

static void MacRandom(UCHAR *m)
{
    LARGE_INTEGER pc = KeQueryPerformanceCounter(nullptr);
    ULONG64 x = __rdtsc() ^ ((ULONG64)pc.QuadPart << 17) ^ KeQueryInterruptTime() ^ (ULONG64)(ULONG_PTR)m;
    // splitmix64 finaliser
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    m[0] = 0x02;
    for (int i = 1; i < 6; i++) {
        m[i] = (UCHAR)(x >> (8 * i));
    }
}

static void MacLoad(P5Adapter *a)
{
    UCHAR m[6] = {};
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = MACROM_PHYS;
    volatile UCHAR *rom = (volatile UCHAR *)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (rom) {
        for (int i = 0; i < 6; i++) {
            m[i] = READ_REGISTER_UCHAR((volatile UCHAR *)(rom + i));
        }
        MmUnmapIoSpace((PVOID)rom, PAGE_SIZE);
    }
    if (!MacUsable(m)) {
        MacRandom(m);
        DiagDword(L"MacRandom", 1);
    }
    RtlCopyMemory(a->PermanentMac, m, 6);
    RtlCopyMemory(a->CurrentMac, m, 6);
    DiagDword(L"MacHi", ((ULONG)m[0] << 8) | m[1]);
    DiagDword(L"MacLo", ((ULONG)m[2] << 24) | ((ULONG)m[3] << 16) | ((ULONG)m[4] << 8) | m[5]);
}

// ---------------------------------------------------------------------------
// Chip control
// ---------------------------------------------------------------------------

// Stop sequence; mayStall selects busy-wait (shutdown at raised IRQL).
static void ChipStop(P5Adapter *a, BOOLEAN mayStall)
{
    RegWrite(a, R_IMR, IMR_ALL);
    RegWrite(a, R_COALESCE, 0);
    RegOr(a, R_TXCTL, CTL_STOP);
    PollReg(a, R_TXCTL, CTL_STOP, 0, 1000, 200000, mayStall);
    RegOr(a, R_RXCTL, CTL_STOP);
    PollReg(a, R_RXCTL, CTL_STOP, 0, 1000, 200000, mayStall);
}

static inline ULONG RxW0Idle(ULONG index)
{
    return RX_BUFSZ | (index == RX_COUNT - 1 ? DESC_WRAP : 0);
}

static void RingsFill(P5Adapter *a)
{
    for (ULONG i = 0; i < TX_COUNT; i++) {
        P5Desc *d = &a->TxRing[i];
        d->w1 = 0;
        d->w2 = TXD_W2_RECLAIMED;
        d->w3 = 0;
        _WriteBarrier();
        d->w0 = DESC_OWN_DONE | (i == TX_COUNT - 1 ? DESC_WRAP : 0);
    }
    for (ULONG i = 0; i < RX_COUNT; i++) {
        P5Desc *d = &a->RxRing[i];
        d->w1 = a->DmaPhys + DMA_OFF_RXBUF + i * RX_BUFSZ;
        d->w2 = 0;
        d->w3 = 0;
        _WriteBarrier();
        d->w0 = RxW0Idle(i);
    }
    a->TxHead = 0;
    a->TxTail = 0;
    a->RxNext = 0;
}

static void MulticastAcceptAll(P5Adapter *a)
{
    RegAndNot(a, R_RXCFG, RXCFG_PROMISC);

    RegWrite(a, R_MCHASH, 0x80000000u);
    BOOLEAN ok = PollReg(a, R_MCHASH, 0x20000u, 0x20000u, 1, 10000, TRUE);
    ULONG failed = ok ? 0 : 0x100;

    for (ULONG e = 0; e < 256; e++) {
        RegWrite(a, R_MCHASH, e | 0x7000u);
        if (!PollReg(a, R_MCHASH, 0x4000u, 0, 1, 10000, TRUE)) {
            failed++;
        }
    }
    DiagDword(L"McastFail", failed);
}

static void ChipStart(P5Adapter *a)
{
    ChipStop(a, FALSE);

    RegWrite(a, R_MACRESET, 9);
    SleepMicro(2000);

    RegOr(a, R_CFG08, 0x07597C00u);
    RegAndNot(a, R_CFG0C, 0x80u);
    RegWrite(a, R_CFG74, 0x00303277u);
    RegWrite(a, R_RXCFG, (RegRead(a, R_RXCFG) & 0xFFFFFF6Eu) | 0x81u);
    RegWrite(a, R_CFG30, 0x00010100u);

    const UCHAR *m = a->CurrentMac;
    RegWrite(a, R_MACHI, ((ULONG)m[0] << 8) | m[1]);
    RegWrite(a, R_MACLO, ((ULONG)m[2] << 24) | ((ULONG)m[3] << 16) | ((ULONG)m[4] << 8) | m[5]);

    RegWrite(a, R_COALESCE, 0);
    (void)RegRead(a, R_STAT0);
    (void)RegRead(a, R_STAT1);

    RingsFill(a);
    _mm_sfence();

    ULONG tx = a->DmaPhys + DMA_OFF_TXRING;
    ULONG rx = a->DmaPhys + DMA_OFF_RXRING;
    RegWrite(a, R_TXBASE_B, tx);
    RegWrite(a, R_TXBASE_A, tx);
    RegWrite(a, R_RXBASE_B, rx);
    RegWrite(a, R_RXBASE_A, rx);

    RegAndNot(a, R_CFG9C, 0x40u);
    RegOr(a, R_CFG9C, 0x40u);

    RegOr(a, R_TXCTL, CTL_ENABLE);
    RegOr(a, R_RXCTL, CTL_ENABLE);

    RegWrite(a, R_ISR, RegRead(a, R_ISR));
    RegWrite(a, R_IMR, IMR_ALL);

    MulticastAcceptAll(a);
}

// ---------------------------------------------------------------------------
// Link state
// ---------------------------------------------------------------------------

static void LinkIndicate(P5Adapter *a, BOOLEAN up)
{
    NDIS_LINK_STATE ls;
    NdisZeroMemory(&ls, sizeof(ls));
    ls.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    ls.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    ls.Header.Size = NDIS_SIZEOF_LINK_STATE_REVISION_1;
    ls.MediaConnectState = up ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    ls.MediaDuplexState = up ? MediaDuplexStateFull : MediaDuplexStateUnknown;
    ls.XmitLinkSpeed = up ? P5_LINK_SPEED_BPS : NDIS_LINK_SPEED_UNKNOWN;
    ls.RcvLinkSpeed = ls.XmitLinkSpeed;
    ls.PauseFunctions = NdisPauseFunctionsUnsupported;

    NDIS_STATUS_INDICATION si;
    NdisZeroMemory(&si, sizeof(si));
    si.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    si.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    si.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    si.SourceHandle = a->MiniportHandle;
    si.StatusCode = NDIS_STATUS_LINK_STATE;
    si.StatusBuffer = &ls;
    si.StatusBufferSize = sizeof(ls);
    NdisMIndicateStatusEx(a->MiniportHandle, &si);
}

static void LinkPoll(P5Adapter *a)
{
    LONG up = (RegRead(a, R_LINK) & 1) ? 1 : 0;
    if (up != a->LinkUp) {
        a->LinkUp = up;
        LinkIndicate(a, up != 0);
        DiagDword(L"Link", (ULONG)up);
    }
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

static inline UCHAR FrameKind(const UCHAR *dst)
{
    if (!(dst[0] & 1)) {
        return 0;
    }
    if (dst[0] == 0xFF && dst[1] == 0xFF && dst[2] == 0xFF && dst[3] == 0xFF && dst[4] == 0xFF && dst[5] == 0xFF) {
        return 2;
    }
    return 1;
}

static void RxIndicate(P5Adapter *a, const UCHAR *frame, ULONG len)
{
    // Busy first, then check state: Pause sets the state, then waits for Busy == 0.
    InterlockedIncrement(&a->Busy);
    if (a->RunState != RunRunning) {
        InterlockedDecrement(&a->Busy);
        a->RxDiscards++;
        return;
    }

    RtlCopyMemory(a->RxCopy, frame, len);

    PNET_BUFFER_LIST nbl = a->RxNbl;
    PNET_BUFFER nb = NET_BUFFER_LIST_FIRST_NB(nbl);
    NET_BUFFER_LIST_NEXT_NBL(nbl) = nullptr;
    NET_BUFFER_LIST_STATUS(nbl) = NDIS_STATUS_SUCCESS;
    nbl->SourceHandle = a->MiniportHandle;
    NET_BUFFER_FIRST_MDL(nb) = a->RxMdl;
    NET_BUFFER_CURRENT_MDL(nb) = a->RxMdl;
    NET_BUFFER_DATA_OFFSET(nb) = 0;
    NET_BUFFER_CURRENT_MDL_OFFSET(nb) = 0;
    NET_BUFFER_DATA_LENGTH(nb) = len;

    UCHAR kind = FrameKind(frame);
    a->RxFrames++;
    a->RxBytes += len;
    if (kind == 0) a->RxUcast++;
    else if (kind == 1) a->RxMcast++;
    else a->RxBcast++;

    NdisMIndicateReceiveNetBufferLists(a->MiniportHandle, nbl, NDIS_DEFAULT_PORT_NUMBER, 1,
                                       NDIS_RECEIVE_FLAGS_RESOURCES);
    InterlockedDecrement(&a->Busy);
}

// Returns number of descriptors consumed.
static ULONG RxPass(P5Adapter *a)
{
    ULONG taken = 0;
    while (taken < P5_RX_PER_PASS) {
        ULONG i = a->RxNext;
        P5Desc *d = &a->RxRing[i];
        ULONG w0 = d->w0;
        if (!(w0 & DESC_OWN_DONE)) {
            break;
        }
        _ReadBarrier();

        ULONG len = w0 & RXD_LEN_MASK;
        if (len < FRAME_MIN || len > FRAME_MAX_HW) {
            a->RxErrors++;
        } else {
            if (len > FRAME_MAX_IND) {
                len = FRAME_MAX_IND;
            }
            RxIndicate(a, a->RxBuf + i * RX_BUFSZ, len);
        }

        d->w2 = 0;
        d->w3 = 0;
        _WriteBarrier();
        d->w0 = RxW0Idle(i);

        a->RxNext = (i + 1) % RX_COUNT;
        taken++;
    }
    return taken;
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

static inline ULONG TxUsed(P5Adapter *a)
{
    return (a->TxHead - a->TxTail) & (TX_COUNT - 1);
}

static inline ULONG TxFree(P5Adapter *a)
{
    return (TX_COUNT - 1) - TxUsed(a);
}

static ULONG TxReclaim(P5Adapter *a)
{
    ULONG n = 0;
    while (a->TxTail != a->TxHead) {
        ULONG i = a->TxTail;
        P5Desc *d = &a->TxRing[i];
        ULONG w0 = d->w0;
        ULONG w2 = d->w2;
        if (!(w0 & DESC_OWN_DONE) || w2 >= TXD_W2_RECLAIMED) {
            break;
        }
        d->w2 = w2 | TXD_W2_RECLAIMED;

        P5TxInfo &ti = a->TxInfo[i];
        a->TxFrames++;
        a->TxBytes += ti.Length;
        if (ti.Kind == 0) a->TxUcast++;
        else if (ti.Kind == 1) a->TxMcast++;
        else a->TxBcast++;

        a->TxTail = (i + 1) % TX_COUNT;
        n++;
    }
    return n;
}

// Copy one NET_BUFFER into a TX slot. Returns the frame length, 0 on failure.
static ULONG TxCopyNb(PNET_BUFFER nb, PUCHAR dst)
{
    ULONG total = NET_BUFFER_DATA_LENGTH(nb);
    if (total < 14 || total > FRAME_MAX_HW) {
        return 0;
    }
    PMDL mdl = NET_BUFFER_CURRENT_MDL(nb);
    ULONG off = NET_BUFFER_CURRENT_MDL_OFFSET(nb);
    ULONG left = total;
    PUCHAR out = dst;
    while (left != 0 && mdl != nullptr) {
        PUCHAR va = (PUCHAR)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
        if (va == nullptr) {
            return 0;
        }
        ULONG cnt = MmGetMdlByteCount(mdl);
        if (off < cnt) {
            ULONG n = cnt - off;
            if (n > left) n = left;
            RtlCopyMemory(out, va + off, n);
            out += n;
            left -= n;
        }
        off = (off >= cnt) ? off - cnt : 0;
        mdl = mdl->Next;
    }
    if (left != 0) {
        return 0;
    }
    if (total < FRAME_MIN) {
        RtlZeroMemory(dst + total, FRAME_MIN - total);
        total = FRAME_MIN;
    }
    return total;
}

static void TxCompleteList(P5Adapter *a, PNET_BUFFER_LIST list, NDIS_STATUS status)
{
    if (list == nullptr) {
        return;
    }
    for (PNET_BUFFER_LIST n = list; n; n = NET_BUFFER_LIST_NEXT_NBL(n)) {
        NET_BUFFER_LIST_STATUS(n) = status;
    }
    ULONG flags = (KeGetCurrentIrql() == DISPATCH_LEVEL) ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0;
    NdisMSendNetBufferListsComplete(a->MiniportHandle, list, flags);
}

static ULONG NbCount(PNET_BUFFER_LIST nbl)
{
    ULONG c = 0;
    for (PNET_BUFFER nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb; nb = NET_BUFFER_NEXT_NB(nb)) {
        c++;
    }
    return c;
}

// Move queued NBLs into the ring while there is room. Returns frames queued to hardware.
static ULONG TxPass(P5Adapter *a)
{
    ULONG posted = 0;
    for (;;) {
        PNET_BUFFER_LIST nbl = nullptr;
        BOOLEAN drop = FALSE;

        NdisAcquireSpinLock(&a->QueueLock);
        if (a->QHead != nullptr) {
            ULONG need = NbCount(a->QHead);
            if (need > TX_COUNT - 1) {
                drop = TRUE;
            }
            if (drop || need <= TxFree(a)) {
                nbl = a->QHead;
                a->QHead = NET_BUFFER_LIST_NEXT_NBL(nbl);
                if (a->QHead == nullptr) {
                    a->QTail = nullptr;
                }
                NET_BUFFER_LIST_NEXT_NBL(nbl) = nullptr;
                InterlockedIncrement(&a->Busy);
            }
        }
        NdisReleaseSpinLock(&a->QueueLock);

        if (nbl == nullptr) {
            break;
        }

        NDIS_STATUS status = NDIS_STATUS_SUCCESS;
        ULONG postedThis = 0;
        if (drop) {
            status = NDIS_STATUS_INVALID_LENGTH;
            InterlockedIncrement64(&a->TxErrors);
        } else {
            for (PNET_BUFFER nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb; nb = NET_BUFFER_NEXT_NB(nb)) {
                ULONG i = a->TxHead;
                PUCHAR slot = a->TxBuf + i * TX_SLOT;
                ULONG len = TxCopyNb(nb, slot);
                if (len == 0) {
                    status = NDIS_STATUS_FAILURE;
                    InterlockedIncrement64(&a->TxErrors);
                    continue;
                }
                a->TxInfo[i].Length = (USHORT)len;
                a->TxInfo[i].Kind = FrameKind(slot);

                P5Desc *d = &a->TxRing[i];
                d->w1 = a->DmaPhys + DMA_OFF_TXBUF + i * TX_SLOT;
                d->w2 = TXD_W2_SUBMIT;
                d->w3 = 0;
                _WriteBarrier();
                d->w0 = len | TXD_FLAGS | (i == TX_COUNT - 1 ? DESC_WRAP : 0);

                a->TxHead = (i + 1) % TX_COUNT;
                postedThis++;
            }
        }

        if (postedThis) {
            posted += postedThis;
            _mm_sfence();
            RegOr(a, R_TXCTL, CTL_KICK);
        }

        // Data has been copied into DMA memory; the NBL can go back now.
        TxCompleteList(a, nbl, status);
        InterlockedDecrement(&a->Busy);
    }
    return posted;
}

static void QueueFlush(P5Adapter *a, NDIS_STATUS status)
{
    NdisAcquireSpinLock(&a->QueueLock);
    PNET_BUFFER_LIST list = a->QHead;
    a->QHead = nullptr;
    a->QTail = nullptr;
    NdisReleaseSpinLock(&a->QueueLock);
    TxCompleteList(a, list, status);
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

static void DiagCounters(P5Adapter *a)
{
    DiagDword(L"RxOk", (ULONG)a->RxFrames);
    DiagDword(L"TxOk", (ULONG)a->TxFrames);
    DiagDword(L"RxErr", (ULONG)a->RxErrors);
    DiagDword(L"TxErr", (ULONG)a->TxErrors);
}

static VOID WorkerThread(PVOID ctx)
{
    P5Adapter *a = (P5Adapter *)ctx;

    ExSetTimerResolution(10000, TRUE);  // 1 ms

    ULONG64 nextLink = 0;
    ULONG64 nextDiag = KeQueryInterruptTime() + P5_DIAG_POLL_100NS;
    ULONG idle = 0;
    ULONG busyRun = 0;

    while (!a->StopThread) {
        ULONG64 now = KeQueryInterruptTime();
        if (a->Ready && now >= nextLink) {
            LinkPoll(a);
            nextLink = now + P5_LINK_POLL_100NS;
        }
        if (now >= nextDiag) {
            DiagCounters(a);
            nextDiag = now + P5_DIAG_POLL_100NS;
        }

        ULONG work = 0;

        ULONG isr = RegRead(a, R_ISR);
        if (isr != 0) {
            RegWrite(a, R_ISR, isr);
        }

        ULONG rx = RxPass(a);
        work += rx;
        if (rx != 0 || (isr & ISR_RX_STARVED)) {
            RegOr(a, R_RXCTL, CTL_KICK);
        }

        work += TxReclaim(a);
        work += TxPass(a);

        if (work != 0) {
            idle = 0;
            // Do not monopolise a core under sustained load.
            if (++busyRun >= 64) {
                busyRun = 0;
                LARGE_INTEGER zero;
                zero.QuadPart = 0;
                KeDelayExecutionThread(KernelMode, FALSE, &zero);
            }
            continue;
        }
        busyRun = 0;

        if (idle < 32) {
            idle++;
            KeStallExecutionProcessor(2);
            continue;
        }

        LARGE_INTEGER timeout;
        timeout.QuadPart = -10000;  // 1 ms
        KeWaitForSingleObject(&a->WakeEvent, Executive, KernelMode, FALSE, &timeout);
        idle = 0;
    }

    DiagCounters(a);
    ExSetTimerResolution(0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// ---------------------------------------------------------------------------
// Resource setup / teardown
// ---------------------------------------------------------------------------

static void AdapterRelease(P5Adapter *a)
{
    if (a->RxNbl) {
        NdisFreeNetBufferList(a->RxNbl);
        a->RxNbl = nullptr;
    }
    if (a->RxMdl) {
        NdisFreeMdl(a->RxMdl);
        a->RxMdl = nullptr;
    }
    if (a->RxCopy) {
        NdisFreeMemory(a->RxCopy, 0, 0);
        a->RxCopy = nullptr;
    }
    if (a->NblPool) {
        NdisFreeNetBufferListPool(a->NblPool);
        a->NblPool = nullptr;
    }
    if (a->Dma) {
        MmUnmapIoSpace(a->Dma, DMA_SIZE);
        a->Dma = nullptr;
    }
    if (a->Regs) {
        MmUnmapIoSpace((PVOID)a->Regs, ETH_BAR0_SIZE);
        a->Regs = nullptr;
    }
    RootPortRestore(a);
    if (a->QueueLockInit) {
        NdisFreeSpinLock(&a->QueueLock);
        a->QueueLockInit = FALSE;
    }
    NdisFreeMemory(a, 0, 0);
}

static NDIS_STATUS RxResourcesAlloc(P5Adapter *a)
{
    NET_BUFFER_LIST_POOL_PARAMETERS pp;
    NdisZeroMemory(&pp, sizeof(pp));
    pp.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    pp.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pp.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pp.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    pp.fAllocateNetBuffer = TRUE;
    pp.PoolTag = P5_TAG;
    a->NblPool = NdisAllocateNetBufferListPool(a->MiniportHandle, &pp);
    if (!a->NblPool) {
        return NDIS_STATUS_RESOURCES;
    }

    a->RxCopy = (PUCHAR)NdisAllocateMemoryWithTagPriority(a->MiniportHandle, RX_BUFSZ, P5_TAG, NormalPoolPriority);
    if (!a->RxCopy) {
        return NDIS_STATUS_RESOURCES;
    }
    a->RxMdl = NdisAllocateMdl(a->MiniportHandle, a->RxCopy, RX_BUFSZ);
    if (!a->RxMdl) {
        return NDIS_STATUS_RESOURCES;
    }
    a->RxNbl = NdisAllocateNetBufferAndNetBufferList(a->NblPool, 0, 0, a->RxMdl, 0, 0);
    if (!a->RxNbl) {
        return NDIS_STATUS_RESOURCES;
    }
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS MapHardware(P5Adapter *a)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = ETH_BAR0_PHYS;
    a->Regs = (volatile UCHAR *)MmMapIoSpaceEx(pa, ETH_BAR0_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!a->Regs) {
        return NDIS_STATUS_RESOURCES;
    }

    pa.QuadPart = DMA_PHYS;
    a->Dma = (PUCHAR)MmMapIoSpaceEx(pa, DMA_SIZE, PAGE_READWRITE);   // cached
    if (!a->Dma) {
        return NDIS_STATUS_RESOURCES;
    }
    a->DmaPhys = DMA_PHYS;
    a->TxRing = (P5Desc *)(a->Dma + DMA_OFF_TXRING);
    a->RxRing = (P5Desc *)(a->Dma + DMA_OFF_RXRING);
    a->TxBuf = a->Dma + DMA_OFF_TXBUF;
    a->RxBuf = a->Dma + DMA_OFF_RXBUF;
    return NDIS_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Miniport handlers
// ---------------------------------------------------------------------------

static NDIS_STATUS SetRegistrationAttributes(P5Adapter *a)
{
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES ra;
    NdisZeroMemory(&ra, sizeof(ra));
    ra.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    ra.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_2;
    ra.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_2;
    ra.MiniportAdapterContext = a;
    ra.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_HARDWARE_DEVICE;
    ra.CheckForHangTimeInSeconds = 0;
    ra.InterfaceType = NdisInterfaceInternal;
    return NdisMSetMiniportAttributes(a->MiniportHandle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&ra);
}

static NDIS_STATUS SetGeneralAttributes(P5Adapter *a)
{
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES ga;
    NdisZeroMemory(&ga, sizeof(ga));
    ga.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    ga.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    ga.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;

    BOOLEAN up = a->LinkUp != 0;
    ga.MediaType = NdisMedium802_3;
    ga.PhysicalMediumType = NdisPhysicalMedium802_3;
    ga.MtuSize = P5_MTU;
    ga.MaxXmitLinkSpeed = P5_LINK_SPEED_BPS;
    ga.MaxRcvLinkSpeed = P5_LINK_SPEED_BPS;
    ga.XmitLinkSpeed = up ? P5_LINK_SPEED_BPS : NDIS_LINK_SPEED_UNKNOWN;
    ga.RcvLinkSpeed = ga.XmitLinkSpeed;
    ga.MediaConnectState = up ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    ga.MediaDuplexState = up ? MediaDuplexStateFull : MediaDuplexStateUnknown;
    ga.LookaheadSize = P5_MTU;
    ga.PowerManagementCapabilities = nullptr;
    ga.MacOptions = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                    NDIS_MAC_OPTION_NO_LOOPBACK;
    ga.SupportedPacketFilters = P5_PACKET_FILTERS;
    ga.MaxMulticastListSize = P5_MCAST_MAX;
    ga.MacAddressLength = 6;
    RtlCopyMemory(ga.PermanentMacAddress, a->PermanentMac, 6);
    RtlCopyMemory(ga.CurrentMacAddress, a->CurrentMac, 6);
    ga.RecvScaleCapabilities = nullptr;
    ga.AccessType = NET_IF_ACCESS_BROADCAST;
    ga.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    ga.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    ga.IfType = IF_TYPE_ETHERNET_CSMACD;
    ga.IfConnectorPresent = TRUE;
    ga.SupportedStatistics = NDIS_STATISTICS_XMIT_OK_SUPPORTED | NDIS_STATISTICS_RCV_OK_SUPPORTED |
                             NDIS_STATISTICS_XMIT_ERROR_SUPPORTED | NDIS_STATISTICS_RCV_ERROR_SUPPORTED |
                             NDIS_STATISTICS_RCV_NO_BUFFER_SUPPORTED |
                             NDIS_STATISTICS_DIRECTED_FRAMES_XMIT_SUPPORTED |
                             NDIS_STATISTICS_MULTICAST_FRAMES_XMIT_SUPPORTED |
                             NDIS_STATISTICS_BROADCAST_FRAMES_XMIT_SUPPORTED |
                             NDIS_STATISTICS_DIRECTED_FRAMES_RCV_SUPPORTED |
                             NDIS_STATISTICS_MULTICAST_FRAMES_RCV_SUPPORTED |
                             NDIS_STATISTICS_BROADCAST_FRAMES_RCV_SUPPORTED |
                             NDIS_STATISTICS_BYTES_RCV_SUPPORTED | NDIS_STATISTICS_BYTES_XMIT_SUPPORTED |
                             NDIS_STATISTICS_RCV_DISCARDS_SUPPORTED | NDIS_STATISTICS_XMIT_DISCARDS_SUPPORTED |
                             NDIS_STATISTICS_GEN_STATISTICS_SUPPORTED;
    ga.SupportedPauseFunctions = NdisPauseFunctionsUnsupported;
    ga.DataBackFillSize = 0;
    ga.ContextBackFillSize = 0;
    ga.SupportedOidList = (PNDIS_OID)g_SupportedOids;
    ga.SupportedOidListLength = sizeof(g_SupportedOids);
    ga.AutoNegotiationFlags = NDIS_LINK_STATE_DUPLEX_AUTO_NEGOTIATED | NDIS_LINK_STATE_XMIT_LINK_SPEED_AUTO_NEGOTIATED |
                              NDIS_LINK_STATE_RCV_LINK_SPEED_AUTO_NEGOTIATED;
    ga.PowerManagementCapabilitiesEx = nullptr;
    return NdisMSetMiniportAttributes(a->MiniportHandle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&ga);
}

static NDIS_STATUS P5Initialize(NDIS_HANDLE miniportHandle, NDIS_HANDLE driverContext,
                                PNDIS_MINIPORT_INIT_PARAMETERS initParams)
{
    UNREFERENCED_PARAMETER(driverContext);
    UNREFERENCED_PARAMETER(initParams);

    DiagStage(1, L"init");

    P5Adapter *a = (P5Adapter *)NdisAllocateMemoryWithTagPriority(miniportHandle, sizeof(P5Adapter), P5_TAG,
                                                                  NormalPoolPriority);
    if (!a) {
        DiagStage(90, L"no memory");
        return NDIS_STATUS_RESOURCES;
    }
    NdisZeroMemory(a, sizeof(*a));
    a->MiniportHandle = miniportHandle;
    a->RunState = RunPaused;
    a->PacketFilter = 0;
    a->Lookahead = P5_MTU;
    NdisAllocateSpinLock(&a->QueueLock);
    a->QueueLockInit = TRUE;
    KeInitializeEvent(&a->WakeEvent, SynchronizationEvent, FALSE);

    NDIS_STATUS st = SetRegistrationAttributes(a);
    if (st != NDIS_STATUS_SUCCESS) {
        DiagStage(91, L"registration attributes failed");
        AdapterRelease(a);
        return st;
    }

    RootPortPrepare(a);
    DiagStage(2, L"root port ok");

    st = EthFunctionCheck();
    if (st != NDIS_STATUS_SUCCESS) {
        DiagStage(92, L"ethernet function not found");
        AdapterRelease(a);
        return st;
    }
    DiagStage(3, L"function ok");

    st = MapHardware(a);
    if (st != NDIS_STATUS_SUCCESS) {
        DiagStage(93, L"map failed");
        AdapterRelease(a);
        return st;
    }

    MacLoad(a);
    DiagStage(4, L"mac ok");

    st = RxResourcesAlloc(a);
    if (st != NDIS_STATUS_SUCCESS) {
        DiagStage(94, L"rx resources failed");
        AdapterRelease(a);
        return st;
    }

    ChipStart(a);
    a->LinkUp = (RegRead(a, R_LINK) & 1) ? 1 : 0;
    DiagDword(L"Link", (ULONG)a->LinkUp);
    DiagStage(5, L"chip started");

    st = SetGeneralAttributes(a);
    if (st != NDIS_STATUS_SUCCESS) {
        DiagStage(95, L"general attributes failed");
        ChipStop(a, FALSE);
        AdapterRelease(a);
        return st;
    }

    HANDLE h = nullptr;
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, nullptr, OBJ_KERNEL_HANDLE, nullptr, nullptr);
    NTSTATUS ns = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, nullptr, nullptr, WorkerThread, a);
    if (NT_SUCCESS(ns)) {
        ns = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&a->Thread, nullptr);
        if (!NT_SUCCESS(ns)) {
            // Thread is running but unreferenced: stop it the hard way is not possible; wait on handle.
            a->StopThread = 1;
            KeSetEvent(&a->WakeEvent, IO_NO_INCREMENT, FALSE);
            ZwWaitForSingleObject(h, FALSE, nullptr);
        }
        ZwClose(h);
    }
    if (!NT_SUCCESS(ns)) {
        DiagStage(96, L"thread failed");
        ChipStop(a, FALSE);
        AdapterRelease(a);
        return NDIS_STATUS_RESOURCES;
    }

    InterlockedExchange(&a->Ready, 1);
    DiagStage(6, L"running");
    return NDIS_STATUS_SUCCESS;
}

static VOID P5Halt(NDIS_HANDLE ctx, NDIS_HALT_ACTION action)
{
    UNREFERENCED_PARAMETER(action);
    P5Adapter *a = (P5Adapter *)ctx;

    DiagStage(10, L"halting");
    InterlockedExchange(&a->RunState, RunPaused);
    InterlockedExchange(&a->StopThread, 1);
    KeSetEvent(&a->WakeEvent, IO_NO_INCREMENT, FALSE);
    if (a->Thread) {
        KeWaitForSingleObject(a->Thread, Executive, KernelMode, FALSE, nullptr);
        ObDereferenceObject(a->Thread);
        a->Thread = nullptr;
    }
    QueueFlush(a, NDIS_STATUS_FAILURE);
    ChipStop(a, FALSE);
    AdapterRelease(a);
    DiagStage(11, L"halted");
}

static VOID P5Unload(PDRIVER_OBJECT driverObject)
{
    UNREFERENCED_PARAMETER(driverObject);
    if (g_DriverHandle) {
        NdisMDeregisterMiniportDriver(g_DriverHandle);
        g_DriverHandle = nullptr;
    }
}

static NDIS_STATUS P5Pause(NDIS_HANDLE ctx, PNDIS_MINIPORT_PAUSE_PARAMETERS params)
{
    UNREFERENCED_PARAMETER(params);
    P5Adapter *a = (P5Adapter *)ctx;

    NdisAcquireSpinLock(&a->QueueLock);
    InterlockedExchange(&a->RunState, RunPausing);
    PNET_BUFFER_LIST list = a->QHead;
    a->QHead = nullptr;
    a->QTail = nullptr;
    NdisReleaseSpinLock(&a->QueueLock);
    TxCompleteList(a, list, NDIS_STATUS_PAUSED);

    // Wait for the worker to finish any receive indication or send it already started.
    while (InterlockedCompareExchange(&a->Busy, 0, 0) != 0) {
        NdisMSleep(100);
    }
    InterlockedExchange(&a->RunState, RunPaused);
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS P5Restart(NDIS_HANDLE ctx, PNDIS_MINIPORT_RESTART_PARAMETERS params)
{
    UNREFERENCED_PARAMETER(params);
    P5Adapter *a = (P5Adapter *)ctx;
    InterlockedExchange(&a->RunState, RunRunning);
    return NDIS_STATUS_SUCCESS;
}

static VOID P5Send(NDIS_HANDLE ctx, PNET_BUFFER_LIST nblChain, NDIS_PORT_NUMBER port, ULONG sendFlags)
{
    UNREFERENCED_PARAMETER(port);
    P5Adapter *a = (P5Adapter *)ctx;
    ULONG completeFlags = (sendFlags & NDIS_SEND_FLAGS_DISPATCH_LEVEL) ? NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0;

    BOOLEAN atDispatch = (sendFlags & NDIS_SEND_FLAGS_DISPATCH_LEVEL) != 0;
    PNET_BUFFER_LIST last = nblChain;
    while (NET_BUFFER_LIST_NEXT_NBL(last)) {
        last = NET_BUFFER_LIST_NEXT_NBL(last);
    }

    // State is checked under the queue lock so P5Pause (which changes it under the same
    // lock and then flushes) can never leave an NBL queued behind it.
    NDIS_STATUS reject = NDIS_STATUS_SUCCESS;
    if (atDispatch) {
        NdisDprAcquireSpinLock(&a->QueueLock);
    } else {
        NdisAcquireSpinLock(&a->QueueLock);
    }
    if (a->RunState != RunRunning) {
        reject = NDIS_STATUS_PAUSED;
    } else if (!a->LinkUp) {
        reject = NDIS_STATUS_MEDIA_DISCONNECTED;
    } else {
        if (a->QTail) {
            NET_BUFFER_LIST_NEXT_NBL(a->QTail) = nblChain;
        } else {
            a->QHead = nblChain;
        }
        a->QTail = last;
    }
    if (atDispatch) {
        NdisDprReleaseSpinLock(&a->QueueLock);
    } else {
        NdisReleaseSpinLock(&a->QueueLock);
    }

    if (reject != NDIS_STATUS_SUCCESS) {
        for (PNET_BUFFER_LIST n = nblChain; n; n = NET_BUFFER_LIST_NEXT_NBL(n)) {
            NET_BUFFER_LIST_STATUS(n) = reject;
            InterlockedIncrement64(&a->TxDiscards);
        }
        NdisMSendNetBufferListsComplete(a->MiniportHandle, nblChain, completeFlags);
        return;
    }

    KeSetEvent(&a->WakeEvent, IO_NO_INCREMENT, FALSE);
}

static VOID P5ReturnNbls(NDIS_HANDLE ctx, PNET_BUFFER_LIST nbls, ULONG flags)
{
    // All receive indications use NDIS_RECEIVE_FLAGS_RESOURCES; nothing is ever returned here.
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(nbls);
    UNREFERENCED_PARAMETER(flags);
}

static VOID P5CancelSend(NDIS_HANDLE ctx, PVOID cancelId)
{
    P5Adapter *a = (P5Adapter *)ctx;
    PNET_BUFFER_LIST cancelled = nullptr;
    PNET_BUFFER_LIST *cancelTail = &cancelled;

    NdisAcquireSpinLock(&a->QueueLock);
    PNET_BUFFER_LIST prev = nullptr;
    PNET_BUFFER_LIST cur = a->QHead;
    while (cur) {
        PNET_BUFFER_LIST next = NET_BUFFER_LIST_NEXT_NBL(cur);
        if (NDIS_GET_NET_BUFFER_LIST_CANCEL_ID(cur) == cancelId) {
            if (prev) {
                NET_BUFFER_LIST_NEXT_NBL(prev) = next;
            } else {
                a->QHead = next;
            }
            if (a->QTail == cur) {
                a->QTail = prev;
            }
            NET_BUFFER_LIST_NEXT_NBL(cur) = nullptr;
            *cancelTail = cur;
            cancelTail = &NET_BUFFER_LIST_NEXT_NBL(cur);
        } else {
            prev = cur;
        }
        cur = next;
    }
    NdisReleaseSpinLock(&a->QueueLock);

    TxCompleteList(a, cancelled, NDIS_STATUS_SEND_ABORTED);
}

static NDIS_STATUS P5Reset(NDIS_HANDLE ctx, PBOOLEAN addressingReset)
{
    UNREFERENCED_PARAMETER(ctx);
    *addressingReset = FALSE;
    return NDIS_STATUS_SUCCESS;
}

static VOID P5PnpEvent(NDIS_HANDLE ctx, PNET_DEVICE_PNP_EVENT event)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(event);
}

static VOID P5Shutdown(NDIS_HANDLE ctx, NDIS_SHUTDOWN_ACTION action)
{
    UNREFERENCED_PARAMETER(action);
    P5Adapter *a = (P5Adapter *)ctx;
    // May run at high IRQL: busy-wait only, no registry, no locks.
    if (a->Regs) {
        ChipStop(a, TRUE);
    }
}

static VOID P5CancelOid(NDIS_HANDLE ctx, PVOID requestId)
{
    // All OID requests complete synchronously.
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(requestId);
}

// ---------------------------------------------------------------------------
// OID handling
// ---------------------------------------------------------------------------

static NDIS_STATUS OidReply(PNDIS_OID_REQUEST req, const void *data, ULONG size)
{
    auto &q = req->DATA.QUERY_INFORMATION;
    if (q.InformationBufferLength < size) {
        q.BytesNeeded = size;
        q.BytesWritten = 0;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }
    RtlCopyMemory(q.InformationBuffer, data, size);
    q.BytesWritten = size;
    q.BytesNeeded = 0;
    return NDIS_STATUS_SUCCESS;
}

static NDIS_STATUS OidReplyUlong(PNDIS_OID_REQUEST req, ULONG v)
{
    return OidReply(req, &v, sizeof(v));
}

// Counters: 8 bytes if there is room, else 4 (with BytesNeeded = 8).
static NDIS_STATUS OidReplyCounter(PNDIS_OID_REQUEST req, ULONG64 v)
{
    auto &q = req->DATA.QUERY_INFORMATION;
    if (q.InformationBufferLength >= sizeof(ULONG64)) {
        return OidReply(req, &v, sizeof(v));
    }
    if (q.InformationBufferLength >= sizeof(ULONG)) {
        ULONG lo = (ULONG)v;
        RtlCopyMemory(q.InformationBuffer, &lo, sizeof(lo));
        q.BytesWritten = sizeof(lo);
        q.BytesNeeded = sizeof(ULONG64);
        return NDIS_STATUS_SUCCESS;
    }
    q.BytesWritten = 0;
    q.BytesNeeded = sizeof(ULONG64);
    return NDIS_STATUS_BUFFER_TOO_SHORT;
}

static NDIS_STATUS OidQuery(P5Adapter *a, PNDIS_OID_REQUEST req)
{
    static const char vendorDesc[] = "PS5 wired Ethernet (p5eth)";
    BOOLEAN up = a->LinkUp != 0;

    switch (req->DATA.QUERY_INFORMATION.Oid) {
    case OID_GEN_SUPPORTED_LIST:
        return OidReply(req, g_SupportedOids, sizeof(g_SupportedOids));
    case OID_GEN_HARDWARE_STATUS:
        return OidReplyUlong(req, NdisHardwareStatusReady);
    case OID_GEN_MEDIA_SUPPORTED:
    case OID_GEN_MEDIA_IN_USE:
        return OidReplyUlong(req, NdisMedium802_3);
    case OID_GEN_PHYSICAL_MEDIUM:
        return OidReplyUlong(req, NdisPhysicalMedium802_3);
    case OID_GEN_MAXIMUM_LOOKAHEAD:
    case OID_GEN_MAXIMUM_FRAME_SIZE:
        return OidReplyUlong(req, P5_MTU);
    case OID_GEN_CURRENT_LOOKAHEAD:
        return OidReplyUlong(req, a->Lookahead);
    case OID_GEN_MAXIMUM_TOTAL_SIZE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE:
        return OidReplyUlong(req, FRAME_MAX_IND);
    case OID_GEN_LINK_SPEED:
        return OidReplyUlong(req, up ? (ULONG)(P5_LINK_SPEED_BPS / 100) : 0);
    case OID_GEN_TRANSMIT_BUFFER_SPACE:
        return OidReplyUlong(req, TX_COUNT * FRAME_MAX_IND);
    case OID_GEN_RECEIVE_BUFFER_SPACE:
        return OidReplyUlong(req, RX_COUNT * FRAME_MAX_IND);
    case OID_GEN_VENDOR_ID:
        return OidReplyUlong(req, 0xFFFFFF00u);   // no IEEE OUI assigned
    case OID_GEN_VENDOR_DESCRIPTION:
        return OidReply(req, vendorDesc, sizeof(vendorDesc));
    case OID_GEN_VENDOR_DRIVER_VERSION:
        return OidReplyUlong(req, 0x00010000u);
    case OID_GEN_DRIVER_VERSION: {
        USHORT v = (6 << 8) | 30;
        return OidReply(req, &v, sizeof(v));
    }
    case OID_GEN_CURRENT_PACKET_FILTER:
        return OidReplyUlong(req, a->PacketFilter);
    case OID_GEN_MAC_OPTIONS:
        return OidReplyUlong(req, NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                                      NDIS_MAC_OPTION_NO_LOOPBACK);
    case OID_GEN_MEDIA_CONNECT_STATUS:
        return OidReplyUlong(req, up ? NdisMediaStateConnected : NdisMediaStateDisconnected);
    case OID_GEN_MAXIMUM_SEND_PACKETS:
        return OidReplyUlong(req, TX_COUNT - 1);
    case OID_GEN_XMIT_OK:
        return OidReplyCounter(req, a->TxFrames);
    case OID_GEN_RCV_OK:
        return OidReplyCounter(req, a->RxFrames);
    case OID_GEN_XMIT_ERROR:
        return OidReplyCounter(req, (ULONG64)a->TxErrors);
    case OID_GEN_RCV_ERROR:
        return OidReplyCounter(req, a->RxErrors);
    case OID_GEN_RCV_NO_BUFFER:
    case OID_802_3_RCV_ERROR_ALIGNMENT:
    case OID_802_3_XMIT_ONE_COLLISION:
    case OID_802_3_XMIT_MORE_COLLISIONS:
        return OidReplyCounter(req, 0);
    case OID_GEN_STATISTICS: {
        NDIS_STATISTICS_INFO s;
        NdisZeroMemory(&s, sizeof(s));
        s.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        s.Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
        s.Header.Size = NDIS_SIZEOF_STATISTICS_INFO_REVISION_1;
        s.SupportedStatistics = NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_RCV |
                                NDIS_STATISTICS_FLAGS_VALID_MULTICAST_FRAMES_RCV |
                                NDIS_STATISTICS_FLAGS_VALID_BROADCAST_FRAMES_RCV |
                                NDIS_STATISTICS_FLAGS_VALID_BYTES_RCV | NDIS_STATISTICS_FLAGS_VALID_RCV_DISCARDS |
                                NDIS_STATISTICS_FLAGS_VALID_RCV_ERROR |
                                NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_XMIT |
                                NDIS_STATISTICS_FLAGS_VALID_MULTICAST_FRAMES_XMIT |
                                NDIS_STATISTICS_FLAGS_VALID_BROADCAST_FRAMES_XMIT |
                                NDIS_STATISTICS_FLAGS_VALID_BYTES_XMIT | NDIS_STATISTICS_FLAGS_VALID_XMIT_ERROR |
                                NDIS_STATISTICS_FLAGS_VALID_XMIT_DISCARDS;
        s.ifInDiscards = a->RxDiscards;
        s.ifInErrors = a->RxErrors;
        s.ifHCInOctets = a->RxBytes;
        s.ifHCInUcastPkts = a->RxUcast;
        s.ifHCInMulticastPkts = a->RxMcast;
        s.ifHCInBroadcastPkts = a->RxBcast;
        s.ifHCOutOctets = a->TxBytes;
        s.ifHCOutUcastPkts = a->TxUcast;
        s.ifHCOutMulticastPkts = a->TxMcast;
        s.ifHCOutBroadcastPkts = a->TxBcast;
        s.ifOutErrors = (ULONG64)a->TxErrors;
        s.ifOutDiscards = (ULONG64)a->TxDiscards;
        return OidReply(req, &s, NDIS_SIZEOF_STATISTICS_INFO_REVISION_1);
    }
    case OID_GEN_INTERRUPT_MODERATION: {
        NDIS_INTERRUPT_MODERATION_PARAMETERS im;
        NdisZeroMemory(&im, sizeof(im));
        im.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
        im.Header.Revision = NDIS_INTERRUPT_MODERATION_PARAMETERS_REVISION_1;
        im.Header.Size = NDIS_SIZEOF_INTERRUPT_MODERATION_PARAMETERS_REVISION_1;
        im.InterruptModeration = NdisInterruptModerationNotSupported;
        return OidReply(req, &im, sizeof(im));
    }
    case OID_802_3_PERMANENT_ADDRESS:
        return OidReply(req, a->PermanentMac, 6);
    case OID_802_3_CURRENT_ADDRESS:
        return OidReply(req, a->CurrentMac, 6);
    case OID_802_3_MULTICAST_LIST:
        return OidReply(req, a->McastList, a->McastCount * 6);
    case OID_802_3_MAXIMUM_LIST_SIZE:
        return OidReplyUlong(req, P5_MCAST_MAX);
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}

static NDIS_STATUS OidSet(P5Adapter *a, PNDIS_OID_REQUEST req)
{
    auto &s = req->DATA.SET_INFORMATION;
    s.BytesRead = 0;
    s.BytesNeeded = 0;

    switch (s.Oid) {
    case OID_GEN_CURRENT_PACKET_FILTER: {
        if (s.InformationBufferLength < sizeof(ULONG)) {
            s.BytesNeeded = sizeof(ULONG);
            return NDIS_STATUS_INVALID_LENGTH;
        }
        ULONG f = *(ULONG *)s.InformationBuffer;
        if (f & ~P5_PACKET_FILTERS) {
            return NDIS_STATUS_NOT_SUPPORTED;
        }
        // The chip already receives all multicast; NDIS filters in software.
        a->PacketFilter = f;
        s.BytesRead = sizeof(ULONG);
        return NDIS_STATUS_SUCCESS;
    }
    case OID_GEN_CURRENT_LOOKAHEAD: {
        if (s.InformationBufferLength < sizeof(ULONG)) {
            s.BytesNeeded = sizeof(ULONG);
            return NDIS_STATUS_INVALID_LENGTH;
        }
        ULONG v = *(ULONG *)s.InformationBuffer;
        if (v > P5_MTU) {
            return NDIS_STATUS_INVALID_DATA;
        }
        a->Lookahead = v;
        s.BytesRead = sizeof(ULONG);
        return NDIS_STATUS_SUCCESS;
    }
    case OID_802_3_MULTICAST_LIST: {
        ULONG len = s.InformationBufferLength;
        if (len % 6 != 0) {
            return NDIS_STATUS_INVALID_LENGTH;
        }
        if (len / 6 > P5_MCAST_MAX) {
            return NDIS_STATUS_MULTICAST_FULL;
        }
        // Accepted and stored for queries; hardware keeps receiving all multicast.
        if (len) {
            RtlCopyMemory(a->McastList, s.InformationBuffer, len);
        }
        a->McastCount = len / 6;
        s.BytesRead = len;
        return NDIS_STATUS_SUCCESS;
    }
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}

static NDIS_STATUS P5OidRequest(NDIS_HANDLE ctx, PNDIS_OID_REQUEST req)
{
    P5Adapter *a = (P5Adapter *)ctx;
    switch (req->RequestType) {
    case NdisRequestQueryInformation:
    case NdisRequestQueryStatistics:
        return OidQuery(a, req);
    case NdisRequestSetInformation:
        return OidSet(a, req);
    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}

// ---------------------------------------------------------------------------
// DriverEntry
// ---------------------------------------------------------------------------

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS mc;
    NdisZeroMemory(&mc, sizeof(mc));
    mc.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    mc.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    mc.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    mc.MajorNdisVersion = 6;
    mc.MinorNdisVersion = 30;
    mc.MajorDriverVersion = 1;
    mc.MinorDriverVersion = 0;
    mc.Flags = 0;
    mc.InitializeHandlerEx = P5Initialize;
    mc.HaltHandlerEx = P5Halt;
    mc.UnloadHandler = P5Unload;
    mc.PauseHandler = P5Pause;
    mc.RestartHandler = P5Restart;
    mc.OidRequestHandler = P5OidRequest;
    mc.SendNetBufferListsHandler = P5Send;
    mc.ReturnNetBufferListsHandler = P5ReturnNbls;
    mc.CancelSendHandler = P5CancelSend;
    mc.CheckForHangHandlerEx = nullptr;
    mc.ResetHandlerEx = P5Reset;
    mc.DevicePnPEventNotifyHandler = P5PnpEvent;
    mc.ShutdownHandlerEx = P5Shutdown;
    mc.CancelOidRequestHandler = P5CancelOid;

    NDIS_STATUS st = NdisMRegisterMiniportDriver(driverObject, registryPath, nullptr, &mc, &g_DriverHandle);
    return (st == NDIS_STATUS_SUCCESS) ? STATUS_SUCCESS : (NTSTATUS)st;
}
