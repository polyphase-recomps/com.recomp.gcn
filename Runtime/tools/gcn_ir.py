#!/usr/bin/env python3
"""Turn big-endian guest IR into WebAssembly IR that keeps GameCube memory big-endian.

The game's C is compiled by clang for a 32-bit big-endian target (armeb: same type sizes,
alignments and bitfield order as the GameCube's PowerPC EABI), so every optimisation clang
does already assumes big-endian memory. This tool then retargets the module to wasm32
(the same data layout apart from byte order) and keeps memory big-endian explicitly:

  * every load / store wider than a byte gets an llvm.bswap (floats and pointers through
    their integer bits), so data from the disc is used as it is, on any host;
  * global initialisers are rewritten as big-endian bytes; pointers in them (relocations)
    stay pointers and are listed in section gcn_befix, which the runtime byte-swaps once
    at boot, after the data is in place;
  * variadic functions take their extra arguments as a pointer to a big-endian buffer the
    caller fills (the layout an AAPCS va_list walks), instead of the wasm convention;
  * volatile accesses become calls to the host (__gcn_vload* / __gcn_vstore*), which knows
    the hardware registers (the GX write-gather pipe at 0xCC008000 and friends).

Usage: gcn_ir.py in.ll out.ll [--unit-id N]
"""
import re
import struct
import sys

WGPIPE_PTR = 'inttoptr (i32 -872382464 to ptr)'  # 0xCC008000, the GX write-gather pipe
WASM_DATALAYOUT ="e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-i128:128-n32:64-S128-ni:1:10:20"
WASM_TRIPLE = "wasm32-unknown-unknown"


class IRError(Exception):
    pass


# ---------------------------------------------------------------------------------------
# Types
# ---------------------------------------------------------------------------------------
# A type is ('int', bits) | ('float',) | ('double',) | ('ptr',) | ('array', n, elem)
#         | ('struct', [fields], packed) | ('named', name) | ('void',) | ('vector', n, elem)

class TypeTable:
    def __init__(self):
        self.named = {}       # name -> type (None = opaque)

    def resolve(self, t):
        while t[0] == 'named':
            r = self.named.get(t[1])
            if r is None:
                raise IRError("opaque or unknown type %" + t[1])
            t = r
        return t

    def size_align(self, t):
        t = self.resolve(t)
        k = t[0]
        if k == 'int':
            n = (t[1] + 7) // 8
            if n <= 1:
                return 1, 1
            if n <= 2:
                return 2, 2
            if n <= 4:
                return 4, 4
            return 8, 8 if n <= 8 else 16
        if k == 'float':
            return 4, 4
        if k == 'double':
            return 8, 8
        if k == 'ptr':
            return 4, 4
        if k == 'array' or k == 'vector':
            s, a = self.size_align(t[2])
            return s * t[1], a
        if k == 'struct':
            off, al = 0, 1
            for f in t[1]:
                s, a = self.size_align(f)
                if t[2]:
                    a = 1
                off = (off + a - 1) // a * a + s
                al = max(al, a)
            return (off + al - 1) // al * al, al
        raise IRError("no layout for type " + str(t))

    def field_offsets(self, t):
        t = self.resolve(t)
        offs, off = [], 0
        for f in t[1]:
            s, a = self.size_align(f)
            if t[2]:
                a = 1
            off = (off + a - 1) // a * a
            offs.append(off)
            off += s
        return offs


class Parser:
    """Minimal recursive descent over LLVM IR type and constant syntax."""

    def __init__(self, text, pos=0):
        self.s = text
        self.p = pos

    def ws(self):
        while self.p < len(self.s) and self.s[self.p] in ' \t':
            self.p += 1

    def peek(self, lit):
        self.ws()
        return self.s.startswith(lit, self.p)

    def eat(self, lit):
        self.ws()
        if not self.s.startswith(lit, self.p):
            raise IRError("expected %r at %r" % (lit, self.s[self.p:self.p + 40]))
        self.p += len(lit)

    def word(self):
        self.ws()
        m = re.compile(r'[%@]?(?:"(?:[^"\\]|\\.)*"|[-+\w.$]+)').match(self.s, self.p)
        if not m:
            raise IRError("expected word at %r" % self.s[self.p:self.p + 40])
        self.p = m.end()
        return m.group(0)

    def type(self):
        self.ws()
        s, p = self.s, self.p
        if s.startswith('<{', p):
            self.p += 2
            fields = self.type_list('}>')
            t = ('struct', fields, True)
        elif s[p] == '{':
            self.p += 1
            fields = self.type_list('}')
            t = ('struct', fields, False)
        elif s[p] == '[':
            self.p += 1
            n = int(self.word())
            self.eat('x')
            e = self.type()
            self.eat(']')
            t = ('array', n, e)
        elif s[p] == '<':
            self.p += 1
            n = int(self.word())
            self.eat('x')
            e = self.type()
            self.eat('>')
            t = ('vector', n, e)
        else:
            w = self.word()
            if w.startswith('%'):
                t = ('named', w[1:].strip('"'))
            elif w.startswith('i') and w[1:].isdigit():
                t = ('int', int(w[1:]))
            elif w == 'float':
                t = ('float',)
            elif w == 'double':
                t = ('double',)
            elif w == 'ptr':
                t = ('ptr',)
            elif w == 'void':
                t = ('void',)
            else:
                raise IRError("unknown type " + w)
        # function types never appear in data; pointer suffixes are gone with opaque pointers
        return t

    def type_list(self, close):
        fields = []
        if self.peek(close):
            self.eat(close)
            return fields
        while True:
            fields.append(self.type())
            if self.peek(','):
                self.eat(',')
                continue
            self.eat(close)
            return fields

    def balanced(self):
        """Text of a parenthesised constant expression, starting at '('."""
        self.ws()
        start = self.p
        depth = 0
        while True:
            c = self.s[self.p]
            if c == '"':
                self.p += 1
                while self.s[self.p] != '"':
                    self.p += 2 if self.s[self.p] == '\\' else 1
            elif c in '([{<':
                depth += 1
            elif c in ')]}>':
                depth -= 1
                if depth == 0:
                    self.p += 1
                    return self.s[start:self.p]
            self.p += 1


# Constant values:
#   ('zero',) ('undef',) ('int', v) ('fbits', bits, nbytes) ('bytes', b) ('agg', [values])
#   ('expr', text)  -- relocatable (pointer or pointer-derived), emitted as is
def parse_value(ps, t, types):
    ps.ws()
    s, p = ps.s, ps.p
    rt = types.resolve(t) if t[0] == 'named' else t
    for lit, v in (('zeroinitializer', ('zero',)), ('undef', ('undef',)), ('poison', ('undef',)),
                   ('null', ('zero',)), ('true', ('int', 1)), ('false', ('int', 0)), ('none', ('zero',))):
        if s.startswith(lit, p) and not (s[p + len(lit):p + len(lit) + 1].isalnum()):
            ps.p += len(lit)
            return v
    if s.startswith('c"', p):
        ps.p += 1
        word = ps.word()
        body = word[1:-1]
        out = bytearray()
        i = 0
        while i < len(body):
            if body[i] == '\\':
                if body[i + 1] == '\\':
                    out.append(0x5C)
                    i += 2
                else:
                    out.append(int(body[i + 1:i + 3], 16))
                    i += 3
            else:
                out.append(ord(body[i]))
                i += 1
        return ('bytes', bytes(out))
    if s[p] in '[{<':
        packed = s.startswith('<{', p)
        close = {'[': ']', '{': '}', '<': '>'}[s[p]]
        if packed:
            ps.p += 2
            close = '}>'
        else:
            ps.p += 1
        if rt[0] == 'array' or rt[0] == 'vector':
            etypes = None
        else:
            etypes = rt[1]
        vals = []
        i = 0
        if ps.peek(close):
            ps.eat(close)
            return ('agg', vals)
        while True:
            et = ps.type()
            vals.append(parse_value(ps, et, types))
            i += 1
            if ps.peek(','):
                ps.eat(',')
                continue
            ps.eat(close)
            return ('agg', vals)
    # constant expressions and symbols: relocatable
    m = re.compile(r'(getelementptr|bitcast|inttoptr|ptrtoint|addrspacecast|add|sub|trunc|zext|sext|dso_local_equivalent|no_cfi)\b').match(s, p)
    if m:
        ps.p = m.end()
        ps.ws()
        # optional keywords before the operand list
        while True:
            m2 = re.compile(r'(inbounds|nuw|nsw|inrange\([^)]*\))\s*').match(ps.s, ps.p)
            if not m2:
                break
            ps.p = m2.end()
        expr = s[p:m.end()] + ' ' + ps.s[m.end():ps.p].strip() + ' ' + ps.balanced()
        return fold_expr(expr.replace('  ', ' '), t, types)
    if s[p] == '@':
        return ('expr', ps.word())
    w = ps.word()
    if rt[0] == 'int':
        return ('int', int(w, 0) if w.startswith(('0x', '-0x')) else int(w))
    if rt[0] in ('float', 'double'):
        return ('fbits',) + float_bits(w, rt[0])
    raise IRError("cannot parse constant %r for %s" % (w, t))


def fold_expr(text, t, types):
    """inttoptr of a plain integer is just that integer; everything else stays relocatable."""
    m = re.match(r'inttoptr \(i(\d+) (-?\d+) to ptr\)$', text)
    if m:
        return ('int', int(m.group(2)))
    return ('expr', text)


def float_bits(w, kind):
    if w.startswith('0x'):
        d = int(w, 16)
        if kind == 'double':
            return d, 8
        # a float written as the double that has its value
        sign = d >> 63
        exp = (d >> 52) & 0x7FF
        mant = d & ((1 << 52) - 1)
        if exp == 0x7FF:
            return (sign << 31) | (0xFF << 23) | (mant >> 29), 4
        f = struct.unpack('>d', d.to_bytes(8, 'big'))[0]
        return int.from_bytes(struct.pack('>f', f), 'big'), 4
    f = float(w)
    if kind == 'double':
        return int.from_bytes(struct.pack('>d', f), 'big'), 8
    return int.from_bytes(struct.pack('>f', f), 'big'), 4


class Image:
    """Big-endian bytes of one initialiser plus its relocatable slots."""

    def __init__(self, size):
        self.buf = bytearray(size)
        self.relocs = []   # (offset, type_text, expr_text)

    def put(self, t, v, off, types):
        rt = types.resolve(t)
        k = v[0]
        if k in ('zero', 'undef'):
            return
        if k == 'expr':
            size = types.size_align(rt)[0]
            if rt[0] == 'ptr':
                tt = 'ptr'
            elif rt[0] == 'int' and size == 4:
                tt = 'i32'
            else:
                raise IRError("relocatable constant of type %s: %s" % (rt, v[1]))
            self.relocs.append((off, tt, v[1]))
            return
        if k == 'int':
            size = types.size_align(rt)[0]
            if rt[0] == 'int' and rt[1] == 1:
                size = 1
            val = v[1] & ((1 << (size * 8)) - 1)
            nbytes = (rt[1] + 7) // 8 if rt[0] == 'int' else size
            # integers narrower than their storage (i24) sit in the leading bytes
            self.buf[off:off + nbytes] = (val & ((1 << (nbytes * 8)) - 1)).to_bytes(nbytes, 'big')
            return
        if k == 'fbits':
            self.buf[off:off + v[2]] = v[1].to_bytes(v[2], 'big')
            return
        if k == 'bytes':
            self.buf[off:off + len(v[1])] = v[1]
            return
        if k == 'agg':
            if rt[0] in ('array', 'vector'):
                es = types.size_align(rt[2])[0]
                for i, ev in enumerate(v[1]):
                    self.put(rt[2], ev, off + i * es, types)
            else:
                for ft, fo, fv in zip(rt[1], types.field_offsets(rt), v[1]):
                    self.put(ft, fv, off + fo, types)
            return
        raise IRError("bad value " + str(v))


def ll_bytes(b):
    out = []
    for c in b:
        if 32 <= c < 127 and c not in (0x22, 0x5C):
            out.append(chr(c))
        else:
            out.append('\\%02X' % c)
    return 'c"' + ''.join(out) + '"'


# ---------------------------------------------------------------------------------------
# Globals
# ---------------------------------------------------------------------------------------
GLOBAL_RE = re.compile(r'^(@(?:"(?:[^"\\]|\\.)*"|[-\w.$]+)) = (.*?)\b(global|constant) (.*)$')


def rewrite_global(line, types, fixups):
    m = GLOBAL_RE.match(line)
    if not m:
        return line
    name, prefix, kind, rest = m.groups()
    # C tentative definitions (-fcommon, as Metrowerks merges them): wasm has no common
    # symbols, weak ones merge the same way
    if re.search(r'\bcommon\b', prefix):
        prefix = re.sub(r'\bcommon\b', 'weak', prefix)
        line = '%s = %s%s %s' % (name, prefix, kind, rest)
    if re.search(r'\b(external|extern_weak|appending)\b', prefix) or name.startswith('@llvm.'):
        return line
    if 'section "llvm.metadata"' in rest:
        return line
    ps = Parser(rest)
    t = ps.type()
    val = parse_value(ps, t, types)
    tail = rest[ps.p:]
    rt = types.resolve(t)
    # Metrowerks aligns every global of 4 bytes or more to 4: games address neighbouring
    # strings and tables from one symbol plus an offset, which needs the same layout
    try:
        gsize = types.size_align(t)[0]
    except IRError:
        gsize = 0
    if gsize >= 4:
        ma = re.search(r', align (\d+)', tail)
        if ma and int(ma.group(1)) < 4:
            tail = tail[:ma.start()] + ', align 4' + tail[ma.end():]
        elif not ma:
            tail = tail + ', align 4' if not re.search(r', !', tail) else re.sub(r'(, !)', r', align 4\1', tail, 1)
        line = '%s = %s%s %s' % (name, prefix, kind, rest[:ps.p] + tail)
    # already byte-shaped or all zeros: nothing to do
    if val[0] in ('zero', 'undef'):
        return line
    if rt[0] == 'array' and types.resolve(rt[2]) == ('int', 8):
        return line
    size = types.size_align(t)[0]
    img = Image(size)
    img.put(t, val, 0, types)
    if not img.relocs:
        return '%s = %s%s [%d x i8] %s%s' % (name, prefix, kind, size, ll_bytes(img.buf), tail)
    # bytes runs and pointer slots, as a packed struct
    ftypes, fvals, pos = [], [], 0
    for off, tt, expr in sorted(img.relocs):
        if off > pos:
            ftypes.append('[%d x i8]' % (off - pos))
            fvals.append('[%d x i8] %s' % (off - pos, ll_bytes(img.buf[pos:off])))
        ftypes.append(tt)
        fvals.append('%s %s' % (tt, expr))
        fixups.append((name, off))
        pos = off + 4
    if pos < size:
        ftypes.append('[%d x i8]' % (size - pos))
        fvals.append('[%d x i8] %s' % (size - pos, ll_bytes(img.buf[pos:])))
    return '%s = %s%s <{ %s }> <{ %s }>%s' % (name, prefix, kind, ', '.join(ftypes), ', '.join(fvals), tail)


# ---------------------------------------------------------------------------------------
# Function bodies
# ---------------------------------------------------------------------------------------
VAL = r'(?:%(?:"(?:[^"\\]|\\.)*"|[-\w.$]+))'
LOAD_RE = re.compile(r'^(\s*)(' + VAL + r') = load (atomic )?(volatile )?([^,]+), ptr (.+?)(, align (\d+))?((?:, !.*)?)$')
STORE_RE = re.compile(r'^(\s*)store (atomic )?(volatile )?(.+)$')

SCALAR = {'i16': 16, 'i32': 32, 'i64': 64, 'float': 32, 'double': 64, 'ptr': 32}


def split_top(text):
    """Split on commas that are not nested in brackets or strings."""
    parts, depth, cur, i = [], 0, [], 0
    while i < len(text):
        c = text[i]
        if c == '"':
            j = i + 1
            while text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if c in '([{<':
            depth += 1
        elif c in ')]}>':
            depth -= 1
        if c == ',' and depth == 0:
            parts.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if cur and ''.join(cur).strip():
        parts.append(''.join(cur).strip())
    return parts


def type_text(t):
    k = t[0]
    if k == 'int':
        return 'i%d' % t[1]
    if k in ('float', 'double', 'ptr', 'void'):
        return k
    if k == 'named':
        return '%' + t[1]
    if k == 'array':
        return '[%d x %s]' % (t[1], type_text(t[2]))
    if k == 'vector':
        return '<%d x %s>' % (t[1], type_text(t[2]))
    inner = ', '.join(type_text(f) for f in t[1])
    return ('<{ %s }>' if t[2] else '{ %s }') % inner


class FuncRewriter:
    def __init__(self, needed, types=None):
        self.n = 0
        self.needed = needed    # declarations to add (name -> text)
        self.types = types or TypeTable()
        self.locals = set()     # allocas of the current function
        self.knr = {}           # K&R callee -> (return type, argument types, variadic) of its first call
        self.knr_names = set()  # functions the module declares without a prototype
        self.variadic = {}      # program-wide: variadic function -> number of fixed parameters
        self.fix = {}           # declarations that disagree with the definition
        self.gpr_params = []    # integer parameters of the function being rewritten

    def tmp(self):
        self.n += 1
        return '%%gcn.%d' % self.n

    def bswap(self, bits, val, ind, out):
        """Emit a byte swap of iN `val`; returns the result name."""
        if bits in (16, 32, 64):
            r = self.tmp()
            out.append('%s%s = call i%d @llvm.bswap.i%d(i%d %s)' % (ind, r, bits, bits, bits, val))
            self.needed['llvm.bswap.i%d' % bits] = 'declare i%d @llvm.bswap.i%d(i%d)' % (bits, bits, bits)
            return r
        if bits % 8 or bits > 64:
            raise IRError("cannot byte-swap i%d" % bits)
        wide = 32 if bits <= 32 else 64
        a, b, c, r = self.tmp(), self.tmp(), self.tmp(), self.tmp()
        out.append('%s%s = zext i%d %s to i%d' % (ind, a, bits, val, wide))
        out.append('%s%s = call i%d @llvm.bswap.i%d(i%d %s)' % (ind, b, wide, wide, wide, a))
        out.append('%s%s = lshr i%d %s, %d' % (ind, c, wide, b, wide - bits))
        out.append('%s%s = trunc i%d %s to i%d' % (ind, r, wide, c, bits))
        self.needed['llvm.bswap.i%d' % wide] = 'declare i%d @llvm.bswap.i%d(i%d)' % (wide, wide, wide)
        return r

    def load(self, m, out):
        ind, res, atomic, vol, ty, ptr, _, align, meta = m.groups()
        ty = ty.strip()
        if vol and ptr.strip() in self.locals:
            vol = None
        if atomic:
            raise IRError("atomic load")
        if ty in ('i8', 'i1'):
            if vol:
                self.vcall_load(ind, res, ty, ptr, out)
                return
            out.append(m.group(0))
            return
        if ty in SCALAR or re.match(r'i\d+$', ty):
            bits = SCALAR.get(ty) or int(ty[1:])
            ity = 'i%d' % bits
            if vol:
                self.vcall_load(ind, res, ty, ptr, out)
                return
            raw = self.tmp()
            out.append('%s%s = load %s, ptr %s%s%s' % (ind, raw, ity, ptr, ', align ' + align if align else '', meta))
            if ty in ('float', 'double', 'ptr'):
                sw = self.bswap(bits, raw, ind, out)
                op = 'inttoptr' if ty == 'ptr' else 'bitcast'
                out.append('%s%s = %s %s %s to %s' % (ind, res, op, ity, sw, ty))
            else:
                sw = self.bswap(bits, raw, ind, out)
                out.append('%s%s = add %s %s, 0' % (ind, res, ity, sw))
            return
        if ty[0] in '[{<%' and not vol:
            self.aggregate_load(ind, res, ty, ptr, out)
            return
        raise IRError("load of type " + ty)

    # Aggregates (unoptimised IR stores coerced struct arguments whole): one scalar access
    # per element, each of which then gets its byte swap.
    def elements(self, ty):
        t = Parser(ty).type()
        rt = self.types.resolve(t)
        if rt[0] in ('array', 'vector'):
            es = self.types.size_align(rt[2])[0]
            et = type_text(rt[2])
            return [(i, i * es, et) for i in range(rt[1])], rt[0] == 'vector'
        offs = self.types.field_offsets(rt)
        return [(i, o, type_text(f)) for i, (o, f) in enumerate(zip(offs, rt[1]))], False

    def aggregate_load(self, ind, res, ty, ptr, out):
        elems, is_vec = self.elements(ty)
        acc = 'poison'
        for i, off, et in elems:
            g, v = self.tmp(), self.tmp()
            out.append('%s%s = getelementptr inbounds i8, ptr %s, i32 %d' % (ind, g, ptr, off))
            self.load(LOAD_RE.match('%s%s = load %s, ptr %s, align 1' % (ind, v, et, g)), out)
            nxt = res if i == len(elems) - 1 else self.tmp()
            if is_vec:
                out.append('%s%s = insertelement %s %s, %s %s, i32 %d' % (ind, nxt, ty, acc, et, v, i))
            else:
                out.append('%s%s = insertvalue %s %s, %s %s, %d' % (ind, nxt, ty, acc, et, v, i))
            acc = nxt
        if not elems:
            out.append('%s%s = add i32 0, 0' % (ind, res))

    def aggregate_store(self, ind, ty, val, ptr, out):
        elems, is_vec = self.elements(ty)
        for i, off, et in elems:
            g, v = self.tmp(), self.tmp()
            if is_vec:
                out.append('%s%s = extractelement %s %s, i32 %d' % (ind, v, ty, val, i))
            else:
                out.append('%s%s = extractvalue %s %s, %d' % (ind, v, ty, val, i))
            out.append('%s%s = getelementptr inbounds i8, ptr %s, i32 %d' % (ind, g, ptr, off))
            self.store(STORE_RE.match('%sstore %s %s, ptr %s, align 1' % (ind, et, v, g)), out)

    def wgpipe_store(self, ind, ty, val, out):
        """A store to the GX write-gather pipe (0xCC008000), volatile or not (some decomp
        code forgets the qualifier): __gcn_wgpipeN(value), which hosts can turn into a few
        inline instructions for vertex data. False for sizes the pipe import doesn't take."""
        bits = {'i1': 8, 'i8': 8, 'i16': 16, 'i32': 32, 'float': 32, 'ptr': 32}.get(ty)
        if bits is None:
            return False
        iv = val
        if ty == 'float':
            iv = self.tmp()
            out.append('%s%s = bitcast float %s to i32' % (ind, iv, val))
        elif ty == 'ptr':
            iv = self.tmp()
            out.append('%s%s = ptrtoint ptr %s to i32' % (ind, iv, val))
        elif ty == 'i1':
            iv = self.tmp()
            out.append('%s%s = zext i1 %s to i8' % (ind, iv, val))
        fn = '__gcn_wgpipe%d' % bits
        self.needed[fn] = 'declare void @%s(i%d)' % (fn, bits)
        out.append('%scall void @%s(i%d %s)' % (ind, fn, bits, iv))
        return True

    def vcall_load(self, ind, res, ty, ptr, out):
        bits = {'i1': 8, 'i8': 8, 'i16': 16, 'i32': 32, 'float': 32, 'ptr': 32}.get(ty)
        if bits is None:
            raise IRError("volatile load of " + ty)
        fn = '__gcn_vload%d' % bits
        self.needed[fn] = 'declare i%d @%s(ptr)' % (bits, fn)
        if ty in ('i8', 'i16', 'i32'):
            out.append('%s%s = call %s @%s(ptr %s)' % (ind, res, ty, fn, ptr))
            return
        r = self.tmp()
        out.append('%s%s = call i%d @%s(ptr %s)' % (ind, r, bits, fn, ptr))
        if ty == 'i1':
            out.append('%s%s = trunc i8 %s to i1' % (ind, res, r))
        elif ty == 'float':
            out.append('%s%s = bitcast i32 %s to float' % (ind, res, r))
        else:
            out.append('%s%s = inttoptr i32 %s to ptr' % (ind, res, r))

    def store(self, m, out):
        ind, atomic, vol, rest = m.groups()
        if atomic:
            raise IRError("atomic store")
        mloc = re.search(r', ptr (' + VAL + r')(?:, align \d+)?(?:, !.*)?$', rest)
        if vol and mloc and mloc.group(1) in self.locals:
            vol = None  # volatile local: a compiler trick (force a rounding store), not hardware
        meta = ''
        mm = re.search(r'(, !.*)$', rest)
        if mm:
            meta = mm.group(1)
            rest = rest[:mm.start()]
        align = ''
        ma = re.search(r', align (\d+)$', rest)
        if ma:
            align = ma.group(1)
            rest = rest[:ma.start()]
        parts = split_top(rest)
        if len(parts) != 2 or not parts[1].startswith('ptr '):
            raise IRError("cannot parse store: " + m.group(0))
        tv, ptr = parts[0], parts[1][4:]
        ps = Parser(tv)
        ty_t = ps.type()
        val = tv[ps.p:].strip()
        ty = tv[:ps.p].strip()
        if ptr.strip() == WGPIPE_PTR and self.wgpipe_store(ind, ty, val, out):
            return
        if ty in ('i8', 'i1') and not vol:
            out.append(m.group(0))
            return
        if ty[0] in '[{<%' and not vol:
            if val in ('zeroinitializer', 'undef', 'poison'):
                out.append(m.group(0))
                return
            self.aggregate_store(ind, ty, val, ptr, out)
            return
        if not (ty in SCALAR or re.match(r'i\d+$', ty)):
            raise IRError("store of type " + ty)
        bits = SCALAR.get(ty) or int(ty[1:])
        ity = 'i%d' % bits
        iv = val
        if ty in ('float', 'double'):
            iv = self.tmp()
            out.append('%s%s = bitcast %s %s to %s' % (ind, iv, ty, val, ity))
        elif ty == 'ptr':
            iv = self.tmp()
            out.append('%s%s = ptrtoint ptr %s to i32' % (ind, iv, val))
        if vol:
            vb = 8 if bits <= 8 else bits
            if vb not in (8, 16, 32):
                raise IRError("volatile store of " + ty)
            if ty == 'i1':
                iv2 = self.tmp()
                out.append('%s%s = zext i1 %s to i8' % (ind, iv2, val))
                iv = iv2
            fn = '__gcn_vstore%d' % vb
            self.needed[fn] = 'declare void @%s(ptr, i%d)' % (fn, vb)
            out.append('%scall void @%s(ptr %s, i%d %s)' % (ind, fn, ptr, vb, iv))
            return
        sw = self.bswap(bits, iv, ind, out)
        out.append('%sstore %s %s, ptr %s%s%s' % (ind, ity, sw, ptr, ', align ' + align if align else '', meta))


# ---- variadic calls ---------------------------------------------------------------------
CALL_RE = re.compile(r'^(\s*)((?:' + VAL + r' = )?)((?:tail |musttail |notail )?call )(.*)$')


def parse_call(rest):
    """rest = '[cc] [attrs] RET (FIXED, ...) CALLEE(ARGS)[ #N][, !...]' for variadic calls."""
    m = re.search(r'\(([^()]*?), \.\.\.\) ', rest)
    if not m or '...' not in rest[:rest.find('(') + 200]:
        return None
    head = rest[:m.start()]
    fixed = split_top(m.group(1)) if m.group(1).strip() else []
    after = rest[m.end():]
    # callee then (args)
    mc = re.match(r'(' + VAL + r'|@(?:"(?:[^"\\]|\\.)*"|[-\w.$]+))\(', after)
    if not mc:
        return None
    callee = mc.group(1)
    p = Parser(after, mc.end() - 1)
    argtext = p.balanced()[1:-1]
    tail = after[p.p:]
    return head, fixed, callee, split_top(argtext) if argtext.strip() else [], tail


def va_slot(ty):
    if ty in ('i64', 'double'):
        return 8, 8
    if ty in ('i32', 'ptr', 'float', 'i16', 'i8', 'i1'):
        return 4, 4
    raise IRError("variadic argument of type " + ty)


def split_arg(a):
    """'i32 noundef %5' -> ('i32', '%5')"""
    ps = Parser(a)
    ps.type()
    ty = a[:ps.p].strip()
    rest = a[ps.p:].strip()
    while True:
        m = re.match(r'(noundef|signext|zeroext|inreg|nonnull|noalias|nocapture|readonly|writeonly|'
                     r'dereferenceable(?:_or_null)?\(\d+\)|align \d+|byval\([^)]*\))\s+', rest)
        if not m:
            break
        if m.group(1).startswith('byval'):
            raise IRError("byval variadic argument")
        rest = rest[m.end():]
    return ty, rest


# ---- calls through declarations that disagree with the definition ----------------------
SCALAR_RE = re.compile(r'^(i\d+|ptr|float|double)$')


def zero_of(ty):
    return 'null' if ty == 'ptr' else '0.0' if ty in ('float', 'double') else '0'


def int_bits(ty):
    return int(ty[1:])


def coerce(fr, out, ind, val, src, dst, target=None):
    """Converts `val` of type src to dst (value semantics, as a register would carry it);
    returns the new value, or writes it to `target`."""
    steps = []
    s = src
    if s == dst:
        if target:
            out.append('%s%s = freeze %s %s' % (ind, target, s, val))
            return target
        return val
    # plan the conversion as a chain of single instructions
    if s == 'ptr':
        steps.append(('ptrtoint ptr %s to i32', 'i32'))
        s = 'i32'
    if s in ('float', 'double') and dst in ('float', 'double'):
        steps.append(('%s %s %%s to %s' % ('fpext' if dst == 'double' else 'fptrunc', s, dst), dst))
        s = dst
    elif s in ('float', 'double'):
        steps.append(('fptosi %s %%s to i32' % s, 'i32'))
        s = 'i32'
    if s != dst and s.startswith('i'):
        want = 'i32' if dst in ('ptr', 'float', 'double') else dst
        if int_bits(s) < int_bits(want):
            steps.append(('sext %s %%s to %s' % (s, want), want))
        elif int_bits(s) > int_bits(want):
            steps.append(('trunc %s %%s to %s' % (s, want), want))
        s = want
        if dst == 'ptr':
            steps.append(('inttoptr i32 %s to ptr', 'ptr'))
        elif dst in ('float', 'double'):
            steps.append(('sitofp i32 %%s to %s' % dst, dst))
    v = val
    for k, (fmt, ty) in enumerate(steps):
        name = target if target and k == len(steps) - 1 else fr.tmp()
        out.append('%s%s = %s' % (ind, name, fmt % v))
        v = name
    return v


GPR_TYPES = ('i1', 'i8', 'i16', 'i32', 'ptr')


def gpr_params(define_line):
    """The integer/pointer parameters of a function, in PowerPC argument register order
    (r3, r4, ...): what a call with fewer arguments leaves in the remaining registers when
    the function passes its own parameters along unchanged."""
    m = re.match(r'^define .*?@(?:"(?:[^"\\]|\\.)*"|[-\w.$]+)\(', define_line)
    if not m:
        return []
    p = Parser(define_line, m.end() - 1)
    out = []
    for a in split_top(p.balanced()[1:-1]):
        a = a.strip()
        if not a or a == '...':
            continue
        try:
            ty, name = split_arg(a)
        except IRError:
            break
        if ty in GPR_TYPES:
            out.append((ty, name))
        elif ty not in ('float', 'double'):
            break   # i64 / aggregates: register pairs, give up on the rest
    return out


def leaked_arg(fr, out, ind, gpr_index, dst):
    """A missing integer argument: the caller's own parameter in that register, else 0."""
    params = getattr(fr, 'gpr_params', [])
    if dst in GPR_TYPES and gpr_index < len(params):
        ty, name = params[gpr_index]
        return coerce(fr, out, ind, name, ty, dst)
    return zero_of(dst)


def fix_call(fr, mc, mp, out):
    """See rewrite_function. Returns False to leave the call alone."""
    ret_d, params_d = fr.fix[mp.group(2)[1:]]
    headtxt = mp.group(1)
    k = headtxt.find('(')
    toks = (headtxt[:k] if k >= 0 else headtxt).split()
    ret_c = toks[-1] if toks else 'void'
    p = Parser(mc.group(4), mp.end() - 1)
    argtext = p.balanced()[1:-1]
    tail = mc.group(4)[p.p:]
    try:
        args = [split_arg(a) for a in (split_top(argtext) if argtext.strip() else [])]
    except IRError:
        return False
    if ret_c != 'void' and not SCALAR_RE.match(ret_c):
        return False
    if any(not SCALAR_RE.match(t) for t, _ in args):
        return False
    ind, res = mc.group(1), mc.group(2)
    new_args = []
    gpr = 0
    for i, ptyx in enumerate(params_d):
        pty, full = param_type(ptyx)
        if i < len(args):
            new_args.append('%s %s' % (full, coerce(fr, out, ind, args[i][1], args[i][0], pty)))
        else:
            new_args.append('%s %s' % (full, leaked_arg(fr, out, ind, gpr, pty)))
        if pty in GPR_TYPES:
            gpr += 1
    call = '%s%s %s(%s)%s' % (mc.group(3), ret_d, mp.group(2), ', '.join(new_args), tail)
    target = res[:-3] if res else None
    if not target:
        out.append(ind + call)
    elif ret_d == ret_c:
        out.append(ind + res + call)
    elif ret_d == 'void':
        out.append(ind + call)
        coerce(fr, out, ind, zero_of(ret_c), ret_c, ret_c, target)
    else:
        t = fr.tmp()
        out.append('%s%s = %s' % (ind, t, call))
        coerce(fr, out, ind, t, ret_d, ret_c, target)
    return True


def function_signature(line):
    """(return type, [parameter types], variadic) of a define/declare line, or None when it
    is not made of scalar types only."""
    m = re.match(r'^(?:define|declare) (.*?)@([-\w.$]+)\(', line)
    if not m:
        return None
    toks = m.group(1).split()
    ret = toks[-1] if toks else None
    if ret != 'void' and not (ret and SCALAR_RE.match(ret)):
        return None
    p = Parser(line, m.end() - 1)
    params = split_top(p.balanced()[1:-1])
    types, variadic = [], False
    for a in params:
        a = a.strip()
        if not a:
            continue
        if a == '...':
            variadic = True
            continue
        try:
            ty = split_arg(a)[0]
        except IRError:
            return None
        if not SCALAR_RE.match(ty):
            return None
        # narrow integers: the callee relies on how the caller extended them
        if re.search(r'\bzeroext\b', a):
            ty += ':z'
        elif re.search(r'\bsignext\b', a):
            ty += ':s'
        types.append(ty)
    return ret, types, variadic


def param_type(t):
    """'i16:z' -> ('i16', 'i16 zeroext') : the type, and the type with its attribute"""
    base, _, ext = t.partition(':')
    return base, base + {'z': ' zeroext', 's': ' signext'}.get(ext, '')


def function_signatures(text):
    """{name: signature} of the functions a module defines for other modules."""
    out = {}
    for line in text.split('\n'):
        m = DEFINE_NAME_RE.match(line)
        if not m or m.group(2).startswith('"') or re.search(r'\b(internal|private)\b', m.group(1)):
            continue
        out[m.group(2)] = function_signature(line)
    return out


def rewrite_function(lines, fr, varargs_fns, entry_allocas):
    """Rewrites one function body (list of lines between define and }), in place."""
    out = []
    va_param = None
    fr.locals = set(re.findall(r'^\s*(' + VAL + r') = alloca ', '\n'.join(lines), re.M))
    for line in lines:
        s = line.strip()
        if 'call void @llvm.va_start' in s:
            mv = re.search(r'\(ptr(?: \w+)* (' + VAL + r')\)', s)
            out.append('  store ptr %%gcn.va, ptr %s, align 4' % mv.group(1))
            continue
        if 'call void @llvm.va_end' in s:
            continue
        if 'call void @llvm.va_copy' in s:
            mv = re.findall(r'ptr(?: \w+)* (' + VAL + r')', s)
            t = fr.tmp()
            out.append('  %s = load ptr, ptr %s, align 4' % (t, mv[1]))
            out.append('  store ptr %s, ptr %s, align 4' % (t, mv[0]))
            continue
        mc = CALL_RE.match(line)
        # A call to a function this module declares differently from the program's
        # definition (K&R declarations, mismatched prototypes): call the definition's
        # signature, converting each argument as the PowerPC calling convention would
        # deliver it (missing arguments read as zero, extra ones are ignored).
        # An indirect call: the target may take more arguments than this call passes
        # (decomps cast handlers to shorter types). On PowerPC those read the registers the
        # caller left, usually its own parameters: pass them along (extra arguments are
        # harmless to a callee that takes fewer).
        if mc and fr.gpr_params and 'musttail' not in mc.group(3):
            mi = re.match(r'([^(]*?)(%[-\w.$]+)\(', mc.group(4))
            if mi and '...' not in mc.group(4)[:mi.end()]:
                p = Parser(mc.group(4), mi.end() - 1)
                argtext = p.balanced()[1:-1]
                tail = mc.group(4)[p.p:]
                try:
                    args = [split_arg(a) for a in (split_top(argtext) if argtext.strip() else [])]
                except IRError:
                    args = None
                if args is not None:
                    k = sum(1 for t, _ in args if t in GPR_TYPES)
                    extra = fr.gpr_params[k:8]
                    if extra and k < 8:
                        more =', '.join('%s %s' % (t, n) for t, n in extra)
                        argtext = (argtext + ', ' + more) if argtext.strip() else more
                        line = '%s%s%s%s%s(%s)%s' % (mc.group(1), mc.group(2), mc.group(3), mi.group(1), mi.group(2),
                                                     argtext, tail)
                        mc = CALL_RE.match(line)
        if mc and fr.fix:
            mp = re.match(r'(.*?)(@[-\w.$]+)\(', mc.group(4))
            if mp and mp.group(2)[1:] in fr.fix:
                fixed_call = fix_call(fr, mc, mp, out)
                if fixed_call:
                    continue
        # A call through a K&R (unprototyped) declaration, written `call T (...) @f(args)`
        # or (newer clang) `call T @f(args)` with f declared `(...)`: an ordinary call with
        # the argument types of this call site, as PowerPC passes them - unless the program
        # defines f variadic (sprintf without its header): then a variadic call with the
        # definition's number of fixed arguments.
        knr = None
        if mc and ' (...) ' in line:
            mk = re.match(r'(.*?) \(\.\.\.\) (' + VAL + r'|@(?:"(?:[^"\\]|\\.)*"|[-\w.$]+))\(', mc.group(4))
            if mk:
                knr = (mk.group(1), mk.group(2), mk.end() - 1)
        elif mc and fr.knr_names:
            mp = re.match(r'(.*?)(@[-\w.$]+)\(', mc.group(4))
            if mp and mp.group(2)[1:] in fr.knr_names and '(' not in mp.group(1):
                knr = (mp.group(1).rstrip(), mp.group(2), mp.end() - 1)
        if knr:
            head, callee, popen = knr
            p = Parser(mc.group(4), popen)
            argtext = p.balanced()[1:-1]
            tail = mc.group(4)[p.p:]
            args = split_top(argtext) if argtext.strip() else []
            types = [split_arg(a)[0] for a in args]
            ret = head.split()[-1] if head.split() else 'void'
            nfixed = fr.variadic.get(callee[1:])
            if nfixed is not None and nfixed <= len(types):
                fr.knr.setdefault(callee, (ret, types[:nfixed], True))
                line = '%s%s%s%s (%s) %s(%s)%s' % (mc.group(1), mc.group(2), mc.group(3), head,
                                                   ', '.join(types[:nfixed] + ['...']), callee, argtext, tail)
                mc = CALL_RE.match(line)
            else:
                fr.knr.setdefault(callee, (ret, types, False))
                out.append('%s%s%s%s (%s) %s(%s)%s' % (mc.group(1), mc.group(2), mc.group(3), head,
                                                      ', '.join(types), callee, argtext, tail))
                continue
        if mc and ', ...)' in line:
            parsed = parse_call(mc.group(4))
            if parsed:
                head, fixed, callee, args, tail = parsed
                ind = mc.group(1)
                va_args = args[len(fixed):]
                off, slots = 0, []
                for a in va_args:
                    ty, v = split_arg(a)
                    size, al = va_slot(ty)
                    off = (off + al - 1) // al * al
                    slots.append((off, ty, v))
                    off += size
                buf = fr.tmp()
                entry_allocas.append('  %s = alloca [%d x i8], align 8' % (buf, max(off, 8)))
                for o, ty, v in slots:
                    if ty in ('i8', 'i16', 'i1'):
                        w = fr.tmp()
                        out.append('%s%s = sext %s %s to i32' % (ind, w, ty, v))
                        ty, v = 'i32', w
                    if ty == 'float':
                        w = fr.tmp()
                        out.append('%s%s = fpext float %s to double' % (ind, w, v))
                        ty, v = 'double', w
                    g = fr.tmp()
                    out.append('%s%s = getelementptr inbounds i8, ptr %s, i32 %d' % (ind, g, buf, o))
                    out.append('%sstore %s %s, ptr %s, align %d' % (ind, ty, v, g, 4 if ty in ('i32', 'ptr') else 8))
                newargs = ', '.join(args[:len(fixed)] + ['ptr ' + buf])
                out.append('%s%s%s%s %s(%s)%s' % (ind, mc.group(2), mc.group(3), head.rstrip(), callee, newargs, tail))
                continue
        out.append(line)
    # loads and stores (including the ones added above)
    final = []
    for line in out:
        m = LOAD_RE.match(line)
        if m:
            fr.load(m, final)
            continue
        m = STORE_RE.match(line)
        if m:
            fr.store(m, final)
            continue
        final.append(line)
    return final


DEFINE_VARARG_RE = re.compile(r'^(define .*?\()(.*), \.\.\.\)(.*)$')
DECLARE_VARARG_RE = re.compile(r'^(declare .*?\()(.*?)(, )?\.\.\.\)(.*)$')


DEFINE_NAME_RE = re.compile(r'^define (.*?)@("(?:[^"\\]|\\.)*"|[-\w.$]+)\(')


def defined_functions(text):
    """External function definitions of a module (for the program-wide duplicate list)."""
    names = []
    for line in text.split('\n'):
        m = DEFINE_NAME_RE.match(line)
        if m and not re.search(r'\b(internal|private|available_externally|linkonce|linkonce_odr|weak|weak_odr)\b',
                                m.group(1)):
            names.append(m.group(2))
    return names


def link_inline(head, weak_names):
    """Metrowerks treats `inline` functions in headers like C++ does: one copy survives the
    link. clang (gnu89) leaves `extern inline` as available_externally (no symbol) and
    emits plain `inline` in every unit; make the first linkonce_odr (emitted where used)
    and the program-wide duplicates weak."""
    head = head.replace('define available_externally ', 'define linkonce_odr ')
    m = DEFINE_NAME_RE.match(head)
    if m and m.group(2) in weak_names and not re.search(r'\b(internal|private|linkonce|linkonce_odr|weak|weak_odr)\b',
                                                        m.group(1)):
        head = 'define weak ' + head[len('define '):]
    return head


def write_signatures(path, sigs):
    """Program-wide signature list (gcn_build.py): one 'name ret params variadic' line per
    function with a single scalar-only definition."""
    lines = []
    for name, sig in sorted(sigs.items()):
        if sig:
            lines.append('%s %s %s %d\n' % (name, sig[0], ','.join(sig[1]) or '-', 1 if sig[2] else 0))
    return ''.join(lines)


def read_signatures(path):
    out = {}
    for l in open(path, encoding='utf-8'):
        f = l.split()
        if len(f) == 4:
            out[f[0]] = (f[1], [] if f[2] == '-' else f[2].split(','), f[3] == '1')
    return out


def rewrite_module(text, unit_id, weak_names=frozenset(), sigs=None):
    lines = text.split('\n')
    types = TypeTable()
    for line in lines:
        m = re.match(r'^%("?[-\w.$]+"?) = type (.*)$', line)
        if m:
            body = m.group(2).strip()
            types.named[m.group(1).strip('"')] = None if body == 'opaque' else Parser(body).type()
    needed = {}
    fixups = []
    out = []
    i = 0
    fr = FuncRewriter(needed, types)
    # functions declared K&R style (no prototype): calls to them may also appear as plain
    # calls with the call site's argument types (newer clang), which give the signature
    sigs = sigs or {}
    fr.variadic = {n: len(s[1]) for n, s in sigs.items() if s[2]}
    fr.knr_names = set(re.findall(r'^declare .*?@([-\w.$]+)\(\.\.\.\)', text, re.M))
    # declarations that disagree with the program's (single) definition
    fr.fix = {}
    for line in lines:
        if line.startswith('declare ') and '@llvm.' not in line:
            m = re.match(r'declare .*?@([-\w.$]+)\(', line)
            prog = sigs.get(m.group(1)) if m else None
            if prog and not prog[2]:
                mine = function_signature(line)
                if mine != prog:
                    fr.fix[m.group(1)] = (prog[0], prog[1])
    declared = set()
    while i < len(lines):
        line = lines[i]
        if line.startswith('target datalayout'):
            out.append('target datalayout = "%s"' % WASM_DATALAYOUT)
        elif line.startswith('target triple'):
            out.append('target triple = "%s"' % WASM_TRIPLE)
        elif line.startswith('@'):
            out.append(rewrite_global(line, types, fixups))
        elif line.startswith('define '):
            head = line
            mv = DEFINE_VARARG_RE.match(head)
            if mv:
                head = '%s%s, ptr %%gcn.va)%s' % (mv.group(1), mv.group(2), mv.group(3))
            head = re.sub(r'\b(arm_aapcscc|arm_aapcs_vfpcc) ', '', head)
            head = link_inline(head, weak_names)
            body = []
            i += 1
            while lines[i] != '}':
                body.append(lines[i])
                i += 1
            fr.n = 0
            fr.gpr_params = gpr_params(line)
            allocas = []
            new = rewrite_function(body, fr, None, allocas)
            out.append(head)
            # allocas go at the start of the entry block
            k = 0
            if new and re.match(r'^[-\w.$]+:', new[0]):
                out.append(new[0])
                k = 1
            out.extend(allocas)
            out.extend(new[k:])
            out.append('}')
        elif line.startswith('declare '):
            m = re.match(r'declare .*?@([-\w.$]+)\(', line)
            if m:
                declared.add(m.group(1))
            if m and m.group(1).startswith('llvm.va_'):
                pass
            elif m and m.group(1) in fr.fix:
                # the definition's signature (fix_call converted the calls)
                ret, params = fr.fix[m.group(1)]
                p = Parser(line, m.end() - 1)
                p.balanced()
                out.append('declare %s @%s(%s)%s' % (ret, m.group(1), ', '.join(param_type(t)[1] for t in params),
                                                     line[p.p:]))
            else:
                mv = DECLARE_VARARG_RE.match(line)
                mk = re.match(r'^(declare .*?)(@(?:"(?:[^"\\]|\\.)*"|[-\w.$]+))\(\.\.\.\)(.*)$', line)
                if mk:
                    # K&R declaration: the signature of its first call in this unit
                    sig = fr.knr.get(mk.group(2))
                    if sig and sig[2]:
                        # variadic: the fixed arguments, then the argument buffer
                        line = '%s%s(%s)%s' % (mk.group(1), mk.group(2), ', '.join(sig[1] + ['ptr']), mk.group(3))
                    elif sig:
                        line = '%s%s(%s)%s' % (mk.group(1), mk.group(2), ', '.join(sig[1]), mk.group(3))
                    else:
                        line = '%s%s()%s' % (mk.group(1), mk.group(2), mk.group(3))
                elif mv:
                    line = '%s%s%sptr)%s' % (mv.group(1), mv.group(2), ', ' if mv.group(2) else '', mv.group(4))
                out.append(re.sub(r'\b(arm_aapcscc|arm_aapcs_vfpcc) ', '', line))
        elif line.startswith('attributes #'):
            line = re.sub(r' "target-(cpu|features)"="[^"]*"', '', line)
            out.append(line)
        else:
            out.append(line)
        i += 1
    for name, decl in sorted(needed.items()):
        if name not in declared:
            out.append(decl)
    if fixups:
        entries = ', '.join('ptr getelementptr (i8, ptr %s, i32 %d)' % (n, o) for n, o in fixups)
        out.append('@__gcn_befix.%s = internal constant [%d x ptr] [%s], section "gcn_befix", align 4'
                   % (unit_id, len(fixups), entries))
        used = '@__gcn_befix.%s' % unit_id
        # keep it alive: append to (or create) llvm.used
        for j, line in enumerate(out):
            m = re.match(r'^@llvm\.used = appending global \[(\d+) x ptr\] \[(.*)\], section "llvm.metadata"$', line)
            if m:
                out[j] = '@llvm.used = appending global [%d x ptr] [%s, ptr %s], section "llvm.metadata"' % (
                    int(m.group(1)) + 1, m.group(2), used)
                break
        else:
            out.append('@llvm.used = appending global [1 x ptr] [ptr %s], section "llvm.metadata"' % used)
    return '\n'.join(out)


BSWAP_DECL_RE = re.compile(r'^declare (i16|i32|i64) @llvm\.bswap\.\1\(\1\)[^\n]*$', re.M)


def late_bswaps(text):
    """Optimised wasm IR -> the same with every scalar llvm.bswap as a call to the host's
    __gcn_bswap16/32/64 (env imports). WebAssembly has no byte swap: lowered, a swap is a
    7-instruction rotate/xor sequence no C compiler recognises again, after every load and
    before every store. As an import, gcn_wasm_to_c.py makes it the host's own byte swap,
    which costs one instruction on x86 and cancels against the load's own swap on a
    big-endian host. Done after LLVM's optimisations, so they still see real byte swaps."""
    text = BSWAP_DECL_RE.sub(lambda m: 'declare %s @__gcn_bswap%s(%s) nounwind willreturn memory(none)'
                             % (m.group(1), m.group(1)[1:], m.group(1)), text)
    for bits in ('16', '32', '64'):
        text = text.replace('@llvm.bswap.i%s(' % bits, '@__gcn_bswap%s(' % bits)
    return text


def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--late-bswaps':
        text = open(sys.argv[2], encoding='utf-8').read()
        open(sys.argv[3], 'w', encoding='utf-8', newline='\n').write(late_bswaps(text))
        return 0
    src, dst = sys.argv[1], sys.argv[2]
    unit = '0'
    weak = frozenset()
    sigs = {}
    for a in sys.argv[3:]:
        if a.startswith('--unit-id='):
            unit = a.split('=', 1)[1]
        elif a.startswith('--weak-list='):
            weak = frozenset(l.strip() for l in open(a.split('=', 1)[1]) if l.strip())
        elif a.startswith('--signatures='):
            sigs = read_signatures(a.split('=', 1)[1])
    text = open(src, encoding='utf-8').read()
    try:
        res = rewrite_module(text, re.sub(r'\W', '_', unit), weak, sigs)
    except IRError as e:
        sys.stderr.write('gcn_ir: %s: %s\n' % (src, e))
        return 1
    open(dst, 'w', encoding='utf-8', newline='\n').write(res)
    return 0


if __name__ == '__main__':
    sys.exit(main())
