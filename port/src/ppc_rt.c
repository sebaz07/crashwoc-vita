/* Runtime for functions translated from the original PowerPC code (ppc2c.py). */
#include <stdlib.h>
#include <string.h>
#include "port.h"
#include "ppc_rt.h"

ppc_ctx_t ppc;

#define PPC_STACK_SIZE (2u * 1024u * 1024u)
static unsigned char ppc_stack[PPC_STACK_SIZE] __attribute__((aligned(64)));
u32 g_ppc_sp = 0;

static u32 frame_cur;
static struct { int off; u32 v; } outargs[16];
static int n_outargs;

void ppc_fatal(const char *msg) {
    port_fatal("ppc runtime: %s", msg);
    abort();
}

static void ppc_init_stack(void) {
    if (!g_ppc_sp)
        g_ppc_sp = (u32)(uintptr_t)(ppc_stack + PPC_STACK_SIZE - 64);
}

void ppc_enter_native_frame(void) {
    ppc_init_stack();
    frame_cur = (g_ppc_sp - 64) & ~15u;
    n_outargs = 0;
}

u32 ppc_alloc_stack(u32 size) {
    frame_cur = (frame_cur - size) & ~15u;
    return frame_cur;
}

u32 ppc_copy_to_stack(const void *p, u32 size) {
    u32 a = ppc_alloc_stack(size);
    memcpy((void *)(uintptr_t)a, p, size);
    return a;
}

void ppc_outarg(int off, u32 v) {
    if (n_outargs < 16) {
        outargs[n_outargs].off = off;
        outargs[n_outargs].v = v;
        n_outargs++;
    }
}

void ppc_finish_native_frame(void) {
    int i;
    u32 sp = (frame_cur - 128) & ~15u;
    if (sp < (u32)(uintptr_t)ppc_stack + 4096)
        ppc_fatal("emulated stack overflow");
    for (i = 0; i < n_outargs; i++)
        ST32(sp + outargs[i].off, outargs[i].v);
    ST32(sp, 0); /* back chain */
    ppc.r[1] = sp;
    g_ppc_sp = sp;
}

/* ---- indirect calls: native function address -> ctx shim */
#define HBITS 12
static struct { void *k; void (*fn)(void); } htab[1 << HBITS];
static int hinit;

static unsigned hslot(void *k) { return ((uintptr_t)k * 2654435761u) >> (32 - HBITS); }

static void hbuild(void) {
    const struct ppc_shim *s;
    for (s = ppc_shims; s->native; s++) {
        unsigned h = hslot(s->native);
        while (htab[h].k && htab[h].k != s->native) h = (h + 1) & ((1 << HBITS) - 1);
        htab[h].k = s->native;
        htab[h].fn = s->fn;
    }
    hinit = 1;
}

void ppc_call_indirect(u32 target) {
    unsigned h;
    void *k = (void *)(uintptr_t)target;
    if (!hinit) hbuild();
    if (!k) ppc_fatal("indirect call to NULL");
    h = hslot(k);
    while (htab[h].k) {
        if (htab[h].k == k) {
            htab[h].fn();
            return;
        }
        h = (h + 1) & ((1 << HBITS) - 1);
    }
    port_log("ppc runtime: indirect call to unknown native %p", k);
    ppc_fatal("indirect call target has no shim");
}

/* translated code called a function that only exists as SDK/hardware asm */
void ppc_missing(const char *name) {
    port_stub_hit(name);
}

/* ---- PPC SysV varargs (sprintf & co. called from translated code) */
struct ppc_va { int gpr, fpr; u32 stk; };

static u32 va_int(struct ppc_va *v) {
    u32 x;
    if (v->gpr <= 10) return ppc.r[v->gpr++];
    x = LD32(v->stk);
    v->stk += 4;
    return x;
}

static unsigned long long va_ll(struct ppc_va *v) {
    u32 hi, lo;
    if (v->gpr <= 9) {
        if (v->gpr % 2 == 0) v->gpr++;
        if (v->gpr <= 9) {
            hi = ppc.r[v->gpr]; lo = ppc.r[v->gpr + 1]; v->gpr += 2;
            return ((unsigned long long)hi << 32) | lo;
        }
    }
    v->gpr = 11;
    v->stk = (v->stk + 7) & ~7u;
    hi = LD32(v->stk); lo = LD32(v->stk + 4); v->stk += 8;
    return ((unsigned long long)hi << 32) | lo;
}

static double va_dbl(struct ppc_va *v) {
    double d;
    if (v->fpr <= 8) return ppc.f[v->fpr++];
    v->stk = (v->stk + 7) & ~7u;
    d = LDD_STK(v->stk);
    v->stk += 8;
    return d;
}

static int ppc_vformat(char *out, size_t cap, const char *fmt, struct ppc_va *v) {
    size_t n = 0;
    char spec[32], tmp[512];
    while (*fmt) {
        const char *st;
        int k, lng = 0;
        if (*fmt != '%') {
            if (n + 1 < cap) out[n] = *fmt;
            n++; fmt++;
            continue;
        }
        st = fmt++;
        if (*fmt == '%') { if (n + 1 < cap) out[n] = '%'; n++; fmt++; continue; }
        k = 0;
        spec[k++] = '%';
        while (*fmt && strchr("-+ #0", *fmt) && k < 20) spec[k++] = *fmt++;
        if (*fmt == '*') { k += snprintf(spec + k, sizeof(spec) - k, "%d", (s32)va_int(v)); fmt++; }
        while (*fmt >= '0' && *fmt <= '9' && k < 24) spec[k++] = *fmt++;
        if (*fmt == '.') {
            spec[k++] = *fmt++;
            if (*fmt == '*') { k += snprintf(spec + k, sizeof(spec) - k, "%d", (s32)va_int(v)); fmt++; }
            while (*fmt >= '0' && *fmt <= '9' && k < 28) spec[k++] = *fmt++;
        }
        while (*fmt && strchr("hlLqjzt", *fmt)) { if (*fmt == 'l' || *fmt == 'q' || *fmt == 'L') lng++; fmt++; }
        if (!*fmt) break;
        {
            char c = *fmt++;
            int w;
            spec[k] = 0;
            if (strchr("diouxXc", c)) {
                if (lng >= 2) {
                    spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = c; spec[k] = 0;
                    w = snprintf(tmp, sizeof(tmp), spec, va_ll(v));
                } else {
                    spec[k++] = c; spec[k] = 0;
                    w = snprintf(tmp, sizeof(tmp), spec, (s32)va_int(v));
                }
            } else if (strchr("feEgGaA", c)) {
                spec[k++] = c; spec[k] = 0;
                w = snprintf(tmp, sizeof(tmp), spec, va_dbl(v));
            } else if (c == 's') {
                u32 p = va_int(v);
                spec[k++] = 's'; spec[k] = 0;
                w = snprintf(tmp, sizeof(tmp), spec, p ? (const char *)(uintptr_t)p : "(null)");
            } else if (c == 'p') {
                w = snprintf(tmp, sizeof(tmp), "0x%08x", va_int(v));
            } else if (c == 'n') {
                u32 p = va_int(v);
                if (p) ST32(p, (u32)n);
                w = 0;
            } else {
                w = snprintf(tmp, sizeof(tmp), "%.*s", (int)(fmt - st), st);
            }
            if (w > (int)sizeof(tmp) - 1) w = sizeof(tmp) - 1;
            if (w > 0) {
                size_t i;
                for (i = 0; i < (size_t)w; i++) { if (n + 1 < cap) out[n] = tmp[i]; n++; }
            }
        }
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return (int)n;
}

/* sprintf(buf, fmt, ...) with arguments in the ppc context */
void ppc_va_sprintf(void) {
    struct ppc_va v = { 5, 1, ppc.r[1] + 8 };
    char *buf = (char *)(uintptr_t)ppc.r[3];
    ppc.r[3] = (u32)ppc_vformat(buf, 0x7FFFFFFF, (const char *)(uintptr_t)ppc.r[4], &v);
}

void ppc_va_printf(void) {
    char buf[1024];
    struct ppc_va v = { 4, 1, ppc.r[1] + 8 };
    ppc.r[3] = (u32)ppc_vformat(buf, sizeof(buf), (const char *)(uintptr_t)ppc.r[3], &v);
    port_log("game: %s", buf);
}

/* block trace for functions translated with PPC2C_TRACE=Func,... (debugging) */
int port_frame_count(void);
void ppc_trace(u32 addr) {
    static int on = -1, from, to;
    if (on < 0) {
        on = getenv("CWOC_PPCTRACE") != NULL;
        from = getenv("CWOC_PPCTRACE_FROM") ? atoi(getenv("CWOC_PPCTRACE_FROM")) : 0;
        to = getenv("CWOC_PPCTRACE_TO") ? atoi(getenv("CWOC_PPCTRACE_TO")) : 0x7FFFFFFF;
    }
    if (on && port_frame_count() >= from && port_frame_count() <= to) port_log("ppc: %08X", addr);
}
