/* OpenGL includes for the GX emulation: desktop GL 2.1 (PC) or vitaGL (Vita). */
#ifndef PORT_GL_H
#define PORT_GL_H

#ifdef PORT_VITA
#include <vitaGL.h>
#define PORT_GLSL_HEADER "precision mediump float;\n"
#else
#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengl.h>
#include <GL/glext.h>
#define PORT_GLSL_HEADER "#version 120\n"
#endif

void port_gx_init(void);
void port_gx_begin_frame(void);
void port_gx_set_drawable(int win_w, int win_h, int off_x, int off_y, int draw_w, int draw_h);
void port_gx_stats_draw(int nverts);
void PSMTXIdentity_port(void);

/* submit the pending merged batch (before swaps, read-backs, clears) */
void port_gx_flush(void);
int port_gx_take_draw_calls(void);
/* end of frame: queue the present (screenshot + swap run on the render thread) */
void port_gx_present(int frame);
void port_gx_finish(void);
int port_gx_render_threaded(void);

#endif
