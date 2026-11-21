/*
 * undo.c - see undo.h.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "clib.h"
#include "be.h"
#include "undo.h"
#include "platform.h"
#include "locale_support.h"

#define UNDO_MAGIC 0x4452554EUL   /* "DRUN" */

static void put32(UBYTE *b, ULONG o, ULONG v)
{
    b[o] = (UBYTE)(v >> 24); b[o + 1] = (UBYTE)(v >> 16); b[o + 2] = (UBYTE)(v >> 8); b[o + 3] = (UBYTE)v;
}

BOOL Undo_Write(const char *path, const struct VolumeInfo *vi, ULONG nblocks, ULONG bsz,
                ULONG count, UndoGet get, void *ud, char *err, ULONG errlen)
{
    UBYTE hdr[32];
    BPTR  fh;
    ULONG i;
    BOOL  ok = TRUE;

    fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        if (err && errlen) snprintf(err, errlen, GS(MSG_U_CANNOT_CREATE), path);
        return FALSE;
    }
    put32(hdr, 0, UNDO_MAGIC);      put32(hdr, 4, 1);
    put32(hdr, 8, vi->start_block); put32(hdr, 12, nblocks);
    put32(hdr, 16, bsz);            put32(hdr, 20, count);
    put32(hdr, 24, vi->dostype);    put32(hdr, 28, 0);
    if (Write(fh, hdr, 32) != 32) ok = FALSE;
    for (i = 0; ok && i < count; i++) {
        const UBYTE *orig;
        ULONG blk;
        UBYTE n[4];
        if (!get(ud, i, &blk, &orig)) { ok = FALSE; break; }
        put32(n, 0, blk);
        if (Write(fh, n, 4) != 4 || Write(fh, (APTR)orig, (LONG)bsz) != (LONG)bsz) ok = FALSE;
    }
    Close(fh);
    if (!ok && err && errlen) snprintf(err, errlen, GS(MSG_U_WRITE_FAILED), path);
    return ok;
}

LONG Undo_Restore(struct BlockDev *bd, const struct VolumeInfo *vi, const char *path,
                  struct Report *rep)
{
    UBYTE hdr[32], n[4], *buf;
    BPTR  fh;
    ULONG bsz, count, spb, i, nblocks;
    char  err[128];
    LONG  rc = RETURN_OK;

    if (Plat_PathOnVolume(path, vi)) {
        Rep_Add(rep, SEV_FATAL, GS(MSG_U_SAME_VOLUME), path);
        return RETURN_FAIL;
    }
    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) { Rep_Add(rep, SEV_FATAL, GS(MSG_U_CANNOT_OPEN), path); return RETURN_FAIL; }
    if (Read(fh, hdr, 32) != 32 || be32(hdr, 0) != UNDO_MAGIC || be32(hdr, 4) != 1) {
        Rep_Add(rep, SEV_FATAL, GS(MSG_U_NOT_UNDO), path);
        Close(fh);
        return RETURN_FAIL;
    }
    bsz = be32(hdr, 16); count = be32(hdr, 20); nblocks = be32(hdr, 12);
    spb = bsz / vi->sector_size;
    if (be32(hdr, 8) != vi->start_block || be32(hdr, 24) != vi->dostype || spb == 0 ||
        bsz % vi->sector_size || nblocks != vi->num_blocks / spb) {
        Rep_Add(rep, SEV_FATAL, GS(MSG_U_OTHER_VOLUME), path);
        Close(fh);
        return RETURN_FAIL;
    }
    buf = (UBYTE *)AllocVec(bsz, MEMF_PUBLIC);
    if (!buf) { Close(fh); Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_OUT_OF_MEMORY)); return RETURN_FAIL; }

    err[0] = '\0';
    if (!Plat_Inhibit(vi, TRUE, err, sizeof(err))) {
        Rep_Add(rep, SEV_FATAL, "%s", err);
        FreeVec(buf); Close(fh);
        return RETURN_FAIL;
    }
    BlockDev_ArmWrites(bd);
    for (i = 0; i < count; i++) {
        ULONG blk;
        if (Read(fh, n, 4) != 4 || Read(fh, buf, (LONG)bsz) != (LONG)bsz) {
            Rep_Add(rep, SEV_ERROR, GS(MSG_U_SHORT), (unsigned long)i, (unsigned long)count);
            rc = RETURN_ERROR;
            break;
        }
        blk = be32(n, 0);
        if (blk >= nblocks || !BlockDev_WriteBlocks(bd, vi->start_block + blk * spb, spb, buf)) {
            Rep_Add(rep, SEV_ERROR, GS(MSG_U_BLOCK_FAILED), (unsigned long)blk);
            rc = RETURN_ERROR;
        }
    }
    BlockDev_Update(bd);
    Plat_Inhibit(vi, FALSE, NULL, 0);
    if (rc == RETURN_OK) Rep_Add(rep, SEV_INFO, GS(MSG_U_RESTORED), (unsigned long)count);
    FreeVec(buf);
    Close(fh);
    return rc;
}
