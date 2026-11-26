/* gta6-60fps.c -- one-shot runtime FPS patch for GTA 6 on a jailbroken PS5.
 *
 * Based on the architecture of armanate/rdr2-60fps-runtime-patcher (MIT).
 *
 * What this payload does:
 *   - loads target FPS from /data/gta6-60fps/fps.txt (default 60; creates the
 *     file if missing so it can be edited over FTP, e.g. fps=40 or fps=60),
 *   - waits up to 10 minutes for GTA 6 (eboot.bin) to start,
 *   - resolves eboot.bin's ASLR base address and verifies 64 bytes of context
 *     around BOTH target FPS writer sites before touching memory,
 *   - patches both sites to load the target FPS instead of reading the preset's
 *     30 FPS cap (using DMAP page-table write on FW >= 8.40, or mdbg_copyin),
 *   - also updates live g_TargetFrameTimeUs and g_RequestedFrameLock in RAM so
 *     the change takes effect even if the game was already running,
 *   - verifies readback, shows PS5 notifications, writes a log, and exits.
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

#define TOOL_VERSION "1.1"

/* Process name shown by payload managers (PLDMGR lists names ending in .elf;
 * kernel keeps at most 19 chars). */
#define PROC_NAME "gta660fps.elf"

/* ---------------------------------------------------------------- config */

#define DEFAULT_FPS 60u
#define MIN_FPS     30u
#define MAX_FPS     120u

/* RVAs relative to eboot.bin map base (vaddr 0x0 in ELF PHDR 0). */
#define SITE1_RVA 0x3f73e51u /* mov eax, dword ptr [rbx + 0xe1c]  (6 bytes) */
#define SITE1_LEN 6u
#define SITE2_RVA 0x40d2be1u /* mov eax, dword ptr [r14 + 0x204]  (7 bytes) */
#define SITE2_LEN 7u

/* Live globals updated by Site 1 & Site 2 and polled by SetFlipRate: */
#define G_TARGET_FRAMETIME_US_RVA 0xe3be220u /* uint32_t: 1000000 / fps */
#define G_REQUESTED_FRAMELOCK_RVA 0xe7bbb08u /* uint8_t:  0xff = auto   */

#define CTX_OFF 32u

/* 64 bytes of the clean GTA 6 eboot around Site 1 (site at +32): */
static const uint8_t SITE1_CTX[64] = {
    0x00,0xc5,0xfa,0x10,0x0d,0x32,0x89,0xd0,0x05,0xba,0xe8,0x03,0x00,0x00,0xb9,0xe8,
    0x03,0x00,0x00,0x4c,0x8d,0x3d,0x65,0x78,0xdb,0xfd,0x89,0x05,0xe7,0x67,0xfa,0x07,
    0x8b,0x83,0x1c,0x0e,0x00,0x00,0xc6,0x05,0xaa,0x7c,0x84,0x0a,0xff,0x3d,0xe8,0x03,
    0x00,0x00,0x0f,0x42,0xd0,0x85,0xc0,0x0f,0x44,0xd1,0x48,0x8b,0x0d,0x9e,0xb6,0x7f
};

/* 64 bytes of the clean GTA 6 eboot around Site 2 (site at +32): */
static const uint8_t SITE2_CTX[64] = {
    0x00,0x0f,0x82,0x96,0x00,0x00,0x00,0xc6,0x05,0x39,0x8f,0x6e,0x0a,0xff,0xc5,0xfa,
    0x10,0x0d,0x95,0x9b,0xba,0x05,0xba,0xe8,0x03,0x00,0x00,0xb9,0xe8,0x03,0x00,0x00,
    0x41,0x8b,0x86,0x04,0x02,0x00,0x00,0x3d,0xe8,0x03,0x00,0x00,0x0f,0x42,0xd0,0x85,
    0xc0,0x0f,0x44,0xd1,0xc5,0xea,0x2a,0xc2,0xc5,0xf2,0x5e,0xc0,0xc4,0xe1,0xfa,0x2c
};

#ifndef LOG_DIR
#define LOG_DIR "/data/gta6-60fps"
#endif
#define LOG_PATH  LOG_DIR "/gta6-60fps.log"
#define CONF_PATH LOG_DIR "/fps.txt"

#define LOCK_PORT 47961 /* loopback UDP port held as a single-instance lock */

#define POLL_USEC     20000u  /* 20 ms */
#define WAIT_POLLS    30000u  /* 30000 * 20 ms = 10 min */
#define LIST_TRIES    5u
#define RESOLVE_TRIES 25u     /* 25 * 200 ms = 5 s for eboot mapping */
#define RESOLVE_USEC  200000u
#define WRITE_TRIES   3u
#define WRITE_USEC    200000u

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
    klog_printf("[gta6-60fps] %s %s\n", lvl, msg);
#else
    fprintf(stderr, "[gta6-60fps] %s %s\n", lvl, msg);
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
    send_notification("GTA 6 FPS: ", fmt, ap);
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
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) {
        f = fopen(CONF_PATH, "w");
        if (f) {
            fprintf(f, "# GTA 6 runtime FPS patcher settings\n"
                       "# Set target FPS (30..120). Recommended: 60 or 40 (for 120Hz/VRR TVs).\n"
                       "fps=%u\n", DEFAULT_FPS);
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
        if (!strncmp(line, "fps", 3)) {
            v = atoi(eq + 1);
            if (v >= (int)MIN_FPS && v <= (int)MAX_FPS) {
                g_target_fps = (uint32_t)v;
            } else {
                logmsg("WARN", "ignoring fps=%d (must be %u..%u)", v, MIN_FPS, MAX_FPS);
            }
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------- game access */

#define APP_INFO_LEN 0x100

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

#define MAX_EBOOTS 16

struct eboot_proc {
    uint32_t pid;
    char titleid[16];
};

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
    uint8_t patch[8];
};

static void put32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int ctx_matches_site(const uint8_t cur[64], const struct patch_site *s) {
    return !memcmp(cur, s->ctx, CTX_OFF) &&
           !memcmp(cur + CTX_OFF + s->len, s->ctx + CTX_OFF + s->len,
                   64u - CTX_OFF - s->len);
}

/* Returns: 1 = matches GTA 6 (clean or already patched), 0 = different game,
 * -1 = memory not ready yet. */
static int verify_gta6_process(uint32_t pid, const struct patch_site sites[2],
                               uint64_t *out_base) {
    uint64_t base = 0;
    uint8_t ctx[64];
    unsigned tries;
    for (tries = 1; tries <= RESOLVE_TRIES; tries++) {
        if (!eboot_base(pid, &base)) {
            if (!proc_read(pid, base + sites[0].rva - CTX_OFF, ctx, 64)) {
                if (!ctx_matches_site(ctx, &sites[0]))
                    return 0; /* readable, but not our GTA 6 eboot */
                if (!proc_read(pid, base + sites[1].rva - CTX_OFF, ctx, 64)) {
                    if (!ctx_matches_site(ctx, &sites[1]))
                        return 0;
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
    uint8_t cur[8];
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

/* Also update live RAM globals so the patch takes effect even if GTA 6 was
 * already running before the payload was launched. */
static void update_live_globals(uint32_t pid, uint64_t base) {
    uint32_t cur_us = 0;
    uint32_t want_us = 1000000u / g_target_fps;
    uint8_t auto_lock = 0xffu;
    uint8_t buf[4];

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

static int finish(int code) {
    logmsg("INFO", "exit code %d", code);
    if (g_lock >= 0)
        close(g_lock);
    if (g_log)
        fclose(g_log);
    return code;
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

int main(void) {
    struct patch_site sites[2];
    struct eboot_proc procs[MAX_EBOOTS];
    uint32_t skipped[MAX_EBOOTS];
    unsigned skipped_n = 0, proc_n = 0, i;
    uint32_t pid = 0;
    uint64_t base = 0;
    char titleid[16] = "?";
    unsigned poll, fails = 0;
    int already_open = 0, already1 = 0, already2 = 0;
    unsigned long found_ms;

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
    logmsg("INFO", "gta6-60fps v%s started (target=%u FPS)", TOOL_VERSION, g_target_fps);
#ifndef HOST_TEST
    logmsg("INFO", "console firmware %x.%02x", kernel_get_fw_version() >> 24,
           (kernel_get_fw_version() >> 16) & 0xff);
#endif

    /* Build patch bytes for target FPS:
     *   Site 1 (6B): mov eax, <fps>; nop          -> b8 <imm32> 90
     *   Site 2 (7B): mov eax, <fps>; xchg ax, ax  -> b8 <imm32> 66 90 */
    memset(sites, 0, sizeof(sites));
    sites[0].name = "writer1";
    sites[0].rva = SITE1_RVA;
    sites[0].len = SITE1_LEN;
    sites[0].ctx = SITE1_CTX;
    sites[0].patch[0] = 0xb8;
    put32le(sites[0].patch + 1, g_target_fps);
    sites[0].patch[5] = 0x90;

    sites[1].name = "writer2";
    sites[1].rva = SITE2_RVA;
    sites[1].len = SITE2_LEN;
    sites[1].ctx = SITE2_CTX;
    sites[1].patch[0] = 0xb8;
    put32le(sites[1].patch + 1, g_target_fps);
    sites[1].patch[5] = 0x66;
    sites[1].patch[6] = 0x90;

    notify("ready (%u FPS). Start GTA 6 (waiting up to 10 min)", g_target_fps);

    for (poll = 0; poll < WAIT_POLLS; poll++) {
        if (list_eboot_procs(procs, &proc_n)) {
            if (++fails > LIST_TRIES) {
                notify("couldn't list running apps. Nothing changed.");
                return finish(EXIT_NO_PROC_LIST);
            }
            sleep(1);
            continue;
        }
        fails = 0;
        for (i = 0; i < proc_n; i++) {
            int match;
            if (is_skipped_pid(skipped, skipped_n, procs[i].pid))
                continue;
            /* Ignore PlayStation system/UI apps (NPXSxxxxx) silently */
            if (!strncmp(procs[i].titleid, "NPXS", 4)) {
                logmsg("INFO", "skipping system app %s (pid %u)",
                       procs[i].titleid, procs[i].pid);
                add_skipped_pid(skipped, &skipped_n, procs[i].pid);
                continue;
            }
            match = verify_gta6_process(procs[i].pid, sites, &base);
            if (match == 1) {
                pid = procs[i].pid;
                strcpy(titleid, procs[i].titleid);
                already_open = (poll == 0);
                break;
            }
            if (match == 0) {
                logmsg("INFO", "ignoring %s (pid %u): eboot bytes do not match GTA 6",
                       procs[i].titleid, procs[i].pid);
                notify("%s is not matching GTA 6, still waiting", procs[i].titleid);
                add_skipped_pid(skipped, &skipped_n, procs[i].pid);
            }
        }
        if (base)
            break;
        if (poll && poll % 1500u == 0)
            logmsg("INFO", "still waiting for GTA 6 (%u s)", poll / 50u);
        usleep(POLL_USEC);
    }

    if (!base) {
        notify("GTA 6 was not started within 10 min. Closed, nothing changed.");
        return finish(EXIT_NO_GAME);
    }

    found_ms = elapsed_ms();
    logmsg("OK", "GTA 6 verified: %s pid %u base 0x%llx%s", titleid, pid,
           (unsigned long long)base, already_open ? " (already open)" : "");
    notify("found %s, applying %u FPS patch...", titleid, g_target_fps);

    if (apply_site(pid, base, &sites[0], &already1) ||
        apply_site(pid, base, &sites[1], &already2)) {
        notify("write failed. Restart GTA 6 and check %s", LOG_PATH);
        return finish(EXIT_WRITE_FAIL);
    }

    update_live_globals(pid, base);
    logmsg("OK", "all patches applied in %lu ms", elapsed_ms() - found_ms);

    if (already1 && already2)
        notify("%u FPS was already ON for this session.", g_target_fps);
    else
        notify("%u FPS IS ON! Enjoy.", g_target_fps);

    return finish(EXIT_OK);
}
