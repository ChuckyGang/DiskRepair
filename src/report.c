/*
 * report.c - see report.h.
 */
#include <exec/types.h>
#include "clib.h"
#include "report.h"

void Rep_Init(struct Report *r)
{
    memset(r, 0, sizeof(*r));
}

void Rep_Add(struct Report *r, int sev, const char *fmt, ...)
{
    char buf[300];
    va_list ap;
    if (sev < 0 || sev >= SEV_COUNT) sev = SEV_INFO;
    r->count[sev]++;
    if (r->max_per_sev && sev != SEV_INFO && sev != SEV_FATAL && r->shown[sev] >= r->max_per_sev) return;
    r->shown[sev]++;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (r->line) r->line(r->ud, sev, buf);
}

BOOL Rep_Progress(struct Report *r, ULONG done, ULONG total, const char *what)
{
    if (r->aborted) return FALSE;
    if (r->progress && !r->progress(r->ud, done, total, what)) { r->aborted = TRUE; return FALSE; }
    return TRUE;
}
