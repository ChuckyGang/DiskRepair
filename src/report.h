/*
 * report.h - how the engines talk to a front end (CLI, Amiga window,
 * Linux window): findings with a severity, progress, and an abort poll.
 *
 * The engines never print.  Every finding goes through Rep_Add(), already
 * formatted and localized, so the same text ends up in the Shell, in the
 * window's list and in a log file.
 */
#ifndef REPORT_H
#define REPORT_H

#include <exec/types.h>

#define SEV_INFO   0     /* statistics, notes */
#define SEV_WARN   1     /* harmless or cosmetic, volume usable */
#define SEV_ERROR  2     /* structure damaged */
#define SEV_FATAL  3     /* nothing more can be checked */
#define SEV_FIX    4     /* a planned or applied repair step */
#define SEV_COUNT  5

struct Report {
    void  *ud;
    void (*line)(void *ud, int sev, const char *text);
    /* done/total in whatever unit the stage counts; FALSE = stop */
    BOOL (*progress)(void *ud, ULONG done, ULONG total, const char *what);
    ULONG  count[SEV_COUNT];
    ULONG  shown[SEV_COUNT];
    ULONG  max_per_sev;      /* 0 = unlimited; further lines are only counted */
    BOOL   aborted;
};

void Rep_Init(struct Report *r);
void Rep_Add(struct Report *r, int sev, const char *fmt, ...);
/* TRUE = keep going */
BOOL Rep_Progress(struct Report *r, ULONG done, ULONG total, const char *what);

#endif /* REPORT_H */
