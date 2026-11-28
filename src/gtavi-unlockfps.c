/* gtavi-unlockfps.c -- one-shot runtime FPS unlock & TAA2U render scale payload
 * for Grand Theft Auto VI (PPSA38500) on a jailbroken PS5.
 *
 * What this payload does (and nothing else):
 *   - loads settings from /data/gtavi-unlockfps/fps.txt (defaults: fps=60,
 *     res_pct=50 [1080p internal on 4K via TAA2U]; stock PS5 is 67% / 1440p),
 *   - waits up to 10 minutes for PPSA38500 (eboot.bin) to start without
 *     touching memory of any other app or system process,
 *   - resolves eboot.bin's ASLR base address and verifies 64 bytes of context
 *     around each target site before writing anything,
 *   - unlocks the 30 FPS cap (0x3f73e51, 0x40d2be1) and updates live
 *     g_TargetFrameTimeUs / g_RequestedFrameLock globals in RAM,
 *   - fixes game speed-up above 30 FPS by lowering fwTimer's minimum delta-time
 *     floor from 1/30s to 1ms (0x3f73ecd, 0xabab49c, 0xabab4a0),
 *   - when res_pct > 0 (25..100), patches TAA2U::Update() (0x62eadc0, 18 bytes)
 *     to set the internal render scale float before TAA2U upscaling (all other
 *     graphics quality settings remain 100% stock),
 *   - uses DMAP page-table writes on FW >= 8.40 (or mdbg_copyin on older FW),
 *     verifies readback, shows PS5 notifications, writes a log, and exits.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef HOST_TEST
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include "ptwalk.h"
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/mdbg.h>
#endif

#define TOOL_VERSION "1.17"

/* Process name shown by payload managers (PLDMGR lists names ending in .elf;
 * kernel keeps at most 19 chars). */
#define PROC_NAME "gtavi_fps.elf"

/* ---------------------------------------------------------------- config */

static const char *const WANT_TITLE_IDS[] = {
    "PPSA01547", "PPSA29660", "PPSA38500"
};

#define DEFAULT_FPS         60u
#define MIN_FPS             30u
#define MAX_FPS             120u

/* Internal TAA2U (PS5) / PSSR (PS5 Pro) render resolution % of output.
 * 0 = stock (untouched), 25..100 = custom percent (default 50 = 1080p on 4K; <50 experimental). */
#define DEFAULT_RES_PCT     50u
#define MIN_RES_PCT         25u
#define MAX_RES_PCT         100u

/* Refresh rate assumed by sceVideoOutSetFlipRate divider (60 = stock, 120 = forced 120 Hz). */
#define DEFAULT_REFRESH_HZ  60u

/* Optional swapchain output buffer resolution:
 * 0 = stock 4K (3840x2160), 1440 = 2560x1440, 1080 = 1920x1080 (experimental). */
#define DEFAULT_OUTPUT_RES  0u

/* Experimental PS5 Pro Half-Res RT Reflections (lowers Tier 3 -> base PS5 Tier 0):
 * 0 = stock (default), 1 = half-res RT Reflections on PS5 Pro. */
#define DEFAULT_PRO_RT_HALF 0u

/* Background daemon mode:
 * 0 = one-shot (wait up to 10 min, patch once, exit), 1 = stay in background across restarts. */
#define DEFAULT_DAEMON      0u

/* PS5 Pro above-stock resolution (pro_highres=1). The 67%+ crash (int 0x41, RVA 0x8530da7) is
 * the transient render heap (0x3550 x 64 KB pages, set at boot) being too small. It is raised
 * before init, its page array is moved above 1 GB, heap #2 gets +512 MB, and the main pool
 * shrinks by the same total. Measured peak: 883 MB at 2572x1447, 1066 MB at 2752x1548. */
#define DEFAULT_PRO_HIGHRES 0u
#define HEAP_PAGES_RVA      0x3e0fb71u /* imm32 of 'mov eax, 0x3550' (Pro page count)   */
#define HEAP_SIZE_G_RVA     0xe3d1f20u /* qword: heap pages, 0 until the heap is created */
#define HEAP_PAGES_STOCK    0x3550u
#define HEAP_PAGES_ARRAY    0x4000u    /* stock page pointer array 0xe3d1f50 capacity    */
#define NEW_ARRAY_RVA       0xdb10000u /* unused zero region (no code refs) for the array */
#define NEW_ARRAY_END_RVA   0xdc87000u
#define HEAP_PAGES_MAX      ((NEW_ARRAY_END_RVA - NEW_ARRAY_RVA) / 8u)
#define LEA1_RVA            0x3e0fb7eu /* lea r15, [page array] (heap creation)          */
#define LEA2_RVA            0x85310e3u /* lea rcx, [page array] (per-frame page mapping) */
#define HEAP2_RVA           0x9d65778u /* Pro row of per-platform heap table, heap #2     */
#define HEAP2_STOCK         0xc4700000ull
#define HEAP2_ADD           0x20000000ull /* +512 MB, needed above ~70% */
#define POOL_STOCK          0x185c00000ull
#define HEAP_MARGIN_MB      24.0
static const uint32_t POOL_RVAS[10] = {
    0xc7a3e1u, 0xc7a627u, 0x148f3b7u, 0x64a230eu, 0x67a2cf2u,
    0x8480de4u, 0x84811d7u, 0x85443a4u, 0x8b032ecu, 0x8b03968u
};

/* Base PS5 experimental heap expansion (base_highres=1):
 * Dynamically scales Base PS5 transient render heap (stock 0x2d80 = 728 MB) and Base PS5
 * heap #2 (0x9d65728, stock 2718 MB), shrinking the shared main pool by the same total:
 *   res_pct <= 75: 0x3800 (896 MB, +168 MB) + heap #2 +256 MB (confirmed on 1440p + 75%)
 *   res_pct >  75: 0x4000 (1024 MB, +296 MB, fits stock 0x4000 array) + heap #2 +512 MB */
#define DEFAULT_BASE_HIGHRES  0u
#define BASE_HEAP_PAGES_RVA   0x3e0fb76u /* imm32 of 'mov ebx, 0x2d80' (Base PS5 pages)  */
#define BASE_HEAP_PAGES_STOCK 0x2d80u
#define BASE_HEAP_PAGES_MED   0x3800u    /* 896 MB  (res_pct <= 75)                      */
#define BASE_HEAP_PAGES_HI    0x4000u    /* 1024 MB (res_pct > 75, <= HEAP_PAGES_ARRAY)  */
#define BASE_HEAP2_RVA        0x9d65728u /* Base PS5 row of per-platform heap table, #2  */
#define BASE_HEAP2_STOCK      0xa9e00000ull /* 2718 MB */
#define BASE_HEAP2_ADD_MED    0x10000000ull /* +256 MB */
#define BASE_HEAP2_ADD_HI     0x20000000ull /* +512 MB */

/* Close-game CE-108255-1 fix (close_fix=1, default ON):
 * Full bypass of the synchronous SAVEDATAA00 / SPACECHECK00.dat probe at 0x65742c0. */
#define DEFAULT_CLOSE_FIX      1u

/* Motion blur toggle (motion_blur=1 stock default, 0 = off): */
#define DEFAULT_MOTION_BLUR    1u

/* Experimental GPU / CPU / upscaler performance tweaks (all default 0 / OFF):
 *   pro_taa2u=1       : PS5 Pro uses TAA2U instead of PSSR (clears PSSR bit 0x10)
 *   low_shadows=1|2   : 1 = Shadow Map Tier 1 (1280), 2 = + Soft Shadows filter 12 -> 2
 *   low_reflections=1 : ReflectionQuality map 256 (8 mips) -> 128 (7 mips)
 *   low_volumetric=1  : VolumetricsRaymarch + VolumetricsLighting -> Tier 0 (low)
 *   lod_scale=50..100 : LodScale draw distance % (0 = stock, 50..100 = 0.50x..1.00x)
 *   low_bvh=1         : BvhQuality -> Tier 0 (lighter RT acceleration structure) */
#define DEFAULT_PRO_TAA2U       0u
#define DEFAULT_LOW_SHADOWS     0u
#define DEFAULT_LOW_REFLECTIONS 0u
#define DEFAULT_LOW_VOLUMETRIC  0u
#define DEFAULT_LOD_SCALE       0u
#define MIN_LOD_SCALE           50u
#define MAX_LOD_SCALE           100u
#define DEFAULT_LOW_BVH         0u

/* RVAs relative to eboot.bin map base (vaddr 0x0 in ELF PHDR 0). */
#define SITE1_RVA       0x3f73e51u /* mov eax, dword ptr [rbx + 0xe1c]  (6 bytes)  */
#define SITE1_LEN       6u
#define SITE2_RVA       0x40d2be1u /* mov eax, dword ptr [r14 + 0x204]  (7 bytes)  */
#define SITE2_LEN       7u
#define TIMESTEP_RVA    0x3f73ecdu /* mov eax, dword ptr [rax + 0x614]  (6 bytes)  */
#define TIMESTEP_LEN    6u
#define TIMESTEP2_RVA   0x41ec831u /* vmovss [0xabab4a0], xmm0          (8 bytes)  */
#define TIMESTEP2_LEN   8u

/* Hardcoded 60 Hz (0x3c) / 61 (0x3d) constants in sceVideoOutSetFlipRate divider: */
#define HZ1_A_RVA       0x44a326eu /* imm8 0x3c inside mov eax, 0x3c    (1 byte)   */
#define HZ1_B_RVA       0x44a3288u /* imm8 0x3d inside cmp esi, 0x3d    (1 byte)   */
#define HZ2_A_RVA       0x851c81eu
#define HZ2_B_RVA       0x851c838u
#define HZ3_A_RVA       0x852228cu
#define HZ3_B_RVA       0x85222a7u

/* Swapchain output width/height loads in CSettingsManager::ApplySettings: */
#define OUT_W_RVA       0x3f73db4u /* mov eax, dword ptr [rbx + 0xe10]  (6 bytes)  */
#define OUT_H1_RVA      0x3f73dc9u /* mov eax, dword ptr [rbx + 0xe14]  (6 bytes)  */
#define OUT_H2_RVA      0x3f73dd7u /* mov ecx, dword ptr [rbx + 0xe14]  (6 bytes)  */
#define OUT_LEN         6u

/* PS5 Pro GlobalIlluminationQuality (0xc6c) tier loads (all 4 readers -> Tier 0): */
#define PRO_RT_HALF_RVA     0x6326444u /* mov ecx, dword ptr [rdi + 0x54]   (3 bytes)  */
#define PRO_RT_HALF_LEN     3u
#define PRO_RT_HALF2_RVA    0x62fee1fu /* mov r15d, dword ptr [r14 + 0x54]  (4 bytes)  */
#define PRO_RT_HALF2_LEN    4u
#define PRO_RT_HALF3_RVA    0x630e948u /* mov edi, dword ptr [rax + 0x464]  (6 bytes)  */
#define PRO_RT_HALF3_LEN    6u
#define PRO_RT_HALF4_RVA    0x2998c33u /* mov edi, dword ptr [rax + 0x464]  (6 bytes)  */
#define PRO_RT_HALF4_LEN    6u

/* Close-game CE-108255-1 fix: full bypass of SAVEDATAA00/SPACECHECK00.dat probe: */
#define CLOSE_FIX_RVA       0x65742c0u /* lea rbx, [rbp - 0x1ac]            (7 bytes)  */
#define CLOSE_FIX_LEN       7u

/* PS5 Pro PSSR -> TAA2U switch (clears PSSR capability bit 0x10 in 0xb3d212c): */
#define PRO_TAA2U_RVA       0x2afc2c9u /* call qword ptr [rax + 0x10]       (3 bytes)  */
#define PRO_TAA2U_LEN       3u

/* DirectionalShadowQuality (0xc64 -> 1280) & ShadowSoftShadows (0xd70 -> filter 2): */
#define LOW_SHADOWS_RVA     0x3f74417u /* mov eax, dword ptr [rbx + 0xc64]  (6 bytes)  */
#define LOW_SHADOWS_LEN     6u
#define LOW_SHADOWS2_RVA    0x3f74350u /* mov eax, dword ptr [rax + 0x568]  (6 bytes)  */
#define LOW_SHADOWS2_LEN    6u

/* ReflectionQuality (0xc88) map size check in ApplySettings (256 -> 128): */
#define LOW_REFLECTIONS_RVA 0x3f73f11u /* cmp dword ptr [rbx + 0xc88], 0    (7 bytes)  */
#define LOW_REFLECTIONS_LEN 7u

/* VolumetricsRaymarchQuality (0xda8) & VolumetricsLightingQuality (0xda4) -> Tier 0: */
#define LOW_VOL1_RVA        0x3f741b9u /* mov eax, dword ptr [rbx + 0xda8]  (6 bytes)  */
#define LOW_VOL2_RVA        0x3f741c5u /* mov eax, dword ptr [rbx + 0xda4]  (6 bytes)  */
#define LOW_VOL_LEN         6u

/* LodScale (0xce8 -> 0xd796908) 16-byte load+store in ApplySettings: */
#define LOD_SCALE_RVA       0x3f73f47u /* vmovss xmm0, [rbx+0xce8] + store  (16 bytes) */
#define LOD_SCALE_LEN       16u

/* BvhQuality (0xcbc -> 0xa919914) load in ApplySettings: */
#define LOW_BVH_RVA         0x3f7444fu /* mov eax, dword ptr [rbx + 0xcbc]  (6 bytes)  */
#define LOW_BVH_LEN         6u

/* MotionBlur (0xd30 -> 0xc62c3ec) loads: */
#define MOTION_BLUR1_RVA    0x4067792u /* mov al, byte ptr [rax + 0x528]    (6 bytes)  */
#define MOTION_BLUR2_RVA    0x64bbf36u /* mov al, byte ptr [rax + 0x528]    (6 bytes)  */
#define MOTION_BLUR_LEN     6u

/* TAA2U::Update() (vtable+0x50 of base PS5 upscaler 0xb3d2db0):
 *   vbroadcastss xmm0, [0x9c7b9ec]  (0.6666667f)
 *   vmovlps      qword ptr [rdi+0x90], xmm0
 *   ret                                          (18 bytes total) */
#define TAA2U_RVA 0x62eadc0u
#define TAA2U_LEN 18u

/* PSSR::GetRenderDimensions() (vtable+0x58 of PS5 Pro upscaler 0xb3d2b70):
 *   0x62e6bc5: overrides preset scale lookup with custom float32 -> jmp 0x62e6c07 (11 bytes) */
#define PSSR_RVA  0x62e6bc5u
#define PSSR_LEN  11u

/* Live globals polled by SetFlipRate & fwTimer: */
#define G_TARGET_FRAMETIME_US_RVA 0xe3be220u /* uint32_t: 1000000 / fps */
#define G_REQUESTED_FRAMELOCK_RVA 0xe7bbb08u /* uint8_t:  0xff = auto   */
#define G_MIN_FRAME_SEC_RVA       0xabab49cu /* float: fwTimer min dt   */
#define G_TARGET_FRAME_SEC_RVA    0xabab4a0u /* float: fwTimer floor dt */
#define G_MOTION_BLUR_RVA         0xc62c3ecu /* uint8_t: MotionBlur     */

#define CTX_OFF 32u

/* 64 bytes of the clean GTAVI eboot around Site 1 (site at +32): */
static const uint8_t SITE1_CTX[64] = {
    0x00,0xc5,0xfa,0x10,0x0d,0x32,0x89,0xd0,0x05,0xba,0xe8,0x03,0x00,0x00,0xb9,0xe8,
    0x03,0x00,0x00,0x4c,0x8d,0x3d,0x65,0x78,0xdb,0xfd,0x89,0x05,0xe7,0x67,0xfa,0x07,
    0x8b,0x83,0x1c,0x0e,0x00,0x00,0xc6,0x05,0xaa,0x7c,0x84,0x0a,0xff,0x3d,0xe8,0x03,
    0x00,0x00,0x0f,0x42,0xd0,0x85,0xc0,0x0f,0x44,0xd1,0x48,0x8b,0x0d,0x9e,0xb6,0x7f
};

/* 64 bytes of the clean GTAVI eboot around Site 2 (site at +32): */
static const uint8_t SITE2_CTX[64] = {
    0x00,0x0f,0x82,0x96,0x00,0x00,0x00,0xc6,0x05,0x39,0x8f,0x6e,0x0a,0xff,0xc5,0xfa,
    0x10,0x0d,0x95,0x9b,0xba,0x05,0xba,0xe8,0x03,0x00,0x00,0xb9,0xe8,0x03,0x00,0x00,
    0x41,0x8b,0x86,0x04,0x02,0x00,0x00,0x3d,0xe8,0x03,0x00,0x00,0x0f,0x42,0xd0,0x85,
    0xc0,0x0f,0x44,0xd1,0xc5,0xea,0x2a,0xc2,0xc5,0xf2,0x5e,0xc0,0xc4,0xe1,0xfa,0x2c
};

/* 64 bytes of the clean GTAVI eboot around TIMESTEP writer (site at +32, len 6): */
static const uint8_t TIMESTEP_CTX[64] = {
    0x8b,0x05,0x5d,0xb6,0x7f,0x0a,0x48,0x8b,0x40,0x08,0x48,0x8b,0x08,0x48,0x8b,0x49,
    0x50,0x4c,0x39,0xf9,0x0f,0x85,0x70,0x08,0x00,0x00,0x48,0x05,0x08,0x08,0x00,0x00,
    0x8b,0x80,0x14,0x06,0x00,0x00,0xc4,0xe1,0xda,0x2a,0xc0,0xc5,0xfa,0x10,0x0d,0xfc,
    0x63,0xd0,0x05,0x31,0xc9,0xba,0x80,0x00,0x00,0x00,0xc5,0xf2,0x5e,0xc0,0xc5,0xfa
};

/* 64 bytes of the clean GTAVI eboot around second TIMESTEP writer (site at +32, len 8): */
static const uint8_t TIMESTEP2_CTX[64] = {
    0x05,0xde,0xbe,0x56,0x0a,0x10,0x00,0x00,0x00,0x48,0x85,0xdb,0x48,0xb9,0xcd,0xcc,
    0x4c,0x3d,0x32,0x00,0x00,0x00,0xc7,0x05,0x6b,0xec,0x9b,0x06,0x3e,0xc3,0xae,0x39,
    0xc5,0xfa,0x11,0x05,0x67,0xec,0x9b,0x06,0xba,0x08,0x00,0x00,0x00,0x48,0x8b,0x3d,
    0xa3,0xbe,0x56,0x0a,0x0f,0x94,0x05,0x64,0xbe,0x56,0x0a,0x31,0xc0,0x48,0x8b,0x35
};

/* 64 bytes of the clean GTAVI eboot around TAA2U::Update (site at +32, len 18): */
static const uint8_t TAA2U_CTX[64] = {
    0xf8,0x28,0x4d,0x90,0x4c,0x89,0xf7,0x8b,0x80,0x10,0xff,0xff,0xff,0xe9,0x57,0xff,
    0xff,0xff,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0xc4,0xe2,0x79,0x18,0x05,0x23,0x0c,0x99,0x03,0xc5,0xf8,0x13,0x87,0x90,0x00,0x00,
    0x00,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc
};

/* 64 bytes of the clean GTAVI eboot around PSSR::GetRenderDimensions scale select
 * (0x62e6bc5, site at +32, len 11): */
static const uint8_t PSSR_CTX[64] = {
    0x08,0x48,0x8b,0x38,0x4c,0x8b,0x4f,0x50,0x48,0x8d,0x3d,0xfc,0x4a,0xa4,0xfb,0x49,
    0x39,0xf9,0x75,0x74,0x48,0x05,0x08,0x08,0x00,0x00,0x8b,0x80,0x60,0x04,0x00,0x00,
    0x41,0x81,0xf8,0x70,0x08,0x00,0x00,0x75,0x1a,0x81,0xf9,0x00,0x0f,0x00,0x00,0x75,
    0x12,0x83,0xf8,0x03,0x75,0x0d,0xc7,0x06,0xc0,0x08,0x00,0x00,0xb8,0xec,0x04,0x00
};

/* 64-byte contexts for 120 Hz flip-rate constants (sites at +32, len 1): */
static const uint8_t HZ1_A_CTX[64] = {
    0xc0,0xc4,0xe1,0xfa,0x2c,0xf0,0x48,0xb8,0x3c,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x48,0x89,0x05,0x27,0x88,0x31,0x0a,0x81,0xfb,0xff,0x00,0x00,0x00,0x75,0x1f,0xb8,
    0x3c,0x00,0x00,0x00,0x31,0xd2,0xbb,0x01,0x00,0x00,0x00,0xf7,0xf6,0xba,0x04,0x00,
    0x00,0x00,0x83,0xf8,0x04,0x0f,0x42,0xd0,0x83,0xfe,0x3d,0x0f,0x42,0xda,0x80,0x3d
};
static const uint8_t HZ1_B_CTX[64] = {
    0x00,0x00,0x00,0x75,0x1f,0xb8,0x3c,0x00,0x00,0x00,0x31,0xd2,0xbb,0x01,0x00,0x00,
    0x00,0xf7,0xf6,0xba,0x04,0x00,0x00,0x00,0x83,0xf8,0x04,0x0f,0x42,0xd0,0x83,0xfe,
    0x3d,0x0f,0x42,0xda,0x80,0x3d,0x95,0xaf,0xf1,0x09,0x02,0x88,0x1d,0x6f,0x88,0x31,
    0x0a,0x89,0x35,0x39,0xc0,0xf1,0x09,0x89,0x0d,0x07,0xbf,0xf1,0x09,0x0f,0x94,0x05
};
static const uint8_t HZ2_A_CTX[64] = {
    0x06,0xc4,0xe1,0xea,0x2a,0xc9,0xc5,0xfa,0x10,0x05,0x60,0xff,0x75,0x01,0xc5,0xfa,
    0x5e,0xc9,0xc4,0xe1,0xfa,0x2c,0xf1,0x81,0xfb,0xff,0x00,0x00,0x00,0x75,0x1f,0xb8,
    0x3c,0x00,0x00,0x00,0x31,0xd2,0xbb,0x01,0x00,0x00,0x00,0xf7,0xf6,0xba,0x04,0x00,
    0x00,0x00,0x83,0xf8,0x04,0x0f,0x42,0xd0,0x83,0xfe,0x3d,0x0f,0x42,0xda,0x80,0x3d
};
static const uint8_t HZ2_B_CTX[64] = {
    0x00,0x00,0x00,0x75,0x1f,0xb8,0x3c,0x00,0x00,0x00,0x31,0xd2,0xbb,0x01,0x00,0x00,
    0x00,0xf7,0xf6,0xba,0x04,0x00,0x00,0x00,0x83,0xf8,0x04,0x0f,0x42,0xd0,0x83,0xfe,
    0x3d,0x0f,0x42,0xda,0x80,0x3d,0xe5,0x19,0xea,0x05,0x02,0x88,0x1d,0xbf,0xf2,0x29,
    0x06,0x89,0x35,0x89,0x2a,0xea,0x05,0x89,0x0d,0x57,0x29,0xea,0x05,0x0f,0x94,0x05
};
static const uint8_t HZ3_A_CTX[64] = {
    0xc4,0xe1,0xfa,0x2c,0xf0,0x48,0xb8,0x3c,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x48,
    0x89,0x05,0x0a,0x98,0x29,0x06,0x41,0x81,0xfe,0xff,0x00,0x00,0x00,0x75,0x21,0xb8,
    0x3c,0x00,0x00,0x00,0x31,0xd2,0x41,0xbe,0x01,0x00,0x00,0x00,0xf7,0xf6,0xba,0x04,
    0x00,0x00,0x00,0x83,0xf8,0x04,0x0f,0x42,0xd0,0x83,0xfe,0x3d,0x44,0x0f,0x42,0xf2
};
static const uint8_t HZ3_B_CTX[64] = {
    0x00,0x00,0x75,0x21,0xb8,0x3c,0x00,0x00,0x00,0x31,0xd2,0x41,0xbe,0x01,0x00,0x00,
    0x00,0xf7,0xf6,0xba,0x04,0x00,0x00,0x00,0x83,0xf8,0x04,0x0f,0x42,0xd0,0x83,0xfe,
    0x3d,0x44,0x0f,0x42,0xf2,0x80,0x3d,0x75,0xbf,0xe9,0x05,0x02,0x44,0x88,0x35,0x4e,
    0x98,0x29,0x06,0x89,0x35,0x18,0xd0,0xe9,0x05,0x89,0x0d,0xe6,0xce,0xe9,0x05,0x0f
};

/* 64-byte contexts for swapchain output width/height loads (sites at +32, len 6): */
static const uint8_t OUT_W_CTX[64] = {
    0x31,0xc0,0x8b,0xbb,0x04,0x0e,0x00,0x00,0x48,0x8d,0x48,0x08,0x48,0x85,0xc0,0x48,
    0x0f,0x44,0xc8,0x3b,0xb9,0x4c,0x01,0x00,0x00,0x73,0x05,0xe8,0x8c,0xdc,0xac,0xff,
    0x8b,0x83,0x10,0x0e,0x00,0x00,0x4c,0x8d,0xa3,0x18,0x0c,0x00,0x00,0x39,0x05,0xbd,
    0x7c,0x84,0x0a,0x75,0x0e,0x8b,0x83,0x14,0x0e,0x00,0x00,0x89,0x05,0xb3,0x7c,0x84
};
static const uint8_t OUT_H1_CTX[64] = {
    0x4c,0x01,0x00,0x00,0x73,0x05,0xe8,0x8c,0xdc,0xac,0xff,0x8b,0x83,0x10,0x0e,0x00,
    0x00,0x4c,0x8d,0xa3,0x18,0x0c,0x00,0x00,0x39,0x05,0xbd,0x7c,0x84,0x0a,0x75,0x0e,
    0x8b,0x83,0x14,0x0e,0x00,0x00,0x89,0x05,0xb3,0x7c,0x84,0x0a,0xeb,0x1f,0x8b,0x8b,
    0x14,0x0e,0x00,0x00,0x39,0x0d,0xa5,0x7c,0x84,0x0a,0x89,0x05,0x9b,0x7c,0x84,0x0a
};
static const uint8_t OUT_H2_CTX[64] = {
    0x0e,0x00,0x00,0x4c,0x8d,0xa3,0x18,0x0c,0x00,0x00,0x39,0x05,0xbd,0x7c,0x84,0x0a,
    0x75,0x0e,0x8b,0x83,0x14,0x0e,0x00,0x00,0x89,0x05,0xb3,0x7c,0x84,0x0a,0xeb,0x1f,
    0x8b,0x8b,0x14,0x0e,0x00,0x00,0x39,0x0d,0xa5,0x7c,0x84,0x0a,0x89,0x05,0x9b,0x7c,
    0x84,0x0a,0x89,0x0d,0x99,0x7c,0x84,0x0a,0x74,0x05,0xe8,0x8a,0x1e,0x05,0x00,0xe8
};

/* 64-byte context for PS5 Pro RT Reflections quality tier load (site at +32, len 3): */
static const uint8_t PRO_RT_HALF_CTX[64] = {
    0x1f,0x49,0x8b,0x06,0x48,0x3b,0x83,0x60,0x02,0x00,0x00,0x0f,0x85,0xb0,0x31,0x00,
    0x00,0x48,0x8d,0x65,0xd8,0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d,0xc3,
    0x8b,0x4f,0x54,0xb8,0x03,0x00,0x00,0x00,0x44,0x8b,0x97,0x40,0x01,0x00,0x00,0x44,
    0x8b,0x87,0xf8,0x01,0x00,0x00,0x44,0x8b,0x8f,0xfc,0x01,0x00,0x00,0x4c,0x8d,0x35
};

/* 64-byte context for SPACECHECK00.dat probe entry (site at +32, len 7): */
static const uint8_t CLOSE_FIX_CTX[64] = {
    0x88,0x01,0x00,0x00,0x4c,0x8b,0x25,0xfd,0x5c,0xe7,0x03,0x49,0x8b,0x04,0x24,0x48,
    0x89,0x45,0xd0,0x80,0x3d,0xfa,0x7d,0xae,0x05,0x00,0x0f,0x84,0x89,0x02,0x00,0x00,
    0x48,0x8d,0x9d,0x54,0xfe,0xff,0xff,0x48,0x89,0xdf,0xe8,0x71,0x27,0x9f,0x02,0x8b,
    0x03,0x4c,0x8d,0xbd,0x5c,0xfe,0xff,0xff,0x49,0xbd,0x53,0x41,0x56,0x45,0x44,0x41
};

/* 64-byte context for PSSR capability flag call in 0x2afc290 (site at +32, len 3): */
static const uint8_t PRO_TAA2U_CTX[64] = {
    0x48,0x89,0x45,0xe8,0xc7,0x03,0x00,0x00,0x00,0x00,0xe8,0x98,0xff,0xff,0xff,0x48,
    0x8b,0x05,0xb1,0x68,0x8d,0x08,0x48,0x8d,0x3d,0xaa,0x68,0x8d,0x08,0x48,0x89,0xde,
    0xff,0x50,0x10,0xe8,0x2f,0x00,0x00,0x00,0x48,0x8b,0x05,0xd8,0x6a,0x8d,0x08,0x48,
    0x8d,0x3d,0xd1,0x6a,0x8d,0x08,0x48,0x89,0xde,0xff,0x50,0x10,0x8b,0x03,0x49,0x8b
};

/* 64-byte context for Shadow Map resolution tier load (site at +32, len 6): */
static const uint8_t LOW_SHADOWS_CTX[64] = {
    0x00,0x00,0xc5,0xfa,0x10,0x05,0xb3,0x8a,0xd0,0x05,0x88,0x05,0xd6,0x19,0xf1,0x07,
    0x8a,0x83,0x6d,0x0d,0x00,0x00,0xc5,0xf8,0x28,0xc8,0x88,0x05,0xd5,0x19,0xf1,0x07,
    0x8b,0x83,0x64,0x0c,0x00,0x00,0x48,0x83,0xf8,0x03,0x77,0x0c,0x48,0x8d,0x0d,0x76,
    0xb8,0xb3,0x05,0xc5,0xfa,0x10,0x0c,0x81,0xc5,0xfa,0x5e,0xc1,0xc4,0xe2,0x79,0x18
};

/* 64-byte context for ReflectionQuality (0xc88) map size check (site at +32, len 7): */
static const uint8_t LOW_REFLECTIONS_CTX[64] = {
    0xff,0xff,0xc5,0xfa,0x11,0x05,0xa5,0x75,0xc3,0x06,0x0f,0xb6,0x83,0x18,0x0e,0x00,
    0x00,0x83,0xc8,0x02,0x89,0x05,0xb9,0x7b,0x84,0x0a,0x8b,0x05,0xbf,0xe3,0xf5,0x07,
    0x83,0xbb,0x88,0x0c,0x00,0x00,0x00,0xc6,0x05,0xcd,0x17,0xf6,0x07,0x01,0x0f,0x9f,
    0xc1,0xc4,0xe2,0x71,0xf7,0xd2,0x8d,0x71,0x07,0x89,0x0d,0xa0,0xe3,0xf5,0x07,0x89
};

/* 64-byte contexts for remaining 3 GlobalIlluminationQuality (0xc6c) readers: */
static const uint8_t PRO_RT_HALF2_CTX[64] = {
    0x00,0xff,0xce,0x83,0xfe,0x0e,0x77,0x16,0x89,0xf0,0x48,0x8d,0x0d,0x08,0x3a,0xba,
    0x03,0x48,0x8d,0x15,0x3d,0x3a,0xba,0x03,0x8b,0x0c,0x81,0x8b,0x3c,0x82,0x89,0xc8,
    0x45,0x8b,0x7e,0x54,0x41,0x0f,0xaf,0xc9,0x41,0x0f,0xaf,0xc0,0x99,0xf7,0xff,0x41,
    0x89,0xc4,0x89,0x45,0xa0,0x89,0xc8,0x99,0xf7,0xff,0x48,0x83,0x3d,0x37,0x81,0x3b
};
static const uint8_t PRO_RT_HALF3_CTX[64] = {
    0x0b,0x46,0x08,0x48,0x8d,0x15,0x7e,0xcd,0xa1,0xfb,0x48,0x8b,0x40,0x08,0x48,0x8b,
    0x08,0x48,0x8b,0x49,0x50,0x48,0x39,0xd1,0x75,0x1a,0x48,0x05,0x08,0x08,0x00,0x00,
    0x8b,0xb8,0x64,0x04,0x00,0x00,0xe8,0xad,0xa5,0xf3,0xfd,0xc6,0x05,0x3e,0xfb,0x5f,
    0x04,0x01,0x5d,0xc3,0x48,0x89,0xc7,0xff,0xd1,0xeb,0xe5,0xcc,0xcc,0xcc,0xcc,0xcc
};
static const uint8_t PRO_RT_HALF4_CTX[64] = {
    0x68,0xdd,0x0b,0x48,0x8d,0x15,0x93,0x2a,0x39,0xff,0x48,0x8b,0x40,0x08,0x48,0x8b,
    0x08,0x48,0x8b,0x49,0x50,0x48,0x39,0xd1,0x75,0x4c,0x48,0x05,0x08,0x08,0x00,0x00,
    0x8b,0xb8,0x64,0x04,0x00,0x00,0xe8,0xc2,0x02,0x8b,0x01,0xb0,0x01,0x88,0x05,0x52,
    0x58,0xf7,0x07,0x88,0x05,0xf8,0x7a,0xa5,0x08,0x49,0x8b,0x07,0x48,0x3b,0x43,0x48
};

/* 64-byte contexts for VolumetricsRaymarchQuality (0xda8) & VolumetricsLightingQuality (0xda4): */
static const uint8_t LOW_VOL1_CTX[64] = {
    0x79,0x0c,0x83,0x10,0x0d,0x00,0x00,0x03,0xc5,0xf8,0x11,0x05,0x77,0x8c,0x41,0x08,
    0xc5,0xfa,0x10,0x83,0x1c,0x0d,0x00,0x00,0xc5,0xfa,0x11,0x05,0x77,0x8c,0x41,0x08,
    0x8b,0x83,0xa8,0x0d,0x00,0x00,0x89,0x05,0xa3,0x0b,0xed,0x07,0x8b,0x83,0xa4,0x0d,
    0x00,0x00,0x89,0x05,0x9b,0x0b,0xed,0x07,0x8b,0x83,0x58,0x0c,0x00,0x00,0x89,0x05
};
static const uint8_t LOW_VOL2_CTX[64] = {
    0x77,0x8c,0x41,0x08,0xc5,0xfa,0x10,0x83,0x1c,0x0d,0x00,0x00,0xc5,0xfa,0x11,0x05,
    0x77,0x8c,0x41,0x08,0x8b,0x83,0xa8,0x0d,0x00,0x00,0x89,0x05,0xa3,0x0b,0xed,0x07,
    0x8b,0x83,0xa4,0x0d,0x00,0x00,0x89,0x05,0x9b,0x0b,0xed,0x07,0x8b,0x83,0x58,0x0c,
    0x00,0x00,0x89,0x05,0xaf,0x12,0x14,0x09,0x8a,0x83,0x90,0x0d,0x00,0x00,0x88,0x05
};

/* 64-byte context for ShadowSoftShadows (0xd70 -> filter 2/7/11/12): */
static const uint8_t LOW_SHADOWS2_CTX[64] = {
    0x8b,0x05,0xda,0xb1,0x7f,0x0a,0x48,0x8b,0x40,0x08,0x48,0x8b,0x08,0x48,0x8b,0x49,
    0x50,0x4c,0x39,0xf9,0x0f,0x85,0xce,0x03,0x00,0x00,0x48,0x05,0x08,0x08,0x00,0x00,
    0x8b,0x80,0x68,0x05,0x00,0x00,0x85,0xc0,0x7e,0x18,0x31,0xc9,0x83,0xf8,0x02,0x0f,
    0x95,0xc1,0x83,0xc1,0x0b,0x83,0xf8,0x01,0xb8,0x07,0x00,0x00,0x00,0x0f,0x45,0xc1
};

/* 64-byte context for LodScale (0xce8 -> 0xd796908) 16-byte load+store: */
static const uint8_t LOD_SCALE_CTX[64] = {
    0x8d,0x71,0x07,0x89,0x0d,0xa0,0xe3,0xf5,0x07,0x89,0x15,0x82,0xe3,0xf5,0x07,0x89,
    0x35,0x80,0xe3,0xf5,0x07,0x39,0xc8,0x74,0x07,0xc6,0x05,0x79,0x09,0xf1,0x07,0x01,
    0xc5,0xfa,0x10,0x83,0xe8,0x0c,0x00,0x00,0xc5,0xfa,0x11,0x05,0xb1,0x29,0x82,0x09,
    0xe8,0xd4,0xf3,0x43,0x02,0x8b,0xbb,0x84,0x0c,0x00,0x00,0x8b,0xb3,0x3c,0x0d,0x00
};

/* 64-byte context for BvhQuality (0xcbc -> 0xa919914): */
static const uint8_t LOW_BVH_CTX[64] = {
    0xc5,0xfa,0x5e,0xc1,0xc4,0xe2,0x79,0x18,0xc8,0xc5,0xf8,0x11,0x0d,0x40,0x27,0xf1,
    0x07,0xc5,0xfa,0x11,0x05,0x48,0x27,0xf1,0x07,0xc6,0x05,0x00,0x29,0xf1,0x07,0x01,
    0x8b,0x83,0xbc,0x0c,0x00,0x00,0x89,0x05,0xb9,0x54,0x9a,0x06,0x8b,0x83,0x80,0x0d,
    0x00,0x00,0x89,0x05,0xc5,0x02,0xf1,0x07,0x8b,0x93,0x74,0x0c,0x00,0x00,0x8b,0x83
};

/* 64-byte contexts for MotionBlur (0xd30 -> 0xc62c3ec) loads: */
static const uint8_t MOTION_BLUR1_CTX[64] = {
    0xc5,0xfa,0x11,0x0d,0x76,0x4c,0x5c,0x08,0xc5,0xfa,0x11,0x0d,0xba,0x4c,0x5c,0x08,
    0xc5,0xfa,0x10,0x80,0x30,0x05,0x00,0x00,0xc5,0xfa,0x11,0x05,0x62,0x4c,0x5c,0x08,
    0x8a,0x80,0x28,0x05,0x00,0x00,0x88,0x05,0x4e,0x4c,0x5c,0x08,0x5d,0xe9,0x9c,0x59,
    0x93,0xfe,0x48,0x89,0xc7,0xff,0xd1,0xc5,0xfa,0x10,0x05,0x3f,0x4c,0x5c,0x08,0xeb
};
static const uint8_t MOTION_BLUR2_CTX[64] = {
    0x00,0xc5,0xfa,0x10,0x80,0x30,0x05,0x00,0x00,0x48,0x63,0x0d,0x8e,0xcd,0x8f,0x04,
    0xc5,0xf0,0x57,0xc9,0xc5,0xe8,0x57,0xd2,0xc5,0xfa,0x11,0x05,0xbe,0x04,0x17,0x06,
    0x8a,0x80,0x28,0x05,0x00,0x00,0x88,0x05,0xaa,0x04,0x17,0x06,0x48,0x85,0xc9,0x0f,
    0x8e,0x1a,0x02,0x00,0x00,0x48,0x8d,0x15,0x5e,0xcd,0x8f,0x04,0x45,0x31,0xff,0x48
};

#ifndef LOG_DIR
#define LOG_DIR "/data/gtavi-unlockfps"
#endif
#define LOG_PATH  LOG_DIR "/gtavi-unlockfps.log"
#define CONF_PATH LOG_DIR "/fps.txt"

#define LOCK_PORT 47961 /* loopback UDP port held as a single-instance lock */

#define POLL_USEC      100000u /* 100 ms: fast enough to beat render init, gentle on shell */
#define FAST_POLL_USEC 1000u   /* 1 ms with pro_highres: freeze GTAVI before its heap init */
#define IDLE_POLL_USEC 500000u /* 500 ms: daemon sleep while GTAVI is already patched */
#define WAIT_POLLS     6000u   /* 6000 * 100 ms = 10 min (one-shot mode) */
#define LIST_TRIES    5u
#define RESOLVE_TRIES 60u     /* 60 * 100 ms = 6 s for eboot mapping */
#define RESOLVE_USEC  100000u
#define WRITE_TRIES   3u
#define WRITE_USEC    100000u

enum {
    EXIT_OK = 0,
    EXIT_ALREADY_RUNNING = 1,
    EXIT_NO_GAME = 3,
    EXIT_NO_TARGET = 5,
    EXIT_WRITE_FAIL = 7,
    EXIT_NO_PROC_LIST = 8,
};

/* --------------------------------------------------------- notifications */

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

#ifndef HOST_TEST
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceKernelGetAppInfo(pid_t pid, void *info);
#endif

/* ------------------------------------------------------------------- log */

static FILE *g_log;
static struct timespec g_t0;
static uint32_t g_target_fps = DEFAULT_FPS;
static uint32_t g_res_pct = DEFAULT_RES_PCT;
static uint32_t g_refresh_hz = DEFAULT_REFRESH_HZ;
static uint32_t g_output_res = DEFAULT_OUTPUT_RES;
static uint32_t g_pro_rt_half = DEFAULT_PRO_RT_HALF;
static uint32_t g_daemon = DEFAULT_DAEMON;
static uint32_t g_pro_highres = DEFAULT_PRO_HIGHRES;
static uint32_t g_close_fix = DEFAULT_CLOSE_FIX;
static uint32_t g_motion_blur = DEFAULT_MOTION_BLUR;
static uint32_t g_pro_taa2u = DEFAULT_PRO_TAA2U;
static uint32_t g_low_shadows = DEFAULT_LOW_SHADOWS;
static uint32_t g_low_reflections = DEFAULT_LOW_REFLECTIONS;
static uint32_t g_low_volumetric = DEFAULT_LOW_VOLUMETRIC;
static uint32_t g_lod_scale = DEFAULT_LOD_SCALE;
static uint32_t g_low_bvh = DEFAULT_LOW_BVH;
static uint32_t g_base_highres = DEFAULT_BASE_HIGHRES;
static float g_scale;            /* final render scale (even width and height) */
static uint32_t g_res_w, g_res_h;  /* resulting internal resolution */
static uint32_t g_heap_pages;      /* 0 = leave the heap stock */

static unsigned long elapsed_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)((long long)(t.tv_sec - g_t0.tv_sec) * 1000 +
                           (t.tv_nsec - g_t0.tv_nsec) / 1000000);
}

static void log_open(void) {
    mkdir(LOG_DIR, 0777);
    g_log = fopen(LOG_PATH, "w");
}

static void logmsg(const char *lvl, const char *fmt, ...) {
    char msg[256];
    unsigned long ms = elapsed_ms();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (g_log) {
        fprintf(g_log, "[%4lu.%03lu] %-4s %s\n", ms / 1000, ms % 1000, lvl, msg);
        fflush(g_log);
        fsync(fileno(g_log));
    }
#ifndef HOST_TEST
    klog_printf("[gtavi-unlockfps] %s %s\n", lvl, msg);
#else
    fprintf(stderr, "[gtavi-unlockfps] %s %s\n", lvl, msg);
#endif
}

static void send_notification(const char *prefix, const char *fmt, va_list ap) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    strcpy(req.message, prefix);
    vsnprintf(req.message + strlen(req.message),
              sizeof(req.message) - strlen(req.message), fmt, ap);
    logmsg("NOTE", "%s", req.message);
#ifndef HOST_TEST
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
#endif
}

static void notify(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    send_notification("GTAVI Unlock FPS: ", fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------- single instance */

static int g_lock = -1;

static int lock_instance(void) {
    struct sockaddr_in sa;
    g_lock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_lock < 0)
        return 0;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(LOCK_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_lock, (struct sockaddr *)&sa, sizeof(sa))) {
        close(g_lock);
        g_lock = -1;
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------- settings */

static void load_settings(void) {
    char line[128];
    int saw_res_pct = 0;
    int saw_refresh_hz = 0;
    int saw_output_res = 0;
    int saw_pro_rt_half = 0;
    int saw_daemon = 0;
    int saw_pro_highres = 0;
    int saw_close_fix = 0;
    int saw_motion_blur = 0;
    int saw_pro_taa2u = 0;
    int saw_low_shadows = 0;
    int saw_low_reflections = 0;
    int saw_low_volumetric = 0;
    int saw_lod_scale = 0;
    int saw_low_bvh = 0;
    int saw_base_highres = 0;
    FILE *f;

    g_pro_highres = DEFAULT_PRO_HIGHRES;
    g_target_fps = DEFAULT_FPS;
    g_res_pct = DEFAULT_RES_PCT;
    g_refresh_hz = DEFAULT_REFRESH_HZ;
    g_output_res = DEFAULT_OUTPUT_RES;
    g_pro_rt_half = DEFAULT_PRO_RT_HALF;
    g_daemon = DEFAULT_DAEMON;
    g_close_fix = DEFAULT_CLOSE_FIX;
    g_motion_blur = DEFAULT_MOTION_BLUR;
    g_pro_taa2u = DEFAULT_PRO_TAA2U;
    g_low_shadows = DEFAULT_LOW_SHADOWS;
    g_low_reflections = DEFAULT_LOW_REFLECTIONS;
    g_low_volumetric = DEFAULT_LOW_VOLUMETRIC;
    g_lod_scale = DEFAULT_LOD_SCALE;
    g_low_bvh = DEFAULT_LOW_BVH;
    g_base_highres = DEFAULT_BASE_HIGHRES;

    f = fopen(CONF_PATH, "r");
    if (!f) {
        f = fopen(CONF_PATH, "w");
        if (f) {
            fprintf(f, "# GTAVI Unlock FPS v%s settings (/data/gtavi-unlockfps/fps.txt)\n"
                       "#\n"
                       "# --- Framerate & Display ---\n"
                       "# fps: target FPS cap (30..120). 60 Hz TV: 30=locked 30, 60=up to 60. 120 Hz: also 40.\n"
                       "fps=%u\n"
                       "# res_pct: internal render resolution %% (0=stock, 50=1080p default, 25..100)\n"
                       "res_pct=%u\n"
                       "# refresh_hz: 60=standard TV (default), 120=only when forcing 120 Hz via ps-patch-system\n"
                       "refresh_hz=%u\n"
                       "# output_res: output resolution (0=4K default, 1440=1440p, 1080=1080p, experimental)\n"
                       "output_res=%u\n"
                       "# motion_blur: 1=game default, 0=disable motion blur (sharper image at lower res_pct)\n"
                       "motion_blur=%u\n"
                       "#\n"
                       "# --- PS5 Pro Options ---\n"
                       "# pro_highres: 1=expand PS5 Pro memory for res_pct above 66%% (up to 90%%/4K, needs 4K output)\n"
                       "pro_highres=%u\n"
                       "# pro_rt_half: 1=lower PS5 Pro RT/GI quality to base PS5 tier for higher FPS (experimental)\n"
                       "pro_rt_half=%u\n"
                       "# pro_taa2u: 1=use TAA2U instead of PSSR on PS5 Pro (extra GPU headroom / less shimmer)\n"
                       "pro_taa2u=%u\n"
                       "#\n"
                       "# --- Performance & Graphics Tweaks (Experimental, 0=stock) ---\n"
                       "# low_shadows: 0=stock, 1=1280 shadow map, 2=1280 shadow map + fast soft shadows\n"
                       "low_shadows=%u\n"
                       "# low_reflections: 1=lower reflection map resolution (256 -> 128)\n"
                       "low_reflections=%u\n"
                       "# low_volumetric: 1=lower volumetric clouds/fog quality for extra GPU FPS\n"
                       "low_volumetric=%u\n"
                       "# lod_scale: draw distance %% (0=stock, 50..100 = 50%%..100%%, reduces CPU+GPU load)\n"
                       "lod_scale=%u\n"
                       "# low_bvh: 1=lighter ray-tracing BVH geometry tree (reduces CPU+GPU RT load)\n"
                       "low_bvh=%u\n"
                       "# base_highres: 1=expand base PS5 memory for res_pct above 67%% (e.g. 1440p + 75..100%%)\n"
                       "base_highres=%u\n"
                       "#\n"
                       "# --- Stability & Behavior ---\n"
                       "# close_fix: 1=prevent CE-108255-1 error when closing the game (default), 0=stock\n"
                       "close_fix=%u\n"
                       "# daemon: 0=patch once and exit (default), 1=stay in background across game restarts\n"
                       "daemon=%u\n",
                       TOOL_VERSION,
                       DEFAULT_FPS, DEFAULT_RES_PCT, DEFAULT_REFRESH_HZ, DEFAULT_OUTPUT_RES,
                       DEFAULT_MOTION_BLUR, DEFAULT_PRO_HIGHRES, DEFAULT_PRO_RT_HALF,
                       DEFAULT_PRO_TAA2U, DEFAULT_LOW_SHADOWS, DEFAULT_LOW_REFLECTIONS,
                       DEFAULT_LOW_VOLUMETRIC, DEFAULT_LOD_SCALE, DEFAULT_LOW_BVH,
                       DEFAULT_BASE_HIGHRES, DEFAULT_CLOSE_FIX, DEFAULT_DAEMON);
            fclose(f);
            logmsg("INFO", "wrote default settings to %s", CONF_PATH);
        }
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        int v;
        if (line[0] == '#' || !eq)
            continue;
        v = atoi(eq + 1);
        if (!strncmp(line, "fps", 3)) {
            if (v >= (int)MIN_FPS && v <= (int)MAX_FPS) {
                g_target_fps = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring fps=%d (must be %u..%u)", v, MIN_FPS, MAX_FPS);
            }
        } else if (!strncmp(line, "res_pct", 7)) {
            saw_res_pct = 1;
            if (v == 0 || (v >= (int)MIN_RES_PCT && v <= (int)MAX_RES_PCT)) {
                g_res_pct = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring res_pct=%d (must be 0 or %u..%u)",
                       v, MIN_RES_PCT, MAX_RES_PCT);
            }
        } else if (!strncmp(line, "refresh_hz", 10)) {
            saw_refresh_hz = 1;
            if (v == 60 || v == 120) {
                g_refresh_hz = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring refresh_hz=%d (must be 60 or 120)", v);
            }
        } else if (!strncmp(line, "output_res", 10)) {
            saw_output_res = 1;
            if (v == 0 || v == 1080 || v == 1440) {
                g_output_res = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring output_res=%d (must be 0, 1080, or 1440)", v);
            }
        } else if (!strncmp(line, "pro_rt_half", 11)) {
            saw_pro_rt_half = 1;
            if (v == 0 || v == 1) {
                g_pro_rt_half = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring pro_rt_half=%d (must be 0 or 1)", v);
            }
        } else if (!strncmp(line, "pro_highres", 11)) {
            saw_pro_highres = 1;
            if (v == 0 || v == 1) {
                g_pro_highres = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring pro_highres=%d (must be 0 or 1)", v);
            }
        } else if (!strncmp(line, "close_fix", 9)) {
            saw_close_fix = 1;
            if (v == 0 || v == 1) {
                g_close_fix = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring close_fix=%d (must be 0 or 1)", v);
            }
        } else if (!strncmp(line, "motion_blur", 11)) {
            saw_motion_blur = 1;
            if (v == 0 || v == 1) {
                g_motion_blur = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring motion_blur=%d (must be 0 or 1)", v);
            }
        } else if (!strncmp(line, "pro_taa2u", 9)) {
            saw_pro_taa2u = 1;
            if (v == 0 || v == 1) {
                g_pro_taa2u = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring pro_taa2u=%d (must be 0 or 1)", v);
            }
        } else if (!strncmp(line, "low_shadows", 11)) {
            saw_low_shadows = 1;
            if (v >= 0 && v <= 2) {
                g_low_shadows = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring low_shadows=%d (must be 0, 1, or 2)", v);
            }
        } else if (!strncmp(line, "low_reflections", 15)) {
            saw_low_reflections = 1;
            if (v == 0 || v == 1) {
                g_low_reflections = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring low_reflections=%d (must be 0 or 1)", v);
            }
        } else if (!strncmp(line, "low_volumetric", 14)) {
            saw_low_volumetric = 1;
            if (v == 0 || v == 1) {
                g_low_volumetric = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring low_volumetric=%d (must be 0 or 1)", v);
            }
        } else if (!strncmp(line, "lod_scale", 9)) {
            saw_lod_scale = 1;
            if (v == 0 || (v >= (int)MIN_LOD_SCALE && v <= (int)MAX_LOD_SCALE)) {
                g_lod_scale = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring lod_scale=%d (must be 0 or %u..%u)",
                       v, MIN_LOD_SCALE, MAX_LOD_SCALE);
            }
        } else if (!strncmp(line, "low_bvh", 7)) {
            saw_low_bvh = 1;
            if (v == 0 || v == 1) {
                g_low_bvh = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring low_bvh=%d (must be 0 or 1)", v);
            }
        } else if (!strncmp(line, "base_highres", 12)) {
            saw_base_highres = 1;
            if (v == 0 || v == 1) {
                g_base_highres = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring base_highres=%d (must be 0 or 1)", v);
            }
        } else if (!strncmp(line, "daemon", 6)) {
            saw_daemon = 1;
            if (v == 0 || v == 1) {
                g_daemon = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring daemon=%d (must be 0 or 1)", v);
            }
        }
        /* Legacy rt_reflections= lines from older versions are silently ignored. */
    }
    fclose(f);

    if (!saw_res_pct || !saw_refresh_hz || !saw_output_res || !saw_pro_rt_half || !saw_daemon ||
        !saw_pro_highres || !saw_close_fix || !saw_motion_blur || !saw_pro_taa2u ||
        !saw_low_shadows || !saw_low_reflections || !saw_low_volumetric || !saw_lod_scale ||
        !saw_low_bvh || !saw_base_highres) {
        f = fopen(CONF_PATH, "a");
        if (f) {
            if (!saw_pro_highres) {
                fprintf(f, "# pro_highres: 1=expand PS5 Pro memory for res_pct above 66%% (up to 90%%/4K, needs 4K output)\n"
                           "pro_highres=%u\n", DEFAULT_PRO_HIGHRES);
                logmsg("INFO", "appended default pro_highres=%u to %s", DEFAULT_PRO_HIGHRES, CONF_PATH);
            }
            if (!saw_res_pct) {
                fprintf(f, "# res_pct: internal render resolution %% (0=stock, 50=1080p default, 25..100)\n"
                           "res_pct=%u\n", DEFAULT_RES_PCT);
                logmsg("INFO", "appended default res_pct=%u to %s", DEFAULT_RES_PCT, CONF_PATH);
            }
            if (!saw_refresh_hz) {
                fprintf(f, "# refresh_hz: 60=standard TV (default), 120=only when forcing 120 Hz via ps-patch-system\n"
                           "refresh_hz=%u\n", DEFAULT_REFRESH_HZ);
                logmsg("INFO", "appended default refresh_hz=%u to %s", DEFAULT_REFRESH_HZ, CONF_PATH);
            }
            if (!saw_output_res) {
                fprintf(f, "# output_res: output resolution (0=4K default, 1440=1440p, 1080=1080p, experimental)\n"
                           "output_res=%u\n", DEFAULT_OUTPUT_RES);
                logmsg("INFO", "appended default output_res=%u to %s", DEFAULT_OUTPUT_RES, CONF_PATH);
            }
            if (!saw_pro_rt_half) {
                fprintf(f, "# pro_rt_half: 1=lower PS5 Pro RT/GI quality to base PS5 tier for higher FPS (experimental)\n"
                           "pro_rt_half=%u\n", DEFAULT_PRO_RT_HALF);
                logmsg("INFO", "appended default pro_rt_half=%u to %s", DEFAULT_PRO_RT_HALF, CONF_PATH);
            }
            if (!saw_daemon) {
                fprintf(f, "# daemon: 0=patch once and exit (default), 1=stay in background across game restarts\n"
                           "daemon=%u\n", DEFAULT_DAEMON);
                logmsg("INFO", "appended default daemon=%u to %s", DEFAULT_DAEMON, CONF_PATH);
            }
            if (!saw_close_fix) {
                fprintf(f, "# close_fix: 1=prevent CE-108255-1 error when closing the game (default), 0=stock\n"
                           "close_fix=%u\n", DEFAULT_CLOSE_FIX);
                logmsg("INFO", "appended default close_fix=%u to %s", DEFAULT_CLOSE_FIX, CONF_PATH);
            }
            if (!saw_motion_blur) {
                fprintf(f, "# motion_blur: 1=game default, 0=disable motion blur (sharper image at lower res_pct)\n"
                           "motion_blur=%u\n", DEFAULT_MOTION_BLUR);
                logmsg("INFO", "appended default motion_blur=%u to %s", DEFAULT_MOTION_BLUR, CONF_PATH);
            }
            if (!saw_pro_taa2u) {
                fprintf(f, "# pro_taa2u: 1=use TAA2U instead of PSSR on PS5 Pro (extra GPU headroom / less shimmer)\n"
                           "pro_taa2u=%u\n", DEFAULT_PRO_TAA2U);
                logmsg("INFO", "appended default pro_taa2u=%u to %s", DEFAULT_PRO_TAA2U, CONF_PATH);
            }
            if (!saw_low_shadows) {
                fprintf(f, "# low_shadows: 0=stock, 1=1280 shadow map, 2=1280 shadow map + fast soft shadows\n"
                           "low_shadows=%u\n", DEFAULT_LOW_SHADOWS);
                logmsg("INFO", "appended default low_shadows=%u to %s", DEFAULT_LOW_SHADOWS, CONF_PATH);
            }
            if (!saw_low_reflections) {
                fprintf(f, "# low_reflections: 1=lower reflection map resolution (256 -> 128, experimental)\n"
                           "low_reflections=%u\n", DEFAULT_LOW_REFLECTIONS);
                logmsg("INFO", "appended default low_reflections=%u to %s", DEFAULT_LOW_REFLECTIONS, CONF_PATH);
            }
            if (!saw_low_volumetric) {
                fprintf(f, "# low_volumetric: 1=lower volumetric clouds/fog quality for extra GPU FPS (experimental)\n"
                           "low_volumetric=%u\n", DEFAULT_LOW_VOLUMETRIC);
                logmsg("INFO", "appended default low_volumetric=%u to %s", DEFAULT_LOW_VOLUMETRIC, CONF_PATH);
            }
            if (!saw_lod_scale) {
                fprintf(f, "# lod_scale: draw distance %% (0=stock, 50..100 = 50%%..100%%, reduces CPU+GPU load, experimental)\n"
                           "lod_scale=%u\n", DEFAULT_LOD_SCALE);
                logmsg("INFO", "appended default lod_scale=%u to %s", DEFAULT_LOD_SCALE, CONF_PATH);
            }
            if (!saw_low_bvh) {
                fprintf(f, "# low_bvh: 1=lighter ray-tracing BVH geometry tree (reduces CPU+GPU RT load, experimental)\n"
                           "low_bvh=%u\n", DEFAULT_LOW_BVH);
                logmsg("INFO", "appended default low_bvh=%u to %s", DEFAULT_LOW_BVH, CONF_PATH);
            }
            if (!saw_base_highres) {
                fprintf(f, "# base_highres: 1=expand base PS5 memory for res_pct above 67%% (e.g. 1440p + 75..100%%, experimental)\n"
                           "base_highres=%u\n", DEFAULT_BASE_HIGHRES);
                logmsg("INFO", "appended default base_highres=%u to %s", DEFAULT_BASE_HIGHRES, CONF_PATH);
            }
            fclose(f);
        }
    }
}

/* ---------------------------------------------------------- game access */

#define APP_INFO_LEN 0x100
#define MAX_EBOOTS   16

struct eboot_proc {
    uint32_t pid;
    char titleid[16];
};

static int proc_read(uint32_t pid, uint64_t addr, uint8_t *out, uint32_t len) {
#ifndef HOST_TEST
    return mdbg_copyout((pid_t)pid, (intptr_t)addr, out, len) ? -1 : 0;
#else
    (void)pid; (void)addr; (void)out; (void)len;
    return -1;
#endif
}

static int proc_write(uint32_t pid, uint64_t addr, const uint8_t *data,
                      uint32_t len) {
#ifndef HOST_TEST
    if ((kernel_get_fw_version() & 0xffff0000u) >= 0x08400000u) {
        int rc = ptwalk_write(pid, addr, data, len);
        if (rc == PTWALK_OK)
            return 0;
        logmsg("WARN", "DMAP write failed (rc %d), trying mdbg", rc);
    }
    return mdbg_copyin((pid_t)pid, data, (intptr_t)addr, len) ? -1 : 0;
#else
    (void)pid; (void)addr; (void)data; (void)len;
    return -1;
#endif
}

static int eboot_base(uint32_t pid, uint64_t *base) {
#ifndef HOST_TEST
    uint32_t handle;
    intptr_t b;
    if (kernel_dynlib_handle((pid_t)pid, "eboot.bin", &handle))
        return -1;
    b = kernel_dynlib_mapbase_addr((pid_t)pid, handle);
    if (b <= 0)
        return -1;
    *base = (uint64_t)b;
    return 0;
#else
    (void)pid; (void)base;
    return -1;
#endif
}

#ifndef HOST_TEST
static int find_titleid(const uint8_t *info, size_t len, char out[16]) {
    size_t i, k;
    for (i = 0; i + 9 <= len; i++) {
        for (k = 0; k < 9; k++) {
            uint8_t c = info[i + k];
            if (k < 4 ? (c < 'A' || c > 'Z') : (c < '0' || c > '9'))
                break;
        }
        if (k == 9 && (i + 9 == len || info[i + 9] < '0' || info[i + 9] > '9')) {
            memcpy(out, info + i, 9);
            out[9] = 0;
            return 0;
        }
    }
    strcpy(out, "?");
    return -1;
}
#endif

static int list_eboot_procs(struct eboot_proc out[MAX_EBOOTS], unsigned *count) {
#ifndef HOST_TEST
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    uint8_t *buf, *p;
    uint8_t info[APP_INFO_LEN];
    *count = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) || !(buf = malloc(size)))
        return -1;
    if (sysctl(mib, 4, buf, &size, NULL, 0)) {
        free(buf);
        return -1;
    }
    for (p = buf; p < buf + size && *count < MAX_EBOOTS;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)p;
        if (ki->ki_structsize <= 0)
            break;
        p += ki->ki_structsize;
        if (strncmp(ki->ki_comm, "eboot.bin", 9) || ki->ki_pid <= 0)
            continue;
        memset(info, 0, sizeof(info));
        if (sceKernelGetAppInfo(ki->ki_pid, info))
            continue;
        out[*count].pid = (uint32_t)ki->ki_pid;
        find_titleid(info, sizeof(info), out[*count].titleid);
        (*count)++;
    }
    free(buf);
    return 0;
#else
    (void)out;
    *count = 0;
    return 0;
#endif
}

/* ------------------------------------------------------------ the patch */

struct patch_site {
    const char *name;
    uint32_t rva;
    uint32_t len;
    const uint8_t *ctx;
    uint8_t patch[24];
};

static void put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t float_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static int ctx_matches_site(const uint8_t cur[64], const struct patch_site *s) {
    return !memcmp(cur, s->ctx, CTX_OFF) &&
           !memcmp(cur + CTX_OFF + s->len, s->ctx + CTX_OFF + s->len,
                   64u - CTX_OFF - s->len);
}

static int verify_site(uint32_t pid, uint64_t base, const struct patch_site *s) {
    uint8_t ctx[64];
    if (proc_read(pid, base + s->rva - CTX_OFF, ctx, 64))
        return -1;
    return ctx_matches_site(ctx, s) ? 1 : 0;
}

/* Returns: 1 = matches GTAVI (clean or already patched), 0 = mismatch,
 * -1 = memory not ready yet. */
static int verify_gtavi_process(uint32_t pid, const struct patch_site sites[2],
                                uint64_t *out_base) {
    uint64_t base = 0;
    unsigned tries;
    for (tries = 1; tries <= RESOLVE_TRIES; tries++) {
        if (!eboot_base(pid, &base)) {
            int r0 = verify_site(pid, base, &sites[0]);
            if (r0 == 0)
                return 0;
            if (r0 == 1) {
                int r1 = verify_site(pid, base, &sites[1]);
                if (r1 == 0)
                    return 0;
                if (r1 == 1) {
                    *out_base = base;
                    return 1;
                }
            }
        }
        usleep(RESOLVE_USEC);
    }
    return -1;
}

static int apply_site(uint32_t pid, uint64_t base, const struct patch_site *s,
                      int *already_patched) {
    uint64_t addr = base + s->rva;
    uint8_t cur[24];
    unsigned t;
    for (t = 1; t <= WRITE_TRIES; t++) {
        if (proc_read(pid, addr, cur, s->len)) {
            logmsg("WARN", "%s try %u/%u: read failed", s->name, t, WRITE_TRIES);
        } else if (!memcmp(cur, s->patch, s->len)) {
            logmsg("OK", "%s already patched at 0x%llx", s->name,
                   (unsigned long long)addr);
            *already_patched = 1;
            return 0;
        } else if (proc_write(pid, addr, s->patch, s->len)) {
            logmsg("WARN", "%s try %u/%u: write rejected", s->name, t, WRITE_TRIES);
        } else if (proc_read(pid, addr, cur, s->len)) {
            logmsg("WARN", "%s try %u/%u: readback failed", s->name, t, WRITE_TRIES);
        } else if (!memcmp(cur, s->patch, s->len)) {
            logmsg("OK", "%s patched at 0x%llx (try %u)", s->name,
                   (unsigned long long)addr, t);
            return 0;
        } else {
            logmsg("WARN", "%s try %u/%u: readback mismatch", s->name, t, WRITE_TRIES);
        }
        usleep(WRITE_USEC);
    }
    return -1;
}

/* Update live RAM globals so both the FPS unlock and normal 1.0x game speed
 * (fwTimer min delta-time floor) take effect immediately, even mid-session. */
static void update_live_globals(uint32_t pid, uint64_t base) {
    uint32_t cur_us = 0;
    uint32_t want_us = 1000000u / g_target_fps;
    uint8_t auto_lock = 0xffu;
    uint8_t buf[4];
    uint8_t floor_buf[4];

    /* Set fwTimer min/target frame floor to 1/3000s (0x39aec33e, native RAGE value) */
    put32le(floor_buf, 0x39aec33eu);
    if (!proc_write(pid, base + G_MIN_FRAME_SEC_RVA, floor_buf, 4) &&
        !proc_write(pid, base + G_TARGET_FRAME_SEC_RVA, floor_buf, 4)) {
        logmsg("OK", "updated fwTimer min/target frame floor to 1/3000s (1.0x speed)");
    } else {
        logmsg("WARN", "could not update fwTimer floor globals");
    }

    if (proc_read(pid, base + G_TARGET_FRAMETIME_US_RVA, buf, 4))
        return;
    cur_us = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
             ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    if (cur_us == 0 || cur_us == want_us)
        return;

    put32le(buf, want_us);
    if (!proc_write(pid, base + G_TARGET_FRAMETIME_US_RVA, buf, 4) &&
        !proc_write(pid, base + G_REQUESTED_FRAMELOCK_RVA, &auto_lock, 1)) {
        logmsg("OK", "updated live g_TargetFrameTimeUs: %u -> %u us", cur_us, want_us);
    } else {
        logmsg("WARN", "could not update live g_TargetFrameTimeUs (non-fatal)");
    }

    if (!g_motion_blur) {
        uint8_t zero = 0;
        if (!proc_write(pid, base + G_MOTION_BLUR_RVA, &zero, 1))
            logmsg("OK", "disabled live MotionBlur global");
    }
}

static int is_skipped_pid(const uint32_t *skipped, unsigned n, uint32_t pid) {
    unsigned i;
    for (i = 0; i < n; i++)
        if (skipped[i] == pid)
            return 1;
    return 0;
}

static void add_skipped_pid(uint32_t *skipped, unsigned *n, uint32_t pid) {
    if (*n < MAX_EBOOTS && !is_skipped_pid(skipped, *n, pid))
        skipped[(*n)++] = pid;
}

static int is_wanted_title(const char *tid) {
    size_t i;
    for (i = 0; i < sizeof(WANT_TITLE_IDS) / sizeof(WANT_TITLE_IDS[0]); i++)
        if (!strcmp(tid, WANT_TITLE_IDS[i]))
            return 1;
    return 0;
}

static int finish(int code) {
    logmsg("INFO", "exit code %d", code);
    if (g_lock >= 0)
        close(g_lock);
    if (g_log)
        fclose(g_log);
    return code;
}

struct patch_bundle {
    struct patch_site fps[2];
    struct patch_site timestep;
    struct patch_site timestep2;
    struct patch_site taa2u;
    struct patch_site pssr;
    struct patch_site hz[6];
    struct patch_site out[3];
    struct patch_site pro_rt_half[4];
    struct patch_site close_fix;
    struct patch_site pro_taa2u;
    struct patch_site low_shadows;
    struct patch_site low_shadows2;
    struct patch_site low_reflections;
    struct patch_site low_vol[2];
    struct patch_site lod_scale;
    struct patch_site low_bvh;
    struct patch_site motion_blur[2];
};

static int is_site_patched(uint32_t pid, uint64_t base, const struct patch_site *s) {
    uint8_t cur[24];
    if (proc_read(pid, base + s->rva, cur, s->len))
        return 0;
    return !memcmp(cur, s->patch, s->len);
}

/* Verify ALL sites in a group (which may have overlapping 64-byte contexts)
 * before writing ANY of them. Returns 1 if all sites were applied or already
 * patched, 0 on mismatch or write failure. */
static int apply_site_group(uint32_t pid, uint64_t base,
                            const struct patch_site *sites, unsigned n,
                            const char *group_name, int *all_already) {
    unsigned i;
    int all_alr = 1;
    int any_already = 0;

    for (i = 0; i < n; i++) {
        if (is_site_patched(pid, base, &sites[i])) {
            any_already = 1;
        } else {
            all_alr = 0;
        }
    }
    if (all_alr) {
        for (i = 0; i < n; i++)
            logmsg("OK", "%s already patched at 0x%llx", sites[i].name,
                   (unsigned long long)(base + sites[i].rva));
        *all_already = 1;
        return 1;
    }
    if (!any_already) {
        for (i = 0; i < n; i++) {
            if (verify_site(pid, base, &sites[i]) != 1) {
                logmsg("WARN", "%s context mismatch (%s)", group_name, sites[i].name);
                return 0;
            }
        }
    }
    for (i = 0; i < n; i++) {
        int dummy = 0;
        if (apply_site(pid, base, &sites[i], &dummy)) {
            logmsg("WARN", "%s write failed (%s)", group_name, sites[i].name);
            return 0;
        }
    }
    *all_already = 0;
    return 1;
}

static void format_opt_flags(char *buf, size_t sz) {
    size_t len = 0;
    buf[0] = '\0';
    if (g_refresh_hz == 120)
        len += (size_t)snprintf(buf + len, sz - len, ", 120Hz");
    if (g_output_res == 1440 || g_output_res == 1080)
        len += (size_t)snprintf(buf + len, sz - len, ", Out %up", g_output_res);
    if (g_pro_rt_half)
        len += (size_t)snprintf(buf + len, sz - len, ", Pro RT Half");
    if (g_pro_taa2u)
        len += (size_t)snprintf(buf + len, sz - len, ", TAA2U");
    if (g_low_shadows)
        len += (size_t)snprintf(buf + len, sz - len, ", LowShad%u", g_low_shadows);
    if (g_low_reflections)
        len += (size_t)snprintf(buf + len, sz - len, ", LowRefl");
    if (g_low_volumetric)
        len += (size_t)snprintf(buf + len, sz - len, ", LowVol");
    if (g_lod_scale)
        len += (size_t)snprintf(buf + len, sz - len, ", LOD %u%%", g_lod_scale);
    if (g_low_bvh)
        len += (size_t)snprintf(buf + len, sz - len, ", LowBVH");
    if (!g_motion_blur)
        len += (size_t)snprintf(buf + len, sz - len, ", NoMB");
    if (g_base_highres)
        len += (size_t)snprintf(buf + len, sz - len, ", BaseHR");
}

/* Transient heap pages needed for a w x h internal resolution at 4K output, from two
 * measured peaks (883 MB @ 2572x1447, 1066 MB @ 2752x1548) plus a safety margin. */
static uint32_t heap_pages_for(uint32_t w, uint32_t h) {
    double mb = 340.8 * ((double)w * (double)h / 1e6) - 385.5 + HEAP_MARGIN_MB;
    uint32_t pages = (uint32_t)(mb * 16.0) + 1u; /* 16 pages of 64 KB per MB */
    return pages;
}

/* Pick the render scale: closest to res_pct with EVEN width and height (odd sizes break
 * half-res reflection buffers -> green speckles). With pro_highres, also size the heap and
 * step the resolution down until it fits in HEAP_PAGES_MAX. */
static void compute_resolution(void) {
    uint32_t ow = 3840u, oh = 2160u;
    int h0, d, k, step_down = 0;

    g_scale = 0.0f;
    g_res_w = g_res_h = 0;
    g_heap_pages = 0;
    if (g_res_pct == 0)
        return;
    if (g_output_res == 1440) { ow = 2560u; oh = 1440u; }
    if (g_output_res == 1080) { ow = 1920u; oh = 1080u; }

    h0 = (int)(oh * g_res_pct / 100u) & ~1;
    for (;;) {
        /* search 0,+2,-2,+4,-4.. around the target; only downward once stepping down */
        for (k = 0; k <= 40; k++) {
            int h;
            d = step_down ? -2 * k : ((k & 1) ? k + 1 : -k);
            h = h0 + d;
            volatile float s, fw, fh;
            uint32_t w2, h2;
            if (h < 64)
                continue;
            s = ((float)h + 0.25f) / (float)oh;
            fw = (float)ow * s; /* same single-precision math as the game */
            fh = (float)oh * s;
            w2 = (uint32_t)fw;
            h2 = (uint32_t)fh;
            if ((w2 & 1u) || h2 != (uint32_t)h)
                continue;
            g_scale = s;
            g_res_w = w2;
            g_res_h = h2;
            break;
        }
        if (!g_scale) {
            logmsg("WARN", "no even resolution near res_pct=%u; using plain scale", g_res_pct);
            g_scale = (float)g_res_pct / 100.0f;
            g_res_w = (uint32_t)((float)ow * g_scale);
            g_res_h = (uint32_t)((float)oh * g_scale);
            return;
        }
        if (!g_pro_highres || g_output_res != 0)
            return;
        g_heap_pages = heap_pages_for(g_res_w, g_res_h);
        if (g_heap_pages <= HEAP_PAGES_STOCK) {
            g_heap_pages = 0; /* fits the stock heap */
            return;
        }
        if (g_heap_pages <= HEAP_PAGES_MAX)
            return;
        logmsg("INFO", "%ux%u needs %u heap pages (max 0x%x); stepping down",
               g_res_w, g_res_h, g_heap_pages, HEAP_PAGES_MAX);
        h0 = (int)g_res_h - 2;
        step_down = 1;
        g_scale = 0.0f;
    }
}

static void build_patches(struct patch_bundle *b) {
    memset(b, 0, sizeof(*b));
    compute_resolution();

    /* FPS cap writers:
     *   Site 1 (6B): mov eax, <fps>; nop          -> b8 <imm32> 90
     *   Site 2 (7B): mov eax, <fps>; xchg ax, ax  -> b8 <imm32> 66 90 */
    b->fps[0].name = "fps_writer1";
    b->fps[0].rva = SITE1_RVA;
    b->fps[0].len = SITE1_LEN;
    b->fps[0].ctx = SITE1_CTX;
    b->fps[0].patch[0] = 0xb8;
    put32le(b->fps[0].patch + 1, g_target_fps);
    b->fps[0].patch[5] = 0x90;

    b->fps[1].name = "fps_writer2";
    b->fps[1].rva = SITE2_RVA;
    b->fps[1].len = SITE2_LEN;
    b->fps[1].ctx = SITE2_CTX;
    b->fps[1].patch[0] = 0xb8;
    put32le(b->fps[1].patch + 1, g_target_fps);
    b->fps[1].patch[5] = 0x66;
    b->fps[1].patch[6] = 0x90;

    /* fwTimer floor patches (prevents game speed-up above 30 FPS):
     *   0x3f73ecd (6B): mov eax, 3000; nop -> 1/3000s (0x39aec33e)
     *   0x41ec831 (8B): nop*8 -> prevent overwriting 0xabab4a0 later */
    b->timestep.name = "fwtimer_floor";
    b->timestep.rva = TIMESTEP_RVA;
    b->timestep.len = TIMESTEP_LEN;
    b->timestep.ctx = TIMESTEP_CTX;
    b->timestep.patch[0] = 0xb8;
    put32le(b->timestep.patch + 1, 3000u);
    b->timestep.patch[5] = 0x90;

    b->timestep2.name = "fwtimer_floor2";
    b->timestep2.rva = TIMESTEP2_RVA;
    b->timestep2.len = TIMESTEP2_LEN;
    b->timestep2.ctx = TIMESTEP2_CTX;
    memset(b->timestep2.patch, 0x90, TIMESTEP2_LEN);

    /* TAA2U::Update() 18-byte replacement (PS5 Fat/Slim):
     *   b8 <float32>         mov eax, <res_pct / 100.0f>
     *   89 87 90 00 00 00    mov dword ptr [rdi + 0x90], eax
     *   89 87 94 00 00 00    mov dword ptr [rdi + 0x94], eax
     *   c3                   ret */
    b->taa2u.name = "taa2u_scale";
    b->taa2u.rva = TAA2U_RVA;
    b->taa2u.len = TAA2U_LEN;
    b->taa2u.ctx = TAA2U_CTX;
    if (g_res_pct > 0) {
        static const uint8_t store_ret[13] = {
            0x89,0x87,0x90,0x00,0x00,0x00,
            0x89,0x87,0x94,0x00,0x00,0x00,
            0xc3
        };
        float scale_f = g_scale;
        b->taa2u.patch[0] = 0xb8;
        put32le(b->taa2u.patch + 1, float_bits(scale_f));
        memcpy(b->taa2u.patch + 5, store_ret, sizeof(store_ret));
    }

    /* PSSR::GetRenderDimensions() 11-byte override at 0x62e6bc5 (PS5 Pro):
     *   b8 <float32>   mov eax, <res_pct / 100.0f>
     *   c5 f9 6e c0    vmovd xmm0, eax
     *   eb 37          jmp 0x62e6c07 (stock multiply + store + epilogue) */
    b->pssr.name = "pssr_scale";
    b->pssr.rva = PSSR_RVA;
    b->pssr.len = PSSR_LEN;
    b->pssr.ctx = PSSR_CTX;
    if (g_res_pct > 0) {
        static const uint8_t movd_jmp[6] = { 0xc5,0xf9,0x6e,0xc0,0xeb,0x37 };
        float scale_f = g_scale;
        b->pssr.patch[0] = 0xb8;
        put32le(b->pssr.patch + 1, float_bits(scale_f));
        memcpy(b->pssr.patch + 5, movd_jmp, sizeof(movd_jmp));
    }

    /* 120 Hz flip-rate constants (3 sites: 0x3c -> 0x78 [120], 0x3d -> 0x79 [121]): */
    b->hz[0].name = "hz1_div"; b->hz[0].rva = HZ1_A_RVA; b->hz[0].len = 1; b->hz[0].ctx = HZ1_A_CTX; b->hz[0].patch[0] = 0x78;
    b->hz[1].name = "hz1_cmp"; b->hz[1].rva = HZ1_B_RVA; b->hz[1].len = 1; b->hz[1].ctx = HZ1_B_CTX; b->hz[1].patch[0] = 0x79;
    b->hz[2].name = "hz2_div"; b->hz[2].rva = HZ2_A_RVA; b->hz[2].len = 1; b->hz[2].ctx = HZ2_A_CTX; b->hz[2].patch[0] = 0x78;
    b->hz[3].name = "hz2_cmp"; b->hz[3].rva = HZ2_B_RVA; b->hz[3].len = 1; b->hz[3].ctx = HZ2_B_CTX; b->hz[3].patch[0] = 0x79;
    b->hz[4].name = "hz3_div"; b->hz[4].rva = HZ3_A_RVA; b->hz[4].len = 1; b->hz[4].ctx = HZ3_A_CTX; b->hz[4].patch[0] = 0x78;
    b->hz[5].name = "hz3_cmp"; b->hz[5].rva = HZ3_B_RVA; b->hz[5].len = 1; b->hz[5].ctx = HZ3_B_CTX; b->hz[5].patch[0] = 0x79;

    /* Swapchain output width/height overrides in CSettingsManager::ApplySettings: */
    if (g_output_res == 1440 || g_output_res == 1080) {
        uint32_t out_w = (g_output_res == 1440) ? 2560u : 1920u;
        uint32_t out_h = g_output_res;

        b->out[0].name = "out_width";
        b->out[0].rva = OUT_W_RVA;
        b->out[0].len = OUT_LEN;
        b->out[0].ctx = OUT_W_CTX;
        b->out[0].patch[0] = 0xb8; /* mov eax, out_w; nop */
        put32le(b->out[0].patch + 1, out_w);
        b->out[0].patch[5] = 0x90;

        b->out[1].name = "out_height1";
        b->out[1].rva = OUT_H1_RVA;
        b->out[1].len = OUT_LEN;
        b->out[1].ctx = OUT_H1_CTX;
        b->out[1].patch[0] = 0xb8; /* mov eax, out_h; nop */
        put32le(b->out[1].patch + 1, out_h);
        b->out[1].patch[5] = 0x90;

        b->out[2].name = "out_height2";
        b->out[2].rva = OUT_H2_RVA;
        b->out[2].len = OUT_LEN;
        b->out[2].ctx = OUT_H2_CTX;
        b->out[2].patch[0] = 0xb9; /* mov ecx, out_h; nop */
        put32le(b->out[2].patch + 1, out_h);
        b->out[2].patch[5] = 0x90;
    }

    /* PS5 Pro GlobalIlluminationQuality (0xc6c) -> Tier 0 across all 4 readers: */
    b->pro_rt_half[0].name = "pro_rt_half1";
    b->pro_rt_half[0].rva = PRO_RT_HALF_RVA;
    b->pro_rt_half[0].len = PRO_RT_HALF_LEN;
    b->pro_rt_half[0].ctx = PRO_RT_HALF_CTX;
    b->pro_rt_half[0].patch[0] = 0x31; /* xor ecx, ecx; nop */
    b->pro_rt_half[0].patch[1] = 0xc9;
    b->pro_rt_half[0].patch[2] = 0x90;

    b->pro_rt_half[1].name = "pro_rt_half2";
    b->pro_rt_half[1].rva = PRO_RT_HALF2_RVA;
    b->pro_rt_half[1].len = PRO_RT_HALF2_LEN;
    b->pro_rt_half[1].ctx = PRO_RT_HALF2_CTX;
    b->pro_rt_half[1].patch[0] = 0x45; /* xor r15d, r15d; nop */
    b->pro_rt_half[1].patch[1] = 0x31;
    b->pro_rt_half[1].patch[2] = 0xff;
    b->pro_rt_half[1].patch[3] = 0x90;

    b->pro_rt_half[2].name = "pro_rt_half3";
    b->pro_rt_half[2].rva = PRO_RT_HALF3_RVA;
    b->pro_rt_half[2].len = PRO_RT_HALF3_LEN;
    b->pro_rt_half[2].ctx = PRO_RT_HALF3_CTX;
    b->pro_rt_half[2].patch[0] = 0x31; /* xor edi, edi; nop*4 */
    b->pro_rt_half[2].patch[1] = 0xff;
    memset(b->pro_rt_half[2].patch + 2, 0x90, 4);

    b->pro_rt_half[3].name = "pro_rt_half4";
    b->pro_rt_half[3].rva = PRO_RT_HALF4_RVA;
    b->pro_rt_half[3].len = PRO_RT_HALF4_LEN;
    b->pro_rt_half[3].ctx = PRO_RT_HALF4_CTX;
    b->pro_rt_half[3].patch[0] = 0x31; /* xor edi, edi; nop*4 */
    b->pro_rt_half[3].patch[1] = 0xff;
    memset(b->pro_rt_half[3].patch + 2, 0x90, 4);

    /* Close-game CE-108255-1 fix (0x65742c0, 7B: lea rbx, [rbp-0x1ac] -> xor ebx, ebx; jmp 0x6574710): */
    b->close_fix.name = "close_fix";
    b->close_fix.rva = CLOSE_FIX_RVA;
    b->close_fix.len = CLOSE_FIX_LEN;
    b->close_fix.ctx = CLOSE_FIX_CTX;
    b->close_fix.patch[0] = 0x31;
    b->close_fix.patch[1] = 0xdb;
    b->close_fix.patch[2] = 0xe9;
    b->close_fix.patch[3] = 0x49;
    b->close_fix.patch[4] = 0x04;
    b->close_fix.patch[5] = 0x00;
    b->close_fix.patch[6] = 0x00;

    /* PS5 Pro PSSR -> TAA2U switch (0x2afc2c9, 3B: call [rax+0x10] -> nop*3): */
    b->pro_taa2u.name = "pro_taa2u";
    b->pro_taa2u.rva = PRO_TAA2U_RVA;
    b->pro_taa2u.len = PRO_TAA2U_LEN;
    b->pro_taa2u.ctx = PRO_TAA2U_CTX;
    b->pro_taa2u.patch[0] = 0x90;
    b->pro_taa2u.patch[1] = 0x90;
    b->pro_taa2u.patch[2] = 0x90;

    /* Low Shadow Map tier (0x3f74417, 6B: mov eax, [rbx+0xc64] -> mov eax, 1; nop -> 1280.0): */
    b->low_shadows.name = "low_shadows";
    b->low_shadows.rva = LOW_SHADOWS_RVA;
    b->low_shadows.len = LOW_SHADOWS_LEN;
    b->low_shadows.ctx = LOW_SHADOWS_CTX;
    b->low_shadows.patch[0] = 0xb8;
    put32le(b->low_shadows.patch + 1, 1u);
    b->low_shadows.patch[5] = 0x90;

    /* Fast Soft Shadows for low_shadows=2 (0x3f74350, 6B: mov eax, [rax+0x568] -> xor eax, eax; nop*4 -> filter 2): */
    b->low_shadows2.name = "low_shadows2";
    b->low_shadows2.rva = LOW_SHADOWS2_RVA;
    b->low_shadows2.len = LOW_SHADOWS2_LEN;
    b->low_shadows2.ctx = LOW_SHADOWS2_CTX;
    b->low_shadows2.patch[0] = 0x31;
    b->low_shadows2.patch[1] = 0xc0;
    memset(b->low_shadows2.patch + 2, 0x90, 4);

    /* ReflectionQuality map 256 -> 128 (0x3f73f11, 7B: cmp [rbx+0xc88], 0 -> cmp eax, eax; nop*5): */
    b->low_reflections.name = "low_reflections";
    b->low_reflections.rva = LOW_REFLECTIONS_RVA;
    b->low_reflections.len = LOW_REFLECTIONS_LEN;
    b->low_reflections.ctx = LOW_REFLECTIONS_CTX;
    b->low_reflections.patch[0] = 0x39;
    b->low_reflections.patch[1] = 0xc0;
    memset(b->low_reflections.patch + 2, 0x90, 5);

    /* True Volumetrics (VolumetricsRaymarchQuality 0x3f741b9 & VolumetricsLightingQuality 0x3f741c5 -> Tier 0): */
    b->low_vol[0].name = "low_vol_raymarch";
    b->low_vol[0].rva = LOW_VOL1_RVA;
    b->low_vol[0].len = LOW_VOL_LEN;
    b->low_vol[0].ctx = LOW_VOL1_CTX;
    b->low_vol[0].patch[0] = 0x31;
    b->low_vol[0].patch[1] = 0xc0;
    memset(b->low_vol[0].patch + 2, 0x90, 4);

    b->low_vol[1].name = "low_vol_lighting";
    b->low_vol[1].rva = LOW_VOL2_RVA;
    b->low_vol[1].len = LOW_VOL_LEN;
    b->low_vol[1].ctx = LOW_VOL2_CTX;
    b->low_vol[1].patch[0] = 0x31;
    b->low_vol[1].patch[1] = 0xc0;
    memset(b->low_vol[1].patch + 2, 0x90, 4);

    /* LodScale (0x3f73f47, 16B: mov eax, <f32>; vmovd xmm0, eax; mov [0xd796908], eax; nop): */
    b->lod_scale.name = "lod_scale";
    b->lod_scale.rva = LOD_SCALE_RVA;
    b->lod_scale.len = LOD_SCALE_LEN;
    b->lod_scale.ctx = LOD_SCALE_CTX;
    if (g_lod_scale > 0) {
        static const uint8_t tail[11] = {
            0xc5,0xf9,0x6e,0xc0,             /* vmovd xmm0, eax           */
            0x89,0x05,0xb2,0x29,0x82,0x09,   /* mov [rip+0x98229b2], eax  (0xd796908) */
            0x90                             /* nop                       */
        };
        float lod_f = (float)g_lod_scale / 100.0f;
        b->lod_scale.patch[0] = 0xb8;
        put32le(b->lod_scale.patch + 1, float_bits(lod_f));
        memcpy(b->lod_scale.patch + 5, tail, sizeof(tail));
    }

    /* BvhQuality -> Tier 0 (0x3f7444f, 6B: mov eax, [rbx+0xcbc] -> xor eax, eax; nop*4): */
    b->low_bvh.name = "low_bvh";
    b->low_bvh.rva = LOW_BVH_RVA;
    b->low_bvh.len = LOW_BVH_LEN;
    b->low_bvh.ctx = LOW_BVH_CTX;
    b->low_bvh.patch[0] = 0x31;
    b->low_bvh.patch[1] = 0xc0;
    memset(b->low_bvh.patch + 2, 0x90, 4);

    /* MotionBlur off (0x4067792 & 0x64bbf36, 6B each: mov al, [rax+0x528] -> xor al, al; nop*4): */
    b->motion_blur[0].name = "motion_blur1";
    b->motion_blur[0].rva = MOTION_BLUR1_RVA;
    b->motion_blur[0].len = MOTION_BLUR_LEN;
    b->motion_blur[0].ctx = MOTION_BLUR1_CTX;
    b->motion_blur[0].patch[0] = 0x30;
    b->motion_blur[0].patch[1] = 0xc0;
    memset(b->motion_blur[0].patch + 2, 0x90, 4);

    b->motion_blur[1].name = "motion_blur2";
    b->motion_blur[1].rva = MOTION_BLUR2_RVA;
    b->motion_blur[1].len = MOTION_BLUR_LEN;
    b->motion_blur[1].ctx = MOTION_BLUR2_CTX;
    b->motion_blur[1].patch[0] = 0x30;
    b->motion_blur[1].patch[1] = 0xc0;
    memset(b->motion_blur[1].patch + 2, 0x90, 4);
}

/* Raise the transient render heap and shrink the main pool by the same amount.
 * Must run before GTAVI creates the heap (process frozen right after it appears).
 * Returns 1 if the heap is (now) large enough for g_res_w x g_res_h, 0 otherwise. */
static int apply_highres_heap(uint32_t pid, uint64_t base) {
    static const uint8_t heap_stock[9] = { 0x50,0x35,0x00,0x00, 0xbb,0x80,0x2d,0x00,0x00 };
    uint8_t cur[9], q[8], imm[4];
    uint64_t heap_now = 0, extra, pool_new;
    unsigned i;
    int relocate;

    if (!g_heap_pages)
        return 1;
    if (proc_read(pid, base + HEAP_SIZE_G_RVA, q, 8))
        return 0;
    memcpy(&heap_now, q, 8);
    if (heap_now) {
        logmsg("WARN", "render heap already created (%llu pages): too late to enlarge it",
               (unsigned long long)heap_now);
        return heap_now >= g_heap_pages;
    }
    if (proc_read(pid, base + HEAP_PAGES_RVA, cur, 9) || memcmp(cur, heap_stock, 9)) {
        logmsg("WARN", "heap size site does not match the supported build");
        return 0;
    }
    for (i = 0; i < 10; i++) {
        uint64_t v;
        if (proc_read(pid, base + POOL_RVAS[i], q, 8))
            return 0;
        memcpy(&v, q, 8);
        if (v != POOL_STOCK) {
            logmsg("WARN", "main pool site %u does not match the supported build", i);
            return 0;
        }
    }
    relocate = g_heap_pages > HEAP_PAGES_ARRAY;
    if (relocate) {
        static const uint8_t lea1[7] = { 0x4c,0x8d,0x3d,0xcb,0x23,0x5c,0x0a };
        static const uint8_t lea2[7] = { 0x48,0x8d,0x0d,0x66,0x0e,0xea,0x05 };
        uint64_t h2 = 0;
        uint8_t *zero;
        int clean = 0;
        if (proc_read(pid, base + LEA1_RVA, cur, 7) || memcmp(cur, lea1, 7) ||
            proc_read(pid, base + LEA2_RVA, cur, 7) || memcmp(cur, lea2, 7) ||
            proc_read(pid, base + HEAP2_RVA, q, 8)) {
            logmsg("WARN", "page array / heap #2 sites do not match the supported build");
            return 0;
        }
        memcpy(&h2, q, 8);
        if (h2 != HEAP2_STOCK) {
            logmsg("WARN", "heap #2 size is not stock");
            return 0;
        }
        zero = malloc((size_t)g_heap_pages * 8u);
        if (zero && !proc_read(pid, base + NEW_ARRAY_RVA, zero, g_heap_pages * 8u)) {
            clean = 1;
            for (i = 0; i < g_heap_pages * 8u; i++)
                if (zero[i]) { clean = 0; break; }
        }
        free(zero);
        if (!clean) {
            logmsg("WARN", "new page array region is not free");
            return 0;
        }
    }

    extra = (uint64_t)(g_heap_pages - HEAP_PAGES_STOCK) * 0x10000u + (relocate ? HEAP2_ADD : 0u);
    pool_new = POOL_STOCK - ((extra + 0x1fffffu) & ~(uint64_t)0x1fffffu);
    /* Order keeps every partial failure safe: shrink pool first, grow heaps last. */
    for (i = 0; i < 10; i++) {
        memcpy(q, &pool_new, 8);
        if (proc_write(pid, base + POOL_RVAS[i], q, 8) ||
            proc_read(pid, base + POOL_RVAS[i], cur, 8) || memcmp(cur, q, 8)) {
            logmsg("WARN", "main pool write %u failed", i);
            return 0; /* heap left stock: the game still boots at stock size */
        }
    }
    if (relocate) {
        static const uint32_t lea_rva[2] = { LEA1_RVA, LEA2_RVA };
        uint64_t h2 = HEAP2_STOCK + HEAP2_ADD;
        for (i = 0; i < 2; i++) {
            put32le(imm, (uint32_t)((int32_t)NEW_ARRAY_RVA - (int32_t)(lea_rva[i] + 7u)));
            if (proc_write(pid, base + lea_rva[i] + 3u, imm, 4) ||
                proc_read(pid, base + lea_rva[i] + 3u, cur, 4) || memcmp(cur, imm, 4)) {
                logmsg("WARN", "page array lea %u write failed", i);
                return 0;
            }
        }
        memcpy(q, &h2, 8);
        if (proc_write(pid, base + HEAP2_RVA, q, 8) ||
            proc_read(pid, base + HEAP2_RVA, cur, 8) || memcmp(cur, q, 8)) {
            logmsg("WARN", "heap #2 write failed");
            return 0;
        }
        logmsg("OK", "page array moved, heap #2 %llu -> %llu MB",
               (unsigned long long)(HEAP2_STOCK >> 20), (unsigned long long)(h2 >> 20));
    }
    put32le(imm, g_heap_pages);
    if (proc_write(pid, base + HEAP_PAGES_RVA, imm, 4) ||
        proc_read(pid, base + HEAP_PAGES_RVA, cur, 4) || memcmp(cur, imm, 4)) {
        logmsg("WARN", "heap size write failed");
        return 0;
    }
    logmsg("OK", "render heap %u -> %u MB, main pool %llu -> %llu MB",
           HEAP_PAGES_STOCK / 16u, g_heap_pages / 16u,
           (unsigned long long)(POOL_STOCK >> 20), (unsigned long long)(pool_new >> 20));
    return 1;
}

/* Experimental Base PS5 dynamic heap expansion (base_highres=1):
 *   res_pct <= 75: transient 728 -> 896 MB (+168 MB), heap #2 2718 -> 2974 MB (+256 MB)
 *   res_pct >  75: transient 728 -> 1024 MB (+296 MB), heap #2 2718 -> 3230 MB (+512 MB)
 * Both fit the stock 0x4000 page pointer array without relocation, shrinking the shared
 * main pool by the exact same total. */
static int apply_base_highres_heap(uint32_t pid, uint64_t base) {
    static const uint8_t heap_stock[9] = { 0x50,0x35,0x00,0x00, 0xbb,0x80,0x2d,0x00,0x00 };
    uint8_t cur[9], q[8], imm[4];
    uint64_t heap_now = 0, h2 = 0, extra, pool_new;
    uint32_t target_pages = (g_res_pct > 75u) ? BASE_HEAP_PAGES_HI : BASE_HEAP_PAGES_MED;
    uint64_t heap2_add = (g_res_pct > 75u) ? BASE_HEAP2_ADD_HI : BASE_HEAP2_ADD_MED;
    unsigned i;

    if (!g_base_highres)
        return 1;
    if (proc_read(pid, base + HEAP_SIZE_G_RVA, q, 8))
        return 0;
    memcpy(&heap_now, q, 8);
    if (heap_now) {
        logmsg("WARN", "base_highres: render heap already created (%llu pages): too late",
               (unsigned long long)heap_now);
        return heap_now >= target_pages;
    }
    if (proc_read(pid, base + HEAP_PAGES_RVA, cur, 9) || memcmp(cur, heap_stock, 9)) {
        logmsg("WARN", "base_highres: heap size site does not match the supported build");
        return 0;
    }
    if (proc_read(pid, base + BASE_HEAP2_RVA, q, 8))
        return 0;
    memcpy(&h2, q, 8);
    if (h2 != BASE_HEAP2_STOCK) {
        logmsg("WARN", "base_highres: base heap #2 size is not stock");
        return 0;
    }
    for (i = 0; i < 10; i++) {
        uint64_t v;
        if (proc_read(pid, base + POOL_RVAS[i], q, 8))
            return 0;
        memcpy(&v, q, 8);
        if (v != POOL_STOCK) {
            logmsg("WARN", "base_highres: main pool site %u does not match the supported build", i);
            return 0;
        }
    }

    extra = (uint64_t)(target_pages - BASE_HEAP_PAGES_STOCK) * 0x10000u + heap2_add;
    pool_new = POOL_STOCK - ((extra + 0x1fffffu) & ~(uint64_t)0x1fffffu);
    /* Shrink pool first, grow heaps last so any partial failure stays safe. */
    for (i = 0; i < 10; i++) {
        memcpy(q, &pool_new, 8);
        if (proc_write(pid, base + POOL_RVAS[i], q, 8) ||
            proc_read(pid, base + POOL_RVAS[i], cur, 8) || memcmp(cur, q, 8)) {
            logmsg("WARN", "base_highres: main pool write %u failed", i);
            return 0;
        }
    }
    h2 = BASE_HEAP2_STOCK + heap2_add;
    memcpy(q, &h2, 8);
    if (proc_write(pid, base + BASE_HEAP2_RVA, q, 8) ||
        proc_read(pid, base + BASE_HEAP2_RVA, cur, 8) || memcmp(cur, q, 8)) {
        logmsg("WARN", "base_highres: base heap #2 write failed");
        return 0;
    }
    put32le(imm, target_pages);
    if (proc_write(pid, base + BASE_HEAP_PAGES_RVA, imm, 4) ||
        proc_read(pid, base + BASE_HEAP_PAGES_RVA, cur, 4) || memcmp(cur, imm, 4)) {
        logmsg("WARN", "base_highres: base transient heap write failed");
        return 0;
    }
    logmsg("OK", "base_highres: transient %u -> %u MB, heap #2 %llu -> %llu MB, main pool %llu -> %llu MB",
           BASE_HEAP_PAGES_STOCK / 16u, target_pages / 16u,
           (unsigned long long)(BASE_HEAP2_STOCK >> 20), (unsigned long long)(h2 >> 20),
           (unsigned long long)(POOL_STOCK >> 20), (unsigned long long)(pool_new >> 20));
    return 1;
}

static int patch_gtavi_session(uint32_t pid, const char *titleid, int already_open,
                               struct patch_bundle *b) {
    uint64_t base = 0;
    int already1 = 0, already2 = 0, already_ts = 0, already_ts2 = 0;
    int taa2u_applied = 0, taa2u_alr = 0;
    int hz_alr = 0, out_alr = 0, rt_half_alr = 0;
    int close_alr = 0, pro_taa2u_alr = 0, low_shad_alr = 0, low_shad2_alr = 0;
    int low_refl_alr = 0, low_vol_alr = 0, lod_alr = 0, low_bvh_alr = 0, mb_alr = 0;
    unsigned long found_ms = elapsed_ms();
    char flags[160];
    int match;

    logmsg("OK", "found %s pid %u%s, verifying eboot...", titleid, pid,
           already_open ? " (already open)" : "");

    match = verify_gtavi_process(pid, b->fps, &base);
    if (match == 0) {
        notify("%s eboot bytes do not match supported build. Nothing changed.",
               titleid);
        return EXIT_NO_TARGET;
    }
    if (match < 0 || !base) {
        notify("couldn't read %s memory. Nothing changed.", titleid);
        return EXIT_NO_TARGET;
    }
    logmsg("OK", "GTAVI verified: %s pid %u base 0x%llx", titleid, pid,
           (unsigned long long)base);

    if (g_heap_pages && !apply_highres_heap(pid, base)) {
        /* Heap could not be enlarged: fall back to the largest even size below stock. */
        g_res_pct = 66u;
        g_pro_highres = 0u;
        build_patches(b);
        notify("high-res heap not applied; using %ux%u. Start GTAVI after this payload.",
               g_res_w, g_res_h);
    } else if (g_base_highres) {
        if (g_heap_pages) {
            logmsg("WARN", "base_highres ignored because pro_highres heap expansion is active");
        } else if (!apply_base_highres_heap(pid, base)) {
            logmsg("WARN", "base_highres heap expansion not applied; continuing with stock heap");
        }
    }
    if (g_res_pct > 0)
        logmsg("INFO", "internal resolution %ux%u (scale %.5f)", g_res_w, g_res_h,
               (double)g_scale);

    /* Patch output resolution, upscaler, TAA2U/PSSR scale, and quality tweaks first
     * so they beat swapchain and render target allocation. */
    if (g_output_res == 1440 || g_output_res == 1080) {
        apply_site_group(pid, base, b->out, 3, "output_res", &out_alr);
    }

    if (g_pro_taa2u) {
        if (verify_site(pid, base, &b->pro_taa2u) == 1) {
            if (apply_site(pid, base, &b->pro_taa2u, &pro_taa2u_alr))
                logmsg("WARN", "pro_taa2u write failed");
        } else {
            logmsg("WARN", "pro_taa2u context mismatch");
        }
    }

    if (g_res_pct > 0) {
        int pssr_alr = 0;
        if (verify_site(pid, base, &b->taa2u) == 1) {
            if (!apply_site(pid, base, &b->taa2u, &taa2u_alr))
                taa2u_applied = 1;
            else
                logmsg("WARN", "TAA2U scale write failed; continuing with FPS unlock");
        } else {
            logmsg("WARN", "TAA2U context mismatch; continuing with FPS unlock");
        }
        if (verify_site(pid, base, &b->pssr) == 1) {
            if (apply_site(pid, base, &b->pssr, &pssr_alr))
                logmsg("WARN", "PSSR scale write failed; continuing");
        } else {
            logmsg("WARN", "PSSR context mismatch");
        }
    }

    if (g_pro_rt_half) {
        apply_site_group(pid, base, b->pro_rt_half, 4, "pro_rt_half", &rt_half_alr);
    }

    if (g_low_shadows >= 1) {
        if (verify_site(pid, base, &b->low_shadows) == 1) {
            if (apply_site(pid, base, &b->low_shadows, &low_shad_alr))
                logmsg("WARN", "low_shadows write failed");
        } else {
            logmsg("WARN", "low_shadows context mismatch");
        }
    }

    if (g_low_shadows >= 2) {
        if (verify_site(pid, base, &b->low_shadows2) == 1) {
            if (apply_site(pid, base, &b->low_shadows2, &low_shad2_alr))
                logmsg("WARN", "low_shadows2 write failed");
        } else {
            logmsg("WARN", "low_shadows2 context mismatch");
        }
    }

    if (g_low_reflections) {
        if (verify_site(pid, base, &b->low_reflections) == 1) {
            if (apply_site(pid, base, &b->low_reflections, &low_refl_alr))
                logmsg("WARN", "low_reflections write failed");
        } else {
            logmsg("WARN", "low_reflections context mismatch");
        }
    }

    if (g_low_volumetric) {
        apply_site_group(pid, base, b->low_vol, 2, "low_volumetric", &low_vol_alr);
    }

    if (g_lod_scale > 0) {
        if (verify_site(pid, base, &b->lod_scale) == 1) {
            if (apply_site(pid, base, &b->lod_scale, &lod_alr))
                logmsg("WARN", "lod_scale write failed");
        } else {
            logmsg("WARN", "lod_scale context mismatch");
        }
    }

    if (g_low_bvh) {
        if (verify_site(pid, base, &b->low_bvh) == 1) {
            if (apply_site(pid, base, &b->low_bvh, &low_bvh_alr))
                logmsg("WARN", "low_bvh write failed");
        } else {
            logmsg("WARN", "low_bvh context mismatch");
        }
    }

    if (!g_motion_blur) {
        apply_site_group(pid, base, b->motion_blur, 2, "motion_blur", &mb_alr);
    }

    if (apply_site(pid, base, &b->fps[0], &already1) ||
        apply_site(pid, base, &b->fps[1], &already2)) {
        notify("write failed. Restart GTAVI and check %s", LOG_PATH);
        return EXIT_WRITE_FAIL;
    }

    if (verify_site(pid, base, &b->timestep) == 1) {
        if (apply_site(pid, base, &b->timestep, &already_ts))
            logmsg("WARN", "fwTimer floor code patch failed; relying on RAM write");
    } else {
        logmsg("WARN", "fwTimer floor context mismatch");
    }

    if (verify_site(pid, base, &b->timestep2) == 1) {
        if (apply_site(pid, base, &b->timestep2, &already_ts2))
            logmsg("WARN", "fwTimer floor2 patch failed");
    } else {
        logmsg("WARN", "fwTimer floor2 context mismatch");
    }

    if (g_refresh_hz == 120) {
        apply_site_group(pid, base, b->hz, 6, "refresh_hz_120", &hz_alr);
    }

    if (g_close_fix) {
        if (verify_site(pid, base, &b->close_fix) == 1) {
            if (apply_site(pid, base, &b->close_fix, &close_alr))
                logmsg("WARN", "close_fix write failed");
        } else {
            logmsg("WARN", "close_fix context mismatch");
        }
    }

    update_live_globals(pid, base);
    logmsg("OK", "all patches applied in %lu ms", elapsed_ms() - found_ms);

    format_opt_flags(flags, sizeof(flags));
    if (already1 && already2 && (!g_res_pct || taa2u_alr) &&
        (g_refresh_hz != 120 || hz_alr) && (!g_output_res || out_alr) &&
        (!g_pro_rt_half || rt_half_alr) && (!g_close_fix || close_alr) &&
        (!g_pro_taa2u || pro_taa2u_alr) && (!g_low_shadows || low_shad_alr) &&
        (g_low_shadows < 2 || low_shad2_alr) && (!g_low_reflections || low_refl_alr) &&
        (!g_low_volumetric || low_vol_alr) && (!g_lod_scale || lod_alr) &&
        (!g_low_bvh || low_bvh_alr) && (g_motion_blur || mb_alr)) {
        notify("already active (Res %u%%%s).", g_res_pct, flags);
    } else if (taa2u_applied) {
        notify("FPS Unlocked, Res %u%%%s", g_res_pct, flags);
    } else {
        notify("FPS Unlocked%s", flags);
    }

    return EXIT_OK;
}

static int need_early_freeze(void) {
    return g_heap_pages != 0 || g_base_highres != 0 || g_output_res != 0 ||
           g_pro_rt_half != 0 || g_pro_taa2u != 0 || g_low_shadows != 0 ||
           g_low_reflections != 0 || g_low_volumetric != 0 || g_lod_scale != 0 ||
           g_low_bvh != 0 || g_motion_blur == 0;
}

int main(void) {
    struct patch_bundle patches;
    struct eboot_proc procs[MAX_EBOOTS];
    uint32_t skipped[MAX_EBOOTS];
    unsigned skipped_n = 0, proc_n = 0, i;
    uint32_t patched_pid = 0;
    unsigned poll = 0, fails = 0;
    int first_session = 1;
    char flags[160];

    clock_gettime(CLOCK_MONOTONIC, &g_t0);
#ifndef HOST_TEST
    syscall(SYS_thr_set_name, -1, PROC_NAME);
#endif

    if (lock_instance()) {
        notify("already running, this copy exits");
        return EXIT_ALREADY_RUNNING;
    }
    log_open();
    load_settings();
    logmsg("INFO", "gtavi-unlockfps v%s started (fps=%u, res_pct=%u%%, hz=%u, out=%u, mb=%u, pro_hr=%u, pro_rt=%u, pro_taa=%u, shad=%u, refl=%u, vol=%u, lod=%u, bvh=%u, base_hr=%u, close=%u, daemon=%u)",
           TOOL_VERSION, g_target_fps, g_res_pct, g_refresh_hz, g_output_res, g_motion_blur,
           g_pro_highres, g_pro_rt_half, g_pro_taa2u, g_low_shadows, g_low_reflections,
           g_low_volumetric, g_lod_scale, g_low_bvh, g_base_highres, g_close_fix, g_daemon);
#ifndef HOST_TEST
    logmsg("INFO", "console firmware %x.%02x", kernel_get_fw_version() >> 24,
           (kernel_get_fw_version() >> 16) & 0xff);
#endif

    build_patches(&patches);
    format_opt_flags(flags, sizeof(flags));

    if (g_daemon) {
        if (g_res_pct > 0)
            notify("ready (Res %u%%%s, daemon). Waiting for GTAVI.", g_res_pct, flags);
        else
            notify("ready (Res stock%s, daemon). Waiting for GTAVI.", flags);
    } else {
        if (g_res_pct > 0)
            notify("ready (Res %u%%%s). Start GTAVI within 10 min.", g_res_pct, flags);
        else
            notify("ready (Res stock%s). Start GTAVI within 10 min.", flags);
    }

    for (poll = 0; g_daemon || (need_early_freeze() ? elapsed_ms() < WAIT_POLLS * (POLL_USEC / 1000u)
                                                    : poll < WAIT_POLLS); poll++) {
        uint32_t pid = 0;
        char titleid[16] = "?";

        if (list_eboot_procs(procs, &proc_n)) {
            if (!g_daemon && ++fails > LIST_TRIES) {
                notify("couldn't list running apps. Nothing changed.");
                return finish(EXIT_NO_PROC_LIST);
            }
            sleep(1);
            continue;
        }
        fails = 0;

        /* In daemon mode, sleep gently while the already-patched GTAVI session is alive */
        if (patched_pid) {
            int alive = 0;
            for (i = 0; i < proc_n; i++) {
                if (procs[i].pid == patched_pid) {
                    alive = 1;
                    break;
                }
            }
            if (alive) {
                usleep(IDLE_POLL_USEC);
                continue;
            }
            logmsg("INFO", "GTAVI pid %u exited, waiting for next launch", patched_pid);
            patched_pid = 0;
            skipped_n = 0;
            poll = 0;
        }

        for (i = 0; i < proc_n; i++) {
            if (is_skipped_pid(skipped, skipped_n, procs[i].pid))
                continue;
            if (!is_wanted_title(procs[i].titleid)) {
                logmsg("INFO", "skipping %s (pid %u)", procs[i].titleid,
                       procs[i].pid);
                add_skipped_pid(skipped, &skipped_n, procs[i].pid);
                continue;
            }
            pid = procs[i].pid;
            strcpy(titleid, procs[i].titleid);
            break;
        }

        if (pid) {
            int rc;
            int freeze_req;
            int frozen = 0;
            if (!first_session) {
                load_settings();
                build_patches(&patches);
                logmsg("INFO", "reloaded settings for new launch (fps=%u, res_pct=%u%%, hz=%u, out=%u, mb=%u, pro_hr=%u, pro_rt=%u, pro_taa=%u, shad=%u, refl=%u, vol=%u, lod=%u, bvh=%u, base_hr=%u, close=%u, daemon=%u)",
                       g_target_fps, g_res_pct, g_refresh_hz, g_output_res, g_motion_blur,
                       g_pro_highres, g_pro_rt_half, g_pro_taa2u, g_low_shadows, g_low_reflections,
                       g_low_volumetric, g_lod_scale, g_low_bvh, g_base_highres, g_close_fix, g_daemon);
            }
            /* Freeze GTAVI before boot-time heap/settings init; always resumed below. */
            freeze_req = need_early_freeze();
#ifndef HOST_TEST
            if (freeze_req && !kill((pid_t)pid, SIGSTOP))
                frozen = 1;
#endif
            if (freeze_req)
                logmsg(frozen ? "OK" : "WARN", "GTAVI pid %u %s", pid,
                       frozen ? "frozen for early boot patches" : "could not be frozen");
            rc = patch_gtavi_session(pid, titleid, first_session && poll == 0, &patches);
#ifndef HOST_TEST
            if (frozen && kill((pid_t)pid, SIGCONT))
                logmsg("WARN", "SIGCONT to pid %u failed", pid);
#endif
            first_session = 0;
            if (!g_daemon)
                return finish(rc);
            patched_pid = pid;
            usleep(IDLE_POLL_USEC);
            continue;
        }

        if (!g_daemon && !need_early_freeze() && poll && poll % 300u == 0)
            logmsg("INFO", "still waiting for GTAVI (%u s)", poll / 10u);
        usleep(need_early_freeze() ? FAST_POLL_USEC : POLL_USEC);
    }

    notify("GTAVI was not started within 10 min. Closed.");
    return finish(EXIT_NO_GAME);
}
