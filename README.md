# DiskRepair

Checks Amiga filesystems, repairs them in place, or copies what can still be
read to another place. For AmigaOS 2.04+ (Kickstart v37), with a Linux build
of the same code that works on disk images and raw disks.

**Experimental.** This is a test project. It has passed every test described
below, but it has not yet met a disk damaged by real life. If the data
matters: rescue first, repair second.

## What it does

| | OFS / FFS (DOS\0 - DOS\5) | PFS3 | SFS |
|---|---|---|---|
| Check (read-only) | yes | basic | basic |
| Rescue to another path | yes | not yet | not yet |
| Repair in place | yes (directories on DOS\4/5: not yet) | not yet | not yet |

Long-filename FFS (DOS\6, DOS\7) is refused for now.

**Check** walks everything reachable from the root, verifies every header,
hash chain, extension chain and the bitmap, and reports what is wrong.
`LOST` additionally reads the rest of the volume for file and directory
headers nothing points to; `DEEP` reads every data block.

**Rescue** never writes to the damaged volume. It copies the directory tree
to a path you give, then everything it found outside the tree into
`Lost+Found` (and, with `DELETED`, deleted files that are still on the disk
into `Deleted`). It works without a root block. A file with unreadable or
missing blocks is still copied and listed as incomplete.

**Undelete** copies only the deleted files that are still on the disk to a
path you give, in the directories they were in. Nothing is written to the
volume. The same function is the **unformat**: a quick format only writes a
new root block and bitmap, so everything the volume held before counts as
deleted and comes out with its directory tree. Files whose blocks have been
reused since are copied too, with a warning.

**Repair in place** follows one rule: *nothing is written unless the result
passes the check.* The whole repair is worked out on a copy of the affected
blocks in memory and checked again there. Only then are the original blocks
saved to an undo file (which must be on another volume) and the new ones
written. If the repair cannot reach a consistent state - unreadable blocks,
no root block, damaged bitmap pointers - the volume is left exactly as it
was and the answer is "rescue instead".

What a repair changes: hash chain and parent pointers, a `Lost+Found`
directory, the bitmap. It never overwrites file data and never erases a
header, so anything it has to take out of a directory (a file whose block
list is broken) is still there for a later rescue run. `UNDO` puts the saved
blocks back.

## Usage (Amiga)

Started from Workbench, or from a Shell without arguments, it opens a window.
With arguments it is a Shell command:

    DiskRepair LIST                               the partitions
    DiskRepair DH1:                               check DH1: (read-only)
    DiskRepair DH1: DEEP LOST                     ... read all data, search for lost files
    DiskRepair DH1: RESCUE=Work:Saved             copy everything readable to Work:Saved
    DiskRepair DH1: RESCUE=Work:Saved DELETED     ... including deleted files
    DiskRepair DH1: UNDELETE=Work:Back            copy only deleted files (also after a quick format)
    DiskRepair DH1: DRYRUN                        show what a repair would do
    DiskRepair DH1: REPAIR UNDOFILE=Work:dh1.undo repair in place (asks for YES)
    DiskRepair DH1: UNDO UNDOFILE=Work:dh1.undo   put the old blocks back
    DiskRepair IMAGE=disk.hdf PART=DH1 ...        the same on a disk image file

Return codes: 0 clean, 5 warnings, 10 damaged, 20 failed.

The boot volume can be checked and rescued from, but not repaired in place
while the system runs from it. `make adf` builds a bootable floppy image
with DiskRepair on it for that.

## Usage (Linux)

    host/diskrepair IMAGE=disk.hdf LIST
    host/diskrepair IMAGE=disk.hdf PART=DH1 LOST
    host/diskrepair IMAGE=disk.hdf PART=DH1 RESCUE=/tmp/saved DELETED
    host/diskrepair IMAGE=/dev/sdX PART=DH0 DRYRUN        (raw disk, needs root)
    host/diskrepair-gui                                   the window
    host/diskrepair-gui disk.hdf                          the window, on that image

Rescued file names are converted from Latin-1 to UTF-8. Protection bits and
file comments, which Linux has no place for, are listed in
`DiskRepair-attributes.txt` in the destination.

The Linux window is the Amiga window: the same `src/gui.c`, running on a
small Intuition/GadTools layer over SDL2 (`host/gui/`, shared with AmiPart).

## Building

    make                Amiga binary out/DiskRepair (Bartman or Bebbo toolchain, auto-detected)
    make linux          host/diskrepair        (plain gcc, no dependencies)
    make linux-gui      host/diskrepair-gui    (needs libsdl2-dev)
    make adf            bootable floppy image  (needs xdftool from amitools)

The Amiga program needs only what is in ROM (exec, dos, intuition, graphics,
gadtools) plus, optionally, locale.library for translations and asl.library
for the "Image..." file requester.

## Translations

All texts are in `catalogs/DiskRepair.cd`; see `catalogs/README.txt`.

## Tests

    python3 test/repairtest.py all --fuzz 150     damage scenarios + random damage, FFS/OFS/DOS\3
    python3 test/handlertest.py                   repaired images through the real FastFileSystem
    python3 test/guitest.py                       the window does what the CLI does (headless)

`repairtest.py` damages a pristine image in known ways (bitmap, hash chains,
headers, directories, the root block, data pointers, extension blocks,
cross-links, deleted files) and with random garbage, then runs check, rescue,
dry run, repair and undo on each. It verifies the result with
`test/ffsimg.py`, an independent reader written for the purpose - not with
the code under test. Every repair must either validate clean or leave the
image untouched, and undo must bring the damaged image back bit for bit.

`handlertest.py` asks the real judge: after a repair, the original
FastFileSystem (through amifuse) has to list the volume, write a new file to
it and read files back, and the volume has to be clean afterwards.

Not tested yet: real hardware, the Amiga window, a mounted volume (inhibit,
undo-file and destination checks against the volume's handler), block sizes
other than 512 bytes, and above all disks damaged by something other than
these tests. Images of really broken disks are very welcome.

## Layout

    src/ffs.c           OFS/FFS access that trusts nothing; block overlay for repairs
    src/ffs_scan.c      the consistency scan check, repair and rescue start from
    src/ffs_repair.c    in-place repair on the overlay, bitmap rebuild, commit
    src/ffs_rescue.c    copy to another path
    src/undo.c          the undo file
    src/fscheck.c       dispatch per filesystem (what front ends call)
    src/pfs_*.c, sfs_map.c, fsmap.c    PFS3 / SFS structure walk (basic check)
    src/cli.c, gui.c    the two front ends
    src/platform_amiga.c, host/host_platform.c   inhibit, rescue destination
    host/               Linux build: AmigaOS shim, ReadArgs, SDL2 GUI layer
    test/               the harnesses above

## Licence

MIT, see LICENSE.
