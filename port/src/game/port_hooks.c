/* Hooks called by translated code (tools/ppc2c.py HOOKS) where the port
 * deliberately deviates from the original flow. */
int port_skip_intro(void);
int port_skip_logos(void);
void firstscreens(void);
void PlayCutMovie(s32 movie);

int CopyFilesThreadProc(int a);

void port_hook_firstscreens(void) {
    if (!port_skip_logos())
        firstscreens();
    else
        CopyFilesThreadProc(0); /* firstscreens() also sets up the sfx tables */
}

/* the intro/title cut movie (.dat + gcutscene data) is not ported yet */
void port_hook_PlayCutMovie(s32 movie) {
    if (port_skip_intro())
        return;
    PlayCutMovie(movie);
}

/* Buggy race (level 3, a\west_a): the opponents' recorded runs
 * (WEST_A.AI0..AI3) are read raw from the disc, big-endian. The translated
 * LoadBuggyData only reads them; convert each 28-byte BUGSAVE record
 * (quaternion: 4 floats, x/y/z/FrontWheelAng: 4 shorts, WheelLength: 4 bytes)
 * to native order, or the other racers get garbage positions (invisible) and
 * the race position counter goes wrong. */
void *LoadBuggyData(void *data, int indx);
void *port_hook_LoadBuggyData(void *data, int indx) {
    unsigned char *p = data, *end = LoadBuggyData(data, indx);
    for (; p + 28 <= end; p += 28) {
        int i;
        for (i = 0; i < 16; i += 4) {
            unsigned char t = p[i]; p[i] = p[i + 3]; p[i + 3] = t;
            t = p[i + 1]; p[i + 1] = p[i + 2]; p[i + 2] = t;
        }
        for (i = 16; i < 24; i += 2) {
            unsigned char t = p[i]; p[i] = p[i + 1]; p[i + 1] = t;
        }
    }
    return end;
}

/* ---- Screen mode and frame rate in the pause menu's OPTIONS ----
 * They used to be SELECT + D-pad up/down, which clash with overlay plugins
 * (PSVshell). The options menu (menu 6: rumble, sound, [language], done) gets
 * two entries before DONE: GetMenuInfo (game.c) counts them, the translated
 * DrawMenu draws them through this DrawMenuEntry hook, and the ProcMenu hook
 * changes them (cross / D-pad right = next, D-pad left = previous): the
 * screen mode and the frame rate (30 fps or unlocked), saved in the save
 * directory (display.txt, framelock.txt) as before. A third
 * entry shows or hides the frames-per-second counter (fps.txt, gx_gl.c).
 * The game's first entry, rumble, is hidden on the handheld Vita, which has
 * no motor (a PS TV's DualShock has: port_pad.c): it is not drawn (the
 * entries below move up half a line, the block stays centred) and the
 * cursor skips it. */
#define PORT_MENU_OPTIONS 6
#define PORT_SFX_YES 0x36
extern char *tDONE[6];
extern char *tON[6];
extern char *tOFF[6];
extern float dme_sx;
extern s32 stick_bits;
extern s32 fadeval;
extern s32 Paused;
int port_display_mode(void);
void port_display_step(int dir);
int port_pace_lock_on(void);
void port_pace_lock_toggle(void);
int port_fps_overlay_on(void);
void port_fps_overlay_toggle(void);
#ifdef PORT_VITA
int port_vibration_supported(void); /* port_pad.c: a PS TV (DualShock), not the handheld */
#else
static int port_vibration_supported(void) { return 1; }
#endif
extern float MENUDY;
void DrawMenuEntry(struct cursor_s *cursor, char *txt, float *x, float *y, s32 *i);
void ProcMenu(struct cursor_s *cursor, struct nupad_s *pad);

/* languages of the game's text tables: EN FR DE IT ES NL (no accents: the
 * font encodes them its own way) */
static const char *const k_screen[6] = { "SCREEN", "ECRAN", "BILD", "SCHERMO", "PANTALLA", "SCHERM" };
static const char *const k_mode[3][6] = {
    { "4:3", "4:3", "4:3", "4:3", "4:3", "4:3" },
    { "STRETCHED", "ETIRE", "GESTRECKT", "ALLUNGATO", "ESTIRADA", "UITGEREKT" },
    { "WIDESCREEN", "LARGE", "BREITBILD", "PANORAMICO", "PANORAMICA", "BREEDBEELD" },
};
/* the frame rate: 30 fps (locked to every other vblank) or unlocked (port_os.c) */
/* (no longer than the game's own option lines: they sit in the menu's panel) */
static const char *const k_rate[6] = { "FRAME RATE", "FREQUENCE", "BILDRATE", "FRAME RATE", "FPS", "BEELDRATE" };
static const char *const k_30[6] = { "30 FPS", "30 FPS", "30 FPS", "30 FPS", "30", "30 FPS" };
static const char *const k_unlocked[6] = { "UNLOCKED", "LIBRE", "FREI", "LIBERO", "DESBLOQUEADOS", "VRIJ" };
static const char *const k_fps[6] = { "SHOW FPS", "AFFICHER FPS", "FPS ANZEIGEN", "MOSTRA FPS", "MOSTRAR FPS",
                                      "FPS TONEN" };

static int menu_lang(void) {
    return (Game.language >= 0 && Game.language < 6) ? Game.language : 0;
}

/* widescreen pause screen: the menus move out with the stretched frame (port_hud.c) */
float port_hud_enter(float x, int *st);
void port_hud_leave(int st);
void port_hud_text(const char *txt);
static void menu_entry(struct cursor_s *cursor, char *txt, float *x, float *y, s32 *i);

void port_hook_DrawMenuEntry(struct cursor_s *cursor, char *txt, float *x, float *y, s32 *i) {
    float x0 = *x;
    int st;
    *x = port_hud_enter(x0, &st);
    menu_entry(cursor, txt, x, y, i);
    port_hud_leave(st);
    *x = x0;
    port_hud_text(NULL);
}

static void menu_entry(struct cursor_s *cursor, char *txt, float *x, float *y, s32 *i) {
    port_hud_text(txt);
    if (cursor->menu == PORT_MENU_OPTIONS && *i == 0 && !port_vibration_supported()) {
        *y += MENUDY * 0.5f; /* as DrawMenuEntry would end, minus the line */
        *i += 1;
        dme_sx = 1.0f;
        return;
    }
    if (cursor->menu == PORT_MENU_OPTIONS && txt == tDONE[menu_lang()]) {
        static char line[3][64];
        int l = menu_lang(), m = port_display_mode();
        /* the width scale the (translated) DrawMenu set for DONE, used for
         * ours too; DrawMenuEntry resets it after each entry */
        float sx = dme_sx;
        if (m < 0 || m > 2) m = 0;
        snprintf(line[0], sizeof(line[0]), "%s: %s", k_screen[l], k_mode[m][l]);
        snprintf(line[1], sizeof(line[1]), "%s: %s", k_rate[l], port_pace_lock_on() ? k_30[l] : k_unlocked[l]);
        snprintf(line[2], sizeof(line[2]), "%s: %s", k_fps[l], port_fps_overlay_on() ? tON[l] : tOFF[l]);
        dme_sx = sx;
        DrawMenuEntry(cursor, line[0], x, y, i);
        dme_sx = sx;
        DrawMenuEntry(cursor, line[1], x, y, i);
        dme_sx = sx;
        DrawMenuEntry(cursor, line[2], x, y, i);
        dme_sx = sx;
    }
    DrawMenuEntry(cursor, txt, x, y, i);
}

void port_hook_ProcMenu(struct cursor_s *cursor, struct nupad_s *pad) {
    int menu0 = cursor->menu, y0 = cursor->y, lock0 = cursor->button_lock, item0 = cursor->item_frame;
    int wait0 = cursor->wait, busy = fadeval > 0 || (u32)(Paused - 1) < 0x1d;
    /* own edge detection on the held buttons: ProcMenu can run more than once
     * per pad read (one call per logic tick), and oldpaddata stays set for
     * every call of that frame */
    static u32 held_prev;
    u32 held = pad ? (u32)pad->paddata : 0, db;
    if (pad && (held & 0xf000) == 0) held |= (u32)stick_bits;
    db = held & ~held_prev;
    held_prev = held;
    ProcMenu(cursor, pad);
    /* the hidden rumble entry (handheld): never the cursor's; up from the
     * first entry shown wraps to DONE like ProcMenu's own wrap */
    if (cursor->menu == PORT_MENU_OPTIONS && cursor->y == 0 && !port_vibration_supported())
        cursor->y = (menu0 == PORT_MENU_OPTIONS && y0 == 1) ? cursor->y_max : 1;
    /* act like ProcMenu would: same menu, cursor not moved this frame, no
     * button lock / wait / fade, and not on the frame the cursor arrived */
    if (menu0 != PORT_MENU_OPTIONS || cursor->menu != PORT_MENU_OPTIONS || cursor->y != y0 || pad == NULL)
        return;
    if (lock0 != 0 || wait0 != 0 || cursor->wait != 0 || item0 == 0 || busy) return;
    if (cursor->y < cursor->y_max - 3 || cursor->y > cursor->y_max - 1) return;
    if (cursor->y == cursor->y_max - 3) {
        if (db & 0x8000) port_display_step(-1);         /* D-pad left */
        else if (db & (0x2000 | 0x840)) port_display_step(1); /* right, cross, start */
        else return;
    } else {
        if (!(db & (0x8000 | 0x2000 | 0x840))) return;
        if (cursor->y == cursor->y_max - 2) port_pace_lock_toggle();
        else port_fps_overlay_toggle();
    }
    GameSfx(PORT_SFX_YES, NULL);
}

/* ---- Fog-based far clip ----
 * Levels draw far past the point where their linear fog is total (bonus
 * castle: farclip 140, fog total at 50; earth: farclip 1000, fog at 100).
 * There the geometry comes out in the fog colour, and the frame is cleared
 * to that same colour (main.c: level_clearcolour from fogr/g/b), so it can be
 * clipped without changing the picture: NuCameraSet caps the camera's far
 * plane (culling + projection) at the full-fog distance plus a margin.
 * Not done when the fog is off, while paused, or when the level has a sky
 * (parallax objects 9/10 are drawn behind the world: clipped terrain would
 * reveal it). Recomputed every frame from NuLightFog (some levels animate
 * their fog). LDATA->farclip (AI, crate ranges) is untouched.
 * CWOC_FOGCLIP=0 disables it. */
float port_fog_farclip;
extern s32 pause_rndr_on;
int port_level_has_sky(void);

void port_fogclip_update(float pnear, float pfar) {
    static int on = -1;
    float cap = 0.0f;
    if (on < 0) {
        const char *e = getenv("CWOC_FOGCLIP");
        on = !(e && e[0] == '0');
    }
    if (on && pfar > pnear && pfar > 1.0f && pause_rndr_on == 0 && !port_level_has_sky())
        cap = pfar * 1.05f + 2.0f;
    if ((cap > 0.0f) != (port_fog_farclip > 0.0f)) {
        extern void port_log(const char *fmt, ...);
        if (cap > 0.0f) port_log("fogclip: camera far clip capped at %.1f (fog %.1f-%.1f)", cap, pnear, pfar);
        else port_log("fogclip: off (fog %.1f-%.1f, sky %d, paused %d)", pnear, pfar, port_level_has_sky(), pause_rndr_on);
    }
    port_fog_farclip = cap;
}
