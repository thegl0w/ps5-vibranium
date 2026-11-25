/* SPDX-License-Identifier: MIT
 * Warm restart, console version: the machine code. Based on warmrestart/exp2/warm_asm.S (proven under OVMF and under
 * Windows in KVM); the tail jumps into stage 1 instead of the reset vector, and the PS5's devices are quietened.
 *
 * Ps5WarmSwitch  in the runtime driver's code (the OS maps it): cli; load our page tables; jump to the blob.
 * Ps5WarmBlob    position-independent, copied to block + 0x1000 at boot and run there at its physical address.
 *
 * Steps, each with a coloured square on the screen (40x40 at y = 40, x = 40 + 56 * slot) and REC_STEP = slot:
 *   (scan-out first put back on the saved frame buffer, so the squares can be seen)
 *   1 white    on our own GDT and page tables
 *   2 yellow   every other CPU sent INIT, local APIC back in xAPIC mode, its timer masked
 *   3..6 cyan  one per xHCI controller halted (red: it did not report Halted in time); buses 00 and 20 only
 *   7 green    GPU: gfx command processor, compute (MEC), both SDMA engines and the interrupt rings halted
 *   8 magenta  HD-audio controller(s) held in reset (grey: none found)
 *   9 orange   Ethernet (Sony GbE behind root port 00:01.1) TX/RX DMA stopped (grey: bus 0x40 not routed, skipped;
 *              red: a stop request did not finish)
 *  10 blue     record updated, caches written back, jumping to stage 1
 * Never touched: anything on bus 0x40 through configuration space (function 40:00.0 is the console's NVMe), the PSP
 * (20:00.2), bus-master bits, PSP/SMU registers or messages, the ICC mailbox.
 */
#include "Ps5Warm.h"

    .intel_syntax noprefix

    .text
    .globl Ps5WarmSwitch, Ps5WarmKick, Ps5WarmNmiEntry
    .balign 256                             /* the three routines below share one page: it is the only page of this
                                               driver that our own page tables map at the OS's virtual address */
Ps5WarmSwitch:                              /* rcx = physical address of our PML4, rdx = physical address of the blob */
    cli
    mov     cr3, rcx
    jmp     rdx

/* Hand-over to the boot CPU (2026-11-24). Windows calls ResetSystem on whichever CPU hit a stop error; the stub can
 * only run on the boot CPU (it parks every other CPU with INIT, and an INIT sends the boot CPU to the reset vector).
 * So a reset that arrives on another CPU offers the restart to the boot CPU: the C side (warm_hand.h) points the
 * NMI entry of the OS at Ps5WarmNmiEntry and calls Ps5WarmKick, which sends the boot CPU an NMI and waits.
 *
 * Ps5WarmKick (rcx = physical address of our PML4, rdx = physical address of the hand-over word, r8d = APIC id of the
 * boot CPU, r9 = TSC ticks to wait). Hand-over word: 1 offered, 2 taken by the boot CPU, 3 withdrawn.
 * Returns eax = 0 on the caller's page tables when the boot CPU did not take over in time. Does not return when it
 * did: this CPU then waits for the stub's INIT. No stack is used while our page tables are loaded. */
Ps5WarmKick:
    cli
    mov     r10, cr3
    mov     r11, rdx
    mov     cr3, rcx                        /* our tables: low memory identity-mapped, this page where the OS has it */
    mov     ecx, 0x1B
    rdmsr
    test    eax, 0x400
    jnz     1f
    and     eax, 0xFFFFF000                 /* xAPIC: registers at the APIC base (below 4 GiB) */
    mov     ecx, r8d
    shl     ecx, 24
    mov     dword ptr [rax + 0x310], ecx
    mov     dword ptr [rax + 0x300], 0x4400 /* NMI, one destination, edge, assert */
    jmp     2f
1:  mov     ecx, 0x830                      /* x2APIC: the interrupt command register is one MSR */
    mov     eax, 0x4400
    mov     edx, r8d
    wrmsr
2:  rdtsc
    shl     rdx, 32
    or      rax, rdx
    add     r9, rax                         /* deadline */
3:  cmp     dword ptr [r11], 2
    je      5f
    pause
    rdtsc
    shl     rdx, 32
    or      rax, rdx
    cmp     rax, r9
    jb      3b
    mov     eax, 1
    mov     ecx, 3
    lock cmpxchg dword ptr [r11], ecx       /* withdraw the offer, unless the boot CPU took it this instant */
    jne     5f
    mov     cr3, r10
    xor     eax, eax
    ret
5:  cli                                     /* taken: the boot CPU runs the stub and parks this CPU with INIT */
    hlt
    jmp     5b

/* Ps5WarmNmiEntry: where the patched NMI entry sends a CPU, still on the OS's page tables. Only the boot CPU, and
 * only while the offer stands, goes on into the stub (as if ResetSystem had been called on it); anything else stops. */
Ps5WarmNmiEntry:
    mov     ecx, 0x1B
    rdmsr
    test    eax, 0x100                      /* boot CPU? */
    jz      9f
    mov     rdx, [rip + gPs5WarmHand]       /* the hand-over word as this code reaches it */
    mov     eax, 1
    mov     ecx, 2
    lock cmpxchg dword ptr [rdx], ecx
    jne     9f
    mov     rcx, [rip + gPs5WarmHand + 8]   /* our PML4 */
    mov     rdx, [rip + gPs5WarmHand + 16]  /* the blob */
    jmp     Ps5WarmSwitch
9:  cli
    hlt
    jmp     9b
    .org    Ps5WarmSwitch + 256, 0xCC       /* fails to assemble if the three routines outgrow the 256 bytes that
                                               keep them on one page */

    .data
    .globl gPs5WarmHand
    .balign 8
gPs5WarmHand:                               /* written by the C side before the NMI: address of the hand-over word as
                                               the OS maps it, physical address of our PML4, physical address of the blob */
    .quad   0, 0, 0
    .text

    .equ TSC_PER_US, 1597                   /* the console's TSC runs at 1596.3 MHz */
    .equ GPU_CP_ME_CNTL, 0x86D8             /* dword 0x21B6 (GC0 0x1260 + 0xF56) */
    .equ GPU_CP_MEC_CNTL, 0x8234            /* dword 0x208D */
    .equ GPU_CP_STAT, 0x8680                /* dword 0x21A0 */
    .equ GPU_SDMA0_CNTL, 0x49F0             /* dword 0x127C */
    .equ GPU_SDMA0_F32_CNTL, 0x4A28         /* dword 0x128A */
    .equ GPU_SDMA0_GFX_RB_CNTL, 0x4B80      /* dword 0x12E0 */
    .equ GPU_SDMA0_GFX_IB_CNTL, 0x4BA8      /* dword 0x12EA */
    .equ GPU_SDMA_STRIDE, 0x1800            /* SDMA1 = SDMA0 + dword 0x600 */
    .equ GPU_IH_RB_CNTL, 0x4480             /* dword 0x1120 (OSS 0x10A0 + 0x80) */
    .equ GPU_IH_RB_RPTR, 0x448C
    .equ GPU_IH_RB_WPTR, 0x4490
    .equ GPU_IH_RB_CNTL_RING1, 0x44B0
    .equ GPU_IH_RB_CNTL_RING2, 0x44E0
    .equ GPU_OTG0_UPDATE_LOCK, 0x1412C      /* dword 0x504B */
    .equ ETH_BASE, 0x85000000               /* Sony GbE BAR0 (fixed, behind root port 00:01.1) */

    .macro CFGRD reg                        /* eax = configuration dword \reg of the function in r10d */
    mov     eax, r10d
    or      eax, \reg
    mov     dx, 0xCF8
    out     dx, eax
    mov     dx, 0xCFC
    in      eax, dx
    .endm
    .macro MARK slot, color
    mov     ecx, \slot
    mov     edx, \color
    call    Mark
    .endm
    .macro DELAY us
    mov     ecx, \us
    call    DelayUs
    .endm

    .section .rodata
    .globl Ps5WarmBlob, Ps5WarmBlobEnd
    .balign 16
Ps5WarmBlob:
    lea     rbx, [rip + Ps5WarmBlob]
    lea     rbp, [rbx - BLK_BLOB]           /* rbp = block */
    lea     rsp, [rbp + BLK_STACK_TOP]
    MARK    0, 0xFF80C0FF                   /* light blue: running here, on our page tables (diagnostic) */
    lgdt    [rbp + BLK_GDTR]
    lidt    [rbp + BLK_IDTR]                /* diagnostic: a fault shows its vector and halts instead of a triple fault */
    mov     eax, 0x620                      /* CR4 = PAE | OSFXSR | OSXMMEXCPT (drops PCID, SMEP, SMAP, PGE, ...) */
    mov     cr4, rax
    push    0x18                            /* 64-bit code segment of our GDT */
    lea     rax, [rip + 1f]
    push    rax
    .byte   0x48, 0xCB                      /* retfq */
1:  mov     eax, 0x10
    mov     ds, eax
    mov     es, eax
    mov     ss, eax
    xor     eax, eax
    mov     fs, eax
    mov     gs, eax

    /* ---- GPU (20:00.0, 1002:13FC): register BAR5, then the scan-out back on the saved frame buffer ---- */
    xor     r13d, r13d
    mov     r10d, 0x80200000                /* bus 0x20, device 0, function 0 */
    CFGRD   0x00
    cmp     eax, 0x13FC1002
    jne     20f
    CFGRD   0x04
    test    al, 2                           /* memory decoding on? */
    jz      20f
    CFGRD   0x24                            /* BAR5: registers, 32-bit */
    test    al, 1
    jnz     20f
    and     eax, 0xFFFFFFF0
    jz      20f
    mov     r13d, eax
    mov     r12, [rbp + BLK_FB]
    test    r12, r12
    jz      20f
    mov     dword ptr [r13 + GPU_OTG0_UPDATE_LOCK], 1
    mov     r14d, 10000                     /* the lock is confirmed within microseconds; go on regardless after 10 ms */
11: test    dword ptr [r13 + GPU_OTG0_UPDATE_LOCK], 0x100
    jnz     12f
    DELAY   1
    dec     r14d
    jnz     11b
12: lea     rsi, [rip + PipeTable]
    mov     r14d, 4
13: mov     eax, [rsi + 24]                 /* DCHUBP_CNTL */
    mov     ecx, [r13 + rax]
    test    ecx, 1                          /* HUBP_BLANK_EN: pipe not showing */
    jnz     14f
    mov     eax, [rsi + 16]                 /* viewport dimension: width and height both non-zero */
    mov     ecx, [r13 + rax]
    test    ecx, 0x3FFF
    jz      14f
    test    ecx, 0x3FFF0000
    jz      14f
    mov     rcx, r12
    shr     rcx, 32
    mov     eax, [rsi + 8]
    mov     [r13 + rax], ecx                /* address high, then low (libps5gpu's order) */
    mov     eax, [rsi]
    mov     [r13 + rax], r12d
14: add     rsi, 32
    dec     r14d
    jnz     13b
    mov     dword ptr [r13 + GPU_OTG0_UPDATE_LOCK], 0
20: MARK    1, 0xFFFFFFFF

    /* ---- every other CPU: INIT (they then wait for a start-up IPI and touch no memory) ---- */
    mov     ecx, 0x1B
    rdmsr
    mov     r12d, eax                       /* APIC base, low half */
    test    eax, 0x400
    jnz     21f
    mov     edi, 0xFEE00000                 /* xAPIC: ICR high, then ICR low = all excluding self, INIT, assert */
    mov     dword ptr [rdi + 0x310], 0
    mov     dword ptr [rdi + 0x300], 0x000C4500
    jmp     22f
21: mov     ecx, 0x830                      /* x2APIC: ICR is one MSR */
    mov     eax, 0x000C4500
    xor     edx, edx
    wrmsr
22: DELAY   40000
    test    r12d, 0x400                     /* x2APIC -> disabled -> xAPIC (the only legal way back; stage 1 and the */
    jz      23f                             /* firmware use the xAPIC registers) */
    mov     ecx, 0x1B
    rdmsr
    and     eax, 0xFFFFF3FF                 /* EN and EXTD off */
    wrmsr
    or      eax, 0x800                      /* EN on, EXTD off */
    wrmsr
23: mov     edi, 0xFEE00000
    mov     dword ptr [rdi + 0x320], 0x10000   /* timer masked */
    MARK    2, 0xFFFFFF00

    /* ---- xHCI controllers (class 0C0330) on buses 00 and 20: Run/Stop = 0, wait for HCHalted ---- */
    mov     r15d, 3                         /* next screen slot */
    xor     r9d, r9d                        /* bus:dev:fn as bus << 8 | dev << 3 | fn */
30: mov     eax, r9d
    shr     eax, 8
    cmp     eax, 0x00
    je      31f
    cmp     eax, 0x20
    jne     39f
31: cmp     r9d, 0x2002                     /* 20:00.2 = the PSP: never probed */
    je      39f
    mov     r10d, r9d
    shl     r10d, 8
    or      r10d, 0x80000000
    CFGRD   0x00
    cmp     ax, 0xFFFF
    jne     32f
    test    r9d, 7                          /* nothing here: if function 0, skip the whole device */
    jnz     39f
    or      r9d, 7
    jmp     39f
32: CFGRD   0x08
    shr     eax, 8
    cmp     eax, 0x0C0330
    jne     38f
    CFGRD   0x04
    test    al, 2
    jz      38f
    CFGRD   0x10
    mov     r11d, eax
    test    al, 1
    jnz     38f
    and     eax, 6
    cmp     eax, 4
    jne     33f
    CFGRD   0x14                            /* 64-bit BAR: our tables map the first 1 TiB */
    cmp     eax, 0x100
    jae     38f
    shl     rax, 32
    and     r11d, 0xFFFFFFF0
    or      r11, rax
    jmp     34f
33: and     r11d, 0xFFFFFFF0
34: test    r11, r11
    jz      38f
    movzx   eax, byte ptr [r11]             /* CAPLENGTH */
    add     r11, rax                        /* operational registers */
    mov     eax, [r11]
    and     eax, 0xFFFFFFFE
    mov     [r11], eax                      /* USBCMD.Run/Stop = 0 */
    mov     r14d, 100000                    /* up to 100 ms */
35: test    dword ptr [r11 + 4], 1          /* USBSTS.HCHalted */
    jnz     36f
    DELAY   1
    dec     r14d
    jnz     35b
    mov     ecx, r15d
    mov     edx, 0xFFFF0000                 /* red: did not halt in time */
    call    Mark
    jmp     37f
36: mov     ecx, r15d
    mov     edx, 0xFF00FFFF                 /* cyan */
    call    Mark
37: cmp     r15d, 6
    jae     38f
    inc     r15d
38: test    r9d, 7                          /* function 0 of a single-function device: skip functions 1-7 */
    jnz     39f
    CFGRD   0x0C
    test    eax, 0x00800000
    jnz     39f
    or      r9d, 7
39: inc     r9d
    cmp     r9d, 0x2100
    jb      30b

    /* ---- GPU engines: libps5gpu's step 1 (CyanDirectLoad::quiesce without the SDMA soft reset) ---- */
    test    r13, r13
    jz      49f
    mov     eax, [r13 + GPU_IH_RB_CNTL]
    and     eax, 0xFFFDFFFE                 /* RB_ENABLE and ENABLE_INTR off */
    mov     [r13 + GPU_IH_RB_CNTL], eax
    mov     dword ptr [r13 + GPU_IH_RB_RPTR], 0
    mov     dword ptr [r13 + GPU_IH_RB_WPTR], 0
    and     dword ptr [r13 + GPU_IH_RB_CNTL_RING1], 0xFFFFFFFE
    and     dword ptr [r13 + GPU_IH_RB_CNTL_RING2], 0xFFFFFFFE
    or      dword ptr [r13 + GPU_CP_ME_CNTL], 0x15000000      /* ME, PFP, CE halt */
    mov     r14d, 100000                    /* CP_STAT idle within 100 ms (go on regardless) */
41: cmp     dword ptr [r13 + GPU_CP_STAT], 0
    je      42f
    DELAY   1
    dec     r14d
    jnz     41b
42: mov     dword ptr [r13 + GPU_CP_MEC_CNTL], 0x50000000     /* MEC ME1 + ME2 halt */
    DELAY   50
    xor     esi, esi
43: and     dword ptr [r13 + rsi + GPU_SDMA0_CNTL], 0xFFFBFFFF            /* auto context switch off */
    and     dword ptr [r13 + rsi + GPU_SDMA0_GFX_RB_CNTL], 0xFFFFFFFE     /* ring off */
    and     dword ptr [r13 + rsi + GPU_SDMA0_GFX_IB_CNTL], 0xFFFFFFFE     /* indirect buffers off */
    or      dword ptr [r13 + rsi + GPU_SDMA0_F32_CNTL], 1                 /* halt */
    add     esi, GPU_SDMA_STRIDE
    cmp     esi, 2 * GPU_SDMA_STRIDE
    jb      43b
    MARK    7, 0xFF00FF00
    jmp     50f
49: MARK    7, 0xFF808080                   /* grey: no GPU registers found */

    /* ---- HD audio (class 0403xx) on bus 20: GCTL.CRST = 0 (controller in reset, every stream stopped) ---- */
50: mov     r15d, 0xFF808080                /* grey unless one is found */
    mov     r9d, 0x2000
51: cmp     r9d, 0x2002
    je      58f
    mov     r10d, r9d
    shl     r10d, 8
    or      r10d, 0x80000000
    CFGRD   0x00
    cmp     ax, 0xFFFF
    je      58f
    CFGRD   0x08
    shr     eax, 16
    cmp     eax, 0x0403
    jne     58f
    CFGRD   0x04
    test    al, 2
    jz      58f
    CFGRD   0x10
    test    al, 1
    jnz     58f
    mov     r11d, eax
    and     eax, 6
    mov     edx, 0
    cmp     eax, 4
    jne     52f
    CFGRD   0x14
    cmp     eax, 0x100                      /* our tables map the first 1 TiB */
    jae     58f
    mov     edx, eax
52: shl     rdx, 32
    and     r11d, 0xFFFFFFF0
    or      r11, rdx
    jz      58f
    and     dword ptr [r11 + 8], 0xFFFFFFFE /* GCTL.CRST = 0 */
    mov     r14d, 10000
53: test    dword ptr [r11 + 8], 1
    jz      54f
    DELAY   1
    dec     r14d
    jnz     53b
54: mov     r15d, 0xFFFF00FF                /* magenta */
58: inc     r9d
    cmp     r9d, 0x2100
    jb      51b
    mov     ecx, 8
    mov     edx, r15d
    call    Mark

    /* ---- Ethernet: only when root port 00:01.1 still forwards bus 0x40 and its window covers BAR0 ---- */
    mov     r15d, 0xFF808080                /* grey: skipped */
    mov     r10d, 0x80000900                /* bus 0, device 1, function 1 */
    CFGRD   0x00
    cmp     eax, 0x13DF1022
    jne     69f
    CFGRD   0x04
    test    al, 2
    jz      69f
    CFGRD   0x18
    and     eax, 0x00FFFF00
    cmp     eax, 0x00404000                 /* secondary and subordinate bus 0x40 */
    jne     69f
    CFGRD   0x20                            /* memory window: base [15:4] << 20, limit [31:20] << 20 | 0xFFFFF */
    mov     ecx, eax
    and     ecx, 0xFFF0
    shl     ecx, 16
    cmp     ecx, ETH_BASE
    ja      69f
    and     eax, 0xFFF00000
    or      eax, 0xFFFFF
    cmp     eax, ETH_BASE + 0xFFF
    jb      69f
    mov     edi, ETH_BASE
    mov     dword ptr [rdi + 0x54], 0x7FFFFF    /* every interrupt source masked */
    mov     dword ptr [rdi + 0x204], 0          /* coalescing off */
    mov     r15d, 0xFFFF8000                /* orange unless a stop request hangs */
    mov     r8d, 0x34                       /* transmit control, then receive control */
61: or      dword ptr [rdi + r8], 2         /* stop request; the chip clears the bit when stopped */
    mov     r14d, 200
62: test    dword ptr [rdi + r8], 2
    jz      63f
    DELAY   1000
    dec     r14d
    jnz     62b
    mov     r15d, 0xFFFF0000                /* red */
63: add     r8d, 4
    cmp     r8d, 0x38
    jbe     61b
69: mov     ecx, 9
    mov     edx, r15d
    call    Mark

    /* ---- record: arming and the loop count go on to the next run; then into stage 1 ---- */
    mov     rdi, [rbp + BLK_RECORD]
    mov     eax, [rbp + BLK_ARMED]
    mov     [rdi + REC_ARMED], eax
    mov     eax, [rbp + BLK_LOOPS]
    inc     eax
    mov     [rdi + REC_LOOPS], eax
    MARK    10, 0xFF0000FF
    wbinvd
    mov     rsi, [rbp + BLK_RSI]            /* boot_params */
    mov     rax, [rbp + BLK_ENTRY]          /* stage 1 entry */
    mov     r15, 0x324D524157355350         /* "PS5WARM2": stage 1 knows it is a warm run */
    mov     r14, r15
    not     r14
    jmp     rax

/* Exceptions 0-31 (diagnostic): 8-byte stubs push the vector; red square 14, the vector as five squares (white = 1)
 * under squares 0-4 (bit 4 first), REC_STEP = 0x100 + vector, then halt with interrupts off. */
    .balign 8
    .globl Ps5WarmVecs
Ps5WarmVecs:
    .irp v, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .balign 8
    push    \v
    jmp     VecCommon
    .endr
VecCommon:
    mov     ebx, [rsp]
    MARK    14, 0xFFFF0000
    mov     rax, [rbp + BLK_RECORD]
    test    rax, rax
    jz      1f
    lea     ecx, [rbx + 0x100]
    mov     [rax + REC_STEP], ecx
1:  mov     rdi, [rbp + BLK_FB]
    test    rdi, rdi
    jz      4f
    mov     r8d, [rbp + BLK_PITCH]
    mov     rax, r8
    imul    rax, rax, 100
    add     rdi, rax                        /* row y = 100 */
    mov     r10d, 4                         /* bit number */
2:  bt      ebx, r10d
    mov     edx, 0xFF404040
    jnc     21f
    mov     edx, 0xFFFFFFFF
21: mov     eax, 4
    sub     eax, r10d                       /* position 0..4 */
    imul    eax, eax, 56
    add     eax, 40
    lea     rsi, [rdi + rax * 4]
    mov     r9d, 40
3:  push    rsi
    mov     ecx, 40
    mov     eax, edx
    xchg    rdi, rsi
    rep stosd
    xchg    rdi, rsi
    pop     rsi
    add     rsi, r8
    dec     r9d
    jnz     3b
    dec     r10d
    jns     2b
4:  cli
    hlt
    jmp     4b

/* Mark: ecx = slot, edx = colour. REC_STEP = slot, then a 40x40 square. Keeps every register but the flags. */
Mark:
    push    rax
    push    rcx
    push    rdx
    push    rdi
    push    r8
    push    r9
    mov     rax, [rbp + BLK_RECORD]
    test    rax, rax
    jz      3f
    mov     [rax + REC_STEP], ecx
3:  mov     rdi, [rbp + BLK_FB]
    test    rdi, rdi
    jz      2f
    mov     r8d, [rbp + BLK_PITCH]
    imul    eax, ecx, 56
    add     eax, 40
    shl     eax, 2
    add     rdi, rax
    mov     rax, r8
    imul    rax, rax, 40
    add     rdi, rax
    mov     r9d, 40
1:  push    rdi
    mov     ecx, 40
    mov     eax, edx
    rep stosd
    pop     rdi
    add     rdi, r8
    dec     r9d
    jnz     1b
2:  pop     r9
    pop     r8
    pop     rdi
    pop     rdx
    pop     rcx
    pop     rax
    ret

/* DelayUs: ecx = microseconds (TSC). Keeps every register but the flags. */
DelayUs:
    push    rax
    push    rcx
    push    rdx
    push    r8
    mov     eax, TSC_PER_US
    mul     ecx                             /* edx:eax = ticks */
    shl     rdx, 32
    or      rax, rdx
    mov     rcx, rax
    rdtsc
    shl     rdx, 32
    or      rax, rdx
    mov     r8, rax
1:  pause
    rdtsc
    shl     rdx, 32
    or      rax, rdx
    sub     rax, r8
    cmp     rax, rcx
    jb      1b
    pop     r8
    pop     rdx
    pop     rcx
    pop     rax
    ret

    .balign 8
PipeTable:                                  /* DCN 2.0.1 HUBP pipes 0-3, byte offsets in BAR5: addr lo, addr hi, viewport dim, cntl */
    .quad 0xEB28, 0xEB2C, 0xEAA8, 0xEACC
    .quad 0xEE98, 0xEE9C, 0xEE18, 0xEE3C
    .quad 0xF208, 0xF20C, 0xF188, 0xF1AC
    .quad 0xF578, 0xF57C, 0xF4F8, 0xF51C
Ps5WarmBlobEnd:
