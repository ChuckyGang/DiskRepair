# Host (Linux) DiskRepair: the CLI (plain gcc, no deps) and the window
# (needs libsdl2-dev).  Both run the same core on image files and raw disks.
CORE = carr volume fsmap pfs_map pfs_core sfs_map blockdev clib build report ffs ffs_scan ffs_repair ffs_rescue resc_common pfs_rescue sfs_rescue undo fscheck
CORESRC = $(addprefix ../src/,$(addsuffix .c,$(CORE)))
SRCS = ../src/cli.c $(CORESRC) amiga_shim.c host_rdargs.c host_locale.c host_platform.c host_main.c
STRINGS_H = ../src/diskrepair_strings.h
CFLAGS = -O2 -Wall -Wno-pointer-sign -Wno-format-security -Wno-stringop-truncation \
         -DDISKREPAIR_HOST=1 -I . -I inc -I ../src
all: diskrepair

$(STRINGS_H): ../catalogs/DiskRepair.cd ../support/gencat.py
	python3 ../support/gencat.py header ../catalogs/DiskRepair.cd $(STRINGS_H)

diskrepair: $(SRCS) amiga_compat.h $(STRINGS_H)
	gcc $(CFLAGS) -o diskrepair $(SRCS)

# Linux window: the Amiga GUI source (../src/gui.c) on the Intuition /
# GadTools layer over SDL2 in gui/ (shared with AmiPart).  Needs libsdl2-dev.
# With arguments it is the same CLI as diskrepair; without, it opens the window.
GSRCS  = ../src/cli.c ../src/gui.c $(CORESRC) amiga_shim.c host_rdargs.c host_locale.c host_platform.c \
         host_main.c gui/host_gui.c gui/host_gadtools.c gui/host_req.c gui/host_guimain.c
GHDRS  = $(wildcard gui/*.h) $(wildcard ../src/*.h) $(STRINGS_H)
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null)
GOBJ   = gobj
GOBJS  = $(addprefix $(GOBJ)/,$(notdir $(GSRCS:.c=.o)))
GFLAGS = -O2 -Wall -Wno-pointer-sign -Wno-format -Wno-format-security \
         -Wno-stringop-truncation -Wno-unused-function \
         -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast \
         -DDISKREPAIR_HOST=1 -DDISKREPAIR_HOST_GUI=1 -I gui -I gui/inc -I ../src $(SDL_CFLAGS)
vpath %.c ../src gui .

gui: diskrepair-gui
diskrepair-gui: $(GOBJS)
	gcc -o $@ $(GOBJS) $(SDL_LIBS)
$(GOBJ)/%.o: %.c $(GHDRS) | $(GOBJ)
	@echo "  CC $<"; gcc $(GFLAGS) -c -o $@ $<
$(GOBJ):
	mkdir -p $(GOBJ)

clean:
	rm -f diskrepair diskrepair-gui runidx_test
	rm -rf $(GOBJ)

.PHONY: all gui clean
