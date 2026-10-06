#!/usr/bin/env python3
"""Metrowerks dialect -> clang, applied to a copy of each decomp source before compiling.

Decomp patches (game package Native/patches/*.patch) fix game-specific code; this tool only
rewrites constructs every Metrowerks-built GameCube decomp has and clang rejects:

  * variables placed at a fixed address, `T name : (0xCC008000);` (hardware registers,
    the GX write-gather pipe): a macro for a volatile access to that address, which the
    runtime routes to the host;
  * lvalue casts, `(u8*)p += n;` / `++((T*)p)`: `(*(T**)&p)`, the same pointer as an lvalue.

Usage: gcn_sources.py <in.c> <out.c>   (writes out.c only when it changes)
"""
import os
import re
import sys

AT_ADDRESS = re.compile(r'^([ \t]*)(?:extern\s+)?(?:static\s+)?(?:volatile\s+)?([A-Za-z_][\w ]*?[\w*])\s+'
                        r'([A-Za-z_]\w*)\s*:\s*\(\s*(0x[0-9A-Fa-f]{8})\s*\)\s*;', re.M)  # not a bitfield: (0x...)
# Lvalue casts, `(T*)p` used as an assignable pointer: `(*(T**)&p)` is that lvalue in C.
_T = r'([A-Za-z_][\w ]*?\s*\*+)'
_N = r'([A-Za-z_][\w.\->\[\]]*)'
LV_PRE = re.compile(r'(\+\+|--)\s*\(\s*\(\s*' + _T + r'\s*\)\s*\(?\s*' + _N + r'\s*\)?\s*\)')
LV_POST = re.compile(r'\(\s*\(\s*' + _T + r'\s*\)\s*\(?\s*' + _N + r'\s*\)?\s*\)\s*(\+\+|--)')
LV_ASSIGN = re.compile(r'(?<![\w)\]*&])(\s*)\(\s*' + _T + r'\s*\)\s*\(?\s*' + _N + r'\s*\)?\s*([+-]=)')


def fix(text):
    def at_address(m):
        ind, ty, name, addr = m.groups()
        # one line, so line numbers stay those of the original
        return '%s#define %s (*(volatile %s *)%s)' % (ind, name, ty, addr)

    text = AT_ADDRESS.sub(at_address, text)
    text = LV_PRE.sub(lambda m: '%s(*(%s*)&(%s))' % (m.group(1), m.group(2), m.group(3)), text)
    text = LV_POST.sub(lambda m: '(*(%s*)&(%s))%s' % (m.group(1), m.group(2), m.group(3)), text)
    text = LV_ASSIGN.sub(lambda m: '%s(*(%s*)&(%s)) %s' % (m.group(1), m.group(2), m.group(3), m.group(4)), text)
    return text


def main():
    src, dst = sys.argv[1], sys.argv[2]
    raw = open(src, 'rb').read()
    try:
        text = raw.decode('utf-8')
        enc = 'utf-8'
    except UnicodeDecodeError:
        text = raw.decode('cp932', errors='surrogateescape')
        enc = 'cp932'
    out = fix(text)
    # keep #line pointing at the original file so diagnostics and debug info name it
    out = '#line 1 "%s"\n%s' % (os.path.abspath(src).replace('\\', '/'), out)
    data = out.encode(enc, errors='surrogateescape')
    if os.path.exists(dst) and open(dst, 'rb').read() == data:
        return 0
    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    open(dst, 'wb').write(data)
    return 0


if __name__ == '__main__':
    sys.exit(main())
