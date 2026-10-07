"""Compares decode_test's disassembly of a DOL with powerpc-eabi-objdump's (-M gekko,raw).

    python compare_objdump.py main.dol ours.txt [--objdump PATH] [--show N]

Extracts each text section, disassembles it with objdump at its address, and reports every line
that differs (normalised: one space between mnemonic and operands). Exit 0 when all match.
"""
import argparse
import collections
import os
import struct
import subprocess
import sys
import tempfile

DEFAULT_OBJDUMP = r"C:\devkitPro\devkitPPC\bin\powerpc-eabi-objdump.exe"


def norm(text):
    text = text.strip()
    parts = text.split(None, 1)
    return parts[0] if len(parts) == 1 else parts[0] + " " + parts[1].strip()


def known_form(ours):
    """Encodings binutils rejects as invalid forms but the hardware runs (and we decode):
    lmw with rA in the loaded range (the SDK's context restore), bc with a reserved BO."""
    m = ours.split(" ")[0]
    return m in ("lmw", "bc", "bcl", "bca", "bcla")


def objdump_lines(dol, objdump):
    out = {}
    offs = struct.unpack(">7I", dol[0:28])
    addrs = struct.unpack(">7I", dol[0x48:0x64])
    sizes = struct.unpack(">7I", dol[0x90:0xAC])
    with tempfile.TemporaryDirectory() as tmp:
        for i in range(7):
            if not sizes[i]:
                continue
            path = os.path.join(tmp, "text%d.bin" % i)
            with open(path, "wb") as f:
                f.write(dol[offs[i]:offs[i] + sizes[i]])
            text = subprocess.run([objdump, "-D", "-b", "binary", "-m", "powerpc", "-M", "gekko,raw", "-EB",
                                   "--adjust-vma=0x%x" % addrs[i], path], capture_output=True, text=True,
                                  check=True).stdout
            for line in text.splitlines():
                fields = line.split("\t")
                if len(fields) >= 3 and fields[0].endswith(":"):
                    out[int(fields[0][:-1].strip(), 16)] = norm(fields[2])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dol")
    ap.add_argument("ours")
    ap.add_argument("--objdump", default=DEFAULT_OBJDUMP)
    ap.add_argument("--show", type=int, default=40)
    args = ap.parse_args()

    dol = open(args.dol, "rb").read()
    ref = objdump_lines(dol, args.objdump)
    ours = {}
    with open(args.ours) as f:
        for line in f:
            addr, _, text = line.rstrip("\n").partition(":\t")
            ours[int(addr, 16)] = norm(text)

    diffs = []
    allowed = []
    for a in sorted(set(ref) | set(ours)):
        r, o = ref.get(a), ours.get(a)
        if r == o or (r is None and o == ".long 0x0"):  # objdump folds runs of zero words into "..."
            continue
        if r is not None and r.startswith(".long") and o is not None and known_form(o):
            allowed.append((a, r, o))
            continue
        diffs.append((a, r, o))
    by_mnem = collections.Counter((r or "?").split(" ")[0] + " / " + (o or "?").split(" ")[0] for _, r, o in diffs)
    print("%d instructions, %d differ, %d allowed (encodings objdump rejects but we decode)" %
          (len(ref), len(diffs), len(allowed)))
    for a, r, o in allowed:
        print("  allowed %08x  objdump: %-24s ours: %s" % (a, r, o))
    for key, count in by_mnem.most_common(30):
        print("  %6d  objdump / ours: %s" % (count, key))
    for a, r, o in diffs[:args.show]:
        print("%08x  objdump: %-40s ours: %s" % (a, r, o))
    return 0 if not diffs else 1


if __name__ == "__main__":
    sys.exit(main())
