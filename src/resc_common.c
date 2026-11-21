/*
 * resc_common.c - see resc_common.h.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include "clib.h"
#include "resc_common.h"
#include "platform.h"
#include "locale_support.h"

BOOL Rc_Init(struct RcOut *o, struct BlockDev *bd, struct Report *rep, struct RescueStats *st)
{
    memset(o, 0, sizeof(*o));
    o->bd = bd; o->rep = rep; o->st = st;
    o->buf = (UBYTE *)AllocVec(RC_CHUNK * 512UL, MEMF_PUBLIC);
    return o->buf != NULL;
}

void Rc_Free(struct RcOut *o)
{
    if (o->buf) FreeVec(o->buf);
    o->buf = NULL;
}

void Rc_FreeName(struct RcOut *o, const char *path, ULONG key)
{
    strncpy(o->dest, path, RC_PATHMAX - 1);
    o->dest[RC_PATHMAX - 1] = '\0';
    if (Out_Exists(o->dest)) snprintf(o->dest, sizeof(o->dest), "%s_%lu", path, (unsigned long)key);
}

BOOL Rc_Open(struct RcOut *o, const char *path, ULONG key, ULONG size)
{
    Rc_FreeName(o, path, key);
    o->fh = Out_Create(o->dest);
    if (!o->fh) {
        Rep_Add(o->rep, SEV_ERROR, GS(MSG_S_CANNOT_CREATE), o->dest);
        o->st->failed++;
        return FALSE;
    }
    o->remaining = o->size = size;
    o->holes = 0; o->write_failed = FALSE; o->broken = FALSE;
    return TRUE;
}

static void put(struct RcOut *o, const UBYTE *p, ULONG len)
{
    if (len > o->remaining) len = o->remaining;
    if (len == 0 || o->write_failed) return;
    if (!Out_Write(o->fh, p, len)) o->write_failed = TRUE;
    o->remaining -= len;
}

void Rc_Run(struct RcOut *o, ULONG first, ULONG n)
{
    while (n && o->remaining && !o->write_failed && !o->rep->aborted) {
        ULONG c = n > RC_CHUNK ? RC_CHUNK : n, k;
        ULONG need = (o->remaining + 511) / 512;
        if (c > need) c = need;
        if (BlockDev_ReadBlocks(o->bd, first, c, o->buf)) {
            put(o, o->buf, c * 512UL);
        } else {
            for (k = 0; k < c; k++) {
                if (!BlockDev_ReadBlocks(o->bd, first + k, 1, o->buf)) {
                    memset(o->buf, 0, 512);
                    o->holes++;
                    o->read_errors++;
                }
                put(o, o->buf, 512);
            }
        }
        first += c; n -= c;
        Rep_Progress(o->rep, o->st->files, 0, o->dest);
    }
}

void Rc_Close(struct RcOut *o, ULONG prot, ULONG days, ULONG mins, ULONG ticks, const char *comment)
{
    Out_Close(o->fh);
    o->fh = NULL;
    Out_SetMeta(o->dest, prot, days, mins, ticks, comment ? comment : "");
    o->st->files++;
    o->st->bytes_kb += (o->size - o->remaining + 1023) / 1024;
    if (o->write_failed) {
        Rep_Add(o->rep, SEV_ERROR, GS(MSG_S_WRITE_FAILED), o->dest);
        o->st->failed++;
    } else if (o->remaining || o->holes || o->broken) {
        Rep_Add(o->rep, SEV_WARN, GS(MSG_S_PARTIAL), o->dest,
                (unsigned long)(o->size - o->remaining), (unsigned long)o->size, (unsigned long)o->holes);
        o->st->partial++;
    }
}

BOOL Rc_Join(char *path, ULONG *len, const char *name)
{
    ULONG nl = strlen(name);
    if (nl == 0 || *len + nl + 2 >= RC_PATHMAX) return FALSE;
    if (*len) path[(*len)++] = '/';
    strcpy(path + *len, name);
    *len += nl;
    return TRUE;
}
