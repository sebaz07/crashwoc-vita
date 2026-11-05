/* Prototypes for engine/system translation units (src/system, src/nucore, ...).
 * Only functions that those units call without a declaration and whose float
 * arguments or float/double results would otherwise be passed incorrectly on
 * ARM/x86 (PowerPC tolerated the implicit declarations). */
#ifndef PORT_SYSPROTOS_H
#define PORT_SYSPROTOS_H
#include <math.h>
#include <stdlib.h>
struct _GXColor;
struct _GXRenderModeObj;
void GXSetViewport(float left, float top, float wd, float ht, float nearz, float farz);
void GXSetFog(int type, float startz, float endz, float nearz, float farz, struct _GXColor color);
void GXSetZMode(unsigned char compare_enable, int func, unsigned char update_enable);
void GXSetBlendMode(int type, int src_factor, int dst_factor, int op);
void GXCopyDisp(void *dest, unsigned char clear);
void GXPosition3f32(float x, float y, float z);
void GXDrawDone(void);
void GXPixModeSync(void);
void GXInvalidateTexAll(void);
void C_MTXPerspective(float mtx[4][4], float fovY, float aspect, float n, float f);
void C_MTXOrtho(float mtx[4][4], float t, float b, float l, float r, float n, float f);
void DEMOSetupScrnSpc(int width, int height, float depth);
void DEMOBeforeRender(void);
void DEMODoneRender(void);
void DEMOSwapBuffers(void);
void *DEMOGetCurrentBuffer(void);
struct _GXRenderModeObj *DEMOGetRenderModeObj(void);
double TimerGetFPS(void);
void GS_RenderClear(unsigned long Flags, unsigned long Color, float Z, unsigned long Stencil);
void CARDInit(void);
void GBA_Init(void);
#endif
