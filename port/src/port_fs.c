/* File system: maps console paths onto the user's extracted game files.
 *
 * The game opens names such as "/levels\\b\\hub\\hub.nus" or "gfx\\licnin.s3".
 * Disc paths are case-insensitive, while the extracted tree keeps mixed case
 * (e.g. HUB.LGT). At startup the data root is indexed once; lookups are then
 * case-insensitive with '\\' and '/' treated alike. Write-mode opens (saves,
 * logs) go to a separate writable directory. */
#include "port.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef PORT_VITA
#define DEFAULT_DATA_ROOT "ux0:data/crashwoc/files"
#define DEFAULT_SAVE_ROOT "ux0:data/crashwoc/save"
#else
#define DEFAULT_DATA_ROOT "data"
#define DEFAULT_SAVE_ROOT "save"
#endif

static char g_root[512];
static char g_save[512];

struct fs_entry { char *key; char *path; };
static struct fs_entry *g_index;
static int g_index_count, g_index_cap;

static void lower_norm(char *dst, const char *src, size_t cap) {
    size_t n = 0;
    while (*src == '/' || *src == '\\') src++;
    for (; *src && n + 1 < cap; src++) {
        char c = *src == '\\' ? '/' : (char)tolower((unsigned char)*src);
        if (c == '/' && n > 0 && dst[n - 1] == '/') continue;
        dst[n++] = c;
    }
    /* the disc file system ignores trailing blanks ("pause.nus ") */
    while (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = 0;
}

static void index_add(const char *rel, const char *full) {
    char key[512];
    if (g_index_count == g_index_cap) {
        g_index_cap = g_index_cap ? g_index_cap * 2 : 4096;
        g_index = realloc(g_index, sizeof(*g_index) * g_index_cap);
    }
    lower_norm(key, rel, sizeof(key));
    g_index[g_index_count].key = strdup(key);
    g_index[g_index_count].path = strdup(full);
    g_index_count++;
}

static void index_dir(const char *full, const char *rel, int depth) {
    DIR *d;
    struct dirent *e;
    if (depth > 12) return;
    d = opendir(full);
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char f2[1024], r2[1024];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(f2, sizeof(f2), "%s/%s", full, e->d_name);
        snprintf(r2, sizeof(r2), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        if (stat(f2, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            index_dir(f2, r2, depth + 1);
        else
            index_add(r2, f2);
    }
    closedir(d);
}

static int cmp_entry(const void *a, const void *b) {
    return strcmp(((const struct fs_entry *)a)->key, ((const struct fs_entry *)b)->key);
}

void port_fs_init(void) {
    const char *r = getenv("CWOC_DATA");
    const char *s = getenv("CWOC_SAVE");
    snprintf(g_root, sizeof(g_root), "%s", r ? r : DEFAULT_DATA_ROOT);
    snprintf(g_save, sizeof(g_save), "%s", s ? s : DEFAULT_SAVE_ROOT);
    index_dir(g_root, "", 0);
    qsort(g_index, g_index_count, sizeof(*g_index), cmp_entry);
    port_log("fs: data root '%s' indexed %d files; save dir '%s'", g_root, g_index_count, g_save);
    if (g_index_count == 0)
        port_log("fs: WARNING no game files found. Copy the disc 'files' folder to %s", g_root);
#ifdef PORT_VITA
    mkdir(g_save, 0777);
#else
    mkdir(g_save, 0777);
#endif
}

const char *port_data_root(void) { return g_root; }
const char *port_save_dir(void) { return g_save; }

/* Returns the host path for a console-relative path, or NULL. */
const char *port_fs_resolve(const char *name) {
    char key[512];
    struct fs_entry probe, *hit;
    if (!name) return NULL;
    if ((name[0] == 'z' || name[0] == 'Z') && name[1] == ':') name += 2;
    lower_norm(key, name, sizeof(key));
    probe.key = key;
    hit = bsearch(&probe, g_index, g_index_count, sizeof(*g_index), cmp_entry);
    return hit ? hit->path : NULL;
}

uint64_t port_io_us, port_io_bytes; /* file read statistics (long-frame log) */
int port_io_files;
uint64_t port_prof_now(void);

/* fread with timing, used by NuFileRead */
size_t port_fread(void *buf, size_t sz, size_t n, FILE *f) {
    extern void port_gx_idle_for_load(void);
    uint64_t t0;
    port_gx_idle_for_load();
    t0 = port_prof_now();
    size_t r = fread(buf, sz, n, f);
    port_io_us += port_prof_now() - t0;
    port_io_bytes += r * sz;
    return r;
}

FILE *port_fopen(const char *name, const char *mode) {
    const char *p;
    FILE *f;
    if (mode && (mode[0] == 'w' || mode[0] == 'a')) {
        char key[512], full[1024];
        char *q;
        lower_norm(key, name, sizeof(key));
        for (q = key; *q; q++) if (*q == '/') *q = '_';
        snprintf(full, sizeof(full), "%s/%s", g_save, key);
        f = fopen(full, mode);
        port_io_files++;
        port_log("fs: open-write '%s' -> '%s' %s", name, full, f ? "ok" : "FAILED");
        return f;
    }
    p = port_fs_resolve(name);
    if (!p) {
        port_log("fs: missing '%s'", name);
        return NULL;
    }
    f = fopen(p, mode ? mode : "rb");
    port_log("fs: open '%s' -> '%s'%s", name, p, f ? "" : " FAILED");
    if (f) {
        /* fopen_NGC (system/gc/fs.c) left the size in filelength; NuFParOpen
         * reads it, so text scripts (texture animations .ats) parsed as empty */
        extern unsigned int filelength;
        fseek(f, 0, SEEK_END);
        filelength = (unsigned int)ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    return f;
}

/* ---- nufile.c companions (originally system/gc/fs.c) ---- */
extern FILE *fpointers[];

int GCFileSize(int handle) {
    FILE *f;
    long cur, end;
    if (handle <= 0 || handle > 10) return 0;
    f = fpointers[handle - 1];
    if (!f) return 0;
    cur = ftell(f);
    fseek(f, 0, SEEK_END);
    end = ftell(f);
    fseek(f, cur, SEEK_SET);
    return (int)end;
}

int NuFileWrite(int handle, const void *data, int size) {
    if (handle <= 0 || handle > 10 || !fpointers[handle - 1]) return 0;
    return (int)fwrite(data, 1, (size_t)size, fpointers[handle - 1]);
}

void GC_DiskErrorPoll(void) {}

/* DVD API: only referenced by leftovers; files go through port_fopen. */
void DVDInit(void) {}
int DVDOpen(char *name, void *info) { (void)name; (void)info; PORT_STUB(); return 0; }
int DVDReadAsyncPrio(void *fi, void *addr, int len, int off, void *cb, int prio) {
    (void)fi; (void)addr; (void)len; (void)off; (void)cb; (void)prio; PORT_STUB(); return 0;
}
int DVDGetDriveStatus(void) { return 0; }
