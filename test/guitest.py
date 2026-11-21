#!/usr/bin/env python3
"""Linux window test, headless: the window must do exactly what the CLI does.

Drives host/diskrepair-gui through its script interface (no display
needed): Check, Repair plan, Rescue, Repair (confirming the requester),
then compares the repaired image and the rescued tree with the CLI's.

  python3 test/guitest.py"""
import os, sys, shutil, subprocess, hashlib, random, filecmp

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import repairtest as rt
from ffsimg import Img

GUI = os.path.join(HERE, '..', 'host', 'diskrepair-gui')

def md5(p): return hashlib.md5(open(p, 'rb').read()).hexdigest()

def same_tree(a, b):
    c = filecmp.dircmp(a, b, ignore=['DiskRepair-attributes.txt'])
    if c.left_only or c.right_only or c.diff_files or c.funny_files: return False
    return all(same_tree(os.path.join(a, d), os.path.join(b, d)) for d in c.common_dirs)

def main():
    os.makedirs(rt.WORK, exist_ok=True)
    pristine, files = rt.build_pristine('ffs')
    fails = 0
    for scen in (rt.s_kill_dir, rt.s_bad_chain_ptr, rt.s_bmgarbage, rt.s_kill_root):
        name = scen.__name__
        tag = '%s_%d' % (name, os.getpid())
        a, b = os.path.join(rt.WORK, 'gui_a_%s.hdf' % tag), os.path.join(rt.WORK, 'gui_b_%s.hdf' % tag)
        shutil.copy(pristine, a)
        im = Img(a); scen(im, random.Random(name)); im.save()
        shutil.copy(a, b)
        ra, rb = a + '.rescued', b + '.rescued'
        rt.run('IMAGE=' + a, 'RESCUE=' + ra)
        rt.run('IMAGE=' + a, 'REPAIR', 'UNDOFILE=' + a + '.undo', 'FORCE')
        script = os.path.join(rt.WORK, 'gui_%s.script' % tag)
        open(script, 'w').write('\n'.join([
            'gadid 9', 'gadid 2', 'gadid 3',
            'set 11 ' + rb, 'gadid 5',
            'set 12 ' + b + '.undo', 'gadid 4', 'gad Repair',
            'quit', '']))
        env = dict(os.environ, DISKREPAIR_GUI_HEADLESS='1', DISKREPAIR_GUI_SCRIPT=script)
        p = subprocess.run([GUI, b], env=env, capture_output=True, text=True, timeout=300)
        ok = p.returncode == 0 and md5(a) == md5(b) and same_tree(ra, rb)
        print('%-18s %s' % (name, 'OK   window = CLI (image and rescued files)' if ok else
                            'FAIL rc %d image %s tree %s\n%s' % (p.returncode, md5(a) == md5(b), same_tree(ra, rb), p.stdout[-800:])))
        fails += not ok
        for f in (a, b, a + '.undo', b + '.undo', script):
            if os.path.exists(f): os.remove(f)
        shutil.rmtree(ra, ignore_errors=True); shutil.rmtree(rb, ignore_errors=True)
    print('%d failure(s)' % fails)
    sys.exit(1 if fails else 0)

if __name__ == '__main__':
    main()
