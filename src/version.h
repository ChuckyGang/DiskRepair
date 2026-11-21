#ifndef VERSION_H
#define VERSION_H

#define DISKREPAIR_VERNUM   "0.1"
#define DISKREPAIR_VERDATE  "21.11.2026"      /* dd.mm.yyyy, update with VERNUM */
#define DISKREPAIR_VERSION  "V" DISKREPAIR_VERNUM
#define DISKREPAIR_VERTITLE "DiskRepair " DISKREPAIR_VERSION
/* AmigaOS "Version" command string; defined in main.c (kept alive there). */
#define DISKREPAIR_VERSTRING "$VER: DiskRepair " DISKREPAIR_VERNUM " (" DISKREPAIR_VERDATE ")"

/* Defined in build.c, recompiled on every build (see Makefile). */
extern const char DiskRepair_BuildStamp[];

#endif /* VERSION_H */
