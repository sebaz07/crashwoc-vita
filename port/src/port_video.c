/* Window, GL context, frame pacing and the Dolphin DEMO library entry points
 * the game uses (DEMOInit/BeforeRender/DoneRender/SwapBuffers, debug text). */
#include "port.h"
#include "port_gl.h"
#include "port_prof.h"
#include "port_gpuprof.h"
#ifdef PORT_VITA
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/display.h>
#endif
#include <SDL2/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void port_os_init(void);
void port_fs_init(void);
int PADInit(void);

static SDL_Window *g_window;
static SDL_GLContext g_glctx;
static int g_frames, g_draws, g_verts;
static Uint32 g_last_report, g_frame_start;
static int g_headless_frames = -1;
int port_frame_count(void) { return g_frames; }

/* GXRenderModeObj (NTSC 640x448 EFB) for GS_Init. */
void port_thread_register(const char *name); /* CPU census (below) */

static struct {
    unsigned int viTVmode;
    unsigned short fbWidth, efbHeight, xfbHeight, viXOrigin, viYOrigin, viWidth, viHeight;
    unsigned int xFBmode;
    unsigned char field_rendering, aa, sample_pattern[12][2], vfilter[7];
} g_rmode = { 0, 640, 448, 448, 40, 16, 640, 448, 0, 0, 0, {{0}}, {0} };

/* Display mode: 0 = original 4:3 picture, pillarboxed; 1 = 4:3 picture
 * stretched to the whole screen; 2 = widescreen (whole screen, wider field of
 * view: gs.c / nucamera.c ask port_wide_factor()). Chosen with CWOC_DISPLAY or
 * <save dir>/display.txt; on the Vita SELECT + D-pad up cycles and saves it. */
static int g_display_mode;
static int g_win_w = 640, g_win_h = 448;
#ifdef PORT_VITA
/* vitaGL's display queue callback, once per frame put on screen: how many
 * vblanks each frame stayed there (the 5 s perf lines). With the 30 fps lock
 * every frame should stay 2; 1 and 3 are an uneven rhythm. */
static volatile unsigned g_vb_hist[5];
static unsigned g_vb_last;
static void disp_cb(void *fb) {
    unsigned v = (unsigned)sceDisplayGetVcount();
    (void)fb;
    if (g_vb_last) {
        unsigned d = v - g_vb_last;
        g_vb_hist[d > 4 ? 4 : d]++;
    }
    g_vb_last = v;
}
#endif

static const char *const g_display_names[3] = { "4:3", "stretch", "wide" };

float port_wide_factor(void) {
    if (g_display_mode != 2 || g_win_h <= 0) return 1.0f;
    return ((float)g_win_w / (float)g_win_h) / (4.0f / 3.0f);
}

static void apply_drawable(void) {
    int w = g_win_w, h = g_win_h, dw, dh;
    if (g_display_mode != 0) {
        port_gx_set_drawable(w, h, 0, 0, w, h);
        return;
    }
    dh = h;
    dw = h * 4 / 3;
    if (dw > w) { dw = w; dh = w * 3 / 4; }
    port_gx_set_drawable(w, h, (w - dw) / 2, (h - dh) / 2, dw, dh);
}

static void display_path(char *buf, size_t n) {
    extern const char *port_save_dir(void);
    snprintf(buf, n, "%s/display.txt", port_save_dir());
}

static void display_load(void) {
    const char *e = getenv("CWOC_DISPLAY");
    char path[512], word[16] = "";
    int i;
    if (!e) {
        FILE *f;
        display_path(path, sizeof(path));
        f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%15s", word) != 1) word[0] = 0;
            fclose(f);
        }
        e = word;
    }
    for (i = 0; i < 3; i++)
        if (!strcmp(e, g_display_names[i])) g_display_mode = i;
    port_log("video: display mode %s", g_display_names[g_display_mode]);
}

int port_display_mode(void) { return g_display_mode; }

/* next (dir > 0) or previous display mode, saved (pause menu > OPTIONS) */
void port_display_step(int dir) {
    char path[512];
    FILE *f;
    g_display_mode = (g_display_mode + (dir < 0 ? 2 : 1)) % 3;
    apply_drawable();
    display_path(path, sizeof(path));
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", g_display_names[g_display_mode]);
        fclose(f);
    }
    port_log("video: display mode %s", g_display_names[g_display_mode]);
}

void port_display_cycle(void) { port_display_step(1); }

static void update_drawable(void) {
    SDL_GL_GetDrawableSize(g_window, &g_win_w, &g_win_h);
    apply_drawable();
}

void port_platform_init(int argc, char **argv) {
    int i, w = 960, h = 720;
    (void)argc; (void)argv;
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_headless_frames = atoi(argv[++i]);
    if (getenv("CWOC_FRAMES")) g_headless_frames = atoi(getenv("CWOC_FRAMES"));
#ifdef PORT_VITA
    /* vitaGL owns the display; SDL only provides timers */
    if (SDL_Init(SDL_INIT_TIMER) != 0)
        port_fatal("SDL_Init: %s", SDL_GetError());
#else
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0)
        port_fatal("SDL_Init: %s", SDL_GetError());
#endif
    port_os_init();
    port_fs_init();
    display_load();
#ifdef PORT_VITA
    w = 960; h = 544;
    {
        extern void port_splash_hold(void);
        port_splash_hold(); /* keep the boot splash up for a moment */
    }
    {
        /* vitaGL built with HAVE_SHADER_CACHE (scripts/build_vitagl.sh) keeps
         * each compiled shader here, keyed by its source only: the compiler
         * option (CWOC_SHOPT) goes in the folder name. Next to the program
         * cache of gx_gl.c (CWOC_SHCACHEDIR moves both). A no-op with the
         * VitaSDK vitaGL. */
        const char *d = getenv("CWOC_SHCACHEDIR"), *o = getenv("CWOC_SHOPT");
        char path[256];
        uint64_t t0 = port_prof_now();
        if (!d || !d[0]) d = "ux0:data/crashwoc/shadercache";
        mkdir(d, 0777);
        snprintf(path, sizeof(path), "%s/vgl%s", d, o && o[0] >= '0' && o[0] <= '4' ? o : "");
        vglSetShaderCachePath(path);
        if (getenv("CWOC_PBSIZE")) { /* measurement: sceGxm parameter buffer in MB (default 16) */
            int mb = atoi(getenv("CWOC_PBSIZE"));
            if (mb >= 4 && mb <= 128) {
                vglSetParamBufferSize((uint32_t)mb * 1024 * 1024);
                port_log("video: parameter buffer %d MB", mb);
            }
        }
        /* shaders shipped in the VPK (CMake CWOC_SHADER_SEED): read when the
         * cache on the card does not have them; CWOC_SHSEED=0 ignores them */
        if (!(getenv("CWOC_SHSEED") && getenv("CWOC_SHSEED")[0] == '0')) {
            char seed[64];
            snprintf(seed, sizeof(seed), "app0:shaders/vgl%s", o && o[0] >= '0' && o[0] <= '4' ? o : "");
            vglSetShaderCacheSeedPath(seed);
        }
#ifdef PORT_GPUPROF
        port_gpuprof_init(); /* GPU profiler build: observe GXM from its initialization */
#endif
        vglInitExtended(0, w, h, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE); /* MSAA off: fill rate */
        vglSetDisplayCallback(disp_cb);
        port_log("video: vitaGL init %.0f ms, shader cache %s", (port_prof_now() - t0) / 1000.0, path);
    }
    {
        /* CWOC_SHOPT=0..4 (shark_opt O0, O1, O2, O3 = vitaGL default, Ofast):
         * optimisation level of the runtime shader compiler, for weighing
         * first-compile time against GPU cost (part of the program cache key) */
        const char *o = getenv("CWOC_SHOPT");
        if (o && o[0] >= '0' && o[0] <= '4') {
            /* a 'p' suffix (e.g. 3p) also turns on fast (half) precision */
            int fp = o[1] == 'p';
            vglSetupRuntimeShaderCompiler((shark_opt)(o[0] - '0'), GL_TRUE, fp ? GL_TRUE : GL_FALSE, GL_TRUE);
            port_log("video: shader compiler optimisation %c%s", o[0], fp ? " + fast precision" : "");
        }
    }
#else
    if (getenv("CWOC_WINSIZE")) sscanf(getenv("CWOC_WINSIZE"), "%dx%d", &w, &h); /* testing */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("Crash Bandicoot: The Wrath of Cortex (native port)", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_window) port_fatal("SDL_CreateWindow: %s", SDL_GetError());
    g_glctx = SDL_GL_CreateContext(g_window);
    if (!g_glctx) port_fatal("SDL_GL_CreateContext: %s", SDL_GetError());
    SDL_GL_SetSwapInterval(1);
    port_log("video: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    update_drawable();
#endif
#ifdef PORT_VITA
    g_win_w = w;
    g_win_h = h;
    apply_drawable();
#endif
    port_thread_register("game");
    port_gx_init();
    PADInit();
    g_last_report = SDL_GetTicks();
}

/* sub-timers for the material pass (numtl.c / nurndr.c MSUB_*).
 * CWOC_MSUB=1: sampled (1 call in `rate`); CWOC_MSUB=2: every call (exact,
 * no sampling bias; each call costs two timer reads, ~1.4 us, counted in
 * the calls column). Slots 8+: item types and their parts. */
#define MSUB_N 16
uint64_t g_msub[MSUB_N];
static uint64_t g_msub_t0[MSUB_N];
static unsigned g_msub_c[MSUB_N], g_msub_r[MSUB_N], g_msub_calls[MSUB_N];
static int g_msub_on = -1;
void port_msub_begin(int k, int rate) {
    GP_MSUB(k, 1); /* GPU profiler build: pass labels */
    /* off by default (each sample is two timer syscalls on the Vita):
     * CWOC_MSUB=1 on PC, or build with -DPORT_MSUB_DEFAULT=1 */
    if (g_msub_on < 0) {
#ifdef PORT_MSUB_DEFAULT
        g_msub_on = PORT_MSUB_DEFAULT;
#else
        g_msub_on = getenv("CWOC_MSUB") ? atoi(getenv("CWOC_MSUB")) : 0;
        if (g_msub_on < 0) g_msub_on = 0;
#endif
    }
    if (!g_msub_on || (unsigned)k >= MSUB_N) return;
    if (g_msub_on == 2) rate = 1;
    g_msub_calls[k]++;
    g_msub_r[k] = (unsigned)rate;
    g_msub_t0[k] = (++g_msub_c[k] % (unsigned)rate) == 0 ? port_prof_now() : 0;
}
void port_msub_end(int k) {
    GP_MSUB(k, 0);
    if ((unsigned)k < MSUB_N && g_msub_t0[k]) g_msub[k] += (port_prof_now() - g_msub_t0[k]) * g_msub_r[k];
}

/* CPU census: each port thread registers itself; every report line gives
 * each one's CPU time (runClocks), its share of the window, its core and its
 * preemptions, so the free time per core is known (CWOC_CPUCENSUS=0: off). */
#define CENSUS_N 8
static struct { const char *name; int uid; uint64_t run0; unsigned pre0; } g_census[CENSUS_N];
static int g_census_n;
void port_thread_register(const char *name) {
#ifdef PORT_VITA
    int i, uid = sceKernelGetThreadId();
    for (i = 0; i < g_census_n; i++)
        if (g_census[i].uid == uid) return;
    if (g_census_n < CENSUS_N) {
        g_census[g_census_n].name = name;
        g_census[g_census_n].uid = uid;
        g_census_n++;
    }
#else
    (void)name;
#endif
}
static int census_report(char *buf, int size, uint64_t span_us) {
    int n = 0;
#ifdef PORT_VITA
    static int on = -1;
    int i;
    if (on < 0) on = !(getenv("CWOC_CPUCENSUS") && getenv("CWOC_CPUCENSUS")[0] == '0');
    if (!on || !span_us) return 0;
    n += snprintf(buf + n, size - n, "cpu:");
    for (i = 0; i < g_census_n && n < size - 64; i++) {
        SceKernelThreadInfo info;
        uint64_t run;
        unsigned pre;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(g_census[i].uid, &info) < 0) continue;
        run = (uint64_t)info.runClocks;
        pre = info.intrPreemptCount + info.threadPreemptCount;
        if (g_census[i].run0)
            n += snprintf(buf + n, size - n, " %s %.0f%% (core %d, %u preempt)", g_census[i].name,
                          100.0 * (double)(run - g_census[i].run0) / (double)span_us, info.lastExecutedCpuId,
                          pre - g_census[i].pre0);
        g_census[i].run0 = run;
        g_census[i].pre0 = pre;
    }
#else
    (void)buf; (void)size; (void)span_us;
#endif
    return n;
}

void port_gx_stats_draw(int nverts) {
    g_draws++;
    g_verts += nverts;
}

static void pump_events(void) {
    SDL_Event e;
#ifdef PORT_VITA
    return;
#endif
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            port_log("quit requested");
            exit(0);
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) exit(0);
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) update_drawable();
    }
}

/* ---- DEMO library ---- */
void DEMOInit(void *mode) { (void)mode; }
void *DEMOGetRenderModeObj(void) { return &g_rmode; }
void *DEMOGetCurrentBuffer(void) { return NULL; }

void DEMOBeforeRender(void) {
    g_frame_start = SDL_GetTicks();
    port_gx_begin_frame();
}

void DEMODoneRender(void) {}

/* Debug: CWOC_SHOT_DIR + CWOC_SHOT_EVERY dump frames as PPM before swapping;
 * port_shot_burst(n) (called from game code under test) shoots the next n. */
static int g_shot_burst;
void port_shot_burst(int n) { g_shot_burst = n; }
static void maybe_screenshot(int frame) {
    static const char *dir;
    static int every = -1;
    int w, h, y;
    unsigned char *px;
    char path[512];
    FILE *f;
    if (every < 0) {
        dir = getenv("CWOC_SHOT_DIR");
        every = getenv("CWOC_SHOT_EVERY") ? atoi(getenv("CWOC_SHOT_EVERY")) : 60;
        if (every <= 0) every = 60;
    }
    {
        static int from = -1;
        if (from < 0) from = getenv("CWOC_SHOT_FROM") ? atoi(getenv("CWOC_SHOT_FROM")) : 0;
        if (frame < from && g_shot_burst <= 0) return;
    }
    if (!dir || ((frame % every) != 0 && g_shot_burst <= 0)) return;
    if (g_shot_burst > 0) g_shot_burst--;
#ifdef PORT_VITA
    /* the back buffer before the swap (a GPU sync: only on the requested
     * frames, for comparing a given game frame between builds or options) */
    mkdir(dir, 0777);
    w = g_win_w;
    h = g_win_h;
#else
    SDL_GL_GetDrawableSize(g_window, &w, &h);
#endif
    {
    px = malloc((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
#ifdef PORT_VITA
    {
        /* vitaGL hands back the colour buffer's BGR order */
        size_t i;
        for (i = 0; i < (size_t)w * h * 3; i += 3) {
            unsigned char t = px[i];
            px[i] = px[i + 2];
            px[i + 2] = t;
        }
    }
#endif
    snprintf(path, sizeof(path), "%s/frame_%05d.ppm", dir, frame);
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
        fclose(f);
    }
    free(px);
    }
}

/* debugging: CWOC_WARP=level@frame requests a level change (new_level) */
static void maybe_warp(void) {
    static int lvl = -2, at;
    extern int new_level;
    if (lvl == -2) {
        const char *w = getenv("CWOC_WARP");
        lvl = -1;
        if (w && sscanf(w, "%i@%d", &lvl, &at) != 2) lvl = -1;
    }
    if (lvl >= 0 && g_frames == at) {
        port_log("warp: new_level = %d", lvl);
        new_level = lvl;
    }
}

uint64_t g_prof[PROF_N];
unsigned port_ps_calls, port_ps_quads; /* renderpsdma (dummyfunc.c): particle batches and quads */
/* game-side GX emulation time so far (crashlib.c splits it per timing slot) */
unsigned long long port_prof_gx_total(void) {
    uint64_t s = 0;
    int i;
    for (i = 0; i < PROF_WAIT; i++) s += g_prof[i];
    return s;
}
uint32_t g_prof_cnt[PROF_N];
uint64_t g_prof_child;
uint64_t g_prof_ovh;
uint32_t g_prof_fast, g_prof_slow;

static volatile float g_bench_sink;
static double bench_mul(float x) {
    uint64_t t0 = port_prof_now();
    float a = x;
    int i;
    for (i = 0; i < 200000; i++) a = a * 0.999f + x * 0.001f;
    g_bench_sink = a;
    return (port_prof_now() - t0) * 1000.0 / 200000.0;
}

/* timer cost (subtracted from every measured section) and a few CPU facts */
void port_prof_calibrate(void) {
    uint64_t t0, t1;
    int i;
    unsigned fpscr = 0;
    t0 = port_prof_now();
    for (i = 0; i < 2000; i++) t1 = port_prof_now();
    t1 = port_prof_now();
    g_prof_ovh = (t1 - t0) / 2001;
#ifdef __arm__
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
#endif
    port_log("prof: timer %.2f us/read, fpscr %08x, float mul-add %.1f ns (normal) %.1f ns (denormal)",
             (double)(t1 - t0) / 2001.0, fpscr, bench_mul(0.5f), bench_mul(1e-39f));
}

uint64_t port_prof_now(void) {
#ifdef PORT_VITA
    return (uint64_t)sceKernelGetProcessTimeWide();
#else
    return (uint64_t)(SDL_GetPerformanceCounter() * 1000000.0 / (double)SDL_GetPerformanceFrequency());
#endif
}

/* CWOC_GPUFIN=1 (measurement only): wait for the GPU after every swap and time
 * it, i.e. the GPU time of each frame (serialises CPU and GPU: lower fps) */
static int g_gpufin = -1;
static uint64_t g_gpufin_us;
static unsigned g_gpufin_n;
static const char *port_gpufin_report(int nf) {
    static char b[48];
    b[0] = 0;
    if (g_gpufin > 0 && g_gpufin_n) {
        snprintf(b, sizeof(b), " | gpufin %.1f (%u)", g_gpufin_us / 1000.0 / g_gpufin_n, g_gpufin_n / (unsigned)nf);
        g_gpufin_us = 0;
        g_gpufin_n = 0;
    }
    return b;
}

static void prof_report(int nf, uint64_t span) {
    static const char *names[PROF_N] = {"block", "capture", "verts", "shader", "texhash", "texdecode", "wait",
                                        "rt_draw", "rt_tex", "rt_copy", "rt_swap", "rt_xf"};
    char buf[1536];
    int i, n = 0, threaded = port_gx_render_threaded();
    uint64_t sum = 0, rt = 0;
    if (nf <= 0 || !span) return;
    for (i = 0; i < PROF_GAME_N; i++) sum += g_prof[i];
    for (i = PROF_GAME_N; i < PROF_N; i++) rt += g_prof[i];
    if (!threaded) sum += rt;
    n += snprintf(buf + n, sizeof(buf) - n, "prof ms/frame: total %.1f game %.1f | fastdecode %u%%", span / 1000.0 / nf,
                  (span > sum ? span - sum : 0) / 1000.0 / nf,
                  g_prof_fast + g_prof_slow ? 100u * g_prof_fast / (g_prof_fast + g_prof_slow) : 0u);
    g_prof_fast = g_prof_slow = 0;
    for (i = 0; i < PROF_N; i++) {
        if (i == PROF_GAME_N) n += snprintf(buf + n, sizeof(buf) - n, " | render %s busy %.1f:", threaded ? "thread" : "inline", rt / 1000.0 / nf);
        n += snprintf(buf + n, sizeof(buf) - n, " %s %.1f(%u)", names[i], g_prof[i] / 1000.0 / nf, g_prof_cnt[i] / (unsigned)nf);
        g_prof[i] = 0;
        g_prof_cnt[i] = 0;
    }
    port_log("%s", buf);
    {
        /* game timing bars (crashlib.c tbslot*, main.c TBCODE/TBDRAW), inclusive ms per frame */
        extern unsigned long long port_tb_us[40];
        extern unsigned int port_tb_n[40];
        static const struct { int slot; const char *name; } tb[] = {
            {1, "logic"}, {20, "plats"}, {21, "chars"}, {22, "update"}, {23, "deb"}, {7, "plrterr"},
            {9, "scene"}, {26, "world"}, {24, "dchars"}, {25, "dcrash"}, {27, "ddeb"}, {28, "dcrates"},
            {29, "dwumpa"}, {30, "dmisc"}, {14, "explo"}, {2, "fx"}, {31, "panel"}, {33, "mtlrender"}};
        n = snprintf(buf, sizeof(buf), "tb ms/frame: ticks %.2f |", port_tb_n[21] / (double)nf);
        for (i = 0; i < (int)(sizeof(tb) / sizeof(tb[0])); i++)
            n += snprintf(buf + n, sizeof(buf) - n, " %s %.1f", tb[i].name, port_tb_us[tb[i].slot] / 1000.0 / nf);
        {
            extern uint64_t g_vsub[5];
            extern unsigned g_idxc_hit, g_idxc_miss;
            static const char *vn[5] = {"blk", "idxrange", "idxother", "tskin", "skin"};
            n += snprintf(buf + n, sizeof(buf) - n, " | verts:");
            for (i = 0; i < 5; i++) {
                n += snprintf(buf + n, sizeof(buf) - n, " %s %.1f", vn[i], g_vsub[i] / 1000.0 / nf);
                g_vsub[i] = 0;
            }
            n += snprintf(buf + n, sizeof(buf) - n, " idxcache %u/%u", g_idxc_hit / (unsigned)nf, g_idxc_miss / (unsigned)nf);
            g_idxc_hit = g_idxc_miss = 0;
            {
                extern unsigned g_skc_hit, g_skc_miss;
                n += snprintf(buf + n, sizeof(buf) - n, " skincache %u/%u", g_skc_hit / (unsigned)nf, g_skc_miss / (unsigned)nf);
                g_skc_hit = g_skc_miss = 0;
            }
            {
                extern int port_gx_sg_report(char *buf, int size, int nf);
                n += port_gx_sg_report(buf + n, (int)sizeof(buf) - n, nf);
            }
            {
                extern int port_gx_rec_report(char *buf, int size, int nf);
                n += port_gx_rec_report(buf + n, (int)sizeof(buf) - n, nf);
            }
            {
                extern uint64_t g_dsub[5];
                extern int g_dsub_on;
                static const char *dn[5] = {"prep", "lookup", "hitcopy", "miss", "tail"};
                if (g_dsub_on > 0) {
                    n += snprintf(buf + n, sizeof(buf) - n, " | dsub:");
                    for (i = 0; i < 5; i++) {
                        n += snprintf(buf + n, sizeof(buf) - n, " %s %.1f", dn[i], g_dsub[i] / 1000.0 / nf);
                        g_dsub[i] = 0;
                    }
                }
            }
            if (g_msub_on > 0) {
                static const char *mn[MSUB_N] = {"m3d", "mstate", "wmtx", "shsetup", "draw", "ot", "other", "item",
                                                 "geom", "skin", "2d/face", "skinsetup", "skindraw", "lights",
                                                 "blendloop", "blendrest"};
                n += snprintf(buf + n, sizeof(buf) - n, " | mtl:");
                for (i = 0; i < MSUB_N; i++) {
                    n += snprintf(buf + n, sizeof(buf) - n, " %s %.2f(%u)", mn[i], g_msub[i] / 1000.0 / nf,
                                  g_msub_calls[i] / (unsigned)nf);
                    g_msub[i] = 0;
                    g_msub_calls[i] = 0;
                }
            }
        }
        port_log("%s", buf);
        {
            /* game thread, exclusive ms/frame: engine material passes (slot 33)
             * are taken out of the slots they run in (crashlib.c port_tb_nest) */
            extern unsigned long long port_tb_nest[40];
#define TBX(s) ((port_tb_us[s] - port_tb_nest[s]) / 1000.0 / nf)
            double fr = port_tb_us[0] / 1000.0 / nf, mtl = port_tb_us[33] / 1000.0 / nf;
            double lg = TBX(34), sc = TBX(9), ex = TBX(14), fx = TBX(2), pn = TBX(31);
            port_log("gt ms/frame: frame %.1f (+%.1f outside) | logic %.1f (%.2f ticks, %.2f/tick) scene %.1f explo %.1f "
                     "fx %.1f panel %.1f mtl %.1f (in scene %.1f explo %.1f fx %.1f panel %.1f) rest %.1f%s",
                     fr, span / 1000.0 / nf - fr, lg, port_tb_n[34] / (double)nf,
                     port_tb_n[34] ? (port_tb_us[34] - port_tb_nest[34]) / 1000.0 / port_tb_n[34] : 0.0, sc, ex, fx, pn,
                     mtl, port_tb_nest[9] / 1000.0 / nf, port_tb_nest[14] / 1000.0 / nf, port_tb_nest[2] / 1000.0 / nf,
                     port_tb_nest[31] / 1000.0 / nf, fr - (lg + sc + ex + fx + pn + mtl), port_gpufin_report(nf));
#undef TBX
            {
                extern unsigned long long port_tb_gx[40];
                port_log("gtgx ms/frame: GX emulation in scene %.1f explo %.1f fx %.1f panel %.1f | clouds %.1f "
                         "(gx %.1f) | particles %u batches %u quads",
                         port_tb_gx[9] / 1000.0 / nf, port_tb_gx[14] / 1000.0 / nf, port_tb_gx[2] / 1000.0 / nf,
                         port_tb_gx[31] / 1000.0 / nf, (port_tb_us[35] - port_tb_nest[35]) / 1000.0 / nf,
                         port_tb_gx[35] / 1000.0 / nf, port_ps_calls / (unsigned)nf, port_ps_quads / (unsigned)nf);
                port_ps_calls = port_ps_quads = 0;
                for (i = 0; i < 40; i++) port_tb_gx[i] = 0;
            }
        }
        for (i = 0; i < 40; i++) port_tb_us[i] = 0, port_tb_n[i] = 0;
        {
            extern unsigned long long port_tb_nest[40];
            for (i = 0; i < 40; i++) port_tb_nest[i] = 0;
        }
    }
    {
        /* shader programs created in this window (gx_gl.c ensure_program) */
        extern unsigned g_shc_n, g_shc_hit;
        extern uint64_t g_shc_us[4];
        extern void port_gxo_report(int nf);
        port_gxo_report(nf);
        extern uint64_t g_wk_us;
        extern unsigned g_wk_jobs;
        if (g_wk_jobs) {
            port_log("xfworker: %.1f ms/frame on core 2, %u jobs/frame", g_wk_us / 1000.0 / nf, g_wk_jobs / (unsigned)nf);
            g_wk_us = 0;
            g_wk_jobs = 0;
        }
        {
            extern int g_xfsub_on;
            extern uint64_t g_xfsub_us[2][4];
            extern unsigned g_xfsub_v[2][4], g_xfsub_n[2][4];
            static const char *kn[4] = {"skin-cached", "copy", "skin", "view"};
            if (g_xfsub_on > 0) {
                char b[512];
                int w, k, n = 0;
                for (w = 0; w < 2; w++) {
                    n += snprintf(b + n, sizeof(b) - n, "%s", w ? " | worker:" : "xfsub render:");
                    for (k = 0; k < 4; k++)
                        n += snprintf(b + n, sizeof(b) - n, " %s %.2f ms %u v %u seg", kn[k],
                                      g_xfsub_us[w][k] / 1000.0 / nf, g_xfsub_v[w][k] / (unsigned)nf,
                                      g_xfsub_n[w][k] / (unsigned)nf);
                }
                port_log("%s", b);
                memset(g_xfsub_us, 0, sizeof(g_xfsub_us));
                memset(g_xfsub_v, 0, sizeof(g_xfsub_v));
                memset(g_xfsub_n, 0, sizeof(g_xfsub_n));
            }
        }
#ifdef PORT_VITA
        {
            unsigned h[5], k;
            for (k = 0; k < 5; k++) { h[k] = g_vb_hist[k]; g_vb_hist[k] = 0; }
            port_log("video: frames on screen for 1/2/3/4+ vblanks: %u %u %u %u", h[0] + h[1], h[2], h[3], h[4]);
        }
#endif
        if (g_shc_n || g_shc_hit)
            port_log("shaders: %u compiled (vs %.1f ms, fs %.1f ms, link+lookups %.1f ms), %u from cache (%.1f ms)",
                     g_shc_n, g_shc_us[0] / 1000.0, g_shc_us[1] / 1000.0, g_shc_us[2] / 1000.0, g_shc_hit,
                     g_shc_us[3] / 1000.0);
        g_shc_n = g_shc_hit = 0;
        g_shc_us[0] = g_shc_us[1] = g_shc_us[2] = g_shc_us[3] = 0;
    }
}

/* frames per vblank of the display (1 = 60 Hz, 2 = the 30 fps lock;
 * port_os.c). vitaGL's display queue waits that many vblanks per frame. */
void port_video_swap_interval(int n) {
#ifdef PORT_VITA
    eglSwapInterval(0, n);
#else
    (void)n; /* PC: port_os.c sleeps instead */
#endif
}

/* render thread (or inline): screenshot + swap */
void port_video_present(int frame) {
    {
        extern void port_splash_free(int frame);
        port_splash_free(frame);
    }
    maybe_screenshot(frame);
#ifdef PORT_VITA
    vglSwapBuffers(GL_FALSE);
    if (g_gpufin < 0) g_gpufin = getenv("CWOC_GPUFIN") && getenv("CWOC_GPUFIN")[0] == '1';
    if (g_gpufin) {
        uint64_t t0 = port_prof_now();
        glFinish();
        g_gpufin_us += port_prof_now() - t0;
        g_gpufin_n++;
    }
#else
    SDL_GL_SwapWindow(g_window);
#endif
}

void port_video_gl_attach(void) {
#ifndef PORT_VITA
    if (SDL_GL_MakeCurrent(g_window, g_glctx) != 0) port_log("video: MakeCurrent: %s", SDL_GetError());
#endif
}

void port_video_gl_release(void) {
#ifndef PORT_VITA
    SDL_GL_MakeCurrent(g_window, NULL);
#endif
}

void DEMOSwapBuffers(void) {
    Uint32 now;
    static uint64_t prof_last;
    if (getenv("CWOC_SLEEP")) SDL_Delay(atoi(getenv("CWOC_SLEEP")));
    maybe_warp();
    {
        static int crelog = -1;
        extern void port_log_creatures(int frame);
        if (crelog < 0) crelog = getenv("CWOC_CRELOG") != NULL;
        if (crelog) port_log_creatures(g_frames);
        { extern void port_teleport(int frame); port_teleport(g_frames); }
    }
    port_gx_icons_commit(g_frames);
    port_gx_present(g_frames);
    {
        extern void port_audio_frame(void);
        port_audio_frame();
    }
    pump_events();
#ifdef PORT_VITA
    {
        /* CWOC_KEEPAWAKE=1 (benchmarks without input): reset the idle timers
         * once a second, as a button press would, so the screen neither dims
         * nor turns off (turning it back on brings up the lock screen) */
        static int keep = -1;
        if (keep < 0) keep = getenv("CWOC_KEEPAWAKE") != NULL;
        if (keep && g_frames % 60 == 0) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    }
#endif
    g_frames++;
    now = SDL_GetTicks();
    {
        /* loading shows up as one very long frame: say where the time went */
        static Uint32 last_swap;
        extern uint64_t port_io_us, port_io_bytes;
        extern int port_io_files;
        extern uint64_t port_io_open_us;
        static uint64_t io_o0;
        static uint64_t io_us0, io_b0;
        static int io_f0;
        static int thresh = -1;
        if (thresh < 0) thresh = getenv("CWOC_LONGFRAME") ? atoi(getenv("CWOC_LONGFRAME")) : 100;
        if (last_swap && now - last_swap > (Uint32)thresh)
            port_log("long frame %d: %u ms (file reads %.0f ms, %.1f MB, %d opens %.0f ms)", g_frames, now - last_swap,
                     (port_io_us - io_us0) / 1000.0, (port_io_bytes - io_b0) / 1048576.0, port_io_files - io_f0,
                     (port_io_open_us - io_o0) / 1000.0);
        io_us0 = port_io_us;
        io_b0 = port_io_bytes;
        io_f0 = port_io_files;
        io_o0 = port_io_open_us;
        last_swap = now;
    }
    if (now - g_last_report >= 5000) {
        static int last_frames;
        int nf = g_frames - last_frames;
        port_log("frame %d: %.1f fps, %d GX blocks/frame, %d GL draws/frame, %d verts/frame", g_frames,
                 nf * 1000.0 / (double)(now - g_last_report), g_draws / (nf ? nf : 1),
                 port_gx_take_draw_calls() / (nf ? nf : 1), g_verts / (nf ? nf : 1));
        {
            uint64_t t = port_prof_now();
            if (prof_last) {
                char cbuf[512];
                prof_report(nf, t - prof_last);
                if (census_report(cbuf, sizeof(cbuf), t - prof_last) > 0) port_log("%s", cbuf);
            } else {
                memset(g_prof, 0, sizeof(g_prof));
                census_report(NULL, 0, 0);
            }
            prof_last = t;
        }
        g_draws = g_verts = 0;
        last_frames = g_frames;
        g_last_report = now;
    }
    if (g_headless_frames > 0 && g_frames >= g_headless_frames) {
        port_gx_finish();
        port_log("reached --frames %d, exiting", g_headless_frames);
#ifdef PORT_VITA
        /* not exit(): newlib's cleanup closes every FILE while the SDL audio
         * thread still streams music from one (data abort in _lseek_r) */
        sceKernelExitProcess(0);
#endif
        exit(0);
    }
}

/* Debug text (fatal error screen): logged instead of drawn. */
void DEMOInitCaption(int font_type, int width, int height) { (void)font_type; (void)width; (void)height; }
void DEMOPrintf(short x, short y, short z, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    (void)x; (void)y; (void)z;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    port_log("screen: %s", buf);
}
void DEMOSetupScrnSpc(int width, int height, float depth) {
    float m[4][4];
    float id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    extern void C_MTXOrtho(float m[4][4], float t, float b, float l, float r, float n, float f);
    extern void GXSetProjection(const float m[4][4], int type);
    extern void GXLoadPosMtxImm(const float m[3][4], unsigned int id);
    extern void GXSetCurrentMtx(unsigned int id);
    C_MTXOrtho(m, 0.0f, (float)height, 0.0f, (float)width, 0.0f, -depth);
    GXSetProjection(m, 1);
    GXLoadPosMtxImm(id, 0);
    GXSetCurrentMtx(0);
}
