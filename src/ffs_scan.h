/*
 * ffs_scan.h - one consistency pass over an OFS/FFS volume.
 *
 * The scan is what check, repair and rescue all start from:
 *
 *   FfsScan_Tree      walk everything reachable from the root, claim every
 *                     block a header, extension or file uses, note what is
 *                     wrong and what would fix it
 *   FfsScan_Bitmap    compare the on-disk bitmap with those claims
 *   FfsScan_Orphans   read the rest of the volume for headers nothing
 *                     points to (lost through a cut chain, or deleted)
 *   FfsScan_Attach    decide which of those can be put back
 *
 * It only reads (through the overlay, when ffs_repair.c has one open).
 */
#ifndef FFS_SCAN_H
#define FFS_SCAN_H

#include <exec/types.h>
#include "ffs.h"
#include "report.h"

#define FIX_SETPTR    1   /* a=dir b=slot c=prev d=expect e=new value */
#define FIX_SETPARENT 2   /* a=header b=parent */
#define FIX_INSERT    3   /* a=header b=directory (0 = Lost+Found) */

#define WHY_CUT      1    /* chain ends at a bad block */
#define WHY_DAMAGED  2    /* file with a broken block list leaves its directory */
#define WHY_UNHASH   3    /* first half of a re-file (no line of its own) */
#define WHY_PARENT   4
#define WHY_REHASH   5
#define WHY_ATTACH   6

struct FfsFix {
    UBYTE op;
    UBYTE why;
    ULONG a, b, c, d, e;
    char  what[72];          /* the object, for the plan / log line */
};

struct FfsOrphan {
    ULONG key;
    LONG  sectype;
    UBYTE deleted;           /* its header block is free in a readable bitmap */
    UBYTE state;             /* 0 untouched, 1 attached / walked, 2 conflict */
};

struct FfsScanOpts {
    BOOL deep;               /* read every data block (OFS: verify its header) */
    BOOL orphans;            /* search for lost and deleted headers */
};

struct FfsScan {
    struct FfsVol   *v;
    struct Report   *rep;
    struct FfsScanOpts o;
    UBYTE *claim;            /* 2 bits per block: 0 none, 1 once, 2 several owners */
    UBYTE *seen;             /* header reached */
    UBYTE *isdir;            /* reachable directory */
    UBYTE *rootbuf;
    UBYTE *iobuf;            /* 32 blocks of scratch */
    BOOL   have_root;
    struct FfsBm bm;
    BOOL   have_bm;

    ULONG dirs, files, links;
    ULONG crosslinked;       /* blocks claimed more than once */
    ULONG bad_entries;       /* chains cut short by a bad pointer */
    ULONG damaged_files;
    ULONG bad_data;          /* DEEP: unreadable / invalid data blocks */
    ULONG used_but_free;     /* in use, bitmap says free */
    ULONG free_but_used;     /* nothing uses it, bitmap says used */
    ULONG lost, deleted;     /* orphan headers by kind */
    ULONG attachable;
    ULONG structural;        /* findings a repair has to act on */
    BOOL  bitmap_dirty;      /* the bitmap needs rebuilding */

    struct FfsFix *fix;
    ULONG  nfix, capfix;
    struct FfsOrphan *orph;
    ULONG  norph, caporph;
    ULONG  ticks;
    BOOL   quiet;            /* count only, no finding lines (later repair rounds) */
};

BOOL FfsScan_Init(struct FfsScan *s, struct FfsVol *v, struct Report *rep,
                  const struct FfsScanOpts *o);
void FfsScan_Free(struct FfsScan *s);

/* FALSE: no root block, out of memory, or aborted (rep->aborted). */
BOOL FfsScan_Tree(struct FfsScan *s);
void FfsScan_Bitmap(struct FfsScan *s);
BOOL FfsScan_Orphans(struct FfsScan *s);
void FfsScan_Attach(struct FfsScan *s);

/* claim map access for the repair and rescue code */
UBYTE FfsScan_Claim(const struct FfsScan *s, ULONG blk);
void  FfsScan_AddClaim(struct FfsScan *s, ULONG blk);
struct FfsOrphan *FfsScan_FindOrphan(struct FfsScan *s, ULONG key);

/* The whole read-only check with its summary.  Returns a DOS return
 * code: 0 clean, 5 warnings only, 10 damaged, 20 not checkable. */
LONG FfsCheck_Run(struct BlockDev *bd, const struct VolumeInfo *vi,
                  const struct FfsScanOpts *o, struct Report *rep);

#endif /* FFS_SCAN_H */
