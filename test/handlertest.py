#!/usr/bin/env python3
"""Does the REAL FastFileSystem accept what DiskRepair wrote?

For each damage scenario of repairtest.py that a repair fixes: put the
damaged partition into an RDB image, repair it in place, then let the
real handler (through amifuse) list it, write a new file into it and read
files back - and check the volume once more afterwards.  A bitmap that
is wrong, or a volume the validator refuses, shows up as a failed write
or as damage in that last check.

  python3 test/handlertest.py [ffs|ofs] [only=<scenario> ...]

Needs: amifuse, a FastFileSystem binary (FFS_HANDLER= or ~/UAE/*/L),
AmiPart's host CLI (AMIPART= or ../AmiPart/host/amipart), xdftool."""
import os, sys, glob, json, shutil, subprocess, hashlib, random

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import repairtest as rt
from ffsimg import Img

AMIPART = os.environ.get('AMIPART', os.path.join(HERE, '..', '..', 'AmiPart', 'host', 'amipart'))
FFS = os.environ.get('FFS_HANDLER') or (sorted(glob.glob(os.path.expanduser('~/UAE/*/L/FastFileSystem'))) or [''])[0]
CYL = 1008 * 512

def sh(cmd, check=True, timeout=600):
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout)
    if check and p.returncode: raise AssertionError('%s\n%s%s' % (' '.join(cmd), p.stdout[-1500:], p.stderr[-1500:]))
    return p

def ami(*args):
    return sh(['amifuse'] + list(args) + ['--partition', 'DH0', '--driver', FFS])

def main():
    kind = 'ofs' if 'ofs' in sys.argv else 'ffs'
    only = [a[5:] for a in sys.argv if a.startswith('only=')]
    for need, what in ((shutil.which('amifuse'), 'amifuse'), (os.path.exists(FFS), 'FastFileSystem'),
                       (os.path.exists(AMIPART), 'AmiPart host CLI')):
        if not need: sys.exit('missing: ' + what)
    os.makedirs(rt.WORK, exist_ok=True)
    pristine, files = rt.build_pristine(kind)
    fails = 0
    for scen in rt.SCENARIOS:
        name = scen.__name__
        if only and name not in only: continue
        try:
            part = os.path.join(rt.WORK, 'h_part.hdf')
            shutil.copy(pristine, part)
            im = Img(part)
            exp = scen(im, random.Random(name))
            im.save()
            if exp['repair'] != 'ok': continue
            rdb = os.path.join(rt.WORK, 'h_%s_%s.hdf' % (kind, name))
            with open(rdb, 'wb') as f: f.truncate(64 * CYL)
            script = os.path.join(rt.WORK, 'h_part.script')
            open(script, 'w').write('OPEN FILE %s\nINIT NEW\nADDPART NAME=DH0 LOW=2 HIGH=61 TYPE=%s VOLNAME=TestVol\nWRITE\nCLOSE\n'
                                    % (rdb, 'DOS1' if kind == 'ffs' else 'DOS0'))
            sh([AMIPART, 'SCRIPT', script, 'FORCE'])
            data = open(part, 'rb').read()
            with open(rdb, 'r+b') as f: f.seek(2 * CYL); f.write(data)

            undo = rdb + '.undo'
            rc, out = rt.run('IMAGE=' + rdb, 'PART=DH0', 'REPAIR', 'UNDOFILE=' + undo, 'FORCE')
            assert rc in (0, 5), 'repair rc %d\n%s' % (rc, out)

            # the real handler: list, write, read back
            ls = ami('ls', rdb, '--recursive', '--json')
            newdata = random.Random(name + 'new').randbytes(300000)
            newfile = os.path.join(rt.WORK, 'h_new.bin')
            open(newfile, 'wb').write(newdata)
            ami('write', rdb, '--file', 'written_by_ffs', '--in', newfile)
            got = ami('hash', rdb, '--file', 'written_by_ffs', '--algorithm', 'md5', '--json')
            assert hashlib.md5(newdata).hexdigest() in got.stdout, 'new file reads back wrong\n' + got.stdout
            lost = exp.get('lost', set()) | exp.get('mismatch', set())
            probe = [p for p in ('f7.bin', 'f2.bin', 'sub/deep/big', 'c042') if os.path.basename(p) not in lost]
            for p in probe:
                r = sh(['amifuse', 'hash', rdb, '--file', p, '--algorithm', 'md5', '--json', '--partition', 'DH0', '--driver', FFS], check=False)
                if hashlib.md5(files[p]).hexdigest() not in r.stdout:
                    # after a lost directory the file lives in Lost+Found
                    alt = 'Lost+Found/' + p.split('/', 1)[1] if '/' in p else p
                    r = ami('hash', rdb, '--file', alt, '--algorithm', 'md5', '--json')
                    assert hashlib.md5(files[p]).hexdigest() in r.stdout, '%s reads back wrong through FFS\n%s' % (p, r.stdout)
            # and the volume is still sound after the handler worked on it
            crc, cout = rt.run('IMAGE=' + rdb, 'PART=DH0', 'LOST')
            okrc = 10 if exp.get('crosslink') else 5
            assert crc <= okrc, 'check after the handler wrote: rc %d\n%s' % (crc, cout)
            if not exp.get('crosslink'): assert 'Error' not in cout, cout
            print('%-4s %-22s OK   real FFS listed, wrote 300 KB, read %d files back; check rc %d' % (kind, name, len(probe) + 1, crc))
            for f in (rdb, undo):
                if os.path.exists(f) and '--keep' not in sys.argv: os.remove(f)
        except (AssertionError, subprocess.TimeoutExpired) as e:
            fails += 1
            print('%-4s %-22s FAIL %s' % (kind, name, str(e)[:2500]))
    print('%d failure(s)' % fails)
    sys.exit(1 if fails else 0)

if __name__ == '__main__':
    main()
