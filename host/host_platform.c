/* host_platform.c - platform.h on Linux (image files and raw disks).
 *
 * There is no handler to inhibit.  The rescue destination is a directory;
 * Amiga names are Latin-1 and become UTF-8 file names.  What Linux has
 * no place for (protection bits, file comments) is written to
 * DiskRepair-attributes.txt in the destination, one line per object. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include "amiga_compat.h"
#include "volume.h"
#include "platform.h"

static char  g_dest[2048];
static FILE *g_attr;

BOOL Plat_Inhibit(const struct VolumeInfo *v, BOOL on, char *err, ULONG errlen)
{ (void)v; (void)on; (void)err; (void)errlen; return TRUE; }

BOOL Plat_PathOnVolume(const char *path, const struct VolumeInfo *v)
{ (void)path; (void)v; return FALSE; }

static void join(char *out, size_t outlen, const char *rel)
{
    size_t n = (size_t)snprintf(out, outlen, "%s", g_dest);
    const unsigned char *p = (const unsigned char *)rel;
    if (!*p) return;
    if (n + 1 < outlen) out[n++] = '/';
    for (; *p && n + 3 < outlen; p++) {
        if (*p < 0x80) out[n++] = (char)*p;
        else { out[n++] = (char)(0xC0 | (*p >> 6)); out[n++] = (char)(0x80 | (*p & 0x3F)); }
    }
    out[n] = '\0';
}

static int make_dir(const char *full)
{
    struct stat st;
    if (stat(full, &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(full, 0755) == 0;
}

BOOL Out_Begin(const char *dest, char *err, ULONG errlen)
{
    char attr[2200];
    snprintf(g_dest, sizeof(g_dest), "%s", dest);
    while (strlen(g_dest) > 1 && g_dest[strlen(g_dest) - 1] == '/') g_dest[strlen(g_dest) - 1] = '\0';
    if (!make_dir(g_dest)) {
        if (err && errlen) snprintf(err, errlen, "Cannot use %s as the destination directory.", dest);
        return FALSE;
    }
    snprintf(attr, sizeof(attr), "%s/DiskRepair-attributes.txt", g_dest);
    g_attr = fopen(attr, "a");
    return TRUE;
}

void Out_End(void)
{
    if (g_attr) { fclose(g_attr); g_attr = NULL; }
    g_dest[0] = '\0';
}

BOOL Out_Exists(const char *rel)
{
    char full[4200];
    struct stat st;
    join(full, sizeof(full), rel);
    return lstat(full, &st) == 0;
}

BOOL Out_MkDir(const char *rel)
{
    char full[4200];
    join(full, sizeof(full), rel);
    return make_dir(full) ? TRUE : FALSE;
}

APTR Out_Create(const char *rel)
{
    char full[4200];
    join(full, sizeof(full), rel);
    return (APTR)fopen(full, "wb");
}

BOOL Out_Write(APTR fh, const void *buf, ULONG len)
{
    return fwrite(buf, 1, len, (FILE *)fh) == len;
}

void Out_Close(APTR fh) { if (fh) fclose((FILE *)fh); }

void Out_SetMeta(const char *rel, ULONG prot, ULONG days, ULONG mins, ULONG ticks, const char *comment)
{
    char full[4200];
    join(full, sizeof(full), rel);
    if (days < 100000UL && mins < 1440 && ticks < 3000) {
        /* Amiga dates count from 1978-01-01 in local time: take the
         * calendar fields and let mktime() apply this machine's zone */
        struct timeval tv[2];
        time_t t = (time_t)(252460800L + (long)days * 86400L + (long)mins * 60L + (long)ticks / 50L);
        struct tm tm;
        gmtime_r(&t, &tm);
        tm.tm_isdst = -1;
        tv[0].tv_sec = tv[1].tv_sec = mktime(&tm);
        tv[0].tv_usec = tv[1].tv_usec = 0;
        utimes(full, tv);
    }
    if (g_attr && (prot || (comment && comment[0])))
        fprintf(g_attr, "%s\tprotection=0x%08lx\tcomment=%s\n", rel, (unsigned long)prot, comment ? comment : "");
}
