/* Dolphin OS / PPC / Win32-Xbox leftovers used by the decompiled code. */
#include "port.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long long s64_t;

/* GameCube bus clock based timebase: 162 MHz bus / 4 = 40.5 MHz tick. */
#define GC_TIMER_HZ 40500000ULL

static Uint64 g_perf_freq, g_perf_base;

void port_os_init(void) {
    g_perf_freq = SDL_GetPerformanceFrequency();
    g_perf_base = SDL_GetPerformanceCounter();
}

static unsigned long long gc_ticks(void) {
    static int fixed = -1;
    Uint64 now;
    if (fixed < 0) fixed = getenv("CWOC_FIXEDTIME") != NULL;
    if (fixed) {
        /* deterministic tests: the clock advances 1/60 s per presented frame
         * (plus a little per call so busy-wait loops still finish) */
        extern int port_frame_count(void);
        static int last_frame = -1;
        static unsigned long long calls;
        int f = port_frame_count();
        if (f != last_frame) { last_frame = f; calls = 0; }
        if (calls < GC_TIMER_HZ / 60 / 64 - 1) calls++;
        return (unsigned long long)f * (GC_TIMER_HZ / 60) + calls * 64;
    }
    now = SDL_GetPerformanceCounter() - g_perf_base;
    return (unsigned long long)((double)now * (double)GC_TIMER_HZ / (double)g_perf_freq);
}

/* Frame pacing (called by TimerGetFPS once per presented frame; the result is
 * the game's cufps, which picks how many 1/60 s logic ticks the next frame
 * runs: >=59 fps 1, >=29 2, >=19 3, >=14 4, else 5). The original maps the
 * instantaneous fps through those thresholds, so at 15-20 fps the game time
 * per frame is out of proportion with the real time (e.g. 54 ms -> 4 ticks =
 * 67 ms) and motion judders. Here the tick count follows the real elapsed
 * time (accumulator). CWOC_FIXEDTIME / CWOC_NOPACE=1: original behaviour.
 *
 * The 30 fps lock (pause > OPTIONS > FRAME RATE: 30 FPS, saved in
 * <save dir>/framelock.txt; CWOC_PACESLEEP=1 when there is no such file): the
 * display queue shows every frame for 2 vblanks (vitaGL swap interval 2) and
 * the game runs 2 ticks a frame, more only when it really falls behind (a
 * frame of 3 periods or more). The render thread waits for the display queue
 * and the game thread for the render thread, so nothing sleeps here. Before,
 * the game thread slept to a whole number of periods chosen from the frame
 * cost (60, 30 or 20 fps) with no tie to the vblank: frames reached the
 * screen 1 or 3 vblanks apart, and scenes that keep 60 stayed at 60. */
static uint64_t pace_now_us(void) {
    return (uint64_t)((double)(SDL_GetPerformanceCounter() - g_perf_base) * 1000000.0 / (double)g_perf_freq);
}

#ifndef PORT_VITA
static void pace_sleep_us(uint64_t us) { SDL_Delay((Uint32)((us + 500) / 1000)); }
#endif

static int g_pace_lock = -1;

/* the display queue's swap interval: 2 vblanks a frame with the lock (port_video.c) */
static void pace_lock_apply(void) {
    extern void port_video_swap_interval(int n);
    port_video_swap_interval(g_pace_lock ? 2 : 1);
    port_log("video: frame rate %s", g_pace_lock ? "30 fps" : "unlocked");
}

static void pace_lock_path(char *buf, size_t n) {
    extern const char *port_save_dir(void);
    snprintf(buf, n, "%s/framelock.txt", port_save_dir());
}

static void pace_lock_load(void) {
    char path[512];
    FILE *f;
    g_pace_lock = getenv("CWOC_PACESLEEP") != NULL;
    pace_lock_path(path, sizeof(path));
    f = fopen(path, "r");
    if (f) {
        int v = 0;
        if (fscanf(f, "%d", &v) == 1) g_pace_lock = v != 0;
        fclose(f);
    }
    pace_lock_apply();
}

int port_pace_lock_on(void) {
    if (g_pace_lock < 0) pace_lock_load();
    return g_pace_lock;
}

/* pause menu > OPTIONS (port_hooks.c) */
void port_pace_lock_toggle(void) {
    char path[512];
    FILE *f;
    if (g_pace_lock < 0) pace_lock_load();
    g_pace_lock = !g_pace_lock;
    pace_lock_path(path, sizeof(path));
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", g_pace_lock);
        fclose(f);
    }
    pace_lock_apply();
}

double port_frame_pacer(double measured_fps) {
    static int mode = -1;
    static uint64_t last;
    static double owed;
    static const double fps_for_ticks[6] = { 60.0, 60.0, 40.0, 24.0, 16.0, 12.0 };
    const double period = 1000000.0 / 60.0;
    uint64_t now;
    int ticks;
    if (mode < 0) {
        mode = !(getenv("CWOC_FIXEDTIME") || getenv("CWOC_NOPACE"));
        port_log("video: frame pacing %s", mode ? "on" : "off");
    }
    if (!mode) return measured_fps;
    if (g_pace_lock < 0) pace_lock_load();
    now = pace_now_us();
    if (!last || now - last > 250000) { /* first frame or a load: start over */
        last = now;
        owed = 0.0;
        return fps_for_ticks[g_pace_lock ? 2 : 1];
    }
#ifndef PORT_VITA
    /* no swap interval here: hold the frame to 2 periods */
    if (g_pace_lock && (double)(now - last) < 2.0 * period - 300.0) {
        pace_sleep_us((uint64_t)(2.0 * period - (double)(now - last) - 300.0));
        now = pace_now_us();
    }
#endif
    owed += (double)(now - last) / period;
    last = now;
    /* locked: 2 ticks a frame (a frame that reached the render thread early
     * or late, around the steady 2 vblanks, is still 2: owed absorbs it) */
    ticks = g_pace_lock && owed < 2.5 ? 2 : (int)(owed + 0.5);
    if (ticks < 1) ticks = 1;
    if (ticks > 5) ticks = 5;
    owed -= ticks;
    if (owed > 2.0) owed = 2.0;
    if (owed < -2.0) owed = -2.0;
    {
        static int plog = -1;
        if (plog < 0) plog = getenv("CWOC_PACELOG") != NULL;
        if (plog) port_log("pace: ticks %d, owed %.2f", ticks, owed);
    }
    return fps_for_ticks[ticks];
}

s64_t OSGetTime(void) { return (s64_t)gc_ticks(); }
unsigned int OSGetTick(void) { return (unsigned int)gc_ticks(); }
int OSDisableInterrupts(void) { return 1; }
int OSRestoreInterrupts(int level) { (void)level; return 1; }
int OSEnableInterrupts(void) { return 1; }
void OSRegisterResetFunction(void *info) { (void)info; }
void OSSetWirelessID(int chan, unsigned short id) { (void)chan; (void)id; }
void OSClearContext(void *ctx) { (void)ctx; }
void OSSetCurrentContext(void *ctx) { (void)ctx; }
void PPCMtmmcr0(unsigned int v) { (void)v; }
void PPCMtmmcr1(unsigned int v) { (void)v; }
void OSReport(const char *fmt, ...) { (void)fmt; }

void _assert(const char *expr, const char *file, int line) {
    port_log("assert: %s at %s:%d", expr ? expr : "?", file ? file : "?", line);
}

unsigned int __cntlzw(unsigned int v) { return v ? (unsigned int)__builtin_clz(v) : 32u; }

char *strlwr(char *s) {
    char *p;
    for (p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
    return s;
}

/* Win32/Xbox API leftovers referenced by the shared TT engine code. */
unsigned int GetTickCount(void) { return SDL_GetTicks(); }
void Sleep(unsigned int ms) { SDL_Delay(ms); }
int GetLastError(void) { return 0; }
int SetThreadPriority(int thread, int prio) { (void)thread; (void)prio; return 1; }
int XbGetNumControllers(void) { return 1; }
void XbWaitForAllBuffersToStop(void) {}
void XbLaunchDashboardToFreeSpace(void) { PORT_STUB(); }

/* The cut-scene loader starts a loading-screen thread. Running it would need
 * the renderer to be thread safe; the port simply skips it for now. */
void *CreateThread(void *attr, unsigned int stack, int proc, void *param, unsigned int flags, void *id) {
    (void)attr; (void)stack; (void)proc; (void)param; (void)flags; (void)id;
    PORT_STUB();
    return NULL;
}

/* VBlank interrupt control used around error display. */
void NuDisableVBlankE(void) {}
void NuEnableVBlankE(void) {}

/* Console reset handling: nothing to do on PC/Vita. */
void Reseter(int a) { (void)a; }

/* Memory card / GBA hardware initialisation: not applicable. */
void GBA_Init(void) {}
