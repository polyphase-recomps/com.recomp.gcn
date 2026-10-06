/*
 * DVD: the disc's file system (FST, loaded to the top of RAM as the IPL does) and reads,
 * which the host serves from the disc image. Reads complete at once; the callbacks of
 * asynchronous ones run at the next event delivery, as an interrupt would.
 */
#include <dolphin.h>
#include <dolphin/dvd.h>

#include "gcn_guest.h"
#include "gcn_sdk.h"

#define MAX_PENDING 1024

static u8 *sFst;        /* FST entries, big-endian, in guest RAM */
static u32 sFstEntries;
static char *sFstStrings;
static DVDCommandBlock *sPending[MAX_PENDING];
static int sPendingCount;
static DVDDiskID sDiskID;

static u32 rd32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

void gcn_dvd_init(void)
{
    u8 hdr[0x440];
    u32 fst_off, fst_size;

    gcn_host_disc_read(hdr, 0, sizeof(hdr));
    memcpy(&sDiskID, hdr, sizeof(sDiskID));
    memcpy((void *)0x80000000, hdr, 0x20); /* disk ID, where the IPL leaves it */
    fst_off = rd32(hdr + 0x424);
    fst_size = rd32(hdr + 0x428);
    if (fst_size > GCN_FST_RESERVE) gcn_host_fatal("gcn: FST larger than its reserve");
    sFst = (u8 *)(0x81800000u - GCN_FST_RESERVE);
    gcn_host_disc_read(sFst, fst_off, fst_size);
    sFstEntries = rd32(sFst + 8);
    sFstStrings = (char *)sFst + sFstEntries * 12;
    *(u32 *)0x80000038 = (u32)sFst; /* OS globals: FST location and size */
    *(u32 *)0x8000003C = fst_size;
    gcn_logf("gcn: disc %.6s, %u FST entries", hdr, (unsigned)sFstEntries);
}

void DVDInit(void) {}

static int is_dir(u32 e) { return sFst[e * 12] != 0; }
static const char *entry_name(u32 e) { return sFstStrings + (rd32(sFst + e * 12) & 0xFFFFFF); }
static u32 entry_off(u32 e) { return rd32(sFst + e * 12 + 4); }
static u32 entry_len(u32 e) { return rd32(sFst + e * 12 + 8); }

static int name_eq(const char *a, const char *b, int n)
{
    int i;

    for (i = 0; i < n; i++)
    {
        char x = a[i], y = b[i];

        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return b[n] == 0;
}

static u32 sCurrentDir;

s32 DVDConvertPathToEntrynum(const char *path)
{
    u32 dir = sCurrentDir;
    const char *p = path;

    if (*p == '/')
    {
        dir = 0;
        p++;
    }
    for (;;)
    {
        const char *end = p;
        u32 e, last;
        int n;

        while (*end && *end != '/') end++;
        n = (int)(end - p);
        if (n == 0)
        {
            return (s32)dir;
        }
        if (n == 1 && p[0] == '.')
        {
            p = *end ? end + 1 : end;
            continue;
        }
        if (n == 2 && p[0] == '.' && p[1] == '.')
        {
            dir = entry_off(dir); /* parent index of a directory */
            p = *end ? end + 1 : end;
            continue;
        }
        last = dir == 0 ? sFstEntries : entry_len(dir);
        for (e = dir + 1; e < last;)
        {
            if (name_eq(p, entry_name(e), n)) break;
            e = is_dir(e) ? entry_len(e) : e + 1;
        }
        if (e >= last) return -1;
        if (*end == 0) return (s32)e;
        if (!is_dir(e)) return -1;
        dir = e;
        p = end + 1;
    }
}

BOOL DVDChangeDir(const char *dirName)
{
    s32 e = DVDConvertPathToEntrynum(dirName);

    if (e < 0 || !is_dir((u32)e)) return FALSE;
    sCurrentDir = (u32)e;
    return TRUE;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo *fileInfo)
{
    if (entrynum < 0 || (u32)entrynum >= sFstEntries || is_dir((u32)entrynum)) return FALSE;
    memset(fileInfo, 0, sizeof(*fileInfo));
    fileInfo->startAddr = entry_off((u32)entrynum);
    fileInfo->length = entry_len((u32)entrynum);
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
}

BOOL DVDOpen(const char *fileName, DVDFileInfo *fileInfo)
{
    s32 e = DVDConvertPathToEntrynum(fileName);

    if (e < 0)
    {
        const u8 *b = (const u8 *)fileName;
        gcn_logf("DVDOpen: %.64s not found (at %08x: %02x %02x %02x %02x %02x %02x %02x %02x)", fileName, (u32)fileName,
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        return FALSE;
    }
    return DVDFastOpen(e, fileInfo);
}

BOOL DVDClose(DVDFileInfo *fileInfo)
{
    return TRUE;
}

static void complete_later(DVDCommandBlock *block)
{
    if (sPendingCount >= MAX_PENDING) gcn_host_fatal("gcn: too many DVD reads in flight");
    sPending[sPendingCount++] = block;
}

static void complete(void);

/* deferred completions: DVD reads, then memory card operations (card.c) */
int gcn_dvd_poll_count(void)
{
    int n = sPendingCount;
    complete();
    return n + gcn_card_poll();
}

void gcn_dvd_poll(void)
{
    complete();
    gcn_card_poll();
}

static void complete(void)
{
    while (sPendingCount)
    {
        DVDCommandBlock *b = sPending[0];
        int i;

        for (i = 1; i < sPendingCount; i++) sPending[i - 1] = sPending[i];
        sPendingCount--;
        b->state = DVD_STATE_END;
        if (b->callback) b->callback((s32)b->transferredSize, b);
    }
}

static void read_now(DVDCommandBlock *b, void *addr, s32 length, u32 offset)
{
    b->command = DVD_COMMAND_READ;
    b->addr = addr;
    b->length = (u32)length;
    b->offset = offset;
    b->transferredSize = gcn_host_disc_read(addr, offset, (u32)length);
    b->currTransferSize = b->transferredSize;
}

/* the file-level callback through the command block's (DVDFileInfo starts with it) */
static void file_done(s32 result, DVDCommandBlock *block)
{
    DVDFileInfo *fi = (DVDFileInfo *)block;

    if (fi->callback) fi->callback(result, fi);
}

BOOL DVDReadAsyncPrio(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset, DVDCallback callback, s32 prio)
{
    read_now(&fileInfo->cb, addr, length, fileInfo->startAddr + (u32)offset);
    fileInfo->callback = callback;
    fileInfo->cb.callback = file_done;
    fileInfo->cb.state = DVD_STATE_BUSY;
    complete_later(&fileInfo->cb);
    return TRUE;
}

s32 DVDReadPrio(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset, s32 prio)
{
    read_now(&fileInfo->cb, addr, length, fileInfo->startAddr + (u32)offset);
    fileInfo->cb.state = DVD_STATE_END;
    return (s32)fileInfo->cb.transferredSize;
}

int DVDReadAbsAsyncPrio(DVDCommandBlock *block, void *addr, s32 length, s32 offset, DVDCBCallback callback, s32 prio)
{
    read_now(block, addr, length, (u32)offset);
    block->callback = callback;
    block->state = DVD_STATE_BUSY;
    complete_later(block);
    return TRUE;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock *block)
{
    if (block->state == DVD_STATE_BUSY) gcn_poll_events();
    return block->state;
}

s32 DVDGetDriveStatus(void)
{
    if (sPendingCount) gcn_poll_events();
    return DVD_STATE_END;
}

s32 DVDGetTransferredSize(DVDFileInfo *fileinfo) { return (s32)fileinfo->cb.transferredSize; }
BOOL DVDSetAutoInvalidation(BOOL autoInval) { return TRUE; }
BOOL DVDCheckDisk(void) { return TRUE; }
void DVDReset(void) {}
int DVDResetRequired(void) { return FALSE; }
DVDDiskID *DVDGetCurrentDiskID(void) { return &sDiskID; }
void *DVDGetFSTLocation(void) { return sFst; }
void DVDPause(void) {}
void DVDResume(void) {}

BOOL DVDCancelAsync(DVDCommandBlock *block, DVDCBCallback callback)
{
    if (callback) callback(0, block);
    return TRUE;
}

s32 DVDCancel(volatile DVDCommandBlock *block) { return 0; }

/* Streamed audio: the drive plays a file's ADPCM audio track in 32-byte blocks, which the
 * AI decodes and mixes (audio.c pulls them while its stream is started). A file prepared
 * while another plays follows it; at the end the drive stops ("stop at end") or loops. */
static u32 sStreamStart, sStreamPos, sStreamEnd; /* disc offsets; sStreamEnd 0: nothing */
static u32 sNextStart, sNextEnd;
static int sStreamStopAtEnd;

static BOOL stream_done(DVDCommandBlock *block, DVDCBCallback callback, u32 result)
{
    block->state = DVD_STATE_END;
    block->transferredSize = result;
    block->callback = callback;
    complete_later(block);
    return TRUE;
}

BOOL DVDPrepareStreamAsync(DVDFileInfo *fileInfo, u32 length, u32 offset, DVDCallback callback)
{
    u32 start = fileInfo->startAddr + offset;
    u32 end = start + (length ? length : fileInfo->length - offset);

    if (!sStreamEnd)
    {
        sStreamStart = sStreamPos = start;
        sStreamEnd = end;
        sStreamStopAtEnd = 0;
    }
    else
    {
        sNextStart = start;
        sNextEnd = end;
    }
    fileInfo->callback = callback;
    return stream_done(&fileInfo->cb, file_done, 0);
}

int DVDCancelStreamAsync(DVDCommandBlock *block, DVDCBCallback callback)
{
    sStreamEnd = sNextEnd = 0;
    return stream_done(block, callback, 0);
}

int DVDStopStreamAtEndAsync(DVDCommandBlock *block, DVDCBCallback callback)
{
    sStreamStopAtEnd = 1;
    return stream_done(block, callback, 0);
}

/* the play address (in words), 0 once stopped */
int DVDGetStreamPlayAddrAsync(DVDCommandBlock *block, DVDCBCallback callback)
{
    return stream_done(block, callback, sStreamEnd ? (sStreamPos >> 2) | 1 : 0);
}

/* The next block of the playing file: returns 0 if nothing plays, 2 for a file's first
 * block (the decoder starts over), else 1. */
int gcn_dvd_stream_block(u8 *block)
{
    int first;

    if (!sStreamEnd) return 0;
    if (sStreamPos + 32 > sStreamEnd)
    {
        if (sNextEnd)
        {
            sStreamStart = sStreamPos = sNextStart;
            sStreamEnd = sNextEnd;
            sNextEnd = 0;
        }
        else if (sStreamStopAtEnd || sStreamPos == sStreamStart)
        {
            sStreamEnd = 0;
            return 0;
        }
        else sStreamPos = sStreamStart;
    }
    first = sStreamPos == sStreamStart;
    gcn_host_disc_read(block, sStreamPos, 32);
    sStreamPos += 32;
    return first ? 2 : 1;
}

/* ---- directories ------------------------------------------------------------------------ */
BOOL DVDFastOpenDir(s32 entrynum, DVDDir *dir)
{
    if (entrynum < 0 || !is_dir((u32)entrynum)) return FALSE;
    dir->entryNum = (u32)entrynum;
    dir->location = (u32)entrynum + 1;
    dir->next = entry_len((u32)entrynum);
    return TRUE;
}

int DVDOpenDir(const char *dirName, DVDDir *dir)
{
    return DVDFastOpenDir(DVDConvertPathToEntrynum(dirName), dir);
}

int DVDReadDir(DVDDir *dir, DVDDirEntry *dirent)
{
    u32 e = dir->location;

    if (e <= dir->entryNum || e >= dir->next) return FALSE;
    dirent->entryNum = e;
    dirent->isDir = is_dir(e);
    dirent->name = (char *)entry_name(e);
    dir->location = is_dir(e) ? entry_len(e) : e + 1;
    return TRUE;
}

int DVDCloseDir(DVDDir *dir) { return TRUE; }
void DVDRewindDir(DVDDir *dir) { dir->location = dir->entryNum + 1; }
