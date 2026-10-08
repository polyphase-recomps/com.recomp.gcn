#!/usr/bin/env python3
"""Builds a GameCube game's guest module: decomp C -> big-endian IR -> wasm -> C.

    gcn_build.py <game Native dir> [--config Release] [--jobs N] [--keep-going] [--only STEP]

Reads <Native>/gcn_game.json (see com.recomp.gcn/README.md). Steps, each incremental:

  sources   decomp units (+ game patches, + runtime guest sources, + game mods)
  patch     Native/patches/*.patch applied to copies (tools/apply_patches.py)
  dialect   Metrowerks constructs rewritten (tools/gcn_sources.py)
  ir        clang, 32-bit big-endian target -> LLVM IR (armeb: the GameCube's type layout)
  be        tools/gcn_ir.py: retarget to wasm32, memory kept big-endian
  obj       clang --target=wasm32 -c
  link      wasm-ld -> <name>.wasm (SDK functions the runtime implements are imports)
  c         wasm2c -> C files for the host compilers (tools/gcn_wasm_to_c.py)

Output: <Native>/build/<config>/ (intermediates) and the C module in the directory the
config names as "output" (default <Native>/build/<config>/guest).
"""
import argparse
import collections
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

TOOLS = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import gcn_ir  # noqa: E402

EXE = '.exe' if os.name == 'nt' else ''


# ---- toolchain ----------------------------------------------------------------------
def find_up(start, pattern):
    d = os.path.abspath(start)
    for _ in range(12):
        hits = sorted(glob.glob(os.path.join(d, 'Tools', pattern)))
        if hits:
            return hits[-1]
        nd = os.path.dirname(d)
        if nd == d:
            break
        d = nd
    return None


def front_clang():
    """A clang with an ARM (big-endian) backend: Visual Studio's on Windows, else PATH."""
    env = os.environ.get('GCN_FRONT_CLANG')
    if env:
        return env
    if os.name == 'nt':
        vswhere = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                               'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
        if os.path.exists(vswhere):
            vs = subprocess.run([vswhere, '-latest', '-products', '*', '-property', 'installationPath'],
                                capture_output=True, text=True).stdout.strip()
            c = os.path.join(vs, 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang.exe')
            if os.path.exists(c):
                return c
    c = shutil.which('clang')
    if c:
        return c
    raise SystemExit('gcn_build: no clang with an ARM target found (set GCN_FRONT_CLANG)')


class Toolchain:
    def __init__(self, native_dir):
        self.front = front_clang()
        wasi = os.environ.get('GCN_WASI_SDK') or find_up(native_dir, 'wasi-sdk-*')
        wabt = os.environ.get('GCN_WABT') or find_up(native_dir, 'wabt-*')
        if not wasi or not wabt:
            raise SystemExit('gcn_build: wasi-sdk and wabt are needed in a Tools/ folder above the project '
                             '(or GCN_WASI_SDK / GCN_WABT)')
        self.wclang = os.path.join(wasi, 'bin', 'clang' + EXE)
        self.wasm_ld = os.path.join(wasi, 'bin', 'wasm-ld' + EXE)
        self.wasm2c = os.path.join(wabt, 'bin', 'wasm2c' + EXE)
        self.wasi = wasi
        wabt_dll_path(self.wasm2c)


WABT_DLL = 'libcrypto-3-x64.dll'


def wabt_dll_path(wasm2c):
    """The Windows wabt release links OpenSSL (libcrypto-3-x64.dll) without shipping it. A Git Bash
    shell has it on PATH (Git's mingw64/bin), a process the editor starts does not: wasm2c then
    exits with 0xC0000135 (DLL not found) and no output. Put a folder that has it on PATH."""
    if os.name != 'nt':
        return
    dirs = [os.path.dirname(wasm2c)] + os.environ.get('PATH', '').split(os.pathsep)
    if any(d and os.path.isfile(os.path.join(d, WABT_DLL)) for d in dirs):
        return
    found = []
    git = shutil.which('git')
    if git:
        root = os.path.dirname(os.path.dirname(os.path.realpath(git)))  # <Git>/cmd/git.exe
        found.append(os.path.join(root, 'mingw64', 'bin'))
    for base in (os.environ.get('ProgramFiles'), os.environ.get('ProgramW6432'), os.environ.get('LOCALAPPDATA')):
        if base:
            found.append(os.path.join(base, 'Git', 'mingw64', 'bin'))
            found.append(os.path.join(base, 'Programs', 'Git', 'mingw64', 'bin'))
    for d in found:
        if os.path.isfile(os.path.join(d, WABT_DLL)):
            os.environ['PATH'] = d + os.pathsep + os.environ.get('PATH', '')
            return
    print('gcn_build: warning: %s (needed by %s) was not found next to it, on PATH or in a Git '
          'install; copy it into %s' % (WABT_DLL, os.path.basename(wasm2c), os.path.dirname(wasm2c)))


# ---- helpers ------------------------------------------------------------------------
def mtime(p):
    try:
        return os.path.getmtime(p)
    except OSError:
        return 0


def write_if_changed(path, data):
    if isinstance(data, str):
        data = data.encode('utf-8')
    if os.path.exists(path) and open(path, 'rb').read() == data:
        return False
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)
    return True


def read_deps(depfile):
    try:
        text = open(depfile, encoding='utf-8', errors='replace').read()
    except OSError:
        return None
    text = text.replace('\\\n', ' ')
    _, _, rest = text.partition(': ')
    deps = []
    for tok in re.findall(r'(?:\\ |[^ \n])+', rest):
        deps.append(tok.replace('\\ ', ' '))
    return deps


def run(cmd, cwd=None):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors='replace')
    return p.returncode, (p.stdout or '') + (p.stderr or '')


# ---- the build ----------------------------------------------------------------------
class Unit:
    def __init__(self, key, src, origin, kind):
        self.key = key          # stable id: 'decomp/main/camera.c', 'runtime/libc.c', 'mods/x.c'
        self.src = src          # file actually compiled before the dialect step
        self.origin = origin    # original file (for -iquote and messages)
        self.kind = kind        # 'decomp' | 'runtime' | 'mod' | 'game'
        stem = re.sub(r'[^\w]+', '_', key)
        self.stem = stem


class Build:
    def __init__(self, native, config, jobs, keep_going):
        self.native = os.path.abspath(native)
        self.cfg = json.load(open(os.path.join(self.native, 'gcn_game.json'), encoding='utf-8'))
        # Native/local.json (not in git; the editor's Tools > Recomp > GameCube > Pre Process Rom
        # writes it): this machine's "decomp" and "disc", absolute or relative to Native/
        local = os.path.join(self.native, 'local.json')
        self.local = json.load(open(local, encoding='utf-8')) if os.path.isfile(local) else {}
        self.config = config
        self.jobs = jobs
        self.keep_going = keep_going
        self.out = os.path.join(self.native, 'build', config)
        self.tc = Toolchain(self.native)
        # GCN_DECOMP (build.ps1 -Decomp, build.sh DECOMP=) overrides gcn_game.json's "decomp"
        self.decomp = os.path.normpath(os.environ.get('GCN_DECOMP') or
                                       os.path.join(self.native, self.local.get('decomp') or self.cfg['decomp']))
        self.version = self.cfg.get('version', '')
        self.errors = []
        self.disc = os.environ.get('GCN_DISC') or (os.path.join(self.native, self.local['disc']) if self.local.get('disc')
                                                   else os.path.join(self.decomp, self.cfg.get('disc', '')))

    def extract(self):
        """Data the decomp includes from the retail executable (dtk `extract:`), from the disc."""
        out = os.path.join(self.out, 'extract')
        if not os.path.isfile(self.disc):
            raise SystemExit('gcn_build: disc image not found: %s (set "disc" in gcn_game.json or GCN_DISC)'
                             % self.disc)
        rc, text = run([sys.executable, os.path.join(TOOLS, 'gcn_extract.py'), self.decomp, self.version,
                        self.disc, out])
        if text.strip():
            print(text.strip())
        if rc:
            raise SystemExit('gcn_build: extraction failed')
        return out

    def fmt(self, s):
        return s.replace('{version}', self.version)

    # -- sources --
    def units(self):
        cfg = self.cfg
        units = []
        lst = os.path.join(self.decomp, self.fmt(cfg['units']['list']))
        exclude = [self.fmt(x) for x in cfg['units'].get('exclude', [])]
        include_extra = [self.fmt(x) for x in cfg['units'].get('include', [])]
        names = [l.strip() for l in open(lst, encoding='utf-8') if l.strip() and not l.startswith('#')]
        names += [n for n in include_extra if n not in names]
        src_root = os.path.join(self.decomp, cfg['units'].get('root', 'src'))
        for n in names:
            if not n.endswith('.c'):
                continue
            if any(n == x or (x.endswith('/') and n.startswith(x)) for x in exclude) and n not in include_extra:
                continue
            units.append(Unit('decomp/' + n, os.path.join(src_root, n), os.path.join(src_root, n), 'decomp'))
        for f in sorted(glob.glob(os.path.join(RUNTIME, 'guest', '**', '*.c'), recursive=True)):
            rel = os.path.relpath(f, os.path.join(RUNTIME, 'guest')).replace('\\', '/')
            if rel.split('/')[0] in cfg.get('runtime_exclude', []):
                continue
            units.append(Unit('runtime/' + rel, f, f, 'runtime'))
        for sub, kind in (('game', 'game'), ('mods', 'mod')):
            for f in sorted(glob.glob(os.path.join(self.native, sub, '**', '*.c'), recursive=True)):
                rel = os.path.relpath(f, self.native).replace('\\', '/')
                units.append(Unit(rel, f, f, kind))
        return units

    def apply_patches(self, units):
        """Copies of the files the game's patches touch, in <out>/patched/<path in decomp>."""
        patches = sorted(glob.glob(os.path.join(self.native, 'patches', '*.patch')))
        pdir = os.path.join(self.out, 'patched')
        touched = set()
        for p in patches:
            for line in open(p, encoding='utf-8', errors='replace'):
                if line.startswith('+++ '):
                    rel = line[4:].strip().split('\t')[0]
                    touched.add(rel[2:] if rel.startswith('b/') else rel)
        # forget files a removed patch used to touch
        for root, _, files in os.walk(pdir):
            for f in files:
                rel = os.path.relpath(os.path.join(root, f), pdir).replace('\\', '/')
                if rel not in touched:
                    os.remove(os.path.join(root, f))
        if patches:
            rc, out = run([sys.executable, os.path.join(TOOLS, 'apply_patches.py'), self.decomp, pdir] + patches)
            if rc:
                raise SystemExit(out)
        for u in units:
            if u.kind != 'decomp':
                continue
            rel = os.path.relpath(u.src, self.decomp).replace('\\', '/')
            if rel in touched:
                u.src = os.path.join(pdir, rel)
        return pdir

    def mod_names(self, units):
        return [os.path.splitext(os.path.basename(u.src))[0] for u in units if u.kind == 'mod']

    # -- compile flags --
    def front_flags(self, pdir):
        cfg = self.cfg
        incs = [os.path.join(RUNTIME, 'include'), os.path.join(self.out, 'extract')]
        if os.path.isdir(os.path.join(self.native, 'include')):
            incs.append(os.path.join(self.native, 'include'))
        # patched headers shadow the decomp's
        for inc in cfg.get('include', []):
            incs.append(os.path.join(pdir, self.fmt(inc)))
            incs.append(os.path.join(self.decomp, self.fmt(inc)))
        # "sdk_include": Dolphin SDK headers from elsewhere (a decomp that has only some of them;
        # the runtime's SDK replacement needs dolphin.h): relative to Native/, or absolute;
        # local.json's wins (this machine's paths)
        for inc in self.local.get('sdk_include') or cfg.get('sdk_include', []):
            incs.append(os.path.join(self.native, self.fmt(inc)))
        flags = ['--target=armeb-none-eabi', '-mfloat-abi=soft', '-fno-short-enums', '-O2', '-Xclang', '-disable-llvm-passes', '-S', '-emit-llvm',
                 '-ffreestanding', '-fno-builtin', '-fno-strict-aliasing', '-fwrapv', '-fno-delete-null-pointer-checks',
                 '-fno-vectorize', '-fno-slp-vectorize', '-fcommon', '-fgnu89-inline', '-std=gnu99', '-fshort-wchar',
                 '-ffp-contract=off', '-nostdinc', '-w', '-ferror-limit=20',
                 '-Wno-implicit-function-declaration', '-Wno-int-conversion', '-Wno-incompatible-pointer-types',
                 '-Wno-return-type',
                 '-include', os.path.join(RUNTIME, 'include', 'gcn_prelude.h')]
        flags.append('-fsigned-char' if cfg.get('signed_char', True) else '-funsigned-char')
        flags += ['-I' + i for i in incs]
        flags += ['-D' + self.fmt(d) for d in cfg.get('defines', [])]
        flags += cfg.get('cflags', [])
        return flags

    # -- per unit --
    def compile_unit(self, u, flags, flags_hash, weak_file):
        d = self.out
        fixed = os.path.join(d, 'src', u.stem + '.c')
        ll = os.path.join(d, 'ir', u.stem + '.ll')
        dep = os.path.join(d, 'ir', u.stem + '.d')
        stamp = os.path.join(d, 'ir', u.stem + '.flags')
        wll = os.path.join(d, 'wir', u.stem + '.ll')
        obj = os.path.join(d, 'obj', u.stem + '.o')
        # dialect
        if u.kind == 'decomp' or u.kind == 'game' or u.kind == 'mod':
            rc, out = run([sys.executable, os.path.join(TOOLS, 'gcn_sources.py'), u.src, fixed])
            if rc:
                return u, 'dialect', out
            cc_src = fixed
        else:
            cc_src = u.src
        # ir
        need = not os.path.exists(ll) or not os.path.exists(stamp) or open(stamp).read() != flags_hash
        if not need:
            deps = read_deps(dep)
            t = mtime(ll)
            if deps is None or any(mtime(x) > t for x in deps):
                need = True
        if need:
            os.makedirs(os.path.dirname(ll), exist_ok=True)
            rc, out = run([self.tc.front] + flags + ['-iquote', os.path.dirname(u.origin), '-MD', '-MF', dep,
                                                      cc_src, '-o', ll])
            if rc:
                return u, 'ir', out
            write_if_changed(stamp, flags_hash)
        return u, 'ok', ''

    def lower_unit(self, u, weak_file, sig_file):
        d = self.out
        ll = os.path.join(d, 'ir', u.stem + '.ll')
        wll = os.path.join(d, 'wir', u.stem + '.ll')
        obj = os.path.join(d, 'obj', u.stem + '.o')
        tool_t = max(mtime(os.path.join(TOOLS, 'gcn_ir.py')), mtime(weak_file), mtime(sig_file))
        if mtime(wll) < max(mtime(ll), tool_t):
            os.makedirs(os.path.dirname(wll), exist_ok=True)
            rc, out = run([sys.executable, os.path.join(TOOLS, 'gcn_ir.py'), ll, wll, '--unit-id=' + u.stem,
                           '--weak-list=' + weak_file, '--signatures=' + sig_file])
            if rc:
                return u, 'be', out
        if mtime(obj) < mtime(wll):
            # optimised IR, its byte swaps made host calls (gcn_ir.late_bswaps), then code
            opt = os.path.join(d, 'wopt', u.stem + '.ll')
            os.makedirs(os.path.dirname(obj), exist_ok=True)
            os.makedirs(os.path.dirname(opt), exist_ok=True)
            wasm_flags = ['--target=wasm32', '-O2', '-mnontrapping-fptoint', '-mbulk-memory', '-Wno-override-module']
            rc, out = run([self.tc.wclang] + wasm_flags + ['-S', '-emit-llvm', wll, '-o', opt])
            if rc:
                return u, 'obj', out
            rc, out = run([sys.executable, os.path.join(TOOLS, 'gcn_ir.py'), '--late-bswaps', opt, opt])
            if rc:
                return u, 'obj', out
            rc, out = run([self.tc.wclang] + wasm_flags + ['-Xclang', '-disable-llvm-passes', '-c', opt, '-o', obj])
            if rc:
                return u, 'obj', out
        return u, 'ok', ''

    def report(self, results, step):
        bad = [(u, k, out) for u, k, out in results if k != 'ok']
        for u, k, out in bad:
            msg = '\n'.join(l for l in out.splitlines() if 'error' in l)[:1500] or out[-1500:]
            print('[%s] %s: %s FAILED\n%s' % (step, u.key, k, msg))
        if bad:
            self.errors += [u.key for u, _, _ in bad]
            if not self.keep_going:
                raise SystemExit('gcn_build: %d units failed in step %s' % (len(bad), step))
        return [u for u, k, _ in results if k == 'ok']

    def build(self, only=None, addon=False):
        os.makedirs(self.out, exist_ok=True)
        self.extract()
        units = self.units()
        pdir = self.apply_patches(units)
        self.write_mods_list(units)
        units = self.units_with_generated(units)
        flags = self.front_flags(pdir)
        flags_hash = hashlib.sha1(json.dumps(flags).encode()).hexdigest()
        print('gcn_build: %d units' % len(units))
        with ThreadPoolExecutor(self.jobs) as pool:
            res = list(pool.map(lambda u: self.compile_unit(u, flags, flags_hash, None), units))
        units = self.report(res, 'compile')
        # program-wide list of functions several units define (header inlines)
        weak_file = os.path.join(self.out, 'weak.txt')
        # and the signatures of its functions (calls through K&R or mismatched declarations
        # are made against the definition; several different definitions: left alone)
        sig_file = os.path.join(self.out, 'signatures.txt')
        cnt = collections.Counter()
        sigs = {}
        for u in units:
            text = open(os.path.join(self.out, 'ir', u.stem + '.ll'), encoding='utf-8').read()
            cnt.update(set(gcn_ir.defined_functions(text)))
            for name, sig in gcn_ir.function_signatures(text).items():
                sigs[name] = sig if sigs.get(name, sig) == sig else None
        write_if_changed(weak_file, '\n'.join(sorted(n for n, c in cnt.items() if c > 1)) + '\n')
        write_if_changed(sig_file, gcn_ir.write_signatures(sig_file, sigs))
        with ThreadPoolExecutor(self.jobs) as pool:
            res = list(pool.map(lambda u: self.lower_unit(u, weak_file, sig_file), units))
        units = self.report(res, 'lower')
        if only == 'obj':
            return
        self.link(units)
        if only == 'link':
            return
        self.to_c()
        if addon:
            self.to_c(addon=True)
            self.unpack_disc()

    def unpack_disc(self):
        """The disc as files in the game package's Assets/Disc (git-ignored), where the
        player reads it and packaging picks it up: packaged games carry their data without
        a path to the user's disc image. Done once; files already there are kept."""
        out = os.path.normpath(os.path.join(self.native, '..', 'Assets', 'Disc'))
        idx = os.path.join(out, 'disc.idx')
        if not os.path.isfile(self.disc):
            raise SystemExit('gcn_build: disc image not found: %s (set "disc" in gcn_game.json or GCN_DISC)'
                             % self.disc)
        # again when asked to restore the original files (GCN_RESTORE_DISC=1, the editor's
        # "Restore the original disc files") or when the disc is another game or version
        restore = os.environ.get('GCN_RESTORE_DISC') == '1'
        with open(self.disc, 'rb') as f:
            want = f.read(6).decode('ascii', 'replace')
        have = open(idx, encoding='utf-8').readline().split()[-1] if os.path.isfile(idx) else ''
        if have == want and not restore:
            return
        print('gcn_build: unpacking the disc (%s) into %s' % (want, out))
        cmd = [sys.executable, os.path.join(TOOLS, 'gcn_disc.py'), 'unpack', self.disc, out]
        if restore or (have and have != want):
            cmd.append('--force')
        rc, text = run(cmd)
        if text.strip():
            print(text.strip().splitlines()[-1])
        if rc != 0:
            raise SystemExit('gcn_build: unpacking the disc failed')

    def write_mods_list(self, units):
        names = self.mod_names(units)
        text = '/* Generated by gcn_build.py from Native/mods/*.c */\n'
        text += ''.join('void mod_%s_init(void);\n' % n for n in names)
        text += 'void gcn_mods_init_all(void)\n{\n' + ''.join('    mod_%s_init();\n' % n for n in names) + '}\n'
        write_if_changed(os.path.join(self.out, 'gen', 'gcn_mods_list.c'), text)

    def units_with_generated(self, units):
        f = os.path.join(self.out, 'gen', 'gcn_mods_list.c')
        return units + [Unit('gen/gcn_mods_list.c', f, f, 'runtime')]

    def raw_objects(self):
        """guest_raw/*.c: compiled for wasm directly (see guest_raw/entry.c)."""
        objs = []
        for f in sorted(glob.glob(os.path.join(RUNTIME, 'guest_raw', '*.c'))):
            obj = os.path.join(self.out, 'obj', 'raw_' + os.path.splitext(os.path.basename(f))[0] + '.o')
            if mtime(obj) < mtime(f):
                os.makedirs(os.path.dirname(obj), exist_ok=True)
                rc, out = run([self.tc.wclang, '--target=wasm32', '-O2', '-mnontrapping-fptoint', '-mbulk-memory',
                               '-ffreestanding', '-nostdlib', '-c', f, '-o', obj])
                if rc:
                    raise SystemExit('gcn_build: %s\n%s' % (f, out))
            objs.append(obj)
        return objs

    def link(self, units):
        objs = [os.path.join(self.out, 'obj', u.stem + '.o') for u in units] + self.raw_objects()
        wasm = os.path.join(self.out, self.cfg['name'] + '.wasm')
        imports = os.path.join(RUNTIME, 'tools', 'gcn_imports.txt')
        if mtime(wasm) >= max([mtime(o) for o in objs] + [mtime(imports)]):
            print('gcn_build: %s up to date' % os.path.basename(wasm))
            return
        rsp = os.path.join(self.out, 'link.rsp')
        write_if_changed(rsp, '\n'.join('"%s"' % o.replace('\\', '/') for o in objs))
        base = int(self.cfg.get('data_base', '0x80003100'), 16)
        mem_end = int(self.cfg.get('memory_end', '0x81800000'), 16)
        # --no-gc-sections: a function only reached through calls with mismatching signatures
        # (K&R declarations) must survive; gcn_wasm_to_c.py points those calls at it
        # --no-stack-first: the default puts the stack at linear 0, which is a mirror of
        # console RAM (0x80000000) and would overlap the game's data
        # --emit-relocs: gcn_place.py moves the decomp's globals to their DOL addresses
        cmd = [self.tc.wasm_ld, '--no-entry', '--no-gc-sections', '--no-stack-first', '--emit-relocs', '--export=gcn_game_entry', '--export=__stack_pointer',
               '--allow-undefined-file=' + imports, '--error-limit=0',
               '--global-base=%d' % base, '--initial-memory=%d' % mem_end, '--max-memory=%d' % mem_end,
               '-z', 'stack-size=%d' % int(self.cfg.get('main_stack', '0x40000'), 16),
               '--Map=' + os.path.join(self.out, self.cfg['name'] + '.map'),
               '@' + rsp, '-o', wasm]
        if self.cfg.get('allow_undefined'):
            cmd.insert(1, '--unresolved-symbols=import-dynamic')
        rc, out = run(cmd)
        lines = [l for l in out.splitlines() if 'signature mismatch' not in l and not l.startswith('>>>') and l.strip()]
        open(os.path.join(self.out, 'link.log'), 'w').write(out)
        if rc:
            print('\n'.join(lines[:80]))
            raise SystemExit('gcn_build: link failed (%s)' % os.path.join(self.out, 'link.log'))
        mism = len(re.findall('signature mismatch', out))
        print('gcn_build: linked %s (%d signature mismatches, see link.log)' % (os.path.basename(wasm), mism))
        self.place(units, wasm)

    def layout_file(self, key, default):
        path = os.path.join(self.decomp, self.fmt(self.cfg.get('layout', {}).get(key, default)))
        return path if os.path.isfile(path) else None

    def place(self, units, wasm):
        """Decomp globals back at their DOL addresses (gcn_place.py), when the decomp has a dtk
        symbols.txt/splits.txt for this version. "layout": false in gcn_game.json turns it off."""
        if self.cfg.get('layout') is False:
            return
        symbols = self.layout_file('symbols', 'config/{version}/symbols.txt')
        splits = self.layout_file('splits', 'config/{version}/splits.txt')
        if not symbols or not splits:
            print('gcn_build: no symbols.txt/splits.txt for %s; globals stay where wasm-ld put them' % self.version)
            return
        root = os.path.join(self.decomp, self.cfg['units'].get('root', 'src'))
        umap = {u.stem: os.path.relpath(u.origin, root).replace('\\', '/') for u in units if u.kind == 'decomp'}
        ufile = os.path.join(self.out, 'units.json')
        write_if_changed(ufile, json.dumps(umap, indent=0, sort_keys=True))
        rc, out = run([sys.executable, os.path.join(TOOLS, 'gcn_place.py'), wasm,
                       os.path.join(self.out, self.cfg['name'] + '.map'), ufile, symbols, splits, self.disc])
        print(out.strip())
        if rc:
            os.remove(wasm)
            raise SystemExit('gcn_build: placing globals failed')

    def to_c(self, addon=False):
        """The game as C: build/<config>/guest for the standalone runner, or (addon) into
        com.recomp.gcn/Source/Guest/<name>, where the Polyphase addon compiles it and
        registers it with GcnPlayer."""
        name = self.cfg['name']
        wasm = os.path.join(self.out, name + '.wasm')
        syms = os.path.join(self.out, name + '.syms')
        if addon:
            outdir = os.path.join(RUNTIME, '..', 'Source', 'Guest', name)
        elif self.cfg.get('output'):
            outdir = os.path.join(self.native, self.cfg['output'])
        else:
            outdir = os.path.join(self.out, 'guest')
        module = os.path.join(outdir, name + '_guest_module.c')
        tool_t = max(mtime(os.path.join(TOOLS, 'gcn_wasm_to_c.py')), mtime(wasm), mtime(syms),
                     mtime(os.path.join(self.native, 'gcn_game.json')))
        if mtime(module) >= tool_t:
            print('gcn_build: %s up to date' % os.path.relpath(outdir, self.native))
            return outdir
        cmd = [sys.executable, os.path.join(TOOLS, 'gcn_wasm_to_c.py'), self.tc.wasm2c, wasm, outdir,
               name, str(self.cfg.get('c_files', 8)),
               '--package=' + self.cfg.get('package', ''), '--title=' + self.cfg.get('title', name),
               '--disc=' + os.path.basename(self.cfg.get('disc', ''))]
        if os.path.isfile(syms):
            cmd.append('--syms=' + syms)
        if addon:
            cmd += ['--register', '--runtime-include=../../Gcn/']
        rc, out = run(cmd)
        print(out.strip())
        if rc:
            raise SystemExit('gcn_build: wasm2c failed')
        return outdir


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('native')
    ap.add_argument('--config', default='Release')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--keep-going', action='store_true')
    ap.add_argument('--only', choices=['obj', 'link'])
    ap.add_argument('--addon', action='store_true',
                    help='also write the game into com.recomp.gcn/Source/Guest for the Polyphase addon')
    a = ap.parse_args()
    b = Build(a.native, a.config, a.jobs, a.keep_going)
    b.build(a.only, a.addon)
    if b.errors:
        print('gcn_build: %d units failed: %s' % (len(b.errors), ' '.join(b.errors[:40])))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
