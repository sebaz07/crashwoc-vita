/* Byte-order helpers for GameCube (big-endian) file data on little-endian
 * hosts. Used from the decompiled loaders under #ifdef PORT. */
#include "port.h"
#include <string.h>

void port_swap32(void *p, int count) {
    uint32_t *w = p;
    int i;
    for (i = 0; i < count; i++) {
        uint32_t v;
        __builtin_memcpy(&v, &w[i], 4);
        v = __builtin_bswap32(v);
        __builtin_memcpy(&w[i], &v, 4);
    }
}

void port_swap16(void *p, int count) {
    uint16_t *h = p;
    int i;
    for (i = 0; i < count; i++) {
        uint16_t v;
        __builtin_memcpy(&v, &h[i], 2);
        v = (uint16_t)((v >> 8) | (v << 8));
        __builtin_memcpy(&h[i], &v, 2);
    }
}

/* Swap according to a layout string: each digit is a field width in bytes
 * ('4' = 32-bit, '2' = 16-bit, '1' = byte). "N:" before a width repeats it,
 * e.g. "16:4" = sixteen 32-bit fields. The layout is applied 'count' times. */
void port_swap_layout(void *p, const char *layout, int count) {
    unsigned char *b = p;
    int n;
    for (n = 0; n < count; n++) {
        const char *l = layout;
        while (*l) {
            int rep = 1, w;
            const char *q = l;
            while (*q >= '0' && *q <= '9') q++;
            if (*q == ':' && q > l) {
                rep = 0;
                while (l < q) rep = rep * 10 + (*l++ - '0');
                l++; /* ':' */
            }
            w = *l++ - '0';
            while (rep--) {
                if (w == 4) port_swap32(b, 1);
                else if (w == 2) port_swap16(b, 1);
                b += w;
            }
        }
    }
}

/* Big-endian GCC packs bitfields from the most significant bit of the storage
 * word, little-endian GCC from the least significant bit; declaration order is
 * the same. 'be' is the (already byte-swapped) value of the storage word as
 * the GameCube saw it; 'widths' are the field widths in declaration order. */
unsigned int port_bitfields_be_to_le(unsigned int be, const unsigned char *widths, int n, int unit_bits) {
    uint32_t le = 0;
    int i, off = 0;
    for (i = 0; i < n; i++) {
        int w = widths[i];
        uint32_t mask = w >= 32 ? 0xFFFFFFFFu : ((1u << w) - 1u);
        uint32_t v = (be >> (unit_bits - off - w)) & mask;
        le |= v << off;
        off += w;
    }
    return le;
}

/* Bitfield structs are declared in reverse under PORT (see nu3dxtypes.h), so a
 * storage word holds the same value as on GameCube: a plain byte swap suffices.
 * (Translated PowerPC code extracts the bits from the word value.) */
static void fix_bitword(unsigned char *p, const unsigned char *widths, int n) {
    (void)widths; (void)n;
    port_swap32(p, 1);
}

/* nuvtx_tc1_s: pnt[3], nrm[3], diffuse, tc[2] -- all 32-bit */
void port_fix_nuvtx_tc1(void *v, int n) { port_swap32(v, n * 9); }

/* nuinstance_s (0x50): mtx[16], objid, flags{visible,onscreen,visitest,isanimated},
 * anim (serialized marker), room_group (s16), special_flag, pad */
void port_fix_nuinstances(void *p, int n) {
    static const unsigned char flags[] = { 1, 1, 1, 1 };
    unsigned char *b = p;
    int i;
    for (i = 0; i < n; i++, b += 0x50) {
        port_swap32(b, 17);
        fix_bitword(b + 0x44, flags, 4);
        port_swap32(b + 0x48, 1);
        port_swap16(b + 0x4c, 1);
    }
}

/* nuinstanim_s (0x60): mtx[16], tfactor, tfirst, tinterval, ltime,
 * bits{playing,backwards,waiting,repeating,oscillate}, ipad[2], anim_ix, pad[3] */
void port_fix_nuinstanims(void *p, int n) {
    static const unsigned char flags[] = { 1, 1, 1, 1, 1 };
    unsigned char *b = p;
    int i;
    for (i = 0; i < n; i++, b += 0x60) {
        port_swap32(b, 20);
        fix_bitword(b + 0x50, flags, 5);
        port_swap32(b + 0x54, 2);
    }
}

/* nufspecial_s (0x50): mtx[16], instanceix, nameix, flags{ext,ext_vis,ext_onscreen}, pad */
void port_fix_nufspecials(void *p, int n) {
    static const unsigned char flags[] = { 1, 1, 1 };
    unsigned char *b = p;
    int i;
    for (i = 0; i < n; i++, b += 0x50) {
        port_swap32(b, 18);
        fix_bitword(b + 0x48, flags, 3);
        port_swap32(b + 0x4c, 1);
    }
}

/* numtl_s (0x54) as stored in .nus material blocks */
void port_fix_numtl(void *p) {
    /* numtlattrib_s in declaration order (see src/nu3dx headers) */
    static const unsigned char attrib[] = { 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 3, 8, 2, 1 };
    unsigned char *b = p;
    unsigned char nib;
    port_swap32(b, 1);                 /* next (1-based index) */
    fix_bitword(b + 0x04, attrib, 14);  /* attrib */
    port_swap32(b + 0x08, 13);         /* ambient, diffuse, fx1..4, power, alpha, tid */
    port_swap16(b + 0x3c, 1);          /* alpha_sort */
    port_swap16(b + 0x40, 1);          /* K */
    (void)nib;                         /* uanmmode:4, vanmmode:4: declared reversed, byte unchanged */
    port_swap32(b + 0x44, 4);          /* du dv su sv */
}

/* nutexanim_s (0x20): 3 words, numtids s16, dynalloc s16 bitfield, 4 words */
void port_fix_nutexanims(void *p, int n) {
    unsigned char *b = p;
    int i;
    for (i = 0; i < n; i++, b += 0x20) {
        port_swap32(b, 3);
        port_swap16(b + 0x0c, 2);
        port_swap32(b + 0x10, 4);
    }
}

/* Visited-set for in-place swapping of pointer graphs (each node once). */
#define VISIT_SIZE 65536
static const void *g_visit[VISIT_SIZE];
static int g_visit_count;

void port_visit_reset(void) {
    memset(g_visit, 0, sizeof(g_visit));
    g_visit_count = 0;
}

/* Returns 1 the first time an address is seen since the last reset. */
int port_visit_once(const void *p) {
    uint32_t h = (uint32_t)(uintptr_t)p;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h &= VISIT_SIZE - 1;
    while (g_visit[h]) {
        if (g_visit[h] == p) return 0;
        h = (h + 1) & (VISIT_SIZE - 1);
    }
    if (g_visit_count >= VISIT_SIZE - 1) return 1;
    g_visit[h] = p;
    g_visit_count++;
    return 1;
}
