/*
 * cli.c - command-line mode (see cli.h).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "clib.h"
#include "blockdev.h"
#include "volume.h"
#include "report.h"
#include "fscheck.h"
#include "cli.h"
#include "version.h"
#include "locale_support.h"

extern BOOL DiskRepair_Debug;

#define TEMPLATE "VOLUME,IMAGE/K,PART/K,LIST/S,CHECK/S,DEEP/S,LOST/S,RESCUE/K,DELETED/S,REPAIR/S,DRYRUN/S,UNDOFILE/K,UNDO/S,FORCE/S,ALL/S,DEBUG/S,UNDELETE/K"
enum { A_VOLUME, A_IMAGE, A_PART, A_LIST, A_CHECK, A_DEEP, A_LOST, A_RESCUE, A_DELETED, A_REPAIR,
       A_DRYRUN, A_UNDOFILE, A_UNDO, A_FORCE, A_ALL, A_DEBUG, A_UNDELETE, A_COUNT };

static void out(const char *s) { PutStr((CONST_STRPTR)s); }

static void outf(const char *fmt, ...)
{
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out(buf);
}

static void cli_line(void *ud, int sev, const char *text)
{
    (void)ud;
    switch (sev) {
    case SEV_WARN:  outf("%s: %s\n", GS(MSG_SEV_WARNING), text); break;
    case SEV_ERROR: outf("%s: %s\n", GS(MSG_SEV_ERROR), text); break;
    case SEV_FATAL: outf("%s: %s\n", GS(MSG_SEV_FATAL), text); break;
    case SEV_FIX:   outf("%s: %s\n", GS(MSG_SEV_FIX), text); break;
    default:        outf("%s\n", text); break;
    }
}

static BOOL cli_progress(void *ud, ULONG done, ULONG total, const char *what)
{
    (void)ud; (void)done; (void)total; (void)what;
    return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) == 0;
}

static BOOL cli_confirm(void)
{
    char buf[16];
    LONG n;
    out(GS(MSG_CLI_TYPE_YES));
    Flush(Output());
    n = Read(Input(), buf, sizeof(buf) - 1);
    if (n <= 0) return FALSE;
    buf[n] = '\0';
    return buf[0] == 'Y' && buf[1] == 'E' && buf[2] == 'S';
}

static void list_volumes(const struct VolumeList *vl)
{
    UWORD i;
    if (vl->count == 0) { out(GS(MSG_CLI_NO_PARTITIONS)); return; }
    out(GS(MSG_CLI_LIST_HEADER));
    for (i = 0; i < vl->count; i++) {
        const struct VolumeInfo *v = &vl->v[i];
        char fs[32], size[16];
        Volume_FsName(v->dostype, fs, sizeof(fs));
        Volume_SizeString(v->num_blocks, v->sector_size, size, sizeof(size));
        outf(GS(MSG_CLI_LIST_ROW), v->dosname, v->volname[0] ? v->volname : "-", fs, size,
             v->devname, (unsigned long)v->unit, v->is_boot ? GS(MSG_CLI_BOOT_SUFFIX) : "");
    }
}

static BOOL name_eq(const char *a, const char *b)
{
    while (*a || *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return FALSE;
    }
    return TRUE;
}

/* Device name (DH0:) first, then mounted volume name (Work:), then list index. */
static const struct VolumeInfo *pick(const struct VolumeList *vl, const char *name)
{
    UWORD i;
    char  want[32];
    ULONG n = 0;
    if (!name) return vl->count ? &vl->v[0] : NULL;
    while (name[n] && name[n] != ':' && n < sizeof(want) - 1) { want[n] = name[n]; n++; }
    want[n] = '\0';
    for (i = 0; i < vl->count; i++)
        if (name_eq(vl->v[i].dosname, want)) return &vl->v[i];
    for (i = 0; i < vl->count; i++)
        if (vl->v[i].volname[0] && name_eq(vl->v[i].volname, want)) return &vl->v[i];
    if (want[0] >= '0' && want[0] <= '9') {
        ULONG ix = strtoul(want, NULL, 10);
        if (ix < vl->count) return &vl->v[ix];
    }
    return NULL;
}

LONG cli_run(void)
{
    SIPTR args[A_COUNT];
    struct RDArgs *rda;
    struct VolumeList *vl;
    struct BlockDev *bd = NULL;
    const struct VolumeInfo *v;
    struct Report rep;
    char fs[32], size[16];
    LONG rc = RETURN_OK;

    memset(args, 0, sizeof(args));
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"DiskRepair");
        return RETURN_ERROR;
    }
    if (!args[A_VOLUME] && !args[A_IMAGE] && !args[A_LIST]) {
        FreeArgs(rda);
        return CLI_NO_ARGS;
    }

    vl = (struct VolumeList *)AllocVec(sizeof(*vl), MEMF_PUBLIC | MEMF_CLEAR);
    if (!vl) { FreeArgs(rda); outf("%s\n", GS(MSG_OUT_OF_MEMORY)); return RETURN_FAIL; }

    if (args[A_IMAGE]) {
        char err[80];
        bd = BlockDev_OpenFile((const char *)args[A_IMAGE]);
        if (!bd) { outf(GS(MSG_CLI_CANNOT_OPEN_IMAGE), (const char *)args[A_IMAGE]); rc = RETURN_ERROR; goto done; }
        if (!Volume_EnumerateImage(bd, vl, err, sizeof(err))) { outf("%s: %s\n", bd->devname, err); rc = RETURN_ERROR; goto done; }
    } else {
        Volume_EnumerateMounted(vl);
    }

    if (args[A_DEBUG]) DiskRepair_Debug = TRUE;
    if (args[A_LIST]) { list_volumes(vl); goto done; }

    v = pick(vl, args[A_IMAGE] ? (const char *)args[A_PART] : (const char *)args[A_VOLUME]);
    if (!v) { out(GS(MSG_CLI_PARTITION_NOT_FOUND)); rc = RETURN_ERROR; goto done; }

    if (!bd) {
        bd = BlockDev_Open(v->devname, v->unit);
        if (!bd) { outf(GS(MSG_CLI_CANNOT_OPEN_UNIT), v->devname, (unsigned long)v->unit); rc = RETURN_ERROR; goto done; }
    }

    Rep_Init(&rep);
    rep.line = cli_line;
    rep.progress = cli_progress;
    rep.max_per_sev = args[A_ALL] ? 0 : 40;

    Volume_FsName(v->dostype, fs, sizeof(fs));
    Volume_SizeString(v->num_blocks, v->sector_size, size, sizeof(size));
    outf(GS(MSG_CLI_VOLUME_LINE), v->dosname, v->volname[0] ? v->volname : "-", fs, size);

    if (args[A_UNDO]) {
        if (!args[A_UNDOFILE]) { outf("%s\n", GS(MSG_R_NEED_UNDO)); rc = RETURN_ERROR; goto done; }
        if (!args[A_FORCE]) {
            outf(GS(MSG_CLI_UNDO_WARNING), v->dosname, (const char *)args[A_UNDOFILE]);
            if (!cli_confirm()) { out(GS(MSG_CLI_CANCELLED)); rc = RETURN_WARN; goto done; }
        }
        rc = Fs_Undo(bd, v, (const char *)args[A_UNDOFILE], &rep);
    } else if (args[A_REPAIR] || args[A_DRYRUN]) {
        struct RepairOpts ro;
        ro.dry = args[A_DRYRUN] != 0;
        ro.undo_path = (const char *)args[A_UNDOFILE];
        if (!ro.dry && !ro.undo_path) { outf("%s\n", GS(MSG_R_NEED_UNDO)); rc = RETURN_ERROR; goto done; }
        if (!ro.dry && !args[A_FORCE]) {
            outf(GS(MSG_CLI_REPAIR_WARNING), v->dosname, v->devname);
            if (!cli_confirm()) { out(GS(MSG_CLI_CANCELLED)); rc = RETURN_WARN; goto done; }
        }
        rc = Fs_Repair(bd, v, &ro, &rep);
    } else if (args[A_RESCUE] || args[A_UNDELETE]) {
        struct RescueOpts so;
        struct RescueStats st;
        so.deleted = args[A_DELETED] != 0;
        so.only_deleted = args[A_UNDELETE] != 0;
        rc = Fs_Rescue(bd, v, (const char *)(args[A_UNDELETE] ? args[A_UNDELETE] : args[A_RESCUE]), &so, &rep, &st);
    } else {
        struct CheckOpts co;
        co.deep = args[A_DEEP] != 0;
        co.lost = args[A_LOST] != 0;
        rc = Fs_Check(bd, v, &co, &rep);
        if (rep.aborted)            out(GS(MSG_CLI_CANCELLED));
        else if (rc == RETURN_OK)   outf("%s\n", GS(MSG_V_CLEAN));
        else if (rc == RETURN_WARN) outf("%s\n", GS(MSG_V_WARN));
        else if (rc == RETURN_ERROR) outf("%s\n", GS(MSG_V_DAMAGED));
        else                        outf("%s\n", GS(MSG_V_FAILED));
    }
    {
        int sv;
        for (sv = SEV_WARN; sv <= SEV_FIX; sv++)
            if (rep.count[sv] > rep.shown[sv])
                outf(GS(MSG_CLI_MORE_LINES), (unsigned long)(rep.count[sv] - rep.shown[sv]));
    }
    if (DiskRepair_Debug)
        outf("Device I/O: %lu reads, %lu writes\n", (unsigned long)bd->reads, (unsigned long)bd->writes);

done:
    if (bd) BlockDev_Close(bd);
    FreeVec(vl);
    FreeArgs(rda);
    return rc;
}
