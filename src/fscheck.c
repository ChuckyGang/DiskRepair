/*
 * fscheck.c - see fscheck.h.
 */
#include <exec/types.h>
#include <dos/dos.h>
#include "clib.h"
#include "fscheck.h"
#include "ffs_scan.h"
#include "fsmap.h"
#include "undo.h"
#include "platform.h"
#include "locale_support.h"

static BOOL is_ffs(const struct VolumeInfo *vi) { return vi->fskind == FS_FFS && (vi->dostype & 7) < 6; }

BOOL Fs_CanRepair(const struct VolumeInfo *vi) { return is_ffs(vi); }
BOOL Fs_CanRescue(const struct VolumeInfo *vi) { return is_ffs(vi) || vi->fskind == FS_PFS || vi->fskind == FS_SFS; }

static BOOL map_progress(void *ud, ULONG done, ULONG total, const char *what)
{
    return Rep_Progress((struct Report *)ud, done, total, what);
}

/* PFS3 / SFS: walk the structures with the block-map builder and see
 * whether every block marked used is accounted for. */
static LONG map_check(struct BlockDev *bd, const struct VolumeInfo *vi, struct Report *rep)
{
    struct FsMap m;
    if (!FsMap_Build(bd, vi, &m, map_progress, rep)) {
        if (rep->aborted) return RETURN_WARN;
        Rep_Add(rep, SEV_FATAL, GS(MSG_M_UNREADABLE), m.note);
        return RETURN_FAIL;
    }
    if (m.note[0]) Rep_Add(rep, SEV_WARN, "%s", m.note);
    if (m.count[BLK_UNKN])
        Rep_Add(rep, SEV_WARN, GS(MSG_M_UNKNOWN), (unsigned long)m.count[BLK_UNKN]);
    Rep_Add(rep, SEV_INFO, GS(MSG_M_SUMMARY), (unsigned long)m.dirs, (unsigned long)m.files,
            (unsigned long)(m.nblocks - m.count[BLK_FREE]), (unsigned long)m.nblocks);
    Rep_Add(rep, SEV_INFO, "%s", GS(MSG_M_BASIC_ONLY));
    FsMap_Free(&m);
    if (rep->count[SEV_ERROR]) return RETURN_ERROR;
    return rep->count[SEV_WARN] ? RETURN_WARN : RETURN_OK;
}

LONG Fs_Check(struct BlockDev *bd, const struct VolumeInfo *vi,
              const struct CheckOpts *o, struct Report *rep)
{
    struct FfsScanOpts so;
    if (vi->mounted) Rep_Add(rep, SEV_INFO, "%s", GS(MSG_C_MOUNTED_NOTE));
    switch (vi->fskind) {
    case FS_FFS:
        so.deep = o ? o->deep : FALSE;
        so.orphans = o ? o->lost : FALSE;
        return FfsCheck_Run(bd, vi, &so, rep);
    case FS_PFS:
    case FS_SFS:
        return map_check(bd, vi, rep);
    default:
        Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_NOTE_UNSUPPORTED_FS));
        return RETURN_FAIL;
    }
}

LONG Fs_Repair(struct BlockDev *bd, const struct VolumeInfo *vi,
               const struct RepairOpts *o, struct Report *rep)
{
    if (!Fs_CanRepair(vi)) { Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_C_REPAIR_UNSUPPORTED)); return RETURN_FAIL; }
    return FfsRepair_Run(bd, vi, o, rep);
}

LONG Fs_Rescue(struct BlockDev *bd, const struct VolumeInfo *vi, const char *dest,
               const struct RescueOpts *o, struct Report *rep, struct RescueStats *st)
{
    char err[160];
    LONG rc;
    memset(st, 0, sizeof(*st));
    if (!Fs_CanRescue(vi)) { Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_C_RESCUE_UNSUPPORTED)); return RETURN_FAIL; }
    if (Plat_PathOnVolume(dest, vi)) { Rep_Add(rep, SEV_FATAL, GS(MSG_S_SAME_VOLUME), dest); return RETURN_FAIL; }
    err[0] = '\0';
    if (!Out_Begin(dest, err, sizeof(err))) { Rep_Add(rep, SEV_FATAL, "%s", err); return RETURN_FAIL; }
    rc = vi->fskind == FS_PFS ? PfsRescue_Run(bd, vi, o, rep, st) :
         vi->fskind == FS_SFS ? SfsRescue_Run(bd, vi, o, rep, st) : FfsRescue_Run(bd, vi, o, rep, st);
    Out_End();
    Rep_Add(rep, SEV_INFO, GS(MSG_S_SUMMARY), (unsigned long)st->files, (unsigned long)st->dirs,
            (unsigned long)st->bytes_kb, (unsigned long)st->partial, (unsigned long)st->failed,
            (unsigned long)st->lost, (unsigned long)st->deleted);
    return rc;
}

LONG Fs_Undo(struct BlockDev *bd, const struct VolumeInfo *vi, const char *path, struct Report *rep)
{
    return Undo_Restore(bd, vi, path, rep);
}
