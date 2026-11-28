#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build, package, or port the HEN-only updater. No console/SD/remote writes."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SKIP_DIRS = {'.git', 'build', '__pycache__', 'dist', 'out'}
SKIP_FILES = {'SOURCE-HASHES.json', 'HEN-SOURCE-ISOLATION.patch', 'main_previous.c'}


def user_path(value):
    value = str(value)
    if os.name != 'nt' and re.match(r'^[A-Za-z]:[\\/]', value):
        value = subprocess.check_output(['wslpath', '-u', value], text=True).strip()
    return Path(value).expanduser().resolve()


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def files(root):
    return {p.relative_to(root).as_posix(): p for p in Path(root).rglob('*')
            if p.is_file() and not p.is_symlink()
            and not any(x in SKIP_DIRS for x in p.relative_to(root).parts)
            and p.name not in SKIP_FILES
            and not p.name.endswith(('.nso', '.nro', '.elf', '.o', '.pyc'))}


def export(source, dest):
    for rel, path in files(source).items():
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        # Imported ZIP timestamps can be ahead of WSL's clock. All files in
        # this fresh build tree get current timestamps, never reference inputs.
        os.utime(target, None)


def unpack(archive, dest):
    """Reject traversal, links, duplicates and case collisions before extraction."""
    with zipfile.ZipFile(archive) as z:
        seen = set()
        total = 0
        for item in z.infolist():
            name = item.filename
            path = PurePosixPath(name)
            if ('\\' in name or path.is_absolute() or '..' in path.parts
                    or ':' in name or stat.S_ISLNK(item.external_attr >> 16)):
                raise ValueError(f'Unsupported ZIP path: {name}')
            key = name.rstrip('/').casefold()
            if key in seen:
                raise ValueError(f'Duplicate ZIP path: {name}')
            seen.add(key)
            total += item.file_size
            if total > 512 * 1024 * 1024:
                raise ValueError('ZIP exceeds the 512 MiB source/release limit.')
        z.extractall(dest)
    if (dest / 'Makefile').is_file():
        return dest
    children = list(dest.iterdir())
    if len(children) == 1 and children[0].is_dir():
        return children[0]
    return dest


def archive(source, output):
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(source.rglob('*')):
            if p.is_file():
                z.write(p, p.relative_to(source).as_posix())


def inventory(folder):
    entries = {p.relative_to(folder).as_posix(): sha(p) for p in sorted(folder.rglob('*'))
               if p.is_file() and p.name != 'SHA256SUMS.txt'}
    (folder / 'SHA256SUMS.txt').write_text(
        ''.join(f'{value}  {key}\n' for key, value in entries.items()), encoding='utf-8')
    return entries


def config(root=None):
    return json.loads(((root or ROOT) / 'release.json').read_text())


def run(command, **kwargs):
    subprocess.run([str(x) for x in command], check=True, **kwargs)


def sdk_path(value):
    candidates = [value, os.environ.get('DEVKITPRO'), '/opt/devkitpro',
                  str(Path.home() / '.local/share/hen-only-updater/sdk-20261123')]
    for candidate in candidates:
        if candidate and (user_path(candidate) / 'libounce/switch2_cpu.mk').is_file():
            sdk = user_path(candidate)
            if not (sdk / 'devkitA64/bin/aarch64-none-elf-g++').is_file():
                raise ValueError('This must be the Linux Switch 2 SDK, not the Windows .exe SDK.')
            return sdk
    raise ValueError('Switch 2 SDK not found. Use --devkitpro /path/to/devkitpro. '
                     'It must contain devkitA64, tools and libounce 20261123 or newer.')


def host_tests(tree, sdk):
    env = dict(os.environ, LIBOUNCE=str(sdk / 'libounce'))
    run(['sh', 'bootstrap/test.sh'], cwd=tree, env=env)
    run(['sh', 'hen/test.sh'], cwd=tree, env=env)
    run(['sh', 'ro/test.sh'], cwd=tree, env=env)
    run([sys.executable, 'hotupdate-controller/tests/check_hen_only.py'], cwd=tree)
    run([sys.executable, 'tools/test_transactions.py'], cwd=tree, env=env)
    run([sys.executable, 'tools/test_release.py'], cwd=tree, env=env)
    compiler = shlex.split(os.environ.get('CXX', 'g++'))
    run(compiler + ['-std=c++20', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', 'hotupdate-controller/tests/hen_request_test.cpp',
         '-o', 'build/request-test'], cwd=tree)
    run([tree / 'build/request-test'], cwd=tree)


def download_official(cfg, dest):
    url = cfg.get('official_zip_url')
    if not url:
        raise ValueError('This port needs a matching official release ZIP: use --official-zip FILE.')
    print('Downloading the pinned official companion files…', flush=True)
    with urllib.request.urlopen(url, timeout=60) as response, dest.open('wb') as out:
        shutil.copyfileobj(response, out)
    return dest


def build(args):
    cfg = config()
    if cfg.get('review_required') and not args.reviewed:
        raise ValueError('This is a new upstream port. Review PORT-REVIEW.json first; '
                         'then pass --reviewed to allow an offline build.')
    review = ROOT / 'PORT-REVIEW.json'
    if review.exists() and json.loads(review.read_text()).get('conflicts'):
        raise ValueError('Resolve the listed files and clear the conflicts list in PORT-REVIEW.json first.')
    if os.name == 'nt':
        raise ValueError('Use Build-Release.cmd to run this in WSL, or use Linux.')
    sdk = sdk_path(args.devkitpro)
    out = user_path(args.out or ROOT / 'dist' / ('kaiHEN-' + cfg['upstream_label'] + '-hen-only'))
    if out.exists():
        raise ValueError(f'Output already exists; choose a new --out folder: {out}')
    with tempfile.TemporaryDirectory(prefix='hen-release-') as temp:
        temp = Path(temp)
        tree = temp / 'source'
        export(ROOT, tree)
        (tree / 'build').mkdir()
        host_tests(tree, sdk)
        env = dict(os.environ, DEVKITPRO=str(sdk), LIBOUNCE=str(sdk / 'libounce'),
                   PATH=str(sdk / 'tools/bin') + os.pathsep + os.environ['PATH'])
        print('Building HEN, RO, bootstrap, supervisor and controller…', flush=True)
        run(['make', '-j1'], cwd=tree, env=env)
        companion = (user_path(args.official_zip) if args.official_zip
                     else download_official(cfg, temp / 'official.zip'))
        companion_hash = sha(companion)
        expected = cfg.get('official_zip_sha256')
        if expected and companion_hash != expected:
            raise ValueError('Official release ZIP checksum differs from the pinned input.')
        official = unpack(companion, temp / 'official')
        for required in ('sdcard/hbmenu.nro', 'sdcard/kaihen/bin/game/main',
                         'sdcard/kaihen/lib/libounce.so.1', 'licenses/kaiHEN-GPL-2.0.txt'):
            if not (official / required).is_file():
                raise ValueError(f'Matching companion ZIP is missing {required}')
        # Stage everything in a temporary directory. Publish only a complete build.
        package = temp / 'result'
        initial = package / 'first-install'
        shutil.copytree(official / 'sdcard', initial / 'sdcard')
        shutil.copytree(official / 'licenses', initial / 'licenses')
        binaries = {
            'hen/build/main': 'sdcard/kaihen/bin/hen/main',
            'ro/build/main': 'ro/main',
            'bootstrap/build/bootstrap.bin': 'bootstrap.bin',
            'hotupdate-supervisor/hotupdate-supervisor.nso': 'sdcard/kaihen/plugins/hotupdate-supervisor.nso',
            'hotupdate-controller/hen-updater.nro': 'sdcard/switch/hen-updater/hen-updater.nro',
        }
        for source, target in binaries.items():
            dest = initial / target
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(tree / source, dest)
        (initial / 'sdcard/kaihen/hotupdate').mkdir(parents=True, exist_ok=True)
        update = package / 'hen-update'
        staged = update / 'sdcard/kaihen/hotupdate/staged/hen/main'
        staged.parent.mkdir(parents=True)
        shutil.copyfile(tree / 'hen/build/main', staged)
        (update / 'sdcard/kaihen/hotupdate/hen-package.txt').write_text(
            'target=hen\nsha256=' + sha(staged) + '\n', encoding='ascii', newline='\n')
        # Commands are inert examples outside sdcard/. Copying the update never arms it.
        actions = update / 'commands'
        actions.mkdir()
        for name, text in {'APPLY': 'APPLY_HEN_PACKAGE_V1', 'STATUS': 'STATUS',
                           'DRY-RUN': 'DEFER_DRYRUN_V1', 'CANCEL': 'CANCEL_DEFERRED_V2'}.items():
            (actions / (name + '.txt')).write_text(text + '\n', encoding='ascii', newline='\n')
        source_out = package / 'source'
        export(ROOT, source_out)
        shutil.copyfile(ROOT / 'README_HEN_ONLY.md', package / 'START-HERE.md')
        shutil.copyfile(ROOT / 'REVIEW.md', package / 'REVIEW.md')
        shutil.copytree(official / 'licenses', update / 'licenses')
        metadata = dict(cfg, official_zip_sha256=companion_hash,
                        hardware_tested=False, host_tests='passed', cross_build='passed',
                        built_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                        compiler=subprocess.check_output(
                            [sdk / 'devkitA64/bin/aarch64-none-elf-g++', '--version'], text=True).splitlines()[0],
                        sdk_switch2_cpu_sha256=sha(sdk / 'libounce/switch2_cpu.mk'),
                        sdk_libounce_core_sha256=sha(sdk / 'libounce/lib/libounce_core.a'))
        (package / 'BUILD-INFO.json').write_text(json.dumps(metadata, indent=2) + '\n')
        for folder in (initial, update, source_out):
            inventory(folder)
            archive(folder, package / (folder.name + '.zip'))
        inventory(package)
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(package, out)
    print(f'\nFinished: {out}\nNo console or SD card was changed. Hardware test still required.')


def git_binary(value):
    if value:
        return value
    if os.name == 'nt':
        candidate = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Git/cmd/git.exe'
        if candidate.is_file():
            return str(candidate)
    found = shutil.which('git')
    if not found:
        raise ValueError('Git is needed for the three-way source merge.')
    return found


def rebase(args):
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,80}', args.revision):
        raise ValueError('Use a tag or commit for --revision, using letters, numbers, dot, dash or underscore.')
    dest = user_path(args.out)
    if dest.exists():
        raise ValueError('Choose a new output folder; source inputs will not be overwritten.')
    with tempfile.TemporaryDirectory(prefix='hen-port-') as temp:
        temp = Path(temp)
        base = unpack(ROOT / 'tools/upstream-base.zip', temp / 'base')
        supplied = user_path(args.upstream)
        upstream = supplied if supplied.is_dir() else unpack(supplied, temp / 'next')
        if not all((upstream / p).is_file() for p in ('Makefile', 'hen/source/server.cpp', 'ro/source/session.cpp')):
            raise ValueError('Select the kaiHEN source folder or GitHub source ZIP, not a binary release.')
        old, current, new = files(base), files(ROOT), files(upstream)
        new.pop('tools/upstream-base.zip', None)
        export(upstream, dest)
        conflicts, changed = [], []
        git = git_binary(args.git)
        for rel in sorted(set(old) | set(current)):
            if rel in ('tools/upstream-base.zip', 'release.json', 'PORT-REVIEW.json'):
                continue
            before = old[rel].read_bytes() if rel in old else None
            ours = current[rel].read_bytes() if rel in current else None
            theirs = new[rel].read_bytes() if rel in new else None
            if ours == before:
                continue
            changed.append(rel)
            target = dest / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            if theirs == before or theirs == ours:
                if ours is None:
                    if target.exists():
                        target.unlink()  # exact file in the fresh, checked output tree
                else:
                    target.write_bytes(ours)
            elif None in (before, ours, theirs) or any(b'\0' in x for x in (before, ours, theirs)):
                conflicts.append(rel)
                if ours is not None:
                    target.with_name(target.name + '.updater-version').write_bytes(ours)
            else:
                result = subprocess.run([git, 'merge-file', '-p', '--diff3',
                    '-L', 'new-upstream', '-L', 'previous-upstream', '-L', 'hen-updater',
                    str(new[rel]), str(old[rel]), str(current[rel])], capture_output=True)
                if result.returncode < 0 or result.returncode > 127:
                    raise ValueError(result.stderr.decode(errors='replace'))
                target.write_bytes(result.stdout)
                if result.returncode:
                    conflicts.append(rel)
        cfg = config()
        cfg.update(upstream_label=args.revision, upstream_revision=args.revision,
                   upstream_source_zip_sha256=sha(supplied) if supplied.is_file() else None,
                   official_zip_url=None, official_zip_sha256=None, review_required=True)
        (dest / 'release.json').write_text(json.dumps(cfg, indent=2) + '\n')
        with tempfile.TemporaryDirectory(dir=temp) as clean:
            export(upstream, Path(clean))
            (dest / 'tools').mkdir(exist_ok=True)
            archive(Path(clean), dest / 'tools/upstream-base.zip')
        (dest / 'hotupdate-controller/source/release_version.hpp').write_text(
            '#pragma once\n#define HEN_UPSTREAM_LABEL "' + args.revision + '"\n')
        report = {'previous_upstream': config()['upstream_revision'],
                  'new_upstream': args.revision,
                  'input_sha256': sha(supplied) if supplied.is_file() else None,
                  'conflicts': conflicts, 'updater_changed_files': changed,
                  'required_review': ['HEN control and plugin ABI', 'RO handoff and Loader mapping',
                                      'bootstrap launch geometry', 'matching companion release files'],
                  'hardware_tested': False}
        (dest / 'PORT-REVIEW.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Port prepared: {dest}\nConflicts: {len(conflicts)}. Nothing built or installed.')
    if conflicts:
        raise ValueError('Resolve every conflict in PORT-REVIEW.json before building.')


def wizard():
    print('HEN-only release helper\n1. Build this source\n2. Port to a newer source ZIP/folder')
    choice = input('Choose [1]: ').strip() or '1'
    if choice == '2':
        upstream = input('New kaiHEN source ZIP or folder: ').strip().strip('"')
        revision = input('Source tag or commit: ').strip()
        out = input('New output folder: ').strip().strip('"')
        return rebase(argparse.Namespace(upstream=upstream, revision=revision, out=out, git=None))
    if choice != '1':
        raise ValueError('Choose 1 or 2.')
    sdk = input('SDK folder (Enter to detect): ').strip() or None
    companion = input('Official binary release ZIP (Enter for pinned download): ').strip().strip('"') or None
    out = input('New output folder (Enter for dist/): ').strip().strip('"') or None
    return build(argparse.Namespace(devkitpro=sdk, official_zip=companion, out=out, reviewed=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    b = commands.add_parser('build', help='Test, compile and package this source')
    b.add_argument('--devkitpro')
    b.add_argument('--official-zip')
    b.add_argument('--out')
    b.add_argument('--reviewed', action='store_true', help='Allow an offline build of a reviewed new port')
    r = commands.add_parser('rebase', help='Three-way merge the updater onto a newer official source')
    r.add_argument('--upstream', required=True)
    r.add_argument('--revision', required=True)
    r.add_argument('--out', required=True)
    r.add_argument('--git')
    commands.add_parser('wizard', help='Guided build/port prompts')
    args = parser.parse_args()
    try:
        if args.command == 'build':
            build(args)
        elif args.command == 'rebase':
            rebase(args)
        else:
            wizard()
    except (ValueError, OSError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print(f'\nStopped: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
