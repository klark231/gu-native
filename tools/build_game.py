#!/usr/bin/env python3
"""MHGU: make the game code for the app from your own copy of the game.

    python build_game.py <folder with exefs and romfs>

The app itself holds no game code. This tool translates your exefs once and makes from it the game code for both versions,
leaving it in that same folder: libmhgugame.so (and libmhgugame.id) for the phone and mhgugame.dll (and mhgugame.id) for
MHGU.exe on PC. One that is already up to date is kept, so running it again only adds what is missing. It reads your files
only: nothing is sent anywhere, and the only downloads are the compilers you don't have, kept in ~/.mhgu-tool: Google's
Android NDK (about 1.8 GB) and llvm-mingw (about 150 MB).

Options: --android  only the phone's   --pc  only the PC's   --force  make them again even if up to date
         --ndk <path>  an NDK of your own   --llvm-mingw <path>  an llvm-mingw of your own
         --jobs <n>  compile with n jobs   --keep  keep the work folder
"""
import argparse
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
NDK_RELEASE = 'r27'
NDK_REVISION = '27.0.12077973'
API = 29
BUILD_ID = bytes.fromhex('FB08F1D20FD1204F')  # version 1.4.0's main
ID_FILES = ['tools/gen.py', 'tools/lifter.py', 'tools/neon.py', 'tools/nso.py', 'tools/gamelib_entry.c',
            'runtime/hle/hle_symbols.txt', 'runtime/cpu.h', 'runtime/vfp.h', 'runtime/neon_rt.h', 'runtime/gamelib.h']
ID_FLAGS = 'android-arm64-v8a -O2 -nossp -fno-strict-aliasing -ffp-contract=off -fno-math-errno'
# (exactly native/CMakeLists.txt's for this library, with the NDK's own)
CFLAGS = ['-DANDROID', '-fdata-sections', '-ffunction-sections', '-funwind-tables', '-no-canonical-prefixes', '-DNDEBUG',
          '-fPIC', '-O2', '-fno-strict-aliasing', '-ffp-contract=off', '-fno-math-errno', '-fvisibility=hidden',
          '-march=armv8.2-a', '-w', '-fno-stack-protector', '-U_FORTIFY_SOURCE', '-g0', '-fno-plt']
# --pc: native/build_windows.sh's GENFLAGS and native/CMakeLists.txt's flags for mhgugame.dll (x86-64-v2: SSE4.2 and
# POPCNT, any 64-bit processor from 2009 on; the game code has no fused multiply-add to need FMA, and AVX2/BMI2 measured no
# faster with the same results). The library imports the runtime's functions from the executable by its name, so it is
# MHGU.exe.
PC_ID_FLAGS = 'windows-x86_64 -O2 -march=x86-64-v2 -fno-strict-aliasing -ffp-contract=off -fno-math-errno'
PC_CFLAGS = ['-DNDEBUG', '-D_FILE_OFFSET_BITS=64', '-O2', '-fno-strict-aliasing', '-ffp-contract=off', '-fno-math-errno',
             '-fvisibility=hidden', '-march=x86-64-v2', '-w', '-fno-stack-protector', '-U_FORTIFY_SOURCE', '-g0']
PC_EXE = 'MHGU.exe'
LLVM_MINGW_RELEASE = '20241217'


def say(text):
    print(text, flush=True)


def fail(text):
    say('\nERROR: ' + text)
    sys.exit(1)


def gamelib_id(flags=ID_FLAGS):
    h = hashlib.sha256()
    for name in ID_FILES:
        with open(os.path.join(ROOT, name), 'rb') as f:
            h.update(f.read().replace(b'\r', b''))
    h.update((flags + '\n').encode())
    return h.hexdigest()[:16]


def check_game(game):
    main = os.path.join(game, 'exefs', 'main')
    if not os.path.isfile(main):
        for sub in ('data',):
            if os.path.isfile(os.path.join(game, sub, 'exefs', 'main')):
                game = os.path.join(game, sub)
                main = os.path.join(game, 'exefs', 'main')
                break
    if not os.path.isfile(main):
        fail(f'there is no exefs/main in {game}. Point to the folder that holds exefs and romfs.')
    if not os.path.isfile(os.path.join(game, 'exefs', 'sdk')):
        fail('exefs/sdk is missing.')
    with open(main, 'rb') as f:
        head = f.read(0x48)
    if head[:4] != b'NSO0' or head[0x40:0x48] != BUILD_ID:
        fail('this exefs is not version 1.4.0. Use the one from the 1.4.0 update.')
    return game


def ensure_lz4():
    try:
        import lz4.block  # noqa: F401
    except ImportError:
        say('Installing the lz4 Python package...')
        subprocess.check_call([sys.executable, '-m', 'pip', 'install', '--user', 'lz4'])


def host_tag():
    s = platform.system()
    return {'Windows': ('windows', 'windows-x86_64'), 'Linux': ('linux', 'linux-x86_64')}.get(s) or fail(f'{s} is not supported: use Windows or Linux.')


def find_ndk(given):
    names, tag = host_tag()[0], host_tag()[1]
    candidates = [given, os.environ.get('ANDROID_NDK_HOME'), os.environ.get('ANDROID_NDK_ROOT')]
    sdk = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk')
    candidates.append(os.path.join(sdk, 'ndk', NDK_REVISION))
    mine = os.path.join(os.path.expanduser('~'), '.mhgu-tool', f'android-ndk-{NDK_RELEASE}')
    candidates.append(mine)
    for c in candidates:
        if c and os.path.isdir(os.path.join(c, 'toolchains', 'llvm', 'prebuilt', tag)):
            return c
    fetch(f'https://dl.google.com/android/repository/android-ndk-{NDK_RELEASE}-{names}.zip',
          f'the Android NDK {NDK_RELEASE} from Google (about 1.8 GB, only once)', mine)
    if platform.system() != 'Windows':
        bin_dir = os.path.join(mine, 'toolchains', 'llvm', 'prebuilt', tag, 'bin')
        for f in os.listdir(bin_dir):
            os.chmod(os.path.join(bin_dir, f), 0o755)
    return mine


def fetch(url, what, mine):
    """Downloads url and unpacks it as the folder `mine` (the archive's one top folder), all or nothing: it is unpacked
    beside it first and moved there whole, so an unpack cut short (closed, a full disk) never leaves a half compiler that
    the next run would take for a whole one."""
    os.makedirs(os.path.dirname(mine), exist_ok=True)
    ext = '.zip' if url.endswith('.zip') else '.tar.xz'
    archive, part = mine + ext, mine + '.part'
    say(f'Downloading {what}...')

    def progress(blocks, size, total):
        if total > 0 and blocks % 2000 == 0:
            say(f'  {min(100, blocks * size * 100 // total)}%')
    urllib.request.urlretrieve(url, archive, progress)
    say('Unpacking it...')
    shutil.rmtree(part, ignore_errors=True)
    os.makedirs(part)
    if ext == '.zip':
        with zipfile.ZipFile(archive) as z:
            z.extractall(part)
    else:
        import tarfile
        with tarfile.open(archive) as t:
            try:
                t.extractall(part, filter='tar')
            except TypeError:  # (Python before 3.12: no filters)
                t.extractall(part)
    tops = os.listdir(part)
    shutil.rmtree(mine, ignore_errors=True)
    os.replace(os.path.join(part, tops[0]) if len(tops) == 1 else part, mine)
    shutil.rmtree(part, ignore_errors=True)
    os.remove(archive)


def find_llvm_mingw(given):
    """PC: llvm-mingw (clang for Windows programs), yours (its folder or its bin folder) or downloaded once from its GitHub
    releases, the build for this computer's system and processor."""
    system, machine = platform.system(), platform.machine().lower()
    arch = {'amd64': 'x86_64', 'x86_64': 'x86_64', 'aarch64': 'aarch64', 'arm64': 'aarch64'}.get(machine)
    if system == 'Windows':
        name, ext, exe = f'llvm-mingw-{LLVM_MINGW_RELEASE}-ucrt-{arch}', '.zip', '.exe'
    elif system == 'Linux':
        name, ext, exe = f'llvm-mingw-{LLVM_MINGW_RELEASE}-ucrt-ubuntu-20.04-{arch}', '.tar.xz', ''
    else:
        fail(f'{system} is not supported: use Windows or Linux.')

    def root_of(c):  # (a folder that holds bin/x86_64-w64-mingw32-clang, or that bin folder itself)
        if not c:
            return None
        for r in (c, os.path.dirname(os.path.normpath(c))):
            if os.path.isfile(os.path.join(r, 'bin', 'x86_64-w64-mingw32-clang' + exe)):
                return r
        return None
    mine = os.path.join(os.path.expanduser('~'), '.mhgu-tool', name)
    for c in (given, os.environ.get('LLVM_MINGW'), mine):
        if root_of(c):
            return root_of(c)
    if given:
        fail(f'there is no llvm-mingw in {given} (bin/x86_64-w64-mingw32-clang{exe} is missing).')
    if not arch:
        fail(f'there is no llvm-mingw for {machine} processors: give one of your own with --llvm-mingw.')
    fetch(f'https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_MINGW_RELEASE}/{name}{ext}',
          f'llvm-mingw {LLVM_MINGW_RELEASE}, the compiler for the PC version (about 150 MB, only once)', mine)
    return mine


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    return r.returncode, r.stdout


def link_pc(clang, nm, dlltool, objs, work):
    """--pc: links mhgugame.dll. The game code calls a few of the runtime's functions (rt_dispatch, the NEON helpers...),
    which MHGU.exe exports: a first link tells which ones nothing else provides, and an import library for MHGU.exe
    made from that list (no copy of the executable needed) lets the second one finish. Variables among them are reached
    through clang's .refptr pointers: they are imported as DATA (the loader fills those pointers)."""
    rsp = os.path.join(work, 'objs.rsp')
    with open(rsp, 'w', encoding='utf-8') as f:  # (LLVM reads response files as UTF-8: a user name with accents)
        f.write('\n'.join('"' + o.replace('\\', '/') + '"' for o in objs))
    lib = os.path.join(work, 'mhgugame.dll')
    code, out = run([clang, '-shared', '-o', lib, '@' + rsp, '-static', '-Wl,--error-limit=0'])
    names = sorted(set(re.findall(r'undefined symbol: (\S+)', out)))
    if code and not names:
        fail('linking failed:\n' + out[-1500:])
    code, out = run([nm, '--defined-only', '-j', '@' + rsp])
    if code:
        fail('reading the compiled code failed:\n' + out[-1500:])
    data = {s[len('.refptr.'):] for s in out.split() if s.startswith('.refptr.')}
    deffile = os.path.join(work, 'mhgu_exe.def')
    with open(deffile, 'w') as f:
        f.write(f'LIBRARY "{PC_EXE}"\nEXPORTS\n' + ''.join(f'  {n}{" DATA" if n in data else ""}\n' for n in names))
    implib = os.path.join(work, 'libmhgu_exe.a')
    code, out = run([dlltool, '-m', 'i386:x86-64', '-d', deffile, '-l', implib])
    if code:
        fail('linking failed:\n' + out[-1500:])
    code, out = run([clang, '-shared', '-s', '-o', lib, '@' + rsp, implib, '-static'])
    if code:
        fail('linking failed:\n' + out[-1500:])
    return lib


def link_android(clang, strip, target, objs, work):
    # (only for its name: the app's own libmain.so answers these calls on the phone)
    stub_c = os.path.join(work, 'stub.c')
    open(stub_c, 'w').close()
    stub = os.path.join(work, 'libmain.so')
    code, out = run([clang] + target + ['-shared', '-Wl,-soname,libmain.so', stub_c, '-o', stub])
    if code:
        fail('linking failed:\n' + out[-1500:])
    rsp = os.path.join(work, 'objs.rsp')
    with open(rsp, 'w', encoding='utf-8') as f:  # (LLVM reads response files as UTF-8: a user name with accents)
        f.write('\n'.join('"' + o.replace('\\', '/') + '"' for o in objs))
    lib = os.path.join(work, 'libmhgugame.so')
    code, out = run([clang] + target + ['-shared', '-fPIC', '-Wl,-soname,libmhgugame.so', '-Wl,--build-id=sha1', '-Wl,--no-rosegment',
                     '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-Wl,--allow-shlib-undefined', '-o', lib, '@' + rsp,
                     '-L' + work, '-lmain', '-latomic', '-lm'])
    if code:
        fail('linking failed:\n' + out[-1500:])
    run([strip, '--strip-unneeded', lib])
    return lib


# The libraries one run makes, each with its own id file, so one folder serves the phone and the PC
TARGETS = {
    'android': {'name': 'Android', 'lib': 'libmhgugame.so', 'id': 'libmhgugame.id', 'flags': ID_FLAGS},
    'pc': {'name': 'PC', 'lib': 'mhgugame.dll', 'id': 'mhgugame.id', 'flags': PC_ID_FLAGS},
}


def up_to_date(game, t):
    try:
        with open(os.path.join(game, t['id'])) as f:
            return f.read().strip() == gamelib_id(t['flags']) and os.path.isfile(os.path.join(game, t['lib']))
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser(description='Make the game code for the app from your own copy of the game.')
    ap.add_argument('game', help='the folder that holds exefs and romfs')
    ap.add_argument('--android', action='store_true', help='only the Android library (libmhgugame.so)')
    ap.add_argument('--pc', action='store_true', help='only the PC library (mhgugame.dll)')
    ap.add_argument('--force', action='store_true', help='make them again even if they are up to date')
    ap.add_argument('--ndk', help='an Android NDK of your own')
    ap.add_argument('--llvm-mingw', help='an llvm-mingw of your own (PC)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--keep', action='store_true', help='keep the work folder')
    a = ap.parse_args()

    t0 = time.time()
    game = check_game(os.path.abspath(a.game))
    wanted = [k for k in ('android', 'pc') if getattr(a, k)] or ['android', 'pc']
    todo = []
    for k in wanted:
        t = TARGETS[k]
        if not a.force and up_to_date(game, t):
            say(f'{t["lib"]} ({t["name"]}) is already up to date: kept.')
        else:
            todo.append(k)
    if not todo:
        say('Nothing to do.')
        return
    ensure_lz4()
    # the compilers first: a missing download stops the run before the long part
    exe = '.exe' if platform.system() == 'Windows' else ''
    tools = {}
    if 'android' in todo:
        ndk = find_ndk(a.ndk)
        bin_dir = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', host_tag()[1], 'bin')
        tools['android'] = {'clang': os.path.join(bin_dir, 'clang' + exe), 'strip': os.path.join(bin_dir, 'llvm-strip' + exe),
                            'target': [f'--target=aarch64-linux-android{API}'], 'cflags': CFLAGS}
    if 'pc' in todo:
        bin_dir = os.path.join(find_llvm_mingw(a.llvm_mingw), 'bin')
        tools['pc'] = {'clang': os.path.join(bin_dir, 'x86_64-w64-mingw32-clang' + exe), 'nm': os.path.join(bin_dir, 'llvm-nm' + exe),
                       'dlltool': os.path.join(bin_dir, 'llvm-dlltool' + exe), 'target': [], 'cflags': PC_CFLAGS}
    for k, tl in tools.items():
        if not os.path.isfile(tl['clang']):
            fail(f'no compiler at {tl["clang"]}')

    # room to work: the C it writes takes 300 MB, its objects a few hundred more
    work = tempfile.mkdtemp(prefix='mhgu_')
    if shutil.disk_usage(work).free < 3 << 30:
        fail('at least 3 GB of free disk space are needed.')
    say(f'Working in {work}')
    steps = 1 + len(todo)

    say(f'Step 1 of {steps}: translating the game code...')
    gen = os.path.join(work, 'gen')
    hle = os.path.join(ROOT, 'runtime', 'hle', 'hle_symbols.txt')
    for cmd in ([sys.executable, 'gen.py', 'sdk', os.path.join(game, 'exefs', 'sdk'), '0x02400000', os.path.join(gen, 'sdk'), '--hle', hle],
                [sys.executable, 'gen.py', 'main', os.path.join(game, 'exefs', 'main'), '0x00200000', os.path.join(gen, 'main')]):
        code, out = run(cmd, cwd=TOOLS)
        if code:
            fail('the translation failed:\n' + out[-2000:])

    sources = []
    for sub in ('main', 'sdk'):
        d = os.path.join(gen, sub)
        sources += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith('.c')]
    sources.append(os.path.join(TOOLS, 'gamelib_entry.c'))

    for step, k in enumerate(todo, 2):
        t, tl = TARGETS[k], tools[k]
        gid = gamelib_id(t['flags'])
        say(f'Step {step} of {steps}: the code for {t["name"]} ({t["lib"]}, id {gid}): compiling {len(sources)} files with '
            f'{a.jobs} jobs (this is the long part)...')
        objs_dir = os.path.join(work, 'obj_' + k)
        os.makedirs(objs_dir)
        done = [0]

        def compile_one(i_src):
            i, src = i_src
            obj = os.path.join(objs_dir, f'{i}.o')
            cmd = [tl['clang']] + tl['target'] + tl['cflags'] + ['-I' + os.path.join(ROOT, 'runtime'), f'-DGAMELIB_ID="{gid}"',
                                                                 '-c', src, '-o', obj]
            code, out = run(cmd)
            if code:
                raise RuntimeError(f'{src}:\n{out[-1500:]}')
            done[0] += 1
            if done[0] % 20 == 0 or done[0] == len(sources):
                say(f'  {done[0]} of {len(sources)}')
            return obj

        with ThreadPoolExecutor(a.jobs) as pool:
            try:
                objs = list(pool.map(compile_one, enumerate(sources)))
            except RuntimeError as e:
                fail(f'the compiler failed on {e}')
        say('  linking...')
        if k == 'pc':
            lib = link_pc(tl['clang'], tl['nm'], tl['dlltool'], objs, work)
        else:
            lib = link_android(tl['clang'], tl['strip'], tl['target'], objs, work)
        shutil.copyfile(lib, os.path.join(game, t['lib']))
        with open(os.path.join(game, t['id']), 'w') as f:
            f.write(gid)
        say(f'  {t["lib"]}: {os.path.getsize(os.path.join(game, t["lib"])) >> 20} MB')
        if not a.keep:
            shutil.rmtree(objs_dir, ignore_errors=True)

    if a.keep:
        say(f'Work folder kept: {work}')
    else:
        shutil.rmtree(work, ignore_errors=True)
    say(f'\nDone in {(time.time() - t0) / 60:.0f} minutes.')
    if 'android' in wanted:
        say(f'Android: copy {game} to your phone and choose it in the app.')
    if 'pc' in wanted:
        say(f'PC: open {PC_EXE} and choose {game}.')


if __name__ == '__main__':
    main()
