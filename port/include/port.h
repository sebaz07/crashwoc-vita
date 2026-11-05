/* Shared declarations for the native port platform layer (port/src). */
#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include <stddef.h>

void port_log_init(void);
void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void port_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
/* Logs the first call of a not-yet-implemented function. */
void port_stub_hit(const char *name);
#define PORT_STUB() port_stub_hit(__func__)

void port_platform_init(int argc, char **argv);
const char *port_data_root(void);

/* Byte-order helpers: GameCube data is big-endian. */
static inline uint16_t port_be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t port_be32(uint32_t v) { return __builtin_bswap32(v); }
static inline float port_befloat(float f) { union { float f; uint32_t u; } x; x.f = f; x.u = __builtin_bswap32(x.u); return x.f; }

#endif
