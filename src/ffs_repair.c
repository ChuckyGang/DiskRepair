/*
 * ffs_repair.c - in-place repair of an OFS/FFS volume.
 *
 * THE RULE: nothing is written unless the result passes the check.
 *
 * The repair runs on the overlay (ffs.h): scan, apply the fixes the scan
 * proposes to blocks in RAM, scan again, until a scan proposes nothing.
 * Then the bitmap is rebuilt from what that last scan found in use.  Only
 * when all of that worked are the original blocks saved to the undo file
 * and the changed ones written.  If anything is left that this code
 * cannot settle (unreadable blocks, damaged bitmap blocks, fixes that
 * keep coming back) the volume is left exactly as it was and the answer
 * is "rescue instead".
 *
 * What it changes: hash chain pointers, parent pointers, a Lost+Found
 * directory, the bitmap.  It never overwrites file data and never erases
 * a header, so whatever it takes out of a directory is still there for a
 * rescue run.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include "clib.h"
#include "be.h"
#include "ffs.h"
#include "ffs_scan.h"
#include "ffs_repair.h"
#include "undo.h"
#include "platform.h"
#include "locale_support.h"

#define MAX_ROUNDS 12

#define R_APPLIED 1
#define R_STALE   0       /* the disk no longer looks like the scan said: next round */
#define R_SKIPPED 2       /* cannot be done, and will not be proposed usefully again */

struct Rep8 {
    struct FfsVol  *v;
    struct FfsScan *s;
    struct Report  *rep;
    UBYTE *a, *b, *c;     /* three scratch blocks */
    ULONG  lf;            /* Lost+Found directory, 0 = not looked up yet */
    ULONG  alloc_next;
    BOOL   dry;
};

static void fixline(struct Rep8 *r, const char *fmt, ...)
{
    char buf[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Rep_Add(r->rep, SEV_FIX, "%s", buf);
}

static BOOL dir_block_ok(const struct FfsVol *v, const UBYTE *b)
{
    LONG st = (LONG)ffs_getl(b, v->nl - FFS_E_SECTYPE);
    return ffs_getl(b, 0) == FFS_T_SHORT && (st == FFS_ST_ROOT || st == FFS_ST_USERDIR) && ffs_sum_ok(b, v->nl);
}

static int do_setptr(struct Rep8 *r, const struct FfsFix *f)
{
    struct FfsVol *v = r->v;
    ULONG blk = f->c ? f->c : f->a;
    ULONG idx = f->c ? v->nl - FFS_E_HASHCHAIN : FFS_L_TABLE + f->b;
    if (!ffs_rd(v, blk, 1, r->a)) return R_STALE;
    if (ffs_getl(r->a, idx) != f->d) return R_STALE;
    ffs_putl(r->a, idx, f->e);
    ffs_sum_set(r->a, v->nl, FFS_L_CHECKSUM);
    if (!ffs_ovl_put(v, blk, r->a)) return R_STALE;
    if (f->why == WHY_CUT)     fixline(r, GS(MSG_FIX_CUT), (unsigned long)f->d, f->what[0] ? f->what : GS(MSG_ROOT_DIR));
    if (f->why == WHY_DAMAGED) fixline(r, GS(MSG_FIX_DAMAGED), f->what);
    return R_APPLIED;
}

static int do_setparent(struct Rep8 *r, const struct FfsFix *f)
{
    struct FfsVol *v = r->v;
    if (!ffs_rd(v, f->a, 1, r->a) || !ffs_header_type(v, r->a, f->a)) return R_STALE;
    if (ffs_getl(r->a, v->nl - FFS_E_PARENT) == f->b) return R_STALE;
    ffs_putl(r->a, v->nl - FFS_E_PARENT, f->b);
    ffs_sum_set(r->a, v->nl, FFS_L_CHECKSUM);
    if (!ffs_ovl_put(v, f->a, r->a)) return R_STALE;
    fixline(r, GS(MSG_FIX_PARENT), f->what);
    return R_APPLIED;
}

/* Link header `key` into directory `dir`, keeping the chain sorted by
 * block number the way FFS files its entries.  quiet: no finding line
 * for a name clash (the caller reports). */
static int insert_entry(struct Rep8 *r, ULONG key, ULONG dir)
{
    struct FfsVol *v = r->v;
    ULONG nl = v->nl, h, cur, prev = 0, guard = 0;
    BOOL  placed = FALSE;
    ULONG ins_prev = 0, ins_next = 0;
    const UBYTE *name;

    if (!ffs_rd(v, key, 1, r->a) || !ffs_header_type(v, r->a, key)) return R_STALE;
    if (!ffs_rd(v, dir, 1, r->b) || !dir_block_ok(v, r->b)) return R_STALE;
    name = r->a + (nl - FFS_E_NAME) * 4;
    h    = ffs_hash(v, name);
    cur  = ffs_getl(r->b, FFS_L_TABLE + h);
    ins_next = cur;
    while (cur) {
        if (cur == key) return R_STALE;                 /* already filed here */
        if (cur < v->reserved || cur >= v->nblocks || ++guard > v->nblocks ||
            !ffs_rd(v, cur, 1, r->c) || !ffs_header_type(v, r->c, cur)) return R_STALE;
        if (ffs_name_eq(v, name, r->c + (nl - FFS_E_NAME) * 4)) return R_SKIPPED;
        if (!placed) {
            if (cur > key) { ins_prev = prev; ins_next = cur; placed = TRUE; }
            else           { ins_prev = cur;  ins_next = ffs_getl(r->c, nl - FFS_E_HASHCHAIN); }
        }
        prev = cur;
        cur  = ffs_getl(r->c, nl - FFS_E_HASHCHAIN);
    }
    ffs_putl(r->a, nl - FFS_E_HASHCHAIN, ins_next);
    ffs_putl(r->a, nl - FFS_E_PARENT, dir);
    ffs_sum_set(r->a, nl, FFS_L_CHECKSUM);
    if (!ffs_ovl_put(v, key, r->a)) return R_STALE;
    if (ins_prev == 0) {
        ffs_putl(r->b, FFS_L_TABLE + h, key);
        ffs_sum_set(r->b, nl, FFS_L_CHECKSUM);
        if (!ffs_ovl_put(v, dir, r->b)) return R_STALE;
    } else {
        if (!ffs_rd(v, ins_prev, 1, r->c)) return R_STALE;
        ffs_putl(r->c, nl - FFS_E_HASHCHAIN, key);
        ffs_sum_set(r->c, nl, FFS_L_CHECKSUM);
        if (!ffs_ovl_put(v, ins_prev, r->c)) return R_STALE;
    }
    return R_APPLIED;
}

/* The Lost+Found directory in the root: found, or made in a free block. */
static ULONG lostfound(struct Rep8 *r)
{
    struct FfsVol *v = r->v;
    ULONG nl = v->nl, cur, guard = 0, blk;
    UBYTE want[FFS_MAXNAME + 2];
    const char *lfname = "Lost+Found";

    if (r->lf) return r->lf;
    want[0] = (UBYTE)strlen(lfname);
    memcpy(want + 1, lfname, want[0]);

    if (!ffs_rd(v, v->root, 1, r->b)) return 0;
    cur = ffs_getl(r->b, FFS_L_TABLE + ffs_hash(v, want));
    while (cur && cur >= v->reserved && cur < v->nblocks && ++guard < v->nblocks) {
        if (!ffs_rd(v, cur, 1, r->c)) break;
        if (ffs_name_eq(v, want, r->c + (nl - FFS_E_NAME) * 4)) {
            if (ffs_header_type(v, r->c, cur) == FFS_ST_USERDIR) { r->lf = cur; return cur; }
            return 0;                                    /* a file of that name: leave it */
        }
        cur = ffs_getl(r->c, nl - FFS_E_HASHCHAIN);
    }
    /* a block nothing claims and no lost header sits in */
    blk = r->alloc_next ? r->alloc_next : v->root + 1;
    for (guard = 0; guard < v->nblocks; guard++, blk++) {
        if (blk >= v->nblocks) blk = v->reserved;
        if (FfsScan_Claim(r->s, blk) == 0 && !FfsScan_FindOrphan(r->s, blk)) break;
    }
    if (guard == v->nblocks) return 0;
    r->alloc_next = blk + 1;
    FfsScan_AddClaim(r->s, blk);

    if (!ffs_rd(v, v->root, 1, r->b)) return 0;
    memset(r->a, 0, v->bsz);
    ffs_putl(r->a, FFS_L_TYPE, FFS_T_SHORT);
    ffs_putl(r->a, FFS_L_OWNKEY, blk);
    memcpy(r->a + (nl - FFS_E_NAME) * 4, want, (ULONG)want[0] + 1);
    memcpy(r->a + (nl - FFS_E_DAYS) * 4, r->b + (nl - FFS_E_DAYS) * 4, 12);
    ffs_putl(r->a, nl - FFS_E_PARENT, v->root);
    ffs_putl(r->a, nl - FFS_E_SECTYPE, (ULONG)FFS_ST_USERDIR);
    ffs_sum_set(r->a, nl, FFS_L_CHECKSUM);
    if (!ffs_ovl_put(v, blk, r->a)) return 0;
    if (insert_entry(r, blk, v->root) != R_APPLIED) return 0;
    fixline(r, GS(MSG_FIX_LOSTFOUND), (unsigned long)blk);
    r->lf = blk;
    return blk;
}

static int do_insert(struct Rep8 *r, const struct FfsFix *f)
{
    ULONG dir = f->b;
    int res;
    if (dir == 0 && (dir = lostfound(r)) == 0) {
        Rep_Add(r->rep, SEV_WARN, GS(MSG_R_NO_LOSTFOUND), f->what);
        return R_SKIPPED;
    }
    res = insert_entry(r, f->a, dir);
    if (res == R_SKIPPED) Rep_Add(r->rep, SEV_WARN, GS(MSG_R_NAMECLASH), f->what, (unsigned long)f->a);
    if (res == R_APPLIED) {
        if (f->why == WHY_REHASH)   fixline(r, GS(MSG_FIX_REHASH), f->what);
        else if (f->b == 0)         fixline(r, GS(MSG_FIX_ATTACH_LF), f->what);
        else                        fixline(r, GS(MSG_FIX_ATTACH), f->what);
    }
    return res;
}

/* Rebuild the bitmap from the claims of scan s (a scan that proposed no
 * fixes).  FALSE: cannot be done here, nothing may be written. */
static BOOL rebuild_bitmap(struct Rep8 *r)
{
    struct FfsVol  *v = r->v;
    struct FfsScan *s = r->s;
    struct FfsBm   *bm = &s->bm;
    ULONG nl = v->nl, page, changed = 0;

    /* A page with a bad checksum or an unreadable one is written afresh;
     * what cannot be worked around is a missing or shared page pointer. */
    if (!s->have_bm || bm->ext_broken) {
        Rep_Add(r->rep, SEV_ERROR, "%s", GS(MSG_R_BITMAP_BLOCKS));
        return FALSE;
    }
    for (page = 0; page < bm->pages; page++)
        if (bm->page_key[page] == 0 || FfsScan_Claim(s, bm->page_key[page]) != 1) {
            Rep_Add(r->rep, SEV_ERROR, "%s", GS(MSG_R_BITMAP_BLOCKS));
            return FALSE;
        }
    for (page = 0; page < bm->pages; page++) {
        ULONG j, first = page * bm->bpb;
        BOOL  diff = FALSE;
        if (bm->page_bad[page]) {
            /* as a freshly formatted page: longs past the volume all ones */
            memset(r->a, 0xFF, v->bsz);
            diff = TRUE;
        } else if (!ffs_rd(v, bm->page_key[page], 1, r->a)) return FALSE;
        for (j = 1; j < nl; j++) {
            ULONG base = first + (j - 1) * 32, bit, w, old;
            if (base >= bm->span) break;
            old = w = ffs_getl(r->a, j);
            if (bm->page_bad[page]) w = 0;
            for (bit = 0; bit < 32 && base + bit < bm->span; bit++) {
                if (FfsScan_Claim(s, v->reserved + base + bit)) w &= ~(1UL << bit);
                else                                             w |= (1UL << bit);
            }
            if (w != old) { ffs_putl(r->a, j, w); diff = TRUE; }
        }
        if (diff) {
            ffs_sum_set(r->a, nl, 0);
            if (!ffs_ovl_put(v, bm->page_key[page], r->a)) return FALSE;
            changed++;
        }
    }
    if (!ffs_rd(v, v->root, 1, r->a)) return FALSE;
    if (ffs_getl(r->a, nl - FFS_E_BMFLAG) != 0xFFFFFFFFUL) {
        ffs_putl(r->a, nl - FFS_E_BMFLAG, 0xFFFFFFFFUL);
        ffs_sum_set(r->a, nl, FFS_L_CHECKSUM);
        if (!ffs_ovl_put(v, v->root, r->a)) return FALSE;
        changed++;
    }
    if (changed) fixline(r, GS(MSG_FIX_BITMAP), (unsigned long)changed);
    return TRUE;
}

static BOOL undo_get(void *ud, ULONG i, ULONG *blk, const UBYTE **orig)
{
    return ffs_ovl_get((const struct FfsVol *)ud, i, blk, orig, NULL);
}

LONG FfsRepair_Run(struct BlockDev *bd, const struct VolumeInfo *vi,
                   const struct RepairOpts *opts, struct Report *rep)
{
    struct FfsVol  v;
    struct FfsScan s;
    struct FfsScanOpts so;
    struct Rep8    r;
    char  err[160];
    LONG  rc = RETURN_FAIL;
    ULONG round, n, i;
    BOOL  have_scan = FALSE, good = FALSE, inhibited = FALSE;

    err[0] = '\0';
    memset(&r, 0, sizeof(r));
    if (!ffs_setup(&v, bd, vi, err, sizeof(err))) {
        Rep_Add(rep, SEV_FATAL, "%s", err);
        ffs_cleanup(&v);
        return RETURN_FAIL;
    }
    if (!opts->dry) {
        if (vi->is_boot) { Rep_Add(rep, SEV_FATAL, GS(MSG_R_BOOT_VOLUME), vi->dosname); goto out; }
        if (!opts->undo_path || !opts->undo_path[0]) { Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_R_NEED_UNDO)); goto out; }
        if (Plat_PathOnVolume(opts->undo_path, vi)) { Rep_Add(rep, SEV_FATAL, GS(MSG_U_SAME_VOLUME), opts->undo_path); goto out; }
        if (!Plat_Inhibit(vi, TRUE, err, sizeof(err))) { Rep_Add(rep, SEV_FATAL, "%s", err); goto out; }
        inhibited = TRUE;
    }
    r.v = &v; r.rep = rep; r.dry = opts->dry;
    r.a = (UBYTE *)AllocVec(v.bsz * 3, MEMF_PUBLIC);
    if (!r.a || !ffs_ovl_begin(&v)) { Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_OUT_OF_MEMORY)); goto out; }
    r.b = r.a + v.bsz; r.c = r.b + v.bsz;

    for (round = 0; round < MAX_ROUNDS; round++) {
        ULONG applied = 0, stale = 0, rerr;
        so.deep = FALSE; so.orphans = TRUE;
        if (!FfsScan_Init(&s, &v, rep, &so)) { Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_OUT_OF_MEMORY)); goto out; }
        have_scan = TRUE;
        s.quiet = (round > 0);
        r.s = &s;
        rerr = v.read_errors;
        if (!FfsScan_Tree(&s)) {
            if (!rep->aborted) Rep_Add(rep, SEV_ERROR, "%s", GS(MSG_R_NO_ROOT));
            goto out;
        }
        FfsScan_Bitmap(&s);
        if (!FfsScan_Orphans(&s)) goto out;
        FfsScan_Attach(&s);
        if (v.read_errors != rerr) { Rep_Add(rep, SEV_ERROR, "%s", GS(MSG_R_READ_ERRORS)); goto out; }
        if (s.nfix == 0) { good = TRUE; break; }
        if (v.dircache) { Rep_Add(rep, SEV_ERROR, "%s", GS(MSG_R_DIRCACHE)); goto out; }
        for (i = 0; i < s.nfix; i++) {
            int res;
            switch (s.fix[i].op) {
            case FIX_SETPTR:    res = do_setptr(&r, &s.fix[i]); break;
            case FIX_SETPARENT: res = do_setparent(&r, &s.fix[i]); break;
            case FIX_INSERT:    res = do_insert(&r, &s.fix[i]); break;
            default:            res = R_SKIPPED; break;
            }
            if (res == R_APPLIED) applied++;
            else if (res == R_STALE) stale++;
        }
        if (applied == 0) {
            if (stale == 0) { good = TRUE; break; }       /* only things that stay as they are */
            Rep_Add(rep, SEV_ERROR, "%s", GS(MSG_R_UNRESOLVED));
            goto out;
        }
        FfsScan_Free(&s);
        have_scan = FALSE;
    }
    if (!good) { Rep_Add(rep, SEV_ERROR, "%s", GS(MSG_R_UNRESOLVED)); goto out; }
    if (!rebuild_bitmap(&r)) goto out;

    n = ffs_ovl_count(&v);
    if (n == 0) {
        Rep_Add(rep, SEV_INFO, "%s", GS(MSG_R_NOTHING));
        if (have_scan && s.deleted) Rep_Add(rep, SEV_INFO, GS(MSG_R_DELETED), (unsigned long)s.deleted);
        rc = RETURN_OK;
    } else if (opts->dry) {
        Rep_Add(rep, SEV_INFO, GS(MSG_R_DRY_SUMMARY), (unsigned long)n);
        rc = RETURN_WARN;
    } else {
        if (!Undo_Write(opts->undo_path, vi, v.nblocks, v.bsz, n, undo_get, &v, err, sizeof(err))) {
            Rep_Add(rep, SEV_FATAL, "%s", err);
            goto out;
        }
        BlockDev_ArmWrites(bd);
        rc = RETURN_OK;
        for (i = 0; i < n; i++) {
            const UBYTE *now;
            ULONG blk;
            ffs_ovl_get(&v, i, &blk, NULL, &now);
            if (!BlockDev_WriteBlocks(bd, v.part_start + blk * v.spb, v.spb, now)) {
                Rep_Add(rep, SEV_FATAL, GS(MSG_R_WRITE_FAILED), (unsigned long)blk, opts->undo_path);
                rc = RETURN_FAIL;
                break;
            }
        }
        BlockDev_Update(bd);
        if (rc == RETURN_OK) Rep_Add(rep, SEV_INFO, GS(MSG_R_DONE), (unsigned long)n, opts->undo_path);
    }
    if (rc != RETURN_FAIL && s.crosslinked)
        Rep_Add(rep, SEV_WARN, GS(MSG_R_CROSSLINK_LEFT), (unsigned long)s.crosslinked);

out:
    if (rc == RETURN_FAIL && !rep->aborted) Rep_Add(rep, SEV_INFO, "%s", GS(MSG_R_UNTOUCHED));
    if (have_scan) FfsScan_Free(&s);
    if (r.a) FreeVec(r.a);
    if (inhibited) Plat_Inhibit(vi, FALSE, NULL, 0);
    ffs_cleanup(&v);
    return rc;
}
