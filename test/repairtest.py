#!/usr/bin/env python3
"""DiskRepair test: damage FFS images in known ways, then check / dry run /
repair / undo / rescue each one with the host binary.

  python3 test/repairtest.py [ffs|ofs|DOS3|all] [--fuzz N] [--keep] [-v]

Needs xdftool (amitools) to build the pristine image.  Verification uses
test/ffsimg.py, an independent reader, not the C code under test."""
import os, sys, shutil, subprocess, hashlib, random, struct

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ffsimg import Img, BS, NL

BIN = os.path.join(HERE, '..', 'host', 'diskrepair')
WORK = os.path.join(HERE, 'work')
VERBOSE = '-v' in sys.argv

def run(*args):
    p = subprocess.run([BIN] + list(args), capture_output=True, text=True, errors='replace')
    if p.returncode not in (0, 5, 10, 20):
        raise AssertionError('crash or odd exit %d: %s\n%s%s' % (p.returncode, ' '.join(args), p.stdout[-2000:], p.stderr[-2000:]))
    return p.returncode, p.stdout

def md5(path):
    return hashlib.md5(open(path, 'rb').read()).hexdigest()

def build_pristine(kind):
    """-> (image path, {amiga path: bytes})"""
    img = os.path.join(WORK, 'pristine_%s.hdf' % kind)
    rnd = random.Random(1234)
    files = {}
    for i in range(1, 9):
        files['f%d.bin' % i] = rnd.randbytes(i * 37000)
    for i in range(150):
        files['c%03d' % i] = rnd.randbytes(rnd.randint(1, 3000))
    files['sub/small'] = rnd.randbytes(300)
    files['sub/deep/big'] = rnd.randbytes(150000)
    files['sub/deep/hello.txt'] = b'hello\n'
    files['sub/empty'] = b''
    for i in range(20):
        files['sub/deep/d%02d' % i] = rnd.randbytes(rnd.randint(1, 9000))
    if os.path.exists(img): os.remove(img)
    src = os.path.join(WORK, 'src')
    shutil.rmtree(src, ignore_errors=True)
    cmd = ['xdftool', img, 'create', 'chs=60,16,63', '+', 'format', 'TestVol', kind,
           '+', 'makedir', 'sub', '+', 'makedir', 'sub/deep']
    for n, (name, data) in enumerate(files.items()):
        p = os.path.join(src, name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(data)
        cmd += ['+', 'write', p, name]
    subprocess.run(cmd, check=True, capture_output=True)
    return img, files

# ---------------------------------------------------------------- scenarios
# each: fn(img, rnd) -> dict of expectations
#   repair   'ok' | 'refuse'
#   lost     basenames that may be gone after the repair
#   partial  basenames whose rescued copy may differ / be missing
#   check    minimum check return code expected before the repair

def junk(rnd):
    return bytes(rnd.getrandbits(8) for _ in range(BS))

def s_clean(im, rnd):
    return dict(repair='ok', check=0)

def s_bmflag(im, rnd):
    im.setl(im.root, -50, 0)
    return dict(repair='ok', check=5)

def s_bmgarbage(im, rnd):
    p = im.bitmap_pages()[(im.root - 2) // ((NL - 1) * 32)]      # the page the files live in
    im.put(p, junk(rnd)); im.fixsum(p, at=0)
    return dict(repair='ok', check=10)

def s_bmchecksum(im, rnd):
    p = im.bitmap_pages()[-1]
    im.d[p * BS + 40] ^= 0x55
    return dict(repair='ok', check=10)

def s_kill_file_header(im, rnd):
    k = im.find('f3.bin')
    im.put(k, junk(rnd))
    return dict(repair='ok', check=10, lost={'f3.bin'}, partial={'f3.bin'})

def s_bad_chain_ptr(im, rnd):
    for k, slot, prev in im.entries(im.root):
        if im.l(k, -4):
            im.setl(k, -4, 0x00FFFFFF)
            return dict(repair='ok', check=10)
    raise AssertionError('no chained entry in the root')

def s_chain_into_data(im, rnd):
    # a chain pointer that lands in the middle of a file's data
    victim = im.file_blocks(im.find('f6.bin'))[5]
    for k, slot, prev in im.entries(im.root):
        if im.l(k, -4):
            im.setl(k, -4, victim)
            return dict(repair='ok', check=10)
    raise AssertionError('no chained entry')

def s_kill_dir(im, rnd):
    im.put(im.find('sub'), junk(rnd))
    return dict(repair='ok', check=10)

def s_kill_root(im, rnd):
    im.put(im.root, junk(rnd))
    return dict(repair='refuse', check=20)

def s_bad_dataptr(im, rnd):
    k = im.find('f5.bin')
    im.setl(k, NL - 51 - 3, 0x7FFFFFFF)
    return dict(repair='ok', check=10, lost={'f5.bin'}, partial={'f5.bin'})

def s_ext_break(im, rnd):
    k = im.find('f8.bin')
    e = im.l(k, -2)
    assert e
    im.put(e, junk(rnd))
    return dict(repair='ok', check=10, lost={'f8.bin'}, partial={'f8.bin'})

def s_wrong_parent(im, rnd):
    im.setl(im.find('sub/small'), -3, im.root)
    return dict(repair='ok', check=5)

def s_wrong_hash(im, rnd):
    k, d, slot, prev = im.ref('f2.bin')
    nxt = im.l(k, -4)
    if prev: im.setl(prev, -4, nxt)
    else:    im.setl(d, 6 + slot, nxt)
    free = [s for s in range(NL - 56) if im.l(d, 6 + s) == 0 and s != slot][0]
    im.setl(k, -4, 0)
    im.setl(d, 6 + free, k)
    return dict(repair='ok', check=10)

def s_crosslink(im, rnd):
    a, b = im.find('f1.bin'), im.find('f2.bin')
    im.setl(a, NL - 51, im.l(b, NL - 51))
    return dict(repair='ok', check=10, partial={'f1.bin'}, crosslink=True, mismatch={'f1.bin'})

def s_deleted(im, rnd):
    # what FFS does on delete: unlink the entry and free the blocks
    k, d, slot, prev = im.ref('f4.bin')
    blocks = [k] + im.file_blocks(k)
    e = im.l(k, -2)
    while e: blocks.append(e); e = im.l(e, -2)
    nxt = im.l(k, -4)
    if prev: im.setl(prev, -4, nxt)
    else:    im.setl(d, 6 + slot, nxt)
    pages = im.bitmap_pages()
    for b in blocks:
        i = b - 2
        pg = pages[i // ((NL - 1) * 32)]; r = i % ((NL - 1) * 32)
        im.setl(pg, 1 + r // 32, im.l(pg, 1 + r // 32) | (1 << (r % 32)), fix=False)
    for pg in pages: im.fixsum(pg, at=0)
    return dict(repair='ok', check=0, lost={'f4.bin'}, deleted={'f4.bin'})

SCENARIOS = [s_clean, s_bmflag, s_bmgarbage, s_bmchecksum, s_kill_file_header, s_bad_chain_ptr,
             s_chain_into_data, s_kill_dir, s_kill_root, s_bad_dataptr, s_ext_break,
             s_wrong_parent, s_wrong_hash, s_crosslink, s_deleted]

def make_fuzz(seed):
    def s_fuzz(im, rnd):
        r = random.Random(seed)
        tree = Img(im.path).tree()
        meta = [im.root] + im.bitmap_pages() + [k for k, _ in tree.values()]
        for _ in range(r.randint(1, 6)):
            mode = r.randint(0, 3)
            b = r.choice(meta) if mode < 3 else r.randrange(2, im.n)
            if mode == 0:
                im.put(b, junk(r))
            elif mode == 1:                      # one long changed, checksum kept valid
                im.setl(b, r.randrange(NL), r.getrandbits(32))
            else:                                # a few bytes flipped, checksum now wrong
                for _ in range(r.randint(1, 4)): im.d[b * BS + r.randrange(BS)] ^= 1 << r.randrange(8)
        return dict(repair='any', check=0, fuzz=True)
    s_fuzz.__name__ = 's_fuzz%d' % seed
    return s_fuzz

# ---------------------------------------------------------------- one run

def rescued_files(out):
    """{basename: [paths]} under the rescue directory"""
    found = {}
    for root, _, names in os.walk(out):
        for n in names:
            if n == 'DiskRepair-attributes.txt': continue
            found.setdefault(n, []).append(os.path.join(root, n))
    return found

def one(kind, pristine, files, scen):
    name = scen.__name__
    base = os.path.join(WORK, '%s_%s' % (kind, name))
    img = base + '.hdf'
    shutil.copy(pristine, img)
    im = Img(img)
    exp = scen(im, random.Random(name))
    im.save()
    damaged = md5(img)
    bybase = {os.path.basename(k): v for k, v in files.items()}
    notes = []

    # --- check
    rc, out = run('IMAGE=' + img, 'DEEP', 'LOST')
    if VERBOSE: print(out)
    assert rc >= exp['check'], 'check rc %d < %d\n%s' % (rc, exp['check'], out)
    if exp['check'] == 0 and not exp.get('fuzz'): assert rc == 0, 'clean image reported rc %d\n%s' % (rc, out)
    assert md5(img) == damaged, 'CHECK changed the image'

    # --- rescue from the damaged image
    outdir = base + '.rescued'
    shutil.rmtree(outdir, ignore_errors=True)
    rrc, rout = run('IMAGE=' + img, 'RESCUE=' + outdir, 'DELETED')
    if VERBOSE: print(rout)
    assert md5(img) == damaged, 'RESCUE changed the image'
    found = rescued_files(outdir)
    partial_ok = exp.get('partial', set())
    partial_said = set()
    for line in rout.splitlines():
        if 'incomplete' in line: partial_said.add(os.path.basename(line.split(': ')[1] if line.startswith('Warning') else line.split(':')[0]))
    missing = bad = 0
    for b, data in bybase.items():
        if b not in found:
            if b in partial_ok or exp.get('fuzz'): missing += 1; continue
            raise AssertionError('rescue lost %s\n%s' % (b, rout))
        good = any(open(p, 'rb').read() == data for p in found[b])
        if not good:
            flagged = any(b in s or (b + '_') in s for s in partial_said)
            if b in exp.get('mismatch', set()): bad += 1; continue
            if flagged and (b in partial_ok or exp.get('fuzz')): bad += 1; continue
            # fuzz: a pointer changed with a valid checksum cannot be noticed by anyone
            if exp.get('fuzz'): bad += 1; notes.append('rescue: %s differs silently' % b); continue
            raise AssertionError('rescued %s differs and was not reported as incomplete\n%s' % (b, rout))
    for b in exp.get('deleted', set()):
        assert any('/Deleted/' in p for p in found.get(b, [])), 'deleted %s not rescued' % b

    # --- dry run changes nothing
    drc, dout = run('IMAGE=' + img, 'DRYRUN')
    assert md5(img) == damaged, 'DRYRUN changed the image'

    # --- repair
    undo = base + '.undo'
    if os.path.exists(undo): os.remove(undo)
    prc, pout = run('IMAGE=' + img, 'REPAIR', 'UNDOFILE=' + undo, 'FORCE')
    if VERBOSE: print(pout)
    if exp['repair'] == 'refuse' or (exp['repair'] == 'any' and prc == 20):
        assert prc == 20, 'repair should have refused (rc %d)\n%s' % (prc, pout)
        assert md5(img) == damaged, 'refused repair changed the image\n' + pout
        return 'refused' + (' rescue:%d missing %d differ' % (missing, bad) if missing or bad else '')
    assert prc in (0, 5), 'repair failed rc %d\n%s' % (prc, pout)
    assert (drc == 0) == (md5(img) == damaged), 'dry run and repair disagree\n%s\n%s' % (dout, pout)

    after = Img(img)
    probs = after.validate()
    allowed = ('shared by', 'free but used') if (exp.get('crosslink') or exp.get('fuzz')) else ('free but used',) if exp.get('lost') else ()
    hard = [p for p in probs if not any(a in p for a in allowed)]
    assert not hard, 'independent validation after repair: %s\n%s' % (hard[:5], pout)
    crc, cout = run('IMAGE=' + img, 'LOST')
    okrc = 10 if (exp.get('crosslink') or exp.get('fuzz')) else 5
    assert crc <= okrc, 'check after repair rc %d\n%s\n--- repair said:\n%s' % (crc, cout, pout)
    if not exp.get('fuzz') and not exp.get('crosslink'):
        assert 'Error' not in cout, 'errors after repair\n%s' % cout
    # a second repair has nothing left to do
    r2, o2 = run('IMAGE=' + img, 'DRYRUN')
    assert r2 == 0, 'second repair still wants changes\n%s' % o2

    tree = after.tree()
    present = {}
    for p, (k, st) in tree.items():
        if st == -3: present.setdefault(os.path.basename(p), []).append(k)
    lost_ok = exp.get('lost', set())
    for b, data in bybase.items():
        if b not in present:
            if b in lost_ok or exp.get('fuzz'): continue
            raise AssertionError('repair lost %s\n%s' % (b, pout))
        try: good = any(after.read_file(k) == data for k in present[b])
        except ValueError: good = False
        if not good and not (b in exp.get('mismatch', set()) or exp.get('fuzz')):
            raise AssertionError('%s differs after repair\n%s' % (b, pout))

    # --- undo brings the damaged image back bit for bit
    if md5(img) != damaged:
        urc, uout = run('IMAGE=' + img, 'UNDO', 'UNDOFILE=' + undo, 'FORCE')
        assert urc == 0 and md5(img) == damaged, 'UNDO did not restore the image\n' + uout
    return 'repaired' + (' rescue:%d missing %d differ' % (missing, bad) if missing or bad else '') + \
           (' [' + '; '.join(notes[:2]) + ']' if notes else '')

def main():
    kinds = ['ffs', 'ofs', 'DOS3'] if 'all' in sys.argv else [a for a in sys.argv[1:] if a in ('ffs', 'ofs', 'DOS3')] or ['ffs']
    nfuzz = int(sys.argv[sys.argv.index('--fuzz') + 1]) if '--fuzz' in sys.argv else 0
    only = [a[5:] for a in sys.argv if a.startswith('only=')]
    os.makedirs(WORK, exist_ok=True)
    fails = 0
    for kind in kinds:
        pristine, files = build_pristine(kind)
        scens = SCENARIOS + [make_fuzz(i) for i in range(nfuzz)]
        for scen in scens:
            if only and scen.__name__ not in only: continue
            try:
                res = one(kind, pristine, files, scen)
                print('%-4s %-22s OK   %s' % (kind, scen.__name__, res))
                if '--keep' not in sys.argv:
                    base = os.path.join(WORK, '%s_%s' % (kind, scen.__name__))
                    for ext in ('.hdf', '.undo'):
                        if os.path.exists(base + ext): os.remove(base + ext)
                    shutil.rmtree(base + '.rescued', ignore_errors=True)
            except AssertionError as e:
                fails += 1
                print('%-4s %-22s FAIL %s' % (kind, scen.__name__, str(e)[:3000]))
    print('%d failure(s)' % fails)
    sys.exit(1 if fails else 0)

if __name__ == '__main__':
    main()
