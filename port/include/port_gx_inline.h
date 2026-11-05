/* Inline GX vertex writes for the hot drawing loops (src/system/gs/gsprim.c).
 * Same contract as the out-of-line versions in port/src/gx_gl.c: data is
 * appended to the emulated FIFO in native byte order (direct colours keep
 * GameCube byte order) and the block is drawn once all its vertices arrived. */
#ifndef PORT_GX_INLINE_H
#define PORT_GX_INLINE_H
#include <string.h>

extern unsigned char *port_gx_wp;   /* FIFO write pointer */
extern unsigned char *port_gx_end;  /* end of the current GXBegin block */
void port_gx_fifo_end(void);        /* block complete (or data outside a block) */

#define PORT_GX_PUT(p, n) do { memcpy(port_gx_wp, (p), (n)); port_gx_wp += (n); \
    if (port_gx_wp >= port_gx_end) port_gx_fifo_end(); } while (0)

static inline void port_gxi_Position3f32(const float x, const float y, const float z) {
    float v[3] = { x, y, z };
    PORT_GX_PUT(v, 12);
}
static inline void port_gxi_Normal3f32(const float x, const float y, const float z) {
    float v[3] = { x, y, z };
    PORT_GX_PUT(v, 12);
}
static inline void port_gxi_TexCoord2f32(const float s, const float t) {
    float v[2] = { s, t };
    PORT_GX_PUT(v, 8);
}
static inline void port_gxi_Color4u8(const unsigned char r, const unsigned char g, const unsigned char b, const unsigned char a) {
    unsigned char v[4] = { r, g, b, a };
    PORT_GX_PUT(v, 4);
}
static inline void port_gxi_Color1u32(const unsigned int c) {
    unsigned char v[4] = { (unsigned char)(c >> 24), (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
    PORT_GX_PUT(v, 4);
}
static inline void port_gxi_Color1u16(const unsigned short c) {
    unsigned char v[2] = { (unsigned char)(c >> 8), (unsigned char)c };
    PORT_GX_PUT(v, 2);
}
static inline void port_gxi_Color3f32(const float r, const float g, const float b) {
    unsigned char v[3] = { (unsigned char)(r * 255.0f), (unsigned char)(g * 255.0f), (unsigned char)(b * 255.0f) };
    PORT_GX_PUT(v, 3);
}
static inline void port_gxi_Position1x16(const unsigned short i) { PORT_GX_PUT(&i, 2); }
static inline void port_gxi_Normal1x16(const unsigned short i) { PORT_GX_PUT(&i, 2); }
static inline void port_gxi_TexCoord1x16(const unsigned short i) { PORT_GX_PUT(&i, 2); }

/* the prelude declares the out-of-line versions: rename the call sites */
#define GXPosition3f32 port_gxi_Position3f32
#define GXNormal3f32 port_gxi_Normal3f32
#define GXTexCoord2f32 port_gxi_TexCoord2f32
#define GXColor4u8 port_gxi_Color4u8
#define GXColor1u32 port_gxi_Color1u32
#define GXColor1u16 port_gxi_Color1u16
#define GXColor3f32 port_gxi_Color3f32
#define GXPosition1x16 port_gxi_Position1x16
#define GXNormal1x16 port_gxi_Normal1x16
#define GXTexCoord1x16 port_gxi_TexCoord1x16

#endif
