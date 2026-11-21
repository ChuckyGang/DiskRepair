/*
 * resc_common.h - what the PFS3 and SFS rescue engines share: one output
 * file being filled from runs of device blocks.
 */
#ifndef RESC_COMMON_H
#define RESC_COMMON_H

#include <exec/types.h>
#include "blockdev.h"
#include "report.h"
#include "ffs_repair.h"     /* RescueOpts, RescueStats */

#define RC_PATHMAX 1024
#define RC_CHUNK   64       /* device blocks per read */

struct RcOut {
    struct BlockDev    *bd;
    struct Report      *rep;
    struct RescueStats *st;
    UBYTE *buf;             /* RC_CHUNK * 512 bytes */
    APTR   fh;
    ULONG  remaining;       /* bytes of the file still to write */
    ULONG  size;
    ULONG  holes;
    BOOL   write_failed;
    BOOL   broken;          /* block list could not be followed to the end */
    ULONG  read_errors;
    char   dest[RC_PATHMAX + 16];
};

BOOL Rc_Init(struct RcOut *o, struct BlockDev *bd, struct Report *rep, struct RescueStats *st);
void Rc_Free(struct RcOut *o);
/* path, or path_<key> when that exists already; result in o->dest */
void Rc_FreeName(struct RcOut *o, const char *path, ULONG key);
/* FALSE: could not be created (reported and counted) */
BOOL Rc_Open(struct RcOut *o, const char *path, ULONG key, ULONG size);
/* n device blocks from absolute device block `first`; unreadable ones
 * become zeros.  Stops by itself at the file size. */
void Rc_Run(struct RcOut *o, ULONG first, ULONG n);
/* close, set the attributes, report an incomplete file, count it */
void Rc_Close(struct RcOut *o, ULONG prot, ULONG days, ULONG mins, ULONG ticks, const char *comment);
/* append "/name" to path (in place); FALSE when it does not fit */
BOOL Rc_Join(char *path, ULONG *len, const char *name);

#endif
