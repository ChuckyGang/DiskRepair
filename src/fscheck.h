/*
 * fscheck.h - the three things DiskRepair does, per filesystem.
 *
 * Front ends (CLI, Amiga window, Linux window) call only these.
 *
 *   OFS/FFS (DOS\0-5)  check, repair in place, rescue
 *   PFS3, SFS          basic check for now (structures are walked and
 *                      every block accounted for); repair and rescue
 *                      report "not supported yet"
 */
#ifndef FSCHECK_H
#define FSCHECK_H

#include <exec/types.h>
#include "blockdev.h"
#include "volume.h"
#include "report.h"
#include "ffs_repair.h"

struct CheckOpts {
    BOOL deep;       /* also read every data block */
    BOOL lost;       /* also search for lost and deleted files */
};

BOOL Fs_CanRepair(const struct VolumeInfo *vi);
BOOL Fs_CanRescue(const struct VolumeInfo *vi);

/* All return DOS return codes (0 fine, 5 warnings, 10 damaged, 20 failed). */
LONG Fs_Check (struct BlockDev *bd, const struct VolumeInfo *vi,
               const struct CheckOpts *o, struct Report *rep);
LONG Fs_Repair(struct BlockDev *bd, const struct VolumeInfo *vi,
               const struct RepairOpts *o, struct Report *rep);
LONG Fs_Rescue(struct BlockDev *bd, const struct VolumeInfo *vi, const char *dest,
               const struct RescueOpts *o, struct Report *rep, struct RescueStats *st);
LONG Fs_Undo  (struct BlockDev *bd, const struct VolumeInfo *vi, const char *path,
               struct Report *rep);

#endif /* FSCHECK_H */
