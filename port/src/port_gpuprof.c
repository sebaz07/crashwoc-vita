/* GPU profiler capture (only built with -DCWOC_GPUPROF=ON; see port_gpuprof.h).
 *
 * The analyzer (psp2-cex-gpues4-prof tools/analyze.py) times draws only when
 * it can replay the retail 3.65 GXM display/scene counters from a seed taken
 * before vglInit, and when every shader program lifetime and GXM context is
 * known. Recording every call from the boot would fill any buffer during the
 * loads, so from vglInit on only the lifetime calls are kept (context and
 * program create/addref/release, scene begin/end, display queue: a few per
 * frame); inside the window [first, first+frames) every GXM call is kept, plus
 * the frame/pass/draw labels of the render thread and the samples of one
 * sampler thread (core 2). Draws are named "#<id>" (draws.csv row) or by what
 * they are (clear, copy, composite); passes come from the engine's material
 * pass slots (MSUB) and the HUD scene. */
#ifdef PORT_GPUPROF
#include "port.h"
#include "port_gpuprof.h"
#include <psp2_gpuprof.h>
#include <psp2_gpuprof_capture.h>
#include <psp2_gpuprof_capture_diagnostic.h>
#include <psp2_gpuprof_gxm_observer.h>
#include <psp2_gpuprof_labels.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GP_DIR "ux0:data/crashwoc/gpuprof"
#define GP_ROWS 65536

int g_gpuprof_tag;
int g_gpuprof_fx;
volatile int g_gpuprof_live;

static Psp2GpuProfCapture g_cap;
static Psp2GpuProfLabels g_lab_rt, g_lab_misc;
static int g_lab_rt_on;
static uint32_t g_rt_tid;
static SceUID g_mtx = -1, g_blk = -1;
static volatile int g_state;   /* 0 off, 1 lifetimes only, 2 window, 3 done */
static int g_first, g_count, g_frames_in;
static int g_hud, g_top, g_item;
static int g_cur_tag = -1, g_scope_open;
static int g_cur_sh = -1;
static PortGpDraw *g_rows;
static unsigned g_nrows, g_rows_full;
static SceUID g_sampler = -1;
static volatile int g_sampler_stop;
static unsigned g_samples, g_sample_fail, g_read_fail;
static Psp2GpuProfInfo g_info;
static uint64_t g_win_t0, g_win_t1;

static void lock(void *p) { (void)p; sceKernelLockMutex(g_mtx, 1, NULL); }
static void unlock(void *p) { (void)p; sceKernelUnlockMutex(g_mtx, 1); }
static uint64_t now(void) { return sceKernelGetSystemTimeWide(); }

/* kinds kept outside the window: what the analyzer needs to know the live
 * contexts and programs and to replay the scene/display counters */
static int lifetime_kind(unsigned k) {
    switch (k) {
    case PSP2_GPUPROF_GXM_BEGIN:
    case PSP2_GPUPROF_GXM_END:
    case PSP2_GPUPROF_GXM_PRESENT:
    case PSP2_GPUPROF_GXM_CONTEXT_CREATE:
    case PSP2_GPUPROF_GXM_CONTEXT_DESTROY:
    case PSP2_GPUPROF_GXM_VERTEX_CREATE:
    case PSP2_GPUPROF_GXM_FRAGMENT_CREATE:
    case PSP2_GPUPROF_GXM_VERTEX_ADDREF:
    case PSP2_GPUPROF_GXM_FRAGMENT_ADDREF:
    case PSP2_GPUPROF_GXM_VERTEX_RELEASE:
    case PSP2_GPUPROF_GXM_FRAGMENT_RELEASE:
    case PSP2_GPUPROF_GXM_PATCHER_DESTROY:
    case PSP2_GPUPROF_GXM_MASK_FRAGMENT_CREATE:
        return 1;
    default:
        return 0;
    }
}

static void observe(void *u, const Psp2GpuProfGxmCall *c) {
    int st = g_state;
    (void)u;
    if (st == 2 || (st == 1 && lifetime_kind(c->kind))) psp2GpuProfCaptureGxmCall(&g_cap, c);
    /* name programs after the game shader that made vitaGL create them */
    if ((st == 1 || st == 2) && !c->result && g_cur_sh >= 0 && c->thread == g_rt_tid && g_lab_rt_on &&
        (c->kind == PSP2_GPUPROF_GXM_VERTEX_CREATE || c->kind == PSP2_GPUPROF_GXM_FRAGMENT_CREATE)) {
        char name[16];
        snprintf(name, sizeof(name), "s%03d%s", g_cur_sh, c->kind == PSP2_GPUPROF_GXM_VERTEX_CREATE ? "v" : "f");
        psp2GpuProfShaderName(&g_lab_rt, now(), c->kind == PSP2_GPUPROF_GXM_VERTEX_CREATE ? 1 : 2, c->args[0],
                              c->args[2], name);
    }
}

void port_gpuprof_init(void) {
    const char *e = getenv("CWOC_GPUPROF");
    int mb = 24, rc;
    size_t cap;
    void *mem;
    Psp2GpuProfCaptureConfig cfg;
    if (!e || sscanf(e, "%d,%d,%d", &g_first, &g_count, &mb) < 2 || g_first < 1 || g_count < 1) {
        port_log("gpuprof: off (CWOC_GPUPROF=<first>,<frames>[,<MB>])");
        return;
    }
    if (mb < 4 || mb > 64) mb = 24;
    g_blk = sceKernelAllocMemBlock("gpuprof", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (SceSize)mb << 20, NULL);
    g_rows = malloc(sizeof(PortGpDraw) * GP_ROWS);
    g_mtx = sceKernelCreateMutex("gpuprof", 0, 0, NULL);
    if (g_blk < 0 || !g_rows || g_mtx < 0 || sceKernelGetMemBlockBase(g_blk, &mem) < 0) {
        port_log("gpuprof: no memory for %d MB (0x%08x)", mb, g_blk);
        return;
    }
    cap = ((size_t)mb << 20) / sizeof(Psp2GpuProfCaptureEvent);
    memset(&cfg, 0, sizeof(cfg));
    cfg.events = mem;
    cfg.capacity = cap;
    cfg.lock = lock;
    cfg.unlock = unlock;
    rc = psp2GpuProfCaptureInit(&g_cap, &cfg);
    if (!rc) rc = psp2GpuProfCaptureStart(&g_cap);
    /* fresh GXM: vitaGL initializes it right after this */
    if (!rc) rc = psp2GpuProfCaptureGxmIdentitySeed365(&g_cap, (uint32_t)sceKernelGetProcessId(), 0, 0);
    if (rc) {
        port_log("gpuprof: capture init %d", rc);
        return;
    }
    psp2GpuProfLabelsBegin(&g_lab_misc, &g_cap, (uint32_t)sceKernelGetThreadId());
    g_info.size = sizeof(g_info);
    g_info.abi = PSP2_GPUPROF_ABI;
    rc = psp2GpuProfGetInfo(&g_info);
    port_log("gpuprof: frames %d-%d, %u events (%d MB), plugin rc %d status %d caps 0x%x fingerprint 0x%08x", g_first,
             g_first + g_count - 1, (unsigned)cap, mb, rc, g_info.status, g_info.capabilities,
             g_info.driver_fingerprint);
    if (rc || g_info.status || !(g_info.capabilities & PSP2_GPUPROF_CAP_DIAGNOSTIC))
        port_log("gpuprof: no hardware samples (plugin not loaded?): the capture will have no timing");
    g_state = 1;
    psp2GpuProfGxmSetObserver(observe, NULL);
}

/* ---------------------------------------------------------------- labels */
void port_gpuprof_msub(int k, int on) {
    switch (k) {
    case 0: g_top = on ? 1 : 0; break; /* NuMtlRender3d: material lists */
    case 5: g_top = on ? 2 : 0; break; /* NuMtlRenderOT: sorted */
    case 6: g_top = on ? 3 : 0; break; /* dynamic, face-on, water, shadow, upd */
    case 8: g_item = on ? 1 : 0; break; /* geometry item */
    case 9: g_item = on ? 2 : 0; break; /* skinned item */
    case 10: g_item = on ? 3 : 0; break; /* 2d / face-on item */
    default: return;
    }
    g_gpuprof_tag = g_hud << 4 | g_top << 2 | g_item;
}
void port_gpuprof_hud(void) {
    g_hud = 1;
    g_gpuprof_tag = g_hud << 4 | g_top << 2 | g_item;
}
void port_gpuprof_frame_begin(void) {
    g_hud = g_top = g_item = 0;
    g_gpuprof_tag = 0;
}

static const char *tag_name(int tag, char *buf, size_t n) {
    static const char *top[4] = {"rest", "opaque", "sorted", "other"};
    static const char *item[4] = {"", ".geom", ".skin", ".face"};
    snprintf(buf, n, "%s %s%s%s", tag & 16 ? "hud" : "3d", top[(tag >> 2) & 3], item[tag & 3], tag & 32 ? " fx" : "");
    return buf;
}

void port_gpuprof_scope(int tag) {
    char buf[32];
    if (!g_gpuprof_live || tag == g_cur_tag) return;
    if (g_scope_open) psp2GpuProfScopePop(&g_lab_rt, now());
    g_scope_open = !psp2GpuProfScopePush(&g_lab_rt, now(), tag_name(tag, buf, sizeof(buf)));
    g_cur_tag = tag;
}
int port_gpuprof_skip(int tag) {
    static int nofx = -1;
    if (nofx < 0) nofx = getenv("CWOC_GPUPROF_NOFX") && getenv("CWOC_GPUPROF_NOFX")[0] == '1';
    return nofx && (tag & 32);
}
void port_gpuprof_name(const char *name) {
    if (g_gpuprof_live) psp2GpuProfDrawName(&g_lab_rt, now(), name);
}
PortGpDraw *port_gpuprof_draw(void) {
    char name[16];
    PortGpDraw *d;
    if (!g_gpuprof_live) return NULL;
    if (g_nrows >= GP_ROWS) {
        if (!g_rows_full++) port_gpuprof_name("rows full");
        return NULL;
    }
    d = &g_rows[g_nrows];
    memset(d, 0, sizeof(*d));
    d->frame = (uint32_t)g_frames_in;
    d->tag = (uint8_t)(g_cur_tag < 0 ? 0 : g_cur_tag);
    snprintf(name, sizeof(name), "#%u", g_nrows++);
    port_gpuprof_name(name);
    return d;
}
void port_gpuprof_shader(int sh) {
    if (!g_lab_rt_on && g_state == 1) {
        g_rt_tid = (uint32_t)sceKernelGetThreadId();
        psp2GpuProfLabelsBegin(&g_lab_rt, &g_cap, g_rt_tid);
        g_lab_rt_on = 1;
    }
    g_cur_sh = sh;
}

/* ---------------------------------------------------------------- sampler */
static int sampler(SceSize size, void *arg) {
    static const unsigned groups[] = {2, 4, 43, 55, 71, 72, 75, 76, 77, 78, 103, 104, 107, 108, 109, 110};
    uint64_t id = 0;
    (void)size; (void)arg;
    while (!g_sampler_stop) {
        unsigned group = groups[id % 16];
        Psp2GpuProfDiagnosticConfig cfg;
        Psp2GpuProfDiagnostic data;
        Psp2GpuProfDiagnosticObservation o;
        int clk;
        memset(&cfg, 0, sizeof(cfg));
        memset(&data, 0, sizeof(data));
        memset(&o, 0, sizeof(o));
        cfg.size = sizeof(cfg);
        cfg.abi = PSP2_GPUPROF_ABI;
        cfg.group = group;
        cfg.tag_group = group >= 103 ? 102 : 70;
        o.sampler_thread = (uint32_t)sceKernelGetThreadId();
        o.sample_id = id++;
        o.group = group;
        o.tag_group = cfg.tag_group;
        o.process_id = (uint32_t)sceKernelGetProcessId();
        o.driver_fingerprint = g_info.driver_fingerprint;
        clk = scePowerGetGpuClockFrequency();
        o.clock_before_mhz = clk > 0 ? (unsigned)clk : 0;
        o.before_us = now();
        o.result = psp2GpuProfReadDiagnostic(&cfg, &data);
        o.after_us = now();
        clk = scePowerGetGpuClockFrequency();
        o.clock_after_mhz = clk > 0 ? (unsigned)clk : 0;
        if (o.result) g_read_fail++;
        if (psp2GpuProfCaptureDiagnostic(&g_cap, &o, o.result ? NULL : &data)) g_sample_fail++;
        else g_samples++;
        sceKernelDelayThread(500);
    }
    return 0;
}

/* ---------------------------------------------------------------- window */
static void write_rows(void) {
    FILE *f = fopen(GP_DIR "/draws.csv", "w");
    unsigned i;
    if (!f) return;
    fprintf(f, "id,frame,tag,kind,shader,mode,nv,ni,ntev,tw,th,blend,bsrc,bdst,zen,zfn,zup,cull,aupd,aref0,aref1,"
               "sx,sy,sw,sh\n");
    for (i = 0; i < g_nrows; i++) {
        const PortGpDraw *d = &g_rows[i];
        fprintf(f, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%u,%u,%u,%u\n", i, d->frame, d->tag,
                d->kind, d->sh, d->mode, d->nv, d->ni, d->ntev, d->tw, d->th, d->blend, d->bsrc, d->bdst, d->zen, d->zfn,
                d->zup, d->cull, d->aupd, d->aref0, d->aref1, d->sx, d->sy, d->sw, d->sh_);
    }
    fclose(f);
}

static void window_open(int frame) {
    g_win_t0 = now();
    g_frames_in = 0;
    g_cur_tag = -1;
    g_state = 2; /* this thread is the only GXM producer and sits between frames */
    g_gpuprof_live = 1;
    psp2GpuProfFrameName(&g_lab_rt, now(), (uint64_t)frame, "game");
    if (!g_info.status && (g_info.capabilities & PSP2_GPUPROF_CAP_DIAGNOSTIC)) {
        g_sampler = sceKernelCreateThread("gpuprof-sampler", sampler, 0x10000100, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_2,
                                          NULL);
        if (g_sampler >= 0 && sceKernelStartThread(g_sampler, 0, NULL) < 0) {
            sceKernelDeleteThread(g_sampler);
            g_sampler = -1;
        }
    }
    port_log("gpuprof: window opens at frame %d (sampler %s)", frame, g_sampler >= 0 ? "on" : "off");
}

static void window_close(int frame) {
    FILE *f;
    int rc;
    uint64_t t0;
    if (g_scope_open) psp2GpuProfScopePop(&g_lab_rt, now());
    g_scope_open = 0;
    psp2GpuProfDrawName(&g_lab_rt, now(), NULL);
    g_gpuprof_live = 0;
    g_state = 3;
    g_win_t1 = now();
    if (g_sampler >= 0) {
        g_sampler_stop = 1;
        sceKernelWaitThreadEnd(g_sampler, NULL, NULL);
        sceKernelDeleteThread(g_sampler);
        g_sampler = -1;
    }
    rc = psp2GpuProfCaptureStop(&g_cap);
    t0 = now();
    sceIoMkdir(GP_DIR, 0777);
    f = fopen(GP_DIR "/capture.csv", "w");
    if (!f) rc = PSP2_GPUPROF_CAPTURE_IO;
    if (!rc) rc = psp2GpuProfCaptureWriteCsv(&g_cap, f);
    if (f && fclose(f)) rc = PSP2_GPUPROF_CAPTURE_IO;
    write_rows();
    f = fopen(GP_DIR "/shaders.txt", "w");
    if (f) {
        port_gpuprof_dump_shaders(f);
        fclose(f);
    }
    port_log("gpuprof: window closed at frame %d: %d frames in %.1f ms, %u events, %llu dropped, %u draws%s, "
             "%u samples (%u read failures, %u record failures); export %d in %.1f s",
             frame, g_frames_in, (g_win_t1 - g_win_t0) / 1000.0, (unsigned)g_cap.count,
             (unsigned long long)g_cap.dropped, g_nrows, g_rows_full ? " (rows full)" : "", g_samples, g_read_fail,
             g_sample_fail, rc, (now() - t0) / 1e6);
    port_log("gpuprof: done");
    /* the recorder stays allocated: other code may still reach the observer */
}

void port_gpuprof_present(int frame) {
    if (g_state == 1 && g_lab_rt_on && frame >= g_first) window_open(frame);
    else if (g_state == 2) {
        if (++g_frames_in >= g_count) window_close(frame);
        else psp2GpuProfFrameName(&g_lab_rt, now(), (uint64_t)frame, "game");
    }
}
#endif
