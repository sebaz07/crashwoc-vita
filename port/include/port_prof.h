/* Lightweight per-stage frame profiler (logged with the fps line). */
#ifndef PORT_PROF_H
#define PORT_PROF_H
#include <stdint.h>

enum {
    /* game thread */
    PROF_DECODE,   /* GX block bookkeeping (state compare, indices, batching) */
    PROF_CAPTURE,  /* batch state snapshot + compare */
    PROF_VERTS,    /* vertex decode + transform */
    PROF_SHADER,   /* shader lookup / source generation */
    PROF_TEXHASH,  /* texture cache validation (hashing) */
    PROF_TEXLOAD,  /* texture decode (to RGBA) */
    PROF_WAIT,     /* waiting for the render thread */
    PROF_GAME_N,
    /* render thread (or inline) */
    PROF_RT_DRAW = PROF_GAME_N, /* GL state, uniforms, draw calls */
    PROF_RT_TEX,   /* texture uploads */
    PROF_RT_COPY,  /* EFB copies (glReadPixels) */
    PROF_RT_SWAP,  /* present (waits for the GPU) */
    PROF_N
};

uint64_t port_prof_now(void);           /* microseconds */
extern uint64_t g_prof[PROF_N];
extern uint32_t g_prof_cnt[PROF_N];

extern uint64_t g_prof_child;
extern uint64_t g_prof_ovh;   /* measured cost of one timer read pair */
void port_prof_calibrate(void);
extern uint32_t g_prof_fast, g_prof_slow; /* decode fast-path / generic blocks */
/* exclusive times: nested sections are subtracted from their parent */
#define PROF_BEGIN(id) uint64_t _prof_t_##id = port_prof_now(), _prof_c_##id = g_prof_child; g_prof_child = 0
#define PROF_END(id) do { uint64_t _el = port_prof_now() - _prof_t_##id; _el = _el > g_prof_ovh ? _el - g_prof_ovh : 0; \
    g_prof[id] += _el - g_prof_child; g_prof_cnt[id]++; g_prof_child = _prof_c_##id + _el; } while (0)

/* sampled variant for sections entered thousands of times per frame (the
 * timer is a syscall on Vita): time one call in `rate`, scale the result */
#define PROF_BEGIN_S(id, rate) static unsigned _ps_##id; int _pon_##id = (++_ps_##id % (rate)) == 0; \
    uint64_t _prof_t_##id = _pon_##id ? port_prof_now() : 0, _prof_c_##id = g_prof_child; g_prof_child = 0
#define PROF_END_S(id, rate) do { if (_pon_##id) { uint64_t _el = port_prof_now() - _prof_t_##id; _el = _el > g_prof_ovh ? _el - g_prof_ovh : 0; \
    g_prof[id] += (_el - g_prof_child) * (rate); g_prof_cnt[id] += (rate); g_prof_child = _prof_c_##id + _el; } \
    else { g_prof_child = _prof_c_##id + g_prof_child; } } while (0)

#endif
