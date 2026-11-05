/* GENERATED once by port tooling (see port/README.md), then maintained by hand.
 * Prototypes for functions that game translation units call without a
 * declaration and that take float/short/char/struct-by-value arguments or
 * return float/struct values. PowerPC passes ints and floats in separate
 * register files, so the original implicit calls worked there; on ARM and x86
 * they would pass doubles or read the wrong return register. */
#ifndef PORT_PROTOS_H
#define PORT_PROTOS_H

struct CharacterModel;
struct GLIDERSTRUCT;
struct MYDRAW;
struct NUHGOBJ_s;
struct NUJOINTANIM_s;
struct NUJOINTDATA_s;
struct _GS_VERTEXTL;
struct anim_s;
struct cammtx_s;
struct creature_s;
struct nuanimcurve2_s;
struct nuanimcurveset_s;
struct nuanimdata2_s;
struct nuanimdata_s;
struct nuanimtime_s;
struct nugscn_s;
struct nugspline_s;
struct numtl_s;
struct numtx_s;
struct nuspecial_s;
struct nuvec_s;
struct obj_s;

void AddAnimDebris(struct CharacterModel *model, struct numtx_s *mtxLOCATOR, s32 action, float time, struct obj_s *obj);
void AddDebrisEffect(s32 *key, s32 type, float x, float y, float z);
void AddKaboom(s32 type, struct nuvec_s *pos, float radius);
void AddScreenWumpa(float x, float y, float z, s32 count);
void AddVariableShotDebrisEffect(s32 type, struct nuvec_s *pos, s32 numdeb, short int emitrotz, short int emitroty);
void AnimateDIVE(struct creature_s *plr, float ratio);
void ApplyFriction(float *val, float rate, float dt);
void BlendGameCamera(struct cammtx_s *cam, float time);
void BossBar(float x, float y, float z, float xs, float ys, s32 i, s32 j);
void DebrisEmitterOrientation(s32 key, short int emitrotz, short int emitroty);
s32 Draw3DObject(s32 object, struct nuvec_s *pos, u16 xrot, u16 yrot, u16 zrot, float scalex, float scaley, float scalez, struct nugscn_s *scn, struct nuspecial_s *obj, s32 rot);
void DrawGameMessage(char *txt, s32 message_frame, float ys);
s32 FurtherALONG(s32 iRAIL0, s32 iALONG0, float fALONG0, s32 iRAIL1, s32 iALONG1, float fALONG1);
s32 FurtherBEHIND(s32 iRAIL0, s32 iALONG0, float fALONG0, s32 iRAIL1, s32 iALONG1, float fALONG1);
void GS_DrawTriListTTL(struct _GS_VERTEXTL *vertlist, s16 nverts);
void GameRayCast(struct nuvec_s *src, struct nuvec_s *dir, float d, struct nuvec_s *dst);
void GetLocatorMtx(struct CharacterModel *model, struct numtx_s *dest, float angle);
void GetSurfaceInfo(struct obj_s *obj, s32 ter, float surface_y);
s16 GetVolumeI(float vol);
void JudderGameCamera(struct cammtx_s *cam, float time, struct nuvec_s *pos);
s32 LimitAng360f(float *dest, float min, float max);
s32 LineCrossed(float xold, float zold, float xnew, float znew, float x0, float z0, float x1, float z1);
float ModelAnimDuration(u32 character, u32 action, float start, float end);
void MyAnimateModelNew(struct MYDRAW *Draw, float dt);
float NewShadowMask(struct nuvec_s *ppos, float size, int extramask);
float NewShadowMaskPlat(struct nuvec_s *ppos, float size, int extramask);
float NewShadowMaskPlatRot(struct nuvec_s *ppos, float size, int extramask);
float NewShadowPlat(struct nuvec_s *ppos, float size);
void NewTerrainScaleY(struct nuvec_s *vpos, struct nuvec_s *vvel, u8 *flags, s32 terid, float stopflag, float size, float yscale);
void NuAnimCurve2SetApplyToMatrix(struct nuanimcurve2_s *animcurveset, char *curveflags, char curvesetflags, struct nuanimtime_s *atime, struct numtx_s *T);
void NuAnimCurveSetApplyBlendToJoint2(struct nuanimcurveset_s *animcurveset1, struct nuanimtime_s *atime1, struct nuanimcurveset_s *animcurveset2, struct nuanimtime_s *atime2, float blend, struct NUJOINTDATA_s *jointdata, struct nuvec_s *scale, struct nuvec_s *parentscale, struct numtx_s *T, struct NUJOINTANIM_s *offset);
void NuAnimData2CalcTime(struct nuanimdata2_s *animdata, float time, struct nuanimtime_s *atime);
void NuAnimDataCalcTime(struct nuanimdata_s *animdata, float time, struct nuanimtime_s *atime);
void NuHGobjEvalAnim(struct NUHGOBJ_s *hgobj, struct nuanimdata_s *animdata, float time, int njanims, struct NUJOINTANIM_s *janim, struct numtx_s *mtx_array);
void NuHGobjEvalAnimBlend(struct NUHGOBJ_s *hgobj, struct nuanimdata_s *animdata1, float time1, struct nuanimdata_s *animdata2, float time2, float blend, s32 njanims, struct NUJOINTANIM_s *janim, struct numtx_s *mtx_array);
float ** NuHGobjEvalDwa(int layer, void *bollox, struct nuanimdata_s *vtxanim, float vtxtime);
float ** NuHGobjEvalDwaBlend(s32 nlayers, short int *layers, struct nuanimdata_s *vtxanim1, float vtxtime1, struct nuanimdata_s *vtxanim2, float vtxtime2, float blend);
void NuHGobjPOIMtx(struct NUHGOBJ_s *hgobj, u8 poi_id, struct numtx_s *world_mtx, struct numtx_s *mtx_array, struct numtx_s *mtx);
void NuLightAddSpotXSpanFade(struct nuvec_s *pos, float XSpan, float fade);
void NuRndrAddWaterRipple(struct nuvec_s *pos, float size, float endsize, s32 duration, s32 shade);
void NuRndrClear(s32 flags, s32 colour, f32 depth);
void NuRndrRectUV2di(s32 x, s32 y, s32 w, s32 h, float tx, float ty, float tw, float th, s32 col, struct numtl_s *mtl);
s32 PlayerObjectAnimCollision(struct obj_s *obj, struct nuspecial_s *special, float radius);
void PointAlongSpline(struct nugspline_s *spl, float ratio, struct nuvec_s *dst, u16 *angle, u16 *tilt);
void ProcessGliderMovement(struct GLIDERSTRUCT *Glider, float DeltaTime);
void ScaleFlatShadow(struct nuvec_s *s, float y, float shadow, float scale);
void SeekAngLimHalfLife360f(float *dest, float target, float max_speed, float halflife, float dt);
void SeekHalfLife(float *dest, float target, float halflife, float dt);
void SeekHalfLifeLim(float *dest, float target, float limit, float halflife, float dt);
void SeekHalfLifeNUVEC(struct nuvec_s *src, struct nuvec_s *target, float halflife, float dt);
void UpdateAnimPacket(struct CharacterModel *mod, struct anim_s *anim, float dt, float xz_distance);
float frandPN(void);

#ifndef POW2
#define POW2(x) ((x) * (x)) /* defined locally in terrain.c, used by move.c too */
#endif

/* vehicle.c helpers with no definition in the decompilation (radian sin/cos) */
float NuFsin(float a);
float NuFcos(float a);
/* translated functions that the decomp calls without a declaration but whose
 * original reads its arguments (found by scanning entry-register use) */
void DrawEnemyJeep(void *jeep);
void ProcessEnemyJeep(void *jeep);
void ProcessFireBoss(void *boss);
void ProcessJeepTrail(void *trail, int id);

#endif
