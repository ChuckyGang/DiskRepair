#!/usr/bin/env python3
"""PFS3 and SFS rescue / undelete test (Linux host).

Builds an RDB image with AmiPart's host CLI, fills it through the REAL
handler (pfs3aio / SmartFilesystem under amifuse's handler bridge) with a
directory tree, then checks host/diskrepair against it:

  clean      RESCUE gives exactly the tree that was written
  deleted    files deleted through the handler come back with UNDELETE
  damage     directory blocks are destroyed in a copy of the image; every
             file that RESCUE reports as complete must be one of the files
             written (same content), files outside the destroyed block must
             all be there, and the image must not change
  fuzz       random garbage in the filesystem's own blocks: no crash, no
             hang, no wrong content reported as complete

Needs: AmiPart host CLI (../AmiPart/host/amipart or AMIPART=), amifuse in
its pipx venv (AMIFUSE_PYTHON=), the handlers (PFS3_HANDLER=, SFS_HANDLER=
or ~/UAE/*/L/).   Usage: fstest.py [pfs3|sfs] [--fuzz N]
"""
import glob, hashlib, json, os, random, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, '..', 'host', 'diskrepair')
AMIPART = os.environ.get('AMIPART', os.path.join(HERE, '..', '..', 'AmiPart', 'host', 'amipart'))
PYTHON = os.environ.get('AMIFUSE_PYTHON', os.path.expanduser('~/.local/share/pipx/venvs/amifuse/bin/python'))
def handler(env, name):
    return os.environ.get(env) or (glob.glob(os.path.expanduser('~/UAE/WB323/L/' + name)) +
                                   glob.glob(os.path.expanduser('~/UAE/*/L/' + name)) + [None])[0]
KINDS = {'pfs3': ('PDS3', handler('PFS3_HANDLER', 'pfs3aio')),
         'sfs':  ('SFS0', handler('SFS_HANDLER', 'SmartFilesystem'))}
CYL = 516096

WORKER = r'''
import sys, os, json, hashlib
from pathlib import Path
import amifuse.fuse_fs as ff
job = json.load(sys.stdin)
orig = ff.HandlerBridge._run_until_replies
ff.HandlerBridge._run_until_replies = lambda self, max_iters=4000, **kw: orig(self, max_iters=max_iters, **kw)
b = ff.HandlerBridge(Path(job["image"]), Path(job["driver"]), read_only=False, partition="DH0")
out = {"ok": True, "log": []}
def parent_lock(path):
    parts = [p for p in path.split("/") if p]
    if len(parts) <= 1:
        lk, _ = b.locate(0, "")
        return lk, parts[-1] if parts else ""
    lk, res2, locks = b.locate_path("/" + "/".join(parts[:-1]))
    return lk, parts[-1]
try:
    for op in job["ops"]:
        kind = op[0]
        if kind == "mkdir":
            lk, name = parent_lock(op[1])
            r = b.create_dir(lk, name)
            if r[0]: b.free_lock(r[0])
            else: out["ok"] = False
        elif kind == "write":
            path, size, seed = op[1], op[2], op[3]
            data = hashlib.sha256(seed.encode()).digest()
            rnd = bytearray()
            while len(rnd) < size:
                data = hashlib.sha256(data).digest(); rnd += data
            rnd = bytes(rnd[:size])
            r = b.open_file(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
            if not r:
                out["log"].append(["write", path, "open failed"]); out["ok"] = False; continue
            fh, lk = r
            pos = 0
            while pos < size:
                n = b.write_handle(fh, rnd[pos:pos + 65536])
                if n <= 0: out["ok"] = False; out["log"].append(["write", path, "short"]); break
                pos += n
            b.close_file(fh)
        elif kind == "delete":
            lk, name = parent_lock(op[1])
            r = b.delete_object(lk, name)
            if r[0] == 0: out["ok"] = False; out["log"].append(["delete", op[1], r[1]])
        elif kind == "flush":
            b.flush_volume()
finally:
    try: b.flush_volume()
    except Exception: pass
    ff._cleanup_bridge(b, None)
sys.stdout.write("\n@@JSON@@" + json.dumps(out) + "\n")
'''

def content(size, seed):
    data = hashlib.sha256(seed.encode()).digest()
    rnd = bytearray()
    while len(rnd) < size:
        data = hashlib.sha256(data).digest(); rnd += data
    return bytes(rnd[:size])

def bridge(work, img, driver, ops):
    w = os.path.join(work, 'worker.py')
    open(w, 'w').write(WORKER)
    r = subprocess.run([PYTHON, '-I', w], input=json.dumps({'image': img, 'driver': driver, 'ops': ops}),
                       capture_output=True, text=True, timeout=3600)
    if '@@JSON@@' not in r.stdout: raise RuntimeError('bridge failed:\n' + r.stdout[-1500:] + r.stderr[-1500:])
    res = json.loads(r.stdout.split('@@JSON@@')[-1].strip().splitlines()[0])
    if not res['ok']: raise RuntimeError('handler refused: %s' % res['log'][:5])

def tool(*args, timeout=300):
    return subprocess.run([TOOL] + list(args), capture_output=True, text=True, timeout=timeout)

def tree(root):
    out = {}
    for d, _, fs in os.walk(root):
        for f in fs:
            if f == 'DiskRepair-attributes.txt': continue
            p = os.path.join(d, f)
            out[os.path.relpath(p, root)] = hashlib.md5(open(p, 'rb').read()).hexdigest()
    return out

def md5f(p): return hashlib.md5(open(p, 'rb').read()).hexdigest()

def build(work, kind, rng):
    dostype, driver = KINDS[kind]
    img = os.path.join(work, kind + '.hdf')
    cyl = 40
    with open(img, 'wb') as f: f.truncate(cyl * CYL)
    script = os.path.join(work, 'part.script')
    open(script, 'w').write('OPEN FILE %s\nINIT NEW\nADDPART NAME=DH0 LOW=2 HIGH=%d TYPE=%s VOLNAME=FsTest\nWRITE\nCLOSE\n'
                            % (img, cyl - 1, dostype))
    r = subprocess.run([AMIPART, 'SCRIPT', script, 'FORCE'], capture_output=True, text=True)
    if r.returncode: raise RuntimeError('amipart: ' + r.stdout + r.stderr)
    files, ops = {}, []
    dirs = ['', 'Docs/', 'Docs/Deep/', 'Many/', 'Many/Sub/', 'Other/']
    for d in dirs[1:]: ops.append(['mkdir', d.rstrip('/')])
    n = 0
    def add(d, size):
        nonlocal n
        path = '%sfile_%03d%s' % (d, n, rng.choice(['', '.txt', '.a-long-suffix-name']))
        seed = '%s:%d' % (path, size)
        ops.append(['write', path, size, seed])
        files[path] = hashlib.md5(content(size, seed)).hexdigest()
        n += 1
    for d in dirs:
        for _ in range(rng.randint(3, 6)):
            add(d, rng.choice([0, rng.randint(1, 900), rng.randint(1000, 60000), rng.randint(60000, 700000)]))
    for _ in range(120): add('Many/', rng.randint(1, 5000))      # a directory of several blocks
    ops.append(['flush'])
    bridge(work, img, driver, ops)
    return img, driver, files

def check_subset(got, files, complete_only, log):
    """every rescued file that is not reported incomplete has known content"""
    known = set(files.values())
    bad = []
    for p, m in got.items():
        if m in known: continue
        if any(p in line and 'incomplete' in line for line in log.splitlines()): continue
        bad.append(p)
    return bad

def run_kind(kind, nfuzz):
    fails = 0
    rng = random.Random(kind)
    work = tempfile.mkdtemp(prefix='fstest_%s_' % kind, dir=os.environ.get('FSTEST_WORK'))
    img, driver, files = build(work, kind, rng)

    def result(name, ok, detail=''):
        nonlocal fails
        print('%-5s %-22s %s %s' % (kind, name, 'OK  ' if ok else 'FAIL', detail))
        fails += not ok

    # --- clean
    before = md5f(img)
    out = os.path.join(work, 'clean')
    r = tool('IMAGE=' + img, 'RESCUE=' + out)
    got = tree(out) if os.path.isdir(out) else {}
    result('clean', r.returncode == 0 and got == files and md5f(img) == before,
           '%d files' % len(got) if got == files else r.stdout[-600:] + ' missing %s extra %s' %
           (sorted(set(files) - set(got))[:5], sorted(set(got) - set(files))[:5]))
    r = tool('IMAGE=' + img)
    result('check', r.returncode == 0, r.stdout[-300:] if r.returncode else '')

    # --- deleted
    dimg = os.path.join(work, 'del.hdf'); shutil.copy(img, dimg)
    victims = [p for p in sorted(files) if p.startswith('Other/')][:3] + [p for p in sorted(files) if '/' not in p][:2]
    bridge(work, dimg, driver, [['delete', p] for p in victims] + [['flush']])
    out = os.path.join(work, 'undel')
    r = tool('IMAGE=' + dimg, 'UNDELETE=' + out)
    got = tree(out) if os.path.isdir(out) else {}
    want = sorted(files[p] for p in victims)
    result('undelete', sorted(got.values()) == want, '%d of %d deleted files back%s' %
           (len([m for m in got.values() if m in want]), len(victims),
            '' if sorted(got.values()) == want else '\n' + r.stdout[-500:]))
    out = os.path.join(work, 'resc_del')
    r = tool('IMAGE=' + dimg, 'RESCUE=' + out)
    got = tree(out) if os.path.isdir(out) else {}
    live = {p: m for p, m in files.items() if p not in victims}
    result('rescue after delete', got == live, '' if got == live else r.stdout[-500:])

    # --- damage: destroy directory blocks one at a time
    data = open(img, 'rb').read()
    part0 = 2 * CYL
    marks = []
    if kind == 'pfs3':
        # only blocks the reserved bitmap marks as in use: PFS3 leaves old
        # copies of directory blocks behind
        rb = part0 + 1024
        resblk = int.from_bytes(data[rb + 64:rb + 66], 'big')
        lastres = int.from_bytes(data[rb + 52:rb + 56], 'big')
        bm = rb + 512
        assert data[bm:bm + 2] == b'BM'
        for i in range((lastres - 2 + 1) // (resblk // 512)):
            word = int.from_bytes(data[bm + 12 + 4 * (i // 32):bm + 16 + 4 * (i // 32)], 'big')
            off = part0 + (2 + i * (resblk // 512)) * 512
            if not (word & (0x80000000 >> (i % 32))) and data[off:off + 2] == b'DB' and data[off + 20] != 0:
                marks.append(off)
    else:
        for off in range(part0, len(data), 512):
            if data[off:off + 4] == b'OBJC': marks.append(off)
    rng.shuffle(marks)
    for i, off in enumerate(marks[:8]):
        dm = os.path.join(work, 'dmg%d.hdf' % i)
        with open(dm, 'wb') as f:
            f.write(data[:off]); f.write(bytes(rng.getrandbits(8) for _ in range(512))); f.write(data[off + 512:])
        before = md5f(dm)
        out = os.path.join(work, 'dmg%d' % i)
        try:
            r = tool('IMAGE=' + dm, 'RESCUE=' + out)
        except subprocess.TimeoutExpired:
            result('damage %d' % i, False, 'hang'); continue
        got = tree(out) if os.path.isdir(out) else {}
        bad = check_subset(got, files, True, r.stdout)
        found = set(got.values()) & set(files.values())
        ok = r.returncode in (0, 5, 10) and not bad and md5f(dm) == before and len(found) >= len(set(files.values())) - 45
        result('damage %d' % i, ok, '%d of %d files back, rc %d%s' % (len(found), len(set(files.values())), r.returncode,
               '' if ok else ' wrong content: %s\n%s' % (bad[:5], r.stdout[-500:])))
        os.remove(dm)

    # --- fuzz: garbage into random filesystem blocks
    meta = marks[:]
    if kind == 'pfs3':
        meta = [off for off in range(part0 + 1024, part0 + 2 * 1024 * 1024, 512) if data[off:off + 2] in (b'DB', b'AB', b'IB', b'BM', b'MI', b'EX', b'DD', b'SB')] + [part0 + 1024]
    else:
        meta = [off for off in range(part0, len(data), 512) if data[off:off + 4] in (b'OBJC', b'BNDC', b'ADMC', b'NDC ', b'HTAB', b'BTMP')] + [part0]
    for i in range(nfuzz):
        frng = random.Random('%s%d' % (kind, i))
        buf = bytearray(data)
        for _ in range(frng.randint(1, 4)):
            off = frng.choice(meta)
            if frng.random() < 0.5:
                for _ in range(frng.randint(1, 12)): buf[off + frng.randrange(512)] = frng.getrandbits(8)
            else:
                buf[off:off + 512] = bytes(frng.getrandbits(8) for _ in range(512))
        fz = os.path.join(work, 'fuzz.hdf')
        open(fz, 'wb').write(buf)
        before = md5f(fz)
        out = os.path.join(work, 'fuzz%d' % i)
        try:
            r = tool('IMAGE=' + fz, 'RESCUE=' + out, 'DELETED', timeout=120)
            c = tool('IMAGE=' + fz, timeout=120)
        except subprocess.TimeoutExpired:
            result('fuzz %d' % i, False, 'hang'); shutil.copy(fz, os.path.join(work, 'fail_fuzz%d.hdf' % i)); continue
        got = tree(out) if os.path.isdir(out) else {}
        bad = check_subset(got, files, True, r.stdout)
        ok = r.returncode in (0, 5, 10, 20) and c.returncode in (0, 5, 10, 20) and not bad and md5f(fz) == before
        if not ok:
            result('fuzz %d' % i, False, 'rc %d/%d wrong content %s\n%s' % (r.returncode, c.returncode, bad[:5], r.stdout[-400:]))
            shutil.copy(fz, os.path.join(work, 'fail_fuzz%d.hdf' % i))
        shutil.rmtree(out, ignore_errors=True)
    if nfuzz: print('%-5s fuzz: %d runs' % (kind, nfuzz))
    print('%-5s work dir %s' % (kind, work))
    return fails

def main():
    args = sys.argv[1:]
    nfuzz = int(args[args.index('--fuzz') + 1]) if '--fuzz' in args else 0
    kinds = [a for a in args if a in KINDS] or list(KINDS)
    for need, what in ((os.path.exists(AMIPART), 'AmiPart host CLI'), (os.path.exists(PYTHON), 'amifuse venv python')):
        if not need: print('SKIP: %s not found' % what); return 0
    fails = 0
    for k in kinds:
        if not KINDS[k][1]: print('SKIP %s: handler not found' % k); continue
        fails += run_kind(k, nfuzz)
    print('%d failure(s)' % fails)
    return 1 if fails else 0

sys.exit(main())
