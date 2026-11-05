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
 * time (accumulator). CWOC_PACESLEEP=1 also holds every frame to a whole
 * number n of 1/60 s (n from the frame cost minus render-thread waits).
 * CWOC_FIXEDTIME / CWOC_NOPACE=1: original behaviour. */
static uint64_t pace_now_us(void) {
    return (uint64_t)((double)(SDL_GetPerformanceCounter() - g_perf_base) * 1000000.0 / (double)g_perf_freq);
}

static void pace_sleep_us(uint64_t us) {
#ifdef PORT_VITA
    extern int sceKernelDelayThread(unsigned int usec);
    sceKernelDelayThread((unsigned int)us);
#else
    SDL_Delay((Uint32)((us + 500) / 1000));
#endif
}

static int g_pace_lock = -1;

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
    port_log("video: frame lock %s", g_pace_lock ? "on" : "off");
}

/* Vita: SELECT + D-pad down (port_pad.c) */
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
    port_log("video: frame lock %s", g_pace_lock ? "on" : "off");
}

double port_frame_pacer(double measured_fps) {
    static int mode = -1, n = 1, below;
    static uint64_t last;
    static double ema, owed;
    static const double fps_for_ticks[6] = { 60.0, 60.0, 40.0, 24.0, 16.0, 12.0 };
    const double period = 1000000.0 / 60.0;
    uint64_t now, work, busy;
    int want, ticks;
    if (mode < 0) {
        mode = !(getenv("CWOC_FIXEDTIME") || getenv("CWOC_NOPACE"));
        port_log("video: frame pacing %s", mode ? "on" : "off");
    }
    if (!mode) return measured_fps;
    now = pace_now_us();
    if (!last || now - last > 250000) { /* first frame or a load: start over */
        last = now;
        owed = 0.0;
        return 60.0;
    }
    work = now - last;
    { /* frame cost = elapsed minus the time blocked on the render thread
       * (vsync, previous frame): a frame that only waits is not expensive */
        extern uint64_t g_port_wait_us;
        static uint64_t wait_last;
        uint64_t waited = g_port_wait_us - wait_last;
        wait_last = g_port_wait_us;
        busy = work > waited ? work - waited : 0;
    }
    if (ema == 0.0) ema = (double)busy;
    else if ((double)busy > ema) ema += 0.3 * ((double)busy - ema);
    else ema += 0.1 * ((double)busy - ema);
    want = (int)((ema * 1.02 + 500.0) / period) + 1;
    if (want > 5) want = 5;
    if (want > n) { n = want; below = 0; }
    else if (want < n) { if (++below > 20) { n = want; below = 0; } }
    else below = 0;
    /* frame lock: sleeping to whole vblank multiples evens the display rhythm
     * but costs some throughput: opt-in (CWOC_PACESLEEP=1, or SELECT + D-pad
     * down on the Vita, saved in <save dir>/framelock.txt) */
    if (g_pace_lock < 0) pace_lock_load();
    if (g_pace_lock && (double)work < n * period - 300.0)
        pace_sleep_us((uint64_t)(n * period - (double)work - 300.0));
    now = pace_now_us();
    owed += (double)(now - last) / period;
    last = now;
    ticks = (int)(owed + 0.5);
    if (ticks < 1) ticks = 1;
    if (ticks > 5) ticks = 5;
    owed -= ticks;
    if (owed > 2.0) owed = 2.0;
    if (owed < -2.0) owed = -2.0;
    {
        static int plog = -1;
        if (plog < 0) plog = getenv("CWOC_PACELOG") != NULL;
        if (plog) port_log("pace: work %.1f ms, busy %.1f ms, n %d, ticks %d, owed %.2f", work / 1000.0, busy / 1000.0, n, ticks, owed);
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
