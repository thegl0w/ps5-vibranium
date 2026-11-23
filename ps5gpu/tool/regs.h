// SPDX-License-Identifier: MIT
// Register names for Cyan Skillfish as absolute dword offsets into the register BAR, from Linux's headers.
#pragma once
#include <cstddef>
#include "asic_reg/gc/gc_10_1_0_offset.h"
#include "asic_reg/gc/gc_10_1_0_sh_mask.h"
#include "asic_reg/oss/osssys_5_0_0_offset.h"
#include "asic_reg/oss/osssys_5_0_0_sh_mask.h"
#include "asic_reg/nbio/nbio_2_3_offset.h"
#include "asic_reg/dcn/dcn_2_0_1_offset.h"
#include "nvd.h"

// Register block bases for Cyan Skillfish (cyan_skillfish_ip_offset.h), by the header's _BASE_IDX.
#define GC_SEG(i) ((i) == 0 ? 0x1260u : 0xA000u)
#define OSS_SEG(i) (0x10A0u)
#define NBIO_SEG(i) ((i) == 0 ? 0x0u : (i) == 1 ? 0x14u : (i) == 2 ? 0xD20u : 0x10400u)
#define DMU_SEG(i) ((i) == 0 ? 0x12u : (i) == 1 ? 0xC0u : (i) == 2 ? 0x34C0u : 0x9000u)
#define GC(r) (GC_SEG(r##_BASE_IDX) + r)
#define OSS(r) (OSS_SEG(r##_BASE_IDX) + r)
#define NBIO(r) (NBIO_SEG(r##_BASE_IDX) + r)
#define DMU(r) (DMU_SEG(r##_BASE_IDX) + r)
