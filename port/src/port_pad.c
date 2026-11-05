/* Dolphin PAD API on top of SDL2 game controllers / keyboard.
 * Replaces system/gc/pad.c (which talks to the serial interface). */
#include "port.h"
#ifdef PORT_VITA
#include <psp2/ctrl.h>
#else
#include <SDL2/SDL.h>
#endif
#include <string.h>
#include <stdlib.h>

/* Layout matches Dolphin PADStatus (12 bytes). */
typedef struct {
    unsigned short button;
    signed char stickX, stickY, substickX, substickY;
    unsigned char triggerL, triggerR, analogA, analogB;
    signed char err;
} PortPADStatus;

#define PAD_BUTTON_LEFT  0x0001
#define PAD_BUTTON_RIGHT 0x0002
#define PAD_BUTTON_DOWN  0x0004
#define PAD_BUTTON_UP    0x0008
#define PAD_TRIGGER_Z    0x0010
#define PAD_TRIGGER_R    0x0020
#define PAD_TRIGGER_L    0x0040
#define PAD_BUTTON_A     0x0100
#define PAD_BUTTON_B     0x0200
#define PAD_BUTTON_X     0x0400
#define PAD_BUTTON_Y     0x0800
#define PAD_BUTTON_START 0x1000

#ifdef PORT_VITA
/* PS Vita: built-in controls. Cross=A, Circle=B, Square=X, Triangle=Y,
 * L/R = triggers, SELECT = Z, left stick = main stick, right stick = C stick. */
int port_frame_count(void);
static void script_apply_fwd(PortPADStatus *s);

int PADInit(void) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    port_log("pad: PS Vita controls");
    return 1;
}

static signed char vaxis(unsigned char v, int invert) {
    int x = (int)v - 128;
    if (x > -12 && x < 12) x = 0; /* dead zone */
    if (invert) x = -x;
    if (x > 127) x = 127;
    if (x < -128) x = -128;
    return (signed char)x;
}

unsigned int PADRead(PortPADStatus *status) {
    SceCtrlData c;
    PortPADStatus *s = &status[0];
    int i;
    for (i = 0; i < 4; i++) { memset(&status[i], 0, sizeof(*status)); status[i].err = -1; }
    s->err = 0;
    sceCtrlPeekBufferPositive(0, &c, 1);
    { /* SELECT + D-pad up: next display mode (port_video.c); hidden from the game */
        static int combo_held;
        extern void port_display_cycle(void);
        int combo = (c.buttons & SCE_CTRL_SELECT) && (c.buttons & SCE_CTRL_UP);
        if (combo && !combo_held) port_display_cycle();
        combo_held = combo;
        if (combo) c.buttons &= ~(SCE_CTRL_SELECT | SCE_CTRL_UP);
    }
    { /* SELECT + D-pad down: frame lock on/off (port_os.c) */
        static int lock_held;
        extern void port_pace_lock_toggle(void);
        int combo = (c.buttons & SCE_CTRL_SELECT) && (c.buttons & SCE_CTRL_DOWN);
        if (combo && !lock_held) port_pace_lock_toggle();
        lock_held = combo;
        if (combo) c.buttons &= ~(SCE_CTRL_SELECT | SCE_CTRL_DOWN);
    }
    if (c.buttons & SCE_CTRL_CROSS) s->button |= PAD_BUTTON_A;
    /* the game maps GC A/X/B/Y to the PS2 cross/circle/square/triangle bits
     * (XInputGetState + the padmap code), so B is the spin: put it on Square */
    if (c.buttons & SCE_CTRL_SQUARE) s->button |= PAD_BUTTON_B;
    if (c.buttons & SCE_CTRL_CIRCLE) s->button |= PAD_BUTTON_X;
    if (c.buttons & SCE_CTRL_TRIANGLE) s->button |= PAD_BUTTON_Y;
    if (c.buttons & SCE_CTRL_START) s->button |= PAD_BUTTON_START;
    if (c.buttons & SCE_CTRL_SELECT) s->button |= PAD_TRIGGER_Z;
    if (c.buttons & SCE_CTRL_LTRIGGER) { s->button |= PAD_TRIGGER_L; s->triggerL = 255; }
    if (c.buttons & SCE_CTRL_RTRIGGER) { s->button |= PAD_TRIGGER_R; s->triggerR = 255; }
    if (c.buttons & SCE_CTRL_UP) s->button |= PAD_BUTTON_UP;
    if (c.buttons & SCE_CTRL_DOWN) s->button |= PAD_BUTTON_DOWN;
    if (c.buttons & SCE_CTRL_LEFT) s->button |= PAD_BUTTON_LEFT;
    if (c.buttons & SCE_CTRL_RIGHT) s->button |= PAD_BUTTON_RIGHT;
    s->stickX = vaxis(c.lx, 0);
    s->stickY = vaxis(c.ly, 1);
    s->substickX = vaxis(c.rx, 0);
    s->substickY = vaxis(c.ry, 1);
    script_apply_fwd(s);
    return 0x80000000u;
}
#else
static SDL_GameController *g_ctrl;

int PADInit(void) {
    int i;
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    for (i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) { g_ctrl = SDL_GameControllerOpen(i); break; }
    }
    port_log("pad: %s", g_ctrl ? SDL_GameControllerName(g_ctrl) : "keyboard only");
    return 1;
}

static signed char axis8(Sint16 v) {
    int x = v / 256;
    if (x > 127) x = 127;
    if (x < -128) x = -128;
    return (signed char)x;
}

#endif /* !PORT_VITA */

/* Scripted input for headless testing: CWOC_INPUT="START@100-104,UP@200-400,A@300"
 * (button names: A B X Y START L R Z UP DOWN LEFT RIGHT = d-pad, SUP SDOWN SLEFT
 * SRIGHT = main stick; frame ranges are inclusive). */
int port_frame_count(void);
struct scripted { unsigned short btn; signed char sx, sy; int a, b; };
static struct scripted g_script[64];
static int g_nscript = -1;

static void script_parse(void) {
    const char *e = getenv("CWOC_INPUT");
    static const struct { const char *n; unsigned short b; signed char x, y; } names[] = {
        {"A", PAD_BUTTON_A}, {"B", PAD_BUTTON_B}, {"X", PAD_BUTTON_X}, {"Y", PAD_BUTTON_Y},
        {"START", PAD_BUTTON_START}, {"L", PAD_TRIGGER_L}, {"R", PAD_TRIGGER_R}, {"Z", PAD_TRIGGER_Z},
        {"UP", PAD_BUTTON_UP}, {"DOWN", PAD_BUTTON_DOWN}, {"LEFT", PAD_BUTTON_LEFT}, {"RIGHT", PAD_BUTTON_RIGHT},
        {"SUP", 0, 0, 127}, {"SDOWN", 0, 0, -127}, {"SLEFT", 0, -127, 0}, {"SRIGHT", 0, 127, 0},
    };
    char buf[1024], *tok, *save;
    g_nscript = 0;
    if (!e) return;
    strncpy(buf, e, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_r(buf, ",", &save); tok && g_nscript < 64; tok = strtok_r(NULL, ",", &save)) {
        char *at = strchr(tok, '@');
        unsigned i;
        struct scripted *sc = &g_script[g_nscript];
        if (!at) continue;
        *at = 0;
        memset(sc, 0, sizeof(*sc));
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (!strcmp(tok, names[i].n)) { sc->btn = names[i].b; sc->sx = names[i].x; sc->sy = names[i].y; }
        sc->a = atoi(at + 1);
        sc->b = strchr(at + 1, '-') ? atoi(strchr(at + 1, '-') + 1) : sc->a;
        g_nscript++;
    }
}

static void script_apply(PortPADStatus *s) {
    int f = port_frame_count(), i;
    if (g_nscript < 0) script_parse();
    for (i = 0; i < g_nscript; i++) {
        if (f >= g_script[i].a && f <= g_script[i].b) {
            s->button |= g_script[i].btn;
            if (g_script[i].sx) s->stickX = g_script[i].sx;
            if (g_script[i].sy) s->stickY = g_script[i].sy;
        }
    }
}

#ifdef PORT_VITA
static void script_apply_fwd(PortPADStatus *s) { script_apply(s); }
#else
/* Fills the first status with the SDL controller/keyboard state. */
unsigned int PADRead(PortPADStatus *status) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    PortPADStatus *s = &status[0];
    int i;
    for (i = 0; i < 4; i++) { memset(&status[i], 0, sizeof(*status)); status[i].err = -1; }
    s->err = 0;
    if (k[SDL_SCANCODE_LEFT]) s->stickX = -127;
    if (k[SDL_SCANCODE_RIGHT]) s->stickX = 127;
    if (k[SDL_SCANCODE_UP]) s->stickY = 127;
    if (k[SDL_SCANCODE_DOWN]) s->stickY = -127;
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_SPACE]) s->button |= PAD_BUTTON_A;
    if (k[SDL_SCANCODE_Z]) s->button |= PAD_BUTTON_B;
    if (k[SDL_SCANCODE_C]) s->button |= PAD_BUTTON_X;
    if (k[SDL_SCANCODE_V]) s->button |= PAD_BUTTON_Y;
    if (k[SDL_SCANCODE_A]) s->button |= PAD_TRIGGER_L;
    if (k[SDL_SCANCODE_S]) s->button |= PAD_TRIGGER_R;
    if (k[SDL_SCANCODE_D]) s->button |= PAD_TRIGGER_Z;
    if (k[SDL_SCANCODE_RETURN]) s->button |= PAD_BUTTON_START;
    if (g_ctrl) {
        Sint16 lx = SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ly = SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_LEFTY);
        Sint16 rx = SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_RIGHTX);
        Sint16 ry = SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_RIGHTY);
        if (lx < -8000 || lx > 8000) s->stickX = axis8(lx);
        if (ly < -8000 || ly > 8000) s->stickY = (signed char)-axis8(ly);
        s->substickX = axis8(rx);
        s->substickY = (signed char)-axis8(ry);
        if (SDL_GameControllerGetButton(g_ctrl, SDL_CONTROLLER_BUTTON_A)) s->button |= PAD_BUTTON_A;
        if (SDL_GameControllerGetButton(g_ctrl, SDL_CONTROLLER_BUTTON_B)) s->button |= PAD_BUTTON_B;
        if (SDL_GameControllerGetButton(g_ctrl, SDL_CONTROLLER_BUTTON_X)) s->button |= PAD_BUTTON_X;
        if (SDL_GameControllerGetButton(g_ctrl, SDL_CONTROLLER_BUTTON_Y)) s->button |= PAD_BUTTON_Y;
        if (SDL_GameControllerGetButton(g_ctrl, SDL_CONTROLLER_BUTTON_START)) s->button |= PAD_BUTTON_START;
        if (SDL_GameControllerGetButton(g_ctrl, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) s->button |= PAD_TRIGGER_L;
        if (SDL_GameControllerGetButton(g_ctrl, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) s->button |= PAD_TRIGGER_R;
        if (SDL_GameControllerGetButton(g_ctrl, SDL_CONTROLLER_BUTTON_BACK)) s->button |= PAD_TRIGGER_Z;
        if (SDL_GameControllerGetButton(g_ctrl, SDL_CONTROLLER_BUTTON_DPAD_UP)) s->button |= PAD_BUTTON_UP;
        if (SDL_GameControllerGetButton(g_ctrl, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) s->button |= PAD_BUTTON_DOWN;
        if (SDL_GameControllerGetButton(g_ctrl, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) s->button |= PAD_BUTTON_LEFT;
        if (SDL_GameControllerGetButton(g_ctrl, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) s->button |= PAD_BUTTON_RIGHT;
        s->triggerL = (unsigned char)(SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
        s->triggerR = (unsigned char)(SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
    }
    script_apply(s);
    return 0x80000000u;
}
#endif

void PADControlMotor(int chan, unsigned int cmd) {
#ifdef PORT_VITA
    (void)chan; (void)cmd;
#else
    if (chan == 0 && g_ctrl) SDL_GameControllerRumble(g_ctrl, cmd == 1 ? 0xC000 : 0, cmd == 1 ? 0xC000 : 0, 100);
#endif
}
void PADControlAllMotors(const unsigned int *cmds) { PADControlMotor(0, cmds[0]); }
int PADReset(unsigned int mask) { (void)mask; return 1; }
int PADRecalibrate(unsigned int mask) { (void)mask; return 1; }
void PADClamp(PortPADStatus *status) { (void)status; }
void PADSetSpec(unsigned int spec) { (void)spec; }
void PADSetAnalogMode(unsigned int mode) { (void)mode; }
int PADSync(void) { return 1; }
