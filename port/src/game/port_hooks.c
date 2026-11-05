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
