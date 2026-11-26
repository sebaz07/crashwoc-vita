/* Particle quads straight into the render batch (gx_gl.c), for renderpsdma
 * (dummyfunc.c): finished vertices instead of GX FIFO writes + block decode. */
#ifndef PORT_QUADS_H
#define PORT_QUADS_H

/* same layout as gx_gl.c's GLVertex (checked there) */
typedef struct {
    float pos[3];
    float nrm[3];
    unsigned char clr[4];
    float tex[2];
} PortQuadVtx;

/* With the state of GS_DrawQuadListBeginBlock set (direct f32 position, RGBA8
 * colour, f32 texcoord): room for maxq quads (4 vertices each, in GX_QUADS
 * order) in the current batch, or NULL when the GX calls are being recorded
 * (the caller then uses the FIFO path). Nothing else may touch GX until
 * port_gx_quads_end(nq), nq <= maxq. */
PortQuadVtx *port_gx_quads_begin(int maxq);
void port_gx_quads_end(int nq);
/* 1 when the last thing drawn was a port_gx_quads_end batch and no GX state
 * changed since (the GX state is still the one of that draw) */
int port_gx_quads_state_kept(void);

#endif
