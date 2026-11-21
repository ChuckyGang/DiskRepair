/*
 * undo.h - the undo file of an in-place repair.
 *
 * Before the first block of a repair is written, the original contents
 * of every block about to change are saved here, and the file is closed.
 * UNDO puts them back.  The file must live on another volume.
 *
 * Layout (big-endian longs):
 *   "DRUN", version 1, partition start (device blocks), FS blocks in the
 *   partition, FS block size, record count, DOS type, 0
 *   then per record: block number (FS blocks from the partition start),
 *   block contents.
 */
#ifndef UNDO_H
#define UNDO_H

#include <exec/types.h>
#include "blockdev.h"
#include "volume.h"
#include "report.h"

/* i-th block to save: its number and its ORIGINAL contents. */
typedef BOOL (*UndoGet)(void *ud, ULONG i, ULONG *blk, const UBYTE **orig);

BOOL Undo_Write(const char *path, const struct VolumeInfo *vi, ULONG nblocks, ULONG bsz,
                ULONG count, UndoGet get, void *ud, char *err, ULONG errlen);

/* Write the saved blocks back.  Returns a DOS return code. */
LONG Undo_Restore(struct BlockDev *bd, const struct VolumeInfo *vi, const char *path,
                  struct Report *rep);

#endif /* UNDO_H */
