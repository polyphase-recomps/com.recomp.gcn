#!/usr/bin/env python3
"""Names a game's unnamed SDK / library functions by matching their code against a game whose
decomp names them (Dolphin SDK, MSL, MusyX / AX, ... are the same libraries in every game).

    gcn_sdkmatch.py --ref-dol sfa.dol --ref-symbols SFA/config/GSAE01_rev1/symbols.txt
                    --dol fzgx.dol --symbols fzgx/config/GFZE01/symbols.txt --out names.txt
                    [--min-insns 4]

A function's signature is its instructions with what linking changes masked out: branch targets
(b / bl), and the 16-bit immediates that are addresses (lis, r2/r13 small-data offsets, the low
halves of what a lis started). Functions of the target whose signature equals exactly one named
reference function get its name; then names spread along calls: a matched pair's bl targets are
the same functions in both games. Names the target's decomp already has are kept (and used to
check the matcher: disagreements are reported).

Writes `<address> <name>` lines (gcn_syms.py --names reads them).
"""
import argparse
import collections
import re
import struct
import sys

SYM_RE = re.compile(r'^(\S+) = (\.?\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?(.*)$')


def read_functions(path):
    """{addr: (name, size)} of type:function symbols."""
    out = {}
    for line in open(path, encoding='utf-8'):
        m = SYM_RE.match(line.strip())
        if m and m.group(4) == 'function' and m.group(5):
            out[int(m.group(3), 16)] = (m.group(1), int(m.group(5), 16))
    return out


class Dol:
    def __init__(self, data):
        self.data = data
        offs = struct.unpack('>18I', data[0:72])
        addrs = struct.unpack('>18I', data[0x48:0x90])
        sizes = struct.unpack('>18I', data[0x90:0xD8])
        self.secs = [(addrs[i], sizes[i], offs[i]) for i in range(18) if sizes[i]]

    def words(self, addr, size):
        for base, n, off in self.secs:
            if base <= addr and addr + size <= base + n:
                o = off + addr - base
                return list(struct.unpack('>%dI' % (size // 4), self.data[o:o + size]))
        return None


def signature(words):
    """masked instruction words, and the indices of the bl / b instructions (call edges)"""
    out = []
    lis = [False] * 32  # registers a lis (address high half) set
    for w in words:
        op = w >> 26
        d, a = (w >> 21) & 31, (w >> 16) & 31
        if op == 18:  # b / bl: target is link-time
            out.append(w & 0xFC000003)
            continue
        if op in (14, 15, 24, 25) or 32 <= op <= 63 and op not in (59, 63, 31, 4):
            masked = False
            if op == 15:  # addis / lis
                masked = True
                if a == 0:
                    lis[d] = True
            elif op in (14, 24) and (a in (2, 13) or lis[a]) and op != 24:
                masked = True
            elif op == 24 and lis[d]:  # ori rA,rS,lo after lis rS
                masked = True
            elif 32 <= op <= 61 and (a in (2, 13) or lis[a]):
                masked = True
            elif op == 14 and a == 0:
                masked = False  # li: a constant
            out.append(w & 0xFFFF0000 if masked else w)
            # the destination of addi / loads no longer holds a lis value
            if op in (14, 24, 25) or 32 <= op <= 47:
                dst = a if op in (24, 25) else d
                if not (op == 14 and a == dst) and dst != a:
                    lis[dst] = False
            continue
        out.append(w)
    return tuple(out)


def calls(words, addr):
    """[(index, target)] of the bl / b instructions"""
    out = []
    for k, w in enumerate(words):
        if w >> 26 == 18:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            out.append((k, (li if w & 2 else addr + 4 * k + li) & 0xFFFFFFFF))
    return out


def address_loads(words):
    """the addresses lis / addi (or ori) pairs build"""
    hi, out = {}, set()
    for w in words:
        op, d, a, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15 and a == 0:
            hi[d] = imm << 16
        elif op == 14 and a in hi:
            out.add((hi[a] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF)
        elif op == 24 and d in hi:  # ori rA,rS,lo: rS is d's field
            out.add(hi[d] | imm)
    return out


def by_rules(tfun, twords, final, unnamed):
    """Names SDK functions the reference game may lack (an SDK library links only what a game
    calls), from what they call. The runtime replaces them (runtime/guest/sdk), so a game whose
    own copy runs instead sees state the runtime never set up (e.g. CARD: no card)."""
    def name(x):
        n = final.get(x, tfun[x][0] if x in tfun else None)
        return None if n is None or unnamed(n) else n
    by_name = {}
    for x in tfun:
        if name(x):
            by_name.setdefault(name(x), x)
    used = {name(x) for x in tfun if name(x)}

    def has(s, *names):
        return all(n in s for n in names)

    def rule(t, w):
        seq = [name(x) for _, x in calls(w, t) if x in tfun and x != t]
        known = [n for n in seq if n]
        s = set(known)
        size = tfun[t][1]
        # the synchronous wrapper of an asynchronous CARD call: XAsync(..., __CARDSyncCallback), __CARDSync
        if len(seq) == 2 and seq[1] == '__CARDSync' and seq[0] and seq[0].endswith('Async') and size <= 0x80:
            cb = by_name.get('__CARDSyncCallback')
            if cb is None or cb in address_loads(w):
                return 'CARDFormat' if seq[0] == '__CARDFormatRegionAsync' else seq[0][:-5]
        if s == {'__CARDGetControlBlock', '__CARDGetDirBlock', '__CARDAccess', '__CARDIsPublic', 'memcpy',
                 '__CARDPutControlBlock'} and known.count('memcpy') == 1:
            return '__CARDGetStatusEx'
        if has(s, '__CARDGetDirBlock', '__CARDCompareFileName', '__CARDAccess', 'strncpy', '__CARDUpdateDir'):
            return 'CARDRenameAsync'
        if has(s, '__CARDGetControlBlock', '__CARDGetDirBlock', '__CARDAccess', '__CARDUpdateDir', 'memcmp', 'memcpy') \
                and not s & {'__CARDCompareFileName', 'UpdateIconOffsets', '__CARDUpdateIconOffsets', 'strncpy',
                             '__CARDIsOpened', '__CARDGetFileNo', 'OSGetTime'}:
            return '__CARDSetStatusExAsync'
        if has(s, '__CARDGetFileNo', '__CARDIsOpened', '__CARDUpdateDir'):
            return 'CARDDeleteAsync'
        if has(s, '__CARDGetDirBlock', '__CARDAccess', '__CARDIsOpened', '__CARDUpdateDir'):
            return 'CARDFastDeleteAsync'
        if known == ['__CARDGetStatusEx', '__CARDSetStatusExAsync']:
            return 'CARDSetAttributesAsync'
        if known == ['__CARDGetStatusEx'] and size <= 0x60:
            return 'CARDGetAttributes'
        if '__CARDSetDiskID' in s and 'OSRegisterResetFunction' in s:
            return 'CARDInit'
        # cards[chan].xferred: mulli rX,r3,sizeof(CARDControl) ... lwz r3,0xB8(r3); blr
        if not seq and len(w) == 6 and (w[0] & 0xFC1FFFFF) == 0x1C030110 and 0x806300B8 in w and w[-1] == 0x4E800020:
            return 'CARDGetXferredBytes'
        return None

    total = 0
    for _ in range(4):  # a name found makes callers' rules match (sync wrappers)
        found = 0
        for t, w in twords.items():
            if name(t):
                continue
            n = rule(t, w)
            if n and n not in used:
                final[t] = n
                used.add(n)
                by_name[n] = t
                found += 1
        total += found
        if not found:
            break
    return total


def card_leftovers(tfun, final, unnamed):
    """unnamed functions between the first and last named CARD function"""
    named = [x for x in tfun if not unnamed(final.get(x, tfun[x][0]))
             and re.match(r'^(__)?CARD', final.get(x, tfun[x][0]))]
    if not named:
        return []
    lo, hi = min(named), max(named)
    return sorted(x for x in tfun if lo < x < hi and unnamed(final.get(x, tfun[x][0])))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref-dol', required=True)
    ap.add_argument('--ref-symbols', required=True)
    ap.add_argument('--dol', required=True)
    ap.add_argument('--symbols', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--min-insns', type=int, default=4)
    ap.add_argument('--max-gap', type=int, default=16, help='longest run of functions named by place')
    ap.add_argument('--verbose', action='store_true')
    a = ap.parse_args()

    rdol, tdol = Dol(open(a.ref_dol, 'rb').read()), Dol(open(a.dol, 'rb').read())
    rfun, tfun = read_functions(a.ref_symbols), read_functions(a.symbols)
    unnamed = lambda n: re.match(r'^(fn|func|sub|lbl)_[0-9A-Fa-f_]+$', n) is not None

    rwords, twords = {}, {}
    by_sig = collections.defaultdict(list)
    for addr, (name, size) in rfun.items():
        w = rdol.words(addr, size)
        if w is None or unnamed(name):
            continue
        rwords[addr] = w
        by_sig[signature(w)].append(addr)
    for addr, (name, size) in tfun.items():
        w = tdol.words(addr, size)
        if w is not None:
            twords[addr] = w

    names = {}          # target addr -> name
    pairs = []          # (target addr, ref addr) with the same code
    agree = disagree = 0
    for addr, w in twords.items():
        refs = by_sig.get(signature(w), [])
        if len(refs) != 1 or len(w) < a.min_insns:
            continue
        r = refs[0]
        pairs.append((addr, r))
        tname, rname = tfun[addr][0], rfun[r][0]
        if unnamed(tname):
            names[addr] = rname
        elif tname == rname:
            agree += 1
        else:
            disagree += 1
            if a.verbose:
                print('  differs: %08X %s (ref %s, %d instructions)' % (addr, tname, rname, len(w)))
    direct = len(names)

    # along the calls of matched pairs (the same code calls the same functions)
    seen = set()
    while pairs:
        t, r = pairs.pop()
        if (t, r) in seen:
            continue
        seen.add((t, r))
        tc, rc = calls(twords[t], t), calls(rwords[r], r)
        if len(tc) != len(rc):
            continue
        for (_, tt), (_, rt) in zip(tc, rc):
            if tt not in tfun or rt not in rfun or unnamed(rfun[rt][0]):
                continue
            if unnamed(tfun[tt][0]) and tt not in names:
                names[tt] = rfun[rt][0]
            if tt in twords and rt in rwords and len(twords[tt]) == len(rwords[rt]) and \
                    signature(twords[tt]) == signature(rwords[rt]):
                pairs.append((tt, rt))

    # one name, one function
    count = collections.Counter(names.values())
    taken = {n for addr, (n, _) in tfun.items() if not unnamed(n)}
    final = {addr: n for addr, n in names.items() if count[n] == 1 and n not in taken}

    # gaps: libraries are linked object by object in the same order in every game, so between two
    # functions known in both, the same number of functions of similar sizes are the same ones
    # (an SDK version changed their code, not their place)
    rby_name = collections.defaultdict(list)
    for addr, (n, _) in rfun.items():
        rby_name[n].append(addr)
    known = []
    for addr, (n, _) in tfun.items():
        n = final.get(addr, n)
        if not unnamed(n) and len(rby_name.get(n, [])) == 1:
            known.append((addr, rby_name[n][0]))
    known.sort()
    # the longest run of anchors in the same order in both games
    best, prev = [], []
    for k, (t, r) in enumerate(known):
        best.append(1)
        prev.append(-1)
        for j in range(max(0, k - 200), k):
            if known[j][1] < r and best[j] + 1 > best[k]:
                best[k], prev[k] = best[j] + 1, j
    k = max(range(len(best)), key=lambda i: best[i]) if best else -1
    anchors = []
    while k >= 0:
        anchors.append(known[k])
        k = prev[k]
    anchors.reverse()
    tsorted, rsorted = sorted(tfun), sorted(rfun)
    import bisect

    def tname_of(addr):
        n = final.get(addr, tfun[addr][0] if addr in tfun else None)
        return None if n is None or unnamed(n) else n

    def evidence(t, r):
        """a placed pair is believed when its calls agree with the reference's (at least one
        known callee the same, none different), or, calling nothing, its size is nearly the same"""
        if t not in twords or r not in rwords:
            return False
        tc = [tname_of(x) for _, x in calls(twords[t], t)]
        rc = [rfun[x][0] if x in rfun and not unnamed(rfun[x][0]) else None for _, x in calls(rwords[r], r)]
        if not tc and not rc:
            return abs(len(twords[t]) - len(rwords[r])) <= max(2, len(rwords[r]) // 10)
        same = sum(1 for n in tc if n and n in rc)
        other = sum(1 for n in tc if n and rc and n not in rc)
        return same >= 1 and other == 0
    gapped = 0
    gap_check = collections.Counter()
    used = set(final.values()) | taken
    for (t0, r0), (t1, r1) in zip(anchors, anchors[1:]):
        ti = tsorted[bisect.bisect_right(tsorted, t0):bisect.bisect_left(tsorted, t1)]
        ri = rsorted[bisect.bisect_right(rsorted, r0):bisect.bisect_left(rsorted, r1)]
        if not ti or len(ti) != len(ri) or len(ti) > a.max_gap:
            continue
        sizes_ok = all(0.6 <= tfun[t][1] / max(rfun[r][1], 4) <= 1.6 for t, r in zip(ti, ri))
        if not sizes_ok:
            continue
        for t, r in zip(ti, ri):
            if unnamed(tfun[t][0]) and not evidence(t, r):
                continue
            tn, rn = tfun[t][0], rfun[r][0]
            if not unnamed(tn) and not unnamed(rn):
                gap_check[tn == rn or tn.startswith(rn) or rn.startswith(tn)] += 1
                if a.verbose and not (tn == rn or tn.startswith(rn) or rn.startswith(tn)):
                    print('  gap differs: %08X %s (ref %s)' % (t, tn, rn))
            if unnamed(tn) and t not in final and not unnamed(rn) and rn not in used:
                final[t] = rn
                used.add(rn)
                gapped += 1
    # callees: a function that calls the same named functions, in the same order, as exactly one
    # reference function (at least two of them, similar size) is that function; repeated, as
    # every name found makes more callees known
    by_callees = collections.defaultdict(list)
    for r, w in rwords.items():
        seq = tuple(rfun[x][0] if x in rfun and not unnamed(rfun[x][0]) else '?' for _, x in calls(w, r))
        if sum(1 for n in seq if n != '?') >= 2:
            by_callees[seq].append(r)
    def callee_cands(t, w):
        seq = tuple(tname_of(x) or '?' for _, x in calls(w, t))
        if sum(1 for n in seq if n != '?') < 2:
            return []
        cands = [r for s, rs in by_callees.items() if len(s) == len(seq) and
                 all(x == '?' or x == y for x, y in zip(seq, s)) for r in rs]
        return [r for r in cands if 0.5 <= tfun[t][1] / max(rfun[r][1], 4) <= 2.0]

    check = collections.Counter()
    for t, w in twords.items():
        tn = tfun[t][0]
        if unnamed(tn):
            continue
        c = callee_cands(t, w)
        if len(c) == 1:
            rn = rfun[c[0]][0]
            ok = tn == rn or tn.startswith(rn) or rn.startswith(tn)
            check[ok] += 1
            if a.verbose and not ok:
                print('  callees differ: %08X %s (ref %s)' % (t, tn, rn))
    print('gcn_sdkmatch: by callees, against names the decomp has: %d agree, %d disagree' % (check[True], check[False]))
    by_callee_count = 0
    for _ in range(4):
        found = 0
        for t, w in twords.items():
            if not unnamed(tfun[t][0]) or t in final:
                continue
            seq = tuple(tname_of(x) or '?' for _, x in calls(w, t))
            if sum(1 for n in seq if n != '?') < 2:
                continue
            # '?' in the target may be any function; in the reference it must be one too
            cands = [r for s, rs in by_callees.items() if len(s) == len(seq) and
                     all(x == '?' or x == y for x, y in zip(seq, s)) for r in rs]
            cands = [r for r in cands if 0.5 <= tfun[t][1] / max(rfun[r][1], 4) <= 2.0]
            if len(cands) == 1 and rfun[cands[0]][0] not in used:
                final[t] = rfun[cands[0]][0]
                used.add(final[t])
                found += 1
        by_callee_count += found
        if not found:
            break
    print('gcn_sdkmatch: %d more named by the functions they call' % by_callee_count)

    ruled = by_rules(tfun, twords, final, unnamed)
    print('gcn_sdkmatch: %d more named by SDK rules (functions the reference game does not have)' % ruled)
    left = card_leftovers(tfun, final, unnamed)
    if left:
        print('gcn_sdkmatch: unnamed functions inside the CARD library (the game\'s own code runs for them, '
              'which finds no card): ' + ', '.join('%08X' % x for x in left))

    with open(a.out, 'w', newline='\n') as f:
        f.write('# gcn_sdkmatch.py: names for unnamed functions, matched against %s\n' % a.ref_symbols.replace('\\', '/'))
        for addr in sorted(final):
            f.write('0x%08X %s\n' % (addr, final[addr]))
    print('gcn_sdkmatch: %d named (%d by code, %d along calls, %d by their place between known ones), '
          'check against names the decomp has: %d agree, %d disagree'
          % (len(final), direct, len(names) - direct, gapped, agree, disagree))
    print('gcn_sdkmatch: by place, against names the decomp has: %d agree, %d disagree'
          % (gap_check[True], gap_check[False]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
