/*
 * platform_amiga.c - platform.h on AmigaOS (dos.library v37+).
 *
 * Inhibit and everything after it talk to the handler's message port
 * directly, never through a name: when a volume carries the same name as
 * its device, dos.library finds the volume node first, and during an
 * inhibit that node has no task - DOS would ask for the volume to be
 * inserted and the uninhibit would never arrive.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "clib.h"
#include "volume.h"
#include "platform.h"
#include "locale_support.h"

static BOOL no_handler(const struct VolumeInfo *v)
{
    const char *d = v->devname;
    BOOL is_file = (d[0] == 'F' || d[0] == 'f') && (d[1] == 'I' || d[1] == 'i') &&
                   (d[2] == 'L' || d[2] == 'l') && (d[3] == 'E' || d[3] == 'e') && d[4] == ':';
    return !v->mounted || is_file || !v->handler;
}

BOOL Plat_Inhibit(const struct VolumeInfo *v, BOOL on, char *err, ULONG errlen)
{
    struct MsgPort *port;
    int tries;
    if (no_handler(v)) return TRUE;
    port = (struct MsgPort *)v->handler;
    if (!on) { DoPkt(port, ACTION_INHIBIT, DOSFALSE, 0, 0, 0, 0); return TRUE; }
    /* FFS refuses while its own writes are unflushed (up to ~3 s). */
    for (tries = 0; tries < 20; tries++) {
        if (DoPkt(port, ACTION_INHIBIT, DOSTRUE, 0, 0, 0, 0)) return TRUE;
        Delay(25);
    }
    if (err && errlen) snprintf(err, errlen, GS(MSG_P_CANNOT_INHIBIT), v->dosname, (long)IoErr());
    return FALSE;
}

BOOL Plat_PathOnVolume(const char *path, const struct VolumeInfo *v)
{
    struct DevProc *dp;
    BOOL same = FALSE;
    if (!v->handler || !path) return FALSE;
    /* GetDeviceProc resolves the name (assigns, current directory) without
     * touching the volume itself. */
    dp = GetDeviceProc((CONST_STRPTR)path, NULL);
    if (dp) {
        same = (dp->dvp_Port == (struct MsgPort *)v->handler);
        FreeDeviceProc(dp);
    }
    return same;
}

/* --- rescue destination -------------------------------------------- */

static char g_dest[256];

static void join(char *out, ULONG outlen, const char *rel)
{
    ULONG n = strlen(g_dest);
    char last = n ? g_dest[n - 1] : ':';
    if (!rel[0])                       snprintf(out, outlen, "%s", g_dest);
    else if (last == ':' || last == '/') snprintf(out, outlen, "%s%s", g_dest, rel);
    else                               snprintf(out, outlen, "%s/%s", g_dest, rel);
}

/* 1 = directory, 0 = something else, -1 = not there */
static int kind_of(const char *full)
{
    struct FileInfoBlock *fib;
    BPTR l = Lock((CONST_STRPTR)full, ACCESS_READ);
    int kind = 0;
    if (!l) return -1;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (fib) {
        if (Examine(l, fib) && fib->fib_DirEntryType > 0) kind = 1;
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(l);
    return kind;
}

static BOOL make_dir(const char *full)
{
    BPTR l;
    int k = kind_of(full);
    if (k == 1) return TRUE;
    if (k == 0) return FALSE;
    l = CreateDir((CONST_STRPTR)full);
    if (!l) return FALSE;
    UnLock(l);
    return TRUE;
}

BOOL Out_Begin(const char *dest, char *err, ULONG errlen)
{
    strncpy(g_dest, dest, sizeof(g_dest) - 1);
    g_dest[sizeof(g_dest) - 1] = '\0';
    if (!make_dir(g_dest)) {
        if (err && errlen) snprintf(err, errlen, GS(MSG_S_BAD_DEST), dest);
        return FALSE;
    }
    return TRUE;
}

void Out_End(void) { g_dest[0] = '\0'; }

BOOL Out_Exists(const char *rel)
{
    char full[1300];
    join(full, sizeof(full), rel);
    return kind_of(full) >= 0;
}

BOOL Out_MkDir(const char *rel)
{
    char full[1300];
    join(full, sizeof(full), rel);
    return make_dir(full);
}

APTR Out_Create(const char *rel)
{
    char full[1300];
    join(full, sizeof(full), rel);
    return (APTR)Open((CONST_STRPTR)full, MODE_NEWFILE);
}

BOOL Out_Write(APTR fh, const void *buf, ULONG len)
{
    return Write((BPTR)fh, (APTR)buf, (LONG)len) == (LONG)len;
}

void Out_Close(APTR fh) { if (fh) Close((BPTR)fh); }

void Out_SetMeta(const char *rel, ULONG prot, ULONG days, ULONG mins, ULONG ticks, const char *comment)
{
    char full[1300];
    struct DateStamp ds;
    join(full, sizeof(full), rel);
    if (comment && comment[0]) SetComment((CONST_STRPTR)full, (CONST_STRPTR)comment);
    SetProtection((CONST_STRPTR)full, (LONG)prot);
    ds.ds_Days = (LONG)days; ds.ds_Minute = (LONG)mins; ds.ds_Tick = (LONG)ticks;
    if (days < 100000UL && mins < 1440 && ticks < 3000) SetFileDate((CONST_STRPTR)full, &ds);
}
