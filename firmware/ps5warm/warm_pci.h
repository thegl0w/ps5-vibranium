/* SPDX-License-Identifier: MIT
 * Warm restart, stage-1 side: put the PCI state the firmware depends on back to what the first run (after Linux) saw.
 *
 * Why (2026-11-24, console experiment 3b): our firmware runs with PCI enumeration off (PcdPciDisableBusEnumeration):
 * it takes every BAR, bridge window and command register as it finds them and never writes a power state. After a
 * power-on that state comes from Sony's firmware and Linux. After a Windows Restart it is whatever Windows' shutdown
 * left: USB controllers in D3hot (both AMD xHCI report NoSoftRst-, so D3hot -> D0 also clears their BARs), decoding
 * switched off, and the Salina root port 00:01.1 with command 0x0400 and bus numbers 0 (seen under Windows,
 * NEXT.md "p5wifi on the console"). The firmware's second run then found no USB device at all.
 *
 *   wp_snapshot()  first run (from Linux): copies the configuration space (0x00-0xFF) of the two root ports and of
 *                  every xHCI on bus 0x20 into the hand-over record page. Reads only.
 *   wp_restore()   warm run: for the root ports and the xHCIs, compare with the snapshot, bring the function to D0,
 *                  write the saved header back, then (xHCI) halt, reset, wait for Controller Not Ready to clear and
 *                  switch port power on. Prints one line per function and leaves the same facts in the record
 *                  (WPD_*), which the firmware shows again and hands to Windows (ps5warm.ps1 status).
 *                  boot50w: an xHCI that answers and halts is NOT reset here (the firmware's own driver does
 *                  that, as after a launch from Linux); boot49w reset every xHCI here as well and two of its three
 *                  firmware-only loops ended in Windows' 0x7B. When a controller was woken, reset or had port power
 *                  switched on, stage 1 then waits WP_SETTLE_MS so the disk's bridge is not reset again mid-start.
 *
 * Never touched: bus 0x40 (40:00.0 is the console's NVMe), the PSP 20:00.2, bus-master bits are only ever set (never
 * cleared), no secondary bus reset, no PSP/SMU/ICC traffic.
 *
 * The including file provides (stage 1: firmware/build_stage.sh; tests: warmrestart/exp4):
 *   u8 u16 u32 u64
 *   WP_CFG_RD(b, d, f, off) -> u32        WP_CFG_WR(b, d, f, off, v)       configuration dwords
 *   WP_RD8(a) WP_RD32(a) WP_WR32(a, v)                                        device registers, a = u64 physical
 *   WP_DELAY_US(us)   WP_PUTS(s)   WP_HEX(v, digits)
 *   WP_REC                                                                    u64 address of the record page
 *
 * Copyright (c) 2026 ps5win contributors
 */
#ifndef PS5_WARM_PCI_H_
#define PS5_WARM_PCI_H_

/* Record page layout (offsets from PS5_WARM_RECORD; 0x000-0x05F is the hand-over record itself, Ps5Warm.h). */
#define WPG_MAGIC_OFF   0x100           /* u32 "WPG1": snapshot valid */
#define WPG_COUNT_OFF   0x104           /* u32 entries */
#define WPG_ENTRY_OFF   0x110           /* entries: u32 bdf, u32 id, u32 kind, u32 0, then 256 configuration bytes */
#define WPG_ENTRY_SIZE  0x110
#define WPG_MAX         6               /* 2 root ports + up to 4 xHCI */
#define WPG_MAGIC       0x31475057u
#define WPD_OFF         0x800           /* what the warm run found and did (copied into the firmware's block) */
#define WPD_SIZE        0x110
#define WPD_MAGIC       0x31445057u     /* "WPD1" */
/* WPD: +0 magic, +4 stage run number, +8 xHCI count, +0xC bus 0x20 functions (nibble per function: 8 present,
 * 4 memory decoding on, 3 D-state), +0x10 two bridge entries of 0x20 (bdf, cmd, buses, memory window, PMCSR, flags),
 * +0x50 four xHCI entries of 0x30 (bdf, cmd, PMCSR, BAR0, flags, USBSTS after, reset microseconds, ports,
 * PORTSC x 4). cmd/PMCSR/BAR0 are the values FOUND, before any write. */
#define WPD_BRIDGE_OFF  0x10
#define WPD_BRIDGE_SIZE 0x20
#define WPD_XHCI_OFF    0x50
#define WPD_XHCI_SIZE   0x30

#define WPF_ID_OK       0x001           /* same device as in the snapshot */
#define WPF_WAS_ASLEEP  0x002           /* found in D1-D3 */
#define WPF_D0_NOW      0x004           /* D0 confirmed after the write */
#define WPF_RESTORED    0x008           /* header written back */
#define WPF_VENDOR      0x010           /* vendor-specific capability dwords written back */
#define WPF_MMIO        0x020           /* registers answer */
#define WPF_HALTED      0x040
#define WPF_RESET       0x080           /* HCRST finished and Controller Not Ready is clear */
#define WPF_PORT_POWER  0x100           /* a port had power off; switched on */
#define WPF_OTHER_DIFF  0x200           /* configuration bytes we do not restore differ from the snapshot */
#define WPF_POWER_CYCLE 0x400           /* first try failed: D3hot -> D0 cycle (function soft reset) and a second try */
#define WPF_LEFT        0x800           /* answers and is halted: no controller reset in stage 1 (boot50w, 2026-11-24) */
#define WPF_PP_CYCLE    0x1000          /* port power switched off and on again (boot52w): the disk is re-plugged */
#define WPF_PP_GONE     0x2000          /* ... and its port really lost the connection while power was off */
#define WPF_PP_BACK     0x4000          /* ... and showed it again; the time is in the report (+24, microseconds) */
#define WP_PP_OFF_MS    2000            /* port power off for this long */
#define WP_PP_WAIT_MS   10000           /* wait this long at most for the disk to show up again */
#define WP_PP_AFTER_MS  3000            /* then leave it alone for this long before the firmware's driver resets it */
#define WP_SETTLE_MS    6000            /* after a wake / reset / port power-on: time for the disk before the firmware's reset */

#define WP_KIND_BRIDGE  1
#define WP_KIND_XHCI    2

#ifndef WP_NO_CODE

/* The console's layout. Only the tests (warmrestart/exp4) define these differently. */
#ifndef WP_XHCI_BUS
#define WP_XHCI_BUS     0x20                        /* bus of the AMD functions (GPU, audio, PSP, xHCI) */
#define WP_PORT_A       WP_BDF (0x00, 1, 1)         /* Salina root port (bus 0x40 behind it is never touched) */
#define WP_PORT_B       WP_BDF (0x00, 8, 1)         /* root port of bus 0x20 */
#endif

#define WP_R32(o)       (*(volatile u32 *)(WP_REC + (o)))
#define WP_R8(o)        (*(volatile u8 *)(WP_REC + (o)))
#define WP_B(bdf)       (((bdf) >> 8) & 0xFF)
#define WP_D(bdf)       (((bdf) >> 3) & 0x1F)
#define WP_F(bdf)       ((bdf) & 7)
#define WP_BDF(b, d, f) (((u32)(b) << 8) | ((u32)(d) << 3) | (u32)(f))
#define wp_rd(bdf, off)     WP_CFG_RD (WP_B (bdf), WP_D (bdf), WP_F (bdf), (off))
#define wp_wr(bdf, off, v)  WP_CFG_WR (WP_B (bdf), WP_D (bdf), WP_F (bdf), (off), (v))

static void wp_bdf_text(u32 bdf)
{
    WP_HEX (WP_B (bdf), 2); WP_PUTS (":"); WP_HEX (WP_D (bdf), 2); WP_PUTS ("."); WP_HEX (WP_F (bdf), 1);
}

/* Saved configuration dword of entry e. */
static u32 wp_g32(u32 e, u32 off)
{
    return WP_R32 (WPG_ENTRY_OFF + e * WPG_ENTRY_SIZE + 0x10 + (off & 0xFC));
}

/* Offset of capability `id` in the saved configuration space of entry e, 0 if absent. */
static u32 wp_gcap(u32 e, u32 id)
{
    u32 ptr, guard;
    if (!(wp_g32 (e, 0x04) & 0x00100000u)) return 0;                 /* status: no capability list */
    ptr = wp_g32 (e, 0x34) & 0xFC;
    for (guard = 0; ptr >= 0x40 && guard < 48; guard++) {
        u32 w = wp_g32 (e, ptr);
        if ((w & 0xFF) == id) return ptr;
        ptr = (w >> 8) & 0xFC;
    }
    return 0;
}

static void wp_save(u32 e, u32 bdf, u32 kind)
{
    u32 base = WPG_ENTRY_OFF + e * WPG_ENTRY_SIZE, off;
    WP_R32 (base) = bdf; WP_R32 (base + 4) = wp_rd (bdf, 0); WP_R32 (base + 8) = kind; WP_R32 (base + 12) = 0;
    for (off = 0; off < 0x100; off += 4) WP_R32 (base + 0x10 + off) = wp_rd (bdf, off);
}

/* First run: the state Sony's firmware and Linux left, which the firmware and Windows are known to start from. */
static void wp_snapshot(void)
{
    u32 bridges[2] = { WP_PORT_A, WP_PORT_B };
    u32 n = 0, i, f, x = 0;
    WP_R32 (WPG_MAGIC_OFF) = 0;
    for (i = 0; i < WPD_SIZE; i += 4) WP_R32 (WPD_OFF + i) = 0;
    for (i = 0; i < 2; i++) {
        u32 id = wp_rd (bridges[i], 0);
        if ((id & 0xFFFF) == 0xFFFF || ((wp_rd (bridges[i], 0x0C) >> 16) & 0x7F) != 1) continue;
        wp_save (n++, bridges[i], WP_KIND_BRIDGE);
    }
    for (f = 0; f < 8 && n < WPG_MAX; f++) {
        u32 bdf = WP_BDF (WP_XHCI_BUS, 0, f);
        if (f == 2) continue;                                        /* the PSP: never probed */
        if ((wp_rd (bdf, 0) & 0xFFFF) == 0xFFFF) continue;
        if ((wp_rd (bdf, 0x08) >> 8) != 0x0C0330) continue;
        wp_save (n++, bdf, WP_KIND_XHCI);
        x++;
    }
    WP_R32 (WPG_COUNT_OFF) = n;
    WP_R32 (WPG_MAGIC_OFF) = WPG_MAGIC;
    WP_PUTS ("warm-restart PCI snapshot: "); WP_HEX (n - x, 1); WP_PUTS (" root ports, "); WP_HEX (x, 1); WP_PUTS (" xHCI\n");
}

/* Bring a function to D0 if its saved configuration has a power-management capability. Returns WPF_* bits; *found gets
 * the PMCSR as found (0xFFFFFFFF = no capability). */
static u32 wp_wake(u32 e, u32 bdf, u32 *found)
{
    u32 cap = wp_gcap (e, 0x01), v, flags = 0;
    *found = 0xFFFFFFFFu;
    if (!cap) return WPF_D0_NOW;
    v = wp_rd (bdf, cap + 4);
    *found = v;
    if ((v & 3) == 0) return WPF_D0_NOW;
    flags = WPF_WAS_ASLEEP;
    wp_wr (bdf, cap + 4, (v & 0x00001E00u) | 0x8000u);               /* D0, PME off, PME status cleared (write 1) */
    WP_DELAY_US (50000);                                             /* the specification asks for 10 ms */
    if ((wp_rd (bdf, cap + 4) & 3) == 0) flags |= WPF_D0_NOW;
    return flags;
}

/* PCI Express device control and link control: low halves only (the high halves are write-1-to-clear status), and
 * only when they differ. mps = the Max_Payload_Size field (bits 7:5) to use instead of the saved one, or 0xFFFFFFFF:
 * an endpoint must not be set up for a larger payload than its root port has now. */
static void wp_restore_express(u32 e, u32 bdf, u32 mps)
{
    u32 cap = wp_gcap (e, 0x10), want;
    if (!cap) return;
    want = wp_g32 (e, cap + 0x08) & 0xFFFF;
    if (mps != 0xFFFFFFFFu) want = (want & ~0xE0u) | (mps & 0xE0);
    if ((wp_rd (bdf, cap + 0x08) & 0xFFFF) != want) wp_wr (bdf, cap + 0x08, want);
    want = wp_g32 (e, cap + 0x10) & 0xFFFF;
    if ((wp_rd (bdf, cap + 0x10) & 0xFFFF) != want) wp_wr (bdf, cap + 0x10, want);
}

/* Max_Payload_Size field the root port of bus 0x20 (00:08.1) has right now; 0xFFFFFFFF if it is not in the snapshot. */
static u32 wp_parent_mps(void)
{
    u32 e, n = WP_R32 (WPG_COUNT_OFF);
    for (e = 0; e < n && e < WPG_MAX; e++) {
        u32 bdf = WP_R32 (WPG_ENTRY_OFF + e * WPG_ENTRY_SIZE), cap;
        if (bdf != WP_PORT_B) continue;
        cap = wp_gcap (e, 0x10);
        if (cap && wp_rd (bdf, 0) == wp_g32 (e, 0)) return wp_rd (bdf, cap + 0x08) & 0xE0;
    }
    return 0xFFFFFFFFu;
}

static void wp_bridge(u32 e, u32 slot)
{
    static const u8 regs[7] = { 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x30 };
    u32 bdf = WP_R32 (WPG_ENTRY_OFF + e * WPG_ENTRY_SIZE), d = WPD_OFF + WPD_BRIDGE_OFF + slot * WPD_BRIDGE_SIZE;
    u32 flags = 0, pm, cmd, bus, mem, i, differs = 0;
    WP_R32 (d) = bdf;
    WP_PUTS ("  root port "); wp_bdf_text (bdf);
    if (wp_rd (bdf, 0) != wp_g32 (e, 0)) { WP_PUTS (" is not the saved device: left alone\n"); WP_R32 (d + 0x14) = 0; return; }
    flags = WPF_ID_OK;
    cmd = wp_rd (bdf, 0x04); bus = wp_rd (bdf, 0x18); mem = wp_rd (bdf, 0x20);
    WP_R32 (d + 4) = cmd; WP_R32 (d + 8) = bus; WP_R32 (d + 12) = mem;
    WP_PUTS (" cmd "); WP_HEX (cmd & 0xFFFF, 4); WP_PUTS (" buses "); WP_HEX (bus & 0xFFFFFF, 6);
    WP_PUTS (" window "); WP_HEX (mem, 8);
    flags |= wp_wake (e, bdf, &pm);
    WP_R32 (d + 16) = pm;
    WP_PUTS (" pm "); WP_HEX (pm & 0xFFFF, 4);
    /* 0x04 command (low half), 0x18 bus numbers, 0x1C I/O window (low half; the high half is status), 0x20-0x30
     * memory windows, 0x3C interrupt line and bridge control. */
    if (((wp_rd (bdf, 0x04) ^ wp_g32 (e, 0x04)) & 0xFFFF) != 0) differs = 1;
    if (((wp_rd (bdf, 0x1C) ^ wp_g32 (e, 0x1C)) & 0xFFFF) != 0) differs = 1;
    if (((wp_rd (bdf, 0x3C) ^ wp_g32 (e, 0x3C)) & 0xFFBF00FFu) != 0) differs = 1;
    for (i = 0; i < 7; i++)
        if (regs[i] != 0x1C && wp_rd (bdf, regs[i]) != wp_g32 (e, regs[i])) differs = 1;
    if (differs || (flags & WPF_WAS_ASLEEP)) {
        for (i = 0; i < 7; i++)
            wp_wr (bdf, regs[i], regs[i] == 0x1C ? (wp_g32 (e, 0x1C) & 0xFFFF) : wp_g32 (e, regs[i]));
        wp_wr (bdf, 0x0C, wp_g32 (e, 0x0C) & 0xFFFF);
        wp_wr (bdf, 0x3C, wp_g32 (e, 0x3C) & 0xFFBF00FFu);           /* never the secondary bus reset bit */
        if (flags & WPF_WAS_ASLEEP) wp_restore_express (e, bdf, 0xFFFFFFFFu);   /* lost with the soft reset */
        wp_wr (bdf, 0x04, (wp_rd (bdf, 0x04) | wp_g32 (e, 0x04)) & 0xFFFF);   /* decoding last; bits are only added */
        flags |= WPF_RESTORED;
        WP_PUTS (" -> restored: cmd "); WP_HEX (wp_rd (bdf, 0x04) & 0xFFFF, 4);
        WP_PUTS (" buses "); WP_HEX (wp_rd (bdf, 0x18) & 0xFFFFFF, 6); WP_PUTS (" window "); WP_HEX (wp_rd (bdf, 0x20), 8);
    } else {
        WP_PUTS (" -> as saved");
    }
    if (flags & WPF_WAS_ASLEEP) WP_PUTS ((flags & WPF_D0_NOW) ? ", woken to D0" : ", STILL NOT D0");
    WP_PUTS ("\n");
    WP_R32 (d + 0x14) = flags;
}

/* Write the saved header of an xHCI back: BARs, cache line, interrupt line, PCI Express control, then decoding. */
static void wp_xhci_header(u32 e, u32 bdf)
{
    u32 cmd = wp_rd (bdf, 0x04), off;
    if ((cmd & 2) && (wp_rd (bdf, 0x10) != wp_g32 (e, 0x10) || wp_rd (bdf, 0x14) != wp_g32 (e, 0x14)))
        wp_wr (bdf, 0x04, cmd & 0xFFFD);                             /* memory decoding off while the BAR moves */
    wp_wr (bdf, 0x0C, wp_g32 (e, 0x0C) & 0xFFFF);
    for (off = 0x10; off <= 0x24; off += 4) wp_wr (bdf, off, wp_g32 (e, off));
    wp_wr (bdf, 0x3C, wp_g32 (e, 0x3C) & 0xFF);
    wp_restore_express (e, bdf, wp_parent_mps ());
    wp_wr (bdf, 0x04, ((wp_rd (bdf, 0x04) | wp_g32 (e, 0x04)) & 0xFFFF) | 0x6);   /* memory + bus master on */
}

/* Vendor-specific capability (0x09; AMD's is 8 bytes at 0x48): whatever the platform firmware set there. Returns 1
 * if something was written. */
static u32 wp_xhci_vendor(u32 e, u32 bdf)
{
    u32 vend = wp_gcap (e, 0x09), len, off, w = 0;
    if (!vend) return 0;
    len = (wp_g32 (e, vend) >> 16) & 0xFF;
    for (off = vend; off < vend + len && off < 0x100; off += 4)
        if (wp_rd (bdf, off) != wp_g32 (e, off)) { wp_wr (bdf, off, wp_g32 (e, off)); w = 1; }
    return w;
}

/* Halt the controller, reset it (HCRST) and wait until it is ready. Returns WPF_MMIO / WPF_HALTED / WPF_RESET;
 * *us = how long the reset took. A function that is asleep or not decoded answers with all ones. */
static u32 wp_xhci_reset(u64 base, u32 *us)
{
    u32 flags = 0, i, caplen;
    u64 op;
    *us = 0;
    caplen = WP_RD8 (base);
    if (!base || WP_RD32 (base) == 0xFFFFFFFFu || caplen < 0x10) return 0;
    flags = WPF_MMIO;
    op = base + caplen;
    if (!(WP_RD32 (op + 4) & 1)) {                                   /* running: Run/Stop = 0, wait for HCHalted */
        WP_WR32 (op, WP_RD32 (op) & ~1u);
        for (i = 0; i < 1000 && !(WP_RD32 (op + 4) & 1); i++) WP_DELAY_US (100);
    }
    if (WP_RD32 (op + 4) & 1) flags |= WPF_HALTED;
    WP_WR32 (op, WP_RD32 (op) | 2);                                  /* HCRST */
    WP_DELAY_US (1000);
    for (*us = 1000; *us < 5000000; *us += 1000) {
        if (!(WP_RD32 (op) & 2) && !(WP_RD32 (op + 4) & 0x800)) { flags |= WPF_RESET; break; }
        WP_DELAY_US (1000);
    }
    return flags;
}

static void wp_xhci(u32 e, u32 slot)
{
    u32 bdf = WP_R32 (WPG_ENTRY_OFF + e * WPG_ENTRY_SIZE), d = WPD_OFF + WPD_XHCI_OFF + slot * WPD_XHCI_SIZE;
    u32 flags = 0, pm, cmd, bar0, bar1, off, i, n, us, sts, r, pmcap = wp_gcap (e, 0x01);
    u64 base, op;
    WP_R32 (d) = bdf;
    WP_PUTS ("  xhci "); wp_bdf_text (bdf);
    if (wp_rd (bdf, 0) != wp_g32 (e, 0)) { WP_PUTS (" is not the saved device: left alone\n"); return; }
    flags = WPF_ID_OK;
    cmd = wp_rd (bdf, 0x04); bar0 = wp_rd (bdf, 0x10); bar1 = wp_rd (bdf, 0x14);
    WP_R32 (d + 4) = cmd; WP_R32 (d + 12) = bar0;
    WP_PUTS (" cmd "); WP_HEX (cmd & 0xFFFF, 4); WP_PUTS (" bar "); WP_HEX (bar1, 8); WP_HEX (bar0, 8);
    flags |= wp_wake (e, bdf, &pm);
    WP_R32 (d + 8) = pm;
    WP_PUTS (" pm "); WP_HEX (pm & 0xFFFF, 4); WP_PUTS (" ->");
    if (flags & WPF_WAS_ASLEEP) WP_PUTS ((flags & WPF_D0_NOW) ? " woken to D0," : " STILL NOT D0,");

    /* Header. After D3hot -> D0 without No_Soft_Reset the function comes back with its BARs and command cleared. */
    cmd = wp_rd (bdf, 0x04); bar0 = wp_rd (bdf, 0x10); bar1 = wp_rd (bdf, 0x14);
    if ((flags & WPF_WAS_ASLEEP) || bar0 != wp_g32 (e, 0x10) || bar1 != wp_g32 (e, 0x14) ||
        (cmd & 0x6) != (wp_g32 (e, 0x04) & 0x6)) {
        wp_xhci_header (e, bdf);
        flags |= WPF_RESTORED;
        WP_PUTS (" header restored,");
    }
    if (wp_xhci_vendor (e, bdf)) { flags |= WPF_VENDOR; WP_PUTS (" vendor bytes restored,"); }
    /* Anything else that differs (not the status half of 0x04, not the power-management dwords): report only. */
    {
        u32 first = 0, count = 0;
        for (off = 0; off < 0x100; off += 4) {
            u32 a = wp_rd (bdf, off), b = wp_g32 (e, off);
            if (off == 0x04) { a &= 0xFFFF; b &= 0xFFFF; }
            if (pmcap && (off == pmcap || off == pmcap + 4)) continue;
            if (a != b) { if (!count) first = off; count++; }
        }
        if (count) {
            flags |= WPF_OTHER_DIFF;
            WP_PUTS (" "); WP_HEX (count, 2); WP_PUTS (" other dwords differ (first at "); WP_HEX (first, 2); WP_PUTS ("),");
        }
    }

    base = ((u64)wp_g32 (e, 0x10) & 0xFFFFFFF0u);
    if ((wp_g32 (e, 0x10) & 6) == 4) base |= (u64)wp_g32 (e, 0x14) << 32;
    /* No controller reset here unless it is needed (boot50w). boot49w reset every xHCI in stage 1; the firmware's own
     * driver resets it again two or three seconds later, and two of three firmware-only loops then ended in Windows'
     * 0x7B (boot disk gone), which a launch from Linux or boot43w's loop (no reset here) almost never does. So: wait
     * for Controller Not Ready to clear (after a wake), stop the controller if it runs, and leave the one reset to
     * the firmware, exactly as after a launch from Linux. A wake from D3hot has already reset the whole function.
     * Reset here only when it answers but will not halt or become ready; power-cycle when it does not answer. */
    r = 0; us = 0;
    {
        u32 caplen = WP_RD8 (base);
        if (WP_RD32 (base) != 0xFFFFFFFFu && caplen >= 0x10) {
            op = base + caplen;
            for (i = 0; i < 1000 && (WP_RD32 (op + 4) & 0x800); i++) WP_DELAY_US (1000);
            if (!(WP_RD32 (op + 4) & 1)) {
                WP_WR32 (op, WP_RD32 (op) & ~1u);
                for (i = 0; i < 1000 && !(WP_RD32 (op + 4) & 1); i++) WP_DELAY_US (100);
            }
            if ((WP_RD32 (op + 4) & 0x801) == 1 && !(WP_RD32 (op) & 2)) r = WPF_MMIO | WPF_HALTED | WPF_LEFT;
        }
    }
    if (!(r & WPF_LEFT)) {
    r = wp_xhci_reset (base, &us);
    if ((r & (WPF_MMIO | WPF_RESET)) != (WPF_MMIO | WPF_RESET) && pmcap) {
        /* Second try: a power-state cycle. These controllers report No_Soft_Reset = 0, so D3hot -> D0 resets the whole
         * function (not a function-level reset: Linux marks FLR as broken on AMD's xHCI of this generation). */
        u32 v = wp_rd (bdf, pmcap + 4);
        WP_PUTS ((r & WPF_MMIO) ? " reset did not finish," : " registers do not answer,");
        wp_wr (bdf, pmcap + 4, (v & 0x00001E00u) | 0x8003u);
        WP_DELAY_US (50000);
        wp_wr (bdf, pmcap + 4, (v & 0x00001E00u) | 0x8000u);
        WP_DELAY_US (50000);
        wp_xhci_header (e, bdf);
        wp_xhci_vendor (e, bdf);
        flags |= WPF_POWER_CYCLE | WPF_RESTORED;
        WP_PUTS (" power-cycled (D3hot -> D0), header restored,");
        r = wp_xhci_reset (base, &us);
    }
    }
    flags |= r;
    WP_R32 (d + 24) = us;
    if (!(flags & WPF_MMIO)) {
        WP_PUTS (" REGISTERS DO NOT ANSWER\n");
        WP_R32 (d + 16) = flags;
        return;
    }
    op = base + WP_RD8 (base);
    /* Ports: power on where it is off (PORTSC written with its write-1-to-clear and action bits as 0). */
    n = WP_RD32 (base + 4) >> 24;
    WP_R32 (d + 28) = n;
    for (i = 0; i < n && i < 16; i++) {
        u32 p = WP_RD32 (op + 0x400 + i * 0x10);
        if (p != 0xFFFFFFFFu && !(p & 0x200)) {
            WP_WR32 (op + 0x400 + i * 0x10, (p & 0x0E00C3E0u) | 0x200);
            flags |= WPF_PORT_POWER;
        }
    }
    if (flags & (WPF_WAS_ASLEEP | WPF_RESET | WPF_POWER_CYCLE | WPF_PORT_POWER))
        WP_DELAY_US (300000);                                        /* let the ports see what is plugged in */
    sts = WP_RD32 (op + 4);
    WP_R32 (d + 20) = sts;
    if (flags & WPF_LEFT) {
        WP_PUTS (" halted, not reset here (the firmware does that)");
    } else {
        WP_PUTS ((flags & WPF_RESET) ? " reset ok after " : " RESET DID NOT FINISH in "); WP_HEX (us, 6); WP_PUTS (" us");
    }
    WP_PUTS (", usbsts "); WP_HEX (sts, 8);
    if (flags & WPF_PORT_POWER) WP_PUTS (", port power switched on");
    WP_PUTS ("\n      ports ("); WP_HEX (n, 2); WP_PUTS ("):");
    for (i = 0; i < n && i < 8; i++) {
        u32 p = WP_RD32 (op + 0x400 + i * 0x10);
        if (i < 4) WP_R32 (d + 32 + i * 4) = p;
        WP_PUTS (" "); WP_HEX (p, 8);
    }
    WP_PUTS ("  (bit 0 = something connected)\n");
    WP_R32 (d + 16) = flags;
}

/* Base of the operational registers of the xHCI in snapshot entry e (its saved BAR), 0 if `flags` say it does not answer. */
static u64 wp_xhci_op(u32 e, u32 flags, u64 *cap)
{
    u64 base = ((u64)wp_g32 (e, 0x10) & 0xFFFFFFF0u);
    if ((wp_g32 (e, 0x10) & 6) == 4) base |= (u64)wp_g32 (e, 0x14) << 32;
    *cap = base;
    return (flags & WPF_MMIO) ? base + WP_RD8 (base) : 0;
}

/* Re-plug the disks (boot52w). On boot50w/51w some Windows starts after a Restart never saw the boot disk (0x7B even
 * with a 60 s wait) although this stage and the firmware found the same state every time and the firmware read the
 * disk; after a power cut it always works. What a power cut does and a warm restart did not: take the disk's USB
 * bridge through a disconnect. So, on controllers that have per-port power control (HCCPARAMS1.PPC; the console's
 * two report it): switch power off on every port that has a high-speed or SuperSpeed device and on every empty
 * port (its USB 2 / USB 3 partner; VBUS drops only when both are off), wait, switch it on again and wait for the
 * device to show up. Ports with a low/full-speed device (keyboard receiver, the internal Bluetooth) are left alone.
 * PORTSC is written with its write-1-to-clear and action bits as 0 and the wake bits Windows left cleared.
 * Returns 1 if any port was switched. */
static u32 wp_replug(u32 n, u32 x)
{
    u32 e, i, k, any = 0, t, watch[4] = { 0, 0, 0, 0 }, off[4] = { 0, 0, 0, 0 }, np[4] = { 0, 0, 0, 0 };
    u64 op[4] = { 0, 0, 0, 0 }, cap;
    for (e = 0, i = 0; e < n && i < x && i < 4; e++) {
        u32 d = WPD_OFF + WPD_XHCI_OFF + i * WPD_XHCI_SIZE;
        if (WP_R32 (WPG_ENTRY_OFF + e * WPG_ENTRY_SIZE + 8) != WP_KIND_XHCI) continue;
        op[i] = wp_xhci_op (e, WP_R32 (d + 16), &cap);
        if (op[i] && (WP_RD32 (cap + 0x10) & 8)) {                   /* HCCPARAMS1.PPC */
            np[i] = WP_RD32 (cap + 4) >> 24;
            if (np[i] > 16) np[i] = 16;
            for (k = 0; k < np[i]; k++) {
                u32 p = WP_RD32 (op[i] + 0x400 + k * 0x10);
                if (p == 0xFFFFFFFFu) continue;
                if ((p & 1) && ((p >> 10) & 0xF) >= 3) watch[i] |= 1u << k;      /* a disk: high speed or faster */
                if (!(p & 1) || ((p >> 10) & 0xF) >= 3) off[i] |= 1u << k;
            }
            if (!watch[i]) off[i] = 0;                               /* nothing to re-plug on this controller */
        } else {
            op[i] = 0;
        }
        i++;
    }
    for (i = 0; i < 4; i++)
        for (k = 0; k < np[i]; k++)
            if (off[i] & (1u << k)) {
                u32 p = WP_RD32 (op[i] + 0x400 + k * 0x10);
                WP_WR32 (op[i] + 0x400 + k * 0x10, p & 0x0000C000u);             /* PP = 0 */
                any = 1;
            }
    if (!any) return 0;
    WP_PUTS ("  re-plugging the disks: port power off for "); WP_HEX (WP_PP_OFF_MS / 1000, 1); WP_PUTS (" s\n");
    for (t = 0; t < WP_PP_OFF_MS / 100; t++) WP_DELAY_US (100000);
    for (i = 0; i < 4; i++) {
        u32 d = WPD_OFF + WPD_XHCI_OFF + i * WPD_XHCI_SIZE, gone = 1;
        if (!off[i]) continue;
        WP_R32 (d + 16) = WP_R32 (d + 16) | WPF_PP_CYCLE;
        for (k = 0; k < np[i]; k++)
            if ((watch[i] & (1u << k)) && (WP_RD32 (op[i] + 0x400 + k * 0x10) & 1)) gone = 0;
        if (gone) WP_R32 (d + 16) = WP_R32 (d + 16) | WPF_PP_GONE;
        for (k = 0; k < np[i]; k++)
            if (off[i] & (1u << k)) {
                u32 p = WP_RD32 (op[i] + 0x400 + k * 0x10);
                WP_WR32 (op[i] + 0x400 + k * 0x10, (p & 0x0000C000u) | 0x200);   /* PP = 1 */
            }
    }
    for (t = 0; t < WP_PP_WAIT_MS / 10; t++) {
        u32 all = 1;
        for (i = 0; i < 4; i++) {
            u32 d = WPD_OFF + WPD_XHCI_OFF + i * WPD_XHCI_SIZE, back = 1;
            if (!off[i] || (WP_R32 (d + 16) & WPF_PP_BACK)) continue;
            for (k = 0; k < np[i]; k++)
                if ((watch[i] & (1u << k)) && !(WP_RD32 (op[i] + 0x400 + k * 0x10) & 1)) back = 0;
            if (back) { WP_R32 (d + 16) = WP_R32 (d + 16) | WPF_PP_BACK; WP_R32 (d + 24) = t * 10000; }
            else all = 0;
        }
        if (all) break;
        WP_DELAY_US (10000);
    }
    for (i = 0; i < 4; i++) {
        u32 d = WPD_OFF + WPD_XHCI_OFF + i * WPD_XHCI_SIZE, f = WP_R32 (d + 16);
        if (!off[i]) continue;
        WP_PUTS ("      "); wp_bdf_text (WP_R32 (d)); WP_PUTS (" ports "); WP_HEX (off[i], 4);
        WP_PUTS ((f & WPF_PP_GONE) ? ": disk gone while off, " : ": DISK STAYED CONNECTED while off, ");
        if (f & WPF_PP_BACK) { WP_PUTS ("back after 0x"); WP_HEX (WP_R32 (d + 24) / 1000, 4); WP_PUTS (" ms\n"); }
        else { WP_PUTS ("NOT BACK within the wait\n"); WP_R32 (d + 24) = 0xFFFFFFFFu; }
    }
    for (t = 0; t < WP_PP_AFTER_MS / 100; t++) WP_DELAY_US (100000);
    return 1;
}

/* Warm run. `run` is the stage run number (2 = first warm run), for the record only. */
static void wp_restore(u32 run)
{
    u32 n, e, f, x = 0, b = 0, map = 0, i;
    for (i = 0; i < WPD_SIZE; i += 4) WP_R32 (WPD_OFF + i) = 0;
    if (WP_R32 (WPG_MAGIC_OFF) != WPG_MAGIC || WP_R32 (WPG_COUNT_OFF) > WPG_MAX) {
        WP_PUTS ("warm-restart PCI restore: no snapshot from the first run; nothing restored\n");
        return;
    }
    n = WP_R32 (WPG_COUNT_OFF);
    WP_PUTS ("warm-restart PCI restore (found -> done; saved state = first run):\n");
    for (e = 0; e < n; e++)
        if (WP_R32 (WPG_ENTRY_OFF + e * WPG_ENTRY_SIZE + 8) == WP_KIND_BRIDGE && b < 2) wp_bridge (e, b++);
    /* How Windows left every function of bus 0x20 (read only; the PSP is not probed). */
    WP_PUTS ("  bus "); WP_HEX (WP_XHCI_BUS, 2); WP_PUTS (" as found:");
    for (f = 0; f < 8; f++) {
        u32 bdf = WP_BDF (WP_XHCI_BUS, 0, f), id, ptr, guard, st = 0, nib;
        if (f == 2) continue;
        id = wp_rd (bdf, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        nib = 8 | ((wp_rd (bdf, 0x04) & 2) ? 4 : 0);
        if (wp_rd (bdf, 0x04) & 0x00100000u) {
            ptr = wp_rd (bdf, 0x34) & 0xFC;
            for (guard = 0; ptr >= 0x40 && guard < 48; guard++) {
                u32 w = wp_rd (bdf, ptr);
                if ((w & 0xFF) == 0x01) { st = wp_rd (bdf, ptr + 4) & 3; break; }
                ptr = (w >> 8) & 0xFC;
            }
        }
        nib |= st;
        map |= nib << (f * 4);
        WP_PUTS (" ."); WP_HEX (f, 1); WP_PUTS (" D"); WP_HEX (st, 1); WP_PUTS ((nib & 4) ? "" : "(mem off)");
    }
    WP_PUTS ("\n");
    for (e = 0; e < n; e++)
        if (WP_R32 (WPG_ENTRY_OFF + e * WPG_ENTRY_SIZE + 8) == WP_KIND_XHCI && x < 4) wp_xhci (e, x++);
    /* A controller that was woken, reset or had port power switched on has just dropped the link to its disk. The
     * firmware's driver resets it once more as soon as it starts; give the disk's bridge time to come up first. */
    for (i = 0, f = 0; i < x; i++)
        if (WP_R32 (WPD_OFF + WPD_XHCI_OFF + i * WPD_XHCI_SIZE + 16) & (WPF_WAS_ASLEEP | WPF_RESET | WPF_POWER_CYCLE | WPF_PORT_POWER)) f = 1;
    /* Also after anything but the firmware-only loop (record flag bit 0, REC_FLAGS at +0x2C): a Windows that died while
     * starting (stop error with automatic restart) leaves the controllers awake, but reset them only seconds ago. */
    if (x && !(WP_R32 (0x2C) & 1)) f = 1;
    if (x && !(WP_R32 (0x2C) & 1) && wp_replug (n, x)) f = 2;       /* not in the firmware-only loop */
    if (f) {
        if (f == 1) {
            WP_PUTS ("  links were reset: waiting "); WP_HEX (WP_SETTLE_MS / 1000, 1); WP_PUTS (" s before the firmware starts\n");
            for (i = 0; i < WP_SETTLE_MS / 100; i++) WP_DELAY_US (100000);
        }
        for (e = 0, i = 0; e < n && i < x; e++) {
            u32 d = WPD_OFF + WPD_XHCI_OFF + i * WPD_XHCI_SIZE, p, k;
            u64 base;
            if (WP_R32 (WPG_ENTRY_OFF + e * WPG_ENTRY_SIZE + 8) != WP_KIND_XHCI) continue;
            i++;
            if (!(WP_R32 (d + 16) & WPF_MMIO)) continue;
            base = ((u64)wp_g32 (e, 0x10) & 0xFFFFFFF0u);
            if ((wp_g32 (e, 0x10) & 6) == 4) base |= (u64)wp_g32 (e, 0x14) << 32;
            base += WP_RD8 (base);
            WP_PUTS ("      "); wp_bdf_text (WP_R32 (d)); WP_PUTS (" ports now:");
            for (k = 0; k < WP_R32 (d + 28) && k < 4; k++) {
                p = WP_RD32 (base + 0x400 + k * 0x10);
                WP_R32 (d + 32 + k * 4) = p;                         /* the report keeps the values after the wait */
                WP_PUTS (" "); WP_HEX (p, 8);
            }
            WP_PUTS ("\n");
        }
    }
    WP_R32 (WPD_OFF + 4) = run; WP_R32 (WPD_OFF + 8) = x; WP_R32 (WPD_OFF + 12) = map;
    WP_R32 (WPD_OFF) = WPD_MAGIC;
}

#endif /* WP_NO_CODE */
#endif
