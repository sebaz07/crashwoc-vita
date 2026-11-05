/* Audio: the GameCube sound SDK layers the game sits on, emulated natively.
 *
 *  - ARAM (AR/ARQ) is a host buffer; the ARAM manager (AM, ssam.s has no
 *    decompiled C) loads the .spt sound tables into RAM (byte-swapped) and
 *    pushes the .spd sample data into ARAM.
 *  - AX voices: the game's sssfx.s (translated by ppc2c) and the decompiled
 *    SP/MIX libraries (sssp.c, ssmix.c) fill in real AXVPB parameter blocks;
 *    a software DSP here decodes DSP-ADPCM (and PCM) from ARAM with the
 *    block's addresses, loop info, sample-rate ratio and volumes.
 *  - DTK disc streams (music and voice .adp files) are decoded from the
 *    files (Nintendo "ADP" 4-bit stereo, 48 kHz).
 * The mix goes to SDL audio at 48 kHz. The AX frame callback that the game
 * registers runs on the game thread once per video frame (translated code
 * must not run on the audio thread). */
#include "port.h"
#include "sssp.h"
#include <SDL2/SDL.h>
#ifdef PORT_VITA
#include <psp2/kernel/threadmgr.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE 48000
#define AX_RATE 32000
#define AX_VOICES 64
#define ARAM_SIZE (16u << 20)

FILE *port_fopen(const char *name, const char *mode);

static SDL_mutex *g_lock;
static SDL_AudioDeviceID g_dev;
static int g_pull; /* no device: the game thread pulls the mix (tests, CWOC_AUDIO_DUMP) */
static FILE *g_dump;
static long g_dump_frames;

static void lock(void) { if (g_lock) SDL_LockMutex(g_lock); }
static void unlock(void) { if (g_lock) SDL_UnlockMutex(g_lock); }

/* ================================================================== ARAM */
static u8 *g_aram;
static u32 g_ar_next;

u32 ARInit(u32 *stack, u32 num) {
    (void)stack;
    (void)num;
    if (!g_aram) g_aram = calloc(1, ARAM_SIZE);
    g_ar_next = 0x4000; /* the SDK reserves the first 16 KB */
    return g_ar_next;
}
void ARQInit(void) {}
void AIInit(u8 *stack) { (void)stack; }
u32 ARAlloc(u32 length) {
    u32 a = g_ar_next;
    g_ar_next += (length + 31) & ~31u;
    if (g_ar_next > ARAM_SIZE) port_log("audio: ARAM exhausted (%u)", g_ar_next);
    return a;
}

/* ================================================================== AM */
#define AM_ZERO_SIZE 0x400
static u32 g_am_base, g_am_size, g_am_sp;
static u32 g_am_stack[32];
static int g_am_depth;

void AMInit(u32 aramBase, u32 size) {
    g_am_base = aramBase;
    g_am_size = size;
    g_am_depth = 0;
    if (g_aram) memset(g_aram + aramBase, 0, AM_ZERO_SIZE); /* zero buffer: silent ADPCM */
    g_am_sp = aramBase + AM_ZERO_SIZE;
}

u32 AMGetZeroBuffer(void) { return g_am_base; }

static long read_file(const char *name, void **out) {
    FILE *f = port_fopen(name, "rb");
    long n;
    void *b;
    if (!f) {
        port_log("audio: missing %s", name);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(n > 0 ? (size_t)n : 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) port_log("audio: short read %s", name);
    fclose(f);
    *out = b;
    return n;
}

static u32 rd32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* .spt: u32 count, count x SPSoundEntry (7 words), then SPAdpcmEntry (u16s) */
static void swap_spt(u8 *b, long n) {
    u32 count, i;
    u8 *p;
    if (n < 4) return;
    count = rd32(b);
    for (p = b; p + 4 <= b + 4 + (long)count * 28 && p + 4 <= b + n; p += 4) {
        u8 t = p[0];
        p[0] = p[3];
        p[3] = t;
        t = p[1];
        p[1] = p[2];
        p[2] = t;
    }
    for (i = 0; p + 2 <= b + n; p += 2, i++) {
        u8 t = p[0];
        p[0] = p[1];
        p[1] = t;
    }
}

static int is_spt(const char *name) {
    size_t l = strlen(name);
    return l > 4 && (name[l - 1] == 't' || name[l - 1] == 'T') && (name[l - 2] == 'p' || name[l - 2] == 'P');
}

void *AMLoadFile(char *name, u32 *length) {
    void *b = NULL;
    long n = read_file(name, &b);
    if (n < 0) return NULL;
    if (is_spt(name)) swap_spt(b, n);
    if (length) *length = (u32)n;
    return b;
}

void *AMLoadFilePtr(char *name, u32 *length, void *ptr) {
    void *b = NULL;
    long n = read_file(name, &b);
    if (n < 0) return ptr;
    if (is_spt(name)) swap_spt(b, n);
    memcpy(ptr, b, (size_t)n);
    free(b);
    if (length) *length = (u32)n;
    return ptr;
}

u32 __AMPushBuffered(char *name, void *buffer, u32 buffer_size, void *callback, int async) {
    void *b = NULL;
    long n = read_file(name, &b);
    u32 addr = g_am_sp;
    (void)buffer;
    (void)buffer_size;
    (void)callback;
    (void)async;
    if (n < 0 || !g_aram || addr + (u32)n > ARAM_SIZE) {
        /* keep the push/pop stack balanced (SS_LoadBank pops before every load) */
        if (n >= 0) port_log("audio: %s does not fit in ARAM", name);
        free(b);
        if (g_am_depth < 32) g_am_stack[g_am_depth++] = addr;
        return addr;
    }
    lock();
    memcpy(g_aram + addr, b, (size_t)n);
    unlock();
    free(b);
    if (g_am_depth < 32) g_am_stack[g_am_depth++] = addr;
    g_am_sp = (addr + (u32)n + 31) & ~31u;
    port_log("audio: %s -> ARAM 0x%x (%ld bytes)", name, addr, n);
    return addr;
}

u32 AMPushData(void *buffer, u32 size) {
    u32 addr = g_am_sp;
    if (!g_aram || addr + size > ARAM_SIZE) return 0;
    memcpy(g_aram + addr, buffer, size);
    if (g_am_depth < 32) g_am_stack[g_am_depth++] = addr;
    g_am_sp = (addr + size + 31) & ~31u;
    return addr;
}

void AMPop(void) {
    if (g_am_depth > 0) g_am_sp = g_am_stack[--g_am_depth];
}

/* ================================================================== AX */
typedef struct {
    int on;        /* decoding (state was set to run with fresh addresses) */
    u32 pos;       /* nibble (ADPCM), sample (PCM16) or byte (PCM8) address */
    u32 frac;      /* 16.16 resampler phase */
    s32 h1, h2;    /* ADPCM history */
    u16 ps;        /* current frame predictor/scale */
    s32 prev, cur; /* last two decoded samples (linear interpolation) */
} VoiceDSP;

static AXVPB g_vpb[AX_VOICES];
static int g_used[AX_VOICES];
static VoiceDSP g_dsp[AX_VOICES];
static void (*g_ax_cb)(void);
static int g_alog = -1; /* CWOC_AUDIO_LOG=1: log voice starts */

void AXInit(void) {
    int i;
    memset(g_vpb, 0, sizeof(g_vpb));
    memset(g_used, 0, sizeof(g_used));
    memset(g_dsp, 0, sizeof(g_dsp));
    for (i = 0; i < AX_VOICES; i++) g_vpb[i].index = (u32)i;
}

void *AXRegisterCallback(void (*cb)(void)) {
    void *old = (void *)g_ax_cb;
    g_ax_cb = cb;
    return old;
}

AXVPB *AXAcquireVoice(u32 priority, void (*callback)(void *), u32 userContext) {
    int i;
    lock();
    for (i = 0; i < AX_VOICES; i++) {
        if (!g_used[i]) {
            AXVPB *p = &g_vpb[i];
            memset(p, 0, sizeof(*p));
            p->index = (u32)i;
            p->priority = priority;
            p->callback = callback;
            p->userContext = userContext;
            g_used[i] = 1;
            g_dsp[i].on = 0;
            unlock();
            return p;
        }
    }
    unlock();
    return NULL;
}

void AXFreeVoice(AXVPB *p) {
    int i;
    if (!p) return;
    i = (int)p->index;
    if (i < 0 || i >= AX_VOICES) return;
    lock();
    p->pb.state = 0;
    g_used[i] = 0;
    g_dsp[i].on = 0;
    unlock();
}

static u32 addr_of(u16 hi, u16 lo) { return (u32)hi << 16 | lo; }

void AXSetVoiceState(AXVPB *p, u16 state) {
    int i;
    if (!p) return;
    i = (int)p->index;
    if (i < 0 || i >= AX_VOICES) return;
    if (g_alog < 0) g_alog = getenv("CWOC_AUDIO_LOG") != NULL;
    lock();
    if (state && !g_dsp[i].on) {
        VoiceDSP *v = &g_dsp[i];
        v->on = 1;
        v->pos = addr_of(p->pb.addr.currentAddressHi, p->pb.addr.currentAddressLo);
        v->frac = 0;
        v->h1 = (s16)p->pb.adpcm.yn1;
        v->h2 = (s16)p->pb.adpcm.yn2;
        v->ps = p->pb.adpcm.pred_scale;
        v->prev = v->cur = 0;
        if (g_alog)
            port_log("ax: voice %d start fmt %d addr %x..%x loop %d ratio %x vol %x/%x/%x", i, p->pb.addr.format, v->pos,
                     addr_of(p->pb.addr.endAddressHi, p->pb.addr.endAddressLo), p->pb.addr.loopFlag,
                     (u32)p->pb.src.ratioHi << 16 | p->pb.src.ratioLo, p->pb.ve.currentVolume, p->pb.mix.vL, p->pb.mix.vR);
    }
    p->pb.state = state;
    unlock();
}

static s32 clamp16(s32 v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

/* next source sample of a voice; returns 0 when the voice has ended */
static int voice_next(AXVPB *p, VoiceDSP *v, s32 *out) {
    u16 fmt = p->pb.addr.format;
    u32 end = addr_of(p->pb.addr.endAddressHi, p->pb.addr.endAddressLo);
    s32 s;
    if (fmt == 0) { /* DSP ADPCM */
        const u16 *c = p->pb.adpcm.a[0];
        u8 byte;
        s32 nib, scale, c1, c2;
        if ((v->pos & 15) == 0) {
            v->ps = g_aram[(v->pos >> 1) & (ARAM_SIZE - 1)];
            v->pos += 2;
        }
        byte = g_aram[(v->pos >> 1) & (ARAM_SIZE - 1)];
        nib = (v->pos & 1) ? (byte & 15) : (byte >> 4);
        if (nib >= 8) nib -= 16;
        scale = 1 << (v->ps & 15);
        c1 = (s16)c[(v->ps >> 4 & 7) * 2];
        c2 = (s16)c[(v->ps >> 4 & 7) * 2 + 1];
        s = clamp16((((nib * scale) << 11) + 1024 + c1 * v->h1 + c2 * v->h2) >> 11);
        v->h2 = v->h1;
        v->h1 = s;
    } else if (fmt == 0x0A) { /* PCM16 */
        const u8 *b = g_aram + ((v->pos * 2) & (ARAM_SIZE - 2));
        s = (s16)(b[0] << 8 | b[1]);
    } else if (fmt == 0x19) { /* PCM8 */
        s = (s8)g_aram[v->pos & (ARAM_SIZE - 1)] << 8;
    } else {
        return 0;
    }
    *out = s;
    if (v->pos >= end) {
        if (p->pb.addr.loopFlag) {
            v->pos = addr_of(p->pb.addr.loopAddressHi, p->pb.addr.loopAddressLo);
            if (fmt == 0) {
                v->ps = p->pb.adpcmLoop.loop_pred_scale;
                v->h1 = (s16)p->pb.adpcmLoop.loop_yn1;
                v->h2 = (s16)p->pb.adpcmLoop.loop_yn2;
            }
        } else {
            return 0; /* last sample delivered: stop after it */
        }
    } else {
        v->pos++;
    }
    return 1;
}

/* ================================================================== DTK */
extern int SS_TrackDone;

static struct {
    FILE *f;
    void *track;
    int queued, state, repeat;
    int vol_l, vol_r;
    s32 hl1, hl2, hr1, hr2;
    s16 pcm[28 * 2];
    int npcm, ipcm;
} g_dtk;

static s16 adp_sample(s32 bits, s32 q, s32 *h1, s32 *h2) {
    s32 hist = 0, cur;
    switch (q >> 4) {
    case 1: hist = *h1 * 0x3c; break;
    case 2: hist = *h1 * 0x73 - *h2 * 0x34; break;
    case 3: hist = *h1 * 0x62 - *h2 * 0x37; break;
    }
    hist = (hist + 0x20) >> 6;
    if (hist < -0x200000) hist = -0x200000;
    if (hist > 0x1fffff) hist = 0x1fffff;
    cur = ((s32)(s16)(bits << 12) >> (q & 15) << 6) + hist;
    *h2 = *h1;
    *h1 = cur;
    return (s16)clamp16(cur >> 6);
}

static void dtk_rewind(void) {
    if (g_dtk.f) fseek(g_dtk.f, 0, SEEK_SET);
    g_dtk.hl1 = g_dtk.hl2 = g_dtk.hr1 = g_dtk.hr2 = 0;
    g_dtk.npcm = g_dtk.ipcm = 0;
}

/* next stereo frame of the stream (audio thread, lock held) */
static int dtk_next(s32 *l, s32 *r) {
    if (g_dtk.ipcm >= g_dtk.npcm) {
        u8 blk[32];
        int i;
        if (!g_dtk.f || fread(blk, 1, 32, g_dtk.f) != 32) {
            SS_TrackDone = 1;
            if (g_dtk.repeat && g_dtk.f) {
                dtk_rewind();
                if (fread(blk, 1, 32, g_dtk.f) != 32) return 0;
            } else {
                g_dtk.state = 0;
                dtk_rewind();
                return 0;
            }
        }
        for (i = 0; i < 28; i++) {
            g_dtk.pcm[i * 2] = adp_sample(blk[4 + i] & 15, blk[0], &g_dtk.hl1, &g_dtk.hl2);
            g_dtk.pcm[i * 2 + 1] = adp_sample(blk[4 + i] >> 4, blk[1], &g_dtk.hr1, &g_dtk.hr2);
        }
        g_dtk.npcm = 28;
        g_dtk.ipcm = 0;
    }
    *l = g_dtk.pcm[g_dtk.ipcm * 2];
    *r = g_dtk.pcm[g_dtk.ipcm * 2 + 1];
    g_dtk.ipcm++;
    return 1;
}

void DTKInit(void) { memset(&g_dtk, 0, sizeof(g_dtk)); g_dtk.vol_l = g_dtk.vol_r = 255; }

u32 DTKQueueTrack(char *fileName, void *track, u32 eventMask, void (*callback)(u32)) {
    FILE *f;
    (void)eventMask;
    (void)callback;
    f = port_fopen(fileName, "rb");
    if (!f) {
        port_log("dtk: cannot open %s", fileName ? fileName : "?");
        /* behave like a track that ended at once, so code waiting for the end
         * (the intro's LOGOS -> next track chain) does not stall forever */
        SS_TrackDone = 1;
        return 1;
    }
    lock();
    if (g_dtk.f) fclose(g_dtk.f);
    g_dtk.f = f;
    g_dtk.track = track;
    g_dtk.queued = 1;
    g_dtk.state = 0;
    dtk_rewind();
    unlock();
    port_log("dtk: queued %s", fileName);
    return 0;
}

void DTKFlushTracks(void (*callback)(void)) {
    lock();
    if (g_dtk.f) fclose(g_dtk.f);
    g_dtk.f = NULL;
    g_dtk.queued = 0;
    g_dtk.track = NULL;
    g_dtk.state = 0;
    unlock();
    if (callback) callback();
}

void DTKSetRepeatMode(u32 mode) { g_dtk.repeat = mode != 0; }

void DTKSetState(u32 state) {
    lock();
    if (state == 1) SS_TrackDone = 0;
    if (state == 0) dtk_rewind();
    g_dtk.state = (int)state;
    unlock();
}

u32 DTKGetState(void) { return (u32)g_dtk.state; }
void *DTKGetCurrentTrack(void) { return g_dtk.queued ? g_dtk.track : NULL; }
void DTKSetVolume(u8 left, u8 right) { g_dtk.vol_l = left; g_dtk.vol_r = right; }

/* ================================================================== mixer */
static void mix(s16 *out, int frames) {
    static s32 acc[2 * 4096];
    int i, n;
    u32 step_base = (u32)(((u64)AX_RATE << 16) / OUT_RATE);
    while (frames > 0) {
        n = frames > 4096 ? 4096 : frames;
        memset(acc, 0, sizeof(s32) * 2 * n);
        lock();
        if (g_aram) {
            int vi;
            for (vi = 0; vi < AX_VOICES; vi++) {
                AXVPB *p = &g_vpb[vi];
                VoiceDSP *v = &g_dsp[vi];
                float gl, gr;
                u32 step;
                if (!g_used[vi] || !v->on || p->pb.state != 1) continue;
                gl = (float)p->pb.ve.currentVolume * (float)p->pb.mix.vL * (1.0f / (32768.0f * 32768.0f));
                gr = (float)p->pb.ve.currentVolume * (float)p->pb.mix.vR * (1.0f / (32768.0f * 32768.0f));
                step = (u32)(((u64)((u32)p->pb.src.ratioHi << 16 | p->pb.src.ratioLo) * step_base) >> 16);
                if (step == 0) step = step_base;
                for (i = 0; i < n; i++) {
                    s32 smp;
                    v->frac += step;
                    while (v->frac >= 0x10000) {
                        v->frac -= 0x10000;
                        v->prev = v->cur;
                        if (!voice_next(p, v, &v->cur)) {
                            p->pb.state = 0; /* the game's AX callback frees the voice */
                            v->on = 0;
                            break;
                        }
                    }
                    if (!v->on) break;
                    smp = v->prev + (s32)(((s64)(v->cur - v->prev) * (s32)v->frac) >> 16);
                    acc[i * 2] += (s32)(smp * gl);
                    acc[i * 2 + 1] += (s32)(smp * gr);
                }
            }
        }
        if (g_dtk.state == 1 && g_dtk.f) {
            /* streams are 48 kHz like the output */
            for (i = 0; i < n; i++) {
                s32 l, r;
                if (!dtk_next(&l, &r)) break;
                acc[i * 2] += l * g_dtk.vol_l / 255;
                acc[i * 2 + 1] += r * g_dtk.vol_r / 255;
            }
        }
        unlock();
        for (i = 0; i < n * 2; i++) out[i] = (s16)clamp16(acc[i]);
        out += n * 2;
        frames -= n;
    }
}

static void SDLCALL audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
#ifdef PORT_VITA
    { /* SDL's audio thread inherits the game thread's core 0: move it to core 2 */
        static int pinned;
        if (!pinned) {
            pinned = 1;
            sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
        }
    }
#endif
    mix((s16 *)stream, len / 4);
}

static void dump_header(void) {
    u8 h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                OUT_RATE & 255, OUT_RATE >> 8 & 255, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    u32 bps = OUT_RATE * 4, data = (u32)g_dump_frames * 4, riff = data + 36;
    memcpy(h + 28, &bps, 4);
    memcpy(h + 40, &data, 4);
    memcpy(h + 4, &riff, 4);
    fseek(g_dump, 0, SEEK_SET);
    fwrite(h, 1, 44, g_dump);
    fseek(g_dump, 0, SEEK_END);
}

void port_audio_init(void) {
    SDL_AudioSpec want, have;
    const char *dump = getenv("CWOC_AUDIO_DUMP");
    if (!g_lock) g_lock = SDL_CreateMutex();
    if (dump) {
        g_dump = fopen(dump, "wb");
        if (g_dump) dump_header();
    }
    if (getenv("CWOC_NOAUDIO") || g_dump) {
        g_pull = 1;
        port_log("audio: %s", g_dump ? "dumping the mix to a WAV file" : "disabled (CWOC_NOAUDIO)");
        return;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        port_log("audio: SDL audio unavailable (%s)", SDL_GetError());
        g_pull = 1;
        return;
    }
    memset(&want, 0, sizeof(want));
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_cb;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) {
        port_log("audio: no output device (%s)", SDL_GetError());
        g_pull = 1;
        return;
    }
    port_log("audio: %d Hz stereo, %d-frame buffer", have.freq, have.samples);
    SDL_PauseAudioDevice(g_dev, 0);
}

/* game thread: the AX frame callback (ss.c calls it before SS_UpdateSFX) */
void port_audio_ax_callback(void) {
    if (g_ax_cb) g_ax_cb();
}

/* game thread, once per presented frame */
void port_audio_frame(void) {
    if (g_ax_cb) g_ax_cb();
    if (g_pull) {
        static s16 buf[OUT_RATE / 60 * 2];
        mix(buf, OUT_RATE / 60);
        if (g_dump) {
            fwrite(buf, 4, OUT_RATE / 60, g_dump);
            g_dump_frames += OUT_RATE / 60;
            if ((g_dump_frames / (OUT_RATE / 60)) % 60 == 0) dump_header();
        }
    }
}
