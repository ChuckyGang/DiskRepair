"""Minimal independent OFS/FFS image access for the test harness.

Deliberately NOT a port of the C code: it is the second opinion the
corruptor and the verifier work from.  512-byte blocks, DOS\\0..DOS\\3."""
import struct

BS = 512
NL = BS // 4

class Img:
    def __init__(self, path):
        self.path = path
        self.d = bytearray(open(path, 'rb').read())
        self.n = len(self.d) // BS
        self.root = (self.n - 1 + 2) // 2
        self.intl = (self.d[3] & 7) >= 2
        self.ofs = (self.d[3] & 1) == 0

    def save(self, path=None):
        open(path or self.path, 'wb').write(self.d)

    def blk(self, b):
        return self.d[b * BS:(b + 1) * BS]

    def put(self, b, data):
        assert len(data) == BS
        self.d[b * BS:(b + 1) * BS] = data

    def l(self, b, i):
        if i < 0: i += NL
        return struct.unpack_from('>I', self.d, b * BS + i * 4)[0]

    def setl(self, b, i, v, fix=True):
        if i < 0: i += NL
        struct.pack_into('>I', self.d, b * BS + i * 4, v & 0xFFFFFFFF)
        if fix: self.fixsum(b)

    def fixsum(self, b, at=5):
        struct.pack_into('>I', self.d, b * BS + at * 4, 0)
        s = sum(struct.unpack_from('>%dI' % NL, self.d, b * BS)) & 0xFFFFFFFF
        struct.pack_into('>I', self.d, b * BS + at * 4, (-s) & 0xFFFFFFFF)

    def sum_ok(self, b):
        return sum(struct.unpack_from('>%dI' % NL, self.d, b * BS)) & 0xFFFFFFFF == 0

    def name(self, b):
        o = b * BS + (NL - 20) * 4
        return bytes(self.d[o + 1:o + 1 + self.d[o]]).decode('latin-1')

    def up(self, c):
        if 97 <= c <= 122: return c - 32
        if self.intl and 0xE0 <= c <= 0xFE and c != 0xF7: return c - 32
        return c

    def hash(self, name):
        raw = name.encode('latin-1')
        h = len(raw)
        for c in raw: h = (h * 13 + self.up(c)) & 0x7FF
        return h % (NL - 56)

    def entries(self, d):
        """[(key, slot, prev)] of directory block d; stops a chain at nonsense."""
        out = []
        for slot in range(NL - 56):
            k, prev, guard = self.l(d, 6 + slot), 0, 0
            while k and 2 <= k < self.n and guard < 100000:
                if self.l(k, 0) != 2 or not self.sum_ok(k): break
                out.append((k, slot, prev))
                prev, k, guard = k, self.l(k, -4), guard + 1
        return out

    def find(self, path):
        """header block of 'a/b/c', or None"""
        d = self.root
        for part in [p for p in path.split('/') if p]:
            hit = None
            for k, _, _ in self.entries(d):
                if self.name(k).lower() == part.lower(): hit = k; break
            if hit is None: return None
            d = hit
        return d

    def ref(self, path):
        """(key, dir, slot, prev) of the entry"""
        parts = [p for p in path.split('/') if p]
        d = self.find('/'.join(parts[:-1]))
        for k, slot, prev in self.entries(d):
            if self.name(k).lower() == parts[-1].lower(): return k, d, slot, prev
        return None

    def tree(self, d=None, prefix=''):
        """{path: (key, sectype)} of everything reachable"""
        d = self.root if d is None else d
        out = {}
        for k, _, _ in self.entries(d):
            st = struct.unpack('>i', struct.pack('>I', self.l(k, -1)))[0]
            p = prefix + self.name(k)
            out[p] = (k, st)
            if st == 2: out.update(self.tree(k, p + '/'))
        return out

    def file_blocks(self, k):
        """data block numbers of the file with header k (may raise on damage)"""
        out, b, guard = [], k, 0
        while True:
            high = self.l(b, 2)
            if high > NL - 56: raise ValueError('high_seq')
            for i in range(high):
                p = self.l(b, NL - 51 - i)
                if not (2 <= p < self.n): raise ValueError('pointer')
                out.append(p)
            e = self.l(b, -2)
            if e == 0: return out
            if not (2 <= e < self.n) or self.l(e, 0) != 16 or not self.sum_ok(e): raise ValueError('ext')
            b, guard = e, guard + 1
            if guard > self.n: raise ValueError('loop')

    def read_file(self, k):
        size = self.l(k, -47)
        data = bytearray()
        for b in self.file_blocks(k):
            data += self.blk(b)[24:] if self.ofs else self.blk(b)
        return bytes(data[:size])

    def bitmap_pages(self):
        span = self.n - 2
        pages = (span + (NL - 1) * 32 - 1) // ((NL - 1) * 32)
        return [self.l(self.root, NL - 49 + i) for i in range(min(pages, 25))]

    def bm_free(self, b):
        i = b - 2
        page = self.bitmap_pages()[i // ((NL - 1) * 32)]
        r = i % ((NL - 1) * 32)
        return (self.l(page, 1 + r // 32) >> (r % 32)) & 1 == 1

    def validate(self):
        """Independent consistency check: list of problems ([] = clean)."""
        probs, used = [], {}
        def claim(b, who):
            if b in used: probs.append('block %d shared by %s and %s' % (b, used[b], who))
            used[b] = who
        if self.l(self.root, 0) != 2 or self.l(self.root, -1) != 1 or not self.sum_ok(self.root):
            return ['no root']
        claim(self.root, 'root')
        for p in self.bitmap_pages():
            if not self.sum_ok(p): probs.append('bitmap page %d checksum' % p)
            claim(p, 'bitmap')
        if self.l(self.root, -50) != 0xFFFFFFFF: probs.append('bm_flag invalid')
        def walk(d, prefix):
            for slot in range(NL - 56):
                k, guard = self.l(d, 6 + slot), 0
                while k:
                    if not (2 <= k < self.n): probs.append('%s slot %d: key %d out of range' % (prefix, slot, k)); break
                    if self.l(k, 0) != 2 or not self.sum_ok(k) or self.l(k, 1) != k:
                        probs.append('%s slot %d: block %d is not a header' % (prefix, slot, k)); break
                    nm = prefix + self.name(k)
                    if self.l(k, -3) != d: probs.append('%s: parent' % nm)
                    if self.hash(self.name(k)) != slot: probs.append('%s: hash' % nm)
                    claim(k, nm)
                    st = struct.unpack('>i', struct.pack('>I', self.l(k, -1)))[0]
                    if st == 2: walk(k, nm + '/')
                    elif st == -3:
                        try:
                            b, e = k, self.l(k, -2)
                            for x in self.file_blocks(k): claim(x, nm)
                            while e: claim(e, nm + ' ext'); e = self.l(e, -2)
                        except ValueError as ex: probs.append('%s: %s' % (nm, ex))
                    k = self.l(k, -4); guard += 1
                    if guard > 100000: probs.append('chain loop'); break
        walk(self.root, '')
        if not any(p == 'no root' for p in probs):
            for b in range(2, self.n):
                try: free = self.bm_free(b)
                except Exception: probs.append('bitmap unreadable'); break
                if b in used and free: probs.append('block %d used but free in bitmap' % b); break
                if b not in used and not free: probs.append('block %d free but used in bitmap' % b); break
        return probs
