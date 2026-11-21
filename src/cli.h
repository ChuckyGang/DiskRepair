/*
 * cli.h - command-line mode, shared by the Amiga binary and the host build.
 *
 *   DiskRepair VOLUME,IMAGE/K,PART/K,LIST/S,CHECK/S,DEEP/S,LOST/S,RESCUE/K,
 *              DELETED/S,REPAIR/S,DRYRUN/S,UNDOFILE/K,UNDO/S,FORCE/S,ALL/S,DEBUG/S
 *
 *   DiskRepair LIST                       list the partitions
 *   DiskRepair DH1:                       check DH1: (read-only)
 *   DiskRepair DH1: DEEP LOST             ... also read all data, search for lost files
 *   DiskRepair DH1: RESCUE=Work:Saved     copy everything readable to Work:Saved
 *   DiskRepair DH1: RESCUE=Work:Saved DELETED    ... including deleted files
 *   DiskRepair DH1: DRYRUN                show what an in-place repair would do
 *   DiskRepair DH1: REPAIR UNDOFILE=Work:dh1.undo    repair in place (asks for YES)
 *   DiskRepair DH1: UNDO UNDOFILE=Work:dh1.undo      put the old blocks back
 *   DiskRepair IMAGE=disk.hdf PART=DH1 ...           the same on an image file
 *
 * Nothing is written to the volume unless REPAIR or UNDO is given.
 */
#ifndef CLI_H
#define CLI_H

#include <exec/types.h>

#define CLI_NO_ARGS (-1)   /* empty command line: caller falls through to the GUI */

LONG cli_run(void);

#endif /* CLI_H */
