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
    native <name>             of those, the ones the recomp runtime itself implements with the guest
                              registers in hand (Runtime/recomp/recomp_native.c: hand-written code
                              the recompiler cannot express, e.g. GS engine thread switches);
                              from `native <name>` lines of --names files
    data <addr> <size> <name> the decomp's globals at their DOL addresses (--placed): what the
                              package's mods name, in the recomp build too

The HLE set is what the decomp build takes from com.recomp.gcn's runtime instead of the decomp:
every function its link map (wasm-ld -Map, e.g. Native/build/Release/sfa.map) places in a
`runtime_*` object. --write-hle saves that list so a package can ship it (names only) and build
without the decomp: --hle reads it back.

--placed takes the decomp build's placed data symbols (gcn_place.py's <name>.syms, next to the
map): its names are the decomp's own (symbols.txt names many of them lbl_*), and gcn_place.py
found their addresses by name or by contents. Only addresses in the DOL's data sections or its
bss count (what gcn_place.py left at the linker's addresses is not the game's).
"""
import argparse
import hashlib
import os
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


def dol_data_ranges(dol):
    """[(start, end)] of the DOL's data sections and its bss."""
    addrs = struct.unpack(">18I", dol[0x48:0x90])
    sizes = struct.unpack(">18I", dol[0x90:0xD8])
    bss, bss_size = struct.unpack(">II", dol[0xD8:0xE0])
    return [(addrs[i], addrs[i] + sizes[i]) for i in range(7, 18) if sizes[i]] + [(bss, bss + bss_size)]


def read_placed(path, ranges):
    """gcn_place.py's '<addr> 0 <size> <name>' lines that sit in the DOL's data."""
    out = {}
    for line in open(path, encoding="utf-8"):
        f = line.split()
        if len(f) != 4 or line.startswith("#"):
            continue
        addr, size, name = int(f[0], 16), int(f[2], 16), f[3]
        if size and any(lo <= addr and addr + size <= hi for lo, hi in ranges):
            out.setdefault(name, (addr, size))
    return out


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


# the runtime's library-level fallbacks: a game's own C library, matrix math and compiler helpers
# win (they touch no hardware); its other WEAK definitions (OSInit, the arena, time, caches,
# interrupts, console output, __sys_alloc...) are SDK replacements and replace the game's too
LIBRARY_FALLBACKS = ("libc.c", "mtx.c", "intrinsics.c")


def hle_from_runtime(map_path, src_dir, functions):
    """Every function an HLE build's runtime_* objects define that the game has by name, minus
    the runtime's library-level fallbacks (LIBRARY_FALLBACKS): a game without a decomp build
    runs its own machine code for everything, so this is what the decomp build takes from the
    runtime (whose SDK units the decomp builds exclude)."""
    runtime = set()
    with open(map_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = MAP_RE.search(line)
            if m and m.group(1).startswith("runtime_"):
                runtime.add(m.group(2))
    weak = set()
    if src_dir:
        import glob
        for path in glob.glob(os.path.join(src_dir, "**", "*.c"), recursive=True):
            text = open(path, encoding="utf-8", errors="replace").read()
            # any file's FALLBACK definitions: stand-ins for library code the game links itself
            weak |= set(re.findall(r"^FALLBACK\s+[\w\s\*]*?\b(\w+)\s*\(", text, re.M))
            if os.path.basename(path) not in LIBRARY_FALLBACKS:
                continue
            weak |= set(re.findall(r"^WEAK\s+[\w\s\*]*?\b(\w+)\s*\(", text, re.M))
    return (runtime - weak) & functions


def write_modules(f, config_path):
    """The REL modules of a dtk config.yml: per module (its id from dtk's fn_<id>_ / lbl_<id>_ names)
        module <id> <name> <section names, in the order of the REL's non-empty sections>
        mfunc <id> <section> <offset> <size> <name>
        mlabel <id> <section> <offset> <name>
        mtable <id> <section> <offset> <size>      sized data objects (switch tables)
    Section-relative: modules are placed wherever the game links them."""
    root = os.path.dirname(os.path.abspath(config_path))
    while os.path.basename(root) != "config" and os.path.dirname(root) != root:
        root = os.path.dirname(root)
    root = os.path.dirname(root)
    text = open(config_path, encoding="utf-8").read()
    count = 0
    for block in re.split(r"\n- ", text.split("\nmodules:", 1)[1] if "\nmodules:" in text else ""):
        name = re.search(r"name:\s*(\S+)", block)
        sym = re.search(r"symbols:\s*(\S+)", block)
        spl = re.search(r"splits:\s*(\S+)", block)
        if not (name and sym and spl):
            continue
        syms = read_symbols(os.path.join(root, sym.group(1)))
        head = open(os.path.join(root, spl.group(1)), encoding="utf-8").read().split("\n\n")[0]
        sections = re.findall(r"^\s+(\.\w+)", head, re.M)
        ids = [int(m.group(1)) for s in syms for m in [re.match(r"^(?:fn|lbl)_(\d+)_", s["name"])] if m]
        if not ids:
            continue
        mid = max(set(ids), key=ids.count)
        f.write("module %d %s %s\n" % (mid, name.group(1), " ".join(sections)))
        for s in sorted(syms, key=lambda s: (s["section"], s["addr"])):
            if s["kind"] == "function" and s["size"]:
                f.write("mfunc %d %s 0x%X 0x%X %s\n" % (mid, s["section"], s["addr"], s["size"], s["name"]))
            elif s["kind"] == "label" and s["section"] == ".text":
                f.write("mlabel %d %s 0x%X %s\n" % (mid, s["section"], s["addr"], s["name"]))
            elif s["kind"] == "object" and s["size"] and s["section"] in (".data", ".rodata"):
                f.write("mtable %d %s 0x%X 0x%X\n" % (mid, s["section"], s["addr"], s["size"]))
        count += 1
    return count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbols", required=True)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--map", help="the decomp build's wasm-ld map (HLE set from runtime_* objects)")
    ap.add_argument("--hle", help="a saved HLE list (one name per line)")
    ap.add_argument("--write-hle", help="save the HLE list here")
    ap.add_argument("--placed", help="the decomp build's placed data symbols (gcn_place.py's <name>.syms)")
    ap.add_argument("--names", action="append", default=[],
                    help="'0xADDR name' lines naming functions the decomp leaves unnamed (gcn_sdkmatch.py's "
                         "output, a package's hand-checked names.txt); 'stub 0xADDR value' lines make a function "
                         "return value instead of running (hardware the runtime does not have); 'native name' "
                         "lines hand a function to the recomp runtime's own implementation (recomp_native.c)")
    ap.add_argument("--runtime-map", help="an HLE build's link map (Runtime/tools/recomp/gcn_hle_build.py): without "
                                          "a decomp build, the HLE set is every function the runtime defines, "
                                          "except its weak fallbacks (--runtime-src), that the game has by name")
    ap.add_argument("--runtime-src", help="com.recomp.gcn's Runtime/guest (its library-level WEAK definitions are "
                                          "fallbacks the game's own code beats)")
    ap.add_argument("--modules", help="the decomp's dtk config.yml: its REL modules' symbols too (`module` / `mfunc` / "
                                      "`mlabel` / `mtable` lines, section-relative; a Live build recompiles each "
                                      "module when the game links it)")
    args = ap.parse_args()

    dol = open(args.dol, "rb").read()
    text, entry = dol_sections(dol)
    syms = read_symbols(args.symbols)
    in_text = lambda a: any(base <= a < base + size for base, size, _ in text)
    stubs = {}
    native = set()
    logs = set()  # the game's printf-style log functions: to the runtime's OSReport (`log` lines)
    for path in args.names:
        rename = {}
        for line in open(path, encoding="utf-8"):
            f = line.split()
            if len(f) >= 3 and f[0] == "stub":
                stubs[int(f[1], 0)] = int(f[2], 0)
            elif len(f) >= 2 and f[0] == "native":
                native.add(f[1])
            elif len(f) >= 2 and f[0] == "log":
                logs.add(f[1])
            elif len(f) >= 2 and f[0].startswith("0x"):
                rename[int(f[0], 0)] = f[1]
        for s in syms:
            if s["kind"] == "function" and s["addr"] in rename:
                s["name"] = rename[s["addr"]]

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
    elif args.runtime_map:
        hle = hle_from_runtime(args.runtime_map, args.runtime_src, names)
    elif args.hle:
        with open(args.hle) as f:
            hle = {l.strip() for l in f if l.strip() and not l.startswith("#")} & names
    missing = sorted(native - names)
    if missing:
        raise SystemExit("gcn_syms: native functions the game does not have by name: " + " ".join(missing))
    hle |= native | (logs & names)
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
        for n in sorted(native):
            f.write("native %s\n" % n)
        for n in sorted(logs & names):
            f.write("log %s\n" % n)
        for addr in sorted(stubs):
            f.write("stub 0x%08X 0x%X\n" % (addr, stubs[addr] & 0xFFFFFFFF))
        placed = read_placed(args.placed, dol_data_ranges(dol)) if args.placed else {}
        for n, (addr, size) in sorted(placed.items(), key=lambda x: (x[1][0], x[0])):
            f.write("data 0x%08X 0x%X %s\n" % (addr, size, n))
        modules = write_modules(f, args.modules) if args.modules else 0
    print("%d functions, %d labels, %d jump tables, %d HLE, %d data, %d modules, r2=%s r13=%s" %
          (len(funcs), len(labels), len(tables), len(hle), len(placed), modules, hex(bases.get(2, 0)),
           hex(bases.get(13, 0))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
