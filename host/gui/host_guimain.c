/* host_guimain.c - entry point of the Linux window build.
 *
 * With arguments: the command line, exactly as host/diskrepair.
 * With one argument that is a readable file: the window, on that image.
 * Without: the Amiga window (src/gui.c) on the SDL2 layer. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "amiga_compat.h"
#include "host_gui.h"
#include "cli.h"
#include "gui.h"
#include "locale_support.h"

extern int    host_argc;
extern char **host_argv;
void host_usage(void);

/* library bases the shared GUI source refers to; any non-NULL value does */
struct Library *AslBase = (struct Library *)1;

int main(int argc, char **argv)
{
    int rc;
    host_argc = argc;
    host_argv = argv;
    SetSignal(0, 0);
    if (argc == 2 && !strchr(argv[1], '=') && access(argv[1], R_OK) == 0) {
        strncpy(Gui_StartImage, argv[1], sizeof(Gui_StartImage) - 1);     /* diskrepair-gui <image> */
    } else if (argc > 1) {
        LONG r = cli_run();
        if (r == CLI_NO_ARGS) { host_usage(); return 5; }
        return (int)r;
    }
    if (!hostgui_init()) return 20;
    LocaleOpen();
    rc = gui_run();
    LocaleClose();
    hostgui_quit();
    return rc;
}
