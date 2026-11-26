/* GX thread: the GX emulation runs on its own core (included at the end of
 * gx_gl.c, which renames its public entry points gxi_<name>).
 *
 * The game thread runs the engine (logic, scene building and the material
 * pass, which calls GX for every draw). With CWOC_GXTHREAD on (default when
 * the render thread runs), the public GX functions only append a record to a
 * ring buffer; the "gxrec" thread on core 2 replays them with the original
 * code (state capture, batching, caches, the render queue for the render
 * thread). The engine keeps reading its own globals on its own thread: only
 * the port's GX state moves, and only the GX thread touches it.
 *
 * Recording is on only during the engine's static pass, NuMtlRender3d (most
 * of the GX work: the scene's meshes and skinned models; numtl.c calls
 * port_gx_rec_scope). Every other GX call (immediate blocks, 2D, faceon,
 * water, glass, shadows, frame start/end) runs directly on the game thread
 * as before, after the GX thread has caught up: scratch buffers those paths
 * reuse are never read late. Inside the static pass the engine draws its mesh
 * data (pointers kept: the render thread already reads them late) and the
 * per-model data that changes between models is copied into the record:
 * loaded matrices, texture and light objects, skin matrices, blend shape and
 * TSkin vertices. CWOC_GXTHREAD=1 turns it on (off by default, see rec_init). */

#define REC_BYTES (4u << 20)

typedef struct { u16 op, pad; u32 size; } RecHdr; /* size: payload bytes, 8-aligned */

static u8 *g_rec;
static u32 g_rec_wl;              /* producer: end of the records written (monotonic) */
static volatile u32 g_rec_w;      /* published end */
static volatile u32 g_rec_r;      /* consumer position */
static volatile int g_rec_sleep;  /* consumer waits on g_rec_go */
static volatile int g_rec_waiter; /* producer waits on g_rec_done */
static SDL_sem *g_rec_go, *g_rec_done;
static int g_rec_inblock;         /* producer: a GXBegin block is being written */
static u32 g_rec_block;           /* its record (offset) */
static u8 g_rec_scratch[64] __attribute__((aligned(8)));
uint64_t g_rec_busy_us;           /* GX thread busy time (perf lines) */
uint64_t g_rec_wait_us;           /* game thread waiting for it */
unsigned g_rec_syncs;

/* records are published in batches (rec_commit): the earlier "batching made
 * no difference" was measured in castle, which is GPU-bound */
unsigned g_rec_nrec, g_rec_wakes;
/* CWOC_RECHIST=1 (measurement only): records per op, and how many repeat the
 * previous record of the same op byte for byte (upper bound for dropping
 * redundant state on the recording side) */
static int g_rec_hist = -1;
static unsigned g_rec_opn[64], g_rec_opdup[64];
static u8 g_rec_oplast[64][96];
static u32 g_rec_oplen[64];
static void *g_rec_lasth;

static void rec_publish(void) {
    __atomic_store_n(&g_rec_w, g_rec_wl, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_rec_sleep, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&g_rec_sleep, 0, __ATOMIC_SEQ_CST);
        g_rec_wakes++;
        SDL_SemPost(g_rec_go);
    }
}

static void rec_block_abort(void);

/* room for need bytes (the consumer frees space as it advances). The
 * consumer position only moves forward: a stale copy is enough until it
 * says the ring is full (no barrier per record) */
static u32 g_rec_rc;
static void rec_wait_space(u32 need) {
    if (g_rec_wl + need - g_rec_rc <= REC_BYTES) return;
    while (g_rec_wl + need - (g_rec_rc = __atomic_load_n(&g_rec_r, __ATOMIC_ACQUIRE)) > REC_BYTES) {
        g_ws_game = "rec_space";
        rec_publish();
        sceKernelDelayThread(50);
    }
    g_ws_game = "-";
}

static void *rec_alloc(int op, u32 size) {
    u32 need = (u32)sizeof(RecHdr) + ((size + 7u) & ~7u), pos;
    RecHdr *h;
    if (g_rec_inblock) rec_block_abort();
    pos = g_rec_wl % REC_BYTES;
    if (pos + need > REC_BYTES) { /* never wrap inside a record: pad to the end */
        u32 pad = REC_BYTES - pos;
        rec_wait_space(pad + need);
        h = (RecHdr *)(g_rec + pos);
        h->op = 0; /* RECOP_PAD */
        h->size = pad - (u32)sizeof(RecHdr);
        g_rec_wl += pad;
        pos = 0;
    } else {
        rec_wait_space(need);
    }
    h = (RecHdr *)(g_rec + pos);
    h->op = (u16)op;
    h->size = need - (u32)sizeof(RecHdr);
    g_rec_wl += need;
    g_rec_lasth = h;
    return h + 1;
}

static inline void rec_commit(void) {
    g_rec_nrec++;
    if (g_rec_hist > 0 && g_rec_lasth) {
        RecHdr *h = (RecHdr *)g_rec_lasth;
        u32 op = h->op < 63 ? h->op : 63, n = h->size < 96 ? h->size : 96;
        g_rec_opn[op]++;
        if (g_rec_oplen[op] == n && !memcmp(g_rec_oplast[op], h + 1, n)) g_rec_opdup[op]++;
        memcpy(g_rec_oplast[op], h + 1, n);
        g_rec_oplen[op] = n;
    }
    {
        /* publish every 16 records: each publish costs three barriers (DMB)
         * on the game thread. The GX thread sees the tail at the latest at
         * rec_sync (end of the pass) or when the ring is full, which both
         * publish. Pass time -0.5..-1.0 ms (jungle_r 16.1 -> 15.6, earth_r
         * 16.4 -> 15.4). CWOC_RECPUB=N for other batch sizes. */
        static int every = -1, n;
        if (every < 0) every = getenv("CWOC_RECPUB") ? atoi(getenv("CWOC_RECPUB")) : 16;
        if (every > 1 && ++n < every) return;
        n = 0;
    }
    rec_publish();
}

/* producer: wait until the GX thread has replayed everything recorded */
static void rec_sync(void) {
    uint64_t t0;
    int spin;
    rec_publish();
    if (__atomic_load_n(&g_rec_r, __ATOMIC_ACQUIRE) == g_rec_wl) return;
    t0 = port_prof_now();
    g_ws_game = "rec_sync";
    for (spin = 0; spin < 4000; spin++)
        if (__atomic_load_n(&g_rec_r, __ATOMIC_ACQUIRE) == g_rec_wl) goto done;
    while (__atomic_load_n(&g_rec_r, __ATOMIC_ACQUIRE) != g_rec_wl) {
        __atomic_store_n(&g_rec_waiter, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g_rec_r, __ATOMIC_SEQ_CST) == g_rec_wl) break;
        SDL_SemPost(g_rec_go); /* make sure it runs its loop and sees the waiter */
        SDL_SemWaitTimeout(g_rec_done, 2);
    }
done:
    g_ws_game = "-";
    __atomic_store_n(&g_rec_waiter, 0, __ATOMIC_SEQ_CST);
    g_rec_wait_us += port_prof_now() - t0;
    g_rec_syncs++;
}

/* ---- game-side copy of the vertex format: the block size is needed to know
 * where a GXBegin block ends (the inline writers end it by byte count) ---- */
static int g_rsh_desc[GX_VA_MAX_ATTR];
static AttrFmt g_rsh_fmt[MAX_VTXFMT][GX_VA_MAX_ATTR];

static int rec_vtx_size(int fmt) {
    int a, off = 0;
    for (a = 0; a < GX_VA_MAX_ATTR; a++) {
        int desc = g_rsh_desc[a];
        if (desc == GX_NONE) continue;
        off += desc == GX_DIRECT ? direct_size(a, &g_rsh_fmt[fmt][a]) : desc == GX_INDEX8 ? 1 : desc == GX_INDEX16 ? 2 : 0;
    }
    return off;
}

/* ---- records written by hand ---- */
typedef struct { int prim, fmt, nverts, need; } RecBlock; /* + need bytes */
typedef struct {
    int strips, constcol, copied;
    const short *idx;
    u32 count, col;
    const void *verts;
    int vmin, nv; /* copied: vertex range, then the indices */
} RecDrawIndexed;
typedef struct { int n; const void *srcverts; const short *idx; const void *vertlist; } RecDrawTSkin; /* + n vertices */
typedef struct {
    int n, lit, bmin, bn;
    const short *idx;
    const void *srcverts, *blendsrc;
    float one;
    u32 constcol;
    float mtx[256];
} RecSkinDraw; /* + bn blend source vertices */
typedef struct { int frame; } RecPresent;
typedef struct { int maxq, nq; } RecQuads; /* + maxq * 4 PortQuadVtx (native particle quads) */

/* redundant state setters dropped while recording (gen_gx_rec.py DEDUP):
 * which shadows hold a recorded call, reset when a recorded pass starts */
static u32 g_rec_dd;
unsigned g_rec_ddn;
#define GX_REC_BODY
#include "gx_rec_gen.h"

void GXClearVtxDesc(void) {
    if (!g_rec_active) { gxi_GXClearVtxDesc(); return; }
    memset(g_rsh_desc, 0, sizeof(g_rsh_desc));
    g_rsh_desc[GX_VA_POS] = GX_DIRECT;
    rec_alloc(RECOP_GXClearVtxDesc, 0);
    rec_commit();
}

typedef struct { int attr, type; } Rec_GXSetVtxDesc;
void GXSetVtxDesc(int attr, int type) {
    Rec_GXSetVtxDesc *r;
    if (!g_rec_active) { gxi_GXSetVtxDesc(attr, type); return; }
    if (attr >= 0 && attr < GX_VA_MAX_ATTR) g_rsh_desc[attr] = type;
    r = rec_alloc(RECOP_GXSetVtxDesc, sizeof(*r));
    r->attr = attr;
    r->type = type;
    rec_commit();
}

typedef struct { int vtxfmt, attr, cnt, type; u8 frac; } Rec_GXSetVtxAttrFmt;
void GXSetVtxAttrFmt(int vtxfmt, int attr, int cnt, int type, u8 frac) {
    Rec_GXSetVtxAttrFmt *r;
    if (!g_rec_active) { gxi_GXSetVtxAttrFmt(vtxfmt, attr, cnt, type, frac); return; }
    if (vtxfmt >= 0 && vtxfmt < MAX_VTXFMT && attr >= 0 && attr < GX_VA_MAX_ATTR) {
        g_rsh_fmt[vtxfmt][attr].cnt = cnt;
        g_rsh_fmt[vtxfmt][attr].type = type;
        g_rsh_fmt[vtxfmt][attr].frac = frac;
    }
    r = rec_alloc(RECOP_GXSetVtxAttrFmt, sizeof(*r));
    r->vtxfmt = vtxfmt;
    r->attr = attr;
    r->cnt = cnt;
    r->type = type;
    r->frac = frac;
    rec_commit();
}

/* GXBegin: the vertices are written straight into the block's record */
void GXBegin(int prim, int vtxfmt, u16 nverts) {
    int vs, need;
    RecBlock *b;
    if (!g_rec_active) { gxi_GXBegin(prim, vtxfmt, nverts); return; }
    if (g_rec_inblock) rec_block_abort();
    vs = rec_vtx_size(vtxfmt & 7);
    need = vs * (int)nverts;
    if (nverts == 0 || vs == 0 || need > (int)(REC_BYTES / 4)) {
        port_gx_wp = port_gx_end = g_rec_scratch; /* nothing to draw: writes are dropped */
        return;
    }
    b = rec_alloc(RECOP_BLOCK, (u32)sizeof(RecBlock) + (u32)need);
    b->prim = prim;
    b->fmt = vtxfmt;
    b->nverts = nverts;
    b->need = need;
    g_rec_block = (u32)((u8 *)b - g_rec);
    g_rec_inblock = 1;
    port_gx_wp = (u8 *)(b + 1);
    port_gx_end = port_gx_wp + need;
}

/* all the vertices of the block arrived (or data outside a block) */
static void rec_fifo_end(void) {
    if (g_rec_inblock && port_gx_wp >= port_gx_end) {
        g_rec_inblock = 0;
        rec_commit();
    }
    if (!g_rec_inblock) port_gx_wp = port_gx_end = g_rec_scratch;
}

/* another GX call arrived before the block was complete: drop the block (the
 * original also drew nothing for it until its vertices arrived) */
static void rec_block_abort(void) {
    RecBlock *b = (RecBlock *)(g_rec + g_rec_block);
    static int warned;
    if (!warned) {
        warned = 1;
        port_log("gxrec: block of %d vertices incomplete (%d of %d bytes), dropped", b->nverts,
                 (int)(port_gx_wp - (u8 *)(b + 1)), b->need);
    }
    b->need = -1;
    g_rec_inblock = 0;
    port_gx_wp = port_gx_end = g_rec_scratch;
    rec_commit();
}

static void rec_replay_block(const RecBlock *b) {
    if (b->need < 0) return;
    g_prim = b->prim;
    g_vtxfmt = b->fmt & 7;
    g_nverts = b->nverts;
    if (g_plan_fmt != g_vtxfmt) {
        build_plan();
        g_plan_fmt = g_vtxfmt;
    }
    g_need = g_vs * b->nverts;
    if (g_need != b->need || g_need > (int)sizeof(g_fifo) - 64) {
        static int warned;
        if (!warned) {
            warned = 1;
            port_log("gxrec: block size mismatch (%d recorded, %d replayed)", b->need, g_need);
        }
        return;
    }
    memcpy(g_fifo, b + 1, (size_t)g_need);
    g_fifo_len = g_need;
    g_in_begin = 0;
    draw_block();
    g_fifo_len = 0;
}

void port_gs_draw_indexed(int strips, const short *idx, u32 count, const void *verts, int constcol, u32 col) {
    RecDrawIndexed *r;
    if (!g_rec_active) { gxi_port_gs_draw_indexed(strips, idx, count, verts, constcol, col); return; }
    r = rec_alloc(RECOP_DRAW_INDEXED, sizeof(*r));
    r->copied = 0;
    r->strips = strips;
    r->idx = idx;
    r->count = count;
    r->verts = verts;
    r->constcol = constcol;
    r->col = col;
    rec_commit();
}

/* native particle quads (renderpsdma): the engine writes the finished quads
 * straight into the record; the GX thread copies them into its batch with
 * the direct path (quads_begin_impl / quads_end_impl) */
static RecQuads *g_rec_q;
/* end of the ring after the last quads record; nothing recorded since means
 * no GX state or matrix changed since those quads (a setter dropped as
 * redundant changes nothing): the engine may skip setting the material again,
 * as on the direct path (port_gx_quads_state_kept). Reset when the recording
 * opens or closes (direct GX calls in between are not seen here). */
static u32 g_rec_quads_wl = 0xFFFFFFFFu;
static int rec_quads_kept(void) {
    return g_rec_quads_wl == g_rec_wl;
}
static PortQuadVtx *rec_quads_begin(int maxq) {
    RecQuads *r = rec_alloc(RECOP_QUADS, (u32)sizeof(RecQuads) + (u32)maxq * 4u * (u32)sizeof(PortQuadVtx));
    r->maxq = maxq;
    r->nq = 0;
    g_rec_q = r;
    return (PortQuadVtx *)(r + 1);
}
static void rec_quads_end(int nq) {
    g_rec_q->nq = nq < g_rec_q->maxq ? nq : g_rec_q->maxq;
    g_rec_q = NULL;
    rec_commit();
    g_rec_quads_wl = g_rec_wl;
}
static void rec_replay_quads(const RecQuads *r) {
    PortQuadVtx *v;
    if (r->nq <= 0) return;
    v = quads_begin_impl(r->nq);
    if (!v) return;
    memcpy(v, r + 1, (size_t)r->nq * 4 * sizeof(PortQuadVtx));
    quads_end_impl(r->nq);
}
static void rec_hud_begin(void) {
    rec_alloc(RECOP_HUD_BEGIN, 0);
    rec_commit();
}

static void rec_replay_draw_indexed(const RecDrawIndexed *r) {
    if (r->copied) {
        const PortGSVertex *v = (const PortGSVertex *)(r + 1);
        const short *ix = (const short *)((const u8 *)(r + 1) + (size_t)r->nv * sizeof(PortGSVertex));
        g_rec_bypass = 1;
        gxi_port_gs_draw_indexed(r->strips, ix, r->count, v - r->vmin, r->constcol, r->col);
        g_rec_bypass = 0;
    } else {
        gxi_port_gs_draw_indexed(r->strips, r->idx, r->count, r->verts, r->constcol, r->col);
    }
}

void port_gs_draw_tskin(const void *vertlist, int n, const void *srcverts, const short *idx) {
    RecDrawTSkin *r;
    if (!g_rec_active) { gxi_port_gs_draw_tskin(vertlist, n, srcverts, idx); return; }
    if (n <= 0) return;
    r = rec_alloc(RECOP_DRAW_TSKIN, (u32)sizeof(*r) + (u32)n * sizeof(PortGSVertexNorm));
    r->n = n;
    r->srcverts = srcverts;
    r->idx = idx;
    memcpy(r + 1, vertlist, (size_t)n * sizeof(PortGSVertexNorm)); /* the engine's TSkin scratch */
    rec_commit();
}

void port_gs_skin_draw(int n, const short *idx, const void *srcverts, const void *blendsrc, const float *skinmtx,
                       float one, int lit, u32 constcol) {
    RecSkinDraw *r;
    int bmin = 0, bn = 0;
    if (!g_rec_active) { gxi_port_gs_skin_draw(n, idx, srcverts, blendsrc, skinmtx, one, lit, constcol); return; }
    if (blendsrc != NULL && n > 0) { /* blend shape output: a scratch buffer */
        int i, mn = 0x7FFF, mx = -1;
        for (i = 0; i < n; i++) {
            int s = (short)idx[i];
            if (s < mn) mn = s;
            if (s > mx) mx = s;
        }
        if (mx >= mn) { bmin = mn; bn = mx - mn + 1; }
    }
    r = rec_alloc(RECOP_SKIN_DRAW, (u32)sizeof(*r) + (u32)bn * sizeof(PortVec3));
    r->n = n;
    r->idx = idx;
    r->srcverts = srcverts;
    r->blendsrc = blendsrc;
    r->one = one;
    r->lit = lit;
    r->constcol = constcol;
    r->bmin = bmin;
    r->bn = bn;
    memcpy(r->mtx, skinmtx, sizeof(r->mtx)); /* CV_SKINMTX: rewritten for every model */
    if (bn) memcpy(r + 1, (const PortVec3 *)blendsrc + bmin, (size_t)bn * sizeof(PortVec3));
    rec_commit();
}

static void rec_replay_skin_draw(const RecSkinDraw *r) {
    const void *bs = r->blendsrc ? (const void *)((const PortVec3 *)(r + 1) - r->bmin) : NULL;
    gxi_port_gs_skin_draw(r->n, r->idx, r->srcverts, bs, r->mtx, r->one, r->lit, r->constcol);
}

/* recording on/off around the static pass (numtl.c NuMtlRender3d). Off:
 * wait for the GX thread to replay everything, then the game thread uses
 * the GX state directly again (the GX thread is idle until the next pass). */
/* scopes nest: the static pass (NuMtlRender3d) inside the whole material
 * pass (port_gx_rec_all); recording ends when the outermost one closes, or
 * at a forced close (rec_close: present, loads, scene start) */
static int g_rec_depth;
void port_gx_rec_scope(int on) {
    if (!g_rec_on) return;
    if (on) {
        if (g_rec_active) {
            g_rec_depth++;
            return;
        }
        if (g_in_begin) return; /* a direct block is incomplete: stay direct */
        memcpy(g_rsh_desc, gx.vtxdesc, sizeof(g_rsh_desc));
        memcpy(g_rsh_fmt, gx.fmt, sizeof(g_rsh_fmt));
        port_gx_wp = port_gx_end = g_rec_scratch;
        g_rec_dd = 0; /* the state was set directly since the last pass */
        g_rec_quads_wl = 0xFFFFFFFFu;
        g_rec_depth = 1;
        g_rec_active = 1;
    } else if (g_rec_active) {
        if (--g_rec_depth > 0) return;
        if (g_rec_inblock) rec_block_abort();
        g_rec_depth = 0;
        g_rec_active = 0;
        g_rec_quads_wl = 0xFFFFFFFFu;
        rec_sync();
        port_gx_wp = port_gx_end = g_fifo;
    }
}
/* the frame's tail (port_gx_rec_tail): recording stays open from just before
 * the world scene's flush through the scenes after it (particles, glass,
 * clouds, debris, panel) and the present, and the game thread goes on into
 * the next frame's logic while the GX thread finishes this one. The first
 * scene of the next frame waits for it (a frame of lag at most); that wait
 * counts as a render wait for the frame pacer. */
static int g_rec_tail, g_rec_tail_presented;
static void rec_close(void) {
    uint64_t t0 = 0;
    if (g_rec_tail) t0 = port_prof_now();
    if (g_rec_active) {
        g_rec_depth = 1;
        port_gx_rec_scope(0);
    }
    if (g_rec_tail) {
        g_port_wait_us += port_prof_now() - t0;
        g_rec_tail = g_rec_tail_presented = 0;
    }
}

/* main.c, before the world scene's NuRndrEndScene. CWOC_RECTAIL=0: the
 * recording closes at the end of each material pass (as before). */
void port_gx_rec_tail(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("CWOC_RECTAIL") && getenv("CWOC_RECTAIL")[0] == '0');
    if (!on || !g_rec_on || g_rec_tail) return;
    port_gx_rec_scope(1); /* this level of the scope is closed by rec_close */
    if (g_rec_active) {
        g_rec_tail = 1;
        g_rec_tail_presented = 0;
    }
}

/* the whole material pass (NuMtlRender: static, dynamic, sorted, face-on,
 * water, stencil) recorded for the GX thread, not only the static pass: the
 * game thread only walks the lists while the GX thread converts, and waits
 * for it once at the end. Every path those passes reach goes through recorded
 * GX calls (gx_rec_gen.h, the special records above) or keeps its data
 * until the frame's present (dynamic geometry is reset only by the next
 * scene). CWOC_RECALL=0: only the static pass (as before). */
void port_gx_rec_all(int on) {
    static int all = -1;
    if (all < 0) all = !(getenv("CWOC_RECALL") && getenv("CWOC_RECALL")[0] == '0');
    if (all) port_gx_rec_scope(on);
}

void port_gx_present(int frame) {
    if (g_rec_tail && g_rec_active && !g_rec_tail_presented) {
        RecPresent *r = rec_alloc(RECOP_PRESENT, sizeof(*r));
        r->frame = frame;
        rec_commit();
        rec_publish();
        g_rec_tail_presented = 1;
        return;
    }
    rec_close();
    gxi_port_gx_present(frame);
}

void port_gx_idle_for_load(void) {
    rec_close();
    gxi_port_gx_idle_for_load();
}

void port_gx_finish(void) {
    rec_close();
    gxi_port_gx_finish();
}

/* NuRndrBeginScene (nurndr.c): nothing is recorded outside the static pass */
void port_gx_scene_sync(void) {
    if (g_rec_tail && !g_rec_tail_presented) return; /* a later scene of the same frame */
    rec_close();
}

static void rec_replay(int op, void *p) {
    switch (op) {
    case RECOP_BLOCK: rec_replay_block(p); break;
    case RECOP_DRAW_INDEXED: rec_replay_draw_indexed(p); break;
    case RECOP_DRAW_TSKIN: {
        RecDrawTSkin *r = p;
        gxi_port_gs_draw_tskin(r + 1, r->n, r->srcverts, r->idx);
        break;
    }
    case RECOP_SKIN_DRAW: rec_replay_skin_draw(p); break;
    case RECOP_PRESENT: gxi_port_gx_present(((RecPresent *)p)->frame); break;
    case RECOP_QUADS: rec_replay_quads(p); break;
    case RECOP_HUD_BEGIN: hud_begin_impl(); break;
    case RECOP_IDLE_FOR_LOAD: gxi_port_gx_idle_for_load(); break;
    case RECOP_FINISH: gxi_port_gx_finish(); break;
    case RECOP_SYNC: break;
    case RECOP_GXClearVtxDesc: gxi_GXClearVtxDesc(); break;
    case RECOP_GXSetVtxDesc: {
        Rec_GXSetVtxDesc *r = p;
        gxi_GXSetVtxDesc(r->attr, r->type);
        break;
    }
    case RECOP_GXSetVtxAttrFmt: {
        Rec_GXSetVtxAttrFmt *r = p;
        gxi_GXSetVtxAttrFmt(r->vtxfmt, r->attr, r->cnt, r->type, r->frac);
        break;
    }
    default: rec_replay_generated(op, p); break;
    }
}

/* GX thread: prefetch for the indexed draws a few records ahead of r (gx_gl.c
 * idxc_prefetch): stage 2 for the next one, stage 1 for the one after it.
 * CWOC_RECPF=0: off (A/B). */
static int g_recpf = -1;
static void rec_prefetch(u32 r, u32 w) {
    int found = 0, k;
    for (k = 0; k < 12 && r != w && found < 2; k++) {
        const RecHdr *h = (const RecHdr *)(g_rec + r % REC_BYTES);
        if (h->op == RECOP_DRAW_INDEXED) {
            const RecDrawIndexed *d = (const RecDrawIndexed *)(h + 1);
            if (!d->copied) idxc_prefetch(d->idx, d->count, d->verts, found ? 1 : 2);
            found++;
        }
        r += (u32)sizeof(RecHdr) + h->size;
    }
}

static int rec_thread(void *arg) {
    (void)arg;
    { extern void port_thread_register(const char *name); port_thread_register("gxrec"); }
#ifdef PORT_VITA
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
#endif
    g_rec_tid = SDL_ThreadID(); /* records the render queue during the static pass */
    /* CWOC_GXPRIO=1: above the transform worker that shares core 2 */
    if (getenv("CWOC_GXPRIO") && getenv("CWOC_GXPRIO")[0] == '1') SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    for (;;) {
        u32 w = __atomic_load_n(&g_rec_w, __ATOMIC_ACQUIRE), r = g_rec_r;
        if (r == w) {
            if (__atomic_load_n(&g_rec_waiter, __ATOMIC_SEQ_CST)) {
                __atomic_store_n(&g_rec_waiter, 0, __ATOMIC_SEQ_CST);
                SDL_SemPost(g_rec_done);
            }
            __atomic_store_n(&g_rec_sleep, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&g_rec_w, __ATOMIC_SEQ_CST) == r && !__atomic_load_n(&g_rec_waiter, __ATOMIC_SEQ_CST))
                SDL_SemWait(g_rec_go);
            __atomic_store_n(&g_rec_sleep, 0, __ATOMIC_SEQ_CST);
            continue;
        }
        {
            uint64_t t0 = port_prof_now();
            if (g_recpf < 0) g_recpf = !(getenv("CWOC_RECPF") && getenv("CWOC_RECPF")[0] == '0');
            while (r != w) {
                RecHdr *h = (RecHdr *)(g_rec + r % REC_BYTES);
                if (g_recpf && h->op == RECOP_DRAW_INDEXED) rec_prefetch(r + (u32)sizeof(RecHdr) + h->size, w);
                if (h->op != 0) rec_replay(h->op, h + 1);
                r += (u32)sizeof(RecHdr) + h->size;
                __atomic_store_n(&g_rec_r, r, __ATOMIC_RELEASE);
            }
            g_rec_busy_us += port_prof_now() - t0;
        }
    }
    return 0;
}

/* stall watchdog: when no frame was presented for 30 s (loads take up to
 * ~15 s), log where each thread waits and which progress counters still move
 * (twice, 1 s apart), once per stall. Costs a thread that sleeps. */
static int watchdog_thread(void *arg) {
    int last = -1, still = 0, logged = 0, k;
    (void)arg;
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH); /* runs even if a thread spins */
    for (;;) {
        int f = port_frame_count();
        SDL_Delay(1000);
        if (f != last) {
            last = f;
            still = 0;
            logged = 0;
            continue;
        }
        if (++still < 30 || logged) continue;
        logged = 1;
        for (k = 0; k < 2; k++) {
            port_log("watchdog: no frame for %d s at frame %d | waits game %s rt %s gx %s (sleep %d waiter %d rec %d) | "
                     "rec wl %u w %u r %u | rq cf %d pf %d ci %d pub %d/%d ncmd %d | frames sub %u done %u | wk busy %d jobs %u | bn %d",
                     still + k, f, (const char *)g_ws_game, (const char *)g_ws_rt, (const char *)g_ws_gx, g_rec_sleep,
                     g_rec_waiter, g_rec_active, g_rec_wl, g_rec_w, g_rec_r, g_cf, g_pf, g_ci, g_pub[0], g_pub[1], g_ncmd,
                     (unsigned)g_frames_sub, (unsigned)g_frames_done, g_wk_busy, g_wk_jobs, g_bn);
            SDL_Delay(1000);
        }
    }
    return 0;
}

static void rec_init(void) {
    /* on by default since the static geometry (GPU memory) emptied core 2:
     * castle 34.4 -> 35.6 fps, hub 34.2 -> 35.7 (1 run each). It used to
     * lose (33.0 -> 29.9-31.8 in castle) while the transform worker filled
     * core 2. CWOC_GXTHREAD=0 keeps the GX work on the game thread. */
    const char *e = getenv("CWOC_GXTHREAD");
    if (!SDL_CreateThreadWithStackSize(watchdog_thread, "watchdog", 64 * 1024, NULL))
        port_log("gx: watchdog thread failed (%s)", SDL_GetError());
    if (e && e[0] == '0') return;
    if (!g_rt_threaded) return;
#ifdef PORT_GPUPROF
    /* the pass labels of a capture are read where the commands are recorded */
    if (getenv("CWOC_GPUPROF")) {
        port_log("gx: GX thread off for the GPU profiler capture");
        return;
    }
#endif
    g_rec = malloc(REC_BYTES);
    g_rec_go = SDL_CreateSemaphore(0);
    g_rec_done = SDL_CreateSemaphore(0);
    if (!g_rec || !g_rec_go || !g_rec_done) {
        port_log("gx: GX thread allocation failed, staying on the game thread");
        return;
    }
    memcpy(g_rsh_desc, gx.vtxdesc, sizeof(g_rsh_desc));
    memcpy(g_rsh_fmt, gx.fmt, sizeof(g_rsh_fmt));
    if (!SDL_CreateThreadWithStackSize(rec_thread, "gxrec", 1024 * 1024, NULL)) {
        port_log("gx: GX thread failed (%s), staying on the game thread", SDL_GetError());
        return;
    }
    g_rec_on = 1;
    port_log("gx: GX thread on (core 2, %u KB record ring)", REC_BYTES >> 10);
}

/* GX thread figures for the perf report (per frame over nf frames) */
int port_gx_rec_report(char *buf, int size, int nf) {
    int n;
    if (!g_rec_on) return 0;
    n = snprintf(buf, (size_t)size, " gxrec busy %.1f wait %.1f syncs %u recs %u (dropped %u) wakes %u",
                 g_rec_busy_us / 1000.0 / nf, g_rec_wait_us / 1000.0 / nf, g_rec_syncs / (unsigned)nf,
                 g_rec_nrec / (unsigned)nf, g_rec_ddn / (unsigned)nf, g_rec_wakes / (unsigned)nf);
    g_rec_ddn = 0;
    g_rec_busy_us = g_rec_wait_us = 0;
    g_rec_syncs = g_rec_nrec = g_rec_wakes = 0;
    if (g_rec_hist < 0) g_rec_hist = getenv("CWOC_RECHIST") != NULL;
    if (g_rec_hist > 0) {
        char hb[1024];
        int i, k = 0;
        k += snprintf(hb + k, sizeof(hb) - k, "rechist per frame (op:n/dup):");
        for (i = 0; i < 64; i++)
            if (g_rec_opn[i] >= (unsigned)nf * 5 && k < (int)sizeof(hb) - 32)
                k += snprintf(hb + k, sizeof(hb) - k, " %d:%u/%u", i, g_rec_opn[i] / (unsigned)nf, g_rec_opdup[i] / (unsigned)nf);
        port_log("%s", hb);
        memset(g_rec_opn, 0, sizeof(g_rec_opn));
        memset(g_rec_opdup, 0, sizeof(g_rec_opdup));
    }
    return n < size ? n : size - 1;
}
