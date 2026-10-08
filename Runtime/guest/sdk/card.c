/*
 * Memory cards: the CARD API over a card image the host keeps (gcn_host_card_*, one file
 * per slot; slot B stays empty unless the host provides it).
 *
 * The image is a 16 Mbit card (256 blocks of 8 KB). It is not the console's on-card
 * format: block 0 holds a small header, block 1 the directory (127 CARDDir entries,
 * exactly as the SDK keeps them), and every file is one contiguous run of blocks from
 * block 5 on. Directory entries are written big-endian like everything the game sees.
 *
 * Operations finish at once. The asynchronous variants call their callback at the next
 * scheduling point (gcn_card_poll), as completions on the console arrive later.
 */
#include <dolphin.h>
#include <dolphin/card.h>
#include <string.h>

#include "gcn_guest.h"
#include "gcn_sdk.h"

#define SECTOR 0x2000u
#define BLOCKS 256u
#define FIRST_DATA_BLOCK 5u
#define MAX_FILES 127
#define MAGIC 0x47434E43u /* 'GCNC' */

typedef struct
{
    u32 magic;
    u32 version;
    u32 serialHi, serialLo;
    u32 formatTime;
} CardHeader;

typedef struct
{
    BOOL mounted;
    s32 result;
    s32 xferred; /* bytes of the last read / write (CARDGetXferredBytes) */
    CardHeader hdr;
    CARDDir dir[MAX_FILES];
} Card;

static Card sCard[2];

/* deferred completions of the asynchronous calls */
#define MAX_PENDING 16
static struct
{
    CARDCallback cb;
    s32 chan, result;
} sPending[MAX_PENDING];
static int sPendingCount;

static s32 finish(s32 chan, s32 result)
{
    if (chan >= 0 && chan < 2) sCard[chan].result = result;
    return result;
}

static s32 later(s32 chan, s32 result, CARDCallback cb)
{
    finish(chan, result);
    if (cb && sPendingCount < MAX_PENDING)
    {
        sPending[sPendingCount].cb = cb;
        sPending[sPendingCount].chan = chan;
        sPending[sPendingCount].result = result;
        sPendingCount++;
    }
    /* the call itself starts fine; errors arrive through the callback */
    return cb ? CARD_RESULT_READY : result;
}

int gcn_card_poll(void)
{
    int n = 0;

    while (sPendingCount)
    {
        CARDCallback cb = sPending[0].cb;
        s32 chan = sPending[0].chan, result = sPending[0].result;
        int i;

        for (i = 1; i < sPendingCount; i++) sPending[i - 1] = sPending[i];
        sPendingCount--;
        cb(chan, result);
        n++;
    }
    return n;
}

static BOOL present(s32 chan)
{
    return chan >= 0 && chan < 2 && gcn_host_card_size((u32)chan) >= BLOCKS * SECTOR;
}

static BOOL io(s32 chan, void *buf, u32 offset, u32 size, BOOL write)
{
    return gcn_host_card_io((u32)chan, buf, offset, size, write) == size;
}

static u32 now_seconds(void)
{
    /* the card's clock counts from 2000-01-01, like OSTime */
    return (u32)OSTicksToSeconds(OSGetTime());
}

static s32 save_dir(s32 chan)
{
    if (!io(chan, &sCard[chan].hdr, 0, sizeof(CardHeader), TRUE)) return CARD_RESULT_IOERROR;
    if (!io(chan, sCard[chan].dir, SECTOR, sizeof(sCard[chan].dir), TRUE)) return CARD_RESULT_IOERROR;
    return CARD_RESULT_READY;
}

static s32 format(s32 chan)
{
    Card *c = &sCard[chan];
    u32 t = (u32)OSGetTime();

    memset(&c->hdr, 0, sizeof(c->hdr));
    memset(c->dir, 0xFF, sizeof(c->dir));
    c->hdr.magic = MAGIC;
    c->hdr.version = 1;
    c->hdr.serialHi = t ^ 0x5A5A1234u;
    c->hdr.serialLo = (t << 7) ^ (u32)OSGetTick();
    c->hdr.formatTime = now_seconds();
    return save_dir(chan);
}

static void game_ids(u8 *game, u8 *company)
{
    DVDDiskID *id = DVDGetCurrentDiskID();
    memcpy(game, id->gameName, 4);
    memcpy(company, id->company, 2);
}

static BOOL used(const CARDDir *d) { return d->gameName[0] != 0xFF; }

static int find(s32 chan, const char *name)
{
    u8 game[4], company[2];
    int i;

    game_ids(game, company);
    for (i = 0; i < MAX_FILES; i++)
    {
        const CARDDir *d = &sCard[chan].dir[i];
        if (used(d) && !memcmp(d->gameName, game, 4) && !memcmp(d->company, company, 2) &&
            !strncmp((const char *)d->fileName, name, CARD_FILENAME_MAX))
            return i;
    }
    return -1;
}

static s32 ready(s32 chan)
{
    if (chan < 0 || chan > 1) return CARD_RESULT_FATAL_ERROR;
    if (!present(chan)) return CARD_RESULT_NOCARD;
    if (!sCard[chan].mounted) return CARD_RESULT_NOCARD;
    return CARD_RESULT_READY;
}

void CARDInit(void) {}

s32 CARDProbeEx(s32 chan, s32 *memSize, s32 *sectorSize)
{
    if (!present(chan)) return CARD_RESULT_NOCARD;
    if (memSize) *memSize = 16;
    if (sectorSize) *sectorSize = SECTOR;
    return CARD_RESULT_READY;
}

BOOL CARDProbe(s32 chan) { return present(chan); }

s32 CARDMount(s32 chan, void *workArea, CARDCallback detachCallback)
{
    Card *c;

    (void)workArea;
    (void)detachCallback;
    if (chan < 0 || chan > 1) return CARD_RESULT_FATAL_ERROR;
    if (!present(chan)) return finish(chan, CARD_RESULT_NOCARD);
    c = &sCard[chan];
    if (!io(chan, &c->hdr, 0, sizeof(CardHeader), FALSE)) return finish(chan, CARD_RESULT_IOERROR);
    if (c->hdr.magic != MAGIC)
    {
        /* a new (empty) image: format it, as a fresh card from the box */
        if (format(chan) != CARD_RESULT_READY) return finish(chan, CARD_RESULT_IOERROR);
    }
    else if (!io(chan, c->dir, SECTOR, sizeof(c->dir), FALSE))
    {
        return finish(chan, CARD_RESULT_IOERROR);
    }
    c->mounted = TRUE;
    return finish(chan, CARD_RESULT_READY);
}

s32 CARDMountAsync(s32 chan, void *workArea, CARDCallback detachCallback, CARDCallback attachCallback)
{
    return later(chan, CARDMount(chan, workArea, detachCallback), attachCallback);
}

s32 CARDUnmount(s32 chan)
{
    if (chan < 0 || chan > 1) return CARD_RESULT_FATAL_ERROR;
    if (!sCard[chan].mounted) return finish(chan, CARD_RESULT_NOCARD);
    sCard[chan].mounted = FALSE;
    return finish(chan, CARD_RESULT_READY);
}

s32 CARDCheck(s32 chan) { return finish(chan, ready(chan)); }

s32 CARDCheckAsync(s32 chan, CARDCallback callback) { return later(chan, ready(chan), callback); }

s32 CARDCheckExAsync(s32 chan, s32 *xferBytes, CARDCallback callback)
{
    if (xferBytes) *xferBytes = 0;
    return later(chan, ready(chan), callback);
}

s32 CARDFormat(s32 chan)
{
    s32 r = ready(chan);

    if (r == CARD_RESULT_READY) r = format(chan);
    return finish(chan, r);
}

s32 CARDFormatAsync(s32 chan, CARDCallback callback) { return later(chan, CARDFormat(chan), callback); }

s32 CARDGetSerialNo(s32 chan, u64 *serialNo)
{
    s32 r = ready(chan);

    if (r != CARD_RESULT_READY) return finish(chan, r);
    *serialNo = ((u64)sCard[chan].hdr.serialHi << 32) | sCard[chan].hdr.serialLo;
    return finish(chan, CARD_RESULT_READY);
}

s32 CARDFreeBlocks(s32 chan, s32 *byteNotUsed, s32 *filesNotUsed)
{
    s32 r = ready(chan);
    u32 usedBlocks = 0;
    int i, files = 0;

    if (r != CARD_RESULT_READY) return finish(chan, r);
    for (i = 0; i < MAX_FILES; i++)
    {
        if (used(&sCard[chan].dir[i])) usedBlocks += sCard[chan].dir[i].length;
        else files++;
    }
    if (byteNotUsed) *byteNotUsed = (s32)((BLOCKS - FIRST_DATA_BLOCK - usedBlocks) * SECTOR);
    if (filesNotUsed) *filesNotUsed = files;
    return finish(chan, CARD_RESULT_READY);
}

s32 CARDGetSectorSize(s32 chan, u32 *size)
{
    s32 r = ready(chan);

    if (r == CARD_RESULT_READY) *size = SECTOR;
    return finish(chan, r);
}

s32 CARDGetResultCode(s32 chan) { return chan >= 0 && chan < 2 ? sCard[chan].result : CARD_RESULT_FATAL_ERROR; }

/* first run of free blocks long enough */
static int allocate(s32 chan, u32 blocks)
{
    u32 start = FIRST_DATA_BLOCK;

    while (start + blocks <= BLOCKS)
    {
        int i, clash = -1;
        for (i = 0; i < MAX_FILES; i++)
        {
            const CARDDir *d = &sCard[chan].dir[i];
            if (used(d) && d->startBlock < start + blocks && start < (u32)d->startBlock + d->length) clash = i;
        }
        if (clash < 0) return (int)start;
        start = (u32)sCard[chan].dir[clash].startBlock + sCard[chan].dir[clash].length;
    }
    return -1;
}

static void open_info(s32 chan, int fileNo, CARDFileInfo *fi)
{
    fi->chan = chan;
    fi->fileNo = fileNo;
    fi->offset = 0;
    fi->length = (s32)(sCard[chan].dir[fileNo].length * SECTOR);
    fi->iBlock = sCard[chan].dir[fileNo].startBlock;
}

s32 CARDCreate(s32 chan, const char *fileName, u32 size, CARDFileInfo *fileInfo)
{
    s32 r = ready(chan);
    u32 blocks = (size + SECTOR - 1) / SECTOR;
    int i, slot = -1, start;
    CARDDir *d;

    if (r != CARD_RESULT_READY) return finish(chan, r);
    if (strlen(fileName) > CARD_FILENAME_MAX) return finish(chan, CARD_RESULT_NAMETOOLONG);
    if (!size || size % SECTOR) return finish(chan, CARD_RESULT_FATAL_ERROR);
    if (find(chan, fileName) >= 0) return finish(chan, CARD_RESULT_EXIST);
    for (i = 0; i < MAX_FILES && slot < 0; i++)
        if (!used(&sCard[chan].dir[i])) slot = i;
    if (slot < 0) return finish(chan, CARD_RESULT_NOENT);
    start = allocate(chan, blocks);
    if (start < 0) return finish(chan, CARD_RESULT_INSSPACE);
    d = &sCard[chan].dir[slot];
    memset(d, 0, sizeof(*d));
    game_ids(d->gameName, d->company);
    d->_padding0 = 0xFF;
    strncpy((char *)d->fileName, fileName, CARD_FILENAME_MAX);
    d->time = now_seconds();
    d->iconAddr = 0xFFFFFFFF;
    d->commentAddr = 0xFFFFFFFF;
    d->permission = CARD_ATTR_PUBLIC;
    d->startBlock = (u16)start;
    d->length = (u16)blocks;
    d->_padding1[0] = d->_padding1[1] = 0xFF;
    r = save_dir(chan);
    if (r == CARD_RESULT_READY && fileInfo) open_info(chan, slot, fileInfo);
    return finish(chan, r);
}

s32 CARDCreateAsync(s32 chan, const char *fileName, u32 size, CARDFileInfo *fileInfo, CARDCallback callback)
{
    return later(chan, CARDCreate(chan, fileName, size, fileInfo), callback);
}

s32 CARDOpen(s32 chan, const char *fileName, CARDFileInfo *fileInfo)
{
    s32 r = ready(chan);
    int i;

    if (r != CARD_RESULT_READY) return finish(chan, r);
    i = find(chan, fileName);
    if (i < 0) return finish(chan, CARD_RESULT_NOFILE);
    open_info(chan, i, fileInfo);
    return finish(chan, CARD_RESULT_READY);
}

s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo *fileInfo)
{
    s32 r = ready(chan);

    if (r != CARD_RESULT_READY) return finish(chan, r);
    if (fileNo < 0 || fileNo >= MAX_FILES || !used(&sCard[chan].dir[fileNo])) return finish(chan, CARD_RESULT_NOFILE);
    open_info(chan, fileNo, fileInfo);
    return finish(chan, CARD_RESULT_READY);
}

s32 CARDClose(CARDFileInfo *fileInfo)
{
    fileInfo->chan = -1;
    return CARD_RESULT_READY;
}

static s32 transfer(CARDFileInfo *fi, void *buf, s32 length, s32 offset, BOOL write)
{
    s32 chan = fi->chan, r = ready(chan);
    const CARDDir *d;

    if (r != CARD_RESULT_READY) return finish(chan, r);
    if (fi->fileNo < 0 || fi->fileNo >= MAX_FILES) return finish(chan, CARD_RESULT_FATAL_ERROR);
    d = &sCard[chan].dir[fi->fileNo];
    if (!used(d)) return finish(chan, CARD_RESULT_NOFILE);
    if (offset < 0 || length < 0 || (u32)(offset + length) > d->length * SECTOR) return finish(chan, CARD_RESULT_LIMIT);
    if (!io(chan, buf, d->startBlock * SECTOR + (u32)offset, (u32)length, write)) return finish(chan, CARD_RESULT_IOERROR);
    sCard[chan].xferred = length;
    if (write)
    {
        sCard[chan].dir[fi->fileNo].time = now_seconds();
        r = save_dir(chan);
        if (r != CARD_RESULT_READY) return finish(chan, r);
    }
    fi->offset = offset + length;
    return finish(chan, CARD_RESULT_READY);
}

s32 CARDRead(CARDFileInfo *fileInfo, void *buf, s32 length, s32 offset)
{
    return transfer(fileInfo, buf, length, offset, FALSE);
}

s32 CARDReadAsync(CARDFileInfo *fileInfo, void *buf, s32 length, s32 offset, CARDCallback callback)
{
    return later(fileInfo->chan, transfer(fileInfo, buf, length, offset, FALSE), callback);
}

s32 CARDWrite(CARDFileInfo *fileInfo, void *buf, s32 length, s32 offset)
{
    return transfer(fileInfo, buf, length, offset, TRUE);
}

s32 CARDWriteAsync(CARDFileInfo *fileInfo, void *buf, s32 length, s32 offset, CARDCallback callback)
{
    return later(fileInfo->chan, transfer(fileInfo, buf, length, offset, TRUE), callback);
}

s32 CARDDelete(s32 chan, const char *fileName)
{
    s32 r = ready(chan);
    int i;

    if (r != CARD_RESULT_READY) return finish(chan, r);
    i = find(chan, fileName);
    if (i < 0) return finish(chan, CARD_RESULT_NOFILE);
    memset(&sCard[chan].dir[i], 0xFF, sizeof(CARDDir));
    return finish(chan, save_dir(chan));
}

s32 CARDDeleteAsync(s32 chan, const char *fileName, CARDCallback callback)
{
    return later(chan, CARDDelete(chan, fileName), callback);
}

s32 CARDFastDelete(s32 chan, s32 fileNo)
{
    s32 r = ready(chan);

    if (r != CARD_RESULT_READY) return finish(chan, r);
    if (fileNo < 0 || fileNo >= MAX_FILES || !used(&sCard[chan].dir[fileNo])) return finish(chan, CARD_RESULT_NOFILE);
    memset(&sCard[chan].dir[fileNo], 0xFF, sizeof(CARDDir));
    return finish(chan, save_dir(chan));
}

s32 CARDFastDeleteAsync(s32 chan, s32 fileNo, CARDCallback callback)
{
    return later(chan, CARDFastDelete(chan, fileNo), callback);
}

s32 CARDRename(s32 chan, const char *oldName, const char *newName)
{
    s32 r = ready(chan);
    CARDDir *d;
    int i;

    if (r != CARD_RESULT_READY) return finish(chan, r);
    if (!oldName[0] || !newName[0] || (u8)oldName[0] == 0xFF || (u8)newName[0] == 0xFF)
        return finish(chan, CARD_RESULT_FATAL_ERROR);
    if (strlen(oldName) > CARD_FILENAME_MAX || strlen(newName) > CARD_FILENAME_MAX)
        return finish(chan, CARD_RESULT_NAMETOOLONG);
    i = find(chan, oldName);
    if (i < 0) return finish(chan, CARD_RESULT_NOFILE);
    if (find(chan, newName) >= 0) return finish(chan, CARD_RESULT_EXIST);
    d = &sCard[chan].dir[i];
    strncpy((char *)d->fileName, newName, CARD_FILENAME_MAX);
    d->time = now_seconds();
    return finish(chan, save_dir(chan));
}

s32 CARDRenameAsync(s32 chan, const char *oldName, const char *newName, CARDCallback callback)
{
    return later(chan, CARDRename(chan, oldName, newName), callback);
}

/* The directory entry itself (CARDNet: attributes go through these). Another game's files
 * can be read when they are public, as the SDK's __CARDAccess / __CARDIsPublic allow. */
s32 __CARDGetStatusEx(s32 chan, s32 fileNo, CARDDir *dirent);
s32 __CARDSetStatusEx(s32 chan, s32 fileNo, CARDDir *dirent);
s32 __CARDSetStatusExAsync(s32 chan, s32 fileNo, CARDDir *dirent, CARDCallback callback);

static s32 access_entry(s32 chan, s32 fileNo, BOOL write)
{
    u8 game[4], company[2];
    const CARDDir *d;

    if (fileNo < 0 || fileNo >= MAX_FILES) return CARD_RESULT_FATAL_ERROR;
    d = &sCard[chan].dir[fileNo];
    if (!used(d)) return CARD_RESULT_NOFILE;
    game_ids(game, company);
    if (!memcmp(d->gameName, game, 4) && !memcmp(d->company, company, 2)) return CARD_RESULT_READY;
    return !write && (d->permission & CARD_ATTR_PUBLIC) ? CARD_RESULT_READY : CARD_RESULT_NOPERM;
}

s32 __CARDGetStatusEx(s32 chan, s32 fileNo, CARDDir *dirent)
{
    s32 r = ready(chan);

    if (r == CARD_RESULT_READY) r = access_entry(chan, fileNo, FALSE);
    if (r == CARD_RESULT_READY) memcpy(dirent, &sCard[chan].dir[fileNo], sizeof(CARDDir));
    return finish(chan, r);
}

s32 __CARDSetStatusEx(s32 chan, s32 fileNo, CARDDir *dirent)
{
    s32 r = ready(chan);
    CARDDir *d;
    int i;

    if (r == CARD_RESULT_READY) r = access_entry(chan, fileNo, TRUE);
    if (r != CARD_RESULT_READY) return finish(chan, r);
    if (dirent->fileName[0] == 0xFF || dirent->fileName[0] == 0) return finish(chan, CARD_RESULT_FATAL_ERROR);
    d = &sCard[chan].dir[fileNo];
    for (i = 0; i < MAX_FILES; i++)
    {
        const CARDDir *o = &sCard[chan].dir[i];
        if (i != fileNo && used(o) && !memcmp(o->gameName, dirent->gameName, 4) &&
            !memcmp(o->company, dirent->company, 2) &&
            !strncmp((const char *)o->fileName, (const char *)dirent->fileName, CARD_FILENAME_MAX))
            return finish(chan, CARD_RESULT_EXIST);
    }
    /* where the file's blocks are stays this card's business */
    {
        u16 start = d->startBlock, length = d->length;
        memcpy(d, dirent, sizeof(CARDDir));
        d->startBlock = start;
        d->length = length;
    }
    return finish(chan, save_dir(chan));
}

s32 __CARDSetStatusExAsync(s32 chan, s32 fileNo, CARDDir *dirent, CARDCallback callback)
{
    return later(chan, __CARDSetStatusEx(chan, fileNo, dirent), callback);
}

s32 CARDGetAttributes(s32 chan, s32 fileNo, u8 *attr)
{
    CARDDir d;
    s32 r = __CARDGetStatusEx(chan, fileNo, &d);

    if (r == CARD_RESULT_READY) *attr = d.permission;
    return r;
}

s32 CARDSetAttributes(s32 chan, s32 fileNo, u8 attr)
{
    CARDDir d;
    s32 r = __CARDGetStatusEx(chan, fileNo, &d);

    if (r != CARD_RESULT_READY) return r;
    d.permission = attr;
    return __CARDSetStatusEx(chan, fileNo, &d);
}

s32 CARDSetAttributesAsync(s32 chan, s32 fileNo, u8 attr, CARDCallback callback)
{
    return later(chan, CARDSetAttributes(chan, fileNo, attr), callback);
}

s32 CARDGetXferredBytes(s32 chan) { return chan >= 0 && chan < 2 ? sCard[chan].xferred : 0; }

s32 CARDGetEncoding(s32 chan, u16 *encode)
{
    s32 r = ready(chan);

    if (r == CARD_RESULT_READY) *encode = 0; /* ANSI (the image is formatted that way) */
    return finish(chan, r);
}

s32 CARDGetMemSize(s32 chan, u16 *size)
{
    s32 r = ready(chan);

    if (r == CARD_RESULT_READY) *size = (u16)(BLOCKS * SECTOR >> 17); /* in Mbit */
    return finish(chan, r);
}

s32 CARDGetCurrentMode(s32 chan, u32 *mode)
{
    s32 r = ready(chan);

    if (r == CARD_RESULT_READY) *mode = 0; /* CARD_MODE_NORMAL */
    return finish(chan, r);
}

static BOOL sFastMode;
BOOL CARDSetFastMode(BOOL enable)
{
    BOOL was = sFastMode;
    sFastMode = enable;
    return was;
}
BOOL CARDGetFastMode(void) { return sFastMode; }

/* where the banner, icons and data sit in the file (the SDK's __CARDUpdateIconOffsets) */
static void icon_offsets(const CARDDir *d, CARDStat *stat)
{
    u32 offset = d->iconAddr;
    int i;
    BOOL tlut = FALSE;

    if (offset == 0xFFFFFFFF)
    {
        stat->offsetBanner = stat->offsetBannerTlut = 0xFFFFFFFF;
        for (i = 0; i < CARD_ICON_MAX; i++) stat->offsetIcon[i] = 0xFFFFFFFF;
        stat->offsetIconTlut = 0xFFFFFFFF;
        stat->offsetData = 0;
        return;
    }
    switch (d->bannerFormat & CARD_STAT_BANNER_MASK)
    {
    case CARD_STAT_BANNER_C8:
        stat->offsetBanner = offset;
        offset += CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
        stat->offsetBannerTlut = offset;
        offset += 2 * 256;
        break;
    case CARD_STAT_BANNER_RGB5A3:
        stat->offsetBanner = offset;
        offset += 2 * CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
        stat->offsetBannerTlut = 0xFFFFFFFF;
        break;
    default:
        stat->offsetBanner = stat->offsetBannerTlut = 0xFFFFFFFF;
        break;
    }
    for (i = 0; i < CARD_ICON_MAX; i++)
    {
        switch ((d->iconFormat >> (2 * i)) & CARD_STAT_ICON_MASK)
        {
        case CARD_STAT_ICON_C8:
            stat->offsetIcon[i] = offset;
            offset += CARD_ICON_WIDTH * CARD_ICON_HEIGHT;
            tlut = TRUE;
            break;
        case CARD_STAT_ICON_RGB5A3:
            stat->offsetIcon[i] = offset;
            offset += 2 * CARD_ICON_WIDTH * CARD_ICON_HEIGHT;
            break;
        default:
            stat->offsetIcon[i] = 0xFFFFFFFF;
            break;
        }
    }
    if (tlut)
    {
        stat->offsetIconTlut = offset;
        offset += 2 * 256;
    }
    else
    {
        stat->offsetIconTlut = 0xFFFFFFFF;
    }
    stat->offsetData = offset;
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat *stat)
{
    s32 r = ready(chan);
    const CARDDir *d;

    if (r != CARD_RESULT_READY) return finish(chan, r);
    if (fileNo < 0 || fileNo >= MAX_FILES) return finish(chan, CARD_RESULT_FATAL_ERROR);
    d = &sCard[chan].dir[fileNo];
    if (!used(d)) return finish(chan, CARD_RESULT_NOFILE);
    memcpy(stat->fileName, d->fileName, CARD_FILENAME_MAX);
    stat->length = d->length * SECTOR;
    stat->time = d->time;
    memcpy(stat->gameName, d->gameName, 4);
    memcpy(stat->company, d->company, 2);
    stat->bannerFormat = d->bannerFormat;
    stat->iconAddr = d->iconAddr;
    stat->iconFormat = d->iconFormat;
    stat->iconSpeed = d->iconSpeed;
    stat->commentAddr = d->commentAddr;
    icon_offsets(d, stat);
    return finish(chan, CARD_RESULT_READY);
}

s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat *stat)
{
    s32 r = ready(chan);
    CARDDir *d;

    if (r != CARD_RESULT_READY) return finish(chan, r);
    if (fileNo < 0 || fileNo >= MAX_FILES) return finish(chan, CARD_RESULT_FATAL_ERROR);
    d = &sCard[chan].dir[fileNo];
    if (!used(d)) return finish(chan, CARD_RESULT_NOFILE);
    d->bannerFormat = stat->bannerFormat;
    d->iconAddr = stat->iconAddr;
    d->iconFormat = stat->iconFormat;
    d->iconSpeed = stat->iconSpeed;
    d->commentAddr = stat->commentAddr;
    d->time = now_seconds();
    icon_offsets(d, stat);
    return finish(chan, save_dir(chan));
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CARDStat *stat, CARDCallback callback)
{
    return later(chan, CARDSetStatus(chan, fileNo, stat), callback);
}
