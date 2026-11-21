/*
 * ffs_rescue.c - copy what can still be read from an OFS/FFS volume to
 * another place.  The damaged volume is never written.
 *
 * Order of work:
 *   1. a quiet scan, to know which headers the directory tree reaches
 *   2. the tree from the root, directory by directory, into the
 *      destination
 *   3. headers the tree does not reach: lost directories (with all they
 *      still list) and lost files go to "Lost+Found", deleted files that
 *      are still on the disk to "Deleted" when asked for
 *
 * A file is copied as far as its block list can be followed.  A block
 * the device refuses becomes zeros in the copy; such files are listed as
 * partial so nobody mistakes them for good copies.
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
#include "platform.h"
#include "locale_support.h"

#define RUN_MAX 32

#define BIT(a, k)    ((a)[(k) >> 3] & (1 << ((k) & 7)))
#define SETBIT(a, k) ((a)[(k) >> 3] |= (UBYTE)(1 << ((k) & 7)))

struct Resc {
    struct FfsVol      *v;
    struct Report      *rep;
    struct RescueStats *st;
    UBYTE *seen;                 /* headers this run has copied */
    UBYTE *buf;                  /* RUN_MAX blocks */
    UBYTE *hdr2;                 /* link targets, lost headers */
    /* file being copied */
    APTR   fh;
    ULONG  key;
    ULONG  remaining;
    ULONG  run_start, run_len, run_seq;
    ULONG  holes;
    BOOL   write_failed;
};

static void meta(struct Resc *r, const UBYTE *hdr, const char *path)
{
    struct FfsVol *v = r->v;
    char comment[82];
    const UBYTE *c = hdr + (v->nl - FFS_E_COMMENT) * 4;
    ULONG n = c[0], i;
    if (n > 79) n = 79;
    for (i = 0; i < n; i++) comment[i] = (char)(c[1 + i] < 32 ? ' ' : c[1 + i]);
    comment[n] = '\0';
    Out_SetMeta(path, ffs_getl(hdr, v->nl - FFS_E_PROTECT), ffs_getl(hdr, v->nl - FFS_E_DAYS),
                ffs_getl(hdr, v->nl - FFS_E_DAYS + 1), ffs_getl(hdr, v->nl - FFS_E_DAYS + 2), comment);
}

static void put_bytes(struct Resc *r, const UBYTE *p, ULONG len)
{
    if (len > r->remaining) len = r->remaining;
    if (len == 0 || r->write_failed) return;
    if (!Out_Write(r->fh, p, len)) r->write_failed = TRUE;
    r->remaining -= len;
}

static void put_block(struct Resc *r, const UBYTE *blk, ULONG seq)
{
    struct FfsVol *v = r->v;
    if (!v->ofs) { put_bytes(r, blk, v->bsz); return; }
    if (ffs_getl(blk, 0) != FFS_T_DATA || ffs_getl(blk, 1) != r->key ||
        ffs_getl(blk, 2) != seq + 1 || !ffs_sum_ok(blk, v->nl)) r->holes++;
    put_bytes(r, blk + FFS_OFS_HDR, v->datasize);
}

static void flush_run(struct Resc *r)
{
    struct FfsVol *v = r->v;
    ULONG k;
    if (r->run_len == 0) return;
    if (ffs_rd(v, r->run_start, r->run_len, r->buf)) {
        if (!v->ofs) put_bytes(r, r->buf, r->run_len * v->bsz);
        else for (k = 0; k < r->run_len; k++) put_block(r, r->buf + k * v->bsz, r->run_seq + k);
    } else {
        for (k = 0; k < r->run_len; k++) {
            if (!ffs_rd(v, r->run_start + k, 1, r->buf)) {
                memset(r->buf, 0, v->bsz);
                r->holes++;
                put_bytes(r, r->buf, v->datasize);
            } else {
                put_block(r, r->buf, r->run_seq + k);
            }
        }
    }
    r->run_len = 0;
}

static BOOL copy_cb(void *ud, int kind, ULONG seq, ULONG blk)
{
    struct Resc *r = (struct Resc *)ud;
    if (kind != 0) return TRUE;
    if (r->remaining == 0 || r->write_failed) return FALSE;
    if (r->run_len && (blk != r->run_start + r->run_len || r->run_len == RUN_MAX)) flush_run(r);
    if (r->run_len == 0) { r->run_start = blk; r->run_seq = seq; }
    r->run_len++;
    return TRUE;
}

/* Pick a free name at the destination: path, or path_<key>. */
static void free_name(char *out, ULONG outlen, const char *path, ULONG key)
{
    strncpy(out, path, outlen - 1);
    out[outlen - 1] = '\0';
    if (Out_Exists(out)) snprintf(out, outlen, "%s_%lu", path, (unsigned long)key);
}

static void copy_file(struct Resc *r, ULONG key, const UBYTE *hdr, const char *path)
{
    struct FfsVol *v = r->v;
    char  dest[FFS_PATHMAX + 16];
    ULONG size = ffs_getl(hdr, v->nl - FFS_E_BYTESIZE), flags;

    free_name(dest, sizeof(dest), path, key);
    r->fh = Out_Create(dest);
    if (!r->fh) {
        Rep_Add(r->rep, SEV_ERROR, GS(MSG_S_CANNOT_CREATE), dest);
        r->st->failed++;
        return;
    }
    r->key = key; r->remaining = size; r->run_len = 0; r->holes = 0; r->write_failed = FALSE;
    flags = ffs_file_blocks(v, key, hdr, copy_cb, r, NULL);
    flush_run(r);
    Out_Close(r->fh);
    r->fh = NULL;
    meta(r, hdr, dest);
    r->st->files++;
    r->st->bytes_kb += (size - r->remaining + 1023) / 1024;
    if (r->write_failed) {
        Rep_Add(r->rep, SEV_ERROR, GS(MSG_S_WRITE_FAILED), dest);
        r->st->failed++;
    } else if (r->remaining || r->holes || (flags & (FFS_FB_BADPTR | FFS_FB_BADEXT))) {
        Rep_Add(r->rep, SEV_WARN, GS(MSG_S_PARTIAL), dest,
                (unsigned long)(size - r->remaining), (unsigned long)size, (unsigned long)r->holes);
        r->st->partial++;
    }
    Rep_Progress(r->rep, r->st->files, 0, dest);
}

static BOOL r_tick(void *ud) { return !((struct Resc *)ud)->rep->aborted; }

static BOOL r_dir_enter(void *ud, const struct FfsEntryRef *ref, const UBYTE *hdr, const char *path)
{
    struct Resc *r = (struct Resc *)ud;
    (void)ref; (void)hdr;
    if (!Out_MkDir(path)) {
        Rep_Add(r->rep, SEV_ERROR, GS(MSG_S_CANNOT_MKDIR), path);
        r->st->failed++;
        return FALSE;
    }
    r->st->dirs++;
    return TRUE;
}

static void r_dir_leave(void *ud, ULONG key, const UBYTE *hdr, const char *path)
{
    (void)key;
    meta((struct Resc *)ud, hdr, path);
}

static void r_file(void *ud, const struct FfsEntryRef *ref, const UBYTE *hdr, const char *path)
{
    copy_file((struct Resc *)ud, ref->key, hdr, path);
}

static void r_link(void *ud, const struct FfsEntryRef *ref, const UBYTE *hdr, LONG sectype, const char *path)
{
    struct Resc *r = (struct Resc *)ud;
    struct FfsVol *v = r->v;
    (void)ref;
    if (sectype == FFS_ST_LINKFILE) {
        /* a hard link to a file becomes a copy of that file */
        ULONG t = ffs_getl(hdr, v->nl - FFS_E_REALENTRY);
        if (t >= v->reserved && t < v->nblocks && ffs_rd(v, t, 1, r->hdr2) &&
            ffs_header_type(v, r->hdr2, t) == FFS_ST_FILE) {
            copy_file(r, t, r->hdr2, path);
            return;
        }
    }
    Rep_Add(r->rep, SEV_WARN, GS(MSG_S_LINK_SKIPPED), path);
}

static const struct FfsVisitor *resc_visitor(struct Resc *r, struct FfsVisitor *vis)
{
    memset(vis, 0, sizeof(*vis));
    vis->ud = r;
    vis->dir_enter = r_dir_enter;
    vis->dir_leave = r_dir_leave;
    vis->file = r_file;
    vis->link = r_link;
    vis->tick = r_tick;
    return vis;
}

static BOOL ensure_dir(struct Resc *r, const char *name, BOOL *made)
{
    if (*made) return TRUE;
    if (!Out_MkDir(name)) { Rep_Add(r->rep, SEV_ERROR, GS(MSG_S_CANNOT_MKDIR), name); return FALSE; }
    *made = TRUE;
    return TRUE;
}

static void rescue_lost_dir(struct Resc *r, struct FfsOrphan *o, const char *prefix)
{
    struct FfsVol *v = r->v;
    struct FfsVisitor vis;
    char nm[FFS_MAXNAME + 2], path[FFS_MAXNAME + 48], dest[FFS_MAXNAME + 64];

    if (!ffs_rd(v, o->key, 1, r->hdr2) || ffs_header_type(v, r->hdr2, o->key) != FFS_ST_USERDIR) return;
    ffs_name(r->hdr2 + (v->nl - FFS_E_NAME) * 4, FFS_MAXNAME, nm, sizeof(nm));
    if (prefix[0]) snprintf(path, sizeof(path), "%s/%s", prefix, nm);
    else           snprintf(path, sizeof(path), "%s", nm);
    /* two lost directories of one name must not be merged */
    free_name(dest, sizeof(dest), path, o->key);
    if (!Out_MkDir(dest)) { Rep_Add(r->rep, SEV_ERROR, GS(MSG_S_CANNOT_MKDIR), dest); r->st->failed++; return; }
    r->st->dirs++;
    r->st->lost++;
    ffs_walk(v, o->key, dest, r->seen, resc_visitor(r, &vis));
    if (ffs_rd(v, o->key, 1, r->hdr2)) meta(r, r->hdr2, dest);
}

LONG FfsRescue_Run(struct BlockDev *bd, const struct VolumeInfo *vi,
                   const struct RescueOpts *opts, struct Report *rep,
                   struct RescueStats *st)
{
    struct FfsVol  v;
    struct FfsScan s;
    struct FfsScanOpts so;
    struct FfsVisitor vis;
    struct Resc r;
    char  err[96];
    LONG  rc = RETURN_FAIL;
    BOOL  tree, have_scan = FALSE, lf_made = FALSE, del_made = FALSE;
    ULONG i, pass;

    memset(st, 0, sizeof(*st));
    memset(&r, 0, sizeof(r));
    err[0] = '\0';
    if (!ffs_setup(&v, bd, vi, err, sizeof(err))) {
        Rep_Add(rep, SEV_FATAL, "%s", err);
        ffs_cleanup(&v);
        return RETURN_FAIL;
    }
    so.deep = FALSE; so.orphans = TRUE;
    r.v = &v; r.rep = rep; r.st = st;
    r.seen = (UBYTE *)AllocVec(v.nblocks / 8 + 2, MEMF_PUBLIC | MEMF_CLEAR);
    r.buf  = (UBYTE *)AllocVec(v.bsz * (RUN_MAX + 1), MEMF_PUBLIC);
    if (!r.seen || !r.buf || !FfsScan_Init(&s, &v, rep, &so)) {
        Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_OUT_OF_MEMORY));
        goto out;
    }
    r.hdr2 = r.buf + v.bsz * RUN_MAX;
    have_scan = TRUE;
    s.quiet = TRUE;

    tree = FfsScan_Tree(&s);
    if (rep->aborted) goto out;
    if (!tree) Rep_Add(rep, SEV_WARN, "%s", GS(MSG_S_NO_ROOT));
    if (!FfsScan_Orphans(&s)) goto out;

    if (tree) {
        if (!ffs_walk(&v, v.root, "", r.seen, resc_visitor(&r, &vis))) goto out;
    }
    /* lost directories: first those no other lost directory holds, then
     * whatever is still not copied */
    for (pass = 0; pass < 2 && !rep->aborted; pass++) {
        for (i = 0; i < s.norph && !rep->aborted; i++) {
            struct FfsOrphan *o = &s.orph[i];
            ULONG parent;
            const char *prefix = GS(MSG_LOSTFOUND_NAME);
            if (o->sectype != FFS_ST_USERDIR || o->deleted || BIT(r.seen, o->key)) continue;
            if (!ffs_rd(&v, o->key, 1, r.hdr2)) continue;
            parent = ffs_getl(r.hdr2, v.nl - FFS_E_PARENT);
            if (pass == 0) {
                struct FfsOrphan *po = FfsScan_FindOrphan(&s, parent);
                if (po && po != o && po->sectype == FFS_ST_USERDIR && !po->deleted) continue;
            }
            if (!tree && parent == v.root_calc) prefix = "";      /* was in the root directory */
            else if (!ensure_dir(&r, GS(MSG_LOSTFOUND_NAME), &lf_made)) continue;
            rescue_lost_dir(&r, o, prefix);
        }
    }
    for (i = 0; i < s.norph && !rep->aborted; i++) {
        struct FfsOrphan *o = &s.orph[i];
        char nm[FFS_MAXNAME + 2], path[FFS_MAXNAME + 48];
        const char *prefix;
        if (o->sectype != FFS_ST_FILE || BIT(r.seen, o->key)) continue;
        if (o->deleted && !opts->deleted) continue;
        if (!ffs_rd(&v, o->key, 1, r.hdr2) || ffs_header_type(&v, r.hdr2, o->key) != FFS_ST_FILE) continue;
        if (o->deleted) {
            if (!ensure_dir(&r, GS(MSG_DELETED_NAME), &del_made)) continue;
            prefix = GS(MSG_DELETED_NAME);
            st->deleted++;
        } else if (!tree && ffs_getl(r.hdr2, v.nl - FFS_E_PARENT) == v.root_calc) {
            prefix = "";
            st->lost++;
        } else {
            if (!ensure_dir(&r, GS(MSG_LOSTFOUND_NAME), &lf_made)) continue;
            prefix = GS(MSG_LOSTFOUND_NAME);
            st->lost++;
        }
        ffs_name(r.hdr2 + (v.nl - FFS_E_NAME) * 4, FFS_MAXNAME, nm, sizeof(nm));
        if (prefix[0]) snprintf(path, sizeof(path), "%s/%s", prefix, nm);
        else           snprintf(path, sizeof(path), "%s", nm);
        SETBIT(r.seen, o->key);
        /* hdr2 is this file's header; copy_file() does not use hdr2 */
        copy_file(&r, o->key, r.hdr2, path);
    }
    if (v.read_errors) Rep_Add(rep, SEV_WARN, GS(MSG_F_READERRORS), (unsigned long)v.read_errors);
    if (rep->aborted)                     rc = RETURN_WARN;
    else if (st->failed)                  rc = RETURN_ERROR;
    else if (st->partial || !tree)        rc = RETURN_WARN;
    else                                  rc = RETURN_OK;
out:
    if (have_scan) FfsScan_Free(&s);
    if (r.seen) FreeVec(r.seen);
    if (r.buf)  FreeVec(r.buf);
    ffs_cleanup(&v);
    return rc;
}
