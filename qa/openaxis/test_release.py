"""Release selection must never promote a different or incomplete build."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    'release', Path(__file__).resolve().parents[2] / 'tools/prepare-release.py')
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)
SHA = 'a' * 40


class ReleaseTests(unittest.TestCase):
    def run_record(self, **changes):
        result = dict(repository={'full_name': release.REPO},
                      head_repository={'full_name': release.REPO},
                      path=release.WORKFLOW, event='workflow_dispatch',
                      head_branch='rotatrix/work/10.0.6', head_sha=SHA,
                      status='completed', conclusion='success')
        result.update(changes)
        return result

    def test_exact_successful_branch_run(self):
        release.validate_run(self.run_record(), SHA)

    def test_rejects_untrusted_or_different_builds(self):
        for changes in ({'head_sha': 'b' * 40}, {'event': 'pull_request'},
                        {'conclusion': 'failure'}, {'status': 'in_progress'},
                        {'path': '.github/workflows/other.yml'},
                        {'head_repository': {'full_name': 'outsider/kicad'}},
                        {'head_branch': 'main'}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                release.validate_run(self.run_record(**changes), SHA)

    def test_preview_tags_preserve_upstream_version(self):
        self.assertEqual(release.TAG.fullmatch('10.0.6-rotatrix.1-beta.1')[1], '10.0.6')
        for tag in ('latest', '10.0.6', '10.0.6-rotatrix.0', '10.0.6-rotatrix.1/extra'):
            self.assertIsNone(release.TAG.fullmatch(tag))

    def test_requires_both_mac_architectures_and_native_packages(self):
        self.assertEqual(release.expected(SHA), {
            f'KiCad-windows-x64-{SHA}.exe', f'KiCad-linux-x64-{SHA}.flatpak',
            f'macos-app-arm64-{SHA}.tar.gz', f'macos-app-x86_64-{SHA}.tar.gz'})

    def test_incomplete_platform_run_cannot_be_promoted(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(release, 'require_new_release'), \
                 patch.object(release, 'tag_commit', return_value=SHA), \
                 patch.object(release, 'api', side_effect=[{'status': 'identical'}, self.run_record()]), \
                 patch.object(release, 'pages', return_value=[{'name': 'Windows x64', 'conclusion': 'success'}]):
                with self.assertRaisesRegex(ValueError, 'Missing required platform'):
                    release.prepare('10.0.6-rotatrix.1-beta.1', 123, Path(tmp))

    def test_draft_uses_final_packages_without_publishing(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            records = []
            for name in release.expected(SHA):
                package = directory / name
                package.write_bytes(b'unsigned test package')
                records.append(dict(name=name, sha256=release.digest(package)))
            (directory / 'signed').mkdir()
            for arch in ('arm64', 'x86_64'):
                (directory / 'signed' / f'kicad-macos-{arch}-{SHA}.tar.gz').write_bytes(b'signed')
            plan = dict(repository=release.REPO, tag='10.0.6-rotatrix.1-beta.1',
                        commit=SHA, run_id=123, artifacts=records, prerelease=True)
            (directory / 'promotion.json').write_text(json.dumps(plan))
            with patch.object(release, 'tag_commit', return_value=SHA), \
                 patch.object(release, 'require_new_release'), \
                 patch.object(release.subprocess, 'run') as command:
                release.draft(directory)
                args = command.call_args.args[0]
                self.assertIn('--draft', args)
                self.assertIn('--verify-tag', args)
                self.assertIn('--prerelease', args)
                self.assertEqual(len((directory / 'SHA256SUMS').read_text().splitlines()), 4)
                self.assertFalse(any('macos-app-' in arg or arg.endswith('.json') for arg in args))
            with patch.object(release, 'tag_commit', return_value='b' * 40):
                with self.assertRaisesRegex(ValueError, 'Tag moved'):
                    release.draft(directory)


if __name__ == '__main__':
    unittest.main()
