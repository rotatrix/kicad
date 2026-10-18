#!/usr/bin/env python3
"""Extract and check the actual distributable, without launching any GUI apps."""
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile
import tempfile


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def check(app, arch):
    app = app.resolve()
    for relative in (
        'Contents/MacOS/kicad', 'Contents/MacOS/kicad-cli',
        'Contents/Frameworks/Python.framework', 'Contents/Resources/Licenses',
        'Contents/PlugIns/3d', 'Contents/PlugIns/sim',
        'Contents/SharedSupport/symbols', 'Contents/SharedSupport/footprints',
        'Contents/SharedSupport/3dmodels', 'Contents/SharedSupport/template',
        'Contents/SharedSupport/help', 'Contents/SharedSupport/internat',
    ):
        path = app / relative
        assert path.exists(), f'Missing {relative}'
        if path.is_dir():
            assert any(path.iterdir()), f'Empty {relative}'
    for editor in ('eeschema', 'pcbnew', 'gerbview', 'bitmap2component', 'pcb_calculator', 'pl_editor'):
        assert (app / f'Contents/Applications/{editor}.app/Contents/MacOS/{editor}').is_file(), editor
    count = 0
    for path in app.rglob('*'):
        if path.is_symlink():
            assert path.exists(), f'Broken link: {path}'
            assert path.resolve().is_relative_to(app), f'Escaping link: {path}'
            continue
        if not path.is_file():
            continue
        with path.open('rb') as stream:
            magic = stream.read(4)
        if magic not in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
            continue
        count += 1
        assert arch in run('lipo', '-archs', str(path)).split(), f'Wrong architecture: {path}'
        for line in run('otool', '-L', str(path)).splitlines()[1:]:
            dependency = line.strip().split(' (', 1)[0]
            assert not dependency.startswith('/') or dependency.startswith(('/System/Library/', '/usr/lib/')), f'External dependency: {path}: {dependency}'
    assert count > 10, 'Missing native components'
    run('codesign', '--verify', '--deep', '--strict', str(app))
    print(run(str(app / 'Contents/MacOS/kicad-cli'), 'version'))
    versions = list((app / 'Contents/Frameworks/Python.framework/Versions').glob('3.*'))
    assert len(versions) == 1, f'Ambiguous Python versions: {versions}'
    env = dict(os.environ, PYTHONHOME=str(versions[0]), PYTHONNOUSERSITE='1', PYTHONDONTWRITEBYTECODE='1')
    env.pop('PYTHONPATH', None)
    print(run(str(versions[0] / 'bin/python3'), '-c',
              'import wx, pcbnew, ssl, sqlite3, ctypes; print(wx.version()); print(pcbnew.GetBuildVersion())', env=env))
    print(f'Checked {count} Mach-O files, complete resources and Python imports: {arch}')


def main():
    archive, arch = sys.argv[1:]
    assert arch in ('arm64', 'x86_64')
    with tempfile.TemporaryDirectory(prefix='kicad-package-') as temporary:
        with tarfile.open(archive, 'r:gz') as source:
            for member in source.getmembers():
                parts = PurePosixPath(member.name).parts
                assert parts and parts[0] == 'KiCad.app' and '..' not in parts, member.name
            source.extractall(temporary, filter='data')
        check(Path(temporary) / 'KiCad.app', arch)


if __name__ == '__main__':
    main()
