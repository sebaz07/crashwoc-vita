/* GameCube GX emulation on OpenGL (desktop GL 2.1 / vitaGL).
 *
 * The decompiled renderer (src/system/gs/*.c, matched against the DOL) drives
 * the GX API directly. This file implements the subset it uses:
 *
 *  - a byte FIFO fed by GXPosition/GXColor/... writes, decoded per vertex
 *    according to the current vertex descriptor and attribute formats (the
 *    game sometimes writes an index through GXColor1u16, exactly like the FIFO
 *    on hardware, so writes are positional, not typed by function name);
 *  - position/normal matrix memory, projection (GX z range [-w,0] converted
 *    to GL [-w,w] in the vertex shader);
 *  - colour channel lighting (COLOR0/ALPHA0, up to 8 lights, per vertex);
 *  - the TEV combiner (generated GLSL per configuration, cached);
 *  - alpha compare, linear fog, blend/z/cull state;
 *  - texture objects decoded from GameCube formats (I4/I8/IA4/IA8/RGB565/
 *    RGB5A3/RGBA8/CMPR) into RGBA8 GL textures, cached by address/contents;
 *  - EFB copies to textures (pause screen) via glReadPixels.
 */
#include "port.h"
#include "port_gl.h"
#include "port_prof.h"
#include "port_gpuprof.h"
#include <SDL2/SDL.h>
/* GX thread (gx_rec.c, included at the end): inside this file the public GX
 * entry points are the implementations, renamed gxi_<name> */
#define GX_REC_NAMES
#include "gx_rec_gen.h"
#undef GX_REC_NAMES
#ifdef PORT_VITA
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <arm_neon.h>
#include <sys/stat.h>
#endif
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef short s16;
typedef float f32;

typedef struct { u8 r, g, b, a; } GXColor;

/* ------------------------------------------------------------------ enums */
enum { GX_VA_PNMTXIDX = 0, GX_VA_TEX0MTXIDX = 1, GX_VA_POS = 9, GX_VA_NRM = 10, GX_VA_CLR0 = 11,
       GX_VA_CLR1 = 12, GX_VA_TEX0 = 13, GX_VA_MAX_ATTR = 21 };
enum { GX_NONE = 0, GX_DIRECT = 1, GX_INDEX8 = 2, GX_INDEX16 = 3 };
enum { GX_U8 = 0, GX_S8 = 1, GX_U16 = 2, GX_S16 = 3, GX_F32 = 4 };
enum { GX_RGB565 = 0, GX_RGB8 = 1, GX_RGBX8 = 2, GX_RGBA4 = 3, GX_RGBA6 = 4, GX_RGBA8 = 5 };

#define MAX_VTXFMT 8
#define MAX_POSMTX 10
#define MAX_TEV 4
#define MAX_TEXMAP 8


/* CWOC_GXOUT=1: count, per function, the GX calls made outside the engine's
 * material pass (NuMtlRender, flagged by port_in_mtlrender in nurndr.c):
 * what would race with a material pass running on another thread */
int port_in_mtlrender;
static int g_gxo_on = -1;
static const char *g_gxo_name[96];
static unsigned g_gxo_cnt[96];
static int g_gxo_n;
static void gxo_hit(const char *fn) {
    int k;
    for (k = 0; k < g_gxo_n; k++)
        if (g_gxo_name[k] == fn) { g_gxo_cnt[k]++; return; }
    if (g_gxo_n < 96) { g_gxo_name[g_gxo_n] = fn; g_gxo_cnt[g_gxo_n++] = 1; }
}
#define GXO() do { if (g_gxo_on > 0 && !port_in_mtlrender) gxo_hit(__func__); } while (0)
void port_gxo_report(int nf) {
    char buf[1024];
    int k, n = 0;
    if (g_gxo_on <= 0 || nf <= 0) return;
    n += snprintf(buf + n, sizeof(buf) - n, "gx outside mtlrender per frame:");
    for (k = 0; k < g_gxo_n && n < (int)sizeof(buf) - 40; k++) {
        if (g_gxo_cnt[k]) n += snprintf(buf + n, sizeof(buf) - n, " %s %.1f", g_gxo_name[k], g_gxo_cnt[k] / (double)nf);
        g_gxo_cnt[k] = 0;
    }
    port_log("%s", buf);
}
/* -------------------------------------------------------------- state */
typedef struct { int cnt, type, frac; } AttrFmt;

typedef struct {
    float pos[3];
    float nrm[3];
    u8 clr[4];
    float tex[2];
} GLVertex;

typedef struct {
    int enable, amb_src, mat_src, light_mask, diff_fn, attn_fn;
} ChanCtrl;

typedef struct {
    float pos[3];
    float dir[3];
    GXColor color;
} LightSlot;

/* Layout stored inside the opaque 64-byte GXLightObj. */
typedef struct {
    float pos[3];
    float dir[3];
    GXColor color;
    u32 magic;
} PortLightObj;

/* Layout stored inside the opaque 32-byte GXTexObj. */
typedef struct {
    void *image;
    u16 width, height;
    u8 format, wrap_s, wrap_t, mipmap;
    u32 magic;
    u32 pad[4];
} PortTexObj;

#define TEXOBJ_MAGIC 0x54584F42u
#define LIGHTOBJ_MAGIC 0x4C494754u

typedef struct {
    int color_in[4], alpha_in[4];
    int color_op, alpha_op, color_bias, alpha_bias, color_scale, alpha_scale;
    int color_clamp, alpha_clamp, color_reg, alpha_reg;
    int texcoord, texmap, channel;
} TevStage;

static struct {
    int vtxdesc[GX_VA_MAX_ATTR];
    AttrFmt fmt[MAX_VTXFMT][GX_VA_MAX_ATTR];
    const u8 *array_base[GX_VA_MAX_ATTR];
    int array_stride[GX_VA_MAX_ATTR];

    float posmtx[MAX_POSMTX][3][4];
    float nrmmtx[MAX_POSMTX][3][3];
    int cur_posmtx;
    float proj[4][4];
    int proj_ortho;

    ChanCtrl chan[2]; /* 0 = COLOR0, 1 = ALPHA0 */
    GXColor amb_color, mat_color;
    int num_chans;
    LightSlot lights[8];

    TevStage tev[16];
    int num_tev, num_texgens;
    PortTexObj texmap[MAX_TEXMAP];
    int texmap_valid[MAX_TEXMAP];

    int alpha_comp0, alpha_ref0, alpha_op, alpha_comp1, alpha_ref1;
    int fog_type;
    float fog_start, fog_end;
    GXColor fog_color;

    int blend_type, blend_src, blend_dst;
    int z_enable, z_func, z_update;
    int cull;
    int alpha_update;

    float vp[6];
    int scissor[4];
    GXColor copy_clear;

    /* texture copy */
    int copy_src[4];
    int copy_dst_w, copy_dst_h, copy_fmt, copy_mip;
} gx;

/* FIFO for the current GXBegin block */
static u8 g_fifo[(65535 * 64) + 4096] __attribute__((aligned(64))); /* one GXBegin block of up to 65535 vertices */
static int g_fifo_len;
static int g_dirty = 1;
static int g_dirty_chan;         /* only the colour channels changed (see GXSetChanCtrl) */
static ChanCtrl g_chan_snap[2];  /* channels at the last batch-state capture */   /* a GX setter ran since the last batch-state capture */
static int g_prim, g_vtxfmt, g_nverts, g_in_begin;

/* window mapping (EFB 640x448 -> drawable) */
int port_frame_count(void);
static int g_trace, g_trace_from;
/* debug: port_gx_trace_scope(1/0) brackets code whose GX output should be
 * traced (CWOC_GXTRACE_SCOPE=n limits it to n records) */
static int g_trace_scope, g_trace_scope_left = -1;
void port_gx_trace_later(int frames_ahead, int records) {
    if (!getenv("CWOC_GXTRACE_HOOK")) return;
    g_trace_from = port_frame_count() + frames_ahead;
    g_trace = records;
}
void port_gx_trace_scope(int on) {
    if (g_trace_scope_left < 0)
        g_trace_scope_left = getenv("CWOC_GXTRACE_SCOPE") ? atoi(getenv("CWOC_GXTRACE_SCOPE")) : 0;
    g_trace_scope = on && g_trace_scope_left > 0;
}
#define TRACING() ((g_trace > 0 && port_frame_count() >= g_trace_from) || (g_trace_scope && g_trace_scope_left-- > 0))
int port_frame_count(void);
static int g_win_w = 640, g_win_h = 448;
static int g_off_x, g_off_y, g_draw_w = 640, g_draw_h = 448;

void port_gx_set_drawable(int win_w, int win_h, int off_x, int off_y, int draw_w, int draw_h) {
    g_win_w = win_w; g_win_h = win_h;
    g_off_x = off_x; g_off_y = off_y;
    g_draw_w = draw_w; g_draw_h = draw_h;
}

/* -------------------------------------------------------------- FIFO writes */
/* Vertex data written with GXPosition* & co. is kept in native byte order. The
 * layout of a vertex is fixed at GXBegin (see build_plan), so the end of a
 * block is a plain pointer check. gsprim.c uses inline copies of these
 * writers (include/port_gx_inline.h). */
static int g_need;          /* bytes for the current GXBegin block */
static int g_vs;            /* vertex size of the current block */
unsigned char *port_gx_wp = g_fifo;
unsigned char *port_gx_end = g_fifo; /* outside a block every write reaches port_gx_fifo_end */

static void gx_block_done(void);

static int g_rec_on;      /* GX thread running (gx_rec.c) */
static int g_rec_active;  /* game thread: GX calls are being recorded (static pass) */
static SDL_threadID g_rec_tid;
/* where each thread is waiting, for the stall watchdog (gx_rec.inc) */
static volatile const char *g_ws_game = "-", *g_ws_rt = "-", *g_ws_gx = "-";
#define WS_SET(s) do { if (g_rec_tid && SDL_ThreadID() == g_rec_tid) g_ws_gx = (s); else g_ws_game = (s); } while (0)
static int g_rec_bypass;  /* replaying a copied draw: no index cache, no deferred copy */
static void rec_fifo_end(void);
static void rec_init(void);

void port_gx_fifo_end(void) {
    if (g_rec_active) { /* game thread, recording: the block lives in its record */
        rec_fifo_end();
        return;
    }
    if (g_in_begin) {
        g_fifo_len = (int)(port_gx_wp - g_fifo);
        if (g_fifo_len >= g_need) gx_block_done();
    } else {
        port_gx_wp = g_fifo; /* data outside GXBegin/GXEnd: drop */
    }
}

#define PUT(p, n) do { memcpy(port_gx_wp, (p), (n)); port_gx_wp += (n); \
    if (port_gx_wp >= port_gx_end) port_gx_fifo_end(); } while (0)

void GXPosition3f32(f32 x, f32 y, f32 z) { f32 v[3] = { x, y, z }; PUT(v, 12); }
void GXPosition2f32(f32 x, f32 y) { f32 v[2] = { x, y }; PUT(v, 8); }
void GXPosition3s16(s16 x, s16 y, s16 z) { s16 v[3] = { x, y, z }; PUT(v, 6); }
void GXPosition1x16(u16 i) { PUT(&i, 2); }
void GXPosition1x8(u8 i) { PUT(&i, 1); }
void GXNormal3f32(f32 x, f32 y, f32 z) { f32 v[3] = { x, y, z }; PUT(v, 12); }
void GXNormal1x16(u16 i) { PUT(&i, 2); }
void GXColor4u8(u8 r, u8 g, u8 b, u8 a) { u8 v[4] = { r, g, b, a }; PUT(v, 4); }
void GXColor3u8(u8 r, u8 g, u8 b) { u8 v[3] = { r, g, b }; PUT(v, 3); }
/* direct colours keep GameCube byte order (R,G,B,A bytes; RGB565 big-endian) */
void GXColor1u32(u32 c) { u8 v[4] = { (u8)(c >> 24), (u8)(c >> 16), (u8)(c >> 8), (u8)c }; PUT(v, 4); }
void GXColor1u16(u16 c) { u8 v[2] = { (u8)(c >> 8), (u8)c }; PUT(v, 2); }
void GXColor3f32(f32 r, f32 g, f32 b) {
    u8 v[3] = { (u8)(r * 255.0f), (u8)(g * 255.0f), (u8)(b * 255.0f) };
    PUT(v, 3);
}
void GXTexCoord2f32(f32 s, f32 t) { f32 v[2] = { s, t }; PUT(v, 8); }
void GXTexCoord2s16(s16 s, s16 t) { s16 v[2] = { s, t }; PUT(v, 4); }
void GXTexCoord1x16(u16 i) { PUT(&i, 2); }

/* -------------------------------------------------------------- vertex setup */
static int g_plan_fmt = -1; /* vtxfmt the cached plan was built for (-1: stale) */
void GXClearVtxDesc(void) { GXO();
    g_plan_fmt = -1;
    memset(gx.vtxdesc, 0, sizeof(gx.vtxdesc));
    gx.vtxdesc[GX_VA_POS] = GX_DIRECT;
}
void GXSetVtxDesc(int attr, int type) { GXO();
    g_plan_fmt = -1;
    if (attr >= 0 && attr < GX_VA_MAX_ATTR) gx.vtxdesc[attr] = type;
}
void GXSetVtxAttrFmt(int vtxfmt, int attr, int cnt, int type, u8 frac) { GXO();
    g_plan_fmt = -1;
    if (vtxfmt < 0 || vtxfmt >= MAX_VTXFMT || attr < 0 || attr >= GX_VA_MAX_ATTR) return;
    gx.fmt[vtxfmt][attr].cnt = cnt;
    gx.fmt[vtxfmt][attr].type = type;
    gx.fmt[vtxfmt][attr].frac = frac;
}
void GXSetArray(int attr, void *base, u8 stride) { GXO();
    g_plan_fmt = -1;
    if (attr < 0 || attr >= GX_VA_MAX_ATTR) return;
    gx.array_base[attr] = base;
    gx.array_stride[attr] = stride;
}
void GXInvalidateVtxCache(void) { GXO();}

static int comp_size(int type) {
    switch (type) {
    case GX_U8: case GX_S8: return 1;
    case GX_U16: case GX_S16: return 2;
    default: return 4;
    }
}

static int attr_ncomp(int attr, const AttrFmt *f) {
    if (attr == GX_VA_POS) return f->cnt ? 3 : 2;
    if (attr == GX_VA_NRM) return 3;
    if (attr >= GX_VA_TEX0) return f->cnt ? 2 : 1;
    return 0;
}

static int color_size(int type) {
    switch (type) {
    case GX_RGB565: case GX_RGBA4: return 2;
    case GX_RGB8: case GX_RGBA6: return 3;
    default: return 4;
    }
}

static int direct_size(int attr, const AttrFmt *f) {
    if (attr == GX_VA_PNMTXIDX || (attr >= GX_VA_TEX0MTXIDX && attr < GX_VA_POS)) return 1;
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) return color_size(f->type);
    return attr_ncomp(attr, f) * comp_size(f->type);
}

static void read_color(const u8 *p, int type, u8 out[4], int native) {
    u16 v16;
    u32 v;
    switch (type) {
    case GX_RGB565:
        v16 = native ? *(const u16 *)p : (u16)((p[0] << 8) | p[1]);
        out[0] = (u8)(((v16 >> 11) & 31) * 255 / 31);
        out[1] = (u8)(((v16 >> 5) & 63) * 255 / 63);
        out[2] = (u8)((v16 & 31) * 255 / 31);
        out[3] = 255;
        break;
    case GX_RGBA4:
        v16 = native ? *(const u16 *)p : (u16)((p[0] << 8) | p[1]);
        out[0] = (u8)(((v16 >> 12) & 15) * 17);
        out[1] = (u8)(((v16 >> 8) & 15) * 17);
        out[2] = (u8)(((v16 >> 4) & 15) * 17);
        out[3] = (u8)((v16 & 15) * 17);
        break;
    case GX_RGB8:
    case GX_RGBX8:
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255;
        break;
    case GX_RGBA6:
        v = (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
        out[0] = (u8)(((v >> 18) & 63) * 255 / 63);
        out[1] = (u8)(((v >> 12) & 63) * 255 / 63);
        out[2] = (u8)(((v >> 6) & 63) * 255 / 63);
        out[3] = (u8)((v & 63) * 255 / 63);
        break;
    default:
        if (native) {
            /* u32 in game memory holding 0xRRGGBBAA */
            memcpy(&v, p, 4);
            out[0] = (u8)(v >> 24); out[1] = (u8)(v >> 16); out[2] = (u8)(v >> 8); out[3] = (u8)v;
        } else {
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
        }
        break;
    }
}

/* per-block vertex layout, computed once at GXBegin */
typedef struct {
    u8 attr, desc, type, ncomp, cs, kind, off;
    float scale;
    const u8 *base;
    int stride;
} VAttr;
enum { VK_POS, VK_NRM, VK_TEX0, VK_CLR0, VK_SKIP };
static VAttr g_plan[GX_VA_MAX_ATTR];
static int g_nplan;
static int g_fast; /* bit0: fixed layout; bit1: normal; bit2: texcoord */

static void build_plan(void) {
    int a, off = 0;
    g_nplan = 0;
    for (a = 0; a < GX_VA_MAX_ATTR; a++) {
        const AttrFmt *f = &gx.fmt[g_vtxfmt][a];
        int desc = gx.vtxdesc[a], sz;
        VAttr *v;
        if (desc == GX_NONE) continue;
        sz = desc == GX_DIRECT ? direct_size(a, f) : desc == GX_INDEX8 ? 1 : desc == GX_INDEX16 ? 2 : 0;
        if (!sz) continue;
        v = &g_plan[g_nplan];
        v->attr = (u8)a;
        v->desc = (u8)desc;
        v->type = (u8)f->type;
        v->off = (u8)off;
        v->kind = a == GX_VA_POS ? VK_POS : a == GX_VA_NRM ? VK_NRM : a == GX_VA_TEX0 ? VK_TEX0 :
                  a == GX_VA_CLR0 ? VK_CLR0 : VK_SKIP;
        v->ncomp = (u8)attr_ncomp(a, f);
        v->cs = (u8)comp_size(f->type);
        {
            int frac = a == GX_VA_NRM ? (f->type == GX_S8 ? 6 : f->type == GX_S16 ? 14 : 0) : f->frac;
            v->scale = frac ? 1.0f / (float)(1 << frac) : 1.0f;
        }
        v->base = desc == GX_DIRECT ? NULL : gx.array_base[a];
        v->stride = gx.array_stride[a];
        off += sz;
        if (v->kind != VK_SKIP && !(desc != GX_DIRECT && !v->base)) g_nplan++;
    }
    g_vs = off;
    /* the game only uses direct P3f [N3f] C(RGBA8) [T2f]: decode those with a
     * fixed-layout loop (decode_block) */
    g_fast = 0;
    if (g_nplan >= 2 && g_nplan <= 4 && g_plan[0].kind == VK_POS && g_plan[0].ncomp == 3) {
        int k, ok = 1, hn = 0, ht = 0, hc = 0;
        for (k = 0; k < g_nplan; k++) {
            const VAttr *v = &g_plan[k];
            if (v->desc != GX_DIRECT) { ok = 0; break; }
            if (v->kind == VK_POS) ok &= v->type == GX_F32 && k == 0;
            else if (v->kind == VK_NRM) { ok &= v->type == GX_F32 && v->ncomp == 3 && k == 1; hn = 1; }
            else if (v->kind == VK_CLR0) { ok &= v->type == 5 /* RGBA8 */ && k == 1 + hn; hc = 1; }
            else if (v->kind == VK_TEX0) { ok &= v->type == GX_F32 && v->ncomp == 2 && k == g_nplan - 1; ht = 1; }
            else ok = 0;
        }
        if (ok && hc && g_vs == 12 + hn * 12 + 4 + ht * 8) g_fast = 1 | hn << 1 | ht << 2;
    }
}

static inline float read_comp(const u8 *p, int type, float scale) {
    switch (type) {
    case GX_U8: return p[0] * scale;
    case GX_S8: return (s8)p[0] * scale;
    case GX_U16: { u16 v; memcpy(&v, p, 2); return v * scale; }
    case GX_S16: { s16 v; memcpy(&v, p, 2); return v * scale; }
    default: { float f; memcpy(&f, p, 4); return f; }
    }
}

static void decode_vertex(const u8 *src, GLVertex *v) {
    int k;
    v->nrm[0] = v->nrm[1] = v->nrm[2] = 0.0f;
    v->tex[0] = v->tex[1] = 0.0f;
    v->pos[2] = 0.0f;
    v->clr[0] = v->clr[1] = v->clr[2] = v->clr[3] = 255;
    for (k = 0; k < g_nplan; k++) {
        const VAttr *a = &g_plan[k];
        const u8 *p = src + a->off;
        float *dst;
        int i;
        if (a->desc != GX_DIRECT) {
            int idx;
            if (a->desc == GX_INDEX8) idx = p[0];
            else { u16 x; memcpy(&x, p, 2); idx = x; }
            p = a->base + idx * a->stride;
        }
        if (a->kind == VK_CLR0) {
            read_color(p, a->type, v->clr, a->desc != GX_DIRECT);
            continue;
        }
        dst = a->kind == VK_POS ? v->pos : a->kind == VK_NRM ? v->nrm : v->tex;
        if (a->type == GX_F32) {
            memcpy(dst, p, 4u * a->ncomp);
        } else {
            for (i = 0; i < a->ncomp; i++) dst[i] = read_comp(p + i * a->cs, a->type, a->scale);
        }
    }
}

/* -------------------------------------------------------------- shaders */
typedef struct ShaderEntry_s {
    u32 key[8];
    GLuint prog;
    GLint u_proj, u_posmtx, u_nrmmtx, u_amb, u_mat, u_lightpos, u_lightcol, u_alpharef, u_fog, u_fogcol;
    GLint u_tex[MAX_TEV];
    char *vsrc, *fsrc;  /* generated by the game thread, compiled by the render thread */
    /* last uniform values uploaded to this program (skip redundant uploads) */
    int uset;
    float last_proj[4][4], last_posmtx[3][4], last_nrmmtx[3][3];
    GXColor last_amb, last_mat, last_fogcol;
    LightSlot last_lights[8];
    int last_aref0, last_aref1;
    float last_fog_start, last_fog_end;
    /* alpha test that may be dropped (nodisc_variant): -1 unknown, 0 no, 1 yes;
     * atest_lo: lowest alpha from which every value passes, for the reference
     * values atest_ref (uniforms, not in the key) */
    int atest_drop, atest_lo, atest_ref;
    struct ShaderEntry_s *nodisc; /* the same shader without the alpha test */
} ShaderEntry;

#define MAX_SHADERS 256
static ShaderEntry g_shaders[MAX_SHADERS];
static int g_num_shaders;

static const char *color_arg_glsl(int a, int stage0) {
    switch (a) {
    case 0: return "prev.rgb";
    case 1: return "vec3(prev.a)";
    case 2: return "c0.rgb";
    case 3: return "vec3(c0.a)";
    case 4: return "c1.rgb";
    case 5: return "vec3(c1.a)";
    case 6: return "c2.rgb";
    case 7: return "vec3(c2.a)";
    case 8: return "tex.rgb";
    case 9: return "vec3(tex.a)";
    case 10: return "ras.rgb";
    case 11: return "vec3(ras.a)";
    case 12: return "vec3(1.0)";
    case 13: return "vec3(0.5)";
    case 14: return "vec3(1.0)";
    default: (void)stage0; return "vec3(0.0)";
    }
}
static const char *alpha_arg_glsl(int a) {
    switch (a) {
    case 0: return "prev.a";
    case 1: return "c0.a";
    case 2: return "c1.a";
    case 3: return "c2.a";
    case 4: return "tex.a";
    case 5: return "ras.a";
    case 6: return "1.0";
    default: return "0.0";
    }
}
static const char *reg_name(int r) {
    switch (r) { case 1: return "c0"; case 2: return "c1"; case 3: return "c2"; default: return "prev"; }
}
static const char *alpha_cmp_glsl(int comp) {
    switch (comp) {
    case 0: return "false";
    case 1: return "a < r";
    case 2: return "a == r";
    case 3: return "a <= r";
    case 4: return "a > r";
    case 5: return "a != r";
    case 6: return "a >= r";
    default: return "true";
    }
}

/* An alpha test that only rejects alpha 0 changes nothing when the draw blends
 * with source alpha and writes no depth (a = 0 leaves the destination as is);
 * dropping it avoids `discard`, costly on the Vita GPU for the big particle
 * quads (steam, smoke). */
static int atest_pass(int comp, int a, int r) {
    switch (comp) {
    case 0: return 0;
    case 1: return a < r;
    case 2: return a == r;
    case 3: return a <= r;
    case 4: return a > r;
    case 5: return a != r;
    case 6: return a >= r;
    default: return 1;
    }
}
static int atest_redundant(void) {
    /* the compare only depends on 5 values: remember the last answer (this runs
     * for every state capture, and the 255-step scan cost ~10% of the gx CPU
     * time with particles) */
    static u32 last_in = 0xFFFFFFFFu;
    static int last_out;
    u32 in;
    int a;
    if (gx.blend_type != 1 || gx.blend_src != 4 || (gx.z_enable && gx.z_update)) return 0;
    in = (u32)gx.alpha_comp0 | (u32)gx.alpha_comp1 << 3 | (u32)gx.alpha_op << 6 |
         (u32)(gx.alpha_ref0 & 0xFF) << 8 | (u32)(gx.alpha_ref1 & 0xFF) << 16;
    if (in == last_in) return last_out;
    last_in = in;
    last_out = 0;
    for (a = 1; a < 256; a++) {
        int t0 = atest_pass(gx.alpha_comp0, a, gx.alpha_ref0), t1 = atest_pass(gx.alpha_comp1, a, gx.alpha_ref1);
        int ok = gx.alpha_op == 0 ? (t0 && t1) : gx.alpha_op == 1 ? (t0 || t1) : gx.alpha_op == 2 ? (t0 != t1) : (t0 == t1);
        if (!ok) return 0;
    }
    last_out = 1;
    return 1;
}

/* Vertex lighting: only COLOR0 is lit (ALPHA0 only picks its material
 * source). The lights it uses are packed, in index order, into the first
 * slots of u_lightpos/u_lightcol (capture_state), so a vertex shader depends
 * on how many lights are on, not on which: same sums in the same order, 9
 * lit variants instead of one per light mask. A new vertex shader is the
 * dearest compile (330-600 ms on the render thread, a hitch). */
static inline int chan_lit(void) { return gx.num_chans && gx.chan[0].enable; }
static inline int lit_count(void) { return chan_lit() ? __builtin_popcount((unsigned)gx.chan[0].light_mask & 0xFFu) : 0; }

/* the key holds exactly what changes the generated GLSL */
static int g_nodisc_gen; /* generating the variant without the alpha test (nodisc_variant) */
static void build_key(u32 key[8]) {
    int i, lit = chan_lit(), nl = lit_count();
    memset(key, 0, 8 * sizeof(u32));
    key[0] = (u32)gx.num_tev | (u32)lit << 4 | (u32)gx.chan[0].mat_src << 5 | (u32)(lit ? gx.chan[0].amb_src : 0) << 6 |
             (u32)gx.chan[1].mat_src << 8 | (u32)(nl ? gx.chan[0].diff_fn : 0) << 10 |
             (u32)(gx.fog_type ? 1 : 0) << 14 | (u32)gx.alpha_comp0 << 15 | (u32)gx.alpha_comp1 << 18 |
             (u32)gx.alpha_op << 21 | (u32)atest_redundant() << 24 | (u32)g_nodisc_gen << 25;
    key[1] = (u32)nl;
    for (i = 0; i < gx.num_tev && i < MAX_TEV; i++) {
        const TevStage *t = &gx.tev[i];
        u32 c = (u32)t->color_in[0] | t->color_in[1] << 4 | t->color_in[2] << 8 | t->color_in[3] << 12 |
                t->color_op << 16 | t->color_bias << 17 | t->color_scale << 19 | t->color_clamp << 21 |
                t->color_reg << 22 | (t->texmap != 0xFF && gx.texmap_valid[t->texmap & 7] ? 1u : 0u) << 24 |
                (t->channel != 0xFF ? 1u : 0u) << 25;
        u32 a = (u32)t->alpha_in[0] | t->alpha_in[1] << 3 | t->alpha_in[2] << 6 | t->alpha_in[3] << 9 |
                t->alpha_op << 12 | t->alpha_bias << 13 | t->alpha_scale << 15 | t->alpha_clamp << 17 |
                t->alpha_reg << 18;
        key[2 + i] = c;
        key[2 + i] ^= a * 0x9E3779B1u;
        key[6] ^= a << (i * 2);
        key[7] = key[7] * 31 + a;
    }
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        port_log("gx: shader compile failed: %s\n%s", log, src);
    }
    return s;
}

static const char *VS_SRC =
    PORT_GLSL_HEADER
    "attribute vec3 a_pos;\n"
    "attribute vec3 a_nrm;\n"
    "attribute vec4 a_clr;\n"
    "attribute vec2 a_tex;\n"
    "uniform mat4 u_proj;\n"
    "uniform mat4 u_posmtx;\n"
    "uniform mat3 u_nrmmtx;\n"
    "uniform vec4 u_amb;\n"
    "uniform vec4 u_mat;\n"
    "uniform vec3 u_lightpos[8];\n"
    "uniform vec4 u_lightcol[8];\n"
    "uniform vec3 u_fog;\n"
    "varying vec4 v_clr;\n"
    "varying vec2 v_tex;\n"
    "varying float v_fog;\n"
    "void main() {\n"
    "  vec4 vp = u_posmtx * vec4(a_pos, 1.0);\n"
    "  vec4 cp = u_proj * vp;\n"
    "  cp.z = cp.z * 2.0 + cp.w;\n"
    "  gl_Position = cp;\n"
    "  v_fog = clamp((-vp.z - u_fog.x) * u_fog.y, 0.0, 1.0);\n"
    "  v_tex = a_tex;\n"
    "  vec3 n = normalize(u_nrmmtx * a_nrm);\n"
    "  vec4 vc = a_clr;\n"
    "  vec4 c;\n"
    "#ifdef CHAN_C_ENABLE\n"
    "  vec3 amb = CHAN_C_AMB.rgb;\n"
    "  vec3 lit = amb;\n"
    "  LIGHTS_CODE\n"
    "  c.rgb = CHAN_C_MAT.rgb * clamp(lit, 0.0, 1.0);\n"
    "#else\n"
    "  c.rgb = CHAN_C_MAT.rgb;\n"
    "#endif\n"
    "  c.a = CHAN_A_MAT.a;\n"
    "  v_clr = c;\n"
    "}\n";

static ShaderEntry *get_shader_inner(void);
static ShaderEntry *get_shader(void) {
    ShaderEntry *r;
    PROF_BEGIN_S(PROF_SHADER, 8);
    r = get_shader_inner();
    PROF_END_S(PROF_SHADER, 8);
    return r;
}

static ShaderEntry *get_shader_inner(void) {
    u32 key[8];
    int i;
    ShaderEntry *e;
    char vs[8192], fs[8192], defs[1024];
    char *p;
    build_key(key);
    for (i = 0; i < g_num_shaders; i++)
        if (memcmp(g_shaders[i].key, key, sizeof(key)) == 0) return &g_shaders[i];
    if (g_num_shaders == MAX_SHADERS) {
        port_log("gx: shader cache full");
        return &g_shaders[0];
    }
    e = &g_shaders[g_num_shaders++];
    memcpy(e->key, key, sizeof(key));
    /* rare (a few dozen a session) and each one may cost a compile: say which */
    port_log("gx: shader %d new at frame %d: key %08x %08x %08x %08x %08x %08x %08x %08x", g_num_shaders - 1,
             port_frame_count(), key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7]);

    /* vertex shader defines (colour channel config) */
    p = defs;
    if (chan_lit()) {
        /* one unrolled term per light on, in the packed slots (build_key) */
        const char *df = gx.chan[0].diff_fn == 2 ? "max(dot(n, L), 0.0)" : gx.chan[0].diff_fn == 1 ? "dot(n, L)" : "1.0";
        int nl = lit_count();
        p += sprintf(p, "#define CHAN_C_ENABLE 1\n");
        p += sprintf(p, "#define CHAN_C_AMB %s\n", gx.chan[0].amb_src ? "vc" : "u_amb");
        p += sprintf(p, "#define LIGHTS_CODE ");
        for (i = 0; i < nl; i++)
            p += sprintf(p, "{ vec3 L = normalize(u_lightpos[%d] - vp.xyz); lit += %s * u_lightcol[%d].rgb; } ", i, df, i);
        p += sprintf(p, "\n");
    }
    p += sprintf(p, "#define CHAN_C_MAT %s\n", gx.chan[0].mat_src ? "vc" : "u_mat");
    p += sprintf(p, "#define CHAN_A_MAT %s\n", gx.chan[1].mat_src ? "vc" : "u_mat");
    {
        /* insert defines after the GLSL header line(s) */
        const char *body = strstr(VS_SRC, "attribute");
        size_t hl = (size_t)(body - VS_SRC);
        memcpy(vs, VS_SRC, hl);
        vs[hl] = 0;
        strcat(vs, defs);
        strcat(vs, body);
    }

    /* fragment shader: TEV */
    p = fs;
    p += sprintf(p, "%s", PORT_GLSL_HEADER);
    p += sprintf(p, "varying vec4 v_clr;\nvarying vec2 v_tex;\nvarying float v_fog;\n");
    for (i = 0; i < MAX_TEV; i++) p += sprintf(p, "uniform sampler2D u_tex%d;\n", i);
    p += sprintf(p, "uniform vec2 u_alpharef;\nuniform vec3 u_fogcol;\n");
    p += sprintf(p, "void main() {\n  vec4 prev = vec4(1.0); vec4 c0 = vec4(0.0); vec4 c1 = vec4(0.0); vec4 c2 = vec4(0.0);\n"
                    "  vec4 tex; vec4 ras;\n");
    if (gx.num_tev == 0) p += sprintf(p, "  prev = v_clr;\n");
    for (i = 0; i < gx.num_tev && i < MAX_TEV; i++) {
        const TevStage *t = &gx.tev[i];
        int has_tex = t->texmap != 0xFF && gx.texmap_valid[t->texmap & 7];
        const char *bias_c = t->color_bias == 1 ? " + 0.5" : t->color_bias == 2 ? " - 0.5" : "";
        const char *bias_a = t->alpha_bias == 1 ? " + 0.5" : t->alpha_bias == 2 ? " - 0.5" : "";
        const char *scale_c = t->color_scale == 1 ? " * 2.0" : t->color_scale == 2 ? " * 4.0" : t->color_scale == 3 ? " * 0.5" : "";
        const char *scale_a = t->alpha_scale == 1 ? " * 2.0" : t->alpha_scale == 2 ? " * 4.0" : t->alpha_scale == 3 ? " * 0.5" : "";
        if (has_tex) p += sprintf(p, "  tex = texture2D(u_tex%d, v_tex);\n", i);
        else p += sprintf(p, "  tex = vec4(1.0);\n");
        p += sprintf(p, "  ras = %s;\n", t->channel != 0xFF ? "v_clr" : "vec4(0.0)");
        p += sprintf(p, "  {\n    vec3 a = %s, b = %s, c = %s, d = %s;\n", color_arg_glsl(t->color_in[0], i == 0),
                     color_arg_glsl(t->color_in[1], i == 0), color_arg_glsl(t->color_in[2], i == 0),
                     color_arg_glsl(t->color_in[3], i == 0));
        p += sprintf(p, "    float aa = %s, ab = %s, ac = %s, ad = %s;\n", alpha_arg_glsl(t->alpha_in[0]),
                     alpha_arg_glsl(t->alpha_in[1]), alpha_arg_glsl(t->alpha_in[2]), alpha_arg_glsl(t->alpha_in[3]));
        p += sprintf(p, "    vec3 rc = (d %s mix(a, b, c)%s)%s;\n", t->color_op ? "-" : "+", bias_c, scale_c);
        p += sprintf(p, "    float ra = (ad %s mix(aa, ab, ac)%s)%s;\n", t->alpha_op ? "-" : "+", bias_a, scale_a);
        if (t->color_clamp) p += sprintf(p, "    rc = clamp(rc, 0.0, 1.0);\n");
        if (t->alpha_clamp) p += sprintf(p, "    ra = clamp(ra, 0.0, 1.0);\n");
        p += sprintf(p, "    %s.rgb = rc;\n    %s.a = ra;\n  }\n", reg_name(t->color_reg), reg_name(t->alpha_reg));
    }
    /* alpha compare */
    if (!(gx.alpha_comp0 == 7 && gx.alpha_comp1 == 7 && gx.alpha_op == 1) &&
        !(gx.alpha_comp0 == 7 && gx.alpha_op == 1) && !atest_redundant() && !g_nodisc_gen) {
        const char *op = gx.alpha_op == 0 ? "&&" : gx.alpha_op == 1 ? "||" : gx.alpha_op == 2 ? "!=" : "==";
        p += sprintf(p, "  {\n    float a = floor(prev.a * 255.0 + 0.5);\n    float r;\n    bool t0, t1;\n");
        p += sprintf(p, "    r = u_alpharef.x; t0 = %s;\n", alpha_cmp_glsl(gx.alpha_comp0));
        p += sprintf(p, "    r = u_alpharef.y; t1 = %s;\n", alpha_cmp_glsl(gx.alpha_comp1));
        p += sprintf(p, "    if (!(t0 %s t1)) discard;\n  }\n", op);
    }
    if (gx.fog_type) {
        p += sprintf(p, "  prev.rgb = mix(prev.rgb, u_fogcol, v_fog);\n");
    }
    if (getenv("CWOC_DEBUGFS")) p += sprintf(p, "  prev = vec4(1.0, 0.0, 0.0, 1.0);\n");
    p += sprintf(p, "  gl_FragColor = prev;\n}\n");
    if (getenv("CWOC_DUMPSHADERS")) {
        /* CWOC_DUMPSHADERS=<dir>: one file per shader (the log truncates long lines) */
        char path[512];
        FILE *f;
        snprintf(path, sizeof(path), "%s/sh_%08x_%08x_%08x_%08x.glsl", getenv("CWOC_DUMPSHADERS"), key[0], key[1], key[2], key[3]);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "// VS\n%s\n// FS\n%s", vs, fs);
            fclose(f);
        }
    }

    e->vsrc = strdup(vs);
    e->fsrc = strdup(fs);
    e->prog = 0;
    e->uset = 0;
    e->atest_drop = -1;
    e->nodisc = NULL;
    return e;
}

/* render thread: build the GL program of a shader entry on first use */
/* program creation cost (render thread), reported with the 5 s perf lines:
 * programs created, ms in vertex / fragment compile, link + lookups, and
 * programs / ms restored from the binary cache */
unsigned g_shc_n, g_shc_hit;
uint64_t g_shc_us[4];

#ifdef PORT_VITA
/* Persistent program cache. vitaGL compiles GLSL in glLinkProgram (~550 ms a
 * program on the render thread, every session: its own disk cache stays
 * empty here). The first link of each program is saved with
 * glGetProgramBinary (bound attributes + both GXP binaries) and later
 * sessions restore it with glProgramBinary, which skips the compiler.
 * Files are named by a hash of both sources and of the linked vitaGL
 * (PORT_VITAGL_ID, CMake), so another vitaGL never loads stale binaries.
 * CWOC_SHCACHE=0 disables it; CWOC_SHCACHEDIR=<dir> uses another folder
 * (measurements: a fresh folder counts what a first session compiles). */
#define SHCACHE_DIR "ux0:data/crashwoc/shadercache"
static const char *shcache_dir(void) {
    const char *d = getenv("CWOC_SHCACHEDIR");
    return d && d[0] ? d : SHCACHE_DIR;
}
#define SHCACHE_MAGIC 0x31435743u /* "CWC1" */
#ifndef PORT_VITAGL_ID
#define PORT_VITAGL_ID "unknown"
#endif
static int g_shcache = -1;

/* render thread: the program cache is on (CWOC_SHCACHE); made on first use */
static int shcache_on(void) {
    if (g_shcache < 0) {
        const char *s = getenv("CWOC_SHCACHE");
        g_shcache = !(s && s[0] == '0');
        if (g_shcache) mkdir(shcache_dir(), 0777);
        port_log("gx: shader program cache %s (%s, vitaGL %s)", g_shcache ? "on" : "off", shcache_dir(), PORT_VITAGL_ID);
    }
    return g_shcache;
}

static unsigned long long fnv1a64(const char *s, unsigned long long h) {
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static void shcache_path(char *buf, size_t n, const ShaderEntry *e) {
    const char *opt = getenv("CWOC_SHOPT"); /* compiler optimisation (port_video.c) */
    unsigned long long h = fnv1a64(PORT_VITAGL_ID, 0xcbf29ce484222325ULL);
    h = fnv1a64(opt && opt[0] >= '0' && opt[0] <= '4' ? opt : "", h);
    h = fnv1a64("|", h);
    h = fnv1a64(e->vsrc, h);
    h = fnv1a64("|", h);
    h = fnv1a64(e->fsrc, h);
    snprintf(buf, n, "%s/%016llx.bin", shcache_dir(), h);
}

static int shcache_load(ShaderEntry *e, const char *path) {
    u32 hdr[3]; /* magic, binary format, length */
    void *data;
    GLint ok = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* the seed shipped in the VPK (CMake CWOC_SHADER_SEED): read-only,
         * same file name; CWOC_SHSEED=0 ignores it */
        static int seed = -1;
        const char *base = strrchr(path, '/');
        char alt[128];
        if (seed < 0) seed = !(getenv("CWOC_SHSEED") && getenv("CWOC_SHSEED")[0] == '0');
        if (!seed || !base) return 0;
        snprintf(alt, sizeof(alt), "app0:shaders/prog%s", base);
        f = fopen(alt, "rb");
        if (!f) return 0;
        path = NULL; /* never remove a seed file */
    }
    if (fread(hdr, sizeof(hdr), 1, f) != 1 || hdr[0] != SHCACHE_MAGIC || hdr[2] == 0 || hdr[2] > (1u << 20) ||
        !(data = malloc(hdr[2]))) {
        fclose(f);
        if (path) remove(path);
        return 0;
    }
    if (fread(data, hdr[2], 1, f) != 1) {
        fclose(f);
        free(data);
        if (path) remove(path);
        return 0;
    }
    fclose(f);
    e->prog = glCreateProgram();
    glProgramBinary(e->prog, (GLenum)hdr[1], data, (GLsizei)hdr[2]);
    free(data);
    glGetProgramiv(e->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        port_log("gx: cached program %s rejected, compiling", path ? path : "(seed)");
        glDeleteProgram(e->prog);
        e->prog = 0;
        if (path) remove(path);
        return 0;
    }
    return 1;
}

static void shcache_save(GLuint prog, const char *path) {
    GLint len = 0;
    GLsizei got = 0;
    GLenum fmt = 0;
    u32 *buf;
    char tmp[300];
    FILE *f;
    glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0 || len > (1 << 20)) return;
    buf = malloc((size_t)len + 64 + 12); /* vitaGL writes without checking the size: some slack */
    if (!buf) return;
    glGetProgramBinary(prog, len, &got, &fmt, buf + 3);
    if (got <= 0 || got > len) {
        free(buf);
        return;
    }
    buf[0] = SHCACHE_MAGIC;
    buf[1] = (u32)fmt;
    buf[2] = (u32)got;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f) {
        int good = fwrite(buf, 12 + (size_t)got, 1, f) == 1;
        good &= fclose(f) == 0;
        if (good) rename(tmp, path);
        else remove(tmp);
    }
    free(buf);
}
#endif

/* Compiled shader objects, shared by every program with the same source.
 * vitaGL compiles GLSL in glLinkProgram, but only the shaders of the program
 * that are not compiled yet (it reuses the semantic bindings of a compiled
 * one), so a program whose vertex shader is already known only compiles
 * its fragment shader. Most programs differ only in the TEV fragment code. */
static struct { const char *src; GLenum type; GLuint obj; } g_shobj[2 * MAX_SHADERS];
static int g_num_shobj;

static GLuint shader_obj(GLenum type, const char *src, int *reused) {
    int i;
    for (i = 0; i < g_num_shobj; i++)
        if (g_shobj[i].type == type && strcmp(g_shobj[i].src, src) == 0) {
            *reused = 1;
            return g_shobj[i].obj;
        }
    *reused = 0;
    if (g_num_shobj == 2 * MAX_SHADERS) return compile(type, src);
    g_shobj[g_num_shobj].src = src;
    g_shobj[g_num_shobj].type = type;
    g_shobj[g_num_shobj].obj = compile(type, src);
    return g_shobj[g_num_shobj++].obj;
}

static void ensure_program(ShaderEntry *e) {
    GLuint v, f;
    int i, v_old, f_old;
    uint64_t t0, t1, t2, tm, tl;
#ifdef PORT_VITA
    char cpath[256];
#endif
    if (e->prog) return;
    t0 = port_prof_now();
#ifdef PORT_VITA
    if (shcache_on()) {
        shcache_path(cpath, sizeof(cpath), e);
        if (shcache_load(e, cpath)) {
            t2 = port_prof_now();
            g_shc_us[3] += t2 - t0;
            g_shc_hit++;
            goto lookups;
        }
    }
#endif
    tm = port_prof_now(); /* t0..tm: looking up the program cache */
    v = shader_obj(GL_VERTEX_SHADER, e->vsrc, &v_old);
    t1 = port_prof_now();
    f = shader_obj(GL_FRAGMENT_SHADER, e->fsrc, &f_old);
    t2 = port_prof_now();
    g_shc_us[0] += t1 - t0;
    g_shc_us[1] += t2 - t1;
    e->prog = glCreateProgram();
    glAttachShader(e->prog, v);
    glAttachShader(e->prog, f);
    glBindAttribLocation(e->prog, 0, "a_pos");
    glBindAttribLocation(e->prog, 1, "a_nrm");
    glBindAttribLocation(e->prog, 2, "a_clr");
    glBindAttribLocation(e->prog, 3, "a_tex");
    glLinkProgram(e->prog);
    tl = port_prof_now();
    {
        GLint ok = 0;
        glGetProgramiv(e->prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetProgramInfoLog(e->prog, sizeof(log), NULL, log);
            port_log("gx: program link failed: %s", log);
        }
#ifdef PORT_VITA
        else if (g_shcache) shcache_save(e->prog, cpath);
#endif
    }
    t1 = port_prof_now();
    g_shc_us[2] += t1 - t2;
    g_shc_n++;
    port_log("gx: program %d compiled in %.0f ms (vertex shader %s, fragment shader %s; cache lookup %.0f, link %.0f, "
             "save %.0f ms)", (int)(e - g_shaders), (t1 - t0) / 1000.0, v_old ? "reused" : "new", f_old ? "reused" : "new",
             (tm - t0) / 1000.0, (tl - t2) / 1000.0, (t1 - tl) / 1000.0);
    t2 = port_prof_now();
#ifdef PORT_VITA
lookups:
#endif
    e->u_proj = glGetUniformLocation(e->prog, "u_proj");
    e->u_posmtx = glGetUniformLocation(e->prog, "u_posmtx");
    e->u_nrmmtx = glGetUniformLocation(e->prog, "u_nrmmtx");
    e->u_amb = glGetUniformLocation(e->prog, "u_amb");
    e->u_mat = glGetUniformLocation(e->prog, "u_mat");
    e->u_lightpos = glGetUniformLocation(e->prog, "u_lightpos");
    e->u_lightcol = glGetUniformLocation(e->prog, "u_lightcol");
    e->u_alpharef = glGetUniformLocation(e->prog, "u_alpharef");
    e->u_fog = glGetUniformLocation(e->prog, "u_fog");
    e->u_fogcol = glGetUniformLocation(e->prog, "u_fogcol");
    for (i = 0; i < MAX_TEV; i++) {
        char nm[16];
        sprintf(nm, "u_tex%d", i);
        e->u_tex[i] = glGetUniformLocation(e->prog, nm);
    }
    g_shc_us[2] += port_prof_now() - t2;
}

/* -------------------------------------------------------------- textures */
typedef struct {
    /* game thread */
    const void *image;
    int w, h, fmt, used;
    u32 hash, qhash;   /* full / sparse content hash */
    u32 full_frame;    /* frame of the last full hash */
    u32 last_use;
    u32 checked; /* frame in which the contents hash was last verified */
    int amin;    /* lowest texel alpha (nodisc_variant) */
    /* render thread */
    GLuint gltex;
    int wrap_s, wrap_t; /* GL wrap modes currently set on the texture (-1: unknown) */
} TexCacheEntry;

/* shadow of the GL texture bindings (vitaGL calls are not free) */
static GLuint g_bound[8];
static int g_active_unit = -1;
/* CWOC_GPUEXP (measurement only, wrong picture): v = 1x1 scissor (vertex and
 * setup work only), q = quarter-size viewport, t = 1x1 texture everywhere */
static int g_gpuexp;
static GLuint g_exp_tex;
static GLuint exp_tex(void) {
    if (!g_exp_tex) {
        static const u8 px[4] = {255, 255, 255, 255};
        glGenTextures(1, &g_exp_tex);
        glBindTexture(GL_TEXTURE_2D, g_exp_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        g_bound[g_active_unit < 0 ? 0 : g_active_unit] = g_exp_tex;
    }
    return g_exp_tex;
}
/* bind_tex without the CWOC_GPUEXP=t substitution (the port's own textures) */
static void bind_tex_raw(int unit, GLuint tex) {
    if (g_active_unit != unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        g_active_unit = unit;
    }
    if (g_bound[unit] != tex) {
        glBindTexture(GL_TEXTURE_2D, tex);
        g_bound[unit] = tex;
    }
}

static void bind_tex(int unit, GLuint tex) {
    if (g_gpuexp == 't' && tex) tex = exp_tex();
    if (g_active_unit != unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        g_active_unit = unit;
    }
    if (g_bound[unit] != tex) {
        glBindTexture(GL_TEXTURE_2D, tex);
        g_bound[unit] = tex;
    }
}

/* render thread: the small 3D target of CWOC_RES3D (port_gx_begin_frame) */
static int g_rt_lowres;            /* the 3D target is bound */
static GLuint g_r3_fbo, g_r3_tex, g_r3_rb;
static int g_r3_w, g_r3_h, g_r3_dw, g_r3_dh; /* target size, and the drawable it was made for */
static unsigned g_r3_frames, g_r3_hud, g_r3_late;
static int g_res3d_pct_rt;         /* copy of the percent for the render thread (set at init) */
static int g_res3d_dyn;            /* 1: the controller below decides (default); 0: always low (CWOC_RES3D=N) */
static volatile int g_res3d_low;   /* render thread -> game thread: draw the 3D small from the next frame */

/* Dynamic 3D resolution: the 3D goes small only while the GPU sets the frame
 * rate. The render thread measures, per 0.5 s window, the time between
 * presents (T) and the time blocked in the swap (S). GPU bound: S > 2.5 ms
 * and T over the target by 7 % (16.7 ms; 33.3 with the frame lock, where the
 * game thread sleeps instead). Lowering when the GPU did not even fill a 60 Hz
 * frame would cost sharpness for nothing, and at the 60 cap the swap waits
 * for the vblank, not for the GPU.
 * - native: 2 GPU-bound windows in a row -> small; T is kept as the native
 *   GPU cost (gnat)
 * - small: still GPU bound -> stay. Else back to native (a probe) after 2
 *   windows in a row with the CPU alone clearly slower than the native GPU
 *   was (T >= 1.05 gnat), or after a hold (8 s). Not when small reaches the
 *   60 cap: that says nothing of native (widescreen play: small at 60, native
 *   GPU bound at ~48, 28 such round trips in a session). A probe that is GPU
 *   bound within 2 windows goes back small and doubles the hold (up to
 *   64 s); one that lasts 4 s resets it. */
static void rt_res3d_ctrl(uint64_t t_swap0, uint64_t now) {
    extern int port_pace_lock_on(void);
    static uint64_t last, win_t0, low_at, probe_at, probe_t0;
    static double sum_s;
    static int n, streak, exits, probing, hold_ms = 8000;
    static double gnat;
    double T, S, target;
    int gpu;
    if (!g_res3d_dyn || !g_res3d_pct_rt) return;
    if (!last || now - last > 100000) { /* start, or a load / long hitch: start a new window */
        last = win_t0 = now;
        sum_s = 0;
        n = 0;
        return;
    }
    last = now;
    sum_s += (double)(now - t_swap0);
    n++;
    if (now - win_t0 < 500000) return;
    T = (double)(now - win_t0) / n;
    S = sum_s / n;
    win_t0 = now;
    sum_s = 0;
    n = 0;
    target = port_pace_lock_on() ? 33333.0 : 16667.0;
    gpu = S > 2500.0 && T > target * 1.07;
    if (!g_res3d_low) {
        if (probing && !gpu && now - probe_t0 >= 4000000) { probing = 0; hold_ms = 8000; }
        if (!gpu) { streak = 0; return; }
        if (++streak < 2) return;
        streak = 0;
        if (probing) hold_ms = hold_ms * 2 > 64000 ? 64000 : hold_ms * 2;
        probing = 0;
        gnat = T;
        g_res3d_low = 1;
        low_at = now;
        probe_at = now + (uint64_t)hold_ms * 1000;
        port_log("gx: 3D at %d%% (GPU bound: frame %.1f ms, swap wait %.1f ms; next try native in %d s)", g_res3d_pct_rt,
                 T / 1000.0, S / 1000.0, hold_ms / 1000);
        return;
    }
    if (gpu) { exits = 0; return; }
    if (T >= gnat * 1.05) exits++;
    else exits = 0;
    if (exits >= 2 || now >= probe_at) {
        exits = 0;
        g_res3d_low = 0;
        probing = 1;
        probe_t0 = now;
        streak = 0;
        port_log("gx: 3D native again after %.1f s (frame %.1f ms, swap wait %.1f ms, native was %.1f ms)",
                 (now - low_at) / 1e6, T / 1000.0, S / 1000.0, gnat / 1000.0);
    }
}

/* a window rectangle (GL, bottom-left origin) mapped into the 3D target */
static void r3_map(int *x, int *y, int *w, int *h) {
    float fx = (float)g_r3_w / (float)g_draw_w, fy = (float)g_r3_h / (float)g_draw_h;
    int ox = g_off_x, oy = g_win_h - g_off_y - g_draw_h;
    int x0 = (int)((*x - ox) * fx + 0.5f), x1 = (int)((*x + *w - ox) * fx + 0.5f);
    int y0 = (int)((*y - oy) * fy + 0.5f), y1 = (int)((*y + *h - oy) * fy + 0.5f);
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}

/* bind the 3D target (made, or remade when the drawable changed size) */
static void r3_bind(void) {
    int w = (g_draw_w * g_res3d_pct_rt + 50) / 100, h = (g_draw_h * g_res3d_pct_rt + 50) / 100;
    if (!g_r3_fbo || g_r3_dw != g_draw_w || g_r3_dh != g_draw_h) {
        if (g_r3_fbo) {
            glDeleteFramebuffers(1, &g_r3_fbo);
            glDeleteRenderbuffers(1, &g_r3_rb);
            glDeleteTextures(1, &g_r3_tex);
        }
        if (g_active_unit != 0) {
            glActiveTexture(GL_TEXTURE0);
            g_active_unit = 0;
        }
        glGenTextures(1, &g_r3_tex);
        glBindTexture(GL_TEXTURE_2D, g_r3_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g_bound[0] = g_r3_tex;
        glGenFramebuffers(1, &g_r3_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_r3_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_r3_tex, 0);
        glGenRenderbuffers(1, &g_r3_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, g_r3_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_r3_rb);
        g_r3_w = w; g_r3_h = h; g_r3_dw = g_draw_w; g_r3_dh = g_draw_h;
        port_log("gx: 3D target %dx%d for a %dx%d drawable (%d%%)", w, h, g_draw_w, g_draw_h, g_res3d_pct_rt);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_r3_fbo);
    g_rt_lowres = 1;
}

/* scale the 3D target up into the drawable; the rest of the frame is native */
static void r3_composite(void) { /* callers drop the GL state shadow (g_gl.valid = 0) */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, g_win_w, g_win_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_r3_fbo);
    glBlitFramebuffer(0, 0, g_r3_w, g_r3_h, g_off_x, g_win_h - g_off_y - g_draw_h, g_off_x + g_draw_w, g_win_h - g_off_y,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_rt_lowres = 0;
}

#define TEX_CACHE_SIZE 2048
static TexCacheEntry g_texcache[TEX_CACHE_SIZE];
static u32 g_frame;

static int tex_size(int fmt, int w, int h) {
    int bw, bh, bpb; /* block size and bytes per block */
    switch (fmt) {
    case 0: bw = 8; bh = 8; bpb = 32; break;     /* I4 */
    case 1: bw = 8; bh = 4; bpb = 32; break;     /* I8 */
    case 2: bw = 8; bh = 4; bpb = 32; break;     /* IA4 */
    case 3: case 4: case 5: bw = 4; bh = 4; bpb = 32; break;
    case 6: bw = 4; bh = 4; bpb = 64; break;     /* RGBA8 */
    case 0xE: bw = 8; bh = 8; bpb = 32; break;   /* CMPR */
    default: bw = 4; bh = 4; bpb = 32; break;
    }
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpb;
}

static u32 hash_bytes(const u8 *p, int n) {
    u32 h = 2166136261u;
    int step = n > 4096 ? n / 1024 : 1;
    int i;
    for (i = 0; i < n; i += step) h = (h ^ p[i]) * 16777619u;
    if (n) h = (h ^ p[n - 1]) * 16777619u;
    return h;
}

static void put_px(u8 *dst, int w, int h, int x, int y, u8 r, u8 g, u8 b, u8 a) {
    u8 *p;
    if (x >= w || y >= h) return;
    p = dst + (y * w + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

static void decode_rgb5a3(u16 v, u8 *r, u8 *g, u8 *b, u8 *a) {
    if (v & 0x8000) {
        *r = (u8)(((v >> 10) & 31) * 255 / 31);
        *g = (u8)(((v >> 5) & 31) * 255 / 31);
        *b = (u8)((v & 31) * 255 / 31);
        *a = 255;
    } else {
        *r = (u8)(((v >> 8) & 15) * 17);
        *g = (u8)(((v >> 4) & 15) * 17);
        *b = (u8)((v & 15) * 17);
        *a = (u8)(((v >> 12) & 7) * 255 / 7);
    }
}

static void rgb565(u16 v, u8 *c) {
    c[0] = (u8)(((v >> 11) & 31) * 255 / 31);
    c[1] = (u8)(((v >> 5) & 63) * 255 / 63);
    c[2] = (u8)((v & 31) * 255 / 31);
}

static void decode_texture(const u8 *src, int w, int h, int fmt, u8 *dst) {
    int bx, by, x, y;
    switch (fmt) {
    case 0: /* I4 */
        for (by = 0; by < h; by += 8)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 8; y++)
                    for (x = 0; x < 8; x += 2) {
                        u8 v = *src++;
                        u8 i0 = (u8)((v >> 4) * 17), i1 = (u8)((v & 15) * 17);
                        put_px(dst, w, h, bx + x, by + y, i0, i0, i0, i0);
                        put_px(dst, w, h, bx + x + 1, by + y, i1, i1, i1, i1);
                    }
        break;
    case 1: /* I8 */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 8; x++) {
                        u8 i = *src++;
                        put_px(dst, w, h, bx + x, by + y, i, i, i, i);
                    }
        break;
    case 2: /* IA4 */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 8)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 8; x++) {
                        u8 v = *src++;
                        u8 i = (u8)((v & 15) * 17), a = (u8)((v >> 4) * 17);
                        put_px(dst, w, h, bx + x, by + y, i, i, i, a);
                    }
        break;
    case 3: /* IA8 */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 4)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++) {
                        u8 a = src[0], i = src[1];
                        src += 2;
                        put_px(dst, w, h, bx + x, by + y, i, i, i, a);
                    }
        break;
    case 4: /* RGB565 */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 4)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++) {
                        u8 c[3];
                        rgb565((u16)((src[0] << 8) | src[1]), c);
                        src += 2;
                        put_px(dst, w, h, bx + x, by + y, c[0], c[1], c[2], 255);
                    }
        break;
    case 5: /* RGB5A3 */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 4)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++) {
                        u8 r, g, b, a;
                        decode_rgb5a3((u16)((src[0] << 8) | src[1]), &r, &g, &b, &a);
                        src += 2;
                        put_px(dst, w, h, bx + x, by + y, r, g, b, a);
                    }
        break;
    case 6: /* RGBA8: AR block then GB block */
        for (by = 0; by < h; by += 4)
            for (bx = 0; bx < w; bx += 4) {
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 4; x++) {
                        int i = y * 4 + x;
                        u8 a = src[i * 2], r = src[i * 2 + 1], g = src[32 + i * 2], b = src[32 + i * 2 + 1];
                        put_px(dst, w, h, bx + x, by + y, r, g, b, a);
                    }
                src += 64;
            }
        break;
    case 0xE: /* CMPR: 8x8 tiles of four 4x4 DXT1 sub-blocks, big endian */
        for (by = 0; by < h; by += 8)
            for (bx = 0; bx < w; bx += 8) {
                int sb;
                for (sb = 0; sb < 4; sb++) {
                    int sx = bx + (sb & 1) * 4, sy = by + (sb >> 1) * 4;
                    u16 c0 = (u16)((src[0] << 8) | src[1]), c1 = (u16)((src[2] << 8) | src[3]);
                    u8 pal[4][4];
                    rgb565(c0, pal[0]);
                    rgb565(c1, pal[1]);
                    pal[0][3] = pal[1][3] = 255;
                    if (c0 > c1) {
                        int k;
                        for (k = 0; k < 3; k++) {
                            pal[2][k] = (u8)((2 * pal[0][k] + pal[1][k]) / 3);
                            pal[3][k] = (u8)((pal[0][k] + 2 * pal[1][k]) / 3);
                        }
                        pal[2][3] = pal[3][3] = 255;
                    } else {
                        int k;
                        for (k = 0; k < 3; k++) {
                            pal[2][k] = (u8)((pal[0][k] + pal[1][k]) / 2);
                            pal[3][k] = 0;
                        }
                        pal[2][3] = 255;
                        pal[3][3] = 0;
                    }
                    for (y = 0; y < 4; y++) {
                        u8 row = src[4 + y];
                        for (x = 0; x < 4; x++) {
                            int idx = (row >> (6 - x * 2)) & 3;
                            put_px(dst, w, h, sx + x, sy + y, pal[idx][0], pal[idx][1], pal[idx][2], pal[idx][3]);
                        }
                    }
                    src += 8;
                }
            }
        break;
    default:
        port_log("gx: unsupported texture format %d", fmt);
        memset(dst, 0xFF, (size_t)w * h * 4);
        break;
    }
}

static GLenum wrap_gl(int w) {
    switch (w) {
    case 0: return GL_CLAMP_TO_EDGE;
    case 2: return GL_MIRRORED_REPEAT;
    default: return GL_REPEAT;
    }
}

#define TEX_HINT_SIZE 4096
static short g_texhint[TEX_HINT_SIZE]; /* image address -> cache slot + 1 */

static void rt_upload(int slot, int w, int h, u8 *rgba); /* render-thread command */

/* game thread: validate the cached copy of a texture (hash of its data) and
 * queue a decode + upload when it is new or changed. Returns the cache slot. */
static u32 quick_hash(const u8 *p, int n) {
    /* 64 words spread over the image + both ends: cheap per-frame change check */
    u32 h = 2166136261u, w;
    int i, step = n / 64;
    if (n < 512) return hash_bytes(p, n);
    for (i = 0; i < 64; i++) {
        memcpy(&w, p + i * step, 4);
        h = (h ^ w) * 16777619u;
    }
    memcpy(&w, p + n - 4, 4);
    return (h ^ w) * 16777619u;
}

#define TEX_FULL_CHECK_FRAMES 120 /* full re-hash at least every 2 s */

static int get_texture(const PortTexObj *t) {
    int i, size, slot = -1;
    u32 h, q, oldest = 0xFFFFFFFFu;
    u8 *rgba;
    TexCacheEntry *e = NULL;
    unsigned hint;
    if (t->magic != TEXOBJ_MAGIC || !t->image || !t->width || !t->height) return -1;
    hint = (unsigned)(((uintptr_t)t->image >> 4) * 2654435761u) % TEX_HINT_SIZE;
    if (g_texhint[hint]) {
        e = &g_texcache[g_texhint[hint] - 1];
        if (e->used && e->image == t->image && e->w == t->width && e->h == t->height && e->fmt == t->format) {
            slot = g_texhint[hint] - 1;
            if (e->checked == g_frame) {
                e->last_use = g_frame;
                return slot;
            }
        } else {
            e = NULL;
        }
    }
    if (!e) {
        for (i = 0; i < TEX_CACHE_SIZE; i++) {
            TexCacheEntry *x = &g_texcache[i];
            if (x->used && x->image == t->image && x->w == t->width && x->h == t->height && x->fmt == t->format) {
                e = x;
                slot = i;
                g_texhint[hint] = (short)(i + 1);
                break;
            }
        }
    }
    size = tex_size(t->format, t->width, t->height);
    {
        PROF_BEGIN(PROF_TEXHASH);
        q = quick_hash(t->image, size);
        h = 0;
        if (e && e->checked != 0 && q == e->qhash && g_frame - e->full_frame < TEX_FULL_CHECK_FRAMES) {
            h = e->hash; /* unchanged (sparse check) */
        } else {
            h = hash_bytes(t->image, size);
            if (e && e->hash == h) {
                e->full_frame = g_frame;
                e->qhash = q;
            }
        }
        PROF_END(PROF_TEXHASH);
    }
    if (e && e->hash == h) {
        e->last_use = g_frame;
        e->checked = g_frame;
        return slot;
    }
    if (slot < 0) {
        for (i = 0; i < TEX_CACHE_SIZE; i++) {
            if (!g_texcache[i].used) { slot = i; break; }
            if (g_texcache[i].last_use < oldest) { oldest = g_texcache[i].last_use; slot = i; }
        }
    }
    g_texhint[hint] = (short)(slot + 1);
    PROF_BEGIN(PROF_TEXLOAD);
    e = &g_texcache[slot];
    e->used = 1;
    e->image = t->image;
    e->w = t->width;
    e->h = t->height;
    e->fmt = t->format;
    e->hash = h;
    e->qhash = q;
    e->full_frame = g_frame;
    e->last_use = g_frame;
    e->checked = g_frame;
    rgba = malloc((size_t)t->width * t->height * 4);
    decode_texture(t->image, t->width, t->height, t->format, rgba);
    {
        const u8 *a = rgba + 3, *end = rgba + (size_t)t->width * t->height * 4;
        int m = 255;
        for (; a < end && m; a += 4)
            if (*a < m) m = *a;
        e->amin = m;
    }
    if (getenv("CWOC_TEXDUMP")) {
        static int dumped;
        char path[256];
        FILE *f;
        snprintf(path, sizeof(path), "%s/tex_%03d_%dx%d_f%d.ppm", getenv("CWOC_TEXDUMP"), dumped++, t->width, t->height, t->format);
        f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", t->width, t->height);
            for (i = 0; i < t->width * t->height; i++) fwrite(rgba + i * 4, 1, 3, f);
            fclose(f);
        }
    }
    PROF_END(PROF_TEXLOAD);
    rt_upload(slot, t->width, t->height, rgba); /* the render thread frees rgba */
    return slot;
}

/* render thread: (re)create the GL texture of a cache slot */
static int g_mipmaps = 1;
static void exec_upload(int slot, int w, int h, u8 *rgba) {
    TexCacheEntry *e = &g_texcache[slot];
    if (!e->gltex) {
        glGenTextures(1, &e->gltex);
        e->wrap_s = e->wrap_t = -1;
    }
    bind_tex(7, e->gltex); /* scratch unit: do not disturb the units of the draw */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    /* The GameCube version never uses mipmaps; at 960x544 that shimmers on
     * distant geometry and thrashes the texture cache. Build a chain for
     * power-of-two textures (CWOC_MIPMAPS=0: original look). */
    if (g_mipmaps && w >= 8 && h >= 8 && !(w & (w - 1)) && !(h & (h - 1))) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    free(rgba);
}

void GXInitTexObj(void *obj, void *image, u16 width, u16 height, int format, int wrap_s, int wrap_t, u8 mipmap) { GXO();
    PortTexObj *t = obj;
    memset(t, 0, sizeof(*t));
    t->image = image;
    t->width = width;
    t->height = height;
    t->format = (u8)format;
    t->wrap_s = (u8)wrap_s;
    t->wrap_t = (u8)wrap_t;
    t->mipmap = mipmap;
    t->magic = TEXOBJ_MAGIC;
}
void GXInitTexObjWrapMode(void *obj, int s, int t) { GXO();
    PortTexObj *o = obj;
    o->wrap_s = (u8)s;
    o->wrap_t = (u8)t;
}
void GXInitTexObjLOD(void *obj, int min, int mag, float minlod, float maxlod, float bias, u8 clamp, u8 edge, int aniso) { GXO();
    (void)obj; (void)min; (void)mag; (void)minlod; (void)maxlod; (void)bias; (void)clamp; (void)edge; (void)aniso;
}
void GXLoadTexObj(void *obj, int map) { GXO();
    if (map < 0 || map >= MAX_TEXMAP) return;
    if (gx.texmap_valid[map] && !memcmp(&gx.texmap[map], obj, sizeof(PortTexObj))) return;
    g_dirty = 1;
    gx.texmap[map] = *(PortTexObj *)obj;
    gx.texmap_valid[map] = gx.texmap[map].magic == TEXOBJ_MAGIC;
}
void GXInvalidateTexAll(void) { GXO(); g_dirty = 1;}
u32 GXGetTexBufferSize(u16 w, u16 h, u32 fmt, u8 mipmap, u8 maxlod) { GXO();
    int size = tex_size((int)fmt, w, h);
    if (mipmap) {
        int lw = w, lh = h, l;
        for (l = 1; l < (maxlod ? maxlod : 10); l++) {
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
            size += tex_size((int)fmt, lw, lh);
            if (lw == 1 && lh == 1) break;
        }
    }
    return (u32)size;
}

/* -------------------------------------------------------------- matrices */
static unsigned g_mtx_ver = 1; /* bumped by every matrix load/select (seg_add) */
void GXLoadPosMtxImm(const float m[3][4], u32 id) { GXO();
    int slot = (int)(id / 3);
    if (slot < 0 || slot >= MAX_POSMTX) return;
    memcpy(gx.posmtx[slot], m, sizeof(gx.posmtx[slot]));
    g_mtx_ver++;
}
void GXLoadNrmMtxImm(const float m[3][4], u32 id) { GXO();
    int slot = (int)(id / 3), r, c;
    if (slot < 0 || slot >= MAX_POSMTX) return;
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++) gx.nrmmtx[slot][r][c] = m[r][c];
    g_mtx_ver++;
}
void GXLoadTexMtxImm(const float m[3][4], u32 id, int type) { GXO(); (void)m; (void)id; (void)type; }
void GXSetCurrentMtx(u32 id) { GXO(); gx.cur_posmtx = (int)(id / 3); g_mtx_ver++; }
void GXSetProjection(const float m[4][4], int type) { GXO();
    if (gx.proj_ortho == type && !memcmp(gx.proj, m, sizeof(gx.proj))) return;
    g_dirty = 1;
    memcpy(gx.proj, m, sizeof(gx.proj));
    gx.proj_ortho = type;
}

/* -------------------------------------------------------------- lighting */
void GXSetNumChans(u8 n) { GXO(); if (gx.num_chans != n) { gx.num_chans = n; g_dirty = 1; } }
void GXSetChanCtrl(int chan, u8 enable, int amb_src, int mat_src, u32 light_mask, int diff_fn, int attn_fn) { GXO();
    ChanCtrl c;
    c.enable = enable;
    c.amb_src = amb_src;
    c.mat_src = mat_src;
    c.light_mask = (int)light_mask;
    c.diff_fn = diff_fn;
    c.attn_fn = attn_fn;
    /* GX_COLOR0 = 0, GX_ALPHA0 = 2, GX_COLOR0A0 = 4 (setters only dirty the batch state on a change) */
    /* the game often flips a channel and flips it back between two blocks: only
     * flag it here, draw_block compares against the captured channels */
    if ((chan == 0 || chan == 4) && memcmp(&gx.chan[0], &c, sizeof(c))) { gx.chan[0] = c; g_dirty_chan = 1; }
    if ((chan == 2 || chan == 4) && memcmp(&gx.chan[1], &c, sizeof(c))) { gx.chan[1] = c; g_dirty_chan = 1; }
}
/* NOTE: amb/mat keep the always-dirty form: the conditional form changes the
 * hub lighting (Coco renders dark) for a reason not found yet (suspect an
 * uninitialised read in game code); see VITA_PORT_CHECKPOINT.md */
void GXSetChanAmbColor(int chan, GXColor c) { GXO(); (void)chan; if (memcmp(&gx.amb_color, &c, sizeof(c))) { gx.amb_color = c; g_dirty = 1; } }
void GXSetChanMatColor(int chan, GXColor c) { GXO(); (void)chan; if (memcmp(&gx.mat_color, &c, sizeof(c))) { gx.mat_color = c; g_dirty = 1; } }
void GXInitLightPos(void *obj, f32 x, f32 y, f32 z) { GXO();
    PortLightObj *l = obj;
    l->pos[0] = x; l->pos[1] = y; l->pos[2] = z;
    l->magic = LIGHTOBJ_MAGIC;
}
void GXInitLightDir(void *obj, f32 x, f32 y, f32 z) { GXO();
    PortLightObj *l = obj;
    l->dir[0] = x; l->dir[1] = y; l->dir[2] = z;
}
void GXInitLightColor(void *obj, GXColor c) { GXO(); ((PortLightObj *)obj)->color = c; }
void GXLoadLightObjImm(void *obj, u32 light) { GXO();
    int i;
    PortLightObj *l = obj;
    for (i = 0; i < 8; i++)
        if (light == (1u << i)) {
            LightSlot n;
            memcpy(n.pos, l->pos, sizeof(l->pos));
            memcpy(n.dir, l->dir, sizeof(l->dir));
            n.color = l->color;
            if (memcmp(&gx.lights[i], &n, sizeof(n))) { gx.lights[i] = n; g_dirty = 1; }
        }
}

/* -------------------------------------------------------------- TEV */
void GXSetNumTevStages(u8 n) { GXO(); if (gx.num_tev != n) { gx.num_tev = n; g_dirty = 1; } }
void GXSetNumTexGens(u8 n) { GXO(); if (gx.num_texgens != n) { gx.num_texgens = n; g_dirty = 1; } }
void GXSetTexCoordGen2(int dst, int func, int src, u32 mtx, u8 normalize, u32 postmtx) { GXO();
    (void)dst; (void)func; (void)src; (void)mtx; (void)normalize; (void)postmtx;
}
void GXSetTevOrder(int stage, int coord, int map, int color) { GXO();
    if (stage < 0 || stage >= 16) return;
    if (gx.tev[stage].texcoord == (coord & 0xFF) && gx.tev[stage].texmap == (map & 0xFF) && gx.tev[stage].channel == (color & 0xFF)) return;
    g_dirty = 1;
    gx.tev[stage].texcoord = coord & 0xFF;
    gx.tev[stage].texmap = map & 0xFF;
    gx.tev[stage].channel = color & 0xFF;
}
void GXSetTevColorIn(int stage, int a, int b, int c, int d) { GXO();
    TevStage *t = &gx.tev[stage & 15], old = *t;
    t->color_in[0] = a; t->color_in[1] = b; t->color_in[2] = c; t->color_in[3] = d;
    if (memcmp(&old, t, sizeof(old))) g_dirty = 1;
}
void GXSetTevAlphaIn(int stage, int a, int b, int c, int d) { GXO();
    TevStage *t = &gx.tev[stage & 15], old = *t;
    t->alpha_in[0] = a; t->alpha_in[1] = b; t->alpha_in[2] = c; t->alpha_in[3] = d;
    if (memcmp(&old, t, sizeof(old))) g_dirty = 1;
}
void GXSetTevColorOp(int stage, int op, int bias, int scale, int clamp, int reg) { GXO();
    TevStage *t = &gx.tev[stage & 15], old = *t;
    t->color_op = op & 1; t->color_bias = bias; t->color_scale = scale; t->color_clamp = clamp; t->color_reg = reg;
    if (memcmp(&old, t, sizeof(old))) g_dirty = 1;
}
void GXSetTevAlphaOp(int stage, int op, int bias, int scale, int clamp, int reg) { GXO();
    TevStage *t = &gx.tev[stage & 15], old = *t;
    t->alpha_op = op & 1; t->alpha_bias = bias; t->alpha_scale = scale; t->alpha_clamp = clamp; t->alpha_reg = reg;
    if (memcmp(&old, t, sizeof(old))) g_dirty = 1;
}
/* SDK GXSetTevOp presets; stage 0 uses the rasterised colour as "previous". */
void GXSetTevOp(int stage, int mode) { GXO();
    int carg = stage == 0 ? 10 : 0, aarg = stage == 0 ? 5 : 0;
    switch (mode) {
    case 0: /* MODULATE */
        GXSetTevColorIn(stage, 15, 8, carg, 15);
        GXSetTevAlphaIn(stage, 7, 4, aarg, 7);
        break;
    case 1: /* DECAL */
        GXSetTevColorIn(stage, carg, 8, 9, 15);
        GXSetTevAlphaIn(stage, 7, 7, 7, aarg);
        break;
    case 2: /* BLEND */
        GXSetTevColorIn(stage, carg, 12, 8, 15);
        GXSetTevAlphaIn(stage, 7, 4, aarg, 7);
        break;
    case 3: /* REPLACE */
        GXSetTevColorIn(stage, 15, 15, 15, 8);
        GXSetTevAlphaIn(stage, 7, 7, 7, 4);
        break;
    default: /* PASSCLR */
        GXSetTevColorIn(stage, 15, 15, 15, carg);
        GXSetTevAlphaIn(stage, 7, 7, 7, aarg);
        break;
    }
    GXSetTevColorOp(stage, 0, 0, 0, 1, 0);
    GXSetTevAlphaOp(stage, 0, 0, 0, 1, 0);
}
void GXSetTevColor(int reg, GXColor c) { GXO(); (void)reg; (void)c; } /* not part of the captured state */

/* -------------------------------------------------------------- pixel state */
void GXSetAlphaCompare(int comp0, u8 ref0, int op, int comp1, u8 ref1) { GXO();
    if (gx.alpha_comp0 == comp0 && gx.alpha_ref0 == ref0 && gx.alpha_op == op && gx.alpha_comp1 == comp1 && gx.alpha_ref1 == ref1) return;
    g_dirty = 1;
    gx.alpha_comp0 = comp0; gx.alpha_ref0 = ref0; gx.alpha_op = op; gx.alpha_comp1 = comp1; gx.alpha_ref1 = ref1;
}
void GXSetFog(int type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) { GXO();
    static int logfog = -1;
    (void)nearz; (void)farz;
    if (type == gx.fog_type && startz == gx.fog_start && endz == gx.fog_end && !memcmp(&color, &gx.fog_color, sizeof(color)))
        return;
    g_dirty = 1;
    if (logfog < 0) logfog = getenv("CWOC_LOGFOG") != NULL;
    if (logfog && (type != gx.fog_type || startz != gx.fog_start || endz != gx.fog_end ||
                                  memcmp(&color, &gx.fog_color, sizeof(color))))
        port_log("fog type %d start %.1f end %.1f near %.1f far %.1f col %02x%02x%02x%02x", type, startz, endz, nearz, farz,
                 color.r, color.g, color.b, color.a);
    gx.fog_type = type;
    gx.fog_start = startz;
    gx.fog_end = endz;
    gx.fog_color = color;
}
void GXSetBlendMode(int type, int src, int dst, int op) { GXO();
    (void)op;
    if (gx.blend_type == type && gx.blend_src == src && gx.blend_dst == dst) return;
    g_dirty = 1;
    gx.blend_type = type;
    gx.blend_src = src;
    gx.blend_dst = dst;
}
void GXSetZMode(u8 enable, int func, u8 update) { GXO();
    if (gx.z_enable == enable && gx.z_func == func && gx.z_update == update) return;
    g_dirty = 1;
    gx.z_enable = enable;
    gx.z_func = func;
    gx.z_update = update;
}
/* (the setters only mark the state dirty when a captured value changes:
 * capture_state + compare runs on every draw after a dirty mark) */
void GXSetZCompLoc(u8 before_tex) { GXO(); (void)before_tex; }
void GXSetAlphaUpdate(u8 enable) { GXO(); if (gx.alpha_update != enable) { g_dirty = 1; gx.alpha_update = enable; } }
void GXSetColorUpdate(u8 enable) { GXO(); (void)enable; }
void GXSetCullMode(int mode) { GXO(); if (gx.cull != mode) { g_dirty = 1; gx.cull = mode; } }
void GXSetClipMode(int mode) { GXO(); (void)mode; }
void GXSetDither(u8 d) { GXO(); (void)d; }
void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) { GXO();
    if (gx.vp[0] == left && gx.vp[1] == top && gx.vp[2] == wd && gx.vp[3] == ht && gx.vp[4] == nearz && gx.vp[5] == farz) return;
    g_dirty = 1;
    gx.vp[0] = left; gx.vp[1] = top; gx.vp[2] = wd; gx.vp[3] = ht; gx.vp[4] = nearz; gx.vp[5] = farz;
}
void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) { GXO();
    if (gx.scissor[0] == (int)left && gx.scissor[1] == (int)top && gx.scissor[2] == (int)wd && gx.scissor[3] == (int)ht) return;
    g_dirty = 1;
    gx.scissor[0] = (int)left; gx.scissor[1] = (int)top; gx.scissor[2] = (int)wd; gx.scissor[3] = (int)ht;
}
void GXSetCopyClear(GXColor color, u32 z) { GXO(); (void)z; gx.copy_clear = color; } /* used by the copy, not draws */
void GXPixModeSync(void) { GXO();}
void GXDrawDone(void) { GXO();}
void GXFlush(void) { GXO();}
void GXCopyDisp(void *dest, u8 clear) { GXO(); (void)dest; (void)clear; }

/* -------------------------------------------------------------- draw */
static GLenum blend_factor(int f, int is_src) {
    switch (f) {
    case 0: return GL_ZERO;
    case 1: return GL_ONE;
    case 2: return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 3: return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 4: return GL_SRC_ALPHA;
    case 5: return GL_ONE_MINUS_SRC_ALPHA;
    case 6: return GL_DST_ALPHA;
    default: return GL_ONE_MINUS_DST_ALPHA;
    }
}

/* ---------------------------------------------------------- batching
 * The game issues one GXBegin block per strip/fan (thousands per frame).
 * Consecutive blocks with an identical render state are merged into one
 * triangle list; the GL state is applied from the captured snapshot. */
typedef struct {
    u32 key[8];
    int blend_type, blend_src, blend_dst, z_enable, z_func, z_update, cull, alpha_update;
    float vp[6];
    int scissor[4];
    int num_tev;
    PortTexObj tex[MAX_TEV];
    int texvalid[MAX_TEV];
    float proj[4][4];
    float posmtx[3][4];
    float nrmmtx[3][3];
    GXColor amb, mat;
    LightSlot lights[8];
    int aref0, aref1;
    float fog_start, fog_end;
    GXColor fog_color;
} BatchState;

static BatchState g_bstate;      /* state of the pending batch */
static u32 g_bstate_ver;         /* bumped whenever g_bstate is replaced */
static ShaderEntry *g_bshader;
/* the pending batch is built in place in the render queue's vertex/index arenas */
static GLVertex *g_bverts;  /* = arena vertices + batch start */
static u16 *g_bidx;         /* = arena indices + batch start */
static int g_bn, g_bni;     /* vertices / indices in the pending batch */
static u32 g_batch_epoch;   /* bumped whenever the pending batch restarts */
static int g_batch_on = -1;
static int g_draw_calls;
static int g_breakstat = -1, g_breaks[16], g_breaks_frame;

/* did anything the batch state depends on change since the last capture? */
static inline int state_dirty(void) {
    if (g_dirty) return 1;
    if (g_dirty_chan) {
        if (memcmp(g_chan_snap, gx.chan, sizeof(g_chan_snap))) return 1;
        g_dirty_chan = 0;
    }
    return 0;
}

static void capture_state(BatchState *b) {
    int i;
    memcpy(g_chan_snap, gx.chan, sizeof(g_chan_snap));
    g_dirty_chan = 0;
    memset(b, 0, sizeof(*b));
    build_key(b->key);
    b->blend_type = gx.blend_type; b->blend_src = gx.blend_src; b->blend_dst = gx.blend_dst;
    b->z_enable = gx.z_enable; b->z_func = gx.z_func; b->z_update = gx.z_update;
    b->cull = gx.cull; b->alpha_update = gx.alpha_update;
    memcpy(b->vp, gx.vp, sizeof(b->vp));
    memcpy(b->scissor, gx.scissor, sizeof(b->scissor));
    b->num_tev = gx.num_tev;
    for (i = 0; i < MAX_TEV && i < gx.num_tev; i++) {
        int map = gx.tev[i].texmap;
        if (map != 0xFF && gx.texmap_valid[map & 7]) {
            b->texvalid[i] = 1;
            b->tex[i] = gx.texmap[map & 7];
            memset(b->tex[i].pad, 0, sizeof(b->tex[i].pad));
        }
    }
    memcpy(b->proj, gx.proj, sizeof(b->proj));
    /* vertices are pre-transformed to view space on the CPU (draw_block), so
     * the model matrix does not split batches */
    b->posmtx[0][0] = b->posmtx[1][1] = b->posmtx[2][2] = 1.0f;
    b->nrmmtx[0][0] = b->nrmmtx[1][1] = b->nrmmtx[2][2] = 1.0f;
    b->amb = gx.amb_color; b->mat = gx.mat_color;
    if (chan_lit()) { /* packed: the lights COLOR0 uses, in index order (build_key) */
        int n = 0;
        for (i = 0; i < 8; i++)
            if (gx.chan[0].light_mask & (1 << i)) b->lights[n++] = gx.lights[i];
    }
    b->aref0 = gx.alpha_ref0; b->aref1 = gx.alpha_ref1;
    if (gx.fog_type) { b->fog_start = gx.fog_start; b->fog_end = gx.fog_end; b->fog_color = gx.fog_color; }
}

/* shadow of the GL raster state set by apply_state */
static struct {
    int valid;
    int vp[4], sc[4];
    float dr[2];
    int blend, bsrc, bdst, beq, zen, zfunc, zmask, amask, cull;
    GLuint prog;
} g_gl;

static int g_blend10; /* CWOC_BLEND10=1: GX blend (1, 0) drawn with GL blending (as before) */
static void apply_state(const BatchState *b) {
    static const GLenum zfunc[8] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
    float sx = (float)g_draw_w / 640.0f, sy = (float)g_draw_h / 448.0f;
    int vx = g_off_x + (int)(b->vp[0] * sx), vw = (int)(b->vp[2] * sx);
    int vh = (int)(b->vp[3] * sy);
    int vy = g_win_h - (g_off_y + (int)(b->vp[1] * sy)) - vh;
    int sw = (int)(b->scissor[2] * sx), sh = (int)(b->scissor[3] * sy);
    int scx = g_off_x + (int)(b->scissor[0] * sx);
    int scy = g_win_h - (g_off_y + (int)(b->scissor[1] * sy)) - sh;
    int blend, bsrc = 0, bdst = 0, beq = 0, cull;
    int v = g_gl.valid;
    if (g_rt_lowres) {
        r3_map(&vx, &vy, &vw, &vh);
        r3_map(&scx, &scy, &sw, &sh);
    }
    if (g_gpuexp == 'q') {
        vx /= 2; vy /= 2; vw /= 2; vh /= 2;
        scx /= 2; scy /= 2; sw /= 2; sh /= 2;
    } else if (g_gpuexp == 'v') {
        scx = scy = 0;
        sw = sh = 1;
    }
    if (!v || g_gl.vp[0] != vx || g_gl.vp[1] != vy || g_gl.vp[2] != vw || g_gl.vp[3] != vh) {
        glViewport(vx, vy, vw, vh);
        g_gl.vp[0] = vx; g_gl.vp[1] = vy; g_gl.vp[2] = vw; g_gl.vp[3] = vh;
    }
    if (!v || g_gl.dr[0] != b->vp[4] || g_gl.dr[1] != b->vp[5]) {
        glDepthRange(b->vp[4], b->vp[5]);
        g_gl.dr[0] = b->vp[4]; g_gl.dr[1] = b->vp[5];
    }
    if (!v) glEnable(GL_SCISSOR_TEST);
    if (!v || g_gl.sc[0] != scx || g_gl.sc[1] != scy || g_gl.sc[2] != sw || g_gl.sc[3] != sh) {
        glScissor(scx, scy, sw, sh);
        g_gl.sc[0] = scx; g_gl.sc[1] = scy; g_gl.sc[2] = sw; g_gl.sc[3] = sh;
    }
    if (b->blend_type == 1) {
        blend = 1; beq = GL_FUNC_ADD;
        bsrc = (int)blend_factor(b->blend_src, 1); bdst = (int)blend_factor(b->blend_dst, 0);
        /* GX blend (1, 0) is how the game draws its opaque geometry: src*1 + dst*0
         * is the source, so draw it without blending. A blending draw is not
         * opaque to the Vita GPU (no hidden surface removal: every covered
         * pixel is shaded). CWOC_BLEND10=1 keeps the blending (A/B). */
        if (bsrc == GL_ONE && bdst == GL_ZERO && !g_blend10) blend = 0;
    } else if (b->blend_type == 3) {
        blend = 1; beq = GL_FUNC_REVERSE_SUBTRACT; bsrc = GL_ONE; bdst = GL_ONE;
    } else {
        blend = 0;
    }
    if (!v || g_gl.blend != blend) {
        if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        g_gl.blend = blend;
    }
    if (blend && (!v || g_gl.beq != beq)) { glBlendEquation(beq); g_gl.beq = beq; }
    if (blend && (!v || g_gl.bsrc != bsrc || g_gl.bdst != bdst)) {
        glBlendFunc(bsrc, bdst);
        g_gl.bsrc = bsrc; g_gl.bdst = bdst;
    }
    if (!v || g_gl.zen != b->z_enable) {
        if (b->z_enable) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        g_gl.zen = b->z_enable;
    }
    if (b->z_enable && (!v || g_gl.zfunc != (b->z_func & 7))) {
        glDepthFunc(zfunc[b->z_func & 7]);
        g_gl.zfunc = b->z_func & 7;
    }
    if (!v || g_gl.zmask != (b->z_update != 0)) {
        glDepthMask(b->z_update ? GL_TRUE : GL_FALSE);
        g_gl.zmask = b->z_update != 0;
    }
    if (!v || g_gl.amask != (b->alpha_update != 0)) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, b->alpha_update ? GL_TRUE : GL_FALSE);
        g_gl.amask = b->alpha_update != 0;
    }
    cull = b->cull;
    if (!v || g_gl.cull != cull) {
        if (cull == 0) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glFrontFace(GL_CW);
            glCullFace(cull == 1 ? GL_FRONT : cull == 2 ? GL_BACK : GL_FRONT_AND_BACK);
        }
        g_gl.cull = cull;
    }
    g_gl.valid = 1;
}

static void set_uniforms(ShaderEntry *s, const BatchState *b, const int *texslot) {
    float m4[16], n3[9], lp[24], lc[32];
    int i, r, c, first = !s->uset;
    /* GL wants column-major */
    if (first || memcmp(s->last_proj, b->proj, sizeof(b->proj))) {
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++) m4[c * 4 + r] = b->proj[r][c];
        glUniformMatrix4fv(s->u_proj, 1, GL_FALSE, m4);
        memcpy(s->last_proj, b->proj, sizeof(b->proj));
    }
    if (first || memcmp(s->last_posmtx, b->posmtx, sizeof(b->posmtx))) {
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++) m4[c * 4 + r] = r < 3 ? b->posmtx[r][c] : (c == 3 ? 1.0f : 0.0f);
        glUniformMatrix4fv(s->u_posmtx, 1, GL_FALSE, m4);
        memcpy(s->last_posmtx, b->posmtx, sizeof(b->posmtx));
    }
    if (first || memcmp(s->last_nrmmtx, b->nrmmtx, sizeof(b->nrmmtx))) {
        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++) n3[c * 3 + r] = b->nrmmtx[r][c];
        glUniformMatrix3fv(s->u_nrmmtx, 1, GL_FALSE, n3);
        memcpy(s->last_nrmmtx, b->nrmmtx, sizeof(b->nrmmtx));
    }
    if (first || memcmp(&s->last_amb, &b->amb, sizeof(b->amb))) {
        glUniform4f(s->u_amb, b->amb.r / 255.0f, b->amb.g / 255.0f, b->amb.b / 255.0f, b->amb.a / 255.0f);
        s->last_amb = b->amb;
    }
    if (first || memcmp(&s->last_mat, &b->mat, sizeof(b->mat))) {
        glUniform4f(s->u_mat, b->mat.r / 255.0f, b->mat.g / 255.0f, b->mat.b / 255.0f, b->mat.a / 255.0f);
        s->last_mat = b->mat;
    }
    if ((s->u_lightpos >= 0 || s->u_lightcol >= 0) && (first || memcmp(s->last_lights, b->lights, sizeof(b->lights)))) {
        for (i = 0; i < 8; i++) {
            lp[i * 3 + 0] = b->lights[i].pos[0];
            lp[i * 3 + 1] = b->lights[i].pos[1];
            lp[i * 3 + 2] = b->lights[i].pos[2];
            lc[i * 4 + 0] = b->lights[i].color.r / 255.0f;
            lc[i * 4 + 1] = b->lights[i].color.g / 255.0f;
            lc[i * 4 + 2] = b->lights[i].color.b / 255.0f;
            lc[i * 4 + 3] = b->lights[i].color.a / 255.0f;
        }
        if (s->u_lightpos >= 0) glUniform3fv(s->u_lightpos, 8, lp);
        if (s->u_lightcol >= 0) glUniform4fv(s->u_lightcol, 8, lc);
        memcpy(s->last_lights, b->lights, sizeof(b->lights));
    }
    if (s->u_alpharef >= 0 && (first || s->last_aref0 != b->aref0 || s->last_aref1 != b->aref1)) {
        glUniform2f(s->u_alpharef, (float)b->aref0, (float)b->aref1);
        s->last_aref0 = b->aref0;
        s->last_aref1 = b->aref1;
    }
    if (s->u_fog >= 0 && (first || s->last_fog_start != b->fog_start || s->last_fog_end != b->fog_end ||
                          memcmp(&s->last_fogcol, &b->fog_color, sizeof(b->fog_color)))) {
        float range = b->fog_end - b->fog_start;
        glUniform3f(s->u_fog, b->fog_start, range != 0.0f ? 1.0f / range : 0.0f, 0.0f);
        glUniform3f(s->u_fogcol, b->fog_color.r / 255.0f, b->fog_color.g / 255.0f, b->fog_color.b / 255.0f);
        s->last_fog_start = b->fog_start;
        s->last_fog_end = b->fog_end;
        s->last_fogcol = b->fog_color;
    }
    for (i = 0; i < MAX_TEV && i < b->num_tev; i++) {
        if (b->texvalid[i] && texslot[i] >= 0) {
            const PortTexObj *to = &b->tex[i];
            TexCacheEntry *e = &g_texcache[texslot[i]];
            int ws = (int)wrap_gl(to->wrap_s), wt = (int)wrap_gl(to->wrap_t);
            bind_tex(i, e->gltex);
            if (e->wrap_s != ws) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws); e->wrap_s = ws; }
            if (e->wrap_t != wt) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt); e->wrap_t = wt; }
        } else {
            bind_tex(i, 0);
        }
        if (first && s->u_tex[i] >= 0) glUniform1i(s->u_tex[i], i);
    }
    s->uset = 1;
}

static GLuint g_ring_vbo;             /* Vita: buffer object over the GPU ring */
/* Static geometry (draw_indexed_range cache hits): model-space vertices and
 * page-relative indices kept in GPU memory, drawn with the model-view matrix
 * as a uniform instead of being transformed and copied every frame.
 * Allocated by the render thread (rt_ring_init), filled by the game thread
 * the first time a list is drawn. CWOC_STATICGEO=0 disables it. */
#define SG_PAGE_VERTS 65536           /* u16 indices address one page */
static GLVertex *g_sg_v;              /* SG pages of SG_PAGE_VERTS vertices */
static u16 *g_sg_i;                   /* index arena */
static int g_sg_npages;
static u32 g_sg_icap;                 /* indices in the arena */
static GLuint g_sg_vbo, g_sg_ibo;
static volatile int g_sg_ready;       /* arenas allocated (render thread -> game thread) */
static int g_sg_page, g_sg_pv;        /* game thread: page being filled, vertices used in it */
static u32 g_sg_in;                   /* game thread: indices used */
static int g_sg_hold;                 /* presents left before the arenas may be refilled */
unsigned g_sg_lists, g_sg_draws, g_sg_cc; /* stats: static lists drawn, GL draws, constant-colour fallbacks */
unsigned g_sg_made, g_sg_dyn, g_sg_full;  /* stats: lists copied, found rewritten, arena full */
static int g_vb_mode = -1;            /* render thread: bound vertex source: 0 ring, 1+page static */
static void gl_draw(ShaderEntry *s, const BatchState *b, const int *texslot, GLenum mode, const GLVertex *v, int n, const u16 *idx, int nidx) {
    ensure_program(s);
    apply_state(b);
    if (g_gl.prog != s->prog) {
        glUseProgram(s->prog);
        g_gl.prog = s->prog;
    }
    set_uniforms(s, b, texslot);
    {
        static int attribs_on;
        if (!attribs_on) {
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glEnableVertexAttribArray(2);
            glEnableVertexAttribArray(3);
            attribs_on = 1;
        }
    }
#ifdef PORT_VITA
    if (g_ring_vbo) {
        /* GPU-mapped vertices: point the buffer object at this draw's first
         * vertex (attribute offsets are 16-bit in sceGxm) */
        if (g_vb_mode != 0) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); /* client indices (copied by vitaGL) */
            glBindBuffer(GL_ARRAY_BUFFER, g_ring_vbo);
            vglBufferData(GL_ARRAY_BUFFER, v);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), (void *)offsetof(GLVertex, pos));
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), (void *)offsetof(GLVertex, nrm));
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GLVertex), (void *)offsetof(GLVertex, clr));
            glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex), (void *)offsetof(GLVertex, tex));
            g_vb_mode = 0;
        }
        vglBufferData(GL_ARRAY_BUFFER, v);
    } else
#endif
    {
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &v[0].pos);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &v[0].nrm);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GLVertex), &v[0].clr);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &v[0].tex);
    }
    if (idx) glDrawElements(mode, nidx, GL_UNSIGNED_SHORT, idx);
    else glDrawArrays(mode, 0, n);
    g_draw_calls++;
}

#ifdef PORT_VITA
/* render thread: a static draw (vertices and indices already in GPU memory) */
static void gl_draw_static(ShaderEntry *s, const BatchState *b, const int *texslot, const float *mq, int page,
                           u32 first, u32 n) {
    float m4[16], n3[9];
    int r, c;
    ensure_program(s);
    apply_state(b);
    if (g_gl.prog != s->prog) {
        glUseProgram(s->prog);
        g_gl.prog = s->prog;
    }
    set_uniforms(s, b, texslot);
    /* the model-view matrix the deferred transform would have applied */
    if (memcmp(s->last_posmtx, mq, sizeof(s->last_posmtx))) {
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++) m4[c * 4 + r] = r < 3 ? mq[r * 4 + c] : (c == 3 ? 1.0f : 0.0f);
        glUniformMatrix4fv(s->u_posmtx, 1, GL_FALSE, m4);
        memcpy(s->last_posmtx, mq, sizeof(s->last_posmtx));
    }
    if (memcmp(s->last_nrmmtx, mq + 12, sizeof(s->last_nrmmtx))) {
        for (r = 0; r < 3; r++)
            for (c = 0; c < 3; c++) n3[c * 3 + r] = mq[12 + r * 3 + c];
        glUniformMatrix3fv(s->u_nrmmtx, 1, GL_FALSE, n3);
        memcpy(s->last_nrmmtx, mq + 12, sizeof(s->last_nrmmtx));
    }
    {
        static int attribs_on;
        if (!attribs_on) {
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glEnableVertexAttribArray(2);
            glEnableVertexAttribArray(3);
            attribs_on = 1;
        }
    }
    if (g_vb_mode != 1 + page) {
        glBindBuffer(GL_ARRAY_BUFFER, g_sg_vbo);
        vglBufferData(GL_ARRAY_BUFFER, g_sg_v + (size_t)page * SG_PAGE_VERTS);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), (void *)offsetof(GLVertex, pos));
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), (void *)offsetof(GLVertex, nrm));
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GLVertex), (void *)offsetof(GLVertex, clr));
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex), (void *)offsetof(GLVertex, tex));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_sg_ibo);
        vglBufferData(GL_ELEMENT_ARRAY_BUFFER, g_sg_i);
        g_vb_mode = 1 + page;
    }
    glDrawElements(GL_TRIANGLES, (GLsizei)n, GL_UNSIGNED_SHORT, (void *)(uintptr_t)(first * 2u));
    g_draw_calls++;
}
#endif

/* ================================================================ render queue
 * The game thread records draws, texture uploads, clears, EFB copies and
 * presents into a per-frame command list; a render thread (its own CPU core on
 * Vita) executes the GL calls. Two frame buffers: the game builds frame N+1
 * while frame N is drawn. CWOC_RT=0 runs the commands inline instead. */
enum { CMD_DRAW, CMD_UPLOAD, CMD_CLEAR, CMD_COPY, CMD_PRESENT, CMD_DRAW_STATIC, CMD_COMPOSITE };

typedef struct {
    int type;
    union {
        struct { ShaderEntry *sh; GLenum mode; int voff, nv, ioff, ni; int st; int texslot[MAX_TEV]; } draw;
        /* static geometry in GPU memory: indices [first, first+n) of the index
         * arena, over vertex page 'page', with model-view matrix set 'mtx' */
        struct { ShaderEntry *sh; u32 first, n; int st, mtx, page; int texslot[MAX_TEV]; } sdraw;
        struct { int slot, w, h; u8 *rgba; } up;
        struct { GXColor col; int lowres; } clear;
        struct { void *dest; int w, h, rx, ry, rw, rh; } copy;
        struct { int frame; } present;
    } u;
#ifdef PORT_GPUPROF
    int gp; /* GPU profiler build: pass of the command (g_gpuprof_tag) */
#endif
} RCmd;

#define RQ_CMDS 8192
#define RQ_STATES RQ_CMDS /* at most one state per command */
#define RQ_MARGIN 32       /* commands a batch emit (draw + uploads) and a present may need */
#define RQ_VERTS 131072
#define RQ_IDX 393216
/* deferred view transform: the direct paths store model-space vertices and a
 * segment {first vertex, count, matrices}; the render thread transforms them
 * just before the draw that uses them (CWOC_RTXF=0: transform on the game
 * thread as before) */
typedef struct { int vfirst, count; unsigned epoch, ver; float m[3][4]; float q[3][3]; int skm; float one;
                 const void *src; u32 col; int constcol;
                 /* deferred index expansion (draw_indexed_range cache hit): the render
                  * thread writes rel[0..rel_n) + ibase to the frame's index arena at ixa */
                 const u16 *rel; u32 ixa, rel_n; u16 ibase;
                 /* cached static skin list: source vertex of each slot (else skw[].sk) */
                 const u16 *uniq; } XfSeg;
/* GS layer source vertex (GS_DrawTriList / strip lists) */
typedef struct { float x, y, z, nx, ny, nz; u32 diffuse; float u, v; } PortGSVertex;
#define RQ_SEGS 16384
/* deferred skinning (port_gs_skin_draw): per vertex weights/bone indices and
 * a copy of the 16 skin matrices per draw; the render thread skins the
 * model-space source position before the view transform */
typedef struct { float w0, w1; u8 i0, i1, i2, pad; u16 sk, pad2; } SkinW;
#define RQ_SKINM 1024        /* skin matrix sets (16 x 4x4 floats) per frame buffer */

typedef struct {
    RCmd *cmd;
    BatchState *st;   /* draw states (consecutive draws share one when equal) */
    GLVertex *v;
    u16 *ix;
    XfSeg *seg;
    volatile int nseg;      /* published segment count */
    SkinW *skw;             /* per vertex (deferred skinning) */
    float *skm;             /* RQ_SKINM sets of 256 floats */
    float *smtx;            /* RQ_SMTX model-view sets (3x4 + 3x3) of static draws */
} RFrame;
#define RQ_SMTX 4096

static RFrame g_rf[2];
static int g_pf;                      /* game thread: frame buffer being recorded */
static int g_nseg;                    /* game thread: segments recorded in g_rf[g_pf] */
static int g_nskm;                    /* game thread: skin matrix sets used in g_rf[g_pf] */
static int g_nsm;                     /* game thread: static matrix sets used in g_rf[g_pf] */
static int g_rtskin = -1;             /* deferred skinning on (CWOC_RTSKIN=0: off) */
static int g_rtcopy = -1;             /* indexed ranges converted on the render thread (CWOC_RTCOPY=0: off) */
static int g_seg_nomerge;
static SDL_threadID g_game_tid;       /* thread that records the render queue */
static const RFrame *g_rt_fr;         /* render thread: frame buffer of the current output (NULL: start a new one) */
/* game-side vertex work split by path (sampled with PROF_VERTS, inclusive) */
uint64_t g_vsub[5];
unsigned g_idxc_hit, g_idxc_miss;
#define VSUB(k, rate) do { if (_pon_PROF_VERTS) g_vsub[k] += (port_prof_now() - _prof_t_PROF_VERTS) * (rate); } while (0)
static size_t g_idxc_bytes;           /* index-list cache size (draw_indexed_range) */
static void idxc_flush(void);
static void idxc_free_done(void);
static int g_idxc_nfree;             /* evicted index lists waiting to be freed */
static size_t g_skc_bytes;           /* skin list cache size (port_gs_skin_draw) */
static int g_rt_seg[2];               /* render thread: segments transformed per frame buffer */
static int g_rtxf = -1;               /* deferred transform on (CWOC_RTXF=0: off) */
static int g_ncmd, g_nst, g_voff, g_ioff, g_npub_local; /* game thread counters for g_rf[g_pf] */
static int g_last_st_valid;

static int g_rt_threaded;
static SDL_mutex *g_rq_m;
static SDL_cond *g_rq_work, *g_rq_done;
static volatile int g_pub[2];         /* published command count per frame buffer */
static volatile int g_cf, g_ci;       /* render thread: frame buffer and next command */
static volatile int g_frames_done, g_frames_sub;

static void exec_cmds(int f, int from, int to);

/* render thread side: run what is published; returns 0 when nothing was there */
static int rq_consume(int block) {
    int f, from, to, ended;
    if (g_rt_threaded) SDL_LockMutex(g_rq_m);
    while (g_ci >= g_pub[g_cf]) {
        if (!block || !g_rt_threaded) {
            if (g_rt_threaded) SDL_UnlockMutex(g_rq_m);
            return 0;
        }
        g_ws_rt = "idle";
        SDL_CondWait(g_rq_work, g_rq_m);
        g_ws_rt = "-";
    }
    f = g_cf;
    from = g_ci;
    to = g_pub[f];
    if (g_rt_threaded) SDL_UnlockMutex(g_rq_m);
    exec_cmds(f, from, to);
    ended = g_rf[f].cmd[to - 1].type == CMD_PRESENT;
    if (g_rt_threaded) SDL_LockMutex(g_rq_m);
    g_ci = to;
    if (ended) {
        g_pub[f] = 0;
        g_rt_seg[f] = 0;
        g_rt_fr = NULL; /* next frame: next ring slot */
        g_ci = 0;
        g_cf = f ^ 1;
        g_frames_done++;
    }
    if (g_rt_threaded) {
        SDL_CondBroadcast(g_rq_done);
        SDL_UnlockMutex(g_rq_m);
    }
    return 1;
}

static void rq_publish(void) {
    g_rf[g_pf].nseg = g_nseg;
    if (g_rt_threaded) {
        SDL_LockMutex(g_rq_m);
        g_pub[g_pf] = g_ncmd;
        SDL_CondSignal(g_rq_work);
        SDL_UnlockMutex(g_rq_m);
    } else {
        g_pub[g_pf] = g_ncmd;
        while (rq_consume(0)) {}
    }
    g_npub_local = g_ncmd;
}

/* game thread time spent blocked on the render thread (frame pacing, port_os.c) */
uint64_t g_port_wait_us;

/* wait until the render thread executed everything recorded so far */
static void rq_sync(void) {
    rq_publish();
    if (!g_rt_threaded) return;
    {
        uint64_t w0 = port_prof_now();
        PROF_BEGIN(PROF_WAIT);
        WS_SET("rq_sync");
        SDL_LockMutex(g_rq_m);
        while (!(g_cf == g_pf && g_ci >= g_pub[g_pf])) SDL_CondWait(g_rq_done, g_rq_m);
        SDL_UnlockMutex(g_rq_m);
        WS_SET("-");
        PROF_END(PROF_WAIT);
        if (!g_rec_active) g_port_wait_us += port_prof_now() - w0;
    }
}

/* everything recorded was executed: restart the current frame buffer at 0 */
static void rq_restart(void) {
    rq_sync();
    if (g_rt_threaded) SDL_LockMutex(g_rq_m);
    g_pub[g_pf] = 0;
    g_rt_seg[g_pf] = 0;
    g_rt_fr = NULL; /* arena restarts at 0: new ring slot (the GPU may still read the old one) */
    g_ci = 0;
    if (g_rt_threaded) SDL_UnlockMutex(g_rq_m);
    g_ncmd = g_nst = g_voff = g_ioff = g_npub_local = 0;
    g_nseg = 0;
    g_nskm = 0;
    g_nsm = 0;
    g_rf[g_pf].nseg = 0;
    g_last_st_valid = 0;
}

static void batch_emit(void);

static RCmd *rq_cmd(void) {
    if (g_ncmd >= RQ_CMDS) port_fatal("gx: render queue overflow");
    GP_TAG_SET(&g_rf[g_pf].cmd[g_ncmd]);
    return &g_rf[g_pf].cmd[g_ncmd];
}

/* make room for ncmd commands and nv/ni more vertices/indices (beyond the pending batch) */
static void rq_ensure(int ncmd, int nv, int ni) {
    if (g_ncmd + ncmd + RQ_MARGIN > RQ_CMDS || g_voff + g_bn + nv > RQ_VERTS || g_ioff + g_bni + ni > RQ_IDX ||
        g_nseg + 8 > RQ_SEGS || g_nskm + 2 > RQ_SKINM || g_nsm + 2 > RQ_SMTX) {
        batch_emit();
        rq_restart();
        g_bverts = g_rf[g_pf].v;
        g_bidx = g_rf[g_pf].ix;
    }
}

/* game thread: vertices [vfirst, vfirst+count) of the frame arena hold model
 * space positions/normals for the current matrices (deferred transform) */
static void seg_add(int vfirst, int count) {
    XfSeg *sg;
    int r, c;
    if (count <= 0) return;
    if (g_nseg > 0 && !g_seg_nomerge) {
        sg = &g_rf[g_pf].seg[g_nseg - 1];
        /* never across a batch: the render thread transforms a whole segment
         * at the first draw that uses it */
        if (sg->epoch == (unsigned)g_batch_epoch && sg->vfirst + sg->count == vfirst && sg->ver == g_mtx_ver &&
            sg->skm < 0 && sg->src == NULL) {
            sg->count += count;
            return;
        }
    }
    if (g_nseg >= RQ_SEGS) port_fatal("gx: transform segment overflow");
    sg = &g_rf[g_pf].seg[g_nseg++];
    sg->vfirst = vfirst;
    sg->count = count;
    sg->epoch = (unsigned)g_batch_epoch;
    sg->ver = g_mtx_ver;
    sg->skm = -1;
    sg->one = 1.0f;
    sg->src = NULL;
    sg->rel = NULL;
    sg->uniq = NULL;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 4; c++) sg->m[r][c] = gx.posmtx[gx.cur_posmtx][r][c];
        for (c = 0; c < 3; c++) sg->q[r][c] = gx.nrmmtx[gx.cur_posmtx][r][c];
    }
}

/* game thread: a skinned segment (never merged: its own matrix set) */
static void seg_add_skin(int vfirst, int count, int skm, float one) {
    XfSeg *sg;
    if (count <= 0) return;
    g_seg_nomerge = 1;
    seg_add(vfirst, count);
    g_seg_nomerge = 0;
    sg = &g_rf[g_pf].seg[g_nseg - 1];
    sg->skm = skm;
    sg->one = one;
}

static void rq_commit(void) {
    g_ncmd++;
    if (g_ncmd - g_npub_local >= 16) rq_publish();
}

static void rt_upload(int slot, int w, int h, u8 *rgba) {
    RCmd *c = rq_cmd();
    c->type = CMD_UPLOAD;
    c->u.up.slot = slot;
    c->u.up.w = w;
    c->u.up.h = h;
    c->u.up.rgba = rgba;
    rq_commit();
}

/* record a draw of the vertices/indices at the given arena offsets */
static void record_draw(ShaderEntry *sh, const BatchState *st, GLenum mode, int voff, int nv, int ioff, int ni) {
    RCmd *c;
    int i;
    int slots[MAX_TEV];
    /* textures are validated (and uploads queued) before the draw command */
    for (i = 0; i < MAX_TEV; i++) slots[i] = (i < st->num_tev && st->texvalid[i]) ? get_texture(&st->tex[i]) : -1;
    if (!g_last_st_valid || memcmp(&g_rf[g_pf].st[g_nst - 1], st, sizeof(*st))) {
        g_rf[g_pf].st[g_nst++] = *st;
        g_last_st_valid = 1;
    }
    c = rq_cmd();
    c->type = CMD_DRAW;
    c->u.draw.sh = sh;
    c->u.draw.mode = mode;
    c->u.draw.voff = voff;
    c->u.draw.nv = nv;
    c->u.draw.ioff = ioff;
    c->u.draw.ni = ni;
    c->u.draw.st = g_nst - 1;
    memcpy(c->u.draw.texslot, slots, sizeof(slots));
    rq_commit();
}

/* game thread: a static draw. The caller ended the pending batch (draw
 * order); extends the previous static draw when it continues it (same
 * state, textures, matrices and page, contiguous indices). */
/* ---- alpha test without discard. A shader with `discard` (the GX alpha
 * test) is not opaque to the Vita GPU: no hidden surface removal, every
 * covered pixel is shaded. The level geometry is drawn with an alpha test
 * "a > 0" that never rejects anything when its texture has no texel of alpha
 * 0 and its vertices none either. For a static draw both are known (texture
 * alpha minimum at decode, vertex alpha minimum at the copy to GPU memory),
 * so a lower bound of the TEV alpha (interval arithmetic over the stages,
 * as the generator computes them) tells whether the test can ever reject;
 * when it cannot, the variant without the test is drawn: the same pixels.
 * CWOC_NODISC=0: always the shader with the test (A/B). */
static int g_nodisc_on = -1;
typedef struct { float lo, hi; } AIv;
static AIv aiv_mix(AIv a, AIv b, AIv c) {
    AIv r = {1e9f, -1e9f};
    int i;
    for (i = 0; i < 8; i++) { /* multilinear: extremes at the corners */
        float x = (i & 1) ? a.hi : a.lo, y = (i & 2) ? b.hi : b.lo, t = (i & 4) ? c.hi : c.lo;
        float v = x * (1.0f - t) + y * t;
        if (v < r.lo) r.lo = v;
        if (v > r.hi) r.hi = v;
    }
    return r;
}
/* lowest alpha of the TEV output (0..1) for texture alpha minima tmin[] (0..255,
 * -1: unknown) and the rasterized alpha range; gx holds this shader's state */
static float tev_alpha_lo(const int *tmin, AIv ras_ch) {
    AIv reg[4] = {{1, 1}, {0, 0}, {0, 0}, {0, 0}}; /* prev, c0, c1, c2 as generated */
    int i, k;
    if (gx.num_tev == 0) return ras_ch.lo;
    for (i = 0; i < gx.num_tev && i < MAX_TEV; i++) {
        const TevStage *t = &gx.tev[i];
        int has_tex = t->texmap != 0xFF && gx.texmap_valid[t->texmap & 7];
        AIv in[4], m, r;
        float sc = t->alpha_scale == 1 ? 2.0f : t->alpha_scale == 2 ? 4.0f : t->alpha_scale == 3 ? 0.5f : 1.0f;
        float bias = t->alpha_bias == 1 ? 0.5f : t->alpha_bias == 2 ? -0.5f : 0.0f;
        for (k = 0; k < 4; k++) {
            switch (t->alpha_in[k]) {
            case 0: in[k] = reg[0]; break;
            case 1: in[k] = reg[1]; break;
            case 2: in[k] = reg[2]; break;
            case 3: in[k] = reg[3]; break;
            case 4:
                if (!has_tex) in[k] = (AIv){1, 1};
                else if (tmin[i] < 0) return 0.0f;
                else in[k] = (AIv){tmin[i] / 255.0f, 1};
                break;
            case 5: in[k] = t->channel != 0xFF ? ras_ch : (AIv){0, 0}; break;
            case 6: in[k] = (AIv){1, 1}; break;
            default: in[k] = (AIv){0, 0}; break;
            }
        }
        m = aiv_mix(in[0], in[1], in[2]);
        if (t->alpha_op) r = (AIv){in[3].lo - m.hi, in[3].hi - m.lo};
        else r = (AIv){in[3].lo + m.lo, in[3].hi + m.hi};
        r.lo = (r.lo + bias) * sc;
        r.hi = (r.hi + bias) * sc;
        if (t->alpha_clamp) {
            r.lo = r.lo < 0 ? 0 : r.lo > 1 ? 1 : r.lo;
            r.hi = r.hi < 0 ? 0 : r.hi > 1 ? 1 : r.hi;
        }
        reg[t->alpha_reg & 3] = r;
    }
    return reg[0].lo;
}
static ShaderEntry *nodisc_variant(ShaderEntry *s, const int *slots, int vmin) {
    int i, tmin[MAX_TEV];
    AIv ras;
    if (g_nodisc_on < 0) g_nodisc_on = !(getenv("CWOC_NODISC") && getenv("CWOC_NODISC")[0] == '0');
    if (!g_nodisc_on || !s || vmin < 0) return s;
    if (s->atest_drop < 0) {
        /* once per shader: does it test (its state is gx's: same key) */
        u32 key[8];
        build_key(key);
        s->atest_drop = 0;
        s->atest_ref = -1;
        if (!memcmp(key, s->key, sizeof(key)) && strstr(s->fsrc, "discard")) {
            g_nodisc_gen = 1;
            s->nodisc = get_shader_inner();
            g_nodisc_gen = 0;
            s->atest_drop = s->nodisc != s && s->nodisc != &g_shaders[0];
        }
    }
    if (s->atest_drop != 1) return s;
    {
        /* from which alpha does every value pass, for this draw's references
         * (the compares and the op are the shader's: part of its key) */
        int ref = (g_bstate.aref0 & 0xFF) | (g_bstate.aref1 & 0xFF) << 8, a, lo = 256;
        if (ref != s->atest_ref) {
            for (a = 255; a >= 0; a--) {
                int t0 = atest_pass(gx.alpha_comp0, a, g_bstate.aref0), t1 = atest_pass(gx.alpha_comp1, a, g_bstate.aref1);
                int ok = gx.alpha_op == 0 ? (t0 && t1) : gx.alpha_op == 1 ? (t0 || t1) : gx.alpha_op == 2 ? (t0 != t1) : (t0 == t1);
                if (!ok) break;
                lo = a;
            }
            s->atest_ref = ref;
            s->atest_lo = lo;
        }
        if (s->atest_lo > 255) return s;
    }
    for (i = 0; i < MAX_TEV; i++) tmin[i] = slots[i] >= 0 ? g_texcache[slots[i]].amin : -1;
    if (gx.chan[1].mat_src) ras = (AIv){vmin / 255.0f, 1};
    else ras = (AIv){g_bstate.mat.a / 255.0f, g_bstate.mat.a / 255.0f};
    /* floor(a * 255 + 0.5) >= lo for every pixel, with half a step of margin */
    return tev_alpha_lo(tmin, ras) * 255.0f >= (float)s->atest_lo ? s->nodisc : s;
}

static int g_last_sm = -1;            /* static matrix set of the last static draw */
/* state entry and texture slots of the last static draw, valid while
 * g_bstate (version) and the frame's state list are unchanged */
static u32 g_sd_ver;
static int g_sd_st = -1, g_sd_slots[MAX_TEV];
static void record_draw_static(u32 first, u32 n, int page, int vmin) {
    RFrame *fr;
    ShaderEntry *sh;
    const float (*m)[4] = gx.posmtx[gx.cur_posmtx];
    const float (*q)[3] = gx.nrmmtx[gx.cur_posmtx];
    float mq[21];
    int i, *slots = g_sd_slots;
    RCmd *c;
    rq_ensure(1 + MAX_TEV, 0, 0); /* texture uploads + the draw */
    fr = &g_rf[g_pf];
    memcpy(mq, m, sizeof(float) * 12);
    memcpy(mq + 12, q, sizeof(float) * 9);
    if (!(g_last_st_valid && g_sd_ver == g_bstate_ver && g_sd_st == g_nst - 1)) {
        for (i = 0; i < MAX_TEV; i++)
            slots[i] = (i < g_bstate.num_tev && g_bstate.texvalid[i]) ? get_texture(&g_bstate.tex[i]) : -1;
        if (!g_last_st_valid || memcmp(&fr->st[g_nst - 1], &g_bstate, sizeof(g_bstate))) {
            fr->st[g_nst++] = g_bstate;
            g_last_st_valid = 1;
        }
        g_sd_ver = g_bstate_ver;
        g_sd_st = g_nst - 1;
    }
    if (g_last_sm < 0 || g_last_sm >= g_nsm || memcmp(fr->smtx + 21 * g_last_sm, mq, sizeof(mq))) {
        memcpy(fr->smtx + 21 * g_nsm, mq, sizeof(mq));
        g_last_sm = g_nsm++;
    }
    g_sg_lists++;
    sh = nodisc_variant(g_bshader, slots, vmin);
    if (g_ncmd > g_npub_local) { /* the previous command is still unpublished: merge? */
        RCmd *p = &fr->cmd[g_ncmd - 1];
        if (p->type == CMD_DRAW_STATIC && p->u.sdraw.sh == sh && p->u.sdraw.st == g_nst - 1 &&
            p->u.sdraw.mtx == g_last_sm && p->u.sdraw.page == page && p->u.sdraw.first + p->u.sdraw.n == first &&
            !memcmp(p->u.sdraw.texslot, slots, sizeof(g_sd_slots))) {
            p->u.sdraw.n += n;
            return;
        }
    }
    c = rq_cmd();
    c->type = CMD_DRAW_STATIC;
    c->u.sdraw.sh = sh;
    c->u.sdraw.first = first;
    c->u.sdraw.n = n;
    c->u.sdraw.st = g_nst - 1;
    c->u.sdraw.mtx = g_last_sm;
    c->u.sdraw.page = page;
    memcpy(c->u.sdraw.texslot, slots, sizeof(g_sd_slots));
    g_sg_draws++;
    rq_commit();
}

/* record the pending batch; keeps its state/shader so it can continue */
static void batch_emit(void) {
    if (g_bni > 0 && g_bshader) {
        if (TRACING()) {
            const GLVertex *v0 = g_bverts;
            g_trace--;
            port_log("gx: batch nv=%d ni=%d v0=(%.2f %.2f %.2f) clr=%02x%02x%02x%02x ntev=%d tex0=%dx%d amb=%02x%02x%02x%02x mat=%02x%02x%02x%02x blend=%d %d %d z=%d/%d/%d cull=%d key=%08x %08x %08x %08x",
                     g_bn, g_bni, v0->pos[0], v0->pos[1], v0->pos[2], v0->clr[0], v0->clr[1], v0->clr[2], v0->clr[3],
                     g_bstate.num_tev, g_bstate.texvalid[0] ? g_bstate.tex[0].width : 0, g_bstate.texvalid[0] ? g_bstate.tex[0].height : 0,
                     g_bstate.amb.r, g_bstate.amb.g, g_bstate.amb.b, g_bstate.amb.a, g_bstate.mat.r, g_bstate.mat.g, g_bstate.mat.b, g_bstate.mat.a,
                     g_bstate.blend_type, g_bstate.blend_src, g_bstate.blend_dst, g_bstate.z_enable, g_bstate.z_func, g_bstate.z_update, g_bstate.cull,
                     g_bstate.key[0], g_bstate.key[1], g_bstate.key[2], g_bstate.key[3]);
            { int li; for (li = 0; li < 3; li++) port_log("gx:    light%d pos=(%.1f %.1f %.1f) col=%02x%02x%02x%02x", li,
                g_bstate.lights[li].pos[0], g_bstate.lights[li].pos[1], g_bstate.lights[li].pos[2],
                g_bstate.lights[li].color.r, g_bstate.lights[li].color.g, g_bstate.lights[li].color.b, g_bstate.lights[li].color.a); }
            port_log("gx:    nrm0=(%.2f %.2f %.2f)", v0->nrm[0], v0->nrm[1], v0->nrm[2]);
            {
                float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                int vi, k, nz = 0;
                for (vi = 0; vi < g_bn; vi++) {
                    for (k = 0; k < 3; k++) {
                        if (g_bverts[vi].pos[k] < lo[k]) lo[k] = g_bverts[vi].pos[k];
                        if (g_bverts[vi].pos[k] > hi[k]) hi[k] = g_bverts[vi].pos[k];
                    }
                    if (g_bverts[vi].pos[0] != 0.0f || g_bverts[vi].pos[1] != 0.0f || g_bverts[vi].pos[2] != 0.0f) nz++;
                }
                port_log("gx:    bbox (%.2f %.2f %.2f)-(%.2f %.2f %.2f) nonzero=%d uv0=(%.3f %.3f)", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], nz,
                         v0->tex[0], v0->tex[1]);
            }
        }
        record_draw(g_bshader, &g_bstate, GL_TRIANGLES, g_voff, g_bn, g_ioff, g_bni);
        g_voff += g_bn;
        g_ioff += g_bni;
    }
    g_bn = g_bni = 0;
#ifdef PORT_GPUPROF
    g_gpuprof_fx = 0;
#endif
    g_bverts = g_rf[g_pf].v + g_voff;
    g_bidx = g_rf[g_pf].ix + g_ioff;
    g_batch_epoch++;
}

void port_gx_flush(void) {
    batch_emit();
    g_bshader = NULL;
}

/* room for nv more vertices and ni indices in the pending batch */
static void batch_reserve(int nv, int ni) {
    if (g_bn + nv > 65535) batch_emit();
    rq_ensure(1 + MAX_TEV, nv, ni);
}

/* static geometry stats for the perf report (per frame over nf frames) */
int port_gx_sg_report(char *buf, int size, int nf) {
    int n;
    if (!g_sg_npages) return 0;
    n = snprintf(buf, (size_t)size, " static %u lists %u draws (made %u dyn %u full %u cc %u) arena p%d+%dv %uKi",
                 g_sg_lists / (unsigned)nf, g_sg_draws / (unsigned)nf, g_sg_made, g_sg_dyn, g_sg_full,
                 g_sg_cc / (unsigned)nf, g_sg_page, g_sg_pv, g_sg_in >> 10);
    g_sg_lists = g_sg_draws = g_sg_cc = 0;
    return n < size ? n : size - 1;
}

int port_gx_take_draw_calls(void) {
    int n = g_draw_calls;
    g_draw_calls = 0;
    return n;
}

/* ---- render thread: command execution */
static void exec_copy(const RCmd *c);
void port_video_present(int frame); /* port_video.c: screenshot + swap */

/* render thread: skin a segment's model-space source positions in place
 * (same float operations as port_gs_skin_draw's immediate path) */
/* render thread output: the vertices the GPU reads. With the GPU ring (Vita)
 * the render thread writes converted vertices straight into GPU-mapped
 * memory (no vitaGL copy, no index scan per draw); otherwise the arena. */
#define RING_N 4
static GLVertex *g_rt_out;            /* current output base (ring slot or the frame arena) */
static GLVertex *g_ring[RING_N];
static int g_ring_on = -1, g_ring_slot;
static int g_rt_cov;                  /* vertices [0, cov) of this output are complete */

/* copy arena vertices that no segment converts (already final) */
static void rt_gap(const RFrame *fr, int upto) {
    if (upto > g_rt_cov) {
        if (g_rt_out != fr->v) memcpy(g_rt_out + g_rt_cov, fr->v + g_rt_cov, sizeof(GLVertex) * (size_t)(upto - g_rt_cov));
        g_rt_cov = upto;
    }
}

/* the render thread starts a new output (frame start, or arena restart) */
static void rt_ring_init(void) {
    const char *e = getenv("CWOC_GPURING");
    int i;
#ifdef PORT_VITA
    g_ring_on = !e || e[0] != '0';
#else
    g_ring_on = e && e[0] == '1'; /* PC: plain memory, for testing the coverage logic */
#endif
    for (i = 0; g_ring_on > 0 && i < RING_N; i++) {
#ifdef PORT_VITA
        g_ring[i] = vglMalloc(sizeof(GLVertex) * RQ_VERTS);
#else
        g_ring[i] = malloc(sizeof(GLVertex) * RQ_VERTS);
#endif
        if (g_ring[i] == NULL) {
            port_log("gx: GPU vertex ring allocation failed, using client arrays");
            g_ring_on = 0;
        }
    }
#ifdef PORT_VITA
    if (g_ring_on > 0) glGenBuffers(1, &g_ring_vbo);
    {
        const char *sg = getenv("CWOC_STATICGEO");
        size_t vfree = vglMemFree(VGL_MEM_ALL);
        port_log("gx: GPU memory free %u KB (VRAM %u, RAM %u, PHYCONT %u) after the ring",
                 (unsigned)(vfree >> 10), (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10),
                 (unsigned)(vglMemFree(VGL_MEM_RAM) >> 10), (unsigned)(vglMemFree(VGL_MEM_PHYCONT) >> 10));
        if (g_ring_on > 0 && !(sg && sg[0] == '0')) {
            int pages;
            /* up to 4 pages (9 MB) and 384K indices a page, leaving 4 MB to vitaGL */
            for (pages = 4; pages > 0; pages--) {
                size_t vb = (size_t)pages * SG_PAGE_VERTS * sizeof(GLVertex), ib = (size_t)pages * 384 * 1024 * 2;
                if (vb + ib + (4u << 20) > vfree) continue;
                g_sg_v = vglMalloc((uint32_t)vb);
                g_sg_i = g_sg_v ? vglMalloc((uint32_t)ib) : NULL;
                if (g_sg_v && g_sg_i) {
                    g_sg_npages = pages;
                    g_sg_icap = (u32)(ib / 2);
                    break;
                }
                if (g_sg_v) vglFree(g_sg_v);
                g_sg_v = NULL;
            }
            if (g_sg_npages) {
                glGenBuffers(1, &g_sg_vbo);
                glGenBuffers(1, &g_sg_ibo);
                __atomic_store_n(&g_sg_ready, 1, __ATOMIC_RELEASE);
            }
            port_log("gx: static geometry %s (%d pages of %d vertices, %u K indices)", g_sg_npages ? "on" : "off",
                     g_sg_npages, SG_PAGE_VERTS, g_sg_icap >> 10);
        }
    }
#endif
    port_log("gx: GPU vertex ring %s", g_ring_on > 0 ? "on" : "off");
}

static void rt_out_begin(RFrame *fr) {
    if (g_ring_on < 0) rt_ring_init();
    if (g_ring_on > 0) {
        g_ring_slot = (g_ring_slot + 1) % RING_N;
        g_rt_out = g_ring[g_ring_slot];
    } else {
        g_rt_out = fr->v;
    }
    g_rt_fr = fr;
    g_rt_cov = 0;
}

/* skinning prefetch distance in vertices (CWOC_SKINPF=0: none, A/B). The
 * source vertices of a cached skin list are gathered through its slot table
 * from the game's mesh: without a prefetch each one was a miss to memory
 * (~1 us a vertex in hub, 5 ms a frame on the render thread). */
#define SKIN_PF 8
static int g_skinpf = -1;
static void rt_skin(const RFrame *fr, const XfSeg *sg) {
    const float *mt = fr->skm + 256 * sg->skm;
    const float one = sg->one;
    GLVertex *d = fr->v + sg->vfirst, *e = d + sg->count;
    const SkinW *w = fr->skw + sg->vfirst;
    if (g_skinpf < 0) g_skinpf = !(getenv("CWOC_SKINPF") && getenv("CWOC_SKINPF")[0] == '0');
    for (; d < e; d++, w++) {
        if (g_skinpf && d + SKIN_PF < e) {
            __builtin_prefetch(d + SKIN_PF, 1, 0);
            __builtin_prefetch(w + SKIN_PF, 0, 0);
        }
        const float v0 = d->pos[0], v1 = d->pos[1], v2 = d->pos[2];
        const float *m = mt + 16 * w->i0;
        float a0 = m[0] * v0 + m[1] * v1 + m[2] * v2 + m[3];
        float a1 = m[4] * v0 + m[5] * v1 + m[6] * v2 + m[7];
        float a2 = m[8] * v0 + m[9] * v1 + m[10] * v2 + m[11];
        if (w->w0 == 1.0f) {
            d->pos[0] = a0; d->pos[1] = a1; d->pos[2] = a2;
        } else {
            const float w0 = w->w0, w1 = w->w1, w2 = one - (w0 + w1);
            const float *mb = mt + 16 * w->i1, *mc = mt + 16 * w->i2;
            const float b0 = mb[0] * v0 + mb[1] * v1 + mb[2] * v2 + mb[3];
            const float b1 = mb[4] * v0 + mb[5] * v1 + mb[6] * v2 + mb[7];
            const float b2 = mb[8] * v0 + mb[9] * v1 + mb[10] * v2 + mb[11];
            const float c0 = mc[0] * v0 + mc[1] * v1 + mc[2] * v2 + mc[3];
            const float c1 = mc[4] * v0 + mc[5] * v1 + mc[6] * v2 + mc[7];
            const float c2 = mc[8] * v0 + mc[9] * v1 + mc[10] * v2 + mc[11];
            d->pos[0] = a0 * w0 + b0 * w1 + c0 * w2;
            d->pos[1] = a1 * w0 + b1 * w1 + c1 * w2;
            d->pos[2] = a2 * w0 + b2 * w1 + c2 * w2;
        }
    }
}

/* render thread: skin straight from the source vertices (no blend shapes):
 * the game thread only recorded which source vertex each slot holds */
typedef struct { float x, y, z, w[2], ix[3], nx, ny, nz; u32 diffuse; float u, v; } PortGSVertexSkinR;
static void rt_skin_src(const RFrame *fr, const XfSeg *sg) {
    const PortGSVertexSkinR *sv = sg->src;
    const float *mt = fr->skm + 256 * sg->skm;
    const float one = sg->one;
    const int lit = sg->constcol;
    const u32 constcol = sg->col;
    GLVertex *d = fr->v + sg->vfirst, *e = d + sg->count;
    const SkinW *w = fr->skw + sg->vfirst;
    const u16 *uq = sg->uniq;
    if (g_skinpf < 0) g_skinpf = !(getenv("CWOC_SKINPF") && getenv("CWOC_SKINPF")[0] == '0');
    for (; d < e; d++, w++) {
        if (g_skinpf && d + SKIN_PF < e) {
            /* the source vertex SKIN_PF slots ahead (52 bytes: two lines) */
            const char *ps = (const char *)&sv[(short)(uq ? uq[SKIN_PF] : w[SKIN_PF].sk)];
            __builtin_prefetch(ps, 0, 0);
            __builtin_prefetch(ps + 48, 0, 0);
            __builtin_prefetch(d + SKIN_PF, 1, 0);
            if (!uq) __builtin_prefetch(w + 2 * SKIN_PF, 0, 0);
        }
        const PortGSVertexSkinR *s = &sv[(short)(uq ? *uq++ : w->sk)];
        const float v0 = s->x, v1 = s->y, v2 = s->z;
        const float *m = mt + 16 * ((int)s->ix[0] & 15);
        float a0 = m[0] * v0 + m[1] * v1 + m[2] * v2 + m[3];
        float a1 = m[4] * v0 + m[5] * v1 + m[6] * v2 + m[7];
        float a2 = m[8] * v0 + m[9] * v1 + m[10] * v2 + m[11];
        u32 c = lit ? s->diffuse : constcol;
        if (s->w[0] == 1.0f) {
            d->pos[0] = a0; d->pos[1] = a1; d->pos[2] = a2;
        } else {
            const float w0 = s->w[0], w1 = s->w[1], w2 = one - (w0 + w1);
            const float *mb = mt + 16 * ((int)s->ix[1] & 15), *mc = mt + 16 * ((int)s->ix[2] & 15);
            const float b0 = mb[0] * v0 + mb[1] * v1 + mb[2] * v2 + mb[3];
            const float b1 = mb[4] * v0 + mb[5] * v1 + mb[6] * v2 + mb[7];
            const float b2 = mb[8] * v0 + mb[9] * v1 + mb[10] * v2 + mb[11];
            const float c0 = mc[0] * v0 + mc[1] * v1 + mc[2] * v2 + mc[3];
            const float c1 = mc[4] * v0 + mc[5] * v1 + mc[6] * v2 + mc[7];
            const float c2 = mc[8] * v0 + mc[9] * v1 + mc[10] * v2 + mc[11];
            d->pos[0] = a0 * w0 + b0 * w1 + c0 * w2;
            d->pos[1] = a1 * w0 + b1 * w1 + c1 * w2;
            d->pos[2] = a2 * w0 + b2 * w1 + c2 * w2;
        }
        if (lit) {
            d->nrm[0] = s->nx; d->nrm[1] = s->ny; d->nrm[2] = s->nz;
            d->tex[0] = s->u;
            d->tex[1] = s->v;
        } else {
            d->nrm[0] = d->nrm[1] = d->nrm[2] = 0.0f;
            d->tex[0] = d->tex[1] = 0.0f;
        }
        d->clr[0] = (u8)(c >> 24);
        d->clr[1] = (u8)(c >> 16);
        d->clr[2] = (u8)(c >> 8);
        d->clr[3] = (u8)c;
    }
}

#if defined(PORT_VITA) && defined(__ARM_NEON)
/* The ring vertices live in GPU-mapped memory the CPU writes uncached: write
 * each 36-byte vertex as three sequential, non-overlapping stores (the old
 * overlapping 16-byte stores plus byte/float stores made the render thread's
 * transform cost ~0.4 us a vertex, 22-36 ms a frame in busy scenes). */
static inline void rt_store_vtx(GLVertex *d, float32x4_t p, float32x4_t r, u32 clr32, float t0, float t1) {
    float32x4_t q0 = vsetq_lane_f32(vgetq_lane_f32(r, 0), p, 3); /* x y z nx */
    float32x4_t q1 = vextq_f32(r, r, 1);                         /* ny nz - - */
    uint32x4_t u1 = vsetq_lane_u32(clr32, vreinterpretq_u32_f32(q1), 2);
    q1 = vsetq_lane_f32(t0, vreinterpretq_f32_u32(u1), 3);       /* ny nz clr u */
    vst1q_f32(&d->pos[0], q0);
    vst1q_f32(&d->nrm[1], q1);
    d->tex[1] = t1;
}
#endif

/* render thread: convert + view-transform a vertex range straight from the
 * game's source vertices (draw_indexed_range, deferred copy) */
static void rt_copy_xf(const RFrame *fr, const XfSeg *sg) {
    const PortGSVertex *sv = sg->src;
    GLVertex *d = g_rt_out + sg->vfirst, *e = d + sg->count;
    (void)fr;
    const int constcol = sg->constcol;
    const u32 col = sg->col;
#if defined(PORT_VITA) && defined(__ARM_NEON)
    const float32x4_t c0 = { sg->m[0][0], sg->m[1][0], sg->m[2][0], 0.0f };
    const float32x4_t c1 = { sg->m[0][1], sg->m[1][1], sg->m[2][1], 0.0f };
    const float32x4_t c2 = { sg->m[0][2], sg->m[1][2], sg->m[2][2], 0.0f };
    const float32x4_t c3 = { sg->m[0][3], sg->m[1][3], sg->m[2][3], 0.0f };
    const float32x4_t n0 = { sg->q[0][0], sg->q[1][0], sg->q[2][0], 0.0f };
    const float32x4_t n1 = { sg->q[0][1], sg->q[1][1], sg->q[2][1], 0.0f };
    const float32x4_t n2 = { sg->q[0][2], sg->q[1][2], sg->q[2][2], 0.0f };
    for (; d < e; d++, sv++) {
        /* memory-latency bound (~0.4 us a vertex on the Vita): fetch ahead */
        __builtin_prefetch(sv + 8, 0, 0);
        __builtin_prefetch(d + 8, 1, 0);
        float32x4_t p = vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(c3, c0, sv->x), c1, sv->y), c2, sv->z);
        float32x4_t r = vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(n0, sv->nx), n1, sv->ny), n2, sv->nz);
        u32 c = constcol ? col : sv->diffuse;
        rt_store_vtx(d, p, r, __builtin_bswap32(c), sv->u, sv->v); /* clr bytes R,G,B,A */
    }
#else
    const float (*m)[4] = sg->m;
    const float (*q)[3] = sg->q;
    for (; d < e; d++, sv++) {
        float x = sv->x, y = sv->y, z = sv->z, nx = sv->nx, ny = sv->ny, nz = sv->nz;
        u32 c = constcol ? col : sv->diffuse;
        d->pos[0] = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
        d->pos[1] = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
        d->pos[2] = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
        d->nrm[0] = q[0][0] * nx + q[0][1] * ny + q[0][2] * nz;
        d->nrm[1] = q[1][0] * nx + q[1][1] * ny + q[1][2] * nz;
        d->nrm[2] = q[2][0] * nx + q[2][1] * ny + q[2][2] * nz;
        d->clr[0] = (u8)(c >> 24);
        d->clr[1] = (u8)(c >> 16);
        d->clr[2] = (u8)(c >> 8);
        d->clr[3] = (u8)c;
        d->tex[0] = sv->u;
        d->tex[1] = sv->v;
    }
#endif
}

/* render thread: indices of a cached list, relative to its lowest vertex,
 * shifted to the segment's place in the batch (draw_indexed_range hit) */
static void rt_expand_idx(RFrame *fr, const XfSeg *sg) {
    const u16 *r = sg->rel;
    u16 *ix = fr->ix + sg->ixa;
    const u32 n = sg->rel_n;
    const u16 b = sg->ibase;
    u32 j = 0;
#if defined(PORT_VITA) && defined(__ARM_NEON)
    {
        const uint16x8_t vb = vdupq_n_u16(b);
        for (; j + 16 <= n; j += 16) {
            __builtin_prefetch(r + j + 64, 0, 0);
            vst1q_u16(ix + j, vaddq_u16(vld1q_u16(r + j), vb));
            vst1q_u16(ix + j + 8, vaddq_u16(vld1q_u16(r + j + 8), vb));
        }
        for (; j + 8 <= n; j += 8) vst1q_u16(ix + j, vaddq_u16(vld1q_u16(r + j), vb));
    }
#endif
    for (; j < n; j++) ix[j] = (u16)(r[j] + b);
}

/* CWOC_XFSUB=1 (measurement only): time and vertices of the deferred
 * transform per segment kind, render thread [0] and worker [1]: 0 skinned
 * from a cached static skin list, 1 converted copy (src), 2 skinned, 3 view
 * transform only. Reported with the perf lines (port_video.c). */
int g_xfsub_on = -1;
uint64_t g_xfsub_us[2][4];
unsigned g_xfsub_v[2][4], g_xfsub_n[2][4];
static SDL_threadID g_wk_tid;
static void rt_xf_seg_impl(RFrame *fr, const XfSeg *sg);
static void rt_xf_seg(RFrame *fr, const XfSeg *sg) {
    uint64_t t0;
    int k, w;
    if (g_xfsub_on < 0) g_xfsub_on = getenv("CWOC_XFSUB") && getenv("CWOC_XFSUB")[0] == '1';
    if (!g_xfsub_on) {
        rt_xf_seg_impl(fr, sg);
        return;
    }
    k = sg->skm >= 0 && sg->src != NULL ? 0 : sg->src != NULL ? 1 : sg->skm >= 0 ? 2 : 3;
    w = g_wk_tid && SDL_ThreadID() == g_wk_tid;
    t0 = port_prof_now();
    rt_xf_seg_impl(fr, sg);
    g_xfsub_us[w][k] += port_prof_now() - t0;
    g_xfsub_v[w][k] += (unsigned)sg->count;
    g_xfsub_n[w][k]++;
}
/* view-transform (or skin / convert) one recorded segment into g_rt_out,
 * and write its deferred indices: render thread or transform worker */
static void rt_xf_seg_impl(RFrame *fr, const XfSeg *sg) {
    const GLVertex *sp = fr->v + sg->vfirst;
    GLVertex *d = g_rt_out + sg->vfirst, *e = d + sg->count;
    if (sg->rel != NULL) rt_expand_idx(fr, sg);
    if (sg->skm >= 0 && sg->src != NULL) {
        rt_skin_src(fr, sg);
    } else if (sg->src != NULL) {
        rt_copy_xf(fr, sg);
        return;
    } else if (sg->skm >= 0) {
        rt_skin(fr, sg);
    }
#if defined(PORT_VITA) && defined(__ARM_NEON)
    const float32x4_t c0 = { sg->m[0][0], sg->m[1][0], sg->m[2][0], 0.0f };
    const float32x4_t c1 = { sg->m[0][1], sg->m[1][1], sg->m[2][1], 0.0f };
    const float32x4_t c2 = { sg->m[0][2], sg->m[1][2], sg->m[2][2], 0.0f };
    const float32x4_t c3 = { sg->m[0][3], sg->m[1][3], sg->m[2][3], 0.0f };
    const float32x4_t n0 = { sg->q[0][0], sg->q[1][0], sg->q[2][0], 0.0f };
    const float32x4_t n1 = { sg->q[0][1], sg->q[1][1], sg->q[2][1], 0.0f };
    const float32x4_t n2 = { sg->q[0][2], sg->q[1][2], sg->q[2][2], 0.0f };
    for (; d < e; d++, sp++) {
        __builtin_prefetch(sp + 8, 0, 0);
        __builtin_prefetch(d + 8, 1, 0);
        float32x4_t p = vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(c3, c0, sp->pos[0]), c1, sp->pos[1]), c2, sp->pos[2]);
        float32x4_t r = vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(n0, sp->nrm[0]), n1, sp->nrm[1]), n2, sp->nrm[2]);
        u32 clr;
        float t0 = sp->tex[0], t1 = sp->tex[1];
        memcpy(&clr, sp->clr, 4);
        rt_store_vtx(d, p, r, clr, t0, t1);
    }
#else
    const float (*m)[4] = sg->m;
    const float (*q)[3] = sg->q;
    for (; d < e; d++, sp++) {
        float x = sp->pos[0], y = sp->pos[1], z = sp->pos[2], nx = sp->nrm[0], ny = sp->nrm[1], nz = sp->nrm[2];
        if (d != sp) {
            memcpy(d->clr, sp->clr, 4);
            d->tex[0] = sp->tex[0];
            d->tex[1] = sp->tex[1];
        }
        d->pos[0] = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
        d->pos[1] = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
        d->pos[2] = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
        d->nrm[0] = q[0][0] * nx + q[0][1] * ny + q[0][2] * nz;
        d->nrm[1] = q[1][0] * nx + q[1][1] * ny + q[1][2] * nz;
        d->nrm[2] = q[2][0] * nx + q[2][1] * ny + q[2][2] * nz;
    }
#endif
}

/* segments [a, b) of fr, in order, starting with vertex coverage cov: each
 * one is preceded by a copy of the uncovered (final) vertices before it.
 * Segments are recorded in increasing vertex order without overlaps (seg_add
 * always starts at the arena end), so the ranges of [a, m) and [m, b) are
 * disjoint and can run on two cores. */
static void rt_xf_range(RFrame *fr, int a, int b, int cov) {
    int i;
    for (i = a; i < b; i++) {
        const XfSeg *sg = &fr->seg[i];
        if (cov < sg->vfirst && g_rt_out != fr->v)
            memcpy(g_rt_out + cov, fr->v + cov, sizeof(GLVertex) * (size_t)(sg->vfirst - cov));
        rt_xf_seg(fr, sg);
        if (sg->vfirst + sg->count > cov) cov = sg->vfirst + sg->count;
    }
}

/* Transform worker on core 2 (idle otherwise: audio uses ~5% of it). For a
 * draw with enough pending vertices the render thread hands the second half
 * of the segments to it, does the first half and waits: the worker only
 * touches what it was given while the render thread waits, so no other
 * synchronisation is needed. CWOC_XFWORKER=0 disables it. */
#define WK_MIN_VERTS 1024
static int g_wk_on;
static SDL_sem *g_wk_go;
static struct { RFrame *fr; int a, b, cov; } g_wk_job;
static volatile int g_wk_busy;
uint64_t g_wk_us;     /* worker busy time (reported with the perf lines) */
unsigned g_wk_jobs;

static int wk_thread(void *arg) {
    (void)arg;
    g_wk_tid = SDL_ThreadID();
    { extern void port_thread_register(const char *name); port_thread_register("gxxform"); }
#ifdef PORT_VITA
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
#endif
    /* CWOC_WKPRIO=1: run above the GX thread, which shares core 2 (equal
     * priorities do not take turns on the Vita) */
    if (getenv("CWOC_WKPRIO") && getenv("CWOC_WKPRIO")[0] == '1') SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    for (;;) {
        uint64_t t0;
        SDL_SemWait(g_wk_go);
        t0 = port_prof_now();
        rt_xf_range(g_wk_job.fr, g_wk_job.a, g_wk_job.b, g_wk_job.cov);
        g_wk_us += port_prof_now() - t0;
        g_wk_jobs++;
        __atomic_store_n(&g_wk_busy, 0, __ATOMIC_RELEASE);
    }
    return 0;
}

static void wk_init(void) {
    const char *e = getenv("CWOC_XFWORKER");
    if (e && e[0] == '0') return;
    g_wk_go = SDL_CreateSemaphore(0);
    if (g_wk_go && SDL_CreateThreadWithStackSize(wk_thread, "gxxform", 256 * 1024, NULL)) g_wk_on = 1;
    port_log("gx: transform worker %s", g_wk_on ? "on (core 2)" : "off");
}

/* render thread: view-transform the recorded segments that start below vend */
static void rt_transform(int f, int vend) {
    RFrame *fr = &g_rf[f];
    int n = fr->nseg, a = g_rt_seg[f], k = a;
    if (g_rt_out == NULL || g_rt_fr != fr) rt_out_begin(fr);
    while (k < n && fr->seg[k].vfirst < vend) k++;
    if (k > a) {
        const XfSeg *last = &fr->seg[k - 1];
        int lo = fr->seg[a].vfirst, hi = last->vfirst + last->count;
        if (g_wk_on && k - a >= 2 && hi - lo >= WK_MIN_VERTS) {
            int half = lo + (hi - lo) / 2, m = a + 1;
            while (m < k - 1 && fr->seg[m].vfirst < half) m++;
            g_wk_job.fr = fr;
            g_wk_job.a = m;
            g_wk_job.b = k;
            g_wk_job.cov = fr->seg[m - 1].vfirst + fr->seg[m - 1].count;
            __atomic_store_n(&g_wk_busy, 1, __ATOMIC_RELEASE);
            SDL_SemPost(g_wk_go);
            rt_xf_range(fr, a, m, g_rt_cov);
            g_ws_rt = "wk_spin";
            while (__atomic_load_n(&g_wk_busy, __ATOMIC_ACQUIRE)) {
            }
            g_ws_rt = "-";
        } else {
            rt_xf_range(fr, a, k, g_rt_cov);
        }
        if (hi > g_rt_cov) g_rt_cov = hi;
        g_rt_seg[f] = k;
    }
    rt_gap(fr, vend);
}

/* Frames per second in the top-left corner: pause menu > OPTIONS > SHOW FPS
 * (port_hooks.c), saved in <save dir>/fps.txt; CWOC_FPS=1 turns it on when
 * there is no such file. The rate is of frames actually presented, measured
 * here every 0.5 s (rt_fps_tick); the game thread draws it with the game's own
 * 3D font in the HUD scene (port_hud.c), so it looks like the rest of the HUD.
 * CWOC_FPS=2 (measurements): the plain counter instead, drawn here just
 * before every swap (also on loads and movies, and in 4:3 in the black band)
 * with scissored glClear rectangles (a 3x5 font scaled 3x): no shader,
 * texture or vertex data. */
static volatile int g_fps_overlay = -1;
static int g_fps_raw;               /* CWOC_FPS=2 */
static volatile int g_fps_x10 = -1; /* render thread -> game thread: the last rate x 10 */

static void fps_overlay_path(char *buf, size_t n) {
    extern const char *port_save_dir(void);
    snprintf(buf, n, "%s/fps.txt", port_save_dir());
}

int port_fps_overlay_on(void) {
    if (g_fps_overlay < 0) {
        char path[512];
        FILE *f;
        int v = getenv("CWOC_FPS") && (getenv("CWOC_FPS")[0] == '1' || getenv("CWOC_FPS")[0] == '2');
        fps_overlay_path(path, sizeof(path));
        f = fopen(path, "r");
        if (f) {
            int r = 0;
            if (fscanf(f, "%d", &r) == 1) v = r != 0;
            fclose(f);
        }
        g_fps_overlay = v;
        port_log("video: fps counter %s", v ? "on" : "off");
    }
    return g_fps_overlay;
}

/* pause menu > OPTIONS (port_hooks.c) */
void port_fps_overlay_toggle(void) {
    char path[512];
    FILE *f;
    g_fps_overlay = !port_fps_overlay_on();
    fps_overlay_path(path, sizeof(path));
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%d\n", g_fps_overlay);
        fclose(f);
    }
    port_log("video: fps counter %s", g_fps_overlay ? "on" : "off");
}

/* game thread (port_hud.c): frames per second x 10, -1 before the first
 * measure or with CWOC_FPS=2 (the plain counter draws it instead) */
int port_fps_x10(void) { return g_fps_raw ? -1 : g_fps_x10; }

/* render thread, at every present: the rate of presented frames over 0.5 s.
 * A present more than 1 s after the one before (a load) starts a new window,
 * so the first number after a load is not the load's. */
static void rt_fps_tick(void) {
    static uint64_t t_last, t_prev;
    static unsigned n;
    uint64_t now = port_prof_now();
    if (!t_last || now - t_prev > 1000000) {
        t_last = t_prev = now;
        n = 0;
        return;
    }
    t_prev = now;
    n++;
    if (now - t_last >= 500000) {
        g_fps_x10 = (int)(n * 1e7 / (double)(now - t_last) + 0.5);
        t_last = now;
        n = 0;
    }
}

static void rt_fps_overlay(void) {
    /* 3x5 glyphs, rows top to bottom, 3 bits each (bit 2 = left column) */
    static const unsigned short glyph[11] = {
        075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717, 000002, /* 0-9 . */
    };
    char txt[16];
    const int px = 3, adv = 4 * px, x0 = 6, top = 6;
    int i, len, r, c, v = g_fps_x10;
    if (v < 0) return;
    snprintf(txt, sizeof(txt), "%d.%d", v / 10, v % 10);
    len = (int)strlen(txt);
    g_gl.valid = 0;
    glViewport(0, 0, g_win_w, g_win_h);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glScissor(x0 - px, g_win_h - top - 7 * px, len * adv + px, 7 * px);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(1.0f, 1.0f, 0.4f, 1.0f);
    for (i = 0; i < len; i++) {
        int g = txt[i] == '.' ? 10 : txt[i] - '0';
        if (g < 0 || g > 10) continue;
        for (r = 0; r < 5; r++) {
            int bits = (glyph[g] >> (3 * (4 - r))) & 7;
            for (c = 0; c < 3; c++) {
                int run = 0;
                while (c + run < 3 && (bits & (4 >> (c + run)))) run++;
                if (run) {
                    glScissor(x0 + i * adv + c * px, g_win_h - top - px - (r + 1) * px, run * px, px);
                    glClear(GL_COLOR_BUFFER_BIT);
                    c += run;
                }
            }
        }
    }
}

/* ---- PS Vita button icons (port_hud.c) ----
 * The game's 3D font has flat glyphs for the controller buttons (lowercase
 * a, b, x, y: the GameCube A, B, X and Y). port_hud.c does not draw those
 * glyphs and asks here for the PS Vita button in their place, at the same
 * position and size: cross, square, circle and triangle, which is what the
 * port maps A, B, X and Y to (port_pad.c). The icons of a frame are drawn by
 * the render thread just before the swap, on top of everything: one textured
 * quad each from a 256x64 atlas made at the first use (anti-aliased discs,
 * alpha blended), with a small shader of their own. The game's fade (a
 * multiply over the frame, DrawFade) darkens them through the vertex colour.
 * Positions are in the EFB's normalised coordinates (x, y in -1..1, y up);
 * d is the diameter in the same units, of the EFB height. */
#define ICON_MAX 48
#define ICON_RING 8
typedef struct { float x, y, d; int icon; } IconReq;
static struct { volatile int frame; int n; float shade; IconReq ic[ICON_MAX]; } g_icon_ring[ICON_RING];
static IconReq g_icon_build[ICON_MAX]; /* game thread: icons of the frame being made */
static int g_icon_nbuild;
static float g_icon_shade = 1.0f;
static GLuint g_icon_prog, g_icon_tex;
static int g_icon_state; /* render thread: 0 not made yet, 1 ready, -1 failed */

/* game thread */
void port_gx_icon(float x, float y, float d, int icon) {
    IconReq *r;
    if (g_icon_nbuild >= ICON_MAX || icon < 0 || icon > 3 || !(d > 0.0f)) return;
    r = &g_icon_build[g_icon_nbuild++];
    r->x = x;
    r->y = y;
    r->d = d;
    r->icon = icon;
}

/* game thread: the brightness the frame's fade leaves (1 = no fade) */
void port_gx_icon_shade(float s) { g_icon_shade = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s; }

/* game thread, just before port_gx_present(frame): hand the frame's icons over */
void port_gx_icons_commit(int frame) {
    int k = frame & (ICON_RING - 1);
    g_icon_ring[k].n = g_icon_nbuild;
    g_icon_ring[k].shade = g_icon_shade;
    if (g_icon_nbuild) memcpy(g_icon_ring[k].ic, g_icon_build, sizeof(IconReq) * (size_t)g_icon_nbuild);
    __atomic_store_n(&g_icon_ring[k].frame, frame, __ATOMIC_RELEASE);
    g_icon_nbuild = 0;
    g_icon_shade = 1.0f;
}

/* the symbol of icon i at (dx, dy) pixels from the centre of its 64x64 cell (y down) */
static int icon_symbol(int i, float dx, float dy) {
    float u, v, m;
    switch (i) {
    case 0: /* cross */
        u = (dx + dy) * 0.70710678f;
        v = (dx - dy) * 0.70710678f;
        return (fabsf(u) <= 3.1f && fabsf(v) <= 13.5f) || (fabsf(v) <= 3.1f && fabsf(u) <= 13.5f);
    case 1: /* circle */
        return fabsf(sqrtf(dx * dx + dy * dy) - 12.0f) <= 2.9f;
    case 2: /* square */
        m = fmaxf(fabsf(dx), fabsf(dy));
        return m <= 11.5f && m >= 6.3f;
    case 3: /* triangle, point up, centred on the cell */
        dy -= 5.0f;
        m = fmaxf(dy, fmaxf(-0.8660254f * dx - 0.5f * dy, 0.8660254f * dx - 0.5f * dy));
        return m <= 10.0f && m >= 4.6f;
    }
    return 0;
}

/* 256x64 RGBA: cross, circle, square, triangle. A dark disc (radius 30 of the
 * 64 pixel cell, lighter at the top) with a light rim and the coloured
 * symbol, 4x4 samples per pixel */
static void icon_atlas(u8 *px) {
    static const u8 sym[4][3] = { { 0x8a, 0xbc, 0xf6 }, { 0xff, 0x6a, 0x6a }, { 0xf2, 0x92, 0xd8 }, { 0x46, 0xd8, 0xa8 } };
    int i, x, y, sx, sy;
    for (i = 0; i < 4; i++)
        for (y = 0; y < 64; y++)
            for (x = 0; x < 64; x++) {
                float acc[3] = { 0, 0, 0 };
                int n = 0;
                u8 *p = px + ((size_t)y * 256 + (size_t)i * 64 + x) * 4;
                for (sy = 0; sy < 4; sy++)
                    for (sx = 0; sx < 4; sx++) {
                        float dx = x + (sx + 0.5f) * 0.25f - 32.0f, dy = y + (sy + 0.5f) * 0.25f - 32.0f;
                        float r = sqrtf(dx * dx + dy * dy), t;
                        if (r > 30.0f) continue;
                        n++;
                        if (icon_symbol(i, dx, dy)) {
                            acc[0] += sym[i][0]; acc[1] += sym[i][1]; acc[2] += sym[i][2];
                        } else if (r > 27.4f) {
                            acc[0] += 0xb8; acc[1] += 0xb8; acc[2] += 0xc2;
                        } else {
                            t = (dy + 30.0f) / 60.0f;
                            acc[0] += 0x40 - t * 0x2a; acc[1] += 0x40 - t * 0x2a; acc[2] += 0x4a - t * 0x30;
                        }
                    }
                if (n) {
                    p[0] = (u8)(acc[0] / n + 0.5f);
                    p[1] = (u8)(acc[1] / n + 0.5f);
                    p[2] = (u8)(acc[2] / n + 0.5f);
                } else {
                    p[0] = p[1] = p[2] = 0;
                }
                p[3] = (u8)((n * 255 + 8) / 16);
            }
}

static int icon_init(void) {
    static const char *vs =
        PORT_GLSL_HEADER
        "attribute vec3 a_pos;\n"
        "attribute vec4 a_clr;\n"
        "attribute vec2 a_tex;\n"
        "varying vec4 v_clr;\n"
        "varying vec2 v_tex;\n"
        "void main() {\n"
        "  gl_Position = vec4(a_pos.xy, 0.0, 1.0);\n"
        "  v_clr = a_clr;\n"
        "  v_tex = a_tex;\n"
        "}\n";
    static const char *fs =
        PORT_GLSL_HEADER
        "uniform sampler2D u_tex0;\n"
        "varying vec4 v_clr;\n"
        "varying vec2 v_tex;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(u_tex0, v_tex) * v_clr;\n"
        "}\n";
    GLint ok = 0;
    u8 *px;
    uint64_t t0 = port_prof_now();
#ifdef PORT_VITA
    /* through the program cache like the game's (a link is ~550 ms) */
    static ShaderEntry ce;
    char cpath[256];
    ce.vsrc = (char *)vs;
    ce.fsrc = (char *)fs;
    if (shcache_on()) {
        shcache_path(cpath, sizeof(cpath), &ce);
        if (shcache_load(&ce, cpath)) {
            g_icon_prog = ce.prog;
            goto linked;
        }
    }
#endif
    g_icon_prog = glCreateProgram();
    glAttachShader(g_icon_prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(g_icon_prog, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(g_icon_prog, 0, "a_pos");
    glBindAttribLocation(g_icon_prog, 2, "a_clr");
    glBindAttribLocation(g_icon_prog, 3, "a_tex");
    glLinkProgram(g_icon_prog);
    glGetProgramiv(g_icon_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(g_icon_prog, sizeof(log), NULL, log);
        port_log("gx: button icon program link failed: %s", log);
        return 0;
    }
#ifdef PORT_VITA
    if (g_shcache) shcache_save(g_icon_prog, cpath);
linked:
#endif
    glUseProgram(g_icon_prog);
    g_gl.prog = g_icon_prog;
    glUniform1i(glGetUniformLocation(g_icon_prog, "u_tex0"), 0);
    px = malloc(256 * 64 * 4);
    if (!px) return 0;
    icon_atlas(px);
    glGenTextures(1, &g_icon_tex);
    bind_tex_raw(0, g_icon_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    port_log("gx: PS Vita button icons ready in %.0f ms", (port_prof_now() - t0) / 1000.0);
    return 1;
}

/* render thread, before the swap of 'frame' */
static void rt_icons(int frame) {
    static GLVertex v[ICON_MAX * 6];
    int k = frame & (ICON_RING - 1), n, i;
    u8 sh;
    if (!g_icon_state) { /* at the first present (the logos), not at the first icon */
        g_gl.valid = 0;
        g_icon_state = icon_init() ? 1 : -1;
    }
    if (__atomic_load_n(&g_icon_ring[k].frame, __ATOMIC_ACQUIRE) != frame) return;
    n = g_icon_ring[k].n;
    if (n <= 0 || g_icon_state < 0) return;
    g_gl.valid = 0;
    sh = (u8)(g_icon_ring[k].shade * 255.0f + 0.5f);
    memset(v, 0, sizeof(GLVertex) * (size_t)n * 6);
    for (i = 0; i < n; i++) {
        static const int corner[6] = { 0, 1, 2, 1, 3, 2 }; /* TL TR BL, TR BR BL */
        const IconReq *r = &g_icon_ring[k].ic[i];
        /* EFB coordinates -> window pixels (the drawable may be pillarboxed),
         * then window NDC; the disc is 60 of the cell's 64 pixels */
        float cx = g_off_x + (r->x * 0.5f + 0.5f) * g_draw_w;
        float cy = g_off_y + (0.5f - r->y * 0.5f) * g_draw_h;
        float h = r->d * 0.5f * g_draw_h * (64.0f / 60.0f) * 0.5f;
        float x0 = (cx - h) / g_win_w * 2.0f - 1.0f, x1 = (cx + h) / g_win_w * 2.0f - 1.0f;
        float y0 = 1.0f - (cy - h) / g_win_h * 2.0f, y1 = 1.0f - (cy + h) / g_win_h * 2.0f;
        float u0 = r->icon * 0.25f, u1 = u0 + 0.25f;
        int j;
        for (j = 0; j < 6; j++) {
            GLVertex *o = &v[i * 6 + j];
            int cn = corner[j];
            o->pos[0] = (cn & 1) ? x1 : x0;
            o->pos[1] = (cn & 2) ? y1 : y0;
            o->tex[0] = (cn & 1) ? u1 : u0;
            o->tex[1] = (cn & 2) ? 1.0f : 0.0f;
            o->clr[0] = o->clr[1] = o->clr[2] = sh;
            o->clr[3] = 255;
        }
    }
    glViewport(0, 0, g_win_w, g_win_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (g_gl.prog != g_icon_prog) {
        glUseProgram(g_icon_prog);
        g_gl.prog = g_icon_prog;
    }
    bind_tex_raw(0, g_icon_tex);
    /* client arrays, like the draws without the GPU ring */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    g_vb_mode = -1;
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &v[0].pos);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &v[0].nrm);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GLVertex), &v[0].clr);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &v[0].tex);
    glDrawArrays(GL_TRIANGLES, 0, n * 6);
    g_draw_calls++;
}

#ifdef PORT_GPUPROF
/* GPU profiler build, render thread: pass scope and draw name of a command,
 * and the draws.csv row of a game draw (port_gpuprof.c) */
static int gp_cmd(const RFrame *fr, const RCmd *c) {
    const BatchState *st;
    PortGpDraw *d;
    if (c->type == CMD_DRAW) port_gpuprof_shader((int)(c->u.draw.sh - g_shaders));
    else if (c->type == CMD_DRAW_STATIC) port_gpuprof_shader((int)(c->u.sdraw.sh - g_shaders));
    if ((c->type == CMD_DRAW || c->type == CMD_DRAW_STATIC) && port_gpuprof_skip(c->gp)) return 1;
    if (!g_gpuprof_live) return 0;
    port_gpuprof_scope(c->gp);
    switch (c->type) {
    case CMD_DRAW:
    case CMD_DRAW_STATIC:
        if (!(d = port_gpuprof_draw())) break;
        if (c->type == CMD_DRAW) {
            st = &fr->st[c->u.draw.st];
            d->sh = (uint16_t)(c->u.draw.sh - g_shaders);
            d->kind = GP_DRAW_DYN;
            d->mode = (uint8_t)c->u.draw.mode;
            d->nv = (uint32_t)c->u.draw.nv;
            d->ni = (uint32_t)c->u.draw.ni;
        } else {
            st = &fr->st[c->u.sdraw.st];
            d->sh = (uint16_t)(c->u.sdraw.sh - g_shaders);
            d->kind = GP_DRAW_STATIC;
            d->mode = GL_TRIANGLES;
            d->ni = c->u.sdraw.n;
        }
        d->ntev = (uint8_t)st->num_tev;
        if (st->num_tev > 0 && st->texvalid[0]) {
            d->tw = st->tex[0].width;
            d->th = st->tex[0].height;
        }
        d->blend = (uint8_t)st->blend_type;
        d->bsrc = (uint8_t)st->blend_src;
        d->bdst = (uint8_t)st->blend_dst;
        d->zen = (uint8_t)st->z_enable;
        d->zfn = (uint8_t)st->z_func;
        d->zup = (uint8_t)st->z_update;
        d->cull = (uint8_t)st->cull;
        d->aupd = (uint8_t)st->alpha_update;
        d->aref0 = (int16_t)st->aref0;
        d->aref1 = (int16_t)st->aref1;
        d->sx = (uint16_t)st->scissor[0];
        d->sy = (uint16_t)st->scissor[1];
        d->sw = (uint16_t)st->scissor[2];
        d->sh_ = (uint16_t)st->scissor[3];
        break;
    case CMD_CLEAR: port_gpuprof_name("clear"); break;
    case CMD_COPY: port_gpuprof_name("efb copy"); break;
    case CMD_UPLOAD: port_gpuprof_name("texture upload"); break;
    case CMD_COMPOSITE: port_gpuprof_name("3d composite"); break;
    case CMD_PRESENT: port_gpuprof_name("present"); break;
    }
    return 0;
}
void port_gpuprof_dump_shaders(void *file) {
    FILE *f = file;
    int i, k;
    for (i = 0; i < g_num_shaders; i++) {
        fprintf(f, "=== s%03d key", i);
        for (k = 0; k < 8; k++) fprintf(f, " %08x", (unsigned)g_shaders[i].key[k]);
        fprintf(f, "\n--- vertex\n%s\n--- fragment\n%s\n", g_shaders[i].vsrc ? g_shaders[i].vsrc : "(none)",
                g_shaders[i].fsrc ? g_shaders[i].fsrc : "(none)");
    }
}
#endif

static void exec_cmds(int f, int from, int to) {
    RFrame *fr = &g_rf[f];
    int i;
    for (i = from; i < to; i++) {
        const RCmd *c = &fr->cmd[i];
#ifdef PORT_GPUPROF
        if (gp_cmd(fr, c)) continue;
#endif
        switch (c->type) {
        case CMD_DRAW: {
            uint64_t t0 = port_prof_now();
            const BatchState *st = &fr->st[c->u.draw.st];
            uint64_t t1;
            rt_transform(f, c->u.draw.voff + c->u.draw.nv);
            t1 = port_prof_now();
            g_prof[PROF_RT_XF] += t1 - t0;
            g_prof_cnt[PROF_RT_XF]++;
            t0 = t1;
            gl_draw(c->u.draw.sh, st, c->u.draw.texslot, c->u.draw.mode, g_rt_out + c->u.draw.voff, c->u.draw.nv,
                    c->u.draw.ni ? fr->ix + c->u.draw.ioff : NULL, c->u.draw.ni);
            g_prof[PROF_RT_DRAW] += port_prof_now() - t0;
            g_prof_cnt[PROF_RT_DRAW]++;
            break;
        }
#ifdef PORT_VITA
        case CMD_DRAW_STATIC: {
            uint64_t t0 = port_prof_now();
            gl_draw_static(c->u.sdraw.sh, &fr->st[c->u.sdraw.st], c->u.sdraw.texslot, fr->smtx + 21 * c->u.sdraw.mtx,
                           c->u.sdraw.page, c->u.sdraw.first, c->u.sdraw.n);
            g_prof[PROF_RT_DRAW] += port_prof_now() - t0;
            g_prof_cnt[PROF_RT_DRAW]++;
            break;
        }
#endif
        case CMD_UPLOAD: {
            uint64_t t0 = port_prof_now();
            exec_upload(c->u.up.slot, c->u.up.w, c->u.up.h, c->u.up.rgba);
            g_prof[PROF_RT_TEX] += port_prof_now() - t0;
            g_prof_cnt[PROF_RT_TEX]++;
            break;
        }
        case CMD_CLEAR:
            g_gl.valid = 0;
            if (c->u.clear.lowres) {
                r3_bind();
                g_r3_frames++;
                glDisable(GL_SCISSOR_TEST);
                glViewport(0, 0, g_r3_w, g_r3_h);
                glClearColor(c->u.clear.col.r / 255.0f, c->u.clear.col.g / 255.0f, c->u.clear.col.b / 255.0f, 1.0f);
                glDepthMask(GL_TRUE);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                glEnable(GL_SCISSOR_TEST);
                break;
            }
            if (g_rt_lowres) { /* a frame without its composite (never expected) */
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                g_rt_lowres = 0;
            }
            glDisable(GL_SCISSOR_TEST);
            glViewport(0, 0, g_win_w, g_win_h);
            glClearColor(0.0f, getenv("CWOC_DEBUGFS") ? 0.5f : 0.0f, 0, 1);
            glDepthMask(GL_TRUE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
            glScissor(g_off_x, g_win_h - g_off_y - g_draw_h, g_draw_w, g_draw_h);
            glClearColor(c->u.clear.col.r / 255.0f, c->u.clear.col.g / 255.0f, c->u.clear.col.b / 255.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            break;
        case CMD_COPY: {
            uint64_t t0 = port_prof_now();
            exec_copy(c);
            g_prof[PROF_RT_COPY] += port_prof_now() - t0;
            g_prof_cnt[PROF_RT_COPY]++;
            break;
        }
        case CMD_COMPOSITE:
            if (g_rt_lowres) {
                r3_composite();
                g_gl.valid = 0;
                g_r3_hud++;
            }
            break;
        case CMD_PRESENT: {
            uint64_t t0;
            if (g_rt_lowres) { /* no HUD scene this frame: scale up before the swap */
                r3_composite();
                g_gl.valid = 0;
                g_r3_late++;
            }
            rt_fps_tick();
            if (g_fps_raw && g_fps_overlay > 0) rt_fps_overlay();
            rt_icons(c->u.present.frame);
            t0 = port_prof_now();
            if (g_r3_frames >= 300) {
                port_log("gx: 3D at %d%%: %u frames, scaled up at the HUD %u, at the swap %u", g_res3d_pct_rt, g_r3_frames,
                         g_r3_hud, g_r3_late);
                g_r3_frames = g_r3_hud = g_r3_late = 0;
            }
            g_ws_rt = "present";
            port_video_present(c->u.present.frame);
            g_ws_rt = "-";
#ifdef PORT_GPUPROF
            port_gpuprof_present(c->u.present.frame);
#endif
            {
                uint64_t t1 = port_prof_now();
                g_prof[PROF_RT_SWAP] += t1 - t0;
                g_prof_cnt[PROF_RT_SWAP]++;
                rt_res3d_ctrl(t0, t1);
            }
            break;
        }
        }
    }
}

void port_video_gl_attach(void);  /* port_video.c: make the GL context current here */
void port_video_gl_release(void);

static int rt_thread(void *arg) {
    (void)arg;
    { extern void port_thread_register(const char *name); port_thread_register("gxrender"); }
#ifdef PORT_VITA
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
#endif
    port_video_gl_attach();
    for (;;) rq_consume(1);
    return 0;
}

static void rq_init(void) {
    int i;
    const char *e = getenv("CWOC_RT"), *e2;
    for (i = 0; i < 2; i++) {
        g_rf[i].cmd = malloc(sizeof(RCmd) * RQ_CMDS);
        g_rf[i].st = malloc(sizeof(BatchState) * RQ_STATES);
        g_rf[i].v = malloc(sizeof(GLVertex) * RQ_VERTS);
        g_rf[i].ix = malloc(sizeof(u16) * RQ_IDX);
        g_rf[i].seg = malloc(sizeof(XfSeg) * RQ_SEGS);
        g_rf[i].nseg = 0;
        g_rf[i].skw = malloc(sizeof(SkinW) * RQ_VERTS);
        g_rf[i].skm = malloc(sizeof(float) * 256 * RQ_SKINM);
        g_rf[i].smtx = malloc(sizeof(float) * 21 * RQ_SMTX);
        if (!g_rf[i].cmd || !g_rf[i].st || !g_rf[i].v || !g_rf[i].ix || !g_rf[i].seg || !g_rf[i].skw ||
            !g_rf[i].skm || !g_rf[i].smtx)
            port_fatal("gx: render queue allocation failed");
    }
    g_bverts = g_rf[0].v;
    g_bidx = g_rf[0].ix;
    g_rtxf = !(e2 = getenv("CWOC_RTXF")) || e2[0] != '0';
    g_rtskin = g_rtxf && (!(e2 = getenv("CWOC_RTSKIN")) || e2[0] != '0');
    g_rtcopy = g_rtxf && (!(e2 = getenv("CWOC_RTCOPY")) || e2[0] != '0');
    g_game_tid = SDL_ThreadID();
#ifdef PORT_VITA
    g_rt_threaded = !e || e[0] != '0';
#else
    g_rt_threaded = e && e[0] == '1';
#endif
    if (g_rt_threaded) {
        g_rq_m = SDL_CreateMutex();
        g_rq_work = SDL_CreateCond();
        g_rq_done = SDL_CreateCond();
#ifdef PORT_VITA
        sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
#endif
        port_video_gl_release();
        if (!SDL_CreateThreadWithStackSize(rt_thread, "gxrender", 1024 * 1024, NULL)) {
            port_log("gx: render thread failed (%s), running inline", SDL_GetError());
            port_video_gl_attach();
            g_rt_threaded = 0;
        }
    }
    port_log("gx: render %s", g_rt_threaded ? "thread (separate core)" : "inline");
    if (g_rt_threaded && g_rtxf) wk_init();
}

/* game thread: end of frame */
void port_gx_present(int frame) {
    RCmd *c;
    port_gx_flush();
    c = &g_rf[g_pf].cmd[g_ncmd]; /* rq_cmd keeps this slot free */
    GP_TAG_SET(c);
    c->type = CMD_PRESENT;
    c->u.present.frame = frame;
    g_ncmd++;
    rq_publish();
    g_frames_sub++;
    if (g_rt_threaded) {
        uint64_t w0 = port_prof_now();
        PROF_BEGIN(PROF_WAIT);
        g_ws_game = "present";
        SDL_LockMutex(g_rq_m);
        while (g_frames_done < g_frames_sub - 1) SDL_CondWait(g_rq_done, g_rq_m);
        SDL_UnlockMutex(g_rq_m);
        g_ws_game = "-";
        PROF_END(PROF_WAIT);
        /* replayed on the GX thread (recorded frame tail): the game thread
         * counts its own wait for the GX thread instead (rec_close) */
        if (!g_rec_active && !(g_rec_tid && SDL_ThreadID() == g_rec_tid)) g_port_wait_us += port_prof_now() - w0;
    }
    if (g_idxc_nfree) idxc_free_done();
    if (g_sg_hold > 0 && --g_sg_hold == 0) {
        g_sg_page = g_sg_pv = 0;
        g_sg_in = 0;
    }
    g_pf ^= 1;
    g_ncmd = g_nst = g_voff = g_ioff = g_npub_local = 0;
    g_nseg = 0;
    g_nskm = 0;
    g_nsm = 0;
    g_rf[g_pf].nseg = 0;
    g_last_st_valid = 0;
    g_bverts = g_rf[g_pf].v;
    g_bidx = g_rf[g_pf].ix;
}

/* game thread: wait for the render thread (before exiting) */
void port_gx_finish(void) {
    if (!g_rt_threaded) return;
    SDL_LockMutex(g_rq_m);
    while (g_frames_done < g_frames_sub) SDL_CondWait(g_rq_done, g_rq_m);
    SDL_UnlockMutex(g_rq_m);
}

int port_gx_render_threaded(void) { return g_rt_threaded; }

/* before a file load: the render thread may still convert vertices straight
 * from game memory (deferred copies) that the load is about to overwrite.
 * Game thread only (it owns the queue); a no-op elsewhere. */
void port_gx_idle_for_load(void) {
    if (g_game_tid == 0 || (SDL_ThreadID() != g_game_tid && (!g_rec_tid || SDL_ThreadID() != g_rec_tid))) return;
    if (!g_idxc_bytes && !g_skc_bytes && !g_idxc_nfree && (!g_rt_threaded || !g_rtcopy)) return;
    /* emit the pending batch and let the render thread run everything: its
     * segments read game memory (deferred copies) and cached index lists */
    port_gx_flush();
    rq_sync();
    if (g_idxc_bytes || g_skc_bytes || g_idxc_nfree) idxc_flush(); /* game memory may be reloaded */
}

static void draw_block_inner(void);
static void draw_block(void) {
    PROF_BEGIN_S(PROF_DECODE, 16);
    draw_block_inner();
    PROF_END_S(PROF_DECODE, 16);
}

static void xform_verts(GLVertex *v, int n) {
    /* model -> view space (what the vertex shader did with u_posmtx/u_nrmmtx) */
    const float (*m)[4] = gx.posmtx[gx.cur_posmtx];
    const float (*q)[3] = gx.nrmmtx[gx.cur_posmtx];
    int i;
    for (i = 0; i < n; i++, v++) {
        float x = v->pos[0], y = v->pos[1], z = v->pos[2];
        float nx = v->nrm[0], ny = v->nrm[1], nz = v->nrm[2];
        v->pos[0] = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
        v->pos[1] = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
        v->pos[2] = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
        v->nrm[0] = q[0][0] * nx + q[0][1] * ny + q[0][2] * nz;
        v->nrm[1] = q[1][0] * nx + q[1][1] * ny + q[1][2] * nz;
        v->nrm[2] = q[2][0] * nx + q[2][1] * ny + q[2][2] * nz;
    }
}

static void decode_block_slow(GLVertex *dst, int n) {
    const u8 *src = g_fifo;
    int i;
    for (i = 0; i < n; i++, src += g_vs) decode_vertex(src, &dst[i]);
    xform_verts(dst, n);
}

static void decode_block_fast(GLVertex *dst, int n);
static void decode_block_inner(GLVertex *dst, int n);
static void decode_block(GLVertex *dst, int n) {
    PROF_BEGIN_S(PROF_VERTS, 16);
    decode_block_inner(dst, n);
    VSUB(0, 16);
    PROF_END_S(PROF_VERTS, 16);
}

static void decode_block_inner(GLVertex *dst, int n) {
    static int check = -1;
    if (check < 0) check = getenv("CWOC_CHECKDECODE") != NULL;
    if (!g_fast) { g_prof_slow++; decode_block_slow(dst, n); return; }
    g_prof_fast++;
    decode_block_fast(dst, n);
    if (check) {
        static GLVertex tmp[4096];
        static int bad;
        if (n <= 4096) {
            decode_block_slow(tmp, n);
            if (memcmp(tmp, dst, sizeof(GLVertex) * n) && bad++ < 20)
                port_log("decode mismatch fast=%d n=%d pos %f/%f clr %08x/%08x", g_fast, n, tmp[0].pos[0], dst[0].pos[0],
                         *(u32 *)tmp[0].clr, *(u32 *)dst[0].clr);
        }
    }
}

static void seg_add(int vfirst, int count);
static void decode_block_fast(GLVertex *dst, int n) {
    const u8 *src = g_fifo;
    int i;
    /* deferred transform when dst is in the frame arena (render thread) */
    const int defer = g_rtxf && dst >= g_rf[g_pf].v && dst < g_rf[g_pf].v + RQ_VERTS;
    if (defer) seg_add((int)(dst - g_rf[g_pf].v), n);
    {
        const float (*m)[4] = gx.posmtx[gx.cur_posmtx];
        const float (*q)[3] = gx.nrmmtx[gx.cur_posmtx];
        const int hn = g_fast & 2, ht = g_fast & 4;
        for (i = 0; i < n; i++, dst++) {
            float p[3], nr[3];
            memcpy(p, src, 12);
            src += 12;
            if (defer) {
                memcpy(dst->pos, p, 12);
            } else {
                dst->pos[0] = m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3];
                dst->pos[1] = m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3];
                dst->pos[2] = m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3];
            }
            if (hn) {
                memcpy(nr, src, 12);
                src += 12;
                if (defer) {
                    memcpy(dst->nrm, nr, 12);
                } else {
                    dst->nrm[0] = q[0][0] * nr[0] + q[0][1] * nr[1] + q[0][2] * nr[2];
                    dst->nrm[1] = q[1][0] * nr[0] + q[1][1] * nr[1] + q[1][2] * nr[2];
                    dst->nrm[2] = q[2][0] * nr[0] + q[2][1] * nr[1] + q[2][2] * nr[2];
                }
            } else {
                dst->nrm[0] = dst->nrm[1] = dst->nrm[2] = 0.0f;
            }
            memcpy(dst->clr, src, 4);
            src += 4;
            if (ht) {
                memcpy(dst->tex, src, 8);
                src += 8;
            } else {
                dst->tex[0] = dst->tex[1] = 0.0f;
            }
        }
    }
}

#define MODE_QUADS 0xFFFFu /* not a GL primitive: GX_QUADS split into triangles */
static void draw_block_inner(void) {
    int vs = g_vs, n = g_nverts, i, same;
    GLenum mode;
    ShaderEntry *s;
    BatchState cur;
    const BatchState *st;
    if (n <= 0 || vs <= 0) return;
    if (g_batch_on < 0) g_batch_on = getenv("CWOC_NOBATCH") == NULL;
    if (g_breakstat < 0) g_breakstat = getenv("CWOC_BATCHSTAT") != NULL;
    switch (g_prim) {
    case 0x80: mode = MODE_QUADS; break; /* quads (GL_POINTS is 0: use a sentinel) */
    case 0x90: mode = GL_TRIANGLES; break;
    case 0x98: mode = GL_TRIANGLE_STRIP; break;
    case 0xA0: mode = GL_TRIANGLE_FAN; break;
    case 0xA8: mode = GL_LINES; break;
    case 0xB0: mode = GL_LINE_STRIP; break;
    default: mode = GL_POINTS; break;
    }
    same = 0;
    st = &cur;
    if (state_dirty() || !g_bn || !g_bshader || !g_batch_on) {
        PROF_BEGIN_S(PROF_CAPTURE, 16);
        capture_state(&cur);
        same = g_batch_on && g_bn && memcmp(&cur, &g_bstate, sizeof(cur)) == 0;
        PROF_END_S(PROF_CAPTURE, 16);
        if (!same && g_bn && g_breakstat) {
            static const struct { const char *n; size_t o; } F[] = {
                {"key", offsetof(BatchState, key)}, {"blend", offsetof(BatchState, blend_type)},
                {"vp", offsetof(BatchState, vp)}, {"scissor", offsetof(BatchState, scissor)},
                {"num_tev", offsetof(BatchState, num_tev)}, {"tex", offsetof(BatchState, tex)},
                {"texvalid", offsetof(BatchState, texvalid)}, {"proj", offsetof(BatchState, proj)},
                {"posmtx", offsetof(BatchState, posmtx)}, {"amb", offsetof(BatchState, amb)},
                {"lights", offsetof(BatchState, lights)}, {"aref", offsetof(BatchState, aref0)},
                {"fog", offsetof(BatchState, fog_start)}, {"end", sizeof(BatchState)}};
            const u8 *x = (const u8 *)&cur, *y = (const u8 *)&g_bstate;
            size_t k, f;
            for (k = 0; k < sizeof(cur) && x[k] == y[k]; k++) {}
            for (f = 0; F[f + 1].o <= k; f++) {}
            g_breaks[f]++;
            if (port_frame_count() % 300 == 0 && g_breaks_frame != port_frame_count()) {
                char b[512]; int m = 0, j;
                g_breaks_frame = port_frame_count();
                for (j = 0; F[j].o != sizeof(BatchState); j++) { m += snprintf(b + m, sizeof(b) - m, " %s=%d", F[j].n, g_breaks[j]); g_breaks[j] = 0; }
                port_log("batch breaks:%s", b);
            }
        }
    } else {
        same = 1; /* nothing that the batch state depends on changed */
        st = &g_bstate;
    }
    g_dirty = 0;
    int traced = 0;
    if (TRACING()) {
        const float (*m)[4] = st->posmtx;
        traced = 1;
        g_trace--;
        port_log("gx: posfmt desc=%d type=%d frac=%d cnt=%d vtxfmt=%d", gx.vtxdesc[GX_VA_POS], gx.fmt[g_vtxfmt][GX_VA_POS].type,
                 gx.fmt[g_vtxfmt][GX_VA_POS].frac, gx.fmt[g_vtxfmt][GX_VA_POS].cnt, g_vtxfmt);
        port_log("gx: prim=%02X n=%d v0=(%.3f %.3f %.3f) mtx.x=(%.3f %.3f %.3f) t=(%.2f %.2f %.2f) proj00=%.3f 11=%.3f vp=%.0f,%.0f %.0fx%.0f tex0=%dx%d",
                 g_prim, n, 0.0, 0.0, 0.0, m[0][0], m[1][0], m[2][0],
                 m[0][3], m[1][3], m[2][3], st->proj[0][0], st->proj[1][1], st->vp[0], st->vp[1], st->vp[2], st->vp[3],
                 st->texvalid[0] ? st->tex[0].width : 0, st->texvalid[0] ? st->tex[0].height : 0);
        port_log("gx:   blend=%d %d %d z=%d/%d/%d cull=%d ntev=%d amb=%02x%02x%02x%02x mat=%02x%02x%02x%02x aref=%d,%d fog=%.1f-%.1f key=%08x %08x %08x %08x",
                 st->blend_type, st->blend_src, st->blend_dst, st->z_enable, st->z_func, st->z_update, st->cull, st->num_tev,
                 st->amb.r, st->amb.g, st->amb.b, st->amb.a, st->mat.r, st->mat.g, st->mat.b, st->mat.a, st->aref0, st->aref1,
                 st->fog_start, st->fog_end, st->key[0], st->key[1], st->key[2], st->key[3]);
        port_log("gx:   proj %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f vp z %.2f %.2f",
                 st->proj[0][0], st->proj[0][1], st->proj[0][2], st->proj[0][3], st->proj[1][0], st->proj[1][1], st->proj[1][2], st->proj[1][3],
                 st->proj[2][0], st->proj[2][1], st->proj[2][2], st->proj[2][3], st->proj[3][0], st->proj[3][1], st->proj[3][2], st->proj[3][3], st->vp[4], st->vp[5]);
    }
    if (mode == GL_LINES || mode == GL_LINE_STRIP || mode == GL_POINTS) {
        BatchState tmp = *st;
        port_gx_flush();
        rq_ensure(1 + MAX_TEV, n, 0);
        decode_block(g_rf[g_pf].v + g_voff, n);
        s = get_shader();
        record_draw(s, &tmp, mode, g_voff, n, 0, 0);
        g_voff += n;
        g_bverts = g_rf[g_pf].v + g_voff;
        g_batch_epoch++; /* its transform segment is closed (rt_transform) */
        port_gx_stats_draw(n);
        return;
    }
    if (!same || g_bn + n > 65535) {
        port_gx_flush();
        if (!same) {
            g_bstate = cur;
            g_bstate_ver++;
        }
        g_bshader = get_shader();
    }
    /* append the vertices once and index them as a triangle list */
    {
        int base, ni;
        u16 *ix;
        ni = mode == GL_TRIANGLES ? n / 3 * 3 : mode == MODE_QUADS ? n / 4 * 6 : n >= 3 ? (n - 2) * 3 : 0;
        batch_reserve(n, ni);
        base = g_bn;
        decode_block(g_bverts + base, n);
        if (traced)
            port_log("gx:   decoded v0=(%.3f %.3f %.3f) v1=(%.3f %.3f %.3f) clr=%02x%02x%02x%02x uv=(%.3f %.3f)",
                     g_bverts[base].pos[0], g_bverts[base].pos[1], g_bverts[base].pos[2],
                     n > 1 ? g_bverts[base + 1].pos[0] : 0.0f, n > 1 ? g_bverts[base + 1].pos[1] : 0.0f, n > 1 ? g_bverts[base + 1].pos[2] : 0.0f,
                     g_bverts[base].clr[0], g_bverts[base].clr[1], g_bverts[base].clr[2], g_bverts[base].clr[3],
                     g_bverts[base].tex[0], g_bverts[base].tex[1]);
        g_bn += n;
        ix = g_bidx + g_bni;
        if (mode == GL_TRIANGLES) {
            for (i = 0; i < ni; i++) ix[i] = (u16)(base + i);
        } else if (mode == MODE_QUADS) {
            int q;
            for (q = 0; q < n / 4; q++) {
                int v0 = base + q * 4;
                *ix++ = (u16)v0; *ix++ = (u16)(v0 + 1); *ix++ = (u16)(v0 + 2);
                *ix++ = (u16)v0; *ix++ = (u16)(v0 + 2); *ix++ = (u16)(v0 + 3);
            }
        } else if (mode == GL_TRIANGLE_STRIP) {
            for (i = 0; i + 2 < n; i++) {
                int v0 = base + i;
                if (i & 1) { *ix++ = (u16)(v0 + 1); *ix++ = (u16)v0; }
                else { *ix++ = (u16)v0; *ix++ = (u16)(v0 + 1); }
                *ix++ = (u16)(v0 + 2);
            }
        } else { /* fan */
            for (i = 1; i + 1 < n; i++) {
                *ix++ = (u16)base; *ix++ = (u16)(base + i); *ix++ = (u16)(base + i + 1);
            }
        }
        g_bni += ni;
    }
    port_gx_stats_draw(n);
    if (!g_batch_on) port_gx_flush();
}

/* ---- particle quads (renderpsdma, dummyfunc.c; port_quads.h): the caller
 * set the state of GS_DrawQuadListBeginBlock and writes finished vertices
 * here. Same batching and deferred transform as a GX_QUADS block through
 * draw_block_inner, without the FIFO writes and the decode. */
#include "port_quads.h"
_Static_assert(sizeof(PortQuadVtx) == sizeof(GLVertex) && offsetof(PortQuadVtx, clr) == offsetof(GLVertex, clr) &&
                   offsetof(PortQuadVtx, tex) == offsetof(GLVertex, tex),
               "PortQuadVtx must match GLVertex");

/* what port_gx_quads_end left: unchanged batch, state and matrices mean the
 * GX state is still the one of that draw (port_gx_quads_state_kept) */
static struct { const GLVertex *bverts; int bn, bni; u32 ver, frame; unsigned mtx_ver; } g_q_last;

static int rec_quads_kept(void); /* gx_rec.inc */
int port_gx_quads_state_kept(void) {
    if (g_rec_active) return rec_quads_kept();
    return g_bshader && g_bn > 0 && g_q_last.bverts == g_bverts && g_q_last.bn == g_bn &&
           g_q_last.bni == g_bni && g_q_last.ver == g_bstate_ver && g_q_last.mtx_ver == (unsigned)g_mtx_ver &&
           g_q_last.frame == g_frame && !state_dirty();
}

static int g_psdirect = -1; /* CWOC_PSDIRECT=0: old FIFO path (A/B) */
static PortQuadVtx *rec_quads_begin(int maxq); /* gx_rec.inc: recorded for the GX thread */
static void rec_quads_end(int nq);
static PortQuadVtx *quads_begin_impl(int maxq);
static void quads_end_impl(int nq);
static int g_rec_quads_open; /* game thread: port_gx_quads_begin handed out a record */
PortQuadVtx *port_gx_quads_begin(int maxq) {
    if (g_psdirect < 0) g_psdirect = !(getenv("CWOC_PSDIRECT") && getenv("CWOC_PSDIRECT")[0] == '0');
    if (!g_psdirect || maxq <= 0 || maxq * 4 > 65535) return NULL;
    if (g_rec_active) {
        PortQuadVtx *v = rec_quads_begin(maxq);
        g_rec_quads_open = v != NULL;
        return v;
    }
    return quads_begin_impl(maxq);
}
void port_gx_quads_end(int nq) {
    if (g_rec_quads_open) {
        g_rec_quads_open = 0;
        rec_quads_end(nq);
        return;
    }
    quads_end_impl(nq);
}

/* the direct path (game thread outside a recorded pass, or the GX thread
 * replaying a RECOP_QUADS record) */
static PortQuadVtx *quads_begin_impl(int maxq) {
    int n = maxq * 4, same;
    BatchState cur;
    if (g_batch_on < 0) g_batch_on = getenv("CWOC_NOBATCH") == NULL;
    if (state_dirty() || !g_bn || !g_bshader || !g_batch_on) {
        PROF_BEGIN_S(PROF_CAPTURE, 16);
        capture_state(&cur);
        same = g_batch_on && g_bn && memcmp(&cur, &g_bstate, sizeof(cur)) == 0;
        PROF_END_S(PROF_CAPTURE, 16);
    } else {
        same = 1;
    }
    g_dirty = 0;
    if (!same || g_bn + n > 65535) {
        port_gx_flush();
        if (!same) {
            g_bstate = cur;
            g_bstate_ver++;
        }
        g_bshader = get_shader();
    }
    batch_reserve(n, maxq * 6);
    return (PortQuadVtx *)(g_bverts + g_bn);
}

static void quads_end_impl(int nq) {
    int base = g_bn, n = nq * 4, q;
    GLVertex *dst = g_bverts + base;
    u16 *ix = g_bidx + g_bni;
    if (nq <= 0) return;
    if (g_rtxf && dst >= g_rf[g_pf].v && dst < g_rf[g_pf].v + RQ_VERTS)
        seg_add((int)(dst - g_rf[g_pf].v), n); /* the render thread transforms them */
    else
        xform_verts(dst, n);
    for (q = 0; q < nq; q++) {
        int v0 = base + q * 4;
        *ix++ = (u16)v0; *ix++ = (u16)(v0 + 1); *ix++ = (u16)(v0 + 2);
        *ix++ = (u16)v0; *ix++ = (u16)(v0 + 2); *ix++ = (u16)(v0 + 3);
    }
    g_bn += n;
    g_bni += nq * 6;
#ifdef PORT_GPUPROF
    g_gpuprof_fx = 1; /* the batch holds particle quads (pass label " fx") */
#endif
    port_gx_stats_draw(n);
    if (!g_batch_on) port_gx_flush();
    g_q_last.bverts = g_bverts;
    g_q_last.bn = g_bn;
    g_q_last.bni = g_bni;
    g_q_last.ver = g_bstate_ver;
    g_q_last.mtx_ver = (unsigned)g_mtx_ver;
    g_q_last.frame = g_frame;
}

/* ================================================================ direct paths
 * GS_DrawIndexedTriStrip / GS_DrawIndexedTriList (gsprim.c) feed indexed
 * strips of the game's own vertex arrays: transform each referenced vertex once
 * straight into the batch (no FIFO round trip), and index it. */

/* view transform of one vertex into a GLVertex (pos, nrm) after the m00.. /
 * q00.. locals of the direct paths; NEON on the Vita (FZ is on anyway) */
#if defined(PORT_VITA) && defined(__ARM_NEON)
#include <arm_neon.h>
#define XF_SETUP() \
    const float32x4_t xc0 = { m00, m10, m20, 0.0f }, xc1 = { m01, m11, m21, 0.0f }, \
                      xc2 = { m02, m12, m22, 0.0f }, xc3 = { m03, m13, m23, 0.0f }; \
    const float32x4_t xn0 = { q00, q10, q20, 0.0f }, xn1 = { q01, q11, q21, 0.0f }, \
                      xn2 = { q02, q12, q22, 0.0f }
/* stores 4 lanes: pos then nrm (the 4th lane of nrm lands in clr, written after) */
#define XF_POS(d, x, y, z) \
    vst1q_f32((d)->pos, vmlaq_n_f32(vmlaq_n_f32(vmlaq_n_f32(xc3, xc0, (x)), xc1, (y)), xc2, (z)))
#define XF_NRM(d, nx, ny, nz) \
    vst1q_f32((d)->nrm, vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(xn0, (nx)), xn1, (ny)), xn2, (nz)))
#else
#define XF_SETUP() do { } while (0)
#define XF_POS(d, x, y, z) do { \
    (d)->pos[0] = m00 * (x) + m01 * (y) + m02 * (z) + m03; \
    (d)->pos[1] = m10 * (x) + m11 * (y) + m12 * (z) + m13; \
    (d)->pos[2] = m20 * (x) + m21 * (y) + m22 * (z) + m23; } while (0)
#define XF_NRM(d, nx, ny, nz) do { \
    (d)->nrm[0] = q00 * (nx) + q01 * (ny) + q02 * (nz); \
    (d)->nrm[1] = q10 * (nx) + q11 * (ny) + q12 * (nz); \
    (d)->nrm[2] = q20 * (nx) + q21 * (ny) + q22 * (nz); } while (0)
#endif

/* debug aid (CWOC_JUMPLOG=1): log when the view-space size of the vertices a
 * direct draw produces from the same source array jumps between frames */
static int g_jumplog = -1;
static struct { const void *src; float prev, cur; int frame, logged; } g_jump[1024];
static void jump_check(const char *fn, const void *src, int first, int count, const float *skinmtx) {
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f }, size;
    int i, j, h, f = port_frame_count();
    if (g_jumplog < 0) g_jumplog = getenv("CWOC_JUMPLOG") != NULL;
    if (!g_jumplog || count <= 0) return;
    for (i = first; i < first + count; i++)
        for (j = 0; j < 3; j++) {
            float v = g_bverts[i].pos[j];
            if (v < mn[j]) mn[j] = v;
            if (v > mx[j]) mx[j] = v;
        }
    size = mx[0] - mn[0];
    if (mx[1] - mn[1] > size) size = mx[1] - mn[1];
    if (mx[2] - mn[2] > size) size = mx[2] - mn[2];
    h = (int)((((uintptr_t)src >> 2) ^ (uintptr_t)fn[4]) & 1023);
    if (g_jump[h].src != src) {
        g_jump[h].src = src; g_jump[h].prev = g_jump[h].cur = size; g_jump[h].frame = f; g_jump[h].logged = 0;
        return;
    }
    if (g_jump[h].frame != f) {
        g_jump[h].prev = g_jump[h].frame == f - 1 ? g_jump[h].cur : 0.0f;
        g_jump[h].cur = 0.0f;
        g_jump[h].frame = f;
        g_jump[h].logged = 0;
    }
    if (size > g_jump[h].cur) g_jump[h].cur = size;
    if (g_jump[h].prev > 0.0f && size > g_jump[h].prev * 3.0f + 1.0f && !g_jump[h].logged) {
        g_jump[h].logged = 1;
        port_log("jump f%d %s key %p n %d size %.2f (prev frame %.2f) bbox %.2f %.2f %.2f / %.2f %.2f %.2f", f, fn,
                 src, count, size, g_jump[h].prev, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
        if (skinmtx) {
            int k;
            for (k = 0; k < 4; k++)
                port_log("jump   skin[%d] %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", k,
                         skinmtx[16 * k + 0], skinmtx[16 * k + 1], skinmtx[16 * k + 2], skinmtx[16 * k + 3],
                         skinmtx[16 * k + 4], skinmtx[16 * k + 5], skinmtx[16 * k + 6], skinmtx[16 * k + 7],
                         skinmtx[16 * k + 8], skinmtx[16 * k + 9], skinmtx[16 * k + 10], skinmtx[16 * k + 11]);
        }
    }
}

/* deferred: copy model space (render thread transforms, rt_transform) */
#define XF_POS_D(d, x, y, z) do { if (defer) { (d)->pos[0] = (x); (d)->pos[1] = (y); (d)->pos[2] = (z); } \
                                  else XF_POS(d, x, y, z); } while (0)
#define XF_NRM_D(d, nx, ny, nz) do { if (defer) { (d)->nrm[0] = (nx); (d)->nrm[1] = (ny); (d)->nrm[2] = (nz); } \
                                     else XF_NRM(d, nx, ny, nz); } while (0)

static u32 g_rm_tag[65536];
static u16 g_rm_idx[65536];
static u32 g_rm_gen = 1;

static void direct_prepare(void) {
    if (g_batch_on < 0) g_batch_on = getenv("CWOC_NOBATCH") == NULL;
    /* an empty pending batch (after a static draw, a full batch or a queue
     * restart) keeps its state: g_dirty tracking is what batching relies on */
    if (state_dirty() || !g_bshader || !g_batch_on) {
        BatchState cur;
        int same;
        capture_state(&cur);
        same = g_batch_on && g_bshader && memcmp(&cur, &g_bstate, sizeof(cur)) == 0;
        if (!same) {
            port_gx_flush();
            g_bstate = cur;
            g_bstate_ver++;
            g_bshader = get_shader();
            g_batch_epoch++;
        }
    }
    g_dirty = 0;
}

static void direct_new_gen(void) {
    if (++g_rm_gen == 0) {
        memset(g_rm_tag, 0, sizeof(g_rm_tag));
        g_rm_gen = 1;
    }
}

/* CWOC_DSUB=1: sampled sub-timers of port_gs_draw_indexed (one call in 8,
 * scaled), reported with the 5 s perf lines: 0 state capture/compare
 * (direct_prepare), 1 index-cache lookup, 2 segment + index copy on a hit,
 * 3 range scan + conversion on a miss, 4 the rest (flush, bookkeeping) */
uint64_t g_dsub[5];
int g_dsub_on = -1;
static int g_ds_live;
static uint64_t g_ds_t;
static inline void ds_mark(int k) {
    if (g_ds_live) {
        uint64_t now = port_prof_now();
        g_dsub[k] += (now - g_ds_t) * 8;
        g_ds_t = now;
    }
}

/* port_gs_draw_indexed, deferred transform: when the call's indices cover a
 * compact range of the vertex array, convert that range sequentially (model
 * space, no tag table) and remap the indices; same triangles, same order.
 * Returns 0 when the range is sparse or too large (caller dedups instead). */
/* Index lists of the game's models are static: cache, per list, the range
 * scan and the triangle list relative to the lowest index (draw_indexed_range
 * then only adds the batch base). Validated by count/mode and a checksum of
 * the list ends; flushed before file loads (port_gx_idle_for_load). */
typedef struct {
    const short *idx;
    u32 count, nitot, chk;
    int strips;
    u16 mn, range;
    u8 reject;
    u16 *rel;
    u32 stamp; /* frame of the last hit: picks the victim of a 2-way set */
    /* static geometry copy: 0 not tried, 1 in GPU memory, 2 not possible */
    u8 sg_state;
    u8 sg_amin;  /* lowest vertex alpha of the copied range (nodisc_variant) */
    u16 sg_page;
    u32 sg_first;      /* first index in the index arena */
    const void *sg_vl; /* vertex array it was made from */
    u32 sg_sum;        /* sampled checksum of its source vertices */
} IdxCache;
#define IDXC_N 16384
static IdxCache g_idxc[IDXC_N];
static int g_idxc_on = -1;

/* Static-source skin lists (port_gs_skin_draw without blend shapes): the
 * slots a list dedups to depend only on the list, so cache, per list, the
 * source vertex of each slot and the list in slots. A hit only records the
 * segment; the render thread / worker reads the sources from the cache
 * (rt_skin_src) and writes the indices (rt_expand_idx). Same validation,
 * flushing and deferred freeing as the index cache. CWOC_SKINCACHE=0: off. */
typedef struct {
    const short *idx;
    u32 n, chk, stamp;
    u16 nuniq;
    u16 *data; /* [nuniq] source vertices, then [n] slots */
} SkinCache;
#define SKC_N 4096
static SkinCache g_skc[SKC_N];
static int g_skc_on = -1;
unsigned g_skc_hit, g_skc_miss;

/* lists evicted from the cache may still be read by the render thread
 * (deferred index expansion of this or the previous frame): free them once
 * the frame that evicted them has been executed (port_gx_present) */
#define IDXC_FREE_N 1024
static struct { u16 *p; int frame; } g_idxc_free[IDXC_FREE_N];
static void rq_sync(void);
static void idxc_flush(void);
static void idxc_defer_free(u16 *p) {
    if (p == NULL) return;
    if (g_idxc_nfree == IDXC_FREE_N) {
        port_gx_flush();
        rq_sync();
        while (g_idxc_nfree > 0) free(g_idxc_free[--g_idxc_nfree].p);
    }
    g_idxc_free[g_idxc_nfree].p = p;
    g_idxc_free[g_idxc_nfree].frame = g_frames_sub;
    g_idxc_nfree++;
}
static void idxc_free_done(void) {
    int i, k = 0, done = g_frames_done;
    for (i = 0; i < g_idxc_nfree; i++) {
        if (g_idxc_free[i].frame < done) free(g_idxc_free[i].p);
        else g_idxc_free[k++] = g_idxc_free[i];
    }
    g_idxc_nfree = k;
}

static void idxc_flush(void) {
    int i;
    for (i = 0; i < IDXC_N; i++) {
        free(g_idxc[i].rel);
        g_idxc[i].rel = NULL;
        g_idxc[i].idx = NULL;
    }
    g_idxc_bytes = 0;
    for (i = 0; i < SKC_N; i++) {
        free(g_skc[i].data);
        g_skc[i].data = NULL;
        g_skc[i].idx = NULL;
    }
    g_skc_bytes = 0;
    while (g_idxc_nfree > 0) free(g_idxc_free[--g_idxc_nfree].p);
    /* every cached list is gone: refill the static arenas from the start, a
     * few frames from now (the GPU may still be reading the last ones) */
    g_sg_hold = 3;
}

static u32 idxc_sum(const short *idx, u32 count) {
    u32 h = count * 2654435761u, i, n = count < 8 ? count : 8;
    for (i = 0; i < n; i++) h = (h ^ (u16)idx[i]) * 16777619u;
    for (i = count - n; i < count; i++) h = (h ^ (u16)idx[i]) * 16777619u;
    return h;
}

/* ---- static geometry (see g_sg_v): a cached list's vertex range copied
 * once, in model space, to GPU memory, with its indices; later hits only
 * record a draw with the model-view matrix */
/* sampled checksum of a list's source vertices (first, middle, last): a
 * vertex array the game rewrites (animated mesh) stops being static */
static u32 sg_sum(const PortGSVertex *v, u32 n) {
    const u32 *w[3];
    u32 h = 2166136261u;
    int i, k;
    w[0] = (const u32 *)&v[0];
    w[1] = (const u32 *)&v[n / 2];
    w[2] = (const u32 *)&v[n - 1];
    for (i = 0; i < 3; i++)
        for (k = 0; k < 9; k++) h = (h ^ w[i][k]) * 16777619u;
    return h;
}

static int sg_make(IdxCache *c, const PortGSVertex *vl) {
    const PortGSVertex *sv = vl + c->mn;
    const u32 range = c->range, n = c->nitot;
    GLVertex *d;
    u16 *ix;
    u32 j;
    if ((u32)g_sg_pv + range > SG_PAGE_VERTS) {
        if (g_sg_page + 1 >= g_sg_npages) goto full;
        g_sg_page++;
        g_sg_pv = 0;
    }
    if (g_sg_in + n > g_sg_icap) goto full;
    d = g_sg_v + (size_t)g_sg_page * SG_PAGE_VERTS + g_sg_pv;
    c->sg_amin = 255;
    for (j = 0; j < range; j++, d++, sv++) {
        u32 cl = sv->diffuse;
        if ((u8)cl < c->sg_amin) c->sg_amin = (u8)cl;
        memcpy(d->pos, &sv->x, 24); /* pos + nrm, model space */
        d->clr[0] = (u8)(cl >> 24);
        d->clr[1] = (u8)(cl >> 16);
        d->clr[2] = (u8)(cl >> 8);
        d->clr[3] = (u8)cl;
        d->tex[0] = sv->u;
        d->tex[1] = sv->v;
    }
    ix = g_sg_i + g_sg_in;
    for (j = 0; j < n; j++) ix[j] = (u16)(c->rel[j] + g_sg_pv);
    c->sg_page = (u16)g_sg_page;
    c->sg_first = g_sg_in;
    c->sg_vl = vl;
    c->sg_sum = sg_sum(vl + c->mn, range);
    c->sg_state = 1;
    g_sg_pv += (int)range;
    g_sg_in += n;
    g_sg_made++;
    return 1;
full:
    c->sg_state = 2;
    g_sg_full++;
    return 0;
}

/* GX thread, ahead of the replay (gx_rec.inc rec_prefetch): load the memory
 * an indexed draw's cache lookup will read, so its misses overlap the work of
 * the draws before it. stage 1: the list's first and last indices and its
 * 2-way set; stage 2 (the set is cached by then): the 3 vertices sg_sum
 * reads. Prefetches only: no effect on what is drawn. */
static inline void idxc_prefetch(const short *idx, u32 count, const PortGSVertex *vl, int stage) {
    u32 h = (u32)(((uintptr_t)idx >> 1) * 2654435761u) >> 18;
    IdxCache *c0 = &g_idxc[h & ~1u];
    if (stage == 1) {
        __builtin_prefetch(idx);
        if (count > 8) __builtin_prefetch(idx + count - 8);
        __builtin_prefetch(c0);
        __builtin_prefetch(c0 + 1);
    } else {
        IdxCache *c = c0[1].idx == idx ? c0 + 1 : c0[0].idx == idx ? c0 : NULL;
        if (c && c->sg_state == 1 && c->sg_vl == (const void *)vl) {
            const PortGSVertex *v = vl + c->mn;
            __builtin_prefetch(v);
            __builtin_prefetch(v + c->range / 2);
            __builtin_prefetch(v + c->range - 1);
        }
    }
}

static int draw_indexed_range(int strips, const short *idx, u32 count, const PortGSVertex *vl, int constcol, u32 col) {
    if (g_rec_bypass) return 0; /* copied by the GX recorder: convert now, keep no pointer */
    const short *p = idx;
    u32 left = count, total = 0, nitot = 0;
    unsigned mn = 0xFFFF, mx = 0, range, k;
    int base, i;
    u16 *ix;
    GLVertex *d;
    IdxCache *c = NULL;
    if (g_idxc_on < 0) g_idxc_on = getenv("CWOC_IDXCACHE") == NULL || getenv("CWOC_IDXCACHE")[0] != '0';
    if (g_idxc_on && g_rtcopy && count > 0) {
        u32 chk = idxc_sum(idx, count);
        /* 2-way set, multiplicative hash: the old (idx>>1 ^ idx>>13) slot made
         * lists in one level evict each other every frame (82 misses a frame
         * at the start of the bonus castle, each a full rescan) */
        u32 h = (u32)(((uintptr_t)idx >> 1) * 2654435761u) >> 18;
        IdxCache *c0 = &g_idxc[h & ~1u], *c1 = c0 + 1;
        u32 now = (u32)port_frame_count();
        if (c1->idx == idx) c = c1;
        else if (c0->idx == idx) c = c0;
        else if (c0->idx == NULL) c = c0;
        else if (c1->idx == NULL) c = c1;
        else c = (c0->stamp <= c1->stamp) ? c0 : c1;
        if (c->idx == idx && c->count == count && c->strips == strips && c->chk == chk) {
            c->stamp = now;
            if (c->reject) return 0;
            if (c->rel != NULL) {
                if (c->sg_state != 2 && !g_sg_hold && __atomic_load_n(&g_sg_ready, __ATOMIC_ACQUIRE)) {
                    int ok;
                    if (constcol) {
                        ok = 0; /* the colour may change per call: deferred copy */
                        g_sg_cc++;
                    } else if (c->sg_state == 1) {
                        ok = c->sg_vl == (const void *)vl && sg_sum(vl + c->mn, c->range) == c->sg_sum;
                        if (!ok) {
                            c->sg_state = 2;
                            g_sg_dyn++;
                        }
                    } else {
                        ok = sg_make(c, vl);
                    }
                    if (ok) {
                        ds_mark(1);
                        batch_emit(); /* draw order: the pending batch goes first */
                        record_draw_static(c->sg_first, c->nitot, c->sg_page, c->sg_amin);
                        port_gx_stats_draw((int)c->range);
                        g_idxc_hit++;
                        ds_mark(2);
                        return 1;
                    }
                }
                ds_mark(1);
                const u16 *r = c->rel;
                u32 n = c->nitot, j;
                u16 b;
                batch_reserve((int)c->range, (int)n);
                base = g_bn;
                {
                    /* the render thread writes the indices (rt_expand_idx) just
                     * before the draw that uses them: the list stays alive until
                     * then (evicted lists are freed two frames later) */
                    XfSeg *sg;
                    g_seg_nomerge = 1;
                    seg_add(g_voff + base, (int)c->range);
                    g_seg_nomerge = 0;
                    sg = &g_rf[g_pf].seg[g_nseg - 1];
                    sg->src = &vl[c->mn];
                    sg->constcol = constcol;
                    sg->col = col;
                    sg->rel = r;
                    sg->rel_n = n;
                    sg->ixa = (u32)(g_bidx - g_rf[g_pf].ix) + (u32)g_bni;
                    sg->ibase = (u16)base;
                }
                g_bn += (int)c->range;
                (void)ix; (void)b; (void)j;
                g_bni += (int)n;
                port_gx_stats_draw((int)c->range);
                g_idxc_hit++;
                ds_mark(2);
                return 1;
            }
        }
        /* (re)fill this slot below */
        g_idxc_miss++;
        idxc_defer_free(c->rel);
        if (c->rel) g_idxc_bytes -= c->nitot * 2;
        c->rel = NULL;
        c->idx = idx;
        c->count = count;
        c->strips = strips;
        c->chk = chk;
        c->reject = 0;
        c->stamp = now;
        c->sg_state = 0;
    }
    while (left > 0) {
        int n;
        if (strips) {
            n = *p++;
            left -= (u32)n + 1;
        } else {
            n = (int)left;
            left = 0;
        }
        if (n <= 0) continue;
        for (i = 0; i < n; i++) {
            unsigned v = (u16)p[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        total += (u32)n;
        nitot += strips ? (n >= 3 ? (u32)(n - 2) * 3 : 0) : (u32)(n / 3 * 3);
        p += n;
    }
    if (total == 0 || mx < mn) {
        if (c) c->reject = 1;
        return 0;
    }
    range = mx - mn + 1;
    if (range > 30000 || range > total * 2 || nitot > 90000) {
        if (c) c->reject = 1;
        return 0;
    }
    batch_reserve((int)range, (int)nitot);
    base = g_bn;
    if (g_rtcopy) {
        /* the render thread converts the range from the source (rt_copy_xf) */
        XfSeg *sg;
        g_seg_nomerge = 1;
        seg_add(g_voff + base, (int)range);
        g_seg_nomerge = 0;
        sg = &g_rf[g_pf].seg[g_nseg - 1];
        sg->src = &vl[mn];
        sg->constcol = constcol;
        sg->col = col;
        goto indices;
    }
    d = &g_bverts[base];
    for (k = mn; k <= mx; k++, d++) {
        const PortGSVertex *sv = &vl[k];
        u32 c = constcol ? col : sv->diffuse;
        memcpy(d->pos, &sv->x, 24); /* pos + nrm, model space */
        d->clr[0] = (u8)(c >> 24);
        d->clr[1] = (u8)(c >> 16);
        d->clr[2] = (u8)(c >> 8);
        d->clr[3] = (u8)c;
        d->tex[0] = sv->u;
        d->tex[1] = sv->v;
    }
    seg_add(g_voff + base, (int)range);
indices:
    g_bn += (int)range;
    ix = g_bidx + g_bni;
    p = idx;
    left = count;
    while (left > 0) {
        int n;
        if (strips) {
            n = *p++;
            left -= (u32)n + 1;
        } else {
            n = (int)left;
            left = 0;
        }
        if (n <= 0) continue;
        if (strips) {
            u16 a = (u16)(base + ((u16)p[0] - mn)), b = n > 1 ? (u16)(base + ((u16)p[1] - mn)) : 0;
            for (i = 2; i < n; i++) {
                u16 c = (u16)(base + ((u16)p[i] - mn));
                if (i & 1) { *ix++ = b; *ix++ = a; }
                else { *ix++ = a; *ix++ = b; }
                *ix++ = c;
                a = b;
                b = c;
            }
        } else {
            int ni = n / 3 * 3;
            for (i = 0; i < ni; i++) *ix++ = (u16)(base + ((u16)p[i] - mn));
        }
        port_gx_stats_draw(n);
        p += n;
    }
    if (c && nitot > 0 && g_idxc_bytes + nitot * 2 <= (8u << 20)) {
        /* remember the list relative to the lowest index */
        u16 *rel = malloc(nitot * 2);
        if (rel) {
            const u16 *src = g_bidx + g_bni;
            u32 j;
            for (j = 0; j < nitot; j++) rel[j] = (u16)(src[j] - base);
            c->rel = rel;
            c->nitot = nitot;
            c->mn = (u16)mn;
            c->range = (u16)range;
            g_idxc_bytes += nitot * 2;
        }
    }
    g_bni += (int)nitot;
    ds_mark(3);
    return 1;
}

void port_gs_draw_indexed(int strips, const short *idx, u32 count, const void *verts, int constcol, u32 col) { GXO();
    const PortGSVertex *vl = verts;
    const float (*m)[4] = gx.posmtx[gx.cur_posmtx];
    const float (*q)[3] = gx.nrmmtx[gx.cur_posmtx];
    const float m00 = m[0][0], m01 = m[0][1], m02 = m[0][2], m03 = m[0][3];
    const float m10 = m[1][0], m11 = m[1][1], m12 = m[1][2], m13 = m[1][3];
    const float m20 = m[2][0], m21 = m[2][1], m22 = m[2][2], m23 = m[2][3];
    const float q00 = q[0][0], q01 = q[0][1], q02 = q[0][2];
    const float q10 = q[1][0], q11 = q[1][1], q12 = q[1][2];
    const float q20 = q[2][0], q21 = q[2][1], q22 = q[2][2];
    XF_SETUP();
    const int defer = g_rtxf;
    u32 epoch;
    PROF_BEGIN_S(PROF_VERTS, 4);
    {
        static unsigned dsc;
        if (g_dsub_on < 0) g_dsub_on = getenv("CWOC_DSUB") != NULL;
        g_ds_live = g_dsub_on && (++dsc & 7) == 0;
        if (g_ds_live) g_ds_t = port_prof_now();
    }
    direct_prepare();
    direct_new_gen();
    ds_mark(0);
    epoch = g_batch_epoch;
    if (defer && draw_indexed_range(strips, idx, count, vl, constcol, col)) {
        if (!g_batch_on) port_gx_flush();
        ds_mark(4);
        g_ds_live = 0;
        VSUB(1, 4);
        PROF_END_S(PROF_VERTS, 4);
        return;
    }
    g_ds_live = 0;
    {
    int jstart = g_bn;
    const void *idx0 = idx;
    while (count > 0) {
        int n, i, ni;
        const short *p;
        u16 loc[3];
        int segfirst;
        u16 *ix;
        if (strips) {
            n = *idx++;
            count -= (u32)n + 1;
        } else {
            n = (int)count;
            count = 0;
        }
        if (n <= 0) continue;
        ni = strips ? (n >= 3 ? (n - 2) * 3 : 0) : n / 3 * 3;
        batch_reserve(n, ni);
        if (g_batch_epoch != epoch || g_bn == 0) {
            direct_new_gen();
            epoch = g_batch_epoch;
        }
        segfirst = g_voff + g_bn;
        ix = g_bidx + g_bni;
        p = idx;
        for (i = 0; i < n; i++) {
            unsigned k = (u16)p[i];
            u16 li;
            if (g_rm_tag[k] != g_rm_gen) {
                const PortGSVertex *sv = &vl[k];
                GLVertex *d = &g_bverts[g_bn];
                float x = sv->x, y = sv->y, z = sv->z, nx = sv->nx, ny = sv->ny, nz = sv->nz;
                u32 c = constcol ? col : sv->diffuse;
                XF_POS_D(d, x, y, z);
                XF_NRM_D(d, nx, ny, nz);
                d->clr[0] = (u8)(c >> 24);
                d->clr[1] = (u8)(c >> 16);
                d->clr[2] = (u8)(c >> 8);
                d->clr[3] = (u8)c;
                d->tex[0] = sv->u;
                d->tex[1] = sv->v;
                g_rm_tag[k] = g_rm_gen;
                g_rm_idx[k] = (u16)g_bn;
                li = (u16)g_bn++;
            } else {
                li = g_rm_idx[k];
            }
            if (strips) {
                if (i < 2) {
                    loc[i] = li;
                } else {
                    if (i & 1) { *ix++ = loc[1]; *ix++ = loc[0]; }
                    else { *ix++ = loc[0]; *ix++ = loc[1]; }
                    *ix++ = li;
                    loc[0] = loc[1];
                    loc[1] = li;
                }
            } else if (i < ni) {
                *ix++ = li;
            }
        }
        g_bni += ni;
        idx += n;
        if (defer) seg_add(segfirst, g_voff + g_bn - segfirst);
        port_gx_stats_draw(n);
    }
    if (g_bn >= jstart) jump_check("indexed", idx0, jstart, g_bn - jstart, NULL);
    }
    if (!g_batch_on) port_gx_flush();
    VSUB(2, 4);
    PROF_END_S(PROF_VERTS, 4);
}

/* GS_DrawTriListTSkin (lit path): skinned positions in order, normal/colour/uv
 * from the source vertices through an index list */
typedef struct { float x, y, z, nx, ny, nz; } PortGSVertexNorm;
typedef struct { float x, y, z, w[2], ix[3], nx, ny, nz; u32 diffuse; float u, v; } PortGSVertexSkin;

void port_gs_draw_tskin(const void *vertlist, int n, const void *srcverts, const short *idx) { GXO();
    const PortGSVertexNorm *pv = vertlist;
    const PortGSVertexSkin *sv = srcverts;
    const float (*m)[4] = gx.posmtx[gx.cur_posmtx];
    const float (*q)[3] = gx.nrmmtx[gx.cur_posmtx];
    const float m00 = m[0][0], m01 = m[0][1], m02 = m[0][2], m03 = m[0][3];
    const float m10 = m[1][0], m11 = m[1][1], m12 = m[1][2], m13 = m[1][3];
    const float m20 = m[2][0], m21 = m[2][1], m22 = m[2][2], m23 = m[2][3];
    const float q00 = q[0][0], q01 = q[0][1], q02 = q[0][2];
    const float q10 = q[1][0], q11 = q[1][1], q12 = q[1][2];
    const float q20 = q[2][0], q21 = q[2][1], q22 = q[2][2];
    XF_SETUP();
    const int defer = g_rtxf;
    int done = 0;
    PROF_BEGIN_S(PROF_VERTS, 4);
    direct_prepare();
    while (done < n) {
        int k = n - done, i, base;
        u16 *ix;
        if (k > 30000) k = 30000;
        k = k / 3 * 3;
        if (k <= 0) break;
        batch_reserve(k, k);
        base = g_bn;
        ix = g_bidx + g_bni;
        for (i = 0; i < k; i++) {
            const PortGSVertexNorm *p = &pv[done + i];
            const PortGSVertexSkin *s = &sv[idx[done + i]];
            GLVertex *d = &g_bverts[base + i];
            float x = p->x, y = p->y, z = p->z, nx = s->nx, ny = s->ny, nz = s->nz;
            u32 c = s->diffuse;
            XF_POS_D(d, x, y, z);
            XF_NRM_D(d, nx, ny, nz);
            d->clr[0] = (u8)(c >> 24);
            d->clr[1] = (u8)(c >> 16);
            d->clr[2] = (u8)(c >> 8);
            d->clr[3] = (u8)c;
            d->tex[0] = s->u;
            d->tex[1] = s->v;
            ix[i] = (u16)(base + i);
        }
        if (defer) seg_add(g_voff + base, k);
        g_bn += k;
        g_bni += k;
        done += k;
        port_gx_stats_draw(k);
    }
    if (!g_batch_on) port_gx_flush();
    VSUB(3, 4);
    PROF_END_S(PROF_VERTS, 4);
}

/* SkinnedShader / SkinnedShaderBlend + GS_DrawTriListTSkin (lit path) fused:
 * the game skins every index of a triangle list into TVertices (each vertex
 * ~6 times) and the draw transforms every index again. Here each referenced
 * source vertex is skinned and transformed once per batch and the triangles
 * are drawn indexed. Same float operations per vertex as the original path. */
typedef struct { float x, y, z; } PortVec3;
int port_gs_skin_fused(void) { GXO(); /* CWOC_NOFUSE=1: the original two passes (comparison) */
    static int on = -1;
    if (on < 0) on = getenv("CWOC_NOFUSE") == NULL;
    return on;
}
static void skin_mul34(float out[3], const float *v, const float *m) {
    out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];
    out[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7];
    out[2] = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11];
}

void port_gs_skin_draw(int n, const short *idx, const void *srcverts, const void *blendsrc, const float *skinmtx,
                       float one, int lit, u32 constcol) {
    const PortGSVertexSkin *sv = srcverts;
    const PortVec3 *bs = blendsrc;
    const float (*m)[4] = gx.posmtx[gx.cur_posmtx];
    const float (*q)[3] = gx.nrmmtx[gx.cur_posmtx];
    const float m00 = m[0][0], m01 = m[0][1], m02 = m[0][2], m03 = m[0][3];
    const float m10 = m[1][0], m11 = m[1][1], m12 = m[1][2], m13 = m[1][3];
    const float m20 = m[2][0], m21 = m[2][1], m22 = m[2][2], m23 = m[2][3];
    const float q00 = q[0][0], q01 = q[0][1], q02 = q[0][2];
    const float q10 = q[1][0], q11 = q[1][1], q12 = q[1][2];
    const float q20 = q[2][0], q21 = q[2][1], q22 = q[2][2];
    XF_SETUP();
    const int defer = g_rtxf;
    const int dskin = g_rtskin;
    int done = 0;
    u32 epoch;
    PROF_BEGIN_S(PROF_VERTS, 4);
    direct_prepare();
    direct_new_gen();
    epoch = g_batch_epoch;
    {
    int jstart = g_bn;
    while (done < n) {
        int k = n - done, i, segfirst, skm = -1;
        u16 *ix;
        if (k > 30000) k = 30000;
        k = k / 3 * 3;
        if (k <= 0) break;
        batch_reserve(k, k);
        if (g_batch_epoch != epoch || g_bn == 0) {
            direct_new_gen();
            epoch = g_batch_epoch;
        }
        segfirst = g_voff + g_bn;
        ix = g_bidx + g_bni;
        if (dskin) {
            /* rq_ensure keeps room for a set; the vertices skinned on the render thread (rt_skin) */
            skm = g_nskm++;
            memcpy(g_rf[g_pf].skm + 256 * skm, skinmtx, 256 * sizeof(float));
            if (bs == NULL && g_rtcopy) {
                /* static source: record the source index only (rt_skin_src) */
                SkinW *skw = g_rf[g_pf].skw + g_voff;
                SkinCache *sc = NULL;
                const int base0 = g_bn;
                if (g_skc_on < 0) g_skc_on = !(getenv("CWOC_SKINCACHE") && getenv("CWOC_SKINCACHE")[0] == '0');
                if (g_skc_on && done == 0 && k == n && g_bn == segfirst - g_voff) {
                    u32 chk = idxc_sum(idx, (u32)n);
                    u32 h = (u32)(((uintptr_t)idx >> 1) * 2654435761u) >> 20;
                    SkinCache *c0 = &g_skc[(h << 1) & (SKC_N - 1)], *c1 = c0 + 1;
                    u32 now = (u32)port_frame_count();
                    if (c1->idx == idx) sc = c1;
                    else if (c0->idx == idx) sc = c0;
                    else if (c0->idx == NULL) sc = c0;
                    else if (c1->idx == NULL) sc = c1;
                    else sc = (c0->stamp <= c1->stamp) ? c0 : c1;
                    if (sc->idx == idx && sc->n == (u32)n && sc->chk == chk && sc->data != NULL) {
                        XfSeg *sg;
                        sc->stamp = now;
                        seg_add_skin(segfirst, sc->nuniq, skm, one);
                        sg = &g_rf[g_pf].seg[g_nseg - 1];
                        sg->src = sv;
                        sg->constcol = lit;
                        sg->col = constcol;
                        sg->uniq = sc->data;
                        sg->rel = sc->data + sc->nuniq;
                        sg->rel_n = (u32)k;
                        sg->ixa = (u32)(g_bidx - g_rf[g_pf].ix) + (u32)g_bni;
                        sg->ibase = (u16)base0;
                        g_bn += sc->nuniq;
                        g_bni += k;
                        done += k;
                        g_skc_hit++;
                        port_gx_stats_draw(k);
                        continue;
                    }
                    /* refill this slot after the scan below */
                    g_skc_miss++;
                    if (sc->data) g_skc_bytes -= ((size_t)sc->nuniq + sc->n) * 2;
                    idxc_defer_free(sc->data);
                    sc->data = NULL;
                    sc->idx = idx;
                    sc->n = (u32)n;
                    sc->chk = chk;
                    sc->stamp = now;
                }
                for (i = 0; i < k; i++) {
                    unsigned sk = (u16)idx[done + i];
                    if (g_rm_tag[sk] != g_rm_gen) {
                        skw[g_bn].sk = (u16)sk;
                        g_rm_tag[sk] = g_rm_gen;
                        g_rm_idx[sk] = (u16)g_bn;
                        ix[i] = (u16)g_bn++;
                    } else {
                        ix[i] = g_rm_idx[sk];
                    }
                }
                if (sc != NULL) {
                    u32 nu = (u32)(g_bn - base0), j;
                    size_t bytes = ((size_t)nu + (size_t)k) * 2;
                    u16 *data = (nu > 0 && g_skc_bytes + bytes <= (4u << 20)) ? malloc(bytes) : NULL;
                    if (data) {
                        for (j = 0; j < nu; j++) data[j] = skw[base0 + j].sk;
                        for (j = 0; j < (u32)k; j++) data[nu + j] = (u16)(ix[j] - base0);
                        sc->data = data;
                        sc->nuniq = (u16)nu;
                        g_skc_bytes += bytes;
                    } else {
                        sc->idx = NULL;
                    }
                }
                g_bni += k;
                done += k;
                seg_add_skin(segfirst, g_voff + g_bn - segfirst, skm, one);
                if (g_bn > segfirst - g_voff) {
                    XfSeg *sg = &g_rf[g_pf].seg[g_nseg - 1];
                    sg->src = sv;
                    sg->constcol = lit;
                    sg->col = constcol;
                }
                port_gx_stats_draw(k);
                continue;
            }
            for (i = 0; i < k; i++) {
                unsigned sk = (u16)idx[done + i];
                if (g_rm_tag[sk] != g_rm_gen) {
                    const PortGSVertexSkin *s = &sv[(short)sk];
                    const float *v = bs ? &bs[(short)sk].x : &s->x;
                    GLVertex *d = &g_bverts[g_bn];
                    SkinW *w = &g_rf[g_pf].skw[g_voff + g_bn];
                    u32 c = lit ? s->diffuse : constcol;
                    d->pos[0] = v[0]; d->pos[1] = v[1]; d->pos[2] = v[2];
                    w->w0 = s->w[0];
                    w->w1 = s->w[1];
                    w->i0 = (u8)((int)s->ix[0] & 15);
                    if (s->w[0] != 1.0f) {
                        w->i1 = (u8)((int)s->ix[1] & 15);
                        w->i2 = (u8)((int)s->ix[2] & 15);
                    }
                    if (lit) {
                        d->nrm[0] = s->nx; d->nrm[1] = s->ny; d->nrm[2] = s->nz;
                        d->tex[0] = s->u;
                        d->tex[1] = s->v;
                    } else {
                        d->nrm[0] = d->nrm[1] = d->nrm[2] = 0.0f;
                        d->tex[0] = d->tex[1] = 0.0f;
                    }
                    d->clr[0] = (u8)(c >> 24);
                    d->clr[1] = (u8)(c >> 16);
                    d->clr[2] = (u8)(c >> 8);
                    d->clr[3] = (u8)c;
                    g_rm_tag[sk] = g_rm_gen;
                    g_rm_idx[sk] = (u16)g_bn;
                    ix[i] = (u16)g_bn++;
                } else {
                    ix[i] = g_rm_idx[sk];
                }
            }
            g_bni += k;
            done += k;
            seg_add_skin(segfirst, g_voff + g_bn - segfirst, skm, one);
            port_gx_stats_draw(k);
            continue;
        }
        for (i = 0; i < k; i++) {
            unsigned sk = (u16)idx[done + i];
            if (g_rm_tag[sk] != g_rm_gen) {
                const PortGSVertexSkin *s = &sv[(short)sk];
                const float *v = bs ? &bs[(short)sk].x : &s->x;
                GLVertex *d = &g_bverts[g_bn];
                float p[3], x, y, z;
                float nx = s->nx, ny = s->ny, nz = s->nz;
                u32 c = lit ? s->diffuse : constcol;
                if (s->w[0] == 1.0f) {
                    skin_mul34(p, v, skinmtx + 16 * (int)s->ix[0]);
                } else {
                    float a[3], b[3], e[3];
                    float w0 = s->w[0], w1 = s->w[1], w2 = one - (w0 + w1);
                    skin_mul34(a, v, skinmtx + 16 * (int)s->ix[0]);
                    skin_mul34(b, v, skinmtx + 16 * (int)s->ix[1]);
                    skin_mul34(e, v, skinmtx + 16 * (int)s->ix[2]);
                    p[0] = a[0] * w0 + b[0] * w1 + e[0] * w2;
                    p[1] = a[1] * w0 + b[1] * w1 + e[1] * w2;
                    p[2] = a[2] * w0 + b[2] * w1 + e[2] * w2;
                }
                x = p[0]; y = p[1]; z = p[2];
                XF_POS_D(d, x, y, z);
                if (lit) {
                    XF_NRM_D(d, nx, ny, nz);
                    d->tex[0] = s->u;
                    d->tex[1] = s->v;
                } else { /* shadow pass: GX position + colour only */
                    d->nrm[0] = d->nrm[1] = d->nrm[2] = 0.0f;
                    d->tex[0] = d->tex[1] = 0.0f;
                }
                d->clr[0] = (u8)(c >> 24);
                d->clr[1] = (u8)(c >> 16);
                d->clr[2] = (u8)(c >> 8);
                d->clr[3] = (u8)c;
                g_rm_tag[sk] = g_rm_gen;
                g_rm_idx[sk] = (u16)g_bn;
                ix[i] = (u16)g_bn++;
            } else {
                ix[i] = g_rm_idx[sk];
            }
        }
        g_bni += k;
        done += k;
        if (defer) seg_add(segfirst, g_voff + g_bn - segfirst);
        port_gx_stats_draw(k);
    }
    if (g_bn >= jstart && !dskin) jump_check(bs ? "skinblend" : "skin", idx, jstart, g_bn - jstart, skinmtx);
    }
    if (!g_batch_on) port_gx_flush();
    VSUB(4, 4);
    PROF_END_S(PROF_VERTS, 4);
}

static void gx_block_done(void) {
    g_in_begin = 0;
    port_gx_end = g_fifo;
    draw_block();
    g_fifo_len = 0;
    port_gx_wp = g_fifo;
}

void GXBegin(int prim, int vtxfmt, u16 nverts) { GXO();
    if (g_in_begin && port_gx_wp != g_fifo)
        port_log("gx: GXBegin while a block is incomplete (%d bytes)", (int)(port_gx_wp - g_fifo));
    g_prim = prim;
    g_vtxfmt = vtxfmt & 7;
    g_nverts = nverts;
    g_fifo_len = 0;
    port_gx_wp = g_fifo;
    if (g_plan_fmt != g_vtxfmt) {
        build_plan();
        g_plan_fmt = g_vtxfmt;
    }
    g_need = g_vs * nverts;
    g_in_begin = nverts > 0 && g_vs > 0 && g_need <= (int)sizeof(g_fifo) - 64;
    port_gx_end = g_in_begin ? g_fifo + g_need : g_fifo;
}
void GXEnd(void) { GXO();}

/* -------------------------------------------------------------- EFB copies */
void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) { GXO();
    gx.copy_src[0] = left; gx.copy_src[1] = top; gx.copy_src[2] = wd; gx.copy_src[3] = ht;
}
void GXSetTexCopyDst(u16 wd, u16 ht, int fmt, u8 mipmap) { GXO();
    gx.copy_dst_w = wd; gx.copy_dst_h = ht; gx.copy_fmt = fmt; gx.copy_mip = mipmap;
}
/* Reads back the drawable and stores it in GameCube RGB565 tiled layout, so a
 * later GXInitTexObj/GXLoadTexObj of 'dest' decodes it like any texture. */
void GXCopyTex(void *dest, u8 clear) { GXO();
    int w = gx.copy_dst_w, h = gx.copy_dst_h, x;
    float sx = (float)g_draw_w / 640.0f, sy = (float)g_draw_h / 448.0f;
    int rw = (int)(gx.copy_src[2] * sx), rh = (int)(gx.copy_src[3] * sy);
    RCmd *c;
    (void)clear;
    if (!dest || w <= 0 || h <= 0 || rw <= 0 || rh <= 0) return;
    port_gx_flush();
    for (x = 0; x < TEX_CACHE_SIZE; x++)
        if (g_texcache[x].image == dest) g_texcache[x].checked = 0; /* contents change */
    rq_ensure(1, 0, 0);
    c = rq_cmd();
    c->type = CMD_COPY;
    c->u.copy.dest = dest;
    c->u.copy.w = w;
    c->u.copy.h = h;
    c->u.copy.rx = g_off_x + (int)(gx.copy_src[0] * sx);
    c->u.copy.ry = g_win_h - g_off_y - (int)((gx.copy_src[1] + gx.copy_src[3]) * sy);
    c->u.copy.rw = rw;
    c->u.copy.rh = rh;
    rq_commit();
    rq_sync(); /* the game reads the copy back as a texture */
}

/* render thread: read the drawable back into GameCube RGB565 tiled layout */
static void exec_copy(const RCmd *c) {
    int w = c->u.copy.w, h = c->u.copy.h, rw = c->u.copy.rw, rh = c->u.copy.rh, x, y, bx, by;
    int rx = c->u.copy.rx, ry = c->u.copy.ry;
    u8 *pix, *out = c->u.copy.dest;
    if (g_rt_lowres) r3_map(&rx, &ry, &rw, &rh); /* the 3D target holds the picture (CWOC_RES3D) */
    if (rw <= 0 || rh <= 0) return;
    pix = malloc((size_t)rw * rh * 4);
    glReadPixels(rx, ry, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    for (by = 0; by < h; by += 4)
        for (bx = 0; bx < w; bx += 4)
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++) {
                    int px = (bx + x) * rw / w, py = (by + y) * rh / h;
                    const u8 *s = pix + ((rh - 1 - py) * rw + px) * 4;
                    u16 v = (u16)(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3));
                    *out++ = (u8)(v >> 8);
                    *out++ = (u8)v;
                }
    free(pix);
}

/* -------------------------------------------------------------- frame */
/* CWOC_RES3D=<percent>: the 3D of a game frame is drawn into a smaller
 * render target (percent of the drawable, each axis) and scaled up to the
 * screen when the HUD scene starts (tbslotBegin slot 31, crashlib.c); the
 * HUD, menus and fades are drawn at the native resolution on top. A frame is
 * a game frame when the one before it had a HUD scene, so title screens and
 * menus outside the game stay native. Unset: 85 %, only while the GPU sets
 * the frame rate (rt_res3d_ctrl); N: always N %; 0: off. */
static int g_res3d_pct;         /* game thread: requested percent (0 = off) */
static int g_hud_prev;          /* game thread: the last frame had a HUD scene */
static int g_frame_lowres;      /* game thread: this frame's 3D goes to the small target */
static int g_frame_composited;  /* game thread: the composite of this frame was emitted */

void port_gx_begin_frame(void) {
    RCmd *c;
    port_gx_flush();
    GP_FRAME_BEGIN();
    g_frame++;
    if (g_trace > 0 && port_frame_count() >= g_trace_from) port_log("gx: frame %d", port_frame_count());
    g_frame_lowres = g_res3d_pct > 0 && g_hud_prev && (!g_res3d_dyn || g_res3d_low);
    g_frame_composited = 0;
    g_hud_prev = 0;
    rq_ensure(1, 0, 0);
    c = rq_cmd();
    c->type = CMD_CLEAR;
    c->u.clear.col = gx.copy_clear;
    c->u.clear.lowres = g_frame_lowres;
    rq_commit();
}

/* game thread: the HUD scene starts (crashlib.c tbslotBegin, slot 31) */
static void rec_hud_begin(void); /* gx_rec.inc */
static void hud_begin_impl(void);
void port_gx_hud_begin(void) {
    GP_HUD();
    if (g_rec_active) { /* in order with the recorded draws (GX thread) */
        rec_hud_begin();
        return;
    }
    hud_begin_impl();
}
static void hud_begin_impl(void) {
    RCmd *c;
    g_hud_prev = 1;
    if (!g_frame_lowres || g_frame_composited) return;
    port_gx_flush();
    rq_ensure(1, 0, 0);
    c = rq_cmd();
    c->type = CMD_COMPOSITE;
    rq_commit();
    g_frame_composited = 1;
}

void port_gx_init(void) {
    g_gxo_on = getenv("CWOC_GXOUT") != NULL;
    memset(&gx, 0, sizeof(gx));
    port_prof_calibrate();
    rq_init();
    g_mipmaps = !(getenv("CWOC_MIPMAPS") && getenv("CWOC_MIPMAPS")[0] == '0');
    g_blend10 = getenv("CWOC_BLEND10") && getenv("CWOC_BLEND10")[0] == '1';
#if defined(PORT_VITA) && defined(__ARM_NEON)
    {
        /* the NEON matrix products must match the game's scalar ones bit for bit */
        extern int port_mtx_selftest(void), port_gs_mtx_selftest(void);
        port_log("gx: NEON matrix check: %d / %d mismatches (NuMtxMul(H) / world matrix)", port_mtx_selftest(),
                 port_gs_mtx_selftest());
    }
#endif
    g_gpuexp = getenv("CWOC_GPUEXP") ? getenv("CWOC_GPUEXP")[0] : 0;
    port_fps_overlay_on(); /* load the setting before the HUD reads it */
    g_fps_raw = getenv("CWOC_FPS") && getenv("CWOC_FPS")[0] == '2';
    {
        const char *e = getenv("CWOC_RES3D");
        g_res3d_dyn = !e || !e[0];
        g_res3d_pct = g_res3d_dyn ? 85 : atoi(e);
        if (g_res3d_pct < 25 || g_res3d_pct >= 100) g_res3d_pct = 0;
        g_res3d_pct_rt = g_res3d_pct;
        if (!g_res3d_pct) port_log("gx: 3D resolution native (CWOC_RES3D)");
        else port_log("gx: 3D resolution %d%% %s", g_res3d_pct, g_res3d_dyn ? "while the GPU sets the frame rate" : "always (CWOC_RES3D)");
    }
    if (g_gpuexp) port_log("gx: GPU experiment '%c' (measurement only)", g_gpuexp);
    g_trace = getenv("CWOC_GXTRACE") ? atoi(getenv("CWOC_GXTRACE")) : 0;
    g_trace_from = getenv("CWOC_GXTRACE_FROM") ? atoi(getenv("CWOC_GXTRACE_FROM")) : 0;
    GXClearVtxDesc();
    gx.vp[2] = 640; gx.vp[3] = 448; gx.vp[5] = 1;
    gx.scissor[2] = 640; gx.scissor[3] = 448;
    gx.z_enable = 1; gx.z_func = 3; gx.z_update = 1;
    gx.alpha_comp0 = gx.alpha_comp1 = 7; gx.alpha_op = 0;
    gx.num_chans = 1;
    gx.num_tev = 1;
    GXSetTevOp(0, 4);
    gx.tev[0].texmap = 0xFF;
    gx.tev[0].channel = 4;
    PSMTXIdentity_port();
    rec_init();
}

/* identity matrices for all slots */
void PSMTXIdentity_port(void) {
    int i;
    for (i = 0; i < MAX_POSMTX; i++) {
        memset(gx.posmtx[i], 0, sizeof(gx.posmtx[i]));
        gx.posmtx[i][0][0] = gx.posmtx[i][1][1] = gx.posmtx[i][2][2] = 1.0f;
        memset(gx.nrmmtx[i], 0, sizeof(gx.nrmmtx[i]));
        gx.nrmmtx[i][0][0] = gx.nrmmtx[i][1][1] = gx.nrmmtx[i][2][2] = 1.0f;
    }
}

#include "gx_rec.inc"
