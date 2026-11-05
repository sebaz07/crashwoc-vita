/* Force-included before every translation unit of the native port.
 * The decompiled code relies on implicit declarations in many places. That is
 * harmless on PowerPC (ints/floats use separate register files) but breaks the
 * ARM/x86 calling conventions for float arguments/returns. Game translation
 * units therefore see the full game headers up front, plus explicit prototypes
 * for functions that are otherwise only implicitly declared. */
#ifndef PORT_PRELUDE_H
#define PORT_PRELUDE_H

#include <math.h>
#include <stdlib.h>

/* big-endian file data helpers (port/src/port_endian.c) */
void port_swap32(void *p, int count);
void port_swap16(void *p, int count);
void port_swap_layout(void *p, const char *layout, int count);
void port_fix_nuvtx_tc1(void *v, int n);
void port_fix_nuinstances(void *p, int n);
void port_fix_nuinstanims(void *p, int n);
void port_fix_nufspecials(void *p, int n);
void port_fix_numtl(void *p);
void port_fix_nutexanims(void *p, int n);
unsigned int port_bitfields_be_to_le(unsigned int be, const unsigned char *widths, int n, int unit_bits);
void port_visit_reset(void);
int port_visit_once(const void *p);

#ifndef PORT_PLATFORM_SOURCE
/* Game code opens console paths ("/levels\\b\\hub\\hub.nus"); route stdio file
 * opens through the port's data-root aware implementation. */
#define fopen port_fopen
#endif

#ifdef PORT_SYS_PRELUDE
#include "port_sysprotos.h"
#endif

#ifdef PORT_FULL_PRELUDE
#include "nu.h"
#include "gamecode/main.h"
#include "port_protos.h"
#endif
#endif
