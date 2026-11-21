/*
 * ffs_repair.h - in-place repair and rescue-to-path for OFS/FFS.
 */
#ifndef FFS_REPAIR_H
#define FFS_REPAIR_H

#include <exec/types.h>
#include "blockdev.h"
#include "volume.h"
#include "report.h"

struct RepairOpts {
    BOOL        dry;         /* plan only: nothing is written, no undo file */
    const char *undo_path;   /* required unless dry; on another volume */
};

/* DOS return code: 0 repaired / nothing to do, 5 dry run with changes
 * planned, 20 not repaired (volume untouched, or - only after a failed
 * write - to be put back with UNDO). */
LONG FfsRepair_Run(struct BlockDev *bd, const struct VolumeInfo *vi,
                   const struct RepairOpts *opts, struct Report *rep);

struct RescueOpts {
    BOOL deleted;            /* also copy deleted files that are still on the disk */
};

struct RescueStats {
    ULONG dirs, files, bytes_kb;
    ULONG partial;           /* files copied with holes or cut short */
    ULONG failed;            /* could not be created at the destination */
    ULONG lost, deleted;     /* copied from outside the directory tree */
};

/* Copy everything readable to the destination opened with Out_Begin().
 * The volume is only read.  DOS return code. */
LONG FfsRescue_Run(struct BlockDev *bd, const struct VolumeInfo *vi,
                   const struct RescueOpts *opts, struct Report *rep,
                   struct RescueStats *st);

#endif /* FFS_REPAIR_H */
