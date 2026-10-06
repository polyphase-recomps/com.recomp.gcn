"""Applies a game's unified-diff patches to copies of the decomp sources.

The decomp checkout is never modified: every file a patch touches is copied to
<out_dir>/<relative path>, the hunks are applied there, and the build compiles the
copy instead (same mechanism as sjis_sources.py). Patches use paths relative to the
decomp root (git diff style: "a/src/main/efe.c" / "b/src/main/efe.c").

Usage: apply_patches.py <decomp_dir> <out_dir> <patch.patch>... <source>...
Prints "original|patched" for every source that has a patched copy.
"""
import os
import re
import sys


def parse_patch(text):
    """Returns {relative path: [(old_start, old_lines, new_lines), ...]}."""
    files = {}
    current = None
    hunk = None
    for line in text.splitlines(keepends=True):
        if line.startswith("+++ "):
            path = line[4:].strip().split("\t")[0]
            if path.startswith("b/"):
                path = path[2:]
            current = files.setdefault(path, [])
            hunk = None
        elif line.startswith("--- "):
            continue
        elif line.startswith("@@"):
            m = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", line)
            hunk = (int(m.group(1)), [], [])
            current.append(hunk)
        elif hunk is not None and line[:1] in (" ", "-", "+"):
            body = line[1:]
            if line[0] in (" ", "-"):
                hunk[1].append(body)
            if line[0] in (" ", "+"):
                hunk[2].append(body)
        elif line.startswith("\\"):
            continue
    return files


def norm(line):
    return line.rstrip("\r\n")


def apply_hunks(lines, hunks, name):
    offset = 0
    for start, old, new in hunks:
        want = [norm(l) for l in old]
        pos = start - 1 + offset
        found = -1
        for delta in range(0, 200):
            for cand in (pos + delta, pos - delta):
                if 0 <= cand <= len(lines) - len(want) and [norm(l) for l in lines[cand:cand + len(want)]] == want:
                    found = cand
                    break
            if found >= 0:
                break
        if found < 0:
            raise SystemExit("apply_patches: hunk at line %d does not apply to %s" % (start, name))
        eol = "\r\n" if lines and lines[0].endswith("\r\n") else "\n"
        lines[found:found + len(want)] = [norm(l) + eol for l in new]
        offset += found - (start - 1) + len(new) - len(want)
    return lines


def main():
    args = sys.argv[1:]
    decomp, out_dir = os.path.abspath(args[0]), args[1]
    rest = args[2:]
    # applied in file name order (0001-..., 0002-...), whatever order they are passed in
    patches = sorted((a for a in rest if a.endswith(".patch") or a.endswith(".diff")), key=os.path.basename)
    sources = [a for a in rest if a not in patches]
    # path -> [hunks of one patch, ...] in patch order: each patch is applied on top of the
    # previous ones, so a patch may use lines an earlier patch added as its context
    edits = {}
    for patch in patches:
        for path, hunks in parse_patch(open(patch, encoding="utf-8", errors="surrogateescape").read()).items():
            edits.setdefault(path, []).append(hunks)
    patched = {}
    for rel, per_patch in edits.items():
        src = os.path.join(decomp, rel)
        lines = open(src, encoding="utf-8", errors="surrogateescape", newline="").read().splitlines(keepends=True)
        for hunks in per_patch:
            lines = apply_hunks(lines, sorted(hunks, key=lambda h: h[0]), rel)
        dst = os.path.join(out_dir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        data = "".join(lines).encode("utf-8", "surrogateescape")
        if not os.path.exists(dst) or open(dst, "rb").read() != data:
            with open(dst, "wb") as f:
                f.write(data)
        patched[os.path.normcase(os.path.abspath(src))] = dst.replace("\\", "/")
    for s in sources:
        key = os.path.normcase(os.path.abspath(s))
        if key in patched:
            print("%s|%s" % (s, patched[key]))


if __name__ == "__main__":
    main()
