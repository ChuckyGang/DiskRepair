/* host_main.c - Linux DiskRepair entry point (image-file mode). */
#include <stdio.h>
#include <string.h>
#include "amiga_compat.h"
#include "cli.h"

BOOL  DiskRepair_Debug;   /* set by cli.c DEBUG */
#ifdef DISKREPAIR_HOST_GUI
extern int    host_argc;      /* gui/host_req.c owns them in the window build */
extern char **host_argv;
#else
int   host_argc;
char **host_argv;
#endif

void host_usage(void)
{
    printf("DiskRepair (host build, image-file mode)\n"
           "Usage: diskrepair IMAGE=<file> LIST\n"
           "       diskrepair IMAGE=<file> [PART=<name|index>] [DEEP] [LOST]        check (read-only)\n"
           "       diskrepair IMAGE=<file> [PART=...] RESCUE=<directory> [DELETED]   copy the files out\n"
           "       diskrepair IMAGE=<file> [PART=...] UNDELETE=<directory>           copy only deleted files out\n"
           "       diskrepair IMAGE=<file> [PART=...] DRYRUN                         show the repair plan\n"
           "       diskrepair IMAGE=<file> [PART=...] REPAIR UNDOFILE=<file> [FORCE] repair in place\n"
           "       diskrepair IMAGE=<file> [PART=...] UNDO UNDOFILE=<file> [FORCE]   put the old blocks back\n"
           "       diskrepair ?\n"
           "The image may be a raw disk (/dev/sdX, needs root).\n");
}

#ifndef DISKREPAIR_HOST_GUI
int main(int argc, char **argv)
{
    LONG rc;
    host_argc = argc;
    host_argv = argv;
    SetSignal(0, 0);          /* arm the Ctrl-C handler before any long run */
    rc = cli_run();
    if (rc == CLI_NO_ARGS) { host_usage(); return 5; }
    return (int)rc;
}
#endif
