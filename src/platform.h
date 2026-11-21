/*
 * platform.h - what the engines need from the operating system.
 *
 * platform_amiga.c implements it with dos.library, host/host_platform.c
 * with POSIX.  Nothing else in the core touches the OS for these jobs.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <exec/types.h>
#include "volume.h"

/* Take the volume away from its filing system (on) / give it back (off).
 * TRUE at once for image files and unmounted partitions. */
BOOL Plat_Inhibit(const struct VolumeInfo *v, BOOL on, char *err, ULONG errlen);

/* Does this path (existing or about to be created) live on volume v?
 * Used to refuse an undo file or a rescue destination on the damaged
 * volume itself. */
BOOL Plat_PathOnVolume(const char *path, const struct VolumeInfo *v);

/* --- rescue destination -------------------------------------------- */
/* rel paths use '/' between components and are relative to the
 * destination given to Out_Begin(); "" is the destination itself. */
BOOL Out_Begin(const char *dest, char *err, ULONG errlen);
void Out_End(void);
BOOL Out_Exists(const char *rel);
BOOL Out_MkDir(const char *rel);
APTR Out_Create(const char *rel);
BOOL Out_Write(APTR fh, const void *buf, ULONG len);
void Out_Close(APTR fh);
/* protection bits, datestamp and comment as stored on the Amiga volume */
void Out_SetMeta(const char *rel, ULONG prot, ULONG days, ULONG mins, ULONG ticks,
                 const char *comment);

#endif /* PLATFORM_H */
