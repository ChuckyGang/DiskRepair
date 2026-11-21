/*
 * gui.h - the DiskRepair window: volume list, actions, findings, status.
 */
#ifndef GUI_H
#define GUI_H

#include <exec/types.h>

/* Runs the main window until the user closes it.  Returns a DOS return
 * code.  Needs intuition, graphics and gadtools open (main.c). */
int gui_run(void);

/* Set before gui_run() to start with a disk image instead of the mounted
 * volumes (the Linux window: diskrepair-gui <image>). */
extern char Gui_StartImage[256];

#endif /* GUI_H */
