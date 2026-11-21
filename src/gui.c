/*
 * gui.c - GadTools main window (see gui.h).
 *
 * Layout, top to bottom:
 *   listview   one line per partition (name, volume, filesystem, size)
 *   buttons    Check | Repair plan | Repair | Rescue | Undo | Stop
 *   options    read all data / search lost files / deleted files too
 *   strings    rescue destination, undo file
 *   buttons    Image... | Rescan | About
 *   listview   the findings, as the CLI prints them
 *   status     progress while working, the verdict afterwards
 *
 * A job runs synchronously; its progress callback drains the window's
 * message port so Stop and the close gadget work and the window redraws.
 *
 * The same source is the Linux window: host/host_gui.c implements the
 * Intuition / GadTools / ASL calls used here on SDL2.  Pointers in tag
 * lists are cast to IPTR for that reason.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include "gt_compat.h"

#include "clib.h"
#include "blockdev.h"
#include "volume.h"
#include "report.h"
#include "fscheck.h"
#include "gui.h"
#include "version.h"
#include "locale_support.h"

extern struct Library *AslBase;

char Gui_StartImage[256];

enum { GID_LIST = 1, GID_CHECK, GID_PLAN, GID_REPAIR, GID_RESCUE, GID_UNDO, GID_STOP,
       GID_DEEP, GID_LOST, GID_DELETED, GID_DEST, GID_UNDOFILE, GID_IMAGE, GID_RESCAN,
       GID_ABOUT, GID_RESULTS };

#define MAX_LINES   3000
#define PATH_LEN    256

struct Line {
    struct Node node;
    char text[1];
};

struct Gui {
    struct Screen  *scr;
    APTR            vi;
    struct Window  *win;
    struct Gadget  *glist, *lv_gad, *res_gad, *stop_gad, *dest_gad, *undo_gad;
    struct Gadget  *deep_gad, *lost_gad, *del_gad;
    struct List     labels;
    struct Node     nodes[MAX_VOLUMES];
    char            texts[MAX_VOLUMES][96];
    struct VolumeList *vols;
    WORD            selected;
    char            image[PATH_LEN];     /* "" = the mounted volumes */
    struct List     lines;
    ULONG           nlines, pending;
    BOOL            lines_attached;
    UWORD           res_cols;
    /* geometry */
    UWORD fh, baseline, bor_l, bor_t, status_y, win_w, win_h;
    /* state */
    BOOL  quit, busy, stop;
    BOOL  deep, lost, deleted;
    char  status[200];
    ULONG last_pct;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void init_list(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static void draw_status(struct Gui *g)
{
    struct RastPort *rp = g->win->RPort;
    UWORD x = g->bor_l + 6;
    SetAPen(rp, 0);
    RectFill(rp, x, g->status_y, g->win_w - g->win->BorderRight - 1, g->status_y + g->fh - 1);
    SetAPen(rp, 1);
    SetDrMd(rp, JAM1);
    Move(rp, x, g->status_y + g->baseline);
    Text(rp, (CONST_STRPTR)g->status, strlen(g->status));
}

static void set_status(struct Gui *g, const char *s)
{
    strncpy(g->status, s, sizeof(g->status) - 1);
    g->status[sizeof(g->status) - 1] = '\0';
    draw_status(g);
}

static void set_disabled(struct Gui *g, struct Gadget *gad, BOOL off)
{
    struct TagItem t[] = { { GA_Disabled, (ULONG)off }, { TAG_DONE, 0 } };
    GT_SetGadgetAttrsA(gad, g->win, NULL, t);
}

static LONG ask(struct Gui *g, const char *title, const char *body, const char *gadgets)
{
    struct EasyStruct es;
    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)title;
    es.es_TextFormat   = (UBYTE *)body;
    es.es_GadgetFormat = (UBYTE *)gadgets;
    return EasyRequestArgs(g->win, &es, NULL, NULL);
}

static const char *gad_string(struct Gadget *gad)
{
    return (const char *)((struct StringInfo *)gad->SpecialInfo)->Buffer;
}

/* ------------------------------------------------------------------ */
/* result lines                                                        */
/* ------------------------------------------------------------------ */

static void lines_detach(struct Gui *g)
{
    struct TagItem t[] = { { GTLV_Labels, (ULONG)~0UL }, { TAG_DONE, 0 } };
    if (!g->lines_attached) return;
    GT_SetGadgetAttrsA(g->res_gad, g->win, NULL, t);
    g->lines_attached = FALSE;
}

static void lines_attach(struct Gui *g)
{
    struct TagItem t[] = { { GTLV_Labels, (IPTR)&g->lines },
                           { GTLV_Top, g->nlines > 4 ? g->nlines - 4 : 0 }, { TAG_DONE, 0 } };
    GT_SetGadgetAttrsA(g->res_gad, g->win, NULL, t);
    g->lines_attached = TRUE;
    g->pending = 0;
}

static void lines_clear(struct Gui *g)
{
    struct Node *n, *next;
    lines_detach(g);
    for (n = g->lines.lh_Head; (next = n->ln_Succ) != NULL; n = next) FreeVec(n);
    init_list(&g->lines);
    g->nlines = 0;
    lines_attach(g);
}

static void add_piece(struct Gui *g, const char *s, ULONG len)
{
    struct Line *ln;
    if (g->nlines >= MAX_LINES) return;
    ln = (struct Line *)AllocVec(sizeof(struct Line) + len, MEMF_PUBLIC | MEMF_CLEAR);
    if (!ln) return;
    memcpy(ln->text, s, len);
    ln->text[len] = '\0';
    ln->node.ln_Name = ln->text;
    lines_detach(g);
    AddTail(&g->lines, &ln->node);
    g->nlines++;
    g->pending++;
}

/* Add one finding, folded at word boundaries to the list's width. */
static void add_line(struct Gui *g, const char *s)
{
    ULONG cols = g->res_cols ? g->res_cols : 70;
    ULONG len = strlen(s);
    BOOL  first = TRUE;
    while (len > 0 || first) {
        ULONG take = len, room = first ? cols : cols - 2;
        char  buf[200];
        if (room > sizeof(buf) - 4) room = sizeof(buf) - 4;
        if (take > room) {
            take = room;
            while (take > room / 2 && s[take] != ' ') take--;
            if (s[take] != ' ') take = room;
        }
        if (first) { memcpy(buf, s, take); buf[take] = '\0'; }
        else       { buf[0] = ' '; buf[1] = ' '; memcpy(buf + 2, s, take); buf[take + 2] = '\0'; }
        add_piece(g, buf, strlen(buf));
        s += take; len -= take;
        while (len && *s == ' ') { s++; len--; }
        first = FALSE;
    }
    if (g->pending >= 20) lines_attach(g);
}

/* ------------------------------------------------------------------ */
/* volume list                                                         */
/* ------------------------------------------------------------------ */

static void fill_labels(struct Gui *g)
{
    UWORD i;
    char fs[32], sz[24];
    init_list(&g->labels);
    g->vols->count = 0;
    if (g->image[0]) {
        struct BlockDev *bd = BlockDev_OpenFile(g->image);
        char err[80];
        if (bd) {
            if (!Volume_EnumerateImage(bd, g->vols, err, sizeof(err))) g->vols->count = 0;
            BlockDev_Close(bd);
        }
    } else {
        Volume_EnumerateMounted(g->vols);
    }
    for (i = 0; i < g->vols->count; i++) {
        struct VolumeInfo *v = &g->vols->v[i];
        Volume_FsName(v->dostype, fs, sizeof(fs));
        Volume_SizeString(v->num_blocks, v->sector_size, sz, sizeof(sz));
        AD_SNPRINTF(g->texts[i], "%-5s %-14s %-13s %8s  %s", v->dosname,
                    v->volname[0] ? v->volname : "-", fs, sz,
                    g->image[0] ? "" : v->devname);
        g->nodes[i].ln_Name = g->texts[i];
        g->nodes[i].ln_Pri  = 0;
        AddTail(&g->labels, &g->nodes[i]);
    }
}

static void rescan(struct Gui *g)
{
    struct TagItem detach[] = { { GTLV_Labels, (ULONG)~0UL }, { TAG_DONE, 0 } };
    GT_SetGadgetAttrsA(g->lv_gad, g->win, NULL, detach);
    fill_labels(g);
    g->selected = g->vols->count ? 0 : -1;
    {
        struct TagItem attach[] = { { GTLV_Labels, (IPTR)&g->labels },
                                    { GTLV_Selected, (ULONG)(g->selected < 0 ? ~0UL : (ULONG)g->selected) },
                                    { TAG_DONE, 0 } };
        GT_SetGadgetAttrsA(g->lv_gad, g->win, NULL, attach);
    }
    set_status(g, g->vols->count ? GS(MSG_GUI_SELECT) : GS(MSG_GUI_NO_PARTITIONS));
}

/* ------------------------------------------------------------------ */
/* running a job                                                       */
/* ------------------------------------------------------------------ */

static void gui_line(void *ud, int sev, const char *text)
{
    struct Gui *g = (struct Gui *)ud;
    char buf[340];
    switch (sev) {
    case SEV_WARN:  AD_SNPRINTF(buf, "%s: %s", GS(MSG_SEV_WARNING), text); break;
    case SEV_ERROR: AD_SNPRINTF(buf, "%s: %s", GS(MSG_SEV_ERROR), text); break;
    case SEV_FATAL: AD_SNPRINTF(buf, "%s: %s", GS(MSG_SEV_FATAL), text); break;
    case SEV_FIX:   AD_SNPRINTF(buf, "%s: %s", GS(MSG_SEV_FIX), text); break;
    default:        AD_SNPRINTF(buf, "%s", text); break;
    }
    add_line(g, buf);
}

static BOOL gui_progress(void *ud, ULONG done, ULONG total, const char *what)
{
    struct Gui *g = (struct Gui *)ud;
    struct IntuiMessage *imsg;
    ULONG pct = total ? (done >= total ? 100 : (done / 64) * 100 / (total / 64 + 1)) : 0xFFFF0000UL + (done >> 8);

    while ((imsg = GT_GetIMsg(g->win->UserPort)) != NULL) {
        ULONG  iclass = imsg->Class;
        struct Gadget *gad = (struct Gadget *)imsg->IAddress;
        GT_ReplyIMsg(imsg);
        if (iclass == IDCMP_CLOSEWINDOW) g->quit = TRUE;
        else if (iclass == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(g->win); draw_status(g); GT_EndRefresh(g->win, TRUE); }
        else if (iclass == IDCMP_GADGETUP && gad->GadgetID == GID_STOP) g->stop = TRUE;
    }
    if (g->quit || g->stop) return FALSE;
    if (pct != g->last_pct) {
        char s[160];
        g->last_pct = pct;
        if (total) AD_SNPRINTF(s, GS(MSG_GUI_PROGRESS_PCT), what, (unsigned long)pct);
        else       AD_SNPRINTF(s, GS(MSG_GUI_PROGRESS_COUNT), what, (unsigned long)done);
        set_status(g, s);
    }
    return TRUE;
}

/* what: 0 check, 1 repair plan, 2 repair, 3 rescue, 4 undo */
static void run_job(struct Gui *g, int what)
{
    struct VolumeInfo *v;
    struct BlockDev *bd;
    struct Report rep;
    char  head[200], dest[PATH_LEN], undo[PATH_LEN];
    LONG  rc = RETURN_FAIL;

    if (g->busy) return;
    if (g->selected < 0 || g->selected >= (WORD)g->vols->count) { set_status(g, GS(MSG_GUI_SELECT)); return; }
    v = &g->vols->v[g->selected];
    strncpy(dest, gad_string(g->dest_gad), sizeof(dest) - 1); dest[sizeof(dest) - 1] = '\0';
    strncpy(undo, gad_string(g->undo_gad), sizeof(undo) - 1); undo[sizeof(undo) - 1] = '\0';

    if (what == 3 && !dest[0]) { set_status(g, GS(MSG_GUI_NEED_DEST)); ActivateGadget(g->dest_gad, g->win, NULL); return; }
    if ((what == 2 || what == 4) && !undo[0]) { set_status(g, GS(MSG_GUI_NEED_UNDO)); ActivateGadget(g->undo_gad, g->win, NULL); return; }
    if (what == 2) {
        char body[500];
        AD_SNPRINTF(body, GS(MSG_GUI_REPAIR_BODY), v->dosname, undo);
        if (ask(g, GS(MSG_APPNAME), body, GS(MSG_GUI_REPAIR_GADS)) != 1) return;
    }
    if (what == 4) {
        char body[400];
        AD_SNPRINTF(body, GS(MSG_GUI_UNDO_BODY), undo, v->dosname);
        if (ask(g, GS(MSG_APPNAME), body, GS(MSG_GUI_UNDO_GADS)) != 1) return;
    }

    bd = g->image[0] ? BlockDev_OpenFile(g->image) : BlockDev_Open(v->devname, v->unit);
    if (!bd) { set_status(g, GS(MSG_GUI_CANNOT_OPEN)); return; }

    lines_clear(g);
    AD_SNPRINTF(head, GS(MSG_GUI_HEAD), GS(MSG_GUI_JOB_CHECK + what), v->dosname);
    add_line(g, head);
    Rep_Init(&rep);
    rep.ud = g; rep.line = gui_line; rep.progress = gui_progress;
    rep.max_per_sev = 200;
    g->busy = TRUE; g->stop = FALSE; g->last_pct = 0xFFFFFFFFUL;
    set_disabled(g, g->stop_gad, FALSE);

    switch (what) {
    case 0: {
        struct CheckOpts co;
        co.deep = g->deep; co.lost = g->lost;
        rc = Fs_Check(bd, v, &co, &rep);
        if (!rep.aborted)
            add_line(g, rc == RETURN_OK ? GS(MSG_V_CLEAN) : rc == RETURN_WARN ? GS(MSG_V_WARN) :
                        rc == RETURN_ERROR ? GS(MSG_GUI_V_DAMAGED) : GS(MSG_GUI_V_FAILED));
        break; }
    case 1: case 2: {
        struct RepairOpts ro;
        ro.dry = (what == 1); ro.undo_path = undo;
        rc = Fs_Repair(bd, v, &ro, &rep);
        break; }
    case 3: {
        struct RescueOpts so;
        struct RescueStats st;
        so.deleted = g->deleted;
        rc = Fs_Rescue(bd, v, dest, &so, &rep, &st);
        break; }
    case 4:
        rc = Fs_Undo(bd, v, undo, &rep);
        break;
    }
    BlockDev_Close(bd);
    g->busy = FALSE;
    set_disabled(g, g->stop_gad, TRUE);
    if (rep.aborted) add_line(g, GS(MSG_GUI_STOPPED));
    lines_attach(g);
    {
        char s[160];
        AD_SNPRINTF(s, GS(MSG_GUI_DONE), GS(MSG_GUI_JOB_CHECK + what),
                    (unsigned long)rep.count[SEV_ERROR] + rep.count[SEV_FATAL], (unsigned long)rep.count[SEV_WARN]);
        set_status(g, rep.aborted ? GS(MSG_GUI_STOPPED) : s);
    }
    (void)rc;
}

static void pick_image(struct Gui *g)
{
    struct FileRequester *fr;
    struct TagItem t[] = { { ASLFR_TitleText, (IPTR)GS(MSG_GUI_IMAGE_TITLE) }, { TAG_DONE, 0 } };
    if (!AslBase) { set_status(g, GS(MSG_GUI_NO_ASL)); return; }
    fr = (struct FileRequester *)AllocAslRequest(ASL_FileRequest, t);
    if (!fr) return;
    if (AslRequest(fr, NULL) && fr->fr_File && fr->fr_File[0]) {
        strncpy(g->image, (const char *)fr->fr_Drawer, sizeof(g->image) - 1);
        g->image[sizeof(g->image) - 1] = '\0';
        AddPart((STRPTR)g->image, (CONST_STRPTR)fr->fr_File, sizeof(g->image));
        rescan(g);
        if (g->vols->count == 0) { set_status(g, GS(MSG_GUI_CANNOT_OPEN)); }
    }
    FreeAslRequest(fr);
}

static void show_about(struct Gui *g)
{
    char body[600];
    AD_SNPRINTF(body, GS(MSG_ABOUT_BODY), DISKREPAIR_VERSION, DiskRepair_BuildStamp);
    ask(g, DISKREPAIR_VERTITLE, body, GS(MSG_OK));
}

static void handle_msg(struct Gui *g, ULONG iclass, UWORD code, struct Gadget *gad)
{
    switch (iclass) {
    case IDCMP_CLOSEWINDOW:
        g->quit = TRUE;
        break;
    case IDCMP_REFRESHWINDOW:
        GT_BeginRefresh(g->win);
        draw_status(g);
        GT_EndRefresh(g->win, TRUE);
        break;
    case IDCMP_GADGETUP:
        switch (gad->GadgetID) {
        case GID_LIST:    g->selected = (WORD)code; break;
        case GID_CHECK:   run_job(g, 0); break;
        case GID_PLAN:    run_job(g, 1); break;
        case GID_REPAIR:  run_job(g, 2); break;
        case GID_RESCUE:  run_job(g, 3); break;
        case GID_UNDO:    run_job(g, 4); break;
        case GID_DEEP:    g->deep    = (code != 0); break;
        case GID_LOST:    g->lost    = (code != 0); break;
        case GID_DELETED: g->deleted = (code != 0); break;
        case GID_IMAGE:   pick_image(g); break;
        case GID_RESCAN:  g->image[0] = '\0'; rescan(g); break;
        case GID_ABOUT:   show_about(g); break;
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* window                                                              */
/* ------------------------------------------------------------------ */

static struct Gadget *button(struct Gui *g, struct Gadget *prev, struct NewGadget *ng,
                             LONG msg, UWORD id, BOOL disabled)
{
    struct TagItem t[] = { { GA_Disabled, (ULONG)disabled }, { TAG_DONE, 0 } };
    (void)g;
    ng->ng_GadgetText = (UBYTE *)GS(msg);
    ng->ng_GadgetID   = id;
    ng->ng_Flags      = PLACETEXT_IN;
    return CreateGadgetA(BUTTON_KIND, prev, ng, t);
}

static BOOL build_window(struct Gui *g)
{
    struct Gadget *prev;
    struct NewGadget ng;
    UWORD pad = 4, btn_h, btn_w, lv_h, inner_w, y, bor_r, bor_b, cw, lbl_w, x0, res_h;
    static const LONG row1[6] = { MSG_BTN_CHECK, MSG_BTN_PLAN, MSG_BTN_REPAIR, MSG_BTN_RESCUE, MSG_BTN_UNDO, MSG_BTN_STOP };
    static const UWORD id1[6] = { GID_CHECK, GID_PLAN, GID_REPAIR, GID_RESCUE, GID_UNDO, GID_STOP };
    UWORD i;

    g->scr = LockPubScreen(NULL);
    if (!g->scr) return FALSE;
    g->vi = GetVisualInfoA(g->scr, NULL);
    if (!g->vi) return FALSE;

    g->fh       = g->scr->Font->ta_YSize;
    g->baseline = g->scr->RastPort.Font ? g->scr->RastPort.Font->tf_Baseline : g->fh - 2;
    cw          = g->scr->RastPort.Font ? g->scr->RastPort.Font->tf_XSize : 8;
    if (cw == 0) cw = 8;
    g->bor_l    = g->scr->WBorLeft;
    g->bor_t    = (UWORD)(g->scr->WBorTop + g->fh + 1);
    bor_r       = g->scr->WBorRight;
    bor_b       = g->scr->WBorBottom;

    g->win_w = g->scr->Width  < 640 ? g->scr->Width : 640;
    g->win_h = g->scr->Height < 480 ? g->scr->Height - 2 : 480;
    inner_w  = g->win_w - g->bor_l - bor_r;
    btn_h    = g->fh + 4;
    btn_w    = (inner_w - pad * 7) / 6;
    lv_h     = 4 * (g->fh + 1) + 4;
    x0       = g->bor_l + pad;

    if (!CreateContext(&g->glist)) return FALSE;
    prev = g->glist;
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = g->vi;
    ng.ng_TextAttr   = g->scr->Font;

    y = g->bor_t + pad;
    ng.ng_LeftEdge = x0; ng.ng_TopEdge = y;
    ng.ng_Width = inner_w - pad * 2; ng.ng_Height = lv_h;
    ng.ng_GadgetID = GID_LIST;
    {
        struct TagItem t[] = { { GTLV_Labels, (IPTR)&g->labels },
                               { GTLV_Selected, (ULONG)(g->selected < 0 ? ~0UL : (ULONG)g->selected) },
                               { GTLV_ShowSelected, 0 }, { TAG_DONE, 0 } };
        prev = g->lv_gad = CreateGadgetA(LISTVIEW_KIND, prev, &ng, t);
        if (!prev) return FALSE;
    }
    y += lv_h + pad;

    ng.ng_TopEdge = y; ng.ng_Height = btn_h; ng.ng_Width = btn_w;
    for (i = 0; i < 6; i++) {
        ng.ng_LeftEdge = x0 + i * (btn_w + pad);
        prev = button(g, prev, &ng, row1[i], id1[i], id1[i] == GID_STOP);
        if (!prev) return FALSE;
        if (id1[i] == GID_STOP) g->stop_gad = prev;
    }
    y += btn_h + pad;

    /* three checkboxes share a row */
    {
        static const LONG cbm[3] = { MSG_CB_DEEP, MSG_CB_LOST, MSG_CB_DELETED };
        static const UWORD cbid[3] = { GID_DEEP, GID_LOST, GID_DELETED };
        UWORD third = (inner_w - pad * 2) / 3;
        for (i = 0; i < 3; i++) {
            struct TagItem t[] = { { GTCB_Checked, 0 }, { TAG_DONE, 0 } };
            ng.ng_LeftEdge = x0 + i * third; ng.ng_TopEdge = y;
            ng.ng_Width = 26; ng.ng_Height = g->fh + 3;
            ng.ng_GadgetText = (UBYTE *)GS(cbm[i]); ng.ng_GadgetID = cbid[i];
            ng.ng_Flags = PLACETEXT_RIGHT;
            prev = CreateGadgetA(CHECKBOX_KIND, prev, &ng, t);
            if (!prev) return FALSE;
            if (i == 0) g->deep_gad = prev; else if (i == 1) g->lost_gad = prev; else g->del_gad = prev;
        }
        y += g->fh + 3 + pad;
    }

    /* two string gadgets with their labels on the left */
    lbl_w = (UWORD)(strlen(GS(MSG_LBL_DEST)) > strlen(GS(MSG_LBL_UNDO)) ? strlen(GS(MSG_LBL_DEST)) : strlen(GS(MSG_LBL_UNDO)));
    lbl_w = (UWORD)((lbl_w + 2) * cw);
    {
        struct TagItem t[] = { { GTST_MaxChars, PATH_LEN - 1 }, { TAG_DONE, 0 } };
        ng.ng_LeftEdge = x0 + lbl_w; ng.ng_Width = inner_w - pad * 2 - lbl_w;
        ng.ng_Height = g->fh + 6; ng.ng_Flags = PLACETEXT_LEFT;
        ng.ng_TopEdge = y; ng.ng_GadgetText = (UBYTE *)GS(MSG_LBL_DEST); ng.ng_GadgetID = GID_DEST;
        prev = g->dest_gad = CreateGadgetA(STRING_KIND, prev, &ng, t);
        if (!prev) return FALSE;
        y += g->fh + 6 + pad;
        ng.ng_TopEdge = y; ng.ng_GadgetText = (UBYTE *)GS(MSG_LBL_UNDO); ng.ng_GadgetID = GID_UNDOFILE;
        prev = g->undo_gad = CreateGadgetA(STRING_KIND, prev, &ng, t);
        if (!prev) return FALSE;
        y += g->fh + 6 + pad;
    }

    ng.ng_TopEdge = y; ng.ng_Height = btn_h; ng.ng_Width = btn_w;
    ng.ng_LeftEdge = x0;
    prev = button(g, prev, &ng, MSG_BTN_IMAGE, GID_IMAGE, FALSE);   if (!prev) return FALSE;
    ng.ng_LeftEdge += btn_w + pad;
    prev = button(g, prev, &ng, MSG_BTN_RESCAN, GID_RESCAN, FALSE); if (!prev) return FALSE;
    ng.ng_LeftEdge += btn_w + pad;
    prev = button(g, prev, &ng, MSG_BTN_ABOUT, GID_ABOUT, FALSE);   if (!prev) return FALSE;
    y += btn_h + pad;

    g->status_y = g->win_h - bor_b - pad - g->fh;
    res_h = g->status_y - pad - y;
    if (res_h < 3 * (g->fh + 1) + 4) res_h = 3 * (g->fh + 1) + 4;
    ng.ng_LeftEdge = x0; ng.ng_TopEdge = y;
    ng.ng_Width = inner_w - pad * 2; ng.ng_Height = res_h;
    ng.ng_GadgetText = NULL; ng.ng_GadgetID = GID_RESULTS; ng.ng_Flags = 0;
    {
        struct TagItem t[] = { { GTLV_Labels, (IPTR)&g->lines }, { GTLV_ReadOnly, TRUE }, { TAG_DONE, 0 } };
        prev = g->res_gad = CreateGadgetA(LISTVIEW_KIND, prev, &ng, t);
        if (!prev) return FALSE;
    }
    g->lines_attached = TRUE;
    g->res_cols = (UWORD)((ng.ng_Width - 28) / cw);

    {
        struct TagItem wt[] = {
            { WA_Left,      (ULONG)((g->scr->Width  - g->win_w) / 2) },
            { WA_Top,       (ULONG)((g->scr->Height - g->win_h) / 2) },
            { WA_Width,     g->win_w },
            { WA_Height,    g->win_h },
            { WA_Title,     (IPTR)DISKREPAIR_VERTITLE },
            { WA_Gadgets,   (IPTR)g->glist },
            { WA_PubScreen, (IPTR)g->scr },
            { WA_IDCMP,     IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW |
                            IDCMP_GADGETDOWN | IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS |
                            IDCMP_INTUITICKS },   /* = LISTVIEWIDCMP | BUTTONIDCMP | STRINGIDCMP | CHECKBOXIDCMP */
            { WA_Flags,     WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                            WFLG_ACTIVATE | WFLG_SMART_REFRESH },
            { TAG_DONE, 0 }
        };
        g->win = OpenWindowTagList(NULL, wt);
    }
    if (!g->win) return FALSE;
    GT_RefreshWindow(g->win, NULL);
    return TRUE;
}

int gui_run(void)
{
    struct Gui *g = (struct Gui *)AllocVec(sizeof(*g), MEMF_PUBLIC | MEMF_CLEAR);
    int rc = 0;

    if (!g) return 20;
    g->vols = (struct VolumeList *)AllocVec(sizeof(*g->vols), MEMF_PUBLIC | MEMF_CLEAR);
    if (!g->vols) { FreeVec(g); return 20; }
    init_list(&g->lines);
    strncpy(g->image, Gui_StartImage, sizeof(g->image) - 1);

    fill_labels(g);
    g->selected = g->vols->count ? 0 : -1;

    if (!build_window(g)) { rc = 20; goto done; }
    set_status(g, g->vols->count ? GS(MSG_GUI_SELECT) : GS(MSG_GUI_NO_PARTITIONS));

    while (!g->quit) {
        struct IntuiMessage *imsg;
        WaitPort(g->win->UserPort);
        while ((imsg = GT_GetIMsg(g->win->UserPort)) != NULL) {
            ULONG  iclass = imsg->Class;
            UWORD  code   = imsg->Code;
            struct Gadget *gad = (struct Gadget *)imsg->IAddress;
            GT_ReplyIMsg(imsg);
            handle_msg(g, iclass, code, gad);
            if (g->quit) break;
        }
    }

done:
    if (g->win) {
        struct Node *n, *next;
        lines_detach(g);
        for (n = g->lines.lh_Head; (next = n->ln_Succ) != NULL; n = next) FreeVec(n);
        CloseWindow(g->win);
    }
    if (g->glist) FreeGadgets(g->glist);
    if (g->vi)    FreeVisualInfo(g->vi);
    if (g->scr)   UnlockPubScreen(NULL, g->scr);
    FreeVec(g->vols);
    FreeVec(g);
    return rc;
}
