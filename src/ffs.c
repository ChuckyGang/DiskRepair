/*
 * ffs.c - see ffs.h.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include "clib.h"
#include "be.h"
#include "ffs.h"

/* ------------------------------------------------------------------ */
/* overlay                                                              */
/* ------------------------------------------------------------------ */

struct FfsOvl {
    ULONG   count, cap;
    ULONG  *blk;
    UBYTE **orig;
    UBYTE **now;
    ULONG  *hash;                  /* open addressing, entry index + 1 */
    ULONG   hmask;
};

static ULONG ovl_slot(const struct FfsOvl *o, ULONG blk)
{
    ULONG h = (blk * 2654435761UL) & o->hmask;
    while (o->hash[h] && o->blk[o->hash[h] - 1] != blk) h = (h + 1) & o->hmask;
    return h;
}

static LONG ovl_find(const struct FfsOvl *o, ULONG blk)
{
    if (!o || o->count == 0) return -1;
    return (LONG)o->hash[ovl_slot(o, blk)] - 1;
}

static BOOL ovl_grow(struct FfsOvl *o)
{
    ULONG ncap = o->cap ? o->cap * 2 : 64, nh = ncap * 4, i;
    ULONG  *nb = (ULONG *)AllocVec(ncap * sizeof(ULONG), MEMF_PUBLIC);
    UBYTE **no = (UBYTE **)AllocVec(ncap * sizeof(UBYTE *), MEMF_PUBLIC);
    UBYTE **nn = (UBYTE **)AllocVec(ncap * sizeof(UBYTE *), MEMF_PUBLIC);
    ULONG  *hh = (ULONG *)AllocVec(nh * sizeof(ULONG), MEMF_PUBLIC | MEMF_CLEAR);
    if (!nb || !no || !nn || !hh) {
        if (nb) FreeVec(nb);
        if (no) FreeVec(no);
        if (nn) FreeVec(nn);
        if (hh) FreeVec(hh);
        return FALSE;
    }
    for (i = 0; i < o->count; i++) { nb[i] = o->blk[i]; no[i] = o->orig[i]; nn[i] = o->now[i]; }
    if (o->blk)  FreeVec(o->blk);
    if (o->orig) FreeVec(o->orig);
    if (o->now)  FreeVec(o->now);
    if (o->hash) FreeVec(o->hash);
    o->blk = nb; o->orig = no; o->now = nn; o->hash = hh; o->cap = ncap; o->hmask = nh - 1;
    for (i = 0; i < o->count; i++) o->hash[ovl_slot(o, o->blk[i])] = i + 1;
    return TRUE;
}

BOOL ffs_ovl_begin(struct FfsVol *v)
{
    if (v->ovl) return TRUE;
    v->ovl = (struct FfsOvl *)AllocVec(sizeof(struct FfsOvl), MEMF_PUBLIC | MEMF_CLEAR);
    return v->ovl != NULL;
}

void ffs_ovl_end(struct FfsVol *v)
{
    struct FfsOvl *o = v->ovl;
    ULONG i;
    if (!o) return;
    for (i = 0; i < o->count; i++) FreeVec(o->orig[i]);     /* orig and now share one allocation */
    if (o->blk)  FreeVec(o->blk);
    if (o->orig) FreeVec(o->orig);
    if (o->now)  FreeVec(o->now);
    if (o->hash) FreeVec(o->hash);
    FreeVec(o);
    v->ovl = NULL;
}

BOOL ffs_ovl_put(struct FfsVol *v, ULONG blk, const UBYTE *data)
{
    struct FfsOvl *o = v->ovl;
    LONG i;
    if (!o || blk >= v->nblocks) return FALSE;
    i = ovl_find(o, blk);
    if (i < 0) {
        UBYTE *pair;
        if (o->count == o->cap && !ovl_grow(o)) return FALSE;
        pair = (UBYTE *)AllocVec(v->bsz * 2, MEMF_PUBLIC);
        if (!pair) return FALSE;
        /* the original straight from the device: the block is not in the overlay yet */
        if (!BlockDev_ReadBlocks(v->bd, v->part_start + blk * v->spb, v->spb, pair)) {
            FreeVec(pair);
            return FALSE;
        }
        i = (LONG)o->count++;
        o->blk[i] = blk; o->orig[i] = pair; o->now[i] = pair + v->bsz;
        o->hash[ovl_slot(o, blk)] = (ULONG)i + 1;
    }
    if (data != o->now[i]) memcpy(o->now[i], data, v->bsz);
    return TRUE;
}

ULONG ffs_ovl_count(const struct FfsVol *v) { return v->ovl ? v->ovl->count : 0; }

BOOL ffs_ovl_get(const struct FfsVol *v, ULONG i, ULONG *blk, const UBYTE **orig, const UBYTE **now)
{
    if (!v->ovl || i >= v->ovl->count) return FALSE;
    if (blk)  *blk  = v->ovl->blk[i];
    if (orig) *orig = v->ovl->orig[i];
    if (now)  *now  = v->ovl->now[i];
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* setup and block access                                              */
/* ------------------------------------------------------------------ */

BOOL ffs_setup(struct FfsVol *v, struct BlockDev *bd, const struct VolumeInfo *vi,
               char *err, ULONG errlen)
{
    memset(v, 0, sizeof(*v));
    v->bd         = bd;
    v->part_start = vi->start_block;
    v->spb        = vi->spb ? vi->spb : 1;
    v->bsz        = vi->sector_size * v->spb;
    v->nl         = v->bsz / 4;
    v->nblocks    = vi->num_blocks / v->spb;
    v->reserved   = vi->reserved ? vi->reserved : 2;
    v->tsize      = v->nl - 56;
    v->dostype    = vi->dostype;
    v->ofs        = (vi->dostype & 1) == 0;
    v->intl       = (vi->dostype & 7) >= 2;
    v->dircache   = ((vi->dostype & 7) == 4 || (vi->dostype & 7) == 5);
    v->datasize   = v->ofs ? v->bsz - FFS_OFS_HDR : v->bsz;
    if ((vi->dostype & 0xFFFFFF00UL) != 0x444F5300UL || (vi->dostype & 0xFF) > 7) {
        if (err && errlen) strncpy(err, "not an OFS/FFS DOS type", errlen - 1);
        return FALSE;
    }
    if ((vi->dostype & 7) >= 6) {
        if (err && errlen) strncpy(err, "long-filename FFS (DOS\\6, DOS\\7) is not supported yet", errlen - 1);
        return FALSE;
    }
    if (v->nl < 128 || v->nl > 8192 || (v->bsz & (v->bsz - 1)) || v->nblocks <= v->reserved + 2) {
        if (err && errlen) strncpy(err, "unsupported block size or empty partition", errlen - 1);
        return FALSE;
    }
    v->root_calc = (v->nblocks - 1 + v->reserved) / 2;
    v->extbuf = (UBYTE *)AllocVec(v->bsz, MEMF_PUBLIC);
    if (!v->extbuf) {
        if (err && errlen) strncpy(err, "out of memory", errlen - 1);
        return FALSE;
    }
    return TRUE;
}

void ffs_cleanup(struct FfsVol *v)
{
    ffs_ovl_end(v);
    if (v->extbuf) FreeVec(v->extbuf);
    v->extbuf = NULL;
}

BOOL ffs_rd(struct FfsVol *v, ULONG blk, ULONG n, void *buf)
{
    if (blk >= v->nblocks || n > v->nblocks - blk) return FALSE;
    if (n == 1 && v->ovl) {
        LONG i = ovl_find(v->ovl, blk);
        if (i >= 0) { memcpy(buf, v->ovl->now[i], v->bsz); return TRUE; }
    }
    if (!BlockDev_ReadBlocks(v->bd, v->part_start + blk * v->spb, n * v->spb, buf)) {
        if (n == 1) v->read_errors++;
        return FALSE;
    }
    if (n > 1 && v->ovl && v->ovl->count) {
        ULONG k;
        for (k = 0; k < n; k++) {
            LONG i = ovl_find(v->ovl, blk + k);
            if (i >= 0) memcpy((UBYTE *)buf + k * v->bsz, v->ovl->now[i], v->bsz);
        }
    }
    return TRUE;
}

ULONG ffs_getl(const UBYTE *b, ULONG idx) { return be32(b, idx * 4); }

void ffs_putl(UBYTE *b, ULONG idx, ULONG val)
{
    b[idx * 4]     = (UBYTE)(val >> 24); b[idx * 4 + 1] = (UBYTE)(val >> 16);
    b[idx * 4 + 2] = (UBYTE)(val >> 8);  b[idx * 4 + 3] = (UBYTE)val;
}

BOOL ffs_sum_ok(const UBYTE *b, ULONG nl)
{
    ULONG sum = 0, i;
    for (i = 0; i < nl; i++) sum += ffs_getl(b, i);
    return sum == 0;
}

void ffs_sum_set(UBYTE *b, ULONG nl, ULONG at)
{
    ULONG sum = 0, i;
    ffs_putl(b, at, 0);
    for (i = 0; i < nl; i++) sum += ffs_getl(b, i);
    ffs_putl(b, at, (ULONG)(0UL - sum));
}

static UBYTE upc(const struct FfsVol *v, UBYTE c)
{
    if (c >= 'a' && c <= 'z') return (UBYTE)(c - 32);
    if (v->intl && c >= 0xE0 && c <= 0xFE && c != 0xF7) return (UBYTE)(c - 32);
    return c;
}

ULONG ffs_hash(const struct FfsVol *v, const UBYTE *bstr)
{
    ULONG len = bstr[0], h, i;
    if (len > FFS_MAXNAME) len = FFS_MAXNAME;
    h = len;
    for (i = 0; i < len; i++) h = (h * 13 + upc(v, bstr[1 + i])) & 0x7FF;
    return h % v->tsize;
}

BOOL ffs_name_eq(const struct FfsVol *v, const UBYTE *a, const UBYTE *b)
{
    ULONG i, n = a[0];
    if (n != b[0]) return FALSE;
    for (i = 1; i <= n; i++)
        if (upc(v, a[i]) != upc(v, b[i])) return FALSE;
    return TRUE;
}

void ffs_name(const UBYTE *bstr, ULONG maxlen, char *out, ULONG outlen)
{
    ULONG n = bstr[0], i;
    if (n > maxlen) n = maxlen;
    if (n > outlen - 1) n = outlen - 1;
    for (i = 0; i < n; i++) {
        UBYTE c = bstr[1 + i];
        if (c < 32 || c == '/' || c == ':' || c == 127) c = '_';
        out[i] = (char)c;
    }
    out[n] = '\0';
    if (n == 0) strncpy(out, "unnamed", outlen - 1), out[outlen - 1] = '\0';
}

LONG ffs_header_type(const struct FfsVol *v, const UBYTE *b, ULONG blk)
{
    LONG st;
    if (ffs_getl(b, FFS_L_TYPE) != FFS_T_SHORT) return 0;
    if (ffs_getl(b, FFS_L_OWNKEY) != blk) return 0;
    st = (LONG)ffs_getl(b, v->nl - FFS_E_SECTYPE);
    if (st != FFS_ST_USERDIR && st != FFS_ST_FILE && st != FFS_ST_SOFTLINK &&
        st != FFS_ST_LINKDIR && st != FFS_ST_LINKFILE) return 0;
    if (!ffs_sum_ok(b, v->nl)) return 0;
    return st;
}

static BOOL is_root(const struct FfsVol *v, const UBYTE *b)
{
    return ffs_getl(b, FFS_L_TYPE) == FFS_T_SHORT &&
           (LONG)ffs_getl(b, v->nl - FFS_E_SECTYPE) == FFS_ST_ROOT &&
           ffs_sum_ok(b, v->nl);
}

BOOL ffs_find_root(struct FfsVol *v, UBYTE *buf)
{
    ULONG alt = 0;
    v->root = 0;
    if (ffs_rd(v, v->root_calc, 1, buf) && is_root(v, buf)) { v->root = v->root_calc; return TRUE; }
    if (ffs_rd(v, 0, 1, buf)) alt = ffs_getl(buf, 2);
    if (alt >= v->reserved && alt < v->nblocks && alt != v->root_calc &&
        ffs_rd(v, alt, 1, buf) && is_root(v, buf)) { v->root = alt; return TRUE; }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* bitmap                                                               */
/* ------------------------------------------------------------------ */

void ffs_bm_free(struct FfsBm *bm)
{
    if (bm->bits)     FreeVec(bm->bits);
    if (bm->page_key) FreeVec(bm->page_key);
    if (bm->page_bad) FreeVec(bm->page_bad);
    if (bm->ext_key)  FreeVec(bm->ext_key);
    memset(bm, 0, sizeof(*bm));
}

BOOL ffs_bm_load(struct FfsVol *v, const UBYTE *rootbuf, struct FfsBm *bm)
{
    ULONG nl = v->nl, page, ext, ext_slot = 0, li = 0, max_ext;
    UBYTE *pg, *extb;
    BOOL  have_ext = FALSE;

    memset(bm, 0, sizeof(*bm));
    bm->span  = v->nblocks - v->reserved;
    bm->bpb   = (nl - 1) * 32;
    bm->pages = (bm->span + bm->bpb - 1) / bm->bpb;
    bm->longs = (bm->span + 31) / 32;
    max_ext   = bm->pages / (nl - 1) + 2;
    bm->bits     = (ULONG *)AllocVec(bm->longs * 4 + 4, MEMF_PUBLIC | MEMF_CLEAR);
    bm->page_key = (ULONG *)AllocVec(bm->pages * 4 + 4, MEMF_PUBLIC | MEMF_CLEAR);
    bm->page_bad = (UBYTE *)AllocVec(bm->pages + 1, MEMF_PUBLIC | MEMF_CLEAR);
    bm->ext_key  = (ULONG *)AllocVec(max_ext * 4, MEMF_PUBLIC | MEMF_CLEAR);
    pg = (UBYTE *)AllocVec(v->bsz * 2, MEMF_PUBLIC);
    if (!bm->bits || !bm->page_key || !bm->page_bad || !bm->ext_key || !pg) {
        if (pg) FreeVec(pg);
        ffs_bm_free(bm);
        return FALSE;
    }
    extb = pg + v->bsz;
    bm->flag_valid = (ffs_getl(rootbuf, nl - FFS_E_BMFLAG) == 0xFFFFFFFFUL);
    ext = ffs_getl(rootbuf, nl - FFS_E_BMEXT);

    for (page = 0; page < bm->pages; page++) {
        ULONG key = 0, j;
        if (page < 25) {
            key = ffs_getl(rootbuf, nl - FFS_E_BMPAGES + page);
        } else if (!bm->ext_broken) {
            if (page == 25 || ext_slot == nl - 1) {
                if (page != 25) ext = ffs_getl(extb, nl - 1);
                if (ext < v->reserved || ext >= v->nblocks || bm->ext_count >= max_ext ||
                    !ffs_rd(v, ext, 1, extb)) {
                    bm->ext_broken = TRUE;
                    have_ext = FALSE;
                } else {
                    bm->ext_key[bm->ext_count++] = ext;
                    ext_slot = 0;
                    have_ext = TRUE;
                }
            }
            if (have_ext) key = ffs_getl(extb, ext_slot++);
        }
        if (key < v->reserved || key >= v->nblocks) {
            bm->page_bad[page] = 1; bm->bad_pages++;
            li += nl - 1;
            continue;
        }
        bm->page_key[page] = key;
        if (!ffs_rd(v, key, 1, pg) || !ffs_sum_ok(pg, nl)) {
            bm->page_bad[page] = 1; bm->bad_pages++;
            li += nl - 1;
            continue;
        }
        for (j = 1; j < nl; j++, li++)
            if (li < bm->longs) bm->bits[li] = ffs_getl(pg, j);
    }
    FreeVec(pg);
    if (bm->span & 31) bm->bits[bm->longs - 1] &= (1UL << (bm->span & 31)) - 1;
    return TRUE;
}

BOOL ffs_bm_isfree(const struct FfsVol *v, const struct FfsBm *bm, ULONG blk, BOOL *known)
{
    ULONG i;
    if (known) *known = TRUE;
    if (blk < v->reserved || blk >= v->nblocks) return FALSE;
    i = blk - v->reserved;
    if (bm->page_bad[i / bm->bpb]) { if (known) *known = FALSE; return FALSE; }
    return (bm->bits[i >> 5] >> (i & 31)) & 1;
}

/* ------------------------------------------------------------------ */
/* file layout                                                          */
/* ------------------------------------------------------------------ */

ULONG ffs_file_blocks(struct FfsVol *v, ULONG key, const UBYTE *hdr,
                      FfsBlkCb cb, void *ud, ULONG *count)
{
    const UBYTE *b = hdr;
    ULONG nl = v->nl, seq = 0, next = 0, flags = 0;

    for (;;) {
        ULONG high = ffs_getl(b, FFS_L_HIGHSEQ), k, e;
        if (high > v->tsize) { flags |= FFS_FB_BADPTR; break; }
        for (k = 0; k < high; k++) {
            ULONG p = ffs_getl(b, nl - FFS_E_FIRSTPTR - k);
            if (p < v->reserved || p >= v->nblocks) { flags |= FFS_FB_BADPTR; break; }
            if (cb && !cb(ud, 0, seq, p)) { flags |= FFS_FB_ABORT; break; }
            seq++;
        }
        if (flags) break;
        e = ffs_getl(b, nl - FFS_E_EXTENSION);
        if (e == 0) break;
        if (e < v->reserved || e >= v->nblocks || ++next > v->nblocks ||
            !ffs_rd(v, e, 1, v->extbuf) ||
            ffs_getl(v->extbuf, FFS_L_TYPE) != FFS_T_LIST ||
            ffs_getl(v->extbuf, FFS_L_OWNKEY) != e ||
            ffs_getl(v->extbuf, nl - FFS_E_PARENT) != key ||
            (LONG)ffs_getl(v->extbuf, nl - FFS_E_SECTYPE) != FFS_ST_FILE ||
            !ffs_sum_ok(v->extbuf, nl)) { flags |= FFS_FB_BADEXT; break; }
        if (cb && !cb(ud, 1, next - 1, e)) { flags |= FFS_FB_ABORT; break; }
        b = v->extbuf;
    }
    if (count) *count = seq;
    return flags;
}

/* ------------------------------------------------------------------ */
/* tree walk                                                            */
/* ------------------------------------------------------------------ */

struct Frame {
    ULONG dir;
    ULONG slot;        /* next slot to open */
    ULONG cslot;       /* slot of the chain being followed */
    ULONG next;        /* next key in that chain, 0 = open the next slot */
    ULONG prev;        /* header before `next` in the chain, 0 = the slot */
    UWORD pathlen;     /* length of this directory's path */
};

#define SEEN(k) (seen[(k) >> 3] & (1 << ((k) & 7)))
#define MARK(k) (seen[(k) >> 3] |= (UBYTE)(1 << ((k) & 7)))

BOOL ffs_walk(struct FfsVol *v, ULONG start, const char *start_path,
              UBYTE *seen, const struct FfsVisitor *vis)
{
    struct Frame *st;
    UBYTE *dirbuf, *hdrbuf;
    char  *path;
    ULONG  nl = v->nl, loaded = 0xFFFFFFFFUL;
    LONG   depth = 0;
    BOOL   ok = TRUE;

    st     = (struct Frame *)AllocVec(sizeof(struct Frame) * FFS_MAXDEPTH, MEMF_PUBLIC | MEMF_CLEAR);
    dirbuf = (UBYTE *)AllocVec(v->bsz * 2, MEMF_PUBLIC);
    path   = (char *)AllocVec(FFS_PATHMAX, MEMF_PUBLIC);
    if (!st || !dirbuf || !path) {
        if (st) FreeVec(st);
        if (dirbuf) FreeVec(dirbuf);
        if (path) FreeVec(path);
        return FALSE;
    }
    hdrbuf = dirbuf + v->bsz;
    strncpy(path, start_path ? start_path : "", FFS_PATHMAX - 1);
    path[FFS_PATHMAX - 1] = '\0';
    st[0].dir = start;
    st[0].pathlen = (UWORD)strlen(path);
    MARK(start);

    while (depth >= 0) {
        struct Frame *f = &st[depth];
        struct FfsEntryRef ref;
        ULONG key;
        LONG  sec = 0;
        int   why = 0;
        char  nm[FFS_MAXNAME + 2];
        ULONG plen;

        if (loaded != f->dir) {
            if (!ffs_rd(v, f->dir, 1, dirbuf)) { depth--; loaded = 0xFFFFFFFFUL; continue; }
            loaded = f->dir;
        }
        path[f->pathlen] = '\0';
        if (f->next == 0) {
            while (f->slot < v->tsize && ffs_getl(dirbuf, FFS_L_TABLE + f->slot) == 0) f->slot++;
            if (f->slot >= v->tsize) {
                if (depth > 0 && vis->dir_leave) vis->dir_leave(vis->ud, f->dir, dirbuf, path);
                depth--;
                continue;
            }
            f->cslot = f->slot;
            f->next  = ffs_getl(dirbuf, FFS_L_TABLE + f->slot);
            f->prev  = 0;
            f->slot++;
        }
        key = f->next;
        ref.dir = f->dir; ref.slot = f->cslot; ref.prev = f->prev; ref.key = key; ref.next = 0;

        if (vis->tick && !vis->tick(vis->ud)) { ok = FALSE; break; }
        if (key < v->reserved || key >= v->nblocks) why = FFS_BAD_RANGE;
        else if (SEEN(key))                         why = FFS_BAD_LOOP;
        else if (!ffs_rd(v, key, 1, hdrbuf))        why = FFS_BAD_READ;
        else if ((sec = ffs_header_type(v, hdrbuf, key)) == 0) why = FFS_BAD_HEADER;
        if (why) {
            if (vis->entry) vis->entry(vis->ud, &ref, why, path);
            f->next = 0;                       /* the rest of this chain is out of reach */
            continue;
        }
        MARK(key);
        ref.next = ffs_getl(hdrbuf, nl - FFS_E_HASHCHAIN);
        f->prev  = key;
        f->next  = ref.next;

        /* this entry's own path */
        ffs_name(hdrbuf + (nl - FFS_E_NAME) * 4, FFS_MAXNAME, nm, sizeof(nm));
        plen = f->pathlen;
        if (plen + strlen(nm) + 2 < FFS_PATHMAX) {
            if (plen) path[plen++] = '/';
            strcpy(path + plen, nm);
        }
        if (vis->entry) {
            if (ffs_getl(hdrbuf, nl - FFS_E_PARENT) != f->dir) vis->entry(vis->ud, &ref, FFS_ODD_PARENT, path);
            if (ffs_hash(v, hdrbuf + (nl - FFS_E_NAME) * 4) != f->cslot) vis->entry(vis->ud, &ref, FFS_ODD_HASH, path);
        }

        if (sec == FFS_ST_USERDIR) {
            if (depth + 1 >= FFS_MAXDEPTH) {
                if (vis->entry) vis->entry(vis->ud, &ref, FFS_BAD_DEPTH, path);
            } else if (!vis->dir_enter || vis->dir_enter(vis->ud, &ref, hdrbuf, path)) {
                struct Frame *c = &st[++depth];
                memset(c, 0, sizeof(*c));
                c->dir = key;
                c->pathlen = (UWORD)strlen(path);
                memcpy(dirbuf, hdrbuf, v->bsz);
                loaded = key;
            }
        } else if (sec == FFS_ST_FILE) {
            if (vis->file) vis->file(vis->ud, &ref, hdrbuf, path);
        } else {
            if (vis->link) vis->link(vis->ud, &ref, hdrbuf, sec, path);
        }
    }
    FreeVec(st);
    FreeVec(dirbuf);
    FreeVec(path);
    return ok;
}
