#!/usr/bin/env python3
"""GameCube disc images: the file system (FST) and the boot executable (DOL).

Plain .iso / .gcm images, and NKit .nkit.iso (a playable image whose file offsets are
those of the FST, like any other).

    gcn_disc.py info <disc>
    gcn_disc.py ls <disc>
    gcn_disc.py extract <disc> <out_dir> [path...]      files from the FST (all by default)
    gcn_disc.py dol <disc> <out.dol>
    gcn_disc.py unpack <disc> <out_dir> [--force]

`unpack` writes the whole disc as files the runtime reads instead of the image (a game
package's Assets/Disc): files/<path> as in the FST, sys/head.bin (boot block, apploader),
sys/main.dol, sys/fst.bin, and disc.idx, the disc offset of each ("offset<TAB>size<TAB>
path" lines after a "GCNDISC 1 <disc size> <game id>" line). Files already there with the
right size are kept, so replaced (modded) files of the same size survive a re-run;
--force writes them all again.
"""
import os
import struct
import sys


class Disc:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        hdr = self.read(0, 0x440)
        self.game_id = hdr[0:6].decode('ascii', 'replace')
        self.revision = hdr[7]
        self.title = hdr[0x20:0x400].split(b'\0')[0].decode('latin-1')
        if struct.unpack('>I', hdr[0x1C:0x20])[0] != 0xC2339F3D:
            raise ValueError('%s is not a GameCube disc image' % path)
        self.dol_offset, self.fst_offset, self.fst_size = struct.unpack('>III', hdr[0x420:0x42C])
        self._files = None

    def read(self, off, size):
        self.f.seek(off)
        return self.f.read(size)

    def files(self):
        """{path: (offset, size)} for every file in the FST."""
        if self._files is not None:
            return self._files
        fst = self.read(self.fst_offset, self.fst_size)
        count = struct.unpack('>I', fst[8:12])[0]
        strings = 12 * count

        def name(i):
            off = struct.unpack('>I', fst[i * 12:i * 12 + 4])[0] & 0xFFFFFF
            end = fst.index(b'\0', strings + off)
            return fst[strings + off:end].decode('latin-1')

        out = {}

        def walk(first, last, prefix):
            i = first
            while i < last:
                flags, _, a, b = struct.unpack('>BBHII', fst[i * 12:i * 12 + 12])[0], 0, 0, 0
                e = struct.unpack('>III', fst[i * 12:i * 12 + 12])
                is_dir = e[0] >> 24
                if is_dir:
                    walk(i + 1, e[2], prefix + name(i) + '/')
                    i = e[2]
                else:
                    out[prefix + name(i)] = (e[1], e[2])
                    i += 1

        walk(1, count, '')
        self._files = out
        return out

    def file(self, path):
        off, size = self.files()[path]
        return self.read(off, size)

    def dol(self):
        hdr = self.read(self.dol_offset, 0x100)
        offs = struct.unpack('>18I', hdr[0:72])
        sizes = struct.unpack('>18I', hdr[0x90:0xD8])
        end = max(o + s for o, s in zip(offs, sizes) if s)
        return self.read(self.dol_offset, end)


class Dol:
    def __init__(self, data):
        self.data = data
        self.offs = struct.unpack('>18I', data[0:72])
        self.addrs = struct.unpack('>18I', data[72:144])
        self.sizes = struct.unpack('>18I', data[144:216])
        self.bss_addr, self.bss_size, self.entry = struct.unpack('>III', data[216:228])

    def read(self, addr, size):
        for o, a, s in zip(self.offs, self.addrs, self.sizes):
            if s and a <= addr and addr + size <= a + s:
                return self.data[o + addr - a:o + addr - a + size]
        if self.bss_addr <= addr < self.bss_addr + self.bss_size:
            return bytes(size)
        raise KeyError('address %08X (+%X) is not in the DOL' % (addr, size))


def unpack(d, out, force=False):
    files = d.files()
    dol = d.dol()
    first = min([o for o, s in files.values() if s] + [d.dol_offset, d.fst_offset])
    segments = [(0, first, 'sys/head.bin'), (d.dol_offset, len(dol), 'sys/main.dol'),
                (d.fst_offset, d.fst_size, 'sys/fst.bin')]
    segments += [(o, s, 'files/' + p) for p, (o, s) in files.items()]
    segments.sort()
    total = max(max(o + s for o, s, _ in segments), os.path.getsize(d.path))
    for i, (o, s, rel) in enumerate(segments):
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not force and os.path.exists(dst) and os.path.getsize(dst) == s:
            continue
        with open(dst, 'wb') as f:
            left, pos = s, o
            while left:
                chunk = d.read(pos, min(left, 1 << 22))
                f.write(chunk)
                left -= len(chunk)
                pos += len(chunk)
        if i % 500 == 0:
            print('gcn_disc: %d / %d' % (i, len(segments)))
    with open(os.path.join(out, 'disc.idx'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('GCNDISC 1 %d %s\n' % (total, d.game_id))
        for o, s, rel in segments:
            f.write('%d\t%d\t%s\n' % (o, s, rel))
    print('gcn_disc: %s unpacked to %s (%d files)' % (d.game_id, out, len(files)))


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    d = Disc(path)
    if cmd == 'info':
        print('%s rev %d: %s' % (d.game_id, d.revision, d.title))
        print('DOL at 0x%X, FST at 0x%X (0x%X bytes), %d files' % (d.dol_offset, d.fst_offset, d.fst_size,
                                                                    len(d.files())))
    elif cmd == 'ls':
        for p, (o, s) in sorted(d.files().items()):
            print('%10d  %s' % (s, p))
    elif cmd == 'extract':
        out = sys.argv[3]
        want = sys.argv[4:]
        for p, (o, s) in sorted(d.files().items()):
            if want and p not in want:
                continue
            dst = os.path.join(out, p)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            data = d.read(o, s)
            if os.path.exists(dst) and os.path.getsize(dst) == s:
                continue
            open(dst, 'wb').write(data)
    elif cmd == 'dol':
        open(sys.argv[3], 'wb').write(d.dol())
    elif cmd == 'unpack':
        unpack(d, sys.argv[3], '--force' in sys.argv[4:])
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
