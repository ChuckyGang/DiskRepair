#!/usr/bin/env python3
"""Undelete and unformat: delete files and directories (or quick-format the
volume) with xdftool, run UNDELETE, compare what comes out with the source.
Needs xdftool (amitools) and host/diskrepair."""
import filecmp, os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, '..', 'host', 'diskrepair')

def sh(*a):
    return subprocess.run(a, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

def same(a, b):
    c = filecmp.dircmp(a, b, ignore=['DiskRepair-attributes.txt'])
    if c.left_only or c.right_only or c.funny_files: return False
    if filecmp.cmpfiles(a, b, c.common_files, shallow=False)[1:] != ([], []): return False
    return all(same(os.path.join(a, d), os.path.join(b, d)) for d in c.common_dirs)

def main():
    fails = 0
    work = tempfile.mkdtemp(prefix='undel_')
    src = os.path.join(work, 'src')
    os.makedirs(os.path.join(src, 'Docs', 'Deep'))
    os.makedirs(os.path.join(src, 'Empty'))
    rnd = random.Random(1)
    for f, n in (('a.txt', 70000), ('small', 3), ('Docs/b.txt', 70000), ('Docs/Deep/c.bin', 200000)):
        with open(os.path.join(src, f), 'wb') as fh: fh.write(rnd.randbytes(n))
    for kind in ('DOS0', 'DOS1', 'DOS3'):
        for what in ('delete', 'format'):
            img = os.path.join(work, '%s-%s.hdf' % (kind, what))
            out = os.path.join(work, 'out-%s-%s' % (kind, what))
            cmd = ['xdftool', img, 'create', 'chs=60,16,63', '+', 'format', 'TV', kind]
            for n in ('a.txt', 'small', 'Docs', 'Empty'): cmd += ['+', 'write', os.path.join(src, n)]
            if what == 'delete':
                cmd += ['+', 'delete', 'Docs', 'all', '+', 'delete', 'small', '+', 'delete', 'Empty']
                want = os.path.join(work, 'want-' + kind)
                subprocess.run(['cp', '-r', src, want]); os.remove(os.path.join(want, 'a.txt'))
            else:
                want = src
            r = sh(*cmd)
            if r.returncode == 0 and what == 'format': r = sh('xdftool', img, 'format', 'New', kind)
            if r.returncode: print(r.stdout); sys.exit('xdftool failed')
            before = open(img, 'rb').read()
            r = sh(TOOL, 'IMAGE=' + img, 'UNDELETE=' + out)
            ok = os.path.isdir(out) and same(want, out) and open(img, 'rb').read() == before
            print('%s %-7s %s' % (kind, what, 'OK' if ok else 'FAIL\n' + r.stdout))
            fails += not ok
    print('%d failure(s)' % fails)
    sys.exit(1 if fails else 0)

main()
