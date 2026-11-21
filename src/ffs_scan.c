/*
 * ffs_scan.c - see ffs_scan.h.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include "clib.h"
#include "be.h"
#include "ffs.h"
#include "ffs_scan.h"
#include "locale_support.h"

#define CHUNK 32

#define BIT(a, k)    ((a)[(k) >> 3] & (1 << ((k) & 7)))
#define SETBIT(a, k) ((a)[(k) >> 3] |= (UBYTE)(1 << ((k) & 7)))

#define NOTE(s, sev, ...) do { if (!(s)->quiet) Rep_Add((s)->rep, (sev), __VA_ARGS__); } while (0)

static const char *shown(const char *path) { return path[0] ? path : GS(MSG_ROOT_DIR); }

UBYTE FfsScan_Claim(const struct FfsScan *s, ULONG blk)
{
    return (UBYTE)((s->claim[blk >> 2] >> ((blk & 3) * 2)) & 3);
}

static void set_claim(struct FfsScan *s, ULONG blk, UBYTE val)
{
    UBYTE sh = (UBYTE)((blk & 3) * 2);
    s->claim[blk >> 2] = (UBYTE)((s->claim[blk >> 2] & ~(3 << sh)) | (val << sh));
}

void FfsScan_AddClaim(struct FfsScan *s, ULONG blk)
{
    if (blk < s->v->nblocks && FfsScan_Claim(s, blk) == 0) set_claim(s, blk, 1);
}

static void claim(struct FfsScan *s, ULONG blk, const char *path)
{
    UBYTE c = FfsScan_Claim(s, blk);
    if (c == 0) { set_claim(s, blk, 1); return; }
    if (c == 1) set_claim(s, blk, 2);
    s->crosslinked++;
    NOTE(s, SEV_ERROR, GS(MSG_F_CROSSLINK), shown(path), (unsigned long)blk);
}

static void add_fix(struct FfsScan *s, UBYTE op, UBYTE why, ULONG a, ULONG b, ULONG c, ULONG d, ULONG e,
                    const char *what)
{
    struct FfsFix *f;
    if (s->nfix == s->capfix) {
        ULONG ncap = s->capfix ? s->capfix * 2 : 64;
        struct FfsFix *n = (struct FfsFix *)AllocVec(ncap * sizeof(struct FfsFix), MEMF_PUBLIC);
        if (!n) return;
        if (s->fix) { memcpy(n, s->fix, s->nfix * sizeof(struct FfsFix)); FreeVec(s->fix); }
        s->fix = n; s->capfix = ncap;
    }
    f = &s->fix[s->nfix++];
    f->op = op; f->why = why; f->a = a; f->b = b; f->c = c; f->d = d; f->e = e;
    strncpy(f->what, what ? what : "", sizeof(f->what) - 1);
    f->what[sizeof(f->what) - 1] = '\0';
}

BOOL FfsScan_Init(struct FfsScan *s, struct FfsVol *v, struct Report *rep, const struct FfsScanOpts *o)
{
    memset(s, 0, sizeof(*s));
    s->v = v; s->rep = rep;
    if (o) s->o = *o;
    s->claim   = (UBYTE *)AllocVec(v->nblocks / 4 + 2, MEMF_PUBLIC | MEMF_CLEAR);
    s->seen    = (UBYTE *)AllocVec(v->nblocks / 8 + 2, MEMF_PUBLIC | MEMF_CLEAR);
    s->isdir   = (UBYTE *)AllocVec(v->nblocks / 8 + 2, MEMF_PUBLIC | MEMF_CLEAR);
    s->rootbuf = (UBYTE *)AllocVec(v->bsz, MEMF_PUBLIC);
    s->iobuf   = (UBYTE *)AllocVec(v->bsz * CHUNK, MEMF_PUBLIC);
    if (!s->claim || !s->seen || !s->isdir || !s->rootbuf || !s->iobuf) { FfsScan_Free(s); return FALSE; }
    return TRUE;
}

void FfsScan_Free(struct FfsScan *s)
{
    if (s->claim)   FreeVec(s->claim);
    if (s->seen)    FreeVec(s->seen);
    if (s->isdir)   FreeVec(s->isdir);
    if (s->rootbuf) FreeVec(s->rootbuf);
    if (s->iobuf)   FreeVec(s->iobuf);
    if (s->fix)     FreeVec(s->fix);
    if (s->orph)    FreeVec(s->orph);
    if (s->have_bm) ffs_bm_free(&s->bm);
    memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------ */
/* tree                                                                 */
/* ------------------------------------------------------------------ */

struct FileCtx {
    struct FfsScan *s;
    const char *path;
    ULONG key;
    ULONG conflicts;
    ULONG bad_data;
    int   mode;              /* 0 claim, 1 probe (count conflicts only), 2 claim without findings */
};

static BOOL file_cb(void *ud, int kind, ULONG seq, ULONG blk)
{
    struct FileCtx *c = (struct FileCtx *)ud;
    struct FfsScan *s = c->s;
    if (c->mode == 1) { if (FfsScan_Claim(s, blk)) c->conflicts++; return TRUE; }
    if (c->mode == 2) { FfsScan_AddClaim(s, blk); return TRUE; }
    claim(s, blk, c->path);
    if (kind == 0 && s->o.deep) {
        struct FfsVol *v = s->v;
        if (!ffs_rd(v, blk, 1, s->iobuf)) c->bad_data++;
        else if (v->ofs && (ffs_getl(s->iobuf, 0) != FFS_T_DATA || ffs_getl(s->iobuf, 1) != c->key ||
                            ffs_getl(s->iobuf, 2) != seq + 1 || !ffs_sum_ok(s->iobuf, v->nl))) c->bad_data++;
    }
    return TRUE;
}

static void claim_dircache(struct FfsScan *s, ULONG dir, const UBYTE *hdr, const char *path)
{
    struct FfsVol *v = s->v;
    ULONG blk = ffs_getl(hdr, v->nl - FFS_E_EXTENSION), guard = 0;
    while (blk) {
        if (blk < v->reserved || blk >= v->nblocks || ++guard > v->nblocks ||
            !ffs_rd(v, blk, 1, v->extbuf) || ffs_getl(v->extbuf, 0) != FFS_T_DIRCACHE ||
            ffs_getl(v->extbuf, 1) != blk || ffs_getl(v->extbuf, 2) != dir || !ffs_sum_ok(v->extbuf, v->nl)) {
            NOTE(s, SEV_ERROR, GS(MSG_F_DIRCACHE), shown(path), (unsigned long)blk);
            s->structural++;
            return;
        }
        claim(s, blk, path);
        blk = ffs_getl(v->extbuf, 4);
    }
}

static BOOL v_tick(void *ud)
{
    struct FfsScan *s = (struct FfsScan *)ud;
    if ((++s->ticks & 63) == 0)
        return Rep_Progress(s->rep, s->ticks, 0, GS(MSG_PROG_TREE));
    return TRUE;
}

static BOOL v_dir_enter(void *ud, const struct FfsEntryRef *ref, const UBYTE *hdr, const char *path)
{
    struct FfsScan *s = (struct FfsScan *)ud;
    claim(s, ref->key, path);
    SETBIT(s->isdir, ref->key);
    s->dirs++;
    if (s->v->dircache) claim_dircache(s, ref->key, hdr, path);
    return TRUE;
}

/* Claim the blocks of the file in hdr and report what is wrong with it.
 * Returns the ffs_file_blocks flags. */
static ULONG scan_file(struct FfsScan *s, ULONG key, const UBYTE *hdr, const char *path)
{
    struct FfsVol *v = s->v;
    struct FileCtx c;
    ULONG count = 0, flags, size, expect;

    c.s = s; c.path = path; c.key = key; c.conflicts = 0; c.bad_data = 0; c.mode = 0;
    claim(s, key, path);
    flags  = ffs_file_blocks(v, key, hdr, file_cb, &c, &count);
    size   = ffs_getl(hdr, v->nl - FFS_E_BYTESIZE);
    expect = (size + v->datasize - 1) / v->datasize;
    if (size == 0) expect = 0;
    if (flags & FFS_FB_BADPTR)
        NOTE(s, SEV_ERROR, GS(MSG_F_FILE_BADPTR), shown(path), (unsigned long)count, (unsigned long)expect);
    else if (flags & FFS_FB_BADEXT)
        NOTE(s, SEV_ERROR, GS(MSG_F_FILE_BADEXT), shown(path), (unsigned long)count, (unsigned long)expect);
    else if (count < expect) {
        NOTE(s, SEV_ERROR, GS(MSG_F_FILE_SHORT), shown(path), (unsigned long)expect, (unsigned long)count);
        s->damaged_files++;
    } else if (count > expect)
        NOTE(s, SEV_WARN, GS(MSG_F_FILE_LONG), shown(path), (unsigned long)count, (unsigned long)expect);
    if (c.bad_data) {
        NOTE(s, SEV_ERROR, GS(MSG_F_FILE_DATA), shown(path), (unsigned long)c.bad_data);
        s->bad_data += c.bad_data;
    }
    return flags & (FFS_FB_BADPTR | FFS_FB_BADEXT);
}

static void v_file(void *ud, const struct FfsEntryRef *ref, const UBYTE *hdr, const char *path)
{
    struct FfsScan *s = (struct FfsScan *)ud;
    s->files++;
    if (scan_file(s, ref->key, hdr, path)) {
        /* A block list the filing system cannot follow stops the validator:
         * the entry has to leave its directory.  The header stays on the
         * disk, so a rescue run still finds the file. */
        s->damaged_files++;
        s->structural++;
        add_fix(s, FIX_SETPTR, WHY_DAMAGED, ref->dir, ref->slot, ref->prev, ref->key, ref->next, path);
    }
}

static void v_link(void *ud, const struct FfsEntryRef *ref, const UBYTE *hdr, LONG sectype, const char *path)
{
    struct FfsScan *s = (struct FfsScan *)ud;
    struct FfsVol *v = s->v;
    s->links++;
    claim(s, ref->key, path);
    if (sectype == FFS_ST_LINKFILE || sectype == FFS_ST_LINKDIR) {
        ULONG t = ffs_getl(hdr, v->nl - FFS_E_REALENTRY);
        if (t < v->reserved || t >= v->nblocks) {
            NOTE(s, SEV_ERROR, GS(MSG_F_LINK_BAD), shown(path), (unsigned long)t);
            s->structural++;
            add_fix(s, FIX_SETPTR, WHY_DAMAGED, ref->dir, ref->slot, ref->prev, ref->key, ref->next, path);
        }
    }
}

static void v_entry(void *ud, const struct FfsEntryRef *ref, int why, const char *path)
{
    struct FfsScan *s = (struct FfsScan *)ud;
    switch (why) {
    case FFS_BAD_RANGE:  NOTE(s, SEV_ERROR, GS(MSG_F_ENTRY_RANGE),  shown(path), (unsigned long)ref->key); break;
    case FFS_BAD_READ:   NOTE(s, SEV_ERROR, GS(MSG_F_ENTRY_READ),   shown(path), (unsigned long)ref->key); break;
    case FFS_BAD_HEADER: NOTE(s, SEV_ERROR, GS(MSG_F_ENTRY_HEADER), shown(path), (unsigned long)ref->key); break;
    case FFS_BAD_LOOP:   NOTE(s, SEV_ERROR, GS(MSG_F_ENTRY_LOOP),   shown(path), (unsigned long)ref->key); break;
    case FFS_BAD_DEPTH:  NOTE(s, SEV_WARN,  GS(MSG_F_DEPTH), shown(path)); return;
    case FFS_ODD_PARENT:
        NOTE(s, SEV_WARN, GS(MSG_F_PARENT), shown(path));
        s->structural++;
        add_fix(s, FIX_SETPARENT, WHY_PARENT, ref->key, ref->dir, 0, 0, 0, path);
        return;
    case FFS_ODD_HASH:
        NOTE(s, SEV_ERROR, GS(MSG_F_HASH), shown(path));
        s->structural++;
        add_fix(s, FIX_SETPTR, WHY_UNHASH, ref->dir, ref->slot, ref->prev, ref->key, ref->next, path);
        add_fix(s, FIX_INSERT, WHY_REHASH, ref->key, ref->dir, 0, 0, 0, path);
        return;
    default: return;
    }
    s->bad_entries++;
    s->structural++;
    add_fix(s, FIX_SETPTR, WHY_CUT, ref->dir, ref->slot, ref->prev, ref->key, 0, path);
}

static const struct FfsVisitor *scan_visitor(struct FfsScan *s, struct FfsVisitor *vis)
{
    memset(vis, 0, sizeof(*vis));
    vis->ud = s;
    vis->dir_enter = v_dir_enter;
    vis->file  = v_file;
    vis->link  = v_link;
    vis->entry = v_entry;
    vis->tick  = v_tick;
    return vis;
}

BOOL FfsScan_Tree(struct FfsScan *s)
{
    struct FfsVol *v = s->v;
    struct FfsVisitor vis;
    ULONG i;

    if (ffs_rd(v, 0, 1, s->iobuf) && (ffs_getl(s->iobuf, 0) & 0xFFFFFF00UL) != 0x444F5300UL)
        NOTE(s, SEV_WARN, GS(MSG_F_BOOTBLOCK), (unsigned long)ffs_getl(s->iobuf, 0));

    if (!ffs_find_root(v, s->rootbuf)) {
        NOTE(s, SEV_FATAL, GS(MSG_F_NOROOT), (unsigned long)v->root_calc);
        return FALSE;
    }
    s->have_root = TRUE;
    if (v->root != v->root_calc)
        NOTE(s, SEV_WARN, GS(MSG_F_ROOT_MOVED), (unsigned long)v->root, (unsigned long)v->root_calc);
    claim(s, v->root, "");
    SETBIT(s->isdir, v->root);
    if (v->dircache) claim_dircache(s, v->root, s->rootbuf, "");

    if (ffs_bm_load(v, s->rootbuf, &s->bm)) {
        s->have_bm = TRUE;
        for (i = 0; i < s->bm.pages; i++)
            if (s->bm.page_key[i]) claim(s, s->bm.page_key[i], GS(MSG_BITMAP_NAME));
        for (i = 0; i < s->bm.ext_count; i++) claim(s, s->bm.ext_key[i], GS(MSG_BITMAP_NAME));
    }
    if (!ffs_walk(v, v->root, "", s->seen, scan_visitor(s, &vis))) return FALSE;
    return !s->rep->aborted;
}

void FfsScan_Bitmap(struct FfsScan *s)
{
    struct FfsVol *v = s->v;
    ULONG blk;
    if (!s->have_bm) return;
    if (!s->bm.flag_valid) { NOTE(s, SEV_WARN, GS(MSG_F_BM_INVALID)); s->bitmap_dirty = TRUE; }
    if (s->bm.ext_broken)  { NOTE(s, SEV_ERROR, GS(MSG_F_BM_EXTBROKEN)); s->bitmap_dirty = TRUE; }
    if (s->bm.bad_pages) {
        NOTE(s, SEV_ERROR, GS(MSG_F_BM_BADPAGES), (unsigned long)s->bm.bad_pages, (unsigned long)s->bm.pages);
        s->bitmap_dirty = TRUE;
    }
    for (blk = v->reserved; blk < v->nblocks; blk++) {
        BOOL known, isfree = ffs_bm_isfree(v, &s->bm, blk, &known);
        UBYTE c;
        if (!known) continue;
        c = FfsScan_Claim(s, blk);
        if (c && isfree) s->used_but_free++;
        else if (!c && !isfree) s->free_but_used++;
    }
    if (s->used_but_free) {
        NOTE(s, SEV_ERROR, GS(MSG_F_BM_USEDFREE), (unsigned long)s->used_but_free);
        s->bitmap_dirty = TRUE;
    }
}

/* ------------------------------------------------------------------ */
/* orphans                                                              */
/* ------------------------------------------------------------------ */

struct FfsOrphan *FfsScan_FindOrphan(struct FfsScan *s, ULONG key)
{
    ULONG lo = 0, hi = s->norph;
    while (lo < hi) {
        ULONG mid = (lo + hi) / 2;
        if (s->orph[mid].key == key) return &s->orph[mid];
        if (s->orph[mid].key < key) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

static BOOL add_orphan(struct FfsScan *s, ULONG key, LONG st)
{
    struct FfsOrphan *o;
    BOOL known, isfree = FALSE;
    if (s->norph == s->caporph) {
        ULONG ncap = s->caporph ? s->caporph * 2 : 256;
        struct FfsOrphan *n = (struct FfsOrphan *)AllocVec(ncap * sizeof(struct FfsOrphan), MEMF_PUBLIC);
        if (!n) return FALSE;
        if (s->orph) { memcpy(n, s->orph, s->norph * sizeof(struct FfsOrphan)); FreeVec(s->orph); }
        s->orph = n; s->caporph = ncap;
    }
    o = &s->orph[s->norph++];
    o->key = key; o->sectype = st; o->state = 0;
    /* The bits are used even when the flag says "not validated": a stale
     * bitmap is still right about nearly every block, and treating every
     * old deleted file as lost would put years of leftovers back. */
    if (s->have_bm) isfree = ffs_bm_isfree(s->v, &s->bm, key, &known);
    o->deleted = (UBYTE)((s->have_bm && known && isfree) ? 1 : 0);
    if (o->deleted) s->deleted++; else s->lost++;
    return TRUE;
}

BOOL FfsScan_Orphans(struct FfsScan *s)
{
    struct FfsVol *v = s->v;
    ULONG blk = v->reserved;
    while (blk < v->nblocks) {
        ULONG n = v->nblocks - blk, k;
        BOOL  whole, any = FALSE;
        if (n > CHUNK) n = CHUNK;
        for (k = 0; k < n; k++)
            if (!BIT(s->seen, blk + k) && FfsScan_Claim(s, blk + k) == 0) { any = TRUE; break; }
        if (any) {
            whole = ffs_rd(v, blk, n, s->iobuf);
            for (k = 0; k < n; k++) {
                ULONG b = blk + k;
                const UBYTE *p = s->iobuf + k * v->bsz;
                LONG st;
                if (BIT(s->seen, b) || FfsScan_Claim(s, b)) continue;
                if (!whole) {
                    if (!ffs_rd(v, b, 1, v->extbuf)) continue;
                    p = v->extbuf;
                }
                st = ffs_header_type(v, p, b);
                if (st && !add_orphan(s, b, st)) return FALSE;
            }
        }
        blk += n;
        if (!Rep_Progress(s->rep, blk, v->nblocks, GS(MSG_PROG_ORPHANS))) return FALSE;
    }
    return TRUE;
}

static BOOL reachable_dir(const struct FfsScan *s, ULONG key)
{
    return key >= s->v->reserved && key < s->v->nblocks && BIT(s->isdir, key);
}

static void attach_dir(struct FfsScan *s, struct FfsOrphan *o, UBYTE *hdr)
{
    struct FfsVol *v = s->v;
    struct FfsVisitor vis;
    char  nm[FFS_MAXNAME + 2];
    char  path[FFS_MAXNAME + 24];
    ULONG parent;

    if (!ffs_rd(v, o->key, 1, hdr) || ffs_header_type(v, hdr, o->key) != FFS_ST_USERDIR) return;
    parent = ffs_getl(hdr, v->nl - FFS_E_PARENT);
    ffs_name(hdr + (v->nl - FFS_E_NAME) * 4, FFS_MAXNAME, nm, sizeof(nm));
    snprintf(path, sizeof(path), "%s/%s", GS(MSG_LOSTFOUND_SHOWN), nm);
    NOTE(s, SEV_WARN, GS(MSG_F_ORPH_DIR), nm, (unsigned long)o->key);
    add_fix(s, FIX_INSERT, WHY_ATTACH, o->key, reachable_dir(s, parent) ? parent : 0, 0, 0, 0, nm);
    s->attachable++;
    s->structural++;
    o->state = 1;
    claim(s, o->key, path);
    SETBIT(s->isdir, o->key);
    s->dirs++;
    if (v->dircache) claim_dircache(s, o->key, hdr, path);
    ffs_walk(v, o->key, path, s->seen, scan_visitor(s, &vis));
}

void FfsScan_Attach(struct FfsScan *s)
{
    struct FfsVol *v = s->v;
    UBYTE *hdr;
    ULONG  i, round;
    BOOL   progress;

    if (!s->have_root || s->norph == 0) return;
    hdr = (UBYTE *)AllocVec(v->bsz, MEMF_PUBLIC);
    if (!hdr) return;

    /* 1: lost directories nothing lost sits above */
    for (i = 0; i < s->norph; i++) {
        struct FfsOrphan *o = &s->orph[i], *po;
        if (o->sectype != FFS_ST_USERDIR || o->deleted || o->state || BIT(s->seen, o->key)) continue;
        if (!ffs_rd(v, o->key, 1, hdr)) continue;
        po = FfsScan_FindOrphan(s, ffs_getl(hdr, v->nl - FFS_E_PARENT));
        if (po && po->sectype == FFS_ST_USERDIR && !po->deleted && po != o) continue;
        attach_dir(s, o, hdr);
    }
    /* 2: directories whose parent became reachable but did not list them */
    for (round = 0, progress = TRUE; progress && round < 8; round++) {
        progress = FALSE;
        for (i = 0; i < s->norph; i++) {
            struct FfsOrphan *o = &s->orph[i];
            if (o->sectype != FFS_ST_USERDIR || o->deleted || o->state || BIT(s->seen, o->key)) continue;
            if (!ffs_rd(v, o->key, 1, hdr)) continue;
            if (!reachable_dir(s, ffs_getl(hdr, v->nl - FFS_E_PARENT))) continue;
            attach_dir(s, o, hdr);
            progress = TRUE;
        }
    }
    /* 3: files */
    for (i = 0; i < s->norph; i++) {
        struct FfsOrphan *o = &s->orph[i], *po;
        struct FileCtx c;
        char  nm[FFS_MAXNAME + 2];
        ULONG parent, flags, count = 0, size, expect;

        if (o->sectype != FFS_ST_FILE || o->deleted || o->state || BIT(s->seen, o->key)) continue;
        if (!ffs_rd(v, o->key, 1, hdr) || ffs_header_type(v, hdr, o->key) != FFS_ST_FILE) continue;
        parent = ffs_getl(hdr, v->nl - FFS_E_PARENT);
        po = FfsScan_FindOrphan(s, parent);
        if (po && po->sectype == FFS_ST_USERDIR && po->state == 0) continue;   /* its directory stays lost */
        ffs_name(hdr + (v->nl - FFS_E_NAME) * 4, FFS_MAXNAME, nm, sizeof(nm));

        c.s = s; c.path = nm; c.key = o->key; c.conflicts = 0; c.bad_data = 0; c.mode = 1;
        flags  = ffs_file_blocks(v, o->key, hdr, file_cb, &c, &count);
        size   = ffs_getl(hdr, v->nl - FFS_E_BYTESIZE);
        expect = size ? (size + v->datasize - 1) / v->datasize : 0;
        SETBIT(s->seen, o->key);
        if (flags || c.conflicts || count < expect || FfsScan_Claim(s, o->key)) {
            /* not safe to put back; its blocks stay marked used so that a
             * later rescue still finds them untouched */
            NOTE(s, SEV_WARN, GS(MSG_F_ORPH_CONFLICT), nm, (unsigned long)o->key);
            o->state = 2;
            c.mode = 2;
            FfsScan_AddClaim(s, o->key);
            ffs_file_blocks(v, o->key, hdr, file_cb, &c, NULL);
            continue;
        }
        NOTE(s, SEV_WARN, GS(MSG_F_ORPH_FILE), nm, (unsigned long)o->key);
        c.mode = 2;
        FfsScan_AddClaim(s, o->key);
        ffs_file_blocks(v, o->key, hdr, file_cb, &c, NULL);
        add_fix(s, FIX_INSERT, WHY_ATTACH, o->key, reachable_dir(s, parent) ? parent : 0, 0, 0, 0, nm);
        s->attachable++;
        s->structural++;
        s->files++;
        o->state = 1;
    }
    FreeVec(hdr);
}

/* ------------------------------------------------------------------ */
/* the read-only check                                                  */
/* ------------------------------------------------------------------ */

LONG FfsCheck_Run(struct BlockDev *bd, const struct VolumeInfo *vi,
                  const struct FfsScanOpts *o, struct Report *rep)
{
    struct FfsVol  v;
    struct FfsScan s;
    char err[96];
    LONG rc;
    BOOL tree;

    err[0] = '\0';
    if (!ffs_setup(&v, bd, vi, err, sizeof(err))) {
        Rep_Add(rep, SEV_FATAL, "%s", err);
        ffs_cleanup(&v);
        return RETURN_FAIL;
    }
    if (!FfsScan_Init(&s, &v, rep, o)) {
        Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_OUT_OF_MEMORY));
        ffs_cleanup(&v);
        return RETURN_FAIL;
    }
    tree = FfsScan_Tree(&s);
    if (tree) FfsScan_Bitmap(&s);
    if (!rep->aborted && s.o.orphans && FfsScan_Orphans(&s)) {
        if (tree) FfsScan_Attach(&s);
        Rep_Add(rep, s.lost ? SEV_WARN : SEV_INFO, GS(MSG_F_ORPH_FOUND),
                (unsigned long)s.lost, (unsigned long)s.attachable, (unsigned long)s.deleted);
    }
    if (tree) {
        /* lost space is only a finding when nothing lost explains it */
        if (s.free_but_used && !(s.o.orphans && s.lost))
            Rep_Add(rep, SEV_WARN, GS(MSG_F_BM_FREEUSED), (unsigned long)s.free_but_used);
        else if (s.free_but_used)
            Rep_Add(rep, SEV_INFO, GS(MSG_F_BM_FREEUSED), (unsigned long)s.free_but_used);
        Rep_Add(rep, SEV_INFO, GS(MSG_F_SUMMARY), (unsigned long)s.dirs, (unsigned long)s.files, (unsigned long)s.links);
    }
    if (v.read_errors)
        Rep_Add(rep, SEV_ERROR, GS(MSG_F_READERRORS), (unsigned long)v.read_errors);

    if (rep->aborted)                     rc = RETURN_WARN;
    else if (rep->count[SEV_FATAL])       rc = RETURN_FAIL;
    else if (rep->count[SEV_ERROR])       rc = RETURN_ERROR;
    else if (rep->count[SEV_WARN])        rc = RETURN_WARN;
    else                                  rc = RETURN_OK;
    FfsScan_Free(&s);
    ffs_cleanup(&v);
    return rc;
}
