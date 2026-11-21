/*
 * pfs_rescue.c - copy what is readable on a PFS3 volume to another place.
 * The volume is only read.
 *
 * A directory is the chain of 'DB' blocks its anode chain names.  Every
 * 'DB' block also carries the anode number of the directory it belongs
 * to and that directory's parent, so a directory whose chain is broken
 * can still be put together from the blocks themselves: the reserved
 * area is read once and every 'DB' block in use is noted.  Blocks the
 * walk from the root never reached are lost directories; their names are
 * gone with the entry that pointed to them, so they are copied as
 * Lost+Found/Dir_<anode number>.
 *
 * Old copies of directory blocks stay in the reserved area (PFS3 writes
 * a changed block to a new place), so only blocks the reserved bitmap
 * marks as in use are taken.
 *
 * Deleted files: PFS3 keeps the last deleted files in its deldir with
 * their anode chains and data blocks untouched.  They have no directory
 * any more, only a name.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include "clib.h"
#include "be.h"
#include "pfs_core.h"
#include "resc_common.h"
#include "platform.h"
#include "locale_support.h"

#define MAXDEPTH 48

struct DbRef { ULONG blk, anodenr, parent; };

struct PRes {
    struct PfsVol      *v;
    struct Report      *rep;
    struct RescueStats *st;
    struct RcOut        out;
    struct DbRef *db;            /* every 'DB' block in use */
    ULONG  ndb, capdb;
    UBYTE *seen;                 /* per reserved block: copied already */
    char   path[RC_PATHMAX];
    ULONG  depth;
};

#define RESIDX(v, blk)  (((blk) - (v)->firstres) / (v)->rescluster)
#define BIT(a, k)       ((a)[(k) >> 3] & (1 << ((k) & 7)))
#define SETBIT(a, k)    ((a)[(k) >> 3] |= (UBYTE)(1 << ((k) & 7)))

static BOOL res_in_use(const struct PfsVol *v, ULONG idx)
{
    const UBYTE *bm = v->root + 512;
    ULONG avail = (v->rblkcluster * 512UL - 512 - 12) * 8;
    if (be16(bm, 0) != PFS_ID_BM || idx >= avail) return TRUE;       /* no bitmap: take everything */
    return (be32(bm, 12 + 4 * (idx / 32)) & (0x80000000UL >> (idx % 32))) == 0;
}

static BOOL scan_reserved(struct PRes *r)
{
    struct PfsVol *v = r->v;
    ULONG numres = (v->lastres - v->firstres + 1) / v->rescluster, i;
    for (i = 0; i < numres; i++) {
        ULONG blk = v->firstres + i * v->rescluster;
        if ((i & 63) == 0 && !Rep_Progress(r->rep, i, numres, GS(MSG_PROG_ORPHANS))) return FALSE;
        if (!res_in_use(v, i)) continue;
        if (!pfs_res_read(v, blk, v->tbuf) || be16(v->tbuf, 0) != PFS_ID_DB) continue;
        if (r->ndb == r->capdb) {
            ULONG ncap = r->capdb ? r->capdb * 2 : 256;
            struct DbRef *n = (struct DbRef *)AllocVec(ncap * sizeof(struct DbRef), MEMF_PUBLIC);
            if (!n) return FALSE;
            if (r->db) { memcpy(n, r->db, r->ndb * sizeof(struct DbRef)); FreeVec(r->db); }
            r->db = n; r->capdb = ncap;
        }
        r->db[r->ndb].blk = blk;
        r->db[r->ndb].anodenr = be32(v->tbuf, 12);
        r->db[r->ndb].parent  = be32(v->tbuf, 16);
        r->ndb++;
    }
    return TRUE;
}

static void copy_file(struct PRes *r, ULONG anodenr, ULONG size, const char *path,
                      ULONG prot, ULONG days, ULONG mins, ULONG ticks, const char *comment)
{
    struct PfsVol *v = r->v;
    struct RcOut *o = &r->out;
    ULONG a = anodenr, guard = 0;

    if (!Rc_Open(o, path, anodenr, size)) return;
    while (a && o->remaining && !o->write_failed && !r->rep->aborted) {
        ULONG cs, blk, next;
        if (++guard > 4000000UL || !pfs_get_anode(v, a, &cs, &blk, &next)) { o->broken = TRUE; break; }
        if (cs) {
            if (blk <= v->lastres || blk >= v->disksize || cs > v->disksize - blk) { o->broken = TRUE; break; }
            Rc_Run(o, v->part_start + blk, cs);
        }
        a = next;
    }
    Rc_Close(o, prot, days, mins, ticks, comment);
}

static void do_dir(struct PRes *r, ULONG dirnr);

/* One directory block: copy its files, descend into its directories. */
static BOOL do_block(struct PRes *r, ULONG dirnr, const UBYTE *db)
{
    struct PfsVol *v = r->v;
    ULONG off, plen = strlen(r->path);
    BOOL  whole = TRUE;

    for (off = 20; off + 20 <= v->resblksize && !r->rep->aborted; ) {
        ULONG len = db[off], nl, cl, an, size, len2;
        LONG  type;
        char  name[112], comment[82];
        if (len == 0) break;
        nl = db[off + 17];
        if (len < 20 || off + len > v->resblksize || nl == 0 || 18 + nl + 1 > len || nl > 107) { whole = FALSE; break; }
        type = (LONG)(BYTE)db[off + 1];
        an   = be32(db, off + 2);
        size = be32(db, off + 6);
        memcpy(name, db + off + 18, nl);
        name[nl] = '\0';
        cl = db[off + 18 + nl];
        if (cl > 79 || 18 + nl + 1 + cl > len) cl = 0;
        memcpy(comment, db + off + 19 + nl, cl);
        comment[cl] = '\0';
        {
            ULONG k;
            for (k = 0; k < nl; k++) if (name[k] == '/' || name[k] == ':' || (UBYTE)name[k] < 32) name[k] = '_';
            for (k = 0; k < cl; k++) if ((UBYTE)comment[k] < 32) comment[k] = ' ';
        }
        len2 = plen;
        r->path[plen] = '\0';
        if (Rc_Join(r->path, &len2, name)) {
            ULONG prot = db[off + 16], days = be16(db, off + 10), mins = be16(db, off + 12), ticks = be16(db, off + 14);
            if (type == PFS_ST_USERDIR) {
                if (r->depth + 1 >= MAXDEPTH || an == dirnr) {
                    Rep_Add(r->rep, SEV_WARN, GS(MSG_S_DIR_DAMAGED), r->path);
                } else if (!Out_MkDir(r->path)) {
                    Rep_Add(r->rep, SEV_ERROR, GS(MSG_S_CANNOT_MKDIR), r->path);
                    r->st->failed++;
                } else {
                    r->st->dirs++;
                    r->depth++;
                    do_dir(r, an);
                    r->depth--;
                    r->path[len2] = '\0';
                    Out_SetMeta(r->path, prot, days, mins, ticks, comment);
                }
            } else if (type == PFS_ST_FILE || type == PFS_ST_ROLLOVER) {
                copy_file(r, an, size, r->path, prot, days, mins, ticks, comment);
            } else {
                Rep_Add(r->rep, SEV_WARN, GS(MSG_S_LINK_SKIPPED), r->path);
            }
        }
        r->path[plen] = '\0';
        off += len;
    }
    r->path[plen] = '\0';
    return whole;
}

/* The directory with this anode number, into r->path. */
static void do_dir(struct PRes *r, ULONG dirnr)
{
    struct PfsVol *v = r->v;
    UBYTE *db = (UBYTE *)AllocVec(v->resblksize, MEMF_PUBLIC);
    ULONG a = dirnr, guard = 0, i;
    BOOL  damaged = FALSE;

    if (!db) { Rep_Add(r->rep, SEV_FATAL, "%s", GS(MSG_OUT_OF_MEMORY)); r->rep->aborted = TRUE; return; }
    while (a && !r->rep->aborted) {
        ULONG cs, blk, next;
        if (++guard > 200000UL || !pfs_get_anode(v, a, &cs, &blk, &next)) { damaged = TRUE; break; }
        if (cs && blk) {
            if (!pfs_res_read(v, blk, db) || be16(db, 0) != PFS_ID_DB || be32(db, 12) != dirnr) { damaged = TRUE; break; }
            if (BIT(r->seen, RESIDX(v, blk))) { damaged = TRUE; break; }          /* a loop */
            SETBIT(r->seen, RESIDX(v, blk));
            if (!do_block(r, dirnr, db)) damaged = TRUE;
        }
        a = next;
    }
    /* whatever else says it belongs to this directory */
    for (i = 0; i < r->ndb && !r->rep->aborted; i++) {
        if (r->db[i].anodenr != dirnr || BIT(r->seen, RESIDX(v, r->db[i].blk))) continue;
        SETBIT(r->seen, RESIDX(v, r->db[i].blk));
        damaged = TRUE;
        if (pfs_res_read(v, r->db[i].blk, db) && be16(db, 0) == PFS_ID_DB) do_block(r, dirnr, db);
    }
    if (damaged && !r->rep->aborted)
        Rep_Add(r->rep, SEV_WARN, GS(MSG_S_DIR_DAMAGED), r->path[0] ? r->path : GS(MSG_ROOT_DIR));
    FreeVec(db);
}

static BOOL lost_left(const struct PRes *r, ULONG anodenr)
{
    ULONG i;
    for (i = 0; i < r->ndb; i++)
        if (r->db[i].anodenr == anodenr && !BIT(r->seen, RESIDX(r->v, r->db[i].blk))) return TRUE;
    return FALSE;
}

static void lost_dirs(struct PRes *r)
{
    ULONG i, pass;
    BOOL  made = FALSE;
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < r->ndb && !r->rep->aborted; i++) {
            ULONG an = r->db[i].anodenr, len;
            char  nm[32];
            if (BIT(r->seen, RESIDX(r->v, r->db[i].blk))) continue;
            /* first the ones no other lost directory holds */
            if (pass == 0 && r->db[i].parent != an && lost_left(r, r->db[i].parent)) continue;
            if (!made) {
                if (!Out_MkDir(GS(MSG_LOSTFOUND_NAME))) {
                    Rep_Add(r->rep, SEV_ERROR, GS(MSG_S_CANNOT_MKDIR), GS(MSG_LOSTFOUND_NAME));
                    return;
                }
                made = TRUE;
            }
            strcpy(r->path, GS(MSG_LOSTFOUND_NAME));
            len = strlen(r->path);
            snprintf(nm, sizeof(nm), GS(MSG_S_LOSTDIR_NAME), (unsigned long)an);
            if (!Rc_Join(r->path, &len, nm) || !Out_MkDir(r->path)) continue;
            r->st->dirs++;
            r->st->lost++;
            r->depth = 1;
            do_dir(r, an);
        }
    }
}

static void deleted_files(struct PRes *r, BOOL flat)
{
    struct PfsVol *v = r->v;
    ULONG blks[33], n = 0, i, e;
    BOOL  made = flat;
    UBYTE *dd;

    if (v->rext) {
        ULONG ndd = be16(v->rext, 54);
        if (ndd > 32) ndd = 32;
        for (i = 0; i < ndd; i++) if (be32(v->rext, 144 + 4 * i)) blks[n++] = be32(v->rext, 144 + 4 * i);
    }
    if (n == 0 && v->deldir_old) blks[n++] = v->deldir_old;
    if (n == 0) return;
    dd = (UBYTE *)AllocVec(v->resblksize, MEMF_PUBLIC);
    if (!dd) return;
    for (i = 0; i < n && !r->rep->aborted; i++) {
        if (!pfs_res_read(v, blks[i], dd) || be16(dd, 0) != PFS_ID_DD) continue;
        for (e = 0; e < 31 && !r->rep->aborted; e++) {
            ULONG o = 32 + 32 * e, an = be32(dd, o), nl = dd[o + 14], k, len;
            char  name[20];
            if (!an || o + 32 > v->resblksize) continue;
            if (nl > 16) nl = 16;
            if (nl == 0) continue;
            memcpy(name, dd + o + 15, nl > 15 ? 15 : nl);
            name[nl > 15 ? 15 : nl] = '\0';
            for (k = 0; name[k]; k++) if (name[k] == '/' || name[k] == ':' || (UBYTE)name[k] < 32) name[k] = '_';
            if (!made) {
                if (!Out_MkDir(GS(MSG_DELETED_NAME))) {
                    Rep_Add(r->rep, SEV_ERROR, GS(MSG_S_CANNOT_MKDIR), GS(MSG_DELETED_NAME));
                    FreeVec(dd);
                    return;
                }
                made = TRUE;
            }
            r->path[0] = '\0';
            len = 0;
            if (!flat) { strcpy(r->path, GS(MSG_DELETED_NAME)); len = strlen(r->path); }
            if (!Rc_Join(r->path, &len, name)) continue;
            r->st->deleted++;
            copy_file(r, an, be32(dd, o + 4), r->path, 0, be16(dd, o + 8), be16(dd, o + 10), be16(dd, o + 12), "");
        }
    }
    FreeVec(dd);
}

static BOOL p_progress(void *ud, ULONG done, ULONG total, const char *what)
{
    return Rep_Progress((struct Report *)ud, done, total, what);
}

LONG PfsRescue_Run(struct BlockDev *bd, const struct VolumeInfo *vi,
                   const struct RescueOpts *opts, struct Report *rep,
                   struct RescueStats *st)
{
    struct PfsVol v;
    struct PRes *r;
    LONG  rc = RETURN_FAIL;
    ULONG numres, before;

    memset(st, 0, sizeof(*st));
    memset(&v, 0, sizeof(v));
    v.cb = p_progress; v.cb_ud = rep;
    if (!pfs_open(&v, bd, vi)) {
        Rep_Add(rep, SEV_FATAL, GS(MSG_M_UNREADABLE), v.err);
        return RETURN_FAIL;
    }
    r = (struct PRes *)AllocVec(sizeof(struct PRes), MEMF_PUBLIC | MEMF_CLEAR);
    numres = (v.lastres - v.firstres + 1) / v.rescluster;
    if (r) r->seen = (UBYTE *)AllocVec(numres / 8 + 2, MEMF_PUBLIC | MEMF_CLEAR);
    if (!r || !r->seen || !Rc_Init(&r->out, bd, rep, st)) {
        Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_OUT_OF_MEMORY));
        goto out;
    }
    r->v = &v; r->rep = rep; r->st = st;

    if (!opts->only_deleted) {
        if (!scan_reserved(r)) {
            if (!rep->aborted) Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_OUT_OF_MEMORY));
            goto out;
        }
        before = st->files + st->dirs;
        r->path[0] = '\0';
        r->depth = 0;
        do_dir(r, PFS_ANODE_ROOTDIR);
        if (!rep->aborted && st->files + st->dirs == before && r->ndb)
            Rep_Add(rep, SEV_WARN, "%s", GS(MSG_S_NO_ROOTDIR));
        if (!rep->aborted) lost_dirs(r);
    }
    if (!rep->aborted && (opts->deleted || opts->only_deleted)) deleted_files(r, opts->only_deleted);
    if (opts->only_deleted && !rep->aborted && st->deleted == 0)
        Rep_Add(rep, SEV_INFO, "%s", GS(MSG_S_UNDEL_NONE));
    if (r->out.read_errors) Rep_Add(rep, SEV_WARN, GS(MSG_F_READERRORS), (unsigned long)r->out.read_errors);

    if (rep->aborted)                              rc = RETURN_WARN;
    else if (st->failed)                           rc = RETURN_ERROR;
    else if (st->partial || rep->count[SEV_WARN])  rc = RETURN_WARN;
    else                                           rc = RETURN_OK;
out:
    if (r) {
        Rc_Free(&r->out);
        if (r->seen) FreeVec(r->seen);
        if (r->db) FreeVec(r->db);
        FreeVec(r);
    }
    pfs_close(&v);
    return rc;
}
