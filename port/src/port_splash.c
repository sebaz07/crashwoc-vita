/* Vita boot splash (personal builds): shows app0:splash.rgba (960x544 RGBA,
 * packaged from CWOC_LIVEAREA_DIR, see CMakeLists.txt) on the display before
 * vitaGL starts, keeps it up for a moment, and frees it once the game draws. */
#include "port.h"

#ifdef PORT_VITA
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>

#define SPLASH_W 960
#define SPLASH_H 544
#define SPLASH_MIN_US 2000000ull

static SceUID g_splash_blk = -1;
static unsigned long long g_splash_t0;

void port_splash_show(void) {
    SceDisplayFrameBuf fb;
    void *base = NULL;
    size_t size = SPLASH_W * SPLASH_H * 4, got;
    FILE *f = fopen("app0:splash.rgba", "rb");
    if (!f) return;
    g_splash_blk = sceKernelAllocMemBlock("splash", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                          (size + 0x3FFFF) & ~0x3FFFFu, NULL);
    if (g_splash_blk < 0 || sceKernelGetMemBlockBase(g_splash_blk, &base) < 0) {
        fclose(f);
        g_splash_blk = -1;
        return;
    }
    got = fread(base, 1, size, f);
    fclose(f);
    if (got != size) {
        sceKernelFreeMemBlock(g_splash_blk);
        g_splash_blk = -1;
        return;
    }
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = base;
    fb.pitch = SPLASH_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = SPLASH_W;
    fb.height = SPLASH_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    g_splash_t0 = sceKernelGetProcessTimeWide();
    port_log("splash: shown");
}

/* before vitaGL takes the display: make sure the splash was up long enough */
void port_splash_hold(void) {
    unsigned long long el;
    if (g_splash_blk < 0) return;
    el = sceKernelGetProcessTimeWide() - g_splash_t0;
    if (el < SPLASH_MIN_US) sceKernelDelayThread((unsigned int)(SPLASH_MIN_US - el));
}

/* once the game has presented a few frames the display no longer reads it */
void port_splash_free(int frame) {
    if (g_splash_blk < 0 || frame < 30) return;
    sceKernelFreeMemBlock(g_splash_blk);
    g_splash_blk = -1;
}
#else
void port_splash_show(void) {}
void port_splash_hold(void) {}
void port_splash_free(int frame) { (void)frame; }
#endif
