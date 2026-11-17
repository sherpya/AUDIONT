#!/bin/bash
# Leaves a plain installable directory, never writes an image or physical disk.
set -euo pipefail
cd "$(dirname "$0")/.."
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git show -s --format=%ct HEAD)}
bash tools/wdmbuild.sh --arch i386 --out build/geode/2kxp
P=build/geode/package
mkdir -p "$P/2kxp/i386"
cp inf/geode.inf "$P/2kxp/audiont.inf"
for f in audiont.sys audiont.dll; do cp "build/geode/2kxp/i386/$f" "$P/2kxp/i386/"; done
for f in audiont.dll setup.exe audmix.exe audtest.exe; do cp "build/geode/2kxp/i386/$f" "$P/"; done
cp LICENSE "$P/LICENSE"
cp docs/geode.md "$P/README.md"
python3 - <<'PY'
import hashlib, json, os, pathlib, subprocess
p = pathlib.Path('build/geode/package')
def digest(f): return hashlib.sha256(f.read_bytes()).hexdigest()
def git(*args): return subprocess.check_output(['git', *args], text=True).strip()
sources = sorted(f for d in ('drv', 'inc', 'user', 'tools', 'inf', 'test')
                 for f in pathlib.Path(d).rglob('*') if f.is_file() and f.suffix in ('.c','.h','.sh','.py','.inf','.def'))
sources += [pathlib.Path(f) for f in ('Makefile', 'README.md', 'docs/geode.md')]
validated_sys = '2c6751dd41b65e7c03a58b030696cffdf064240b4702d521802569acf7075630'
data = {
    'upstream_commit': '3b78b8e4f6dec85bf707189c63a347514d978fdd',
    'source_commit': git('rev-parse', 'HEAD'),
    'source_tree_dirty': bool(git('status', '--porcelain', '--untracked-files=normal')),
    'source_date_epoch': int(os.environ['SOURCE_DATE_EPOCH']),
    'compiler': subprocess.check_output(['i686-w64-mingw32-gcc','--version'], text=True).splitlines()[0],
    'command': 'bash tools/geodebuild.sh',
    'target': 'Windows XP / XPe SP3 x86, i586, general registers only in kernel',
    'geode_revision': 3,
    'hardware_validation': {
        'cs5536': 'Working playback: Linutop 2 / FIC ION603, XP SP3 x86, Realtek ALC203; clean install, Windows sounds, WAV and MP3 via line out (2026-11-17)',
        'cs5530': 'Untested: no successful audible-output validation',
        'validated_r3_sys_sha256': validated_sys,
        'kernel_matches_validated_r3': digest(p/'2kxp/i386/audiont.sys') == validated_sys,
        'note': 'Historical hardware result; a new binary needs its own validation if the hash differs.',
    },
    'sources_sha256': {str(f): digest(f) for f in sources},
    # User-supplied logs in the package directory are not build artifacts.
    'package_sha256': {name: digest(p/name) for name in (
        '2kxp/audiont.inf', '2kxp/i386/audiont.sys', '2kxp/i386/audiont.dll',
        'audiont.dll', 'setup.exe', 'audmix.exe', 'audtest.exe', 'LICENSE', 'README.md')},
}
(p/'manifest.json').write_text(json.dumps(data, indent=2)+'\n')
PY
printf 'Geode package: %s/%s\n' "$PWD" "$P"
