#!/usr/bin/env python3
"""Writes a unified diff of edited copies against the decomp, for Native/patches/.

    gcn_mkpatch.py <decomp> <out.patch> <rel path>=<edited file> [...]

Paths in the patch are relative to the decomp root (a/src/..., b/src/...), the format
tools/apply_patches.py applies.
"""
import difflib
import os
import sys


def main():
    decomp, out = sys.argv[1], sys.argv[2]
    chunks = []
    for spec in sys.argv[3:]:
        rel, edited = spec.split('=', 1)
        a = open(os.path.join(decomp, rel), encoding='utf-8', errors='surrogateescape', newline='').read()
        b = open(edited, encoding='utf-8', errors='surrogateescape', newline='').read()
        al = [l.rstrip('\r\n') + '\n' for l in a.splitlines(True)]
        bl = [l.rstrip('\r\n') + '\n' for l in b.splitlines(True)]
        diff = list(difflib.unified_diff(al, bl, 'a/' + rel, 'b/' + rel, n=3))
        if diff:
            chunks.append('diff --git a/%s b/%s\n' % (rel, rel) + ''.join(diff))
    open(out, 'w', encoding='utf-8', errors='surrogateescape', newline='\n').write(''.join(chunks))
    print('%s: %d files' % (out, len(chunks)))


if __name__ == '__main__':
    main()
