/* Native port entry point: platform bring-up, then the game's own main(). */
#include "port.h"
#include <stdlib.h>

/* Developer option: CWOC_SKIP_LOGOS=1 skips the licence/publisher screens. */
int port_skip_logos(void) {
    const char *v = getenv("CWOC_SKIP_LOGOS");
    return v && v[0] == '1';
}

#ifdef PORT_VITA
#include <psp2/power.h>
/* newlib heap for the game (GameCube had 24 MB; the port also keeps decoded
 * textures and translated-code stacks in main RAM). vitaGL uses CDRAM. */
int _newlib_heap_size_user = 128 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
#endif

int cwoc_game_main(int argc, char **argv);
void port_dol_data_init(void);

/* The intro/title "cut movie" (logos, title screen with its menu). Developer
 * option: CWOC_INTRO=0 skips it and goes straight to the hub. */
int port_skip_intro(void) {
    const char *intro = getenv("CWOC_INTRO");
    int skip = intro && intro[0] == '0';
    if (skip) port_log("skipping intro cut movie (CWOC_INTRO=0)");
    return skip;
}

#ifdef PORT_VITA
/* vita-elf-create appends ~4 KB of module data to the end of the text segment
 * and fails ("segment 1 overlaps") when the text happens to end just below
 * the 64 KB boundary where the data segment starts. This read-only pad moves
 * that boundary; change PORT_VITA_TEXT_PAD (CMake) if the error comes back. */
#ifndef PORT_VITA_TEXT_PAD
#define PORT_VITA_TEXT_PAD 4096
#endif
const unsigned char port_vita_text_pad[PORT_VITA_TEXT_PAD] = {1};
#endif

int main(int argc, char **argv) {
#ifdef PORT_VITA
    (void)*(volatile const unsigned char *)port_vita_text_pad;
    /* run at the highest standard clocks (default ARM clock is 333 MHz) */
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
#endif
    port_log_init();
    port_log("Crash WOC native port starting");
    {
        extern void port_splash_show(void);
        port_splash_show(); /* Vita: boot image while the port starts */
    }
    /* Flush denormals to zero (GameCube games typically run the FPU in
     * non-IEEE mode). Game data contains denormal floats, and handling them
     * at full IEEE precision made the vertex path ~4x slower. */
#if defined(__SSE__) && !defined(PORT_VITA)
    if (!getenv("CWOC_IEEE")) {
        unsigned csr;
        __asm__ volatile("stmxcsr %0" : "=m"(csr));
        csr |= 0x8040; /* FTZ + DAZ */
        __asm__ volatile("ldmxcsr %0" : : "m"(csr));
    }
#elif defined(__arm__)
    {
        unsigned fpscr;
        __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
        fpscr |= 1u << 24; /* FZ */
        __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
    }
#endif
    port_dol_data_init();
    port_platform_init(argc, argv);
    {
        extern void port_audio_init(void);
        port_audio_init(); /* after SDL video init (port_platform_init) */
    }

    return cwoc_game_main(argc, argv);
}
