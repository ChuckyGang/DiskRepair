/*
 * sfs_rescue.c - copy what is readable on a Smart File System volume
 * (SFS\0) to another place.  The volume is only read.
 *
 * Layouts from the SFS 1.279 sources (objects.h, blockstructure.h,
 * btreenodes.h):
 *
 *   object container 'OBJC'   header 12 bytes, parent node @12 (the
 *                             directory the objects are in), next @16,
 *                             previous @20, objects from @24
 *   object                    uid @0, gid @2, objectnode @4, protection
 *                             @8, {data, size} or {hashtable,
 *                             firstdirblock} @12/@16, datemodified @20
 *                             (seconds since 1.1.1978), bits @24, name\0
 *                             comment\0 from @25, next object at the next
 *                             even offset; an empty name ends the block
 *   file data                 object.data is the first block of the first
 *                             extent; the extent B-tree maps that key to
 *                             {next, prev, blocks}, next is the key of
 *                             the following extent
 *
 * A directory is the chain of containers from its firstdirblock.  Each
 * container names the node of its directory, so when a chain is broken
 * the rest is found among the containers the admin spaces hold; ones no
 * directory led to are lost directories (Lost+Found/Dir_<node>).
 *
 * Deleted files: SFS moves them to the hidden directory ".recycled"
 * (node 2) of the root, where they stay complete until space is needed.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include "clib.h"
#include "be.h"
#include "fsmap_int.h"
#include "resc_common.h"
#include "platform.h"
#include "locale_support.h"

#define SFS_ID_ADMC 0x41444D43UL
#define SFS_ID_BNDC 0x424E4443UL
#define SFS_ID_OBJC 0x4F424A43UL

#define OT_HARDLINK 32
#define OT_LINK     64
#define OT_DIR      128
#define NODE_ROOT     1
#define NODE_RECYCLED 2
#define MAXDEPTH 48

struct ObRef { ULONG blk, parent; };

struct SRes {
    struct BlockDev    *bd;
    struct Report      *rep;
    struct RescueStats *st;
    const struct RescueOpts *opts;
    struct RcOut        out;
    ULONG  start, spb, bsz, total, extroot;
    UBYTE *tbuf;                 /* B-tree block */
    UBYTE *tbuf2;                /* a look into the recycled directory */
    ULONG  tbuf_blk;
    UBYTE *seen;                 /* per block: container copied already */
    struct ObRef *ob;
    ULONG  nob, capob;
    ULONG *refd;                 /* nodes of directories named inside lost containers */
    ULONG  nrefd, caprefd;
    char   path[RC_PATHMAX];
    ULONG  depth;
    ULONG  read_errors;
};

#define BIT(a, k)       ((a)[(k) >> 3] & (1 << ((k) & 7)))
#define SETBIT(a, k)    ((a)[(k) >> 3] |= (UBYTE)(1 << ((k) & 7)))

static BOOL rd(struct SRes *r, ULONG blk, UBYTE *buf)
{
    if (blk >= r->total) return FALSE;
    if (!BlockDev_ReadBlocks(r->bd, r->start + blk * r->spb, r->spb, buf)) { r->read_errors++; return FALSE; }
    return TRUE;
}

static BOOL sum_ok(const struct SRes *r, const UBYTE *b)
{
    ULONG sum = 0, i;
    for (i = 0; i < r->bsz / 4; i++) sum += bel(b, i);
    return sum == 0xFFFFFFFFUL || sum == 0;
}

static BOOL is_container(struct SRes *r, ULONG blk, UBYTE *buf)
{
    return rd(r, blk, buf) && bel(buf, 0) == SFS_ID_OBJC && bel(buf, 2) == blk && sum_ok(r, buf);
}

/* The extent whose key is this block: next key and length. */
static BOOL find_extent(struct SRes *r, ULONG key, ULONG *next, ULONG *blocks)
{
    ULONG blk = r->extroot, depth = 0;
    while (blk && blk < r->total && depth++ < 16) {
        ULONG n, nodesize, i;
        UBYTE *b = r->tbuf;
        if (r->tbuf_blk != blk) {
            r->tbuf_blk = 0xFFFFFFFFUL;
            if (!rd(r, blk, b) || bel(b, 0) != SFS_ID_BNDC) return FALSE;
            r->tbuf_blk = blk;
        }
        n = be16(b, 12);
        nodesize = b[15];
        if (nodesize < 8 || n == 0 || 16 + n * nodesize > r->bsz) return FALSE;
        if (b[14]) {                                         /* leaf */
            for (i = 0; i < n; i++) {
                ULONG off = 16 + i * nodesize;
                if (be32(b, off) == key) {
                    if (nodesize < 14) return FALSE;
                    *next = be32(b, off + 4);
                    *blocks = be16(b, off + 12);
                    return TRUE;
                }
            }
            return FALSE;
        }
        /* inner: the last child whose key is not above ours */
        {
            ULONG child = be32(b, 16 + 4);
            for (i = 1; i < n; i++) {
                ULONG off = 16 + i * nodesize;
                if (be32(b, off) > key) break;
                child = be32(b, off + 4);
            }
            blk = child;
        }
    }
    return FALSE;
}

static void copy_file(struct SRes *r, ULONG node, ULONG data, ULONG size, const char *path,
                      ULONG prot, ULONG secs, const char *comment)
{
    struct RcOut *o = &r->out;
    ULONG key = data, guard = 0;

    if (!Rc_Open(o, path, node, size)) return;
    while (o->remaining && !o->write_failed && !r->rep->aborted) {
        ULONG next, blocks;
        if (key == 0 || ++guard > 4000000UL || !find_extent(r, key, &next, &blocks) ||
            blocks == 0 || key >= r->total || blocks > r->total - key) { o->broken = TRUE; break; }
        Rc_Run(o, r->start + key * r->spb, blocks * r->spb);
        key = next;
    }
    Rc_Close(o, prot, secs / 86400UL, (secs % 86400UL) / 60, (secs % 60) * 50, comment);
}

static void do_dir(struct SRes *r, ULONG first, ULONG node);

/* Walks the objects of one container.  mode 0: copy; mode 1: only note
 * the nodes of the directories it names. */
static BOOL do_container(struct SRes *r, const UBYTE *b, int mode)
{
    ULONG off = 24, plen = strlen(r->path);
    BOOL  whole = TRUE;

    while (off + 27 <= r->bsz && !r->rep->aborted) {
        const UBYTE *o = b + off;
        ULONG nl = 0, cl = 0, max = r->bsz - off - 25, len2, k;
        ULONG node, prot, d0, d1, secs;
        UBYTE bits;
        char  name[112], comment[82];

        if (o[25] == 0) break;
        while (nl < max && o[25 + nl]) nl++;
        if (nl >= max || nl > 107) { whole = FALSE; break; }
        while (nl + 1 + cl < max && o[25 + nl + 1 + cl]) cl++;
        if (nl + 1 + cl >= max) { whole = FALSE; break; }
        node = be32(o, 4); prot = be32(o, 8) ^ 0x0FUL;
        d0 = be32(o, 12); d1 = be32(o, 16); secs = be32(o, 20);
        bits = o[24];
        off += (25 + nl + 1 + cl + 1 + 1) & ~1UL;

        if (mode == 1) {
            if ((bits & OT_DIR) && !(bits & (OT_LINK | OT_HARDLINK))) {
                if (r->nrefd == r->caprefd) {
                    ULONG ncap = r->caprefd ? r->caprefd * 2 : 256;
                    ULONG *n = (ULONG *)AllocVec(ncap * sizeof(ULONG), MEMF_PUBLIC);
                    if (!n) return FALSE;
                    if (r->refd) { memcpy(n, r->refd, r->nrefd * sizeof(ULONG)); FreeVec(r->refd); }
                    r->refd = n; r->caprefd = ncap;
                }
                r->refd[r->nrefd++] = node;
            }
            continue;
        }

        memcpy(name, o + 25, nl); name[nl] = '\0';
        if (cl > 79) cl = 79;
        memcpy(comment, o + 25 + nl + 1, cl); comment[cl] = '\0';
        for (k = 0; k < nl; k++) if (name[k] == '/' || name[k] == ':' || (UBYTE)name[k] < 32) name[k] = '_';
        for (k = 0; k < cl; k++) if ((UBYTE)comment[k] < 32) comment[k] = ' ';

        /* the recycled directory of the root */
        if (r->depth == 0 && node == NODE_RECYCLED && (bits & OT_DIR)) {
            if (!(r->opts->deleted || r->opts->only_deleted)) continue;
            if (d1 == 0) continue;
            if (is_container(r, d1, r->tbuf2) && r->tbuf2[24 + 25] == 0 && bel(r->tbuf2, 4) == 0) {
                SETBIT(r->seen, d1);                                                     /* nothing in it */
                continue;
            }
            if (!r->opts->only_deleted) {
                strcpy(r->path, GS(MSG_DELETED_NAME));
                if (!Out_MkDir(r->path)) { r->path[plen] = '\0'; continue; }
            }
            {
                ULONG before = r->st->files;
                r->depth++;
                do_dir(r, d1, node);
                r->depth--;
                r->st->deleted += r->st->files - before;
            }
            r->path[plen] = '\0';
            continue;
        }
        if (r->depth == 0 && r->opts->only_deleted) continue;

        len2 = plen;
        r->path[plen] = '\0';
        if (!Rc_Join(r->path, &len2, name)) continue;
        if (bits & (OT_LINK | OT_HARDLINK)) {
            Rep_Add(r->rep, SEV_WARN, GS(MSG_S_LINK_SKIPPED), r->path);
        } else if (bits & OT_DIR) {
            if (r->depth + 1 >= MAXDEPTH) {
                Rep_Add(r->rep, SEV_WARN, GS(MSG_S_DIR_DAMAGED), r->path);
            } else if (!Out_MkDir(r->path)) {
                Rep_Add(r->rep, SEV_ERROR, GS(MSG_S_CANNOT_MKDIR), r->path);
                r->st->failed++;
            } else {
                r->st->dirs++;
                r->depth++;
                do_dir(r, d1, node);
                r->depth--;
                r->path[len2] = '\0';
                Out_SetMeta(r->path, prot, secs / 86400UL, (secs % 86400UL) / 60, (secs % 60) * 50, comment);
            }
        } else {
            copy_file(r, node, d0, d1, r->path, prot, secs, comment);
        }
        r->path[plen] = '\0';
    }
    r->path[plen] = '\0';
    return whole;
}

/* The directory with this node: its container chain from `first`, then
 * every other container that names the node. */
static void do_dir(struct SRes *r, ULONG first, ULONG node)
{
    UBYTE *b = (UBYTE *)AllocVec(r->bsz, MEMF_PUBLIC);
    ULONG blk = first, i;
    BOOL  damaged = FALSE;

    if (!b) { Rep_Add(r->rep, SEV_FATAL, "%s", GS(MSG_OUT_OF_MEMORY)); r->rep->aborted = TRUE; return; }
    while (blk && !r->rep->aborted) {
        if (blk >= r->total || BIT(r->seen, blk) || !is_container(r, blk, b) || bel(b, 3) != node) { damaged = TRUE; break; }
        SETBIT(r->seen, blk);
        if (!do_container(r, b, 0)) damaged = TRUE;
        blk = bel(b, 4);
    }
    for (i = 0; i < r->nob && !r->rep->aborted; i++) {
        if (r->ob[i].parent != node || BIT(r->seen, r->ob[i].blk)) continue;
        SETBIT(r->seen, r->ob[i].blk);
        damaged = TRUE;
        if (is_container(r, r->ob[i].blk, b)) do_container(r, b, 0);
    }
    if (damaged && !r->rep->aborted)
        Rep_Add(r->rep, SEV_WARN, GS(MSG_S_DIR_DAMAGED), r->path[0] ? r->path : GS(MSG_ROOT_DIR));
    FreeVec(b);
}

static BOOL note_container(struct SRes *r, ULONG blk, const UBYTE *b)
{
    if (r->nob == r->capob) {
        ULONG ncap = r->capob ? r->capob * 2 : 256;
        struct ObRef *n = (struct ObRef *)AllocVec(ncap * sizeof(struct ObRef), MEMF_PUBLIC);
        if (!n) return FALSE;
        if (r->ob) { memcpy(n, r->ob, r->nob * sizeof(struct ObRef)); FreeVec(r->ob); }
        r->ob = n; r->capob = ncap;
    }
    r->ob[r->nob].blk = blk;
    r->ob[r->nob].parent = bel(b, 3);
    r->nob++;
    return TRUE;
}

/* Every object container in use: from the admin spaces, or, when their
 * chain cannot be followed, from the whole volume. */
static BOOL scan_containers(struct SRes *r, ULONG adminspace, UBYTE *b, UBYTE *b2)
{
    ULONG blk = adminspace, guard = 0, k;
    BOOL  chain_ok = TRUE;

    while (blk && guard++ < 65536UL) {
        ULONG off;
        if (blk >= r->total || !rd(r, blk, b) || bel(b, 0) != SFS_ID_ADMC) { chain_ok = FALSE; break; }
        for (off = 24; off + 8 <= r->bsz; off += 8) {
            ULONG space = be32(b, off), bits = be32(b, off + 4);
            if (space == 0) continue;
            for (k = 0; k < 32; k++) {
                if (!(bits & (0x80000000UL >> k)) || space + k >= r->total) continue;
                if (is_container(r, space + k, b2) && !note_container(r, space + k, b2)) return FALSE;
            }
        }
        if (!Rep_Progress(r->rep, guard, 0, GS(MSG_PROG_ORPHANS))) return FALSE;
        blk = be32(b, 12);
    }
    if (chain_ok) return TRUE;
    r->nob = 0;
    for (k = 1; k < r->total; k++) {
        if ((k & 255) == 0 && !Rep_Progress(r->rep, k, r->total, GS(MSG_PROG_ORPHANS))) return FALSE;
        if (is_container(r, k, b2) && !note_container(r, k, b2)) return FALSE;
    }
    return TRUE;
}

static void lost_dirs(struct SRes *r, UBYTE *b)
{
    ULONG i, pass;
    BOOL  made = FALSE;

    /* directories that a lost container names will be reached from it */
    for (i = 0; i < r->nob && !r->rep->aborted; i++) {
        if (BIT(r->seen, r->ob[i].blk)) continue;
        if (is_container(r, r->ob[i].blk, b) && !do_container(r, b, 1)) { /* out of memory: order only */ }
    }
    if (r->nrefd) keyset_sort(r->refd, r->nrefd);
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < r->nob && !r->rep->aborted; i++) {
            ULONG node = r->ob[i].parent, len;
            char  nm[32];
            if (BIT(r->seen, r->ob[i].blk)) continue;
            /* deleted files that were not asked for are not lost */
            if (node == NODE_RECYCLED && !r->opts->deleted) continue;
            if (pass == 0 && r->nrefd && keyset_has(r->refd, r->nrefd, node)) continue;
            if (!made) {
                if (!Out_MkDir(GS(MSG_LOSTFOUND_NAME))) {
                    Rep_Add(r->rep, SEV_ERROR, GS(MSG_S_CANNOT_MKDIR), GS(MSG_LOSTFOUND_NAME));
                    return;
                }
                made = TRUE;
            }
            strcpy(r->path, GS(MSG_LOSTFOUND_NAME));
            len = strlen(r->path);
            snprintf(nm, sizeof(nm), GS(MSG_S_LOSTDIR_NAME), (unsigned long)node);
            if (!Rc_Join(r->path, &len, nm) || !Out_MkDir(r->path)) continue;
            r->st->dirs++;
            r->st->lost++;
            r->depth = 1;
            do_dir(r, 0, node);
        }
    }
}

LONG SfsRescue_Run(struct BlockDev *bd, const struct VolumeInfo *vi,
                   const struct RescueOpts *opts, struct Report *rep,
                   struct RescueStats *st)
{
    struct SRes *r;
    UBYTE *b = NULL, *b2;
    UBYTE  rb[512];
    LONG   rc = RETURN_FAIL;
    ULONG  rootobj, adminspace, before, firstdir = 0;

    memset(st, 0, sizeof(*st));
    r = (struct SRes *)AllocVec(sizeof(struct SRes), MEMF_PUBLIC | MEMF_CLEAR);
    if (!r) { Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_OUT_OF_MEMORY)); return RETURN_FAIL; }
    r->bd = bd; r->rep = rep; r->st = st; r->opts = opts;
    r->start = vi->start_block;
    r->tbuf_blk = 0xFFFFFFFFUL;

    if (!BlockDev_ReadBlocks(bd, r->start, 1, rb) || (bel(rb, 0) & 0xFFFFFF00UL) != 0x53465300UL) {
        Rep_Add(rep, SEV_FATAL, GS(MSG_M_UNREADABLE), "no SFS root block");
        goto out;
    }
    if (rb[3] != 0) { Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_S_SFS2)); goto out; }
    r->total   = be32(rb, 48);
    r->bsz     = be32(rb, 52);
    adminspace = be32(rb, 100);
    rootobj    = be32(rb, 104);
    r->extroot = be32(rb, 108);
    if (r->bsz < 512 || r->bsz > 32768 || (r->bsz & (r->bsz - 1)) || r->total == 0 ||
        r->total > vi->num_blocks / (r->bsz / 512)) {
        Rep_Add(rep, SEV_FATAL, GS(MSG_M_UNREADABLE), "SFS root block geometry not understood");
        goto out;
    }
    r->spb = r->bsz / 512;
    b = (UBYTE *)AllocVec(r->bsz * 4, MEMF_PUBLIC);
    r->seen = (UBYTE *)AllocVec(r->total / 8 + 2, MEMF_PUBLIC | MEMF_CLEAR);
    if (!b || !r->seen || !Rc_Init(&r->out, bd, rep, st)) {
        Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_OUT_OF_MEMORY));
        goto out;
    }
    b2 = b + r->bsz;
    r->tbuf = b2 + r->bsz;
    r->tbuf2 = r->tbuf + r->bsz;

    if (!scan_containers(r, adminspace, b, b2)) {
        if (!rep->aborted) Rep_Add(rep, SEV_FATAL, "%s", GS(MSG_OUT_OF_MEMORY));
        goto out;
    }
    /* the root object: first object of the root container */
    if (is_container(r, rootobj, b)) {
        SETBIT(r->seen, rootobj);
        firstdir = be32(b, 24 + 16);
    }
    before = st->files + st->dirs;
    r->path[0] = '\0';
    r->depth = 0;
    do_dir(r, firstdir, NODE_ROOT);
    if (!opts->only_deleted) {
        if (!rep->aborted && st->files + st->dirs == before && r->nob > 1)
            Rep_Add(rep, SEV_WARN, "%s", GS(MSG_S_NO_ROOTDIR));
        if (!rep->aborted) lost_dirs(r, b);
    } else if (!rep->aborted && st->deleted == 0) {
        Rep_Add(rep, SEV_INFO, "%s", GS(MSG_S_UNDEL_NONE));
    }
    if (r->read_errors + r->out.read_errors)
        Rep_Add(rep, SEV_WARN, GS(MSG_F_READERRORS), (unsigned long)(r->read_errors + r->out.read_errors));

    if (rep->aborted)                              rc = RETURN_WARN;
    else if (st->failed)                           rc = RETURN_ERROR;
    else if (st->partial || rep->count[SEV_WARN])  rc = RETURN_WARN;
    else                                           rc = RETURN_OK;
out:
    Rc_Free(&r->out);
    if (r->seen) FreeVec(r->seen);
    if (r->ob) FreeVec(r->ob);
    if (r->refd) FreeVec(r->refd);
    if (b) FreeVec(b);
    FreeVec(r);
    return rc;
}
