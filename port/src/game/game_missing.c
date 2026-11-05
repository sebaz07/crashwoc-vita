/* Game functions that are still assembly-only in the decompilation.
 *
 * Each one is a temporary stub that logs its first call. They are being
 * replaced one by one with C reimplementations (see port/README.md and the
 * F4 milestone). Critical for the MVP: MoveCRASH, DoInput, NuPs2ReadPad
 * (already in nuxbox/dummyfunc.c as asm), ReadNuIFFTexAnimSet, projectiles. */
#include "port.h"

/* ---- player movement (gamecode/move.c) ---- */
void MoveCRASH(struct creature_s *plr, struct nupad_s *pad) { PORT_STUB(); }
void MoveCOCO(struct creature_s *plr, struct nupad_s *pad) { PORT_STUB(); }
void MoveSCOOTER(struct creature_s *plr, struct nupad_s *pad) { PORT_STUB(); }
void MoveSNOWBOARD(struct creature_s *plr, struct nupad_s *pad) { PORT_STUB(); }
void MoveOFFROADER(struct creature_s *plr, struct nupad_s *pad) { PORT_STUB(); }
void MoveMECH(struct creature_s *plr, struct nupad_s *pad) { PORT_STUB(); }
void MoveFIREENGINE(struct creature_s *plr, struct nupad_s *pad) { PORT_STUB(); }
void MoveGYRO(struct creature_s *plr, struct nupad_s *pad) { PORT_STUB(); }
void MoveSUBMARINE(struct creature_s *plr, struct nupad_s *pad) { PORT_STUB(); }
void MoveMINECART(struct creature_s *plr, struct nupad_s *pad) { PORT_STUB(); }
void MoveTub(struct tubinfo_s *tub, s32 LEFT, s32 RIGHT) { PORT_STUB(); }

/* ---- projectiles (gamecode/game_obj.c) ---- */
void AddProjectile(struct nuvec_s *src, struct nuvec_s *dst, struct nuvec_s *mom, s32 type, u16 yrot,
                   struct obj_s *obj) { PORT_STUB(); }
void UpdateProjectiles(void) { PORT_STUB(); }

/* ---- main.c ---- */
void DoInput(void) { PORT_STUB(); }
void LoadGBABG(void) { PORT_STUB(); }
void Managememcard(void) { PORT_STUB(); }

/* ---- jeep / vehicle levels (post-MVP) ----
 * The signatures here become the translated wrappers' signatures: they must
 * match the arguments the original reads (r3, r4, ...). */
s32 CurrentWesternPosition(void) { PORT_STUB(); return 0; }
struct numtx_s *DrawJeep(struct JEEPSTRUCT *Jeep) { PORT_STUB(); return NULL; }
void DrawEnemyJeep(void *jeep) { PORT_STUB(); }
void JeepCam(struct cammtx_s *CamMtx) { PORT_STUB(); }
void JeepCamFollowAng(struct cammtx_s *cam, s32 blend) { PORT_STUB(); }
void MovePlayerJeep(struct creature_s *Cre, struct nupad_s *Pad) { PORT_STUB(); }
void ProcessEnemyJeep(void *jeep) { PORT_STUB(); }
void ProcessFireBoss(void *boss) { PORT_STUB(); }
void ProcessJeepTrail(void *trail, int id) { PORT_STUB(); }
void ResetJeep(struct creature_s *Cre) { PORT_STUB(); }
void CheckForPotentialMidAirCollisions(void) { PORT_STUB(); }
void ProcessAsteroid(struct ASTEROIDSTRUCT *Asteroid) { PORT_STUB(); }
void ProcessAtlas(struct ATLASSTRUCT *Atlas) { PORT_STUB(); }
void ProcessAtlasAtlasCollisions_a(struct ATLASSTRUCT *a, struct ATLASSTRUCT *b) { PORT_STUB(); }
void ProcessAtlasTrail(struct ATLASSTRUCT *Atlas) { PORT_STUB(); }
void ProcessBattleShip(struct BATTLESHIPSTRUCT *BattleShip) { PORT_STUB(); }
void ProcessEarthBossLevel(struct nupad_s *Pad) { PORT_STUB(); }
void ProcessGliderBombs(void) { PORT_STUB(); }
void ProcessVehMasks(void) { PORT_STUB(); }
struct BUGSAVE *LoadBuggyData(struct BUGSAVE *Data, s32 Indx) { PORT_STUB(); return Data; }
/* nuxbox/ps2dma: chains particle chunk DMA sets (from the DOL, 0x800C5D68):
 * the last set is marked as the end, every other set links to its successor. */
void LinkDmaParticalSets(int **table, int count) {
    /* ps2dma.s: the last set terminates the chain, the others link forward.
     * (Like the DOL, the terminator is written before the count check.) */
    int k;
    table[count - 1][0] = 1;
    for (k = count - 2; k >= 0; k--) {
        table[k][0] = 0;
        table[k][1] = (int)table[k + 1];
    }
}

/* ---- PS2/Xbox pad layer (nuxbox/dummyfunc.c) ---- */
static struct nupad_s port_pads[2];
struct nupad_s *NuPs2OpenPad(s32 port, s32 slot) {
    PORT_STUB();
    if (port < 0 || port > 1) return NULL;
    port_pads[port].port = port;
    port_pads[port].slot = slot;
    return &port_pads[port];
}
void NuPs2PadSetMotors(struct nupad_s *pad, s32 motor1, s32 motor2) {}
void storemenu1(void) { PORT_STUB(); }
void restoremenu1(void) { PORT_STUB(); }
void storemenu2(void) { PORT_STUB(); }
void restoremenu2(void) { PORT_STUB(); }

/* ---- memory card / saves ----
 * memcard.s is translated (recomp.txt) on top of port_card.c (CARD API on host
 * files); these weak stubs only fix the C-side signatures of the wrappers. */
void MC_Init(void) { PORT_STUB(); }
void MemCardRetry(void) { PORT_STUB(); }
void MemCardDelete(void) { PORT_STUB(); }
void saveloadASCallEachFrame(void) { PORT_STUB(); }
void saveloadASDelete(char *name, s32 slot) { PORT_STUB(); }
void saveloadASFormat(void) { PORT_STUB(); }
void saveloadASSave(char *name, struct game_s *data, s32 size) { PORT_STUB(); }
s32 DoRead(s32 header) { PORT_STUB(); return 0; }
s32 ParseNintendoErrorCode(void) { PORT_STUB(); return 0; }

/* ---- Game Boy Advance link (not applicable) ---- */
void GBA_Download(void) {}
void GBA_DownloadEnd(void) {}
void GBA_DownloadStart(void) {}
void GBA_Draw(void) {}

/* ---- misc ---- */
void NuSoundFlushLoops(void) {}
void UpdateTime(s32 *year, s32 *month, s32 *day, s32 *hour, s32 *minute) { PORT_STUB(); }

float NuFsin(float a) { return sinf(a); }
float NuFcos(float a) { return cosf(a); }

/* vehicle.c refers to a 'vec' global that does not exist in the DOL under that
 * name (decompilation artefact); keep the post-MVP vehicle code linking. */
struct nuvec_s *vec;

/* GBA link-cable probe called from the pad poll: no GBA on the port. */
void PadCheck(void) {}

/* ---- boss AI command callbacks (creatcmd tables in the DOL data) ----
 * Translated by ppc2c; these weak stubs only give the DOL data generator a
 * C symbol to point the tables at (the translation overrides them). */
void SpaceCortexFunction_Defeated(struct creature_s *c, void *arg) { PORT_STUB(); }
void SpaceCortexFunction_TakeHit(struct creature_s *c, void *arg) { PORT_STUB(); }
void WaterCrunchFunction_Attack(struct creature_s *c, void *arg) { PORT_STUB(); }
void WaterCrunchFunction_Punch1(struct creature_s *c, void *arg) { PORT_STUB(); }
void WaterCrunchFunction_Punch2(struct creature_s *c, void *arg) { PORT_STUB(); }
void WaterCrunchFunction_Spladoosh(struct creature_s *c, void *arg) { PORT_STUB(); }
