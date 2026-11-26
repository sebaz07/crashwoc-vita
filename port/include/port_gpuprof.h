/* GPU profiler capture (psp2-cex-gpues4-prof): only in a build configured
 * with -DCWOC_GPUPROF=ON (scripts/build_vita.sh with CWOC_GPUPROF=1). In the
 * normal build every hook below is an empty macro: no code, no data.
 *
 * CWOC_GPUPROF=<first>,<frames>[,<MB>] records the GPU work of presented
 * frames [first, first+frames) (MB: event buffer, default 24) and writes
 * ux0:data/crashwoc/gpuprof/capture.csv (tools/analyze.py), draws.csv (one
 * row per game draw, joined by the #id in each draw name) and shaders.txt. */
#ifndef PORT_GPUPROF_H
#define PORT_GPUPROF_H

#ifdef PORT_GPUPROF
#include <stdint.h>

/* one row per game draw of the capture window (render thread) */
typedef struct {
    uint32_t frame, nv, ni;
    uint16_t sh, tw, th;
    uint8_t tag, kind, mode, ntev;
    uint8_t blend, bsrc, bdst, zen, zfn, zup, cull, aupd;
    int16_t aref0, aref1;
    uint16_t sx, sy, sw, sh_;
} PortGpDraw;

enum { GP_DRAW_DYN = 1, GP_DRAW_STATIC = 2 };

void port_gpuprof_init(void);            /* before vglInit */
extern int g_gpuprof_tag;                /* game thread: pass of the commands being recorded */
extern int g_gpuprof_fx;                 /* game thread: the pending batch has native particle quads */
extern volatile int g_gpuprof_live;      /* render thread: capture window open */
void port_gpuprof_msub(int k, int on);   /* game thread: material pass sub-timer slots */
void port_gpuprof_hud(void);             /* game thread: HUD scene starts */
void port_gpuprof_frame_begin(void);     /* game thread: a new frame's commands start */
/* render thread */
void port_gpuprof_scope(int tag);        /* the pass of the next commands */
int port_gpuprof_skip(int tag);          /* CWOC_GPUPROF_NOFX=1: leave out particle batches (A/B screenshots) */
void port_gpuprof_name(const char *name);
PortGpDraw *port_gpuprof_draw(void);     /* next row (names the draw "#id"); NULL when full */
void port_gpuprof_shader(int sh);        /* shader of the next GL draw (names new GXM programs) */
void port_gpuprof_present(int frame);    /* after the swap: opens/closes the window */
void port_gpuprof_dump_shaders(void *file); /* gx_gl.c: source of every shader */

#define GP_TAG_SET(c) ((c)->gp = g_gpuprof_tag | g_gpuprof_fx << 5)
#define GP_MSUB(k, on) port_gpuprof_msub(k, on)
#define GP_HUD() port_gpuprof_hud()
#define GP_FRAME_BEGIN() port_gpuprof_frame_begin()
#define GP_LIVE g_gpuprof_live
#else
#define GP_TAG_SET(c) ((void)0)
#define GP_MSUB(k, on) ((void)0)
#define GP_HUD() ((void)0)
#define GP_FRAME_BEGIN() ((void)0)
#define GP_LIVE 0
#endif

#endif
