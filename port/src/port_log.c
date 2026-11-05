/* Logging for the native port: stderr plus a log file next to the data root. */
#include "port.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_log;

void port_log_init(void) {
#ifdef PORT_VITA
    g_log = fopen("ux0:data/crashwoc/crashwoc.log", "w");
#else
    const char *p = getenv("CWOC_LOG");
    g_log = fopen(p ? p : "crashwoc.log", "w");
#endif
}

static void vlog(const char *prefix, const char *fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    fprintf(stderr, "%s%s\n", prefix, buf);
    if (g_log) {
        fprintf(g_log, "%s%s\n", prefix, buf);
        fflush(g_log);
    }
}

void port_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("", fmt, ap);
    va_end(ap);
}

void port_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("FATAL: ", fmt, ap);
    va_end(ap);
    exit(1);
}

#define MAX_STUBS 512
static const char *g_stub_seen[MAX_STUBS];
static int g_stub_count;

void port_stub_hit(const char *name) {
    int i;
    for (i = 0; i < g_stub_count; i++)
        if (g_stub_seen[i] == name || strcmp(g_stub_seen[i], name) == 0)
            return;
    if (g_stub_count < MAX_STUBS)
        g_stub_seen[g_stub_count++] = name;
    port_log("stub: %s", name);
}
