/* GameCube memory card (CARD API) on top of host files.
 *
 * The game's own save code (memcard.s: MC_Init, MC_Check, DoOpen, DoSave,
 * DoRead, saveloadASCallEachFrame, ...) is translated by ppc2c and talks to
 * the SDK CARD functions below. One card in slot A is emulated: every card
 * file is "<save dir>/card_a/<name>" (raw contents) plus "<name>.stat" (the
 * CARDStat fields set with CARDSetStatus). Slot B reports "no card".
 * Everything is synchronous; results use the SDK codes. */
#include "port.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef int32_t s32;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

#define CARD_RESULT_READY 0
#define CARD_RESULT_NOCARD (-3)
#define CARD_RESULT_NOFILE (-4)
#define CARD_RESULT_IOERROR (-5)
#define CARD_RESULT_EXIST (-7)
#define CARD_RESULT_NAMETOOLONG (-12)

#define CARD_SECTOR 0x2000
#define CARD_BLOCKS 59 /* the standard "Memory Card 59" */
#define CARD_FILENAME_MAX 32

typedef struct {
    s32 chan;
    s32 fileNo;
    s32 offset;
    s32 length;
    u16 iBlock;
    u16 pad;
} PortCARDFileInfo;

typedef struct {
    char fileName[CARD_FILENAME_MAX];
    u32 length;
    u32 time;
    u8 gameName[4];
    u8 company[2];
    u8 bannerFormat;
    u8 pad;
    u32 iconAddr;
    u16 iconFormat;
    u16 iconSpeed;
    u32 commentAddr;
    u32 offsetBanner;
    u32 offsetBannerTlut;
    u32 offsetIcon[8];
    u32 offsetIconTlut;
    u32 offsetData;
} PortCARDStat; /* 0x6C, same layout as the SDK's CARDStat */

typedef struct {
    u8 gameName[4];
    u8 company[2];
    u8 diskNumber;
    u8 gameVersion;
    u8 streaming;
    u8 streamingBufSize;
    u8 padding[22];
} PortDVDDiskID;

const char *port_save_dir(void);

static char g_open_name[CARD_FILENAME_MAX + 1]; /* fileNo 0 = the last opened/created file */

static void card_dir(char *out, size_t n) {
    snprintf(out, n, "%s/card_a", port_save_dir());
    mkdir(out, 0777);
}

static int card_path(char *out, size_t n, const char *name, const char *ext) {
    char dir[512];
    if (!name || strlen(name) > CARD_FILENAME_MAX) return 0;
    card_dir(dir, sizeof(dir));
    snprintf(out, n, "%s/%s%s", dir, name, ext);
    return 1;
}

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

/* ------------------------------------------------------------ card level */
void CARDInit(void) {}

s32 CARDProbeEx(s32 chan, s32 *memSize, s32 *sectorSize) {
    if (chan != 0) return CARD_RESULT_NOCARD;
    if (memSize) *memSize = 4; /* Mbit: 59 blocks */
    if (sectorSize) *sectorSize = CARD_SECTOR;
    return CARD_RESULT_READY;
}

s32 CARDMount(s32 chan, void *workArea, void *detachCallback) {
    char dir[512];
    (void)workArea;
    (void)detachCallback;
    if (chan != 0) return CARD_RESULT_NOCARD;
    card_dir(dir, sizeof(dir));
    return CARD_RESULT_READY;
}

s32 CARDUnmount(s32 chan) { return chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD; }
s32 CARDCheck(s32 chan) { return chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD; }

s32 CARDFormat(s32 chan) {
    /* the game offers to format a broken card: nothing to repair here */
    port_log("card: format requested (ignored)");
    return chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
}

s32 CARDFreeBlocks(s32 chan, s32 *byteNotUsed, s32 *filesNotUsed) {
    if (chan != 0) return CARD_RESULT_NOCARD;
    if (byteNotUsed) *byteNotUsed = CARD_BLOCKS * CARD_SECTOR;
    if (filesNotUsed) *filesNotUsed = 127;
    return CARD_RESULT_READY;
}

/* ------------------------------------------------------------ files */
static s32 open_info(s32 chan, const char *name, PortCARDFileInfo *fi, long len) {
    snprintf(g_open_name, sizeof(g_open_name), "%s", name);
    if (fi) {
        memset(fi, 0, sizeof(*fi));
        fi->chan = chan;
        fi->fileNo = 0;
        fi->length = (s32)len;
    }
    return CARD_RESULT_READY;
}

s32 CARDOpen(s32 chan, const char *fileName, PortCARDFileInfo *fileInfo) {
    char path[600];
    long len;
    if (chan != 0) return CARD_RESULT_NOCARD;
    if (!card_path(path, sizeof(path), fileName, "")) return CARD_RESULT_NAMETOOLONG;
    len = file_size(path);
    if (len < 0) return CARD_RESULT_NOFILE;
    return open_info(chan, fileName, fileInfo, len);
}

s32 CARDCreate(s32 chan, const char *fileName, u32 size, PortCARDFileInfo *fileInfo) {
    char path[600];
    FILE *f;
    void *z;
    if (chan != 0) return CARD_RESULT_NOCARD;
    if (!card_path(path, sizeof(path), fileName, "")) return CARD_RESULT_NAMETOOLONG;
    if (file_size(path) >= 0) return CARD_RESULT_EXIST;
    size = (size + CARD_SECTOR - 1) / CARD_SECTOR * CARD_SECTOR;
    f = fopen(path, "wb");
    if (!f) {
        port_log("card: cannot create %s (%s)", path, strerror(errno));
        return CARD_RESULT_IOERROR;
    }
    z = calloc(1, size);
    if (z) fwrite(z, 1, size, f);
    free(z);
    fclose(f);
    port_log("card: created %s (%u bytes)", fileName, size);
    return open_info(chan, fileName, fileInfo, (long)size);
}

s32 CARDClose(PortCARDFileInfo *fileInfo) {
    if (fileInfo) fileInfo->chan = -1;
    return CARD_RESULT_READY;
}

s32 CARDDelete(s32 chan, const char *fileName) {
    char path[600];
    if (chan != 0) return CARD_RESULT_NOCARD;
    if (!card_path(path, sizeof(path), fileName, "")) return CARD_RESULT_NAMETOOLONG;
    if (file_size(path) < 0) return CARD_RESULT_NOFILE;
    remove(path);
    if (card_path(path, sizeof(path), fileName, ".stat")) remove(path);
    port_log("card: deleted %s", fileName);
    return CARD_RESULT_READY;
}

static s32 rw(PortCARDFileInfo *fi, void *buf, s32 length, s32 offset, int write) {
    char path[600];
    FILE *f;
    size_t done;
    if (!fi || fi->chan != 0) return CARD_RESULT_NOCARD;
    if (!card_path(path, sizeof(path), g_open_name, "")) return CARD_RESULT_NOFILE;
    f = fopen(path, write ? "r+b" : "rb");
    if (!f) return CARD_RESULT_NOFILE;
    if (fseek(f, offset, SEEK_SET) != 0) {
        fclose(f);
        return CARD_RESULT_IOERROR;
    }
    if (write) {
        done = fwrite(buf, 1, (size_t)length, f);
    } else {
        done = fread(buf, 1, (size_t)length, f);
        if (done < (size_t)length) memset((u8 *)buf + done, 0, (size_t)length - done);
        done = (size_t)length;
    }
    if (write) fflush(f);
    fclose(f);
    if (done != (size_t)length) return CARD_RESULT_IOERROR;
    if (write) port_log("card: wrote %d bytes to %s", length, g_open_name);
    return CARD_RESULT_READY;
}

s32 CARDRead(PortCARDFileInfo *fileInfo, void *addr, s32 length, s32 offset) {
    return rw(fileInfo, addr, length, offset, 0);
}

s32 CARDWrite(PortCARDFileInfo *fileInfo, const void *addr, s32 length, s32 offset) {
    return rw(fileInfo, (void *)addr, length, offset, 1);
}

/* ------------------------------------------------------------ status */
/* banner / icon data offsets, as the SDK derives them from the formats */
static void update_offsets(PortCARDStat *st) {
    u32 offset = st->iconAddr;
    int i, tlut = 0;
    if (offset == 0xFFFFFFFFu) {
        st->bannerFormat = 0;
        st->iconFormat = 0;
        st->iconSpeed = 0;
        offset = 0;
    }
    switch (st->bannerFormat & 3) {
    case 1: /* C8 */
        st->offsetBanner = offset;
        offset += 96 * 32;
        st->offsetBannerTlut = offset;
        offset += 2 * 256;
        break;
    case 2: /* RGB5A3 */
        st->offsetBanner = offset;
        offset += 2 * 96 * 32;
        st->offsetBannerTlut = 0xFFFFFFFFu;
        break;
    default:
        st->offsetBanner = st->offsetBannerTlut = 0xFFFFFFFFu;
        break;
    }
    for (i = 0; i < 8; i++) {
        switch ((st->iconFormat >> (2 * i)) & 3) {
        case 1:
            st->offsetIcon[i] = offset;
            offset += 32 * 32;
            tlut = 1;
            break;
        case 2:
            st->offsetIcon[i] = offset;
            offset += 2 * 32 * 32;
            break;
        default:
            st->offsetIcon[i] = 0xFFFFFFFFu;
            break;
        }
    }
    if (tlut) {
        st->offsetIconTlut = offset;
        offset += 2 * 256;
    } else {
        st->offsetIconTlut = 0xFFFFFFFFu;
    }
    st->offsetData = offset;
}

PortDVDDiskID *DVDGetCurrentDiskID(void) {
    static PortDVDDiskID id = {{'G', 'C', 'B', 'E'}, {'7', 'D'}, 0, 0, 0, 0, {0}};
    return &id;
}

s32 CARDGetStatus(s32 chan, s32 fileNo, PortCARDStat *stat) {
    char path[600];
    FILE *f;
    PortCARDStat st;
    long len;
    (void)fileNo;
    if (chan != 0) return CARD_RESULT_NOCARD;
    if (!card_path(path, sizeof(path), g_open_name, "")) return CARD_RESULT_NOFILE;
    len = file_size(path);
    if (len < 0) return CARD_RESULT_NOFILE;
    memset(&st, 0, sizeof(st));
    st.iconAddr = st.commentAddr = 0xFFFFFFFFu;
    if (card_path(path, sizeof(path), g_open_name, ".stat") && (f = fopen(path, "rb")) != NULL) {
        if (fread(&st, 1, sizeof(st), f) != sizeof(st)) port_log("card: short status file for %s", g_open_name);
        fclose(f);
    }
    snprintf(st.fileName, sizeof(st.fileName), "%s", g_open_name);
    st.length = (u32)len;
    memcpy(st.gameName, DVDGetCurrentDiskID()->gameName, 4);
    memcpy(st.company, DVDGetCurrentDiskID()->company, 2);
    update_offsets(&st);
    *stat = st;
    return CARD_RESULT_READY;
}

s32 CARDSetStatus(s32 chan, s32 fileNo, PortCARDStat *stat) {
    char path[600];
    FILE *f;
    PortCARDStat st;
    (void)fileNo;
    if (chan != 0) return CARD_RESULT_NOCARD;
    if (!stat || !card_path(path, sizeof(path), g_open_name, ".stat")) return CARD_RESULT_NOFILE;
    st = *stat;
    if (st.iconAddr == 0 && st.iconFormat == 0) {
        /* what memcard.s LoadIconC8 puts in the status template (the port
         * skips it): one C8 icon after a 0x40-byte comment, no banner */
        st.iconAddr = 0x40;
        st.commentAddr = 0;
        st.iconFormat = (u16)((st.iconFormat & 0xFFFC) | 1);
        st.iconSpeed = (u16)((st.iconSpeed | 3) & ~0xC);
        st.bannerFormat &= (u8)~7;
    }
    st.time = (u32)time(NULL);
    update_offsets(&st);
    *stat = st;
    f = fopen(path, "wb");
    if (!f) return CARD_RESULT_IOERROR;
    fwrite(&st, 1, sizeof(st), f);
    fclose(f);
    return CARD_RESULT_READY;
}

/* ------------------------------------------------------------ misc SDK */
void DCInvalidateRange(void *addr, u32 n) {
    (void)addr;
    (void)n;
}

typedef struct {
    s32 sec, min, hour, mday, mon, year, wday, yday, msec, usec;
} PortOSCalendarTime;

/* UpdateTime (save time stamps) asks for the calendar time of OSGetTime():
 * the port's timebase starts at boot, so use the host clock instead. */
void OSTicksToCalendarTime(long long ticks, PortOSCalendarTime *td) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    (void)ticks;
    memset(td, 0, sizeof(*td));
    if (!t) return;
    td->sec = t->tm_sec;
    td->min = t->tm_min;
    td->hour = t->tm_hour;
    td->mday = t->tm_mday;
    td->mon = t->tm_mon;
    td->year = t->tm_year + 1900;
    td->wday = t->tm_wday;
    td->yday = t->tm_yday;
}

/* memcard.s LoadIconC8 reads the card icon from the disc with the DVD API;
 * the port does not show card icons (tools/ppc2c.py HOOKS). */
void port_hook_LoadIconC8(void) {}
