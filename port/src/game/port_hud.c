/* The port's additions to the HUD, made on the game's own drawing: hooks of
 * calls from the translated DrawPanel, DrawMenu and Text3D (tools/ppc2c.py
 * HOOKS).
 *
 * - The frames-per-second counter (pause > OPTIONS > SHOW FPS) is text of the
 *   game's 3D font in the top-left corner, drawn in the HUD scene like the
 *   wumpa counter. The rate is measured on the render thread (gx_gl.c).
 *
 * - Widescreen pause screen. In widescreen the HUD keeps the original 4:3
 *   layout in the middle of the screen (the panel camera keeps its scale), so
 *   the pause screen's frame (panel objects 98 and 99, made for 4:3) left the
 *   sides of the screen uncovered. While that frame is up (DrawPanel draws it
 *   for menus 5-10), the frame is stretched to the whole width and what is
 *   placed on it (menus, stats, icons) moves out with it: the x of each thing
 *   the translated DrawPanel / DrawMenu place is multiplied by the wide factor,
 *   but text and models keep their shape. Nested calls (the glyphs of a text,
 *   what BigOutOf or DrawMenuEntry draw) are not moved again: g_hud_depth, and
 *   nurndr_forced_mtl_table, which Text3D sets while it draws its glyphs.
 *
 * - PS Vita buttons. The game's texts write the controller buttons as
 *   lowercase a, b, x, y, w and n ("APPUYER SUR a POUR CONTINUER"): Text3D
 *   draws those as the uppercase letter A, B, X, Y, R or L, flat (a quarter of
 *   the usual depth; the font's lowercase entries are the same objects as the
 *   uppercase ones). The flat A, B, X and Y are not drawn: the render thread
 *   draws the PS Vita button the port maps that GameCube button to in their
 *   place (port_pad.c: A = cross, B = square, X = circle; gx_gl.c). The
 *   flat Y becomes the triangle: no game text names Y (on Select in the
 *   port, it only shows the HUD counters) and the rewritten texts write y
 *   for the tiptoe (GameCube Z), which the port puts on Triangle. The flat
 *   L and R stay: the Vita's are called the same.
 *   CWOC_GCBUTTONS=1 keeps the GameCube glyphs.
 *
 *   The texts that name GameCube buttons in words ("Z BUTTON", "CONTROL
 *   STICK", the triggers) are rewritten for the Vita in the six languages
 *   (port_hud_init). The log gets the first strings where a button was
 *   replaced. */

extern void port_log(const char *fmt, ...);
extern float port_wide_factor(void);
extern int port_frame_count(void);
extern int port_fps_overlay_on(void);
extern int port_fps_x10(void);
extern void port_gx_icon(float x, float y, float d, int icon);
extern void port_gx_icon_shade(float s);

extern s32 Paused;
extern s32 fadeval;
extern s32 disable_safearea_clamp;
extern struct ldata_s *LDATA;
extern struct nugscn_s *font3d_scene;
extern struct numtl_s **nurndr_forced_mtl_table;
extern s16 Font3DRemap[256];
extern float FONT3DSIZE;
extern float FONT3DYMUL;
extern float PANEL3DMULX;
extern float PANEL3DMULY;

extern char *tPRESSxTOCONTINUE[6];
extern char *tSUPERBELLYFLOPTEXT[6][7];
extern char *tDEATHTORNADOSPINTEXT[6][7];
extern char *tDOUBLEJUMPTEXT[6][7];
extern char *tFRUITBAZOOKATEXT[6][7];
extern char *tSPEEDSHOESTEXT[6][7];
extern char *tSNEAKSHOESTEXT[6][7];

void DrawPanel(void);
void DrawMenu(struct cursor_s *cursor, s32 paused);
void BigOutOf(float x, float y, float scale, int value, int total);
void DrawTimeTrialTimes(int level, float x, float y, float z);
void DrawMenuEntry2(struct cursor_s *cursor, char *txt0, char *txt1, float *x, float *y, s32 *i);

/* panel objects of the pause screen's frame (DrawPanel) */
#define PAUSE_FRAME_A 98
#define PAUSE_FRAME_B 99

/* ---------------------------------------------------------------- layout */
static float g_hud_wf = 1.0f; /* != 1: wide pause layout, x of placed things times this */
static int g_hud_depth;       /* hooked calls in progress (only the outermost one moves) */
static const char *g_text_cur; /* the text being drawn (log of replaced buttons) */

static int hud_moves(void) {
    return g_hud_wf != 1.0f && g_hud_depth == 0 && nurndr_forced_mtl_table == NULL;
}

/* used by every hook that places something: returns the x to use; *st is
 * the safe-area clamp to put back (-1: untouched). Text3D squeezes text
 * that starts left of -0.81: not wanted once the layout is wider. */
float port_hud_enter(float x, int *st) {
    *st = -1;
    if (hud_moves()) {
        x *= g_hud_wf;
        *st = disable_safearea_clamp;
        disable_safearea_clamp = 1;
    }
    g_hud_depth++;
    return x;
}

void port_hud_leave(int st) {
    g_hud_depth--;
    if (st >= 0) disable_safearea_clamp = st;
}

/* port_hooks.c (DrawMenuEntry) */
void port_hud_text(const char *txt) { g_text_cur = txt; }

static int wide_pause_wanted(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("CWOC_WIDEPAUSE");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* ---------------------------------------------------------------- buttons */
enum { ICON_CROSS, ICON_CIRCLE, ICON_SQUARE, ICON_TRIANGLE }; /* atlas order (gx_gl.c) */
static const char k_btn_char[4] = { 'a', 'b', 'x', 'y' };
static const int k_btn_icon[4] = { ICON_CROSS, ICON_SQUARE, ICON_CIRCLE, ICON_TRIANGLE };
static const char *const k_icon_name[4] = { "cross", "circle", "square", "triangle" };

static int vita_buttons_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("CWOC_GCBUTTONS");
        on = !(e && e[0] == '1');
        port_log("hud: %s button glyphs", on ? "PS Vita" : "GameCube");
    }
    return on;
}

/* z / y scale of the text being drawn (Text3D / Text3D2 hooks; 0: not known) */
static float g_text_zr;

/* which GameCube button glyph Text3D is drawing (-1: none). The font's
 * lowercase a, b, x, y entries are the same objects as the uppercase letters
 * (Font3DTab names them "a", "b", "x", "y" too): a button is the letter drawn
 * flat, a quarter of the depth Text3D gives every other glyph. So: flat, and
 * then which letter. (w and n, the flat R and L, are the PS Vita's L and R
 * too: they stay.) */
static int button_of(const struct nuspecial_s *obj, float scaley, float scalez) {
    /* a text drawn from native code (DrawMenuEntry: "PRESS a TO CONTINUE")
     * does not go through the Text3D hook: the game's texts have z = y
     * scale (1.2 at most), so a glyph is flat below 2 */
    float zr = g_text_zr > 0.0f ? g_text_zr : 1.0f;
    int b;
    if (scaley == 0.0f || fabsf(scalez / scaley) >= 2.0f * zr) return -1;
    for (b = 0; b < 4; b++) {
        int i = Font3DRemap[(u8)k_btn_char[b]];
        if (i >= 0 && i < 62 && Font3DTab[i].obj.special == obj) return b;
    }
    return -1;
}

static float text_zr(float scaley, float scalez) { return scaley != 0.0f ? fabsf(scalez / scaley) : 0.0f; }

static void log_text(const char *label, const char *s);

/* the glyph Text3D was going to draw at (x, y, z), with its y scale sy
 * (FONT3DSIZE x the text's y scale): the icon at the same place, a fifth
 * taller than the text's line (font3d_dy), so the symbol reads at the Vita's
 * resolution (the glyph sits between spaces: there is room) */
static void button_icon(int b, float x, float y, float z, float sy) {
    /* GS_SetProjectionMatrix: 40 degree vertical field of view, aspect
     * 1.428571 times the wide factor; a panel object sits at
     * (x * PANEL3DMULX, y * PANEL3DMULY, z) in front of the camera */
    const float t = 0.36397023f; /* tan(20 deg) */
    float aspect = 1.428571f * port_wide_factor();
    float scaley = FONT3DSIZE > 0.0f ? sy / FONT3DSIZE : sy;
    float d = 0.1f * scaley * (FONT3DYMUL > 0.0f ? FONT3DYMUL : 1.0f) * 1.2f;
    static int logged;
    if (z <= 0.01f) return;
    port_gx_icon(x * PANEL3DMULX / (z * t * aspect), y * PANEL3DMULY / (z * t), d * PANEL3DMULY / (z * t),
                 k_btn_icon[b]);
    if (logged < 24 && g_text_cur) {
        static const char *seen[24];
        int i;
        for (i = 0; i < logged; i++)
            if (seen[i] == g_text_cur) break;
        if (i == logged) {
            char label[64];
            seen[logged++] = g_text_cur;
            snprintf(label, sizeof(label), "button '%c' -> %s in", k_btn_char[b], k_icon_name[k_btn_icon[b]]);
            log_text(label, g_text_cur);
        }
    }
}

/* ---------------------------------------------------------------- hooks */
s32 port_hook_DrawPanel3DObject(s32 object, float x, float y, float z, float scalex, float scaley, float scalez,
                                u16 xrot, u16 yrot, u16 zrot, struct nugscn_s *scn, struct nuspecial_s *obj, s32 rot) {
    s32 r;
    int st;
    if (scn != NULL && scn == font3d_scene && obj != NULL && vita_buttons_on()) {
        int b = button_of(obj, scaley, scalez);
        if (b >= 0) {
            button_icon(b, x, y, z, scaley);
            return 1;
        }
    }
    if ((object == PAUSE_FRAME_A || object == PAUSE_FRAME_B) && hud_moves())
        scalex *= g_hud_wf; /* the frame itself: stretched to the whole width */
    x = port_hud_enter(x, &st);
    r = DrawPanel3DObject(object, x, y, z, scalex, scaley, scalez, xrot, yrot, zrot, scn, obj, rot);
    port_hud_leave(st);
    return r;
}

s32 port_hook_DrawPanel3DCharacter(s32 character, float x, float y, float z, float scalex, float scaley, float scalez,
                                   u16 xrot, u16 yrot, u16 zrot, s32 action, float anim_time, s32 rot) {
    s32 r;
    int st;
    x = port_hud_enter(x, &st);
    r = DrawPanel3DCharacter(character, x, y, z, scalex, scaley, scalez, xrot, yrot, zrot, action, anim_time, rot);
    port_hud_leave(st);
    return r;
}

void port_hook_Text3D(char *txt, float x, float y, float z, float scalex, float scaley, float scalez, s32 align,
                      s32 colour) {
    const char *prev = g_text_cur;
    float prev_zr = g_text_zr;
    int st;
    g_text_cur = txt;
    g_text_zr = text_zr(scaley, scalez);
    x = port_hud_enter(x, &st);
    Text3D(txt, x, y, z, scalex, scaley, scalez, align, colour);
    port_hud_leave(st);
    g_text_cur = prev;
    g_text_zr = prev_zr;
}

void port_hook_Text3D2(char *txt, float x, float y, float z, float scalex, float scaley, float scalez, s32 align,
                       s32 colour) {
    const char *prev = g_text_cur;
    float prev_zr = g_text_zr;
    int st;
    g_text_cur = txt;
    g_text_zr = text_zr(scaley, scalez);
    x = port_hud_enter(x, &st);
    Text3D2(txt, x, y, z, scalex, scaley, scalez, align, colour);
    port_hud_leave(st);
    g_text_cur = prev;
    g_text_zr = prev_zr;
}

void port_hook_BigOutOf(float x, float y, float scale, int value, int total) {
    int st;
    x = port_hud_enter(x, &st);
    BigOutOf(x, y, scale, value, total);
    port_hud_leave(st);
}

void port_hook_DrawTimeTrialTimes(int level, float x, float y, float z) {
    int st;
    x = port_hud_enter(x, &st);
    DrawTimeTrialTimes(level, x, y, z);
    port_hud_leave(st);
}

void port_hook_DrawMenuEntry2(struct cursor_s *cursor, char *txt0, char *txt1, float *x, float *y, s32 *i) {
    float x0 = *x;
    int st;
    g_text_cur = txt0;
    *x = port_hud_enter(x0, &st);
    DrawMenuEntry2(cursor, txt0, txt1, x, y, i);
    port_hud_leave(st);
    *x = x0;
    g_text_cur = NULL;
}

/* ---------------------------------------------------------------- fps */
static void fps_draw(void) {
    static int last_frame = -1;
    char buf[40];
    int v = port_fps_x10(), clamp, f = port_frame_count();
    float x, y;
    if (!port_fps_overlay_on() || v < 0 || f == last_frame || PANEL3DMULX <= 0.0f || PANEL3DMULY <= 0.0f) return;
    last_frame = f;
    snprintf(buf, 16, "%d.%d", v / 10, v % 10); /* the number alone, smaller than the HUD's text */
    if (Game.language == 0x63) AddSpacesIntoText(buf, 1);
    /* left-aligned near the top-left corner of the picture: the panel x, y
     * that the projection (button_icon) puts at 3.5 % / 8.5 % of the screen
     * from the edges. In widescreen that is past the HUD's 4:3 part. */
    x = -0.965f * (0.36397023f * 1.428571f * port_wide_factor()) / PANEL3DMULX;
    y = 0.915f * 0.36397023f / PANEL3DMULY;
    clamp = disable_safearea_clamp;
    disable_safearea_clamp = 1;
    g_hud_depth++; /* its glyphs are not moved by the wide pause layout */
    Text3D(buf, x, y, 1.0f, 0.35f, 0.35f, 0.35f, 4, 0);
    g_hud_depth--;
    disable_safearea_clamp = clamp;
}

/* ---------------------------------------------------------------- log */
static void log_text(const char *label, const char *s) {
    char buf[256];
    int n = 0;
    if (s == NULL) {
        port_log("hud: %s (null)", label);
        return;
    }
    for (; *s && n < 236; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x20 && c < 0x7f && c != '\\') buf[n++] = (char)c;
        else n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\\x%02x", c);
    }
    buf[n] = 0;
    port_log("hud: %s \"%s\"", label, buf);
}

/* ---------------------------------------------------------------- texts */
/* The texts that name GameCube buttons, rewritten for the PS Vita, by what
 * each button does in the port rather than by the letter the text uses (the
 * languages disagree: the tornado spin is "B BUTTON" in English and button x
 * in the other five, and the belly flop and the bazooka name b, leftovers of
 * the Xbox texts). A lowercase a, b, x or y
 * becomes the PS Vita button (cross, square, circle, triangle: see above);
 * Z (the tiptoe) is on Triangle, so it is written y; the L and R triggers are the L and R buttons; the control
 * stick is the left stick. The game's own code page (CP850: \232 = U umlaut,
 * \216 = A umlaut, \220 = E acute, \242 = O acute). Only applied when the
 * original is the expected one (the GCBE7D disc). CWOC_GCBUTTONS=1: the
 * original texts. */
/* MoveCRASH (move.s): jump 0x40 = A, spin 0x80 = B (the tornado: B again),
 * crouch 0x20 = X (the belly flop: X in the air), bazooka aim 0x04 = L and
 * fire a new 0x20 or 0x08 = X or R, sprint 0x08 = R, tiptoe 0x01 = Z */
#define FIRE "x"   /* fruit bazooka fire: X (circle), not B as the texts said */
#define CROUCH "x" /* belly flop: X (circle), at the top of the jump */
/* tiptoe: GameCube Z, which the port puts on Triangle (port_pad.c): the y
 * glyph, which is drawn as the triangle icon */
#define TIPTOE "y"
struct hud_text { char **slot; const char *label; const char *from, *to; };
#define T1(t, l, f, n) { &t[l], #t "[" #l "]", f, n }
#define T7(t, l, k, f, n) { &t[l][k], #t "[" #l "][" #k "]", f, n }
static const struct hud_text k_texts[] = {
    /* 0 English */
    T1(tPRESSxTOCONTINUE, 0, "PRESS A BUTTON TO CONTINUE", "PRESS a TO CONTINUE"),
    T7(tDOUBLEJUMPTEXT, 0, 1, "PRESS A BUTTON TO JUMP, AND THEN", "PRESS a TO JUMP, AND THEN"),
    T7(tDOUBLEJUMPTEXT, 0, 2, "PRESS A BUTTON AGAIN AT THE", "PRESS a AGAIN AT THE"),
    T7(tDEATHTORNADOSPINTEXT, 0, 2, "PRESS B BUTTON REPEATEDLY", "PRESS b REPEATEDLY"),
    T7(tSUPERBELLYFLOPTEXT, 0, 2, "PRESS b AT THE", "PRESS " CROUCH " AT THE"),
    T7(tFRUITBAZOOKATEXT, 0, 2, "THE GUN, AIM WITH THE +CONTROL", "THE GUN, AIM WITH THE"),
    T7(tFRUITBAZOOKATEXT, 0, 3, "PAD OR CONTROL STICK,", "DIRECTIONAL BUTTONS OR LEFT STICK,"),
    T7(tFRUITBAZOOKATEXT, 0, 4, "THEN PRESS b TO FIRE", "THEN PRESS " FIRE " TO FIRE"),
    T7(tSNEAKSHOESTEXT, 0, 1, "CRATES HOLD Z BUTTON WHILE MOVING", "CRATES HOLD " TIPTOE " WHILE MOVING"),
    /* 1 French */
    T7(tDEATHTORNADOSPINTEXT, 1, 3, "PLUSIEURS FOIS SUR LA TOUCHE x", "PLUSIEURS FOIS SUR LA TOUCHE b"),
    T7(tSUPERBELLYFLOPTEXT, 1, 2, "PUIS SUR LA TOUCHE b AU", "PUIS SUR LA TOUCHE " CROUCH " AU"),
    T7(tFRUITBAZOOKATEXT, 1, 1, "LONGUEMENT LA GACHETTE G,", "LONGUEMENT LA TOUCHE L,"),
    T7(tFRUITBAZOOKATEXT, 1, 2, "VISE A L'AIDE DU BOUTON", "VISE A L'AIDE DES TOUCHES"),
    T7(tFRUITBAZOOKATEXT, 1, 3, "MULTIDIRECTIONNEL ET APPUIE", "DIRECTIONNELLES ET APPUIE"),
    T7(tFRUITBAZOOKATEXT, 1, 4, "SUR LA TOUCHE b POUR TIRER", "SUR LA TOUCHE " FIRE " POUR TIRER"),
    T7(tSPEEDSHOESTEXT, 1, 2, "G\266CHETTE DROIT ENFONC\220E", "R ENFONC\220E"),
    T7(tSNEAKSHOESTEXT, 1, 1, "DES PIEDS MAINTIENS Z", "DES PIEDS MAINTIENS " TIPTOE),
    /* 2 German */
    T7(tDEATHTORNADOSPINTEXT, 2, 1, "x-TASTE UM SICH L\216NGER UND", "b-TASTE UM SICH L\216NGER UND"),
    T7(tSUPERBELLYFLOPTEXT, 2, 2, "DEM SPRUNG, UND DIE b-TASTE", "DEM SPRUNG, UND DIE " CROUCH "-TASTE"),
    T7(tFRUITBAZOOKATEXT, 2, 0, "DR\232CKEN UND HALTEN SIE DEN", "DR\232CKEN UND HALTEN SIE DIE"),
    T7(tFRUITBAZOOKATEXT, 2, 1, "SCHALTER LINKS FEST, UM DAS GEWEHR", "L-TASTE FEST, UM DAS GEWEHR"),
    T7(tFRUITBAZOOKATEXT, 2, 4, "DR\232CKEN SIE b, UM MIT FR\232CHTEN", "DR\232CKEN SIE " FIRE ", UM MIT FR\232CHTEN"),
    T7(tSPEEDSHOESTEXT, 2, 1, "DEN SCHALTER RECHTS GEDR\232CKT", "DIE R-TASTE GEDR\232CKT"),
    T7(tSNEAKSHOESTEXT, 2, 1, "ODER GEHENS DIE Z-TASTE", "ODER GEHENS DIE " TIPTOE "-TASTE"),
    /* 3 Italian */
    T7(tDEATHTORNADOSPINTEXT, 3, 3, "RIPETUTAMENTE IL TASTO x", "RIPETUTAMENTE IL TASTO b"),
    T7(tSUPERBELLYFLOPTEXT, 3, 2, "QUINDI PREMI IL TASTO b NEL", "QUINDI PREMI IL TASTO " CROUCH " NEL"),
    T7(tFRUITBAZOOKATEXT, 3, 2, "GRILLETTO SINISTRO PER SOLLEVARE", "TASTO L PER SOLLEVARE"),
    T7(tFRUITBAZOOKATEXT, 3, 4, "TASTO DIREZIONALE E PREMI IL", "TASTI DIREZIONALI E PREMI IL"),
    T7(tFRUITBAZOOKATEXT, 3, 5, "PULSANTE b PER SPARARE", "TASTO " FIRE " PER SPARARE"),
    T7(tSPEEDSHOESTEXT, 3, 1, "TIENI PREMUTO L'AZIONATORE", "TIENI PREMUTO IL TASTO R"),
    T7(tSPEEDSHOESTEXT, 3, 2, "DESTRO, DURANTE LA CORSA", "DURANTE LA CORSA"),
    T7(tSNEAKSHOESTEXT, 3, 1, "TIENI PREMUTO IL TASTO Z", "TIENI PREMUTO IL TASTO " TIPTOE),
    /* 4 Spanish (the bazooka's text lost its aim and fire lines: written anew) */
    T7(tDEATHTORNADOSPINTEXT, 4, 4, "BOT\242N x REPETIDAMENTE", "BOT\242N b REPETIDAMENTE"),
    T7(tSUPERBELLYFLOPTEXT, 4, 2, "Y, A CONTINUACI\242N PULSA b", "Y, A CONTINUACI\242N PULSA " CROUCH),
    T7(tFRUITBAZOOKATEXT, 4, 1, "PULSADO EL DISPARADOR IZQUIERDO", "PULSADO EL BOT\242N L"),
    T7(tFRUITBAZOOKATEXT, 4, 2, "PARA LEVANTAR EL MANDO DE", "PARA LEVANTAR EL ARMA, APUNTA"),
    T7(tFRUITBAZOOKATEXT, 4, 3, "DIRECCI\242N Y, A CONTINUACI\242N", "CON LOS BOTONES DE DIRECCI\242N"),
    T7(tFRUITBAZOOKATEXT, 4, 4, "BOT\242N b PARA DISPARAR", "Y PULSA " FIRE " PARA DISPARAR"),
    T7(tSPEEDSHOESTEXT, 4, 1, "MANT\220N PULSADO EL DISPARADOR", "MANT\220N PULSADO EL BOT\242N R"),
    T7(tSPEEDSHOESTEXT, 4, 2, "DERECHO MIENTRAS CORRES", "MIENTRAS CORRES"),
    T7(tSNEAKSHOESTEXT, 4, 1, "MANT\220N PULSADO Z MIENTRAS", "MANT\220N PULSADO " TIPTOE " MIENTRAS"),
    /* 5 Dutch */
    T7(tDEATHTORNADOSPINTEXT, 5, 1, "x -TOETS OM LANGER EN SNELLER", "b -TOETS OM LANGER EN SNELLER"),
    T7(tSUPERBELLYFLOPTEXT, 5, 3, "OP DE b -TOETS ALS JE HET", "OP DE " CROUCH " -TOETS ALS JE HET"),
    T7(tFRUITBAZOOKATEXT, 5, 1, "HOUD JE DE TREKKER LINKS", "HOUD JE DE L-TOETS"),
    T7(tFRUITBAZOOKATEXT, 5, 3, "RICHT JE D.M.V. DE NAVIGATIEPAD", "RICHT JE MET DE RICHTINGSTOETSEN"),
    T7(tFRUITBAZOOKATEXT, 5, 4, "EN DRUK JE OP b OM TE VUREN", "EN DRUK JE OP " FIRE " OM TE VUREN"),
    T7(tSPEEDSHOESTEXT, 5, 0, "HOUD DE TREKKER RECHTS INGEDRUKT", "HOUD DE R-TOETS INGEDRUKT"),
    T7(tSNEAKSHOESTEXT, 5, 0, "HOUD DE Z-TOETS INGEDRUKT", "HOUD DE " TIPTOE " -TOETS INGEDRUKT"),
};

/* port_main.c, after the DOL data is in place: the texts for the PS Vita */
void port_hud_init(void) {
    unsigned i, n = 0;
    if (!vita_buttons_on()) return;
    for (i = 0; i < sizeof(k_texts) / sizeof(k_texts[0]); i++) {
        const struct hud_text *t = &k_texts[i];
        if (*t->slot != NULL && strcmp(*t->slot, t->from) == 0) {
            *t->slot = (char *)t->to;
            n++;
        } else {
            log_text(t->label, *t->slot);
            port_log("hud: ^ not the expected text, kept");
        }
    }
    port_log("hud: %u of %u texts rewritten for the PS Vita buttons", n, (unsigned)(sizeof(k_texts) / sizeof(k_texts[0])));
}

/* ---------------------------------------------------------------- panel */
void port_hook_DrawPanel(void) {
    float wf = port_wide_factor();
    int menu = Cursor.menu;
    g_hud_depth = 0;
    g_text_cur = NULL;
    g_text_zr = 0.0f;
    /* the pause screen's frame is up: DrawPanel draws it for these menus */
    g_hud_wf = (wf > 1.001f && wide_pause_wanted() && Paused != 0 && LDATA != NULL && (LDATA->flags & 1) != 0 &&
                menu >= 5 && menu <= 10) ? wf : 1.0f;
    DrawPanel();
    g_hud_wf = 1.0f;
    g_hud_depth = 0;
    /* DrawFade, after the HUD scene, multiplies the frame by 1 - fadeval/255
     * (not on menu 2): the icons, drawn later, get the same */
    port_gx_icon_shade((Cursor.menu != 2 && fadeval > 0) ? 1.0f - fadeval / 255.0f : 1.0f);
}

/* DrawPanel's menus: the fps counter goes in the same scene, with the same
 * set-up (DrawPanel calls DrawMenu before anything else it draws) */
void port_hook_DrawMenu(struct cursor_s *cursor, s32 paused) {
    DrawMenu(cursor, paused);
    fps_draw();
}
