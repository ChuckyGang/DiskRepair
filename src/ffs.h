/*
 * ffs.h - OFS/FFS access for a repair tool: nothing here trusts the disk.
 *
 * Every pointer read from a block is range-checked before it is followed,
 * every chain has a loop guard, and a block that does not look like what
 * it should be ends that path with a finding instead of an error return.
 *
 * All block numbers are FS blocks relative to the partition start.
 *
 * Writes never go to the disk directly.  A repair works on an OVERLAY: a
 * set of modified blocks kept in RAM that every read sees.  Check passes
 * can therefore be run again on the repaired state before a single block
 * has been written, and the commit is one step: save the originals to an
 * undo file, then write the overlay (ffs_repair.c).
 */
#ifndef FFS_H
#define FFS_H

#include <exec/types.h>
#include "blockdev.h"
#include "volume.h"

#define FFS_T_SHORT     2UL
#define FFS_T_DATA      8UL
#define FFS_T_LIST      16UL
#define FFS_T_DIRCACHE  33UL
#define FFS_ST_ROOT     1L
#define FFS_ST_USERDIR  2L
#define FFS_ST_SOFTLINK 3L
#define FFS_ST_LINKDIR  4L
#define FFS_ST_FILE     (-3L)
#define FFS_ST_LINKFILE (-4L)

/* longword indices from the start of a header block */
#define FFS_L_TYPE      0
#define FFS_L_OWNKEY    1
#define FFS_L_HIGHSEQ   2
#define FFS_L_FIRSTDATA 4
#define FFS_L_CHECKSUM  5
#define FFS_L_TABLE     6
/* ... and counted from the end (nl - n) */
#define FFS_E_FIRSTPTR  51         /* data pointer 0; pointer k is nl-51-k */
#define FFS_E_BMFLAG    50
#define FFS_E_BMPAGES   49         /* 25 entries: nl-49 .. nl-25 */
#define FFS_E_PROTECT   48
#define FFS_E_BYTESIZE  47
#define FFS_E_COMMENT   46         /* BCPL string, 80 bytes */
#define FFS_E_BMEXT     24
#define FFS_E_DAYS      23         /* days, mins, ticks */
#define FFS_E_NAME      20         /* BCPL string, 32 bytes */
#define FFS_E_REALENTRY 11
#define FFS_E_NEXTLINK  10
#define FFS_E_HASHCHAIN 4
#define FFS_E_PARENT    3
#define FFS_E_EXTENSION 2
#define FFS_E_SECTYPE   1

#define FFS_OFS_HDR     24         /* OFS data block: bytes before the payload */
#define FFS_MAXNAME     30
#define FFS_MAXDEPTH    96
#define FFS_PATHMAX     1024

struct FfsOvl;                     /* modified blocks, see ffs.c */

struct FfsVol {
    struct BlockDev *bd;
    ULONG  part_start;             /* device block of FS block 0 */
    ULONG  spb;                    /* device blocks per FS block */
    ULONG  bsz;                    /* FS block size in bytes */
    ULONG  nl;                     /* longs per block */
    ULONG  nblocks;
    ULONG  reserved;
    ULONG  tsize;                  /* hash table / data table entries: nl - 56 */
    ULONG  datasize;               /* file bytes per data block */
    ULONG  dostype;
    ULONG  root;                   /* 0 until ffs_find_root() succeeded */
    ULONG  root_calc;              /* where the root block belongs */
    BOOL   ofs, intl, dircache;
    ULONG  read_errors;            /* blocks the device refused */
    UBYTE *extbuf;                 /* one block: extension chain walks */
    struct FfsOvl *ovl;            /* NULL = plain read-only view */
};

/* Geometry only, no disk access.  FALSE: err says why (unsupported). */
BOOL ffs_setup(struct FfsVol *v, struct BlockDev *bd, const struct VolumeInfo *vi,
               char *err, ULONG errlen);
void ffs_cleanup(struct FfsVol *v);

/* Read n blocks (overlay applied).  FALSE on a range or device error. */
BOOL ffs_rd(struct FfsVol *v, ULONG blk, ULONG n, void *buf);

/* --- overlay ------------------------------------------------------- */
BOOL   ffs_ovl_begin(struct FfsVol *v);
void   ffs_ovl_end(struct FfsVol *v);                    /* drops everything */
/* Replace block blk in the overlay (the original is fetched and kept on
 * first touch).  FALSE: out of memory or the original is unreadable. */
BOOL   ffs_ovl_put(struct FfsVol *v, ULONG blk, const UBYTE *data);
ULONG  ffs_ovl_count(const struct FfsVol *v);
/* i-th modified block in the order of first modification */
BOOL   ffs_ovl_get(const struct FfsVol *v, ULONG i, ULONG *blk,
                   const UBYTE **orig, const UBYTE **now);

/* --- block helpers -------------------------------------------------- */
ULONG ffs_getl(const UBYTE *b, ULONG idx);
void  ffs_putl(UBYTE *b, ULONG idx, ULONG val);
BOOL  ffs_sum_ok(const UBYTE *b, ULONG nl);
void  ffs_sum_set(UBYTE *b, ULONG nl, ULONG at);
/* Hash slot of a BCPL name (pointer to the length byte). */
ULONG ffs_hash(const struct FfsVol *v, const UBYTE *bstr);
BOOL  ffs_name_eq(const struct FfsVol *v, const UBYTE *bstr_a, const UBYTE *bstr_b);
/* Copy a BCPL name to C, replacing what no filing system takes ('/', ':',
 * control characters); empty names become "unnamed". */
void  ffs_name(const UBYTE *bstr, ULONG maxlen, char *out, ULONG outlen);

/* Does b (read from block blk) pass as a header: T_SHORT, checksum, own
 * key, known secondary type?  Returns the secondary type or 0. */
LONG  ffs_header_type(const struct FfsVol *v, const UBYTE *b, ULONG blk);

/* Locate the root block: the calculated position, then the pointer in
 * the boot block.  buf (one block) receives it.  FALSE = none found. */
BOOL  ffs_find_root(struct FfsVol *v, UBYTE *buf);

/* --- bitmap --------------------------------------------------------- */
struct FfsBm {
    ULONG *bits;                   /* one bit per block from `reserved`, LSB first, 1 = free */
    ULONG  longs;
    ULONG  span;                   /* nblocks - reserved */
    ULONG  bpb;                    /* blocks per page */
    ULONG  pages;
    ULONG *page_key;               /* 0 = pointer missing or out of range */
    UBYTE *page_bad;               /* 1 = unreadable / bad checksum / bad pointer */
    ULONG *ext_key;
    ULONG  ext_count;
    ULONG  bad_pages;
    BOOL   ext_broken;             /* the extension chain ends early */
    BOOL   flag_valid;             /* root bm_flag */
};
BOOL ffs_bm_load(struct FfsVol *v, const UBYTE *rootbuf, struct FfsBm *bm);
void ffs_bm_free(struct FfsBm *bm);
/* TRUE = free.  *known = FALSE when the page holding blk is bad. */
BOOL ffs_bm_isfree(const struct FfsVol *v, const struct FfsBm *bm, ULONG blk, BOOL *known);

/* --- file layout ---------------------------------------------------- */
#define FFS_FB_BADPTR  0x01        /* a data pointer is zero or out of range */
#define FFS_FB_BADEXT  0x02        /* the extension chain is broken */
#define FFS_FB_ABORT   0x04        /* callback asked to stop */
/* cb(ud, kind, seq, blk): kind 0 = data block number seq, 1 = extension
 * block.  Returns FALSE to stop. */
typedef BOOL (*FfsBlkCb)(void *ud, int kind, ULONG seq, ULONG blk);
/* Walk the blocks of the file whose header (block key) is in hdr.
 * Stops at the first damage.  *count = data blocks reported. */
ULONG ffs_file_blocks(struct FfsVol *v, ULONG key, const UBYTE *hdr,
                      FfsBlkCb cb, void *ud, ULONG *count);

/* --- tree walk ------------------------------------------------------ */
/* why codes for entry() */
#define FFS_BAD_RANGE   1          /* pointer outside the volume */
#define FFS_BAD_READ    2          /* device error */
#define FFS_BAD_HEADER  3          /* not a header (type / checksum / own key / kind) */
#define FFS_BAD_LOOP    4          /* already reached another way */
#define FFS_BAD_DEPTH   5          /* directory nesting too deep, not entered */
#define FFS_ODD_PARENT  6          /* parent pointer names another directory */
#define FFS_ODD_HASH    7          /* linked in the wrong hash chain */

struct FfsEntryRef {               /* where the pointer to an entry lives */
    ULONG dir;                     /* directory being listed */
    ULONG slot;                    /* hash slot of the chain */
    ULONG prev;                    /* previous header in the chain, 0 = the slot itself */
    ULONG key;                     /* the entry (pointer value) */
    ULONG next;                    /* its hash chain successor (0 when unknown) */
};

struct FfsVisitor {
    void *ud;
    /* path uses '/' between components, no leading separator, "" = root */
    /* ref says where the pointer to this entry lives (for repairs) */
    BOOL (*dir_enter)(void *ud, const struct FfsEntryRef *ref, const UBYTE *hdr, const char *path);  /* FALSE = skip */
    void (*dir_leave)(void *ud, ULONG key, const UBYTE *hdr, const char *path);
    void (*file)(void *ud, const struct FfsEntryRef *ref, const UBYTE *hdr, const char *path);
    void (*link)(void *ud, const struct FfsEntryRef *ref, const UBYTE *hdr, LONG sectype, const char *path);
    /* codes 1-5 end that hash chain, 6-7 are remarks about a good entry */
    void (*entry)(void *ud, const struct FfsEntryRef *ref, int why, const char *path);
    BOOL (*tick)(void *ud);        /* called per header; FALSE = abort */
};

/* Walk the tree below directory `start` (root or a lost directory).
 * seen: one bit per block, shared between walks - a header is visited
 * once.  start_path prefixes the paths handed to the visitor.
 * FALSE when aborted or out of memory. */
BOOL ffs_walk(struct FfsVol *v, ULONG start, const char *start_path,
              UBYTE *seen, const struct FfsVisitor *vis);

#endif /* FFS_H */
