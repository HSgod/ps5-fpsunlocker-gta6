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

#define TOOL_VERSION "1.13"

/* Process name shown by payload managers (PLDMGR lists names ending in .elf;
 * kernel keeps at most 19 chars). */
#define PROC_NAME "gtavi_fps.elf"

/* ---------------------------------------------------------------- config */

static const char *const WANT_TITLE_IDS[] = {
    "PPSA01547", "PPSA29660", "PPSA38500"
};

#define DEFAULT_FPS     60u
#define MIN_FPS         30u
#define MAX_FPS         120u

/* Internal TAA2U render resolution percentage of output (stock PS5 = 67% / 1440p).
 * 0 = stock (untouched), 25..100 = custom percent (default 50 = 1080p on 4K; <50 experimental). */
#define DEFAULT_RES_PCT 50u
#define MIN_RES_PCT     25u
#define MAX_RES_PCT     100u

/* Runtime RT Reflections toggle (sr_tracing + sr_denoising):
 * 1 = stock ON (default), 0 = OFF via runtime flag 0xb3f3e90 (experimental, may glitch water). */
#define DEFAULT_RT_REFL 1u

/* Background daemon mode:
 * 0 = one-shot (wait up to 10 min, patch once, exit), 1 = stay in background across restarts. */
#define DEFAULT_DAEMON  0u

/* RVAs relative to eboot.bin map base (vaddr 0x0 in ELF PHDR 0). */
#define SITE1_RVA     0x3f73e51u /* mov eax, dword ptr [rbx + 0xe1c]  (6 bytes)  */
#define SITE1_LEN     6u
#define SITE2_RVA     0x40d2be1u /* mov eax, dword ptr [r14 + 0x204]  (7 bytes)  */
#define SITE2_LEN     7u
#define TIMESTEP_RVA  0x3f73ecdu /* mov eax, dword ptr [rax + 0x614]  (6 bytes)  */
#define TIMESTEP_LEN  6u
#define TIMESTEP2_RVA 0x41ec831u /* vmovss [0xabab4a0], xmm0          (8 bytes)  */
#define TIMESTEP2_LEN 8u
#define RT_REFL_RVA   0x149dd26u /* mov al, byte ptr [rax + 0x753]    (6 bytes)  */
#define RT_REFL_LEN   6u

/* TAA2U::Update() (vtable+0x50 of base PS5 upscaler 0xb3d2db0):
 *   vbroadcastss xmm0, [0x9c7b9ec]  (0.6666667f)
 *   vmovlps      qword ptr [rdi+0x90], xmm0
 *   ret                                          (18 bytes total) */
#define TAA2U_RVA 0x62eadc0u
#define TAA2U_LEN 18u

/* Live globals polled by SetFlipRate & fwTimer: */
#define G_TARGET_FRAMETIME_US_RVA 0xe3be220u /* uint32_t: 1000000 / fps */
#define G_REQUESTED_FRAMELOCK_RVA 0xe7bbb08u /* uint8_t:  0xff = auto   */
#define G_MIN_FRAME_SEC_RVA       0xabab49cu /* float: fwTimer min dt   */
#define G_TARGET_FRAME_SEC_RVA    0xabab4a0u /* float: fwTimer floor dt */

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

/* 64 bytes of the clean GTAVI eboot around RT Reflections runtime flag writer
 * (0x149dd26: mov al, [rax+0x753] -> [0xb3f3e90], site at +32, len 6): */
static const uint8_t RT_REFL_CTX[64] = {
    0x83,0x3d,0xab,0xe2,0xa6,0x0a,0x01,0x89,0x0d,0x45,0x21,0xf5,0x09,0x89,0x0d,0xc7,
    0x21,0xf5,0x09,0x0f,0x84,0x80,0xdf,0xff,0xff,0x48,0x8b,0x05,0xd2,0x70,0xa6,0x0a,
    0x8a,0x80,0x53,0x07,0x00,0x00,0x88,0x05,0x5e,0x61,0xf5,0x09,0x84,0xc0,0x0f,0x84,
    0x6c,0xdf,0xff,0xff,0x80,0x3d,0xc7,0x20,0xf5,0x09,0x00,0x75,0x0d,0xf6,0x05,0xca
};

#ifndef LOG_DIR
#define LOG_DIR "/data/gtavi-unlockfps"
#endif
#define LOG_PATH  LOG_DIR "/gtavi-unlockfps.log"
#define CONF_PATH LOG_DIR "/fps.txt"

#define LOCK_PORT 47961 /* loopback UDP port held as a single-instance lock */

#define POLL_USEC      100000u /* 100 ms: fast enough to beat render init, gentle on shell */
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
static uint32_t g_rt_refl = DEFAULT_RT_REFL;
static uint32_t g_daemon = DEFAULT_DAEMON;

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
    int saw_rt_refl = 0;
    int saw_daemon = 0;
    FILE *f;

    g_target_fps = DEFAULT_FPS;
    g_res_pct = DEFAULT_RES_PCT;
    g_rt_refl = DEFAULT_RT_REFL;
    g_daemon = DEFAULT_DAEMON;

    f = fopen(CONF_PATH, "r");
    if (!f) {
        f = fopen(CONF_PATH, "w");
        if (f) {
            fprintf(f, "# GTAVI Unlock FPS runtime patcher settings\n"
                       "# fps: target FPS cap (30..120). Default: 60.\n"
                       "# res_pct: TAA2U internal render resolution %% of 4K output (stock PS5 = 67%% / 1440p):\n"
                       "#   0 = stock, 50 = 1080p (default), 25..49 = experimental (33 = ~720p)\n"
                       "# rt_reflections: 1 = stock ON (default), 0 = OFF (experimental, may glitch ocean water)\n"
                       "# daemon: 0 = one-shot (default), 1 = stay in background and re-patch on game restarts\n"
                       "fps=%u\n"
                       "res_pct=%u\n"
                       "rt_reflections=%u\n"
                       "daemon=%u\n", DEFAULT_FPS, DEFAULT_RES_PCT, DEFAULT_RT_REFL, DEFAULT_DAEMON);
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
        } else if (!strncmp(line, "rt_reflections", 14)) {
            saw_rt_refl = 1;
            if (v == 0 || v == 1) {
                g_rt_refl = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring rt_reflections=%d (must be 0 or 1)", v);
            }
        } else if (!strncmp(line, "daemon", 6)) {
            saw_daemon = 1;
            if (v == 0 || v == 1) {
                g_daemon = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring daemon=%d (must be 0 or 1)", v);
            }
        }
    }
    fclose(f);

    if (!saw_res_pct || !saw_rt_refl || !saw_daemon) {
        f = fopen(CONF_PATH, "a");
        if (f) {
            if (!saw_res_pct) {
                fprintf(f, "# res_pct: TAA2U internal resolution %% (0=stock, 50=1080p, 25..49=experimental)\n"
                           "res_pct=%u\n", DEFAULT_RES_PCT);
                logmsg("INFO", "appended default res_pct=%u to %s", DEFAULT_RES_PCT, CONF_PATH);
            }
            if (!saw_rt_refl) {
                fprintf(f, "# rt_reflections: 1 = stock ON (default), 0 = OFF (experimental, may glitch ocean water)\n"
                           "rt_reflections=%u\n", DEFAULT_RT_REFL);
                logmsg("INFO", "appended default rt_reflections=%u to %s", DEFAULT_RT_REFL, CONF_PATH);
            }
            if (!saw_daemon) {
                fprintf(f, "# daemon: 0 = one-shot (default), 1 = stay in background and re-patch on game restarts\n"
                           "daemon=%u\n", DEFAULT_DAEMON);
                logmsg("INFO", "appended default daemon=%u to %s", DEFAULT_DAEMON, CONF_PATH);
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
    struct patch_site rt_refl;
};

static void build_patches(struct patch_bundle *b) {
    memset(b, 0, sizeof(*b));

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

    /* TAA2U::Update() 18-byte replacement:
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
        float scale_f = (float)g_res_pct / 100.0f;
        b->taa2u.patch[0] = 0xb8;
        put32le(b->taa2u.patch + 1, float_bits(scale_f));
        memcpy(b->taa2u.patch + 5, store_ret, sizeof(store_ret));
    }

    /* RT Reflections runtime disable patch (0x149dd26, 6B):
     *   31 c0 90 90 90 90   xor eax, eax; nop*4
     * Keeps 0xb3f3dd4 = 1 (all render target buffers allocated, no NULL crash)
     * while zeroing per-frame runtime flag 0xb3f3e90 to skip sr_tracing/denoising. */
    b->rt_refl.name = "rt_reflections_off";
    b->rt_refl.rva = RT_REFL_RVA;
    b->rt_refl.len = RT_REFL_LEN;
    b->rt_refl.ctx = RT_REFL_CTX;
    b->rt_refl.patch[0] = 0x31;
    b->rt_refl.patch[1] = 0xc0;
    b->rt_refl.patch[2] = 0x90;
    b->rt_refl.patch[3] = 0x90;
    b->rt_refl.patch[4] = 0x90;
    b->rt_refl.patch[5] = 0x90;
}

static int patch_gtavi_session(uint32_t pid, const char *titleid, int already_open,
                               const struct patch_bundle *b) {
    uint64_t base = 0;
    int already1 = 0, already2 = 0, already_ts = 0, already_ts2 = 0;
    int taa2u_applied = 0, taa2u_alr = 0;
    int rt_refl_applied = 0, rt_refl_alr = 0;
    unsigned long found_ms = elapsed_ms();
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

    /* Patch TAA2U::Update() first so it beats render target creation */
    if (g_res_pct > 0) {
        if (verify_site(pid, base, &b->taa2u) == 1) {
            if (!apply_site(pid, base, &b->taa2u, &taa2u_alr))
                taa2u_applied = 1;
            else
                logmsg("WARN", "TAA2U scale write failed; continuing with FPS unlock");
        } else {
            logmsg("WARN", "TAA2U context mismatch; continuing with FPS unlock");
        }
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

    if (!g_rt_refl) {
        if (verify_site(pid, base, &b->rt_refl) == 1) {
            if (!apply_site(pid, base, &b->rt_refl, &rt_refl_alr))
                rt_refl_applied = 1;
            else
                logmsg("WARN", "RT Reflections disable write failed");
        } else {
            logmsg("WARN", "RT Reflections context mismatch");
        }
    }

    update_live_globals(pid, base);
    logmsg("OK", "all patches applied in %lu ms", elapsed_ms() - found_ms);

    if (already1 && already2 && (!g_res_pct || taa2u_alr) &&
        (g_rt_refl || rt_refl_alr)) {
        notify("already active (Res %u%%%s).",
               g_res_pct, rt_refl_alr ? ", RT Refl OFF" : "");
    } else if (taa2u_applied) {
        notify("FPS Unlocked, Res %u%%%s",
               g_res_pct, rt_refl_applied ? ", RT Refl OFF" : "");
    } else {
        notify("FPS Unlocked%s",
               rt_refl_applied ? ", RT Refl OFF" : "");
    }

    return EXIT_OK;
}

int main(void) {
    struct patch_bundle patches;
    struct eboot_proc procs[MAX_EBOOTS];
    uint32_t skipped[MAX_EBOOTS];
    unsigned skipped_n = 0, proc_n = 0, i;
    uint32_t patched_pid = 0;
    unsigned poll = 0, fails = 0;
    int first_session = 1;

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
    logmsg("INFO", "gtavi-unlockfps v%s started (fps=%u, res_pct=%u%%, rt_reflections=%u, daemon=%u)",
           TOOL_VERSION, g_target_fps, g_res_pct, g_rt_refl, g_daemon);
#ifndef HOST_TEST
    logmsg("INFO", "console firmware %x.%02x", kernel_get_fw_version() >> 24,
           (kernel_get_fw_version() >> 16) & 0xff);
#endif

    build_patches(&patches);

    if (g_daemon) {
        if (g_res_pct > 0)
            notify("ready (Res %u%%%s, daemon). Waiting for GTAVI.",
                   g_res_pct, g_rt_refl ? "" : ", RT Refl OFF");
        else
            notify("ready (Res stock%s, daemon). Waiting for GTAVI.",
                   g_rt_refl ? "" : ", RT Refl OFF");
    } else {
        if (g_res_pct > 0)
            notify("ready (Res %u%%%s). Start GTAVI within 10 min.",
                   g_res_pct, g_rt_refl ? "" : ", RT Refl OFF");
        else
            notify("ready (Res stock%s). Start GTAVI within 10 min.",
                   g_rt_refl ? "" : ", RT Refl OFF");
    }

    for (poll = 0; g_daemon || poll < WAIT_POLLS; poll++) {
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
            if (!first_session) {
                load_settings();
                build_patches(&patches);
                logmsg("INFO", "reloaded settings for new launch (fps=%u, res_pct=%u%%, rt_reflections=%u, daemon=%u)",
                       g_target_fps, g_res_pct, g_rt_refl, g_daemon);
            }
            rc = patch_gtavi_session(pid, titleid, first_session && poll == 0, &patches);
            first_session = 0;
            if (!g_daemon)
                return finish(rc);
            patched_pid = pid;
            usleep(IDLE_POLL_USEC);
            continue;
        }

        if (!g_daemon && poll && poll % 300u == 0)
            logmsg("INFO", "still waiting for GTAVI (%u s)", poll / 10u);
        usleep(POLL_USEC);
    }

    notify("GTAVI was not started within 10 min. Closed.");
    return finish(EXIT_NO_GAME);
}
