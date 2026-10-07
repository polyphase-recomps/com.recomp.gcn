"""dtk symbols -> the GameCube recompiler's symbol file.

    python gcn_syms.py --symbols config/GSAE01_rev1/symbols.txt --dol main.dol --out syms.txt
                       [--map build/Release/sfa.map | --hle hle.txt] [--write-hle hle.txt]

Reads decomp-toolkit's symbols.txt (names, addresses and sizes of every function) and the DOL,
and writes a plain text file the recompiler reads:

    dol_sha1 <sha1 of main.dol>
    entry 0x80003140
    r2 0x803E7180             small-data bases, from __init_registers
    r13 0x803E3E40
    ctors 0x802C2000          static constructors (_ctors), run before main
    text <addr> <size> <dol offset>     one per DOL text section
    func <addr> <size> <name>
    label <addr> <name>       labels inside text (extra entry points)
    jumptable <addr> <size>   data objects that may be switch tables (absolute code addresses)
    hle <name>                functions the runtime supplies instead of the recompiled code

The HLE set is what the decomp build takes from com.recomp.gcn's runtime instead of the decomp:
every function its link map (wasm-ld -Map, e.g. Native/build/Release/sfa.map) places in a
`runtime_*` object. --write-hle saves that list so a package can ship it (names only) and build
without the decomp: --hle reads it back.
"""
import argparse
import hashlib
import re
import struct
import sys

SYM_RE = re.compile(r"^(\S+) = (\.?\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?(.*)$")
MAP_RE = re.compile(r"/obj/([A-Za-z0-9_]+)\.o:\((.+)\)\s*$")


def read_symbols(path):
    syms = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = SYM_RE.match(line.strip())
            if not m:
                continue
            name, section, addr, kind, size, rest = m.groups()
            syms.append({"name": name, "section": section, "addr": int(addr, 16), "kind": kind,
                         "size": int(size, 16) if size else 0, "rest": rest})
    return syms


def dol_sections(dol):
    offs = struct.unpack(">18I", dol[0:0x48])
    addrs = struct.unpack(">18I", dol[0x48:0x90])
    sizes = struct.unpack(">18I", dol[0x90:0xD8])
    entry = struct.unpack(">I", dol[0xE0:0xE4])[0]
    text = [(addrs[i], sizes[i], offs[i]) for i in range(7) if sizes[i]]
    return text, entry


def word_at(dol, text, addr):
    for base, size, off in text:
        if base <= addr < base + size:
            return struct.unpack(">I", dol[off + addr - base:off + addr - base + 4])[0]
    return None


def small_data_bases(dol, text, syms):
    """r2 / r13 as __init_registers sets them: lis rN,hi then addi/ori rN,rN,lo."""
    init = next((s for s in syms if s["name"] == "__init_registers"), None)
    if init is None:
        return {}
    regs = {}
    for a in range(init["addr"], init["addr"] + max(init["size"], 4), 4):
        w = word_at(dol, text, a)
        if w is None:
            break
        op, d, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15 and ra == 0:  # lis
            regs[d] = (imm << 16) & 0xFFFFFFFF
        elif op == 14 and ra == d and d in regs:  # addi
            regs[d] = (regs[d] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
        elif op == 24 and d in regs:  # ori rA,rS,imm (rA = bits 11-15 is the destination)
            regs[ra] = regs[d] | imm
    return {n: regs[n] for n in (1, 2, 13) if n in regs}


def hle_from_map(path, functions):
    """Functions the decomp build takes from the runtime: defined in runtime_* objects and in no
    decomp / game object (the runtime's WEAK fallbacks lose to a decomp definition)."""
    runtime, other = set(), set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = MAP_RE.search(line)
            if m and m.group(2) in functions:
                (runtime if m.group(1).startswith("runtime_") else other).add(m.group(2))
    return runtime - other


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbols", required=True)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--map", help="the decomp build's wasm-ld map (HLE set from runtime_* objects)")
    ap.add_argument("--hle", help="a saved HLE list (one name per line)")
    ap.add_argument("--write-hle", help="save the HLE list here")
    args = ap.parse_args()

    dol = open(args.dol, "rb").read()
    text, entry = dol_sections(dol)
    syms = read_symbols(args.symbols)
    in_text = lambda a: any(base <= a < base + size for base, size, _ in text)

    funcs = [s for s in syms if s["kind"] == "function" and in_text(s["addr"])]
    names = {s["name"] for s in funcs}
    labels = [s for s in syms if s["kind"] == "label" and in_text(s["addr"])]
    # switch tables: dtk names most "jumptable_*", some are compiler locals ("@179"); any sized
    # data object can be one (the recompiler only uses it where code indexes it and jumps)
    tables = [s for s in syms if s["kind"] == "object" and s["size"] and not in_text(s["addr"]) and
              s["section"] in (".data", ".rodata", ".sdata", ".sdata2")]
    bases = small_data_bases(dol, text, syms)

    hle = set()
    if args.map:
        hle = hle_from_map(args.map, names)
    elif args.hle:
        with open(args.hle) as f:
            hle = {l.strip() for l in f if l.strip() and not l.startswith("#")} & names
    if args.write_hle:
        with open(args.write_hle, "w", newline="\n") as f:
            f.write("# functions com.recomp.gcn's runtime supplies (from the decomp build's link map)\n")
            for n in sorted(hle):
                f.write(n + "\n")

    dupes = {}
    for s in funcs:
        dupes.setdefault(s["name"], []).append(s["addr"])
    with open(args.out, "w", newline="\n") as f:
        src = args.symbols.replace("\\", "/")
        src = src[src.find("config/"):] if "config/" in src else src.split("/")[-1]
        f.write("# GameCube recompiler symbols, generated by gcn_syms.py from the decomp's %s\n" % src)
        f.write("dol_sha1 %s\n" % hashlib.sha1(dol).hexdigest())
        f.write("entry 0x%08X\n" % entry)
        for n in (1, 2, 13):
            if n in bases:
                f.write("r%d 0x%08X\n" % (n, bases[n]))
        ctors = next((s for s in syms if s["name"] == "_ctors"), None)
        if ctors:
            f.write("ctors 0x%08X\n" % ctors["addr"])
        for base, size, off in text:
            f.write("text 0x%08X 0x%X 0x%X\n" % (base, size, off))
        for s in sorted(funcs, key=lambda s: s["addr"]):
            name = s["name"]
            if len(dupes[name]) > 1:  # local statics with the same name: keep them apart
                name = "%s_%08X" % (name, s["addr"])
            f.write("func 0x%08X 0x%X %s\n" % (s["addr"], s["size"], name))
        for s in sorted(labels, key=lambda s: s["addr"]):
            f.write("label 0x%08X %s\n" % (s["addr"], s["name"]))
        for s in sorted(tables, key=lambda s: s["addr"]):
            f.write("jumptable 0x%08X 0x%X\n" % (s["addr"], s["size"]))
        for n in sorted(hle):
            f.write("hle %s\n" % n)
    print("%d functions, %d labels, %d jump tables, %d HLE, r2=%s r13=%s" %
          (len(funcs), len(labels), len(tables), len(hle), hex(bases.get(2, 0)), hex(bases.get(13, 0))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
