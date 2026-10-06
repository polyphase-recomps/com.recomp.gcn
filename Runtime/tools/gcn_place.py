#!/usr/bin/env python3
"""
Puts the game's globals back at their addresses in the original DOL.

Matching decomps lean on the original data layout: structs laid over several neighbouring
globals ("address views"), strings reached as base + offset, tables that run into the next
symbol. wasm-ld lays data out its own way, so after linking (with --emit-relocs) this tool
moves every decomp global it can identify to its original address:

  1. Reads the linked wasm: data segments, the symbol table and the relocations.
  2. Finds each decomp global's original address: by name in the decomp's symbols.txt
     (dtk), else by contents among the unnamed symbols of the same unit (splits.txt gives
     each unit's address ranges, the user's DOL gives the bytes), else for zeroed data by
     order among the unit's unclaimed symbols of the same size.
  3. Rewrites every code and data relocation that targets a placed global to the original
     address (+ addend), so code reaches the neighbours the original reached.
  4. Writes the placement table into gcn_place_table (guest_raw/entry.c). At boot the guest
     loads the DOL's data sections from the disc to their addresses (unplaced data keeps
     its original bytes) and copies each placed global's initial contents over them.

  gcn_place.py <game.wasm> <game.map> <units.json> <symbols.txt> <splits.txt> <disc image>

units.json maps object file stems to the unit's path in splits.txt.
"""
import bisect
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gcn_disc import Disc, Dol  # noqa: E402

PLACE_MAGIC = 0x47504C43   # 'GPLC', first word of gcn_place_table
PLACE_HEADER = 4           # magic, count, placed end, reserved


# -- LEB128 -----------------------------------------------------------------

def uleb(b, p):
    v = shift = 0
    while True:
        x = b[p]
        p += 1
        v |= (x & 0x7F) << shift
        shift += 7
        if not x & 0x80:
            return v, p


def sleb(b, p):
    v = shift = 0
    while True:
        x = b[p]
        p += 1
        v |= (x & 0x7F) << shift
        shift += 7
        if not x & 0x80:
            if x & 0x40:
                v -= 1 << shift
            return v, p


def put_uleb5(b, p, v):
    v &= 0xFFFFFFFF
    for i in range(4):
        b[p + i] = (v & 0x7F) | 0x80
        v >>= 7
    b[p + 4] = v & 0x7F


def put_sleb5(b, p, v):
    v &= 0xFFFFFFFF
    if v >= 0x80000000:
        v -= 1 << 32
    for i in range(4):
        b[p + i] = (v & 0x7F) | 0x80
        v >>= 7
    b[p + 4] = v & 0x7F


def padded(b, p):
    return all(b[p + i] & 0x80 for i in range(4)) and not b[p + 4] & 0x80


# -- wasm -------------------------------------------------------------------

RELOC_ADDEND = {3, 4, 5, 8, 9, 11, 14, 15, 16, 17, 21, 22, 23, 25}
R_MEMORY_ADDR_LEB, R_MEMORY_ADDR_SLEB, R_MEMORY_ADDR_I32 = 3, 4, 5
SYM_DATA = 1
SYM_UNDEFINED = 0x10


class Wasm:
    def __init__(self, data):
        self.b = bytearray(data)
        assert self.b[:4] == b'\0asm', 'not a wasm file'
        self.sections = []          # (id, name, payload start, payload end)
        p = 8
        while p < len(self.b):
            sid = self.b[p]
            size, q = uleb(self.b, p + 1)
            name = None
            if sid == 0:
                n, r = uleb(self.b, q)
                name = self.b[r:r + n].decode()
            self.sections.append((sid, name, q, q + size))
            p = q + size
        self.segments = []          # (memory address, file offset of bytes, size)
        self.segment_names = []
        self.symbols = []           # (kind, flags, name, address or None, size)
        self.relocs = []            # (target section index, type, file offset, symbol, addend)
        self._data()
        self._linking()
        self._relocs()

    def section(self, sid, name=None):
        for i, s in enumerate(self.sections):
            if s[0] == sid and (name is None or s[1] == name):
                return i, s
        return None, None

    def _data(self):
        _, s = self.section(11)
        p = s[2]
        count, p = uleb(self.b, p)
        for _ in range(count):
            flags, p = uleb(self.b, p)
            if flags == 2:
                _, p = uleb(self.b, p)
            addr = None
            if flags in (0, 2):
                assert self.b[p] == 0x41, 'data segment offset is not i32.const'
                addr, p = sleb(self.b, p + 1)
                assert self.b[p] == 0x0B
                p += 1
                addr &= 0xFFFFFFFF
            size, p = uleb(self.b, p)
            self.segments.append((addr, p, size))
            p += size

    def _linking(self):
        _, s = self.section(0, 'linking')
        assert s, 'no linking section: link with --emit-relocs'
        n, p = uleb(self.b, s[2])
        p += n
        version, p = uleb(self.b, p)
        assert version == 2, 'linking section version %d' % version
        while p < s[3]:
            kind = self.b[p]
            size, p = uleb(self.b, p + 1)
            end = p + size
            if kind == 5:          # WASM_SEGMENT_INFO
                count, q = uleb(self.b, p)
                for _ in range(count):
                    n, q = uleb(self.b, q)
                    self.segment_names.append(self.b[q:q + n].decode())
                    q += n
                    _, q = uleb(self.b, q)
                    _, q = uleb(self.b, q)
            elif kind == 8:        # WASM_SYMBOL_TABLE
                count, q = uleb(self.b, p)
                for _ in range(count):
                    k = self.b[q]
                    flags, q = uleb(self.b, q + 1)
                    name, addr, size = None, None, 0
                    if k in (0, 2, 4, 5):
                        _, q = uleb(self.b, q)
                        if not flags & SYM_UNDEFINED or flags & 0x40:
                            n, q = uleb(self.b, q)
                            name = self.b[q:q + n].decode('utf-8', 'replace')
                            q += n
                    elif k == SYM_DATA:
                        n, q = uleb(self.b, q)
                        name = self.b[q:q + n].decode('utf-8', 'replace')
                        q += n
                        if not flags & SYM_UNDEFINED:
                            seg, q = uleb(self.b, q)
                            off, q = uleb(self.b, q)
                            size, q = uleb(self.b, q)
                            if not flags & 0x200 and seg < len(self.segments) and self.segments[seg][0] is not None:
                                addr = self.segments[seg][0] + off
                    elif k == 3:
                        _, q = uleb(self.b, q)
                    self.symbols.append((k, flags, name, addr, size))
            p = end

    def _relocs(self):
        for sid, name, start, end in self.sections:
            if sid != 0 or not name.startswith('reloc.'):
                continue
            n, p = uleb(self.b, start)
            p += n
            target, p = uleb(self.b, p)
            count, p = uleb(self.b, p)
            base = self.sections[target][2]
            for _ in range(count):
                t = self.b[p]
                off, p = uleb(self.b, p + 1)
                idx, p = uleb(self.b, p)
                addend = 0
                if t in RELOC_ADDEND:
                    addend, p = sleb(self.b, p)
                self.relocs.append((target, t, base + off, idx, addend))

    def data_bytes(self, addr, size):
        """Initial bytes at a linked address, or None in a segment without bytes (bss)."""
        for a, off, n in self.segments:
            if a is not None and a <= addr and addr + size <= a + n:
                return bytes(self.b[off + addr - a:off + addr - a + size])
        return None

    def data_offset(self, addr):
        for a, off, n in self.segments:
            if a is not None and a <= addr < a + n:
                return off + addr - a
        return None


# -- decomp layout ------------------------------------------------------------

def read_symbols(path):
    """symbols.txt objects: name -> [(address, size, local)]; address-sorted list."""
    by_name = {}
    rx = re.compile(r'^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?(.*)')
    for line in open(path, encoding='utf-8'):
        m = rx.match(line)
        if not m or m.group(4) != 'object':
            continue
        name, sec, addr, _, size, rest = m.groups()
        by_name.setdefault(name, []).append((int(addr, 16), int(size or '0', 16), 'scope:local' in rest, sec))
    flat = sorted((a, s, n, sec) for n, v in by_name.items() for a, s, _, sec in v)
    return by_name, flat


def read_splits(path):
    """unit path -> [(section, start, end)]"""
    units, cur = {}, None
    rx = re.compile(r'^\s+(\S+)\s+start:0x([0-9A-Fa-f]+)\s+end:0x([0-9A-Fa-f]+)')
    for line in open(path, encoding='utf-8'):
        if line.strip().endswith(':') and not line[0].isspace():
            cur = units.setdefault(line.strip()[:-1], [])
        elif cur is not None:
            m = rx.match(line)
            if m:
                cur.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
    return units


def read_map_objects(path):
    """(symbol name, address) -> object file stem, for data symbols."""
    out = {}
    obj = None
    rx = re.compile(r'^([0-9a-f]+)\s+[0-9a-f]+\s+[0-9a-f]+(\s+)(.*)$')
    for line in open(path, encoding='utf-8', errors='replace'):
        m = rx.match(line)
        if not m:
            continue
        addr, indent, rest = int(m.group(1), 16), len(m.group(2)), m.group(3)
        if '.o:(' in rest:
            obj = os.path.splitext(os.path.basename(rest.split(':(')[0]))[0]
        elif indent > 12 and obj:
            out[(rest.strip(), addr)] = obj
    return out


# -- placement ----------------------------------------------------------------

def main():
    if len(sys.argv) != 7:
        print(__doc__)
        return 2
    wasm_path, map_path, units_path, symbols_path, splits_path, disc_path = sys.argv[1:]
    w = Wasm(open(wasm_path, 'rb').read())
    units = json.load(open(units_path, encoding='utf-8'))
    by_name, flat = read_symbols(symbols_path)
    flat_addrs = [a for a, _, _, _ in flat]
    splits = read_splits(splits_path)
    objects = read_map_objects(map_path)
    disc = Disc(disc_path)
    dol = Dol(disc.dol())

    # reloc slots inside data, by linked address (their bytes are pointers, not contents)
    data_sec = w.section(11)[0]
    slot_at = set()
    seg_by_off = sorted((off, a, n) for a, off, n in w.segments if a is not None)
    offs = [s[0] for s in seg_by_off]
    for target, t, off, idx, addend in w.relocs:
        if target == data_sec and t in (R_MEMORY_ADDR_I32, 2, 26):
            i = bisect.bisect_right(offs, off) - 1
            so, sa, sn = seg_by_off[i]
            slot_at.add(sa + off - so)

    def contents(addr, size):
        """(bytes, mask of bytes that are reloc slots) of a linked symbol, None for bss"""
        b = w.data_bytes(addr, size)
        if b is None:
            return None, None
        mask = bytearray(size)
        for o in range(0, size):
            if addr + o in slot_at:
                for k in range(4):
                    if o + k < size:
                        mask[o + k] = 1
        return b, mask

    def same(b, mask, orig):
        return all(mask[i] or b[i] == orig[i] for i in range(len(b)))

    def orig_bytes(addr, size):
        try:
            return dol.read(addr, size)
        except KeyError:
            return None

    def unit_of(sym_name, addr):
        obj = objects.get((sym_name, addr))
        return units.get(obj) if obj else None

    # our data symbols that belong to decomp units
    ours = []
    for i, (k, flags, name, addr, size) in enumerate(w.symbols):
        if k != SYM_DATA or addr is None or flags & SYM_UNDEFINED or not size:
            continue
        u = unit_of(name, addr)
        if u is None:
            continue
        ours.append((i, name, addr, size, u))

    placed = {}            # symbol index -> original address
    taken = []             # sorted (start, end) of claimed original ranges
    claimed = set()        # original symbol addresses taken
    stats = {'name': 0, 'content': 0, 'order': 0, 'size': 0, 'overlap': 0, 'differs': 0}
    differs = []

    def free(a, n):
        j = bisect.bisect_left(taken, (a, a + n))
        if j > 0 and taken[j - 1][1] > a:
            return False
        if j < len(taken) and taken[j][0] < a + n:
            return False
        return True

    def claim(idx, a, n):
        bisect.insort(taken, (a, a + n))
        claimed.add(a)
        placed[idx] = a

    def in_unit(u, a):
        return any(s <= a < e for _, s, e in splits.get(u, []))

    # 1. by name
    rest = []
    for idx, name, addr, size, u in ours:
        base = name.split('.')[0] if '.' in name and not name.startswith('.') else name
        cands = by_name.get(name) or by_name.get(base) or []
        cands = [c for c in cands if not c[2] or in_unit(u, c[0])]
        if len(cands) > 1:
            cands = [c for c in cands if in_unit(u, c[0])] or cands
        if len(cands) != 1:
            rest.append((idx, name, addr, size, u))
            continue
        a, osize, _, _ = cands[0]
        if osize and size > osize:
            stats['size'] += 1
            rest.append((idx, name, addr, size, u))
            continue
        if not free(a, size):
            stats['overlap'] += 1
            continue
        b, mask = contents(addr, size)
        if b is not None:
            o = orig_bytes(a, size)
            if o is not None and not same(b, mask, o):
                stats['differs'] += 1
                differs.append(name)
        claim(idx, a, size)
        stats['name'] += 1

    # 2. by contents / order, among the unit's original symbols nobody claimed
    by_unit = {}
    for r in rest:
        by_unit.setdefault(r[4], []).append(r)
    for u, syms in by_unit.items():
        ranges = splits.get(u, [])
        orig = []
        for sec, s, e in ranges:
            i = bisect.bisect_left(flat_addrs, s)
            while i < len(flat) and flat[i][0] < e:
                orig.append(flat[i])
                i += 1
        orig = [o for o in orig if o[0] not in claimed]
        zero_left = []
        for idx, name, addr, size, _ in syms:
            b, mask = contents(addr, size)
            if b is None or not any(b):
                zero_left.append((idx, name, addr, size, b is None))
                continue
            hits = [o for o in orig if o[1] == size and o[0] not in claimed
                    and (lambda ob: ob is not None and same(b, mask, ob))(orig_bytes(o[0], size))]
            if hits and free(hits[0][0], size):
                claim(idx, hits[0][0], size)
                stats['content'] += 1
        # zeroed data: pair by order with same-size unclaimed symbols of the same kind
        for bss in (True, False):
            mine = [z for z in zero_left if z[4] == bss]
            kinds = ('.bss', '.sbss', '.sbss2') if bss else ('.data', '.sdata', '.rodata', '.sdata2')
            theirs = [o for o in orig if o[3] in kinds and o[0] not in claimed
                      and (bss or not any(orig_bytes(o[0], o[1]) or b'\1'))]
            if mine and [m[3] for m in mine] == [t[1] for t in theirs]:
                for m, t in zip(mine, theirs):
                    if free(t[0], m[3]):
                        claim(m[0], t[0], m[3])
                        stats['order'] += 1

    # 3. retarget relocations
    fixed = 0
    for target, t, off, idx, addend in w.relocs:
        if idx not in placed or t not in (R_MEMORY_ADDR_LEB, R_MEMORY_ADDR_SLEB, R_MEMORY_ADDR_I32):
            continue
        k, flags, name, addr, size = w.symbols[idx]
        if k != SYM_DATA:
            continue
        old = (addr + addend) & 0xFFFFFFFF
        new = (placed[idx] + addend) & 0xFFFFFFFF
        if t == R_MEMORY_ADDR_I32:
            cur = struct.unpack_from('<I', w.b, off)[0]
            assert cur == old, '%s: data reloc holds %08X, expected %08X' % (name, cur, old)
            struct.pack_into('<I', w.b, off, new)
        else:
            assert padded(w.b, off), '%s: relocation at %X is not padded' % (name, off)
            cur, _ = (uleb if t == R_MEMORY_ADDR_LEB else sleb)(w.b, off)
            assert cur & 0xFFFFFFFF == old, '%s: code reloc holds %08X, expected %08X' % (name, cur & 0xFFFFFFFF, old)
            (put_uleb5 if t == R_MEMORY_ADDR_LEB else put_sleb5)(w.b, off, new)
        fixed += 1

    # 4. placement table: (original address, linked address, size) of initialised data
    table = [s for s in w.symbols if s[0] == SYM_DATA and s[2] == 'gcn_place_table' and s[3] is not None]
    assert table, 'gcn_place_table missing (Runtime/guest_raw/entry.c)'
    taddr, tsize = table[0][3], table[0][4]
    toff = w.data_offset(taddr)
    assert struct.unpack_from('<I', w.b, toff)[0] == PLACE_MAGIC
    entries = []
    for idx, a in sorted(placed.items(), key=lambda x: x[1]):
        _, _, name, addr, size = w.symbols[idx]
        if w.data_bytes(addr, size) is None:
            continue           # bss: the region starts zeroed
        if entries and entries[-1][0] + entries[-1][2] == a and entries[-1][1] + entries[-1][2] == addr:
            entries[-1][2] += size
        else:
            entries.append([a, addr, size])
    cap = (tsize // 4 - PLACE_HEADER) // 3
    assert len(entries) <= cap, 'gcn_place_table holds %d entries, %d needed' % (cap, len(entries))
    end = max(e for r in splits.values() for _, _, e in r)
    struct.pack_into('<4I', w.b, toff, PLACE_MAGIC, len(entries), end, 0)
    for i, (a, addr, size) in enumerate(entries):
        struct.pack_into('<3I', w.b, toff + 4 * (PLACE_HEADER + 3 * i), a, addr, size)

    # final addresses of the data symbols ("address offset size name", like the map's lines),
    # for the runner's --watch and for mods that read game variables by name
    syms = os.path.splitext(wasm_path)[0] + '.syms'
    with open(syms, 'w', encoding='utf-8') as f:
        f.write('# data symbols after gcn_place.py: address 0 size name\n')
        for i, (k, flags, name, addr, size) in enumerate(w.symbols):
            if k == SYM_DATA and addr is not None and name and not name.startswith('.L'):
                f.write('%08x 0 %x %s\n' % (placed.get(i, addr), size, name))

    # drop what --emit-relocs added: the output is then what a plain link gives (wasm2c names
    # functions differently when the symbol table is there)
    out = bytearray(w.b[:8])
    for sid, name, start, end in w.sections:
        if sid == 0 and (name == 'linking' or name.startswith('reloc.')):
            continue
        hdr = bytearray([sid])
        n = end - start
        while True:
            x = n & 0x7F
            n >>= 7
            hdr.append(x | (0x80 if n else 0))
            if not n:
                break
        out += hdr + w.b[start:end]
    tmp = wasm_path + '.tmp'
    open(tmp, 'wb').write(out)
    os.replace(tmp, wasm_path)
    print('gcn_place: %d of %d decomp globals at their original addresses (%d by name, %d by contents, '
          '%d by order); %d references moved; %d copies at boot; %d differ from the DOL; '
          '%d too big, %d overlapping' % (len(placed), len(ours), stats['name'], stats['content'], stats['order'],
                                          fixed, len(entries), stats['differs'], stats['size'], stats['overlap']))
    if differs:
        print('gcn_place: contents differ from the DOL: ' + ' '.join(differs[:30]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
