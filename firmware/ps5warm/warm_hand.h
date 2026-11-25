/* SPDX-License-Identifier: MIT
 * Warm restart: hand the restart over to the boot CPU when ResetSystem was called on another one (2026-11-24).
 *
 * Windows' restart after a stop error calls the firmware on the CPU that hit the stop (HalReturnToFirmware does not
 * move to the boot CPU). The reset stub must run on the boot CPU, so this CPU:
 *   1. finds the OS's NMI entry (interrupt gate 2 of the table this CPU uses; every CPU of the OS has the same one),
 *   2. overwrites its first 14 bytes with a jump to Ps5WarmNmiEntry (CR0.WP off for the write),
 *   3. sends the boot CPU an NMI from our own page tables and waits (Ps5WarmKick, Ps5WarmAsm.S).
 * The boot CPU, frozen by the stop error with interrupts off, takes the NMI, finds the offer and goes into the stub
 * exactly as if ResetSystem had been called on it. If it does not within the time given (it may be inside an NMI
 * already), the offer is withdrawn, the 14 bytes are put back and the caller goes on to the ordinary restart.
 *
 * Shared by the firmware (Ps5Warm.c) and the emulator test (warmrestart/exp5). The including file provides:
 *   u8 u32 u64, WH_SIDT(p) (10-byte limit/base), WH_RDCR0() -> u64, WH_WRCR0(v),
 *   WH_KICK(pml4, hand_phys, bsp, ticks) -> u32 (Ps5WarmKick)
 *
 * Copyright (c) 2026 ps5win contributors
 */
#ifndef PS5_WARM_HAND_H_
#define PS5_WARM_HAND_H_

#define WH_OFFERED   1u
#define WH_TAKEN     2u
#define WH_WITHDRAWN 3u
#define WH_PATCH     14u

/* Address of the NMI entry of the interrupt table this CPU uses, 0 if it has no usable gate 2. */
static u64 wh_nmi_entry(void)
{
    u8  idtr[10], *gate;
    u64 base = 0, target;
    u32 i, limit;
    WH_SIDT (idtr);
    limit = (u32)idtr[0] | ((u32)idtr[1] << 8);
    for (i = 0; i < 8; i++) base |= (u64)idtr[2 + i] << (8 * i);
    if (limit < 3 * 16 - 1 || base == 0) return 0;
    gate = (u8 *)(unsigned long long)base + 2 * 16;
    if ((gate[5] & 0x9F) != 0x8E) return 0;                          /* present 64-bit interrupt gate */
    target = (u64)gate[0] | ((u64)gate[1] << 8) | ((u64)gate[6] << 16) | ((u64)gate[7] << 24);
    for (i = 0; i < 4; i++) target |= (u64)gate[8 + i] << (32 + 8 * i);
    return target;
}

static void wh_write(u8 *at, const u8 *bytes)
{
    u64 cr0 = WH_RDCR0 ();
    u32 i;
    WH_WRCR0 (cr0 & ~0x10000ull);                                    /* the OS maps its code read-only */
    for (i = 0; i < WH_PATCH; i++) ((volatile u8 *)at)[i] = bytes[i];
    WH_WRCR0 (cr0);
}

/* Offer the restart to the boot CPU. Returns 0 when it did not take over (everything put back); does not return
 * when it did. hand = the hand-over word as this code reaches it, hand_phys = its physical address, slots = the
 * three quadwords Ps5WarmNmiEntry reads (gPs5WarmHand), entry = address of Ps5WarmNmiEntry as the OS maps it. */
static u32 wh_handover(u64 pml4_phys, u64 blob_phys, u64 hand_phys, volatile u32 *hand, volatile u64 *slots, u64 entry,
                       u32 bsp_id, u64 ticks)
{
    u8  save[WH_PATCH], jump[WH_PATCH], *at;
    u64 target = wh_nmi_entry ();
    u32 i;
    if (target == 0 || bsp_id > 0xFF) return 0;
    at = (u8 *)(unsigned long long)target;
    for (i = 0; i < WH_PATCH; i++) save[i] = ((volatile u8 *)at)[i];
    jump[0] = 0xFF; jump[1] = 0x25; jump[2] = jump[3] = jump[4] = jump[5] = 0;        /* jmp qword ptr [rip + 0] */
    for (i = 0; i < 8; i++) jump[6 + i] = (u8)(entry >> (8 * i));
    slots[0] = (u64)(unsigned long long)hand; slots[1] = pml4_phys; slots[2] = blob_phys;
    *hand = WH_OFFERED;
    wh_write (at, jump);
    WH_KICK (pml4_phys, hand_phys, bsp_id, ticks);                   /* comes back only when the offer was withdrawn */
    wh_write (at, save);
    return 0;
}

#endif
