#!/bin/sh
# make_adf.sh <DiskRepair binary> <output.adf>
# Bootable OFS floppy image (xdftool from amitools) that runs DiskRepair
# from the Shell: the way to repair the boot volume in place, since
# nothing on the hard disk is open then.
set -e
BIN="$1"; OUT="$2"; HERE="$(dirname "$0")"
[ -f "$BIN" ] || { echo "make_adf.sh: $BIN not found" >&2; exit 1; }
rm -f "$OUT"
xdftool "$OUT" create + format "DiskRepair" ofs + boot install
xdftool "$OUT" makedir S + write "$HERE/adf/Startup-Sequence" S/Startup-Sequence \
             + write "$BIN" DiskRepair + write "$HERE/../README.md" README.md
xdftool "$OUT" list
