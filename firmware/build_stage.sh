#!/bin/bash
# SPDX-License-Identifier: MIT
# Build the existing stage in the Windows scratch tree, with PCI access limited
# to the devices used by this boot. The original source stays read-only.
set -eu
cd "$(dirname "$0")/.."
src=../ps5-hackintosh
mkdir -p build/stage/bringup build/stage/tools
cp -R "$src/bringup/." build/stage/bringup/
cp "$src/tools/llvm.sh" build/stage/tools/
cp firmware/ps5warm/warm_pci.h build/stage/bringup/      # ps5win warm restart: PCI snapshot/restore (2026-11-24)
python3 - <<'PY'
from pathlib import Path
p = Path("build/stage/bringup/stub.c")
s = p.read_text()
needle = "                u32 id = pci_cfg(b, d, f, 0);"
assert s.count(needle) == 1
s = s.replace(needle, """                if (b == 0x40 || (b == 0x20 && d == 0 && f == 2))
                    continue;   /* never probe Salina or the PSP */
""" + needle)

# ps5win: any screen resolution (2026-11-22). kms_linear.py writes the picture's geometry after its 8 signature words
# ("PS5Z", width, height, pitch); stage 1 takes it from there (default 2560x1440, pitch 10240), uses it for its own text,
# patches it into the GOP HOB next to the framebuffer address (EFI_PEI_GRAPHICS_INFO_HOB: +8 size, +16 width,
# +20 height, +44 pixels per scan line), and keeps the UEFI debug console (fixed to 2560x1440) quiet on other screens.
def rep(old, new, count=1):
    global s
    assert s.count(old) == count, (old, s.count(old))
    s = s.replace(old, new)
rep("#define FB_PITCH  10240u\n#define FB_W      2560u\n#define FB_H      1440u\n",
    "static unsigned int g_fb_pitch = 10240u, g_fb_w = 2560u, g_fb_h = 1440u;   /* ps5win: from kms_linear.py's geometry words */\n"
    "#define FB_PITCH  g_fb_pitch\n#define FB_W      g_fb_w\n#define FB_H      g_fb_h\n")
rep("""        if (p[0] == 0xDEADBE00u && p[1] == 0xDEADBE01u && p[7] == 0xDEADBE07u) {
            fb = p;""",
    """        if (p[0] == 0xDEADBE00u && p[1] == 0xDEADBE01u && p[7] == 0xDEADBE07u) {
            fb = p;
            if (p[8] == 0x5A355350u && p[9] >= 640u && p[9] <= 3840u && p[10] >= 480u && p[10] <= 2160u &&
                p[11] >= p[9] * 4u && p[11] <= 16384u && (p[11] & 3u) == 0) {
                g_fb_w = p[9]; g_fb_h = p[10]; g_fb_pitch = p[11];
            }""")
rep("    *(volatile u32 *)0x300001f18ULL = 0;               /* console not quiet: draw the debug ring */\n",
    "    /* debug ring drawn only on the 2560x1440 picture it was written for; quiet (log only) on any other screen */\n"
    "    *(volatile u32 *)0x300001f18ULL = (g_fb_w == 2560u && g_fb_h == 1440u && g_fb_pitch == 10240u) ? 0 : 1;\n")
rep("        u32 a = 700u * (FB_PITCH / 4) + 1200u, b = 1000u * (FB_PITCH / 4) + 1234u;\n",
    "        u32 a = (FB_H * 7u / 16u) * (FB_PITCH / 4) + FB_W / 2u, b = (FB_H * 11u / 16u) * (FB_PITCH / 4) + FB_W / 2u + 34u;\n")
rep("    if (fbpatch) *(volatile u64 *)fbpatch = gopfb;\n",
    "    if (fbpatch) {\n"
    "        *(volatile u64 *)fbpatch = gopfb;\n"
    "        *(volatile u32 *)(fbpatch + 8) = g_fb_pitch * g_fb_h;     /* FrameBufferSize */\n"
    "        *(volatile u32 *)(fbpatch + 16) = g_fb_w;                 /* HorizontalResolution */\n"
    "        *(volatile u32 *)(fbpatch + 20) = g_fb_h;                 /* VerticalResolution */\n"
    "        *(volatile u32 *)(fbpatch + 44) = g_fb_pitch / 4;         /* PixelsPerScanLine */\n"
    "    }\n"
    "    puts_(\"screen \"); put_hex(g_fb_w, 4); puts_(\"x\"); put_hex(g_fb_h, 4); puts_(\" pitch \"); put_hex(g_fb_pitch, 5); puts_(\"\\n\");\n")

# ps5win warm restart (2026-11-23; design in WARM-RESTART-STUDY.md, firmware side in firmware/ps5warm). Windows'
# Restart can re-enter this stage without a power cycle: the firmware's reset stub jumps to startup_64 with rsi =
# boot_params and r15 = "PS5WARM2", r14 = ~r15 (entry.S below passes that on as `warm`). Then:
#  - stage1_main must not shrink init_size a second time, and must set its static variables itself;
#  - the picture's signature is gone (Windows drew over it): the frame buffer comes from the hand-over record;
#  - the old return-to-Linux tables at 0x63000-0x67FFF are no longer written (that memory is Windows RAM);
#  - every run writes the hand-over record at 0x3000F0000 (layout: firmware/ps5warm/Ps5Warm.h, REC_*), which tells
#    the firmware where to come back to. A run from Linux starts it unarmed, loop 0, cap 20; launch option
#    ps5warm=3a on the kexec command line sets the firmware-only loop test flag.
#  - (2026-11-24, after console experiment 3b found no USB device on the second run) firmware/ps5warm/warm_pci.h: a
#    run from Linux saves the configuration space of the two root ports and the xHCIs of bus 0x20 in the record page;
#    a warm run puts them back (D0, BARs, decoding, the Salina root port's routing), resets each xHCI and prints what
#    it found. The firmware does no PCI enumeration, so it needs the state the first run had.
rep("void stage1_main(u64 boot_params)\n{\n    volatile u32 *init_size = (volatile u32 *)(unsigned long)(boot_params + 0x260);\n    *init_size -= STUB_SIZE;\n",
    "void stage1_main(u64 boot_params, u64 warm)\n{\n    volatile u32 *init_size = (volatile u32 *)(unsigned long)(boot_params + 0x260);\n"
    "    if (!warm) *init_size -= STUB_SIZE;              /* ps5win: only once (a warm re-entry runs this again) */\n"
    "#if VARIANT == 5\n    stage_reset_statics();\n#endif\n")
rep("static void stage5_launch(u64 bp)\n{\n", """#define PS5_WARM_RECORD 0x3000F0000ULL                 /* ps5win warm restart: firmware/ps5warm/Ps5Warm.h, REC_* */
#define PS5_WARM_RECORD_MAGIC 0x3143455257355350ULL     /* "PS5WREC1" */
#define WREC32(o) (*(volatile u32 *)(PS5_WARM_RECORD + (o)))
#define WREC64(o) (*(volatile u64 *)(PS5_WARM_RECORD + (o)))

/* ps5win warm restart: PCI state of the first run, saved and put back (see warm_pci.h). */
#define WP_CFG_RD(b, d, f, off)     pci_cfg(b, d, f, off)
#define WP_CFG_WR(b, d, f, off, v)  pci_wr32(b, d, f, off, v)
#define WP_RD8(a)                   (*(volatile u8 *)(a))
#define WP_RD32(a)                  (*(volatile u32 *)(a))
#define WP_WR32(a, v)               (*(volatile u32 *)(a) = (v))
#define WP_DELAY_US(us)             wait_tsc((u64)(us) * 1597u)      /* the console's TSC: 1596.3 MHz */
#define WP_PUTS(s)                  puts_(s)
#define WP_HEX(v, d)                put_hex(v, d)
#define WP_REC                      PS5_WARM_RECORD
#include "warm_pci.h"

/* ps5win: a warm re-entry finds the static variables as the first run left them; set every one explicitly. */
static void stage_reset_statics(void)
{
    carve_base = 0x450000000ULL;
    fb = (volatile u32 *)FB_BASE;
    g_fb_pitch = 10240u; g_fb_w = 2560u; g_fb_h = 1440u;
    cx = cy = logn = 0;
    fg = 0xFFFFFFFFu; bg = 0xFF101040u;
}

/* ps5win: the frame buffer of the first run, from the hand-over record (Windows has drawn over the signature). */
static u64 warm_fb(void)
{
    u64 a = WREC64(0x18);
    u32 w = WREC32(0x20), h = WREC32(0x24), p = WREC32(0x28);
    if (WREC64(0x00) != PS5_WARM_RECORD_MAGIC || a < carve_base || (a & 0xFFF) || w < 640 || w > 3840 || h < 480 ||
        h > 2160 || p < w * 4 || p > 16384 || (p & 3) || a + (u64)p * h > CARVE_TOP)
        return 0;
    fb = (volatile u32 *)a;
    g_fb_w = w; g_fb_h = h; g_fb_pitch = p;
    return a;
}

/* ps5win: launch option ps5warm=3a on the kexec command line (boot_params cmd_line_ptr / ext_cmd_line_ptr). */
static u32 warm_test_flags(u64 bp)
{
    u64 cl = (u64)*(volatile u32 *)(bp + 0x228) | ((u64)*(volatile u32 *)(bp + 0xC8) << 32);
    static const char want[] = "ps5warm=3a";
    if (!cl) return 0;
    for (u32 i = 0; i < 4096 && *(volatile char *)(cl + i); i++) {
        u32 k = 0;
        while (want[k] && *(volatile char *)(cl + i + k) == want[k]) k++;
        if (!want[k]) return 1;
    }
    return 0;
}

static void stage5_launch(u64 bp, u64 warm)
{
""")
rep("""    puts_("bundle (initrd) at "); put_hex(img, 12); puts_(" size "); put_hex(isz, 8); puts_("\\n");\n""",
    """    puts_("bundle (initrd) at "); put_hex(img, 12); puts_(" size "); put_hex(isz, 8); puts_("\\n");
    if (warm) {
        puts_("WARM RESTART: re-entered from Windows' Restart without a power cycle. loop "); put_hex(WREC32(0x44), 4);
        puts_(", armed "); put_hex(WREC32(0x40), 1); puts_(", last stub step "); put_hex(WREC32(0x58), 2);
        puts_(", stage run "); put_hex(WREC32(0x4C) + 1, 4); puts_("\\n");
    }
""")
rep("    *(volatile u32 *)0x63000ULL = (u32)(realfb - carve_base);   /* VRAM offset of the picture, for bringup/tripwire */\n", "")
rep("    if (bar0 && bar0 < 0x100000000ULL && vofs + (u64)FB_PITCH * FB_H <= 0x10000000ULL) {",
    "    if (warm && WREC64(0x30)) {\n        gopfb = WREC64(0x30);                         /* ps5win: the first run's choice (BAR0 alias or carve-out) */\n        puts_(\"  (warm: as the first run)\");\n    } else if (bar0 && bar0 < 0x100000000ULL && vofs + (u64)FB_PITCH * FB_H <= 0x10000000ULL) {")
a = s.index('    /* Everything a later "return to Linux" needs (see bringup/retlinux)')
b = s.index('    puts_("\\njumping to the UEFI payload...\\n");')
s = s[:a] + """    /* ps5win warm restart: the hand-over record (replaces the old return-to-Linux tables at 0x63000-0x67FFF, which lie
     * in Windows RAM). Arming, loop counter, cap and test flags carry over a warm run; a run from Linux starts afresh. */
    {
        int keep = warm && WREC64(0x00) == PS5_WARM_RECORD_MAGIC;
        u32 armed = keep ? WREC32(0x40) : 0, loops = keep ? WREC32(0x44) : 0, cap = keep ? WREC32(0x48) : 20;
        u32 runs = keep ? WREC32(0x4C) : 0, flags = keep ? WREC32(0x2C) : warm_test_flags(bp), step = keep ? WREC32(0x58) : 0;
        WREC64(0x00) = 0;                                  /* magic last */
        WREC64(0x08) = (u64)(unsigned long)startup_64;
        WREC64(0x10) = bp;
        WREC64(0x18) = realfb;
        WREC32(0x20) = g_fb_w; WREC32(0x24) = g_fb_h; WREC32(0x28) = g_fb_pitch; WREC32(0x2C) = flags;
        WREC64(0x30) = gopfb;
        WREC64(0x38) = carve_base;
        WREC32(0x40) = armed; WREC32(0x44) = loops; WREC32(0x48) = cap; WREC32(0x4C) = runs + 1;
        WREC64(0x50) = 0;
        WREC32(0x58) = step;
        __asm__ volatile("" ::: "memory");
        WREC64(0x00) = PS5_WARM_RECORD_MAGIC;
        puts_("warm-restart record: entry "); put_hex(WREC64(0x08), 12); puts_(" boot_params "); put_hex(bp, 12);
        puts_(" armed "); put_hex(armed, 1); puts_(" loops "); put_hex(loops, 4); puts_(" flags "); put_hex(flags, 2); puts_("\\n");
    }
    /* ps5win warm restart: the firmware takes every BAR, window and power state as it finds it. A run from Linux saves
     * that state (reads only); a warm run puts back what Windows' shutdown changed and resets the xHCIs. After
     * quiesce_pci on both paths, so the saved state has MSI/MSI-X off. */
    if (warm) wp_restore(WREC32(0x4C)); else wp_snapshot();
""" + s[b:]
rep("""        carve_from_bundle(boot_params);
        u64 fbaddr = find_fb();
        m[n++] = fbaddr;
        if (fbaddr)
            stage5_launch(boot_params);
""", """        carve_from_bundle(boot_params);
        u64 fbaddr = warm ? warm_fb() : find_fb();
        m[n++] = fbaddr;
        if (fbaddr)
            stage5_launch(boot_params, warm);
        if (warm)                                    /* ps5win: never fall back into Linux from a warm re-entry */
            for (;;) __asm__ volatile("cli; hlt");
""")
p.write_text(s)

# entry.S: the macOS return flag at 0x63030 is gone (Windows RAM); the warm re-entry is told by r15/r14 instead.
e = Path("build/stage/bringup/entry.S")
t = e.read_text()
a = t.index("    cmpl    $0x52455455, 0x63030")
b = t.index(".Lnormal:\n")
t = t[:a] + """    /* ps5win warm restart: the firmware's reset stub enters with r15 = "PS5WARM2" and r14 = its complement */
    xorl    %r13d, %r13d
    movabsq $0x324D524157355350, %rax
    cmpq    %rax, %r15
    jne     .Lnormal
    notq    %rax
    cmpq    %rax, %r14
    jne     .Lnormal
    movl    $1, %r13d                     /* warm: kept across the bss clear (rep stosb uses rdi, rcx, al) */
""" + t[b:]
old = "    movq    %r12, %rdi\n    call    stage1_main\n"
assert t.count(old) == 1
t = t.replace(old, "    movq    %r12, %rdi\n    movq    %r13, %rsi                  /* ps5win: warm re-entry flag */\n    call    stage1_main\n")
e.write_text(t)
PY
bash build/stage/bringup/build.sh "$src/build/ext/vmlinuz-7.2.3-p5f6b" >/dev/null
cp build/stage/build/bringup/chainV5.bzImage build/windows.bzImage
