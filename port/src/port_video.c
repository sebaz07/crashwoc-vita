/* Window, GL context, frame pacing and the Dolphin DEMO library entry points
 * the game uses (DEMOInit/BeforeRender/DoneRender/SwapBuffers, debug text). */
#include "port.h"
#include "port_gl.h"
#include "port_prof.h"
#ifdef PORT_VITA
#include <psp2/kernel/processmgr.h>
#endif
#include <SDL2/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void port_display_cycle(void) {
    char path[512];
    FILE *f;
    g_display_mode = (g_display_mode + 1) % 3;
    apply_drawable();
    display_path(path, sizeof(path));
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", g_display_names[g_display_mode]);
        fclose(f);
    }
    port_log("video: display mode %s", g_display_names[g_display_mode]);
}

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
    vglInitExtended(0, w, h, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE); /* MSAA off: fill rate */
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
    port_gx_init();
    PADInit();
    g_last_report = SDL_GetTicks();
}

/* sampled sub-timers for the material pass (numtl.c / nurndr.c MSUB_*) */
uint64_t g_msub[8];
static uint64_t g_msub_t0[8];
static unsigned g_msub_c[8], g_msub_r[8];
static int g_msub_on = -1;
void port_msub_begin(int k, int rate) {
    /* off by default (each sample is two timer syscalls on the Vita):
     * CWOC_MSUB=1 on PC, or build with -DPORT_MSUB_DEFAULT=1 */
    if (g_msub_on < 0) {
#ifdef PORT_MSUB_DEFAULT
        g_msub_on = PORT_MSUB_DEFAULT;
#else
        g_msub_on = getenv("CWOC_MSUB") != NULL;
#endif
    }
    if (!g_msub_on) { g_msub_t0[k] = 0; return; }
    g_msub_r[k] = (unsigned)rate;
    g_msub_t0[k] = (++g_msub_c[k] % (unsigned)rate) == 0 ? port_prof_now() : 0;
}
void port_msub_end(int k) {
    if (g_msub_t0[k]) g_msub[k] += (port_prof_now() - g_msub_t0[k]) * g_msub_r[k];
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
    return;
#else
    SDL_GL_GetDrawableSize(g_window, &w, &h);
    px = malloc((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    snprintf(path, sizeof(path), "%s/frame_%05d.ppm", dir, frame);
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
        fclose(f);
    }
    free(px);
#endif
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

static void prof_report(int nf, uint64_t span) {
    static const char *names[PROF_N] = {"block", "capture", "verts", "shader", "texhash", "texdecode", "wait",
                                        "rt_draw", "rt_tex", "rt_copy", "rt_swap", "rt_xf"};
    char buf[1024];
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
            if (g_msub_on > 0) {
                static const char *mn[8] = {"m3d", "mstate", "wmtx", "shsetup", "draw", "ot", "other", "item"};
                n += snprintf(buf + n, sizeof(buf) - n, " | mtl:");
                for (i = 0; i < 8; i++) {
                    n += snprintf(buf + n, sizeof(buf) - n, " %s %.1f", mn[i], g_msub[i] / 1000.0 / nf);
                    g_msub[i] = 0;
                }
            }
        }
        for (i = 0; i < 40; i++) port_tb_us[i] = 0, port_tb_n[i] = 0;
        port_log("%s", buf);
    }
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
    port_gx_present(g_frames);
    {
        extern void port_audio_frame(void);
        port_audio_frame();
    }
    pump_events();
    g_frames++;
    now = SDL_GetTicks();
    {
        /* loading shows up as one very long frame: say where the time went */
        static Uint32 last_swap;
        extern uint64_t port_io_us, port_io_bytes;
        extern int port_io_files;
        static uint64_t io_us0, io_b0;
        static int io_f0;
        static int thresh = -1;
        if (thresh < 0) thresh = getenv("CWOC_LONGFRAME") ? atoi(getenv("CWOC_LONGFRAME")) : 500;
        if (last_swap && now - last_swap > (Uint32)thresh)
            port_log("long frame %d: %u ms (file reads %.0f ms, %.1f MB, %d opens)", g_frames, now - last_swap,
                     (port_io_us - io_us0) / 1000.0, (port_io_bytes - io_b0) / 1048576.0, port_io_files - io_f0);
        io_us0 = port_io_us;
        io_b0 = port_io_bytes;
        io_f0 = port_io_files;
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
            if (prof_last) prof_report(nf, t - prof_last);
            else memset(g_prof, 0, sizeof(g_prof));
            prof_last = t;
        }
        g_draws = g_verts = 0;
        last_frames = g_frames;
        g_last_report = now;
    }
    if (g_headless_frames > 0 && g_frames >= g_headless_frames) {
        port_gx_finish();
        port_log("reached --frames %d, exiting", g_headless_frames);
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
