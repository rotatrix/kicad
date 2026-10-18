"""Verify and stage exact CI outputs. No compilation or package rewriting."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from urllib.parse import quote

REPO = 'rotatrix/kicad'
WORKFLOW = '.github/workflows/build-kicad.yml'
JOBS = {'Windows x64', 'Linux x64', 'macOS arm64', 'macOS x86_64'}
TAG = re.compile(r'([0-9]+\.[0-9]+\.[0-9]+)-rotatrix\.[1-9][0-9]*(?:-beta\.[1-9][0-9]*)?\Z')


def require(value, message):
    if not value:
        raise ValueError(message)


def api(path):
    return json.loads(subprocess.check_output(['gh', 'api', f'repos/{REPO}/{path}'], text=True))


def pages(path, key):
    result = []
    for page in range(1, 101):
        rows = api(f'{path}{"&" if "?" in path else "?"}per_page=100&page={page}')[key]
        result.extend(rows)
        if len(rows) < 100:
            return result
    raise ValueError('Unexpected pagination limit')


def tag_commit(tag):
    obj = api('git/ref/tags/' + quote(tag, safe=''))['object']
    for _ in range(8):
        if obj['type'] == 'commit':
            return obj['sha']
        require(obj['type'] == 'tag', 'Tag does not identify a commit')
        obj = api('git/tags/' + obj['sha'])['object']
    raise ValueError('Excessively nested tag')


def validate_run(run, sha):
    require(run['repository']['full_name'] == REPO and run['head_repository']['full_name'] == REPO,
            'Build must originate in rotatrix/kicad')
    require(run['path'] == WORKFLOW, 'Unapproved build workflow')
    require(run['event'] == 'workflow_dispatch' and
            re.fullmatch(r'rotatrix/[A-Za-z0-9._/-]+', run.get('head_branch') or ''),
            'Only trusted Rotatrix branch builds can be promoted; PRs are excluded')
    require(run['head_sha'] == sha, 'Build SHA differs from immutable tag')
    require(run['status'] == 'completed' and run['conclusion'] == 'success', 'Build has not succeeded')


def expected(sha):
    return {f'KiCad-windows-x64-{sha}.exe',
            f'KiCad-linux-x64-{sha}.flatpak',
            *(f'macos-app-{arch}-{sha}.tar.gz' for arch in ('x86_64', 'arm64'))}



def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def require_new_release(tag):
    existing = json.loads(subprocess.check_output(
        ['gh', 'api', '--paginate', '--slurp', f'repos/{REPO}/releases?per_page=100'], text=True))
    require(not any(r['tag_name'] == tag for page in existing for r in page),
            'Release or draft exists; inspect it before retrying')


def prepare(tag, run_id, output):
    require(not output.exists() or not any(output.iterdir()), 'Use an empty promotion directory')
    match = TAG.fullmatch(tag)
    require(match, 'Invalid KiCad release tag')
    require_new_release(tag)
    sha = tag_commit(tag)
    base = tag_commit(match[1])
    require(api(f'compare/{base}...{sha}')['status'] in ('ahead', 'identical'),
            'Release does not descend from the upstream tag')
    require(run_id and run_id > 0, 'Select an explicit build run ID')
    run = api(f'actions/runs/{run_id}')
    validate_run(run, sha)
    jobs = pages(f'actions/runs/{run_id}/jobs?filter=latest', 'jobs')
    required = JOBS
    require(required <= {j['name'] for j in jobs if j['conclusion'] == 'success'},
            'Missing required platform checks; focused-only runs cannot be promoted alone')
    artifacts = pages(f'actions/runs/{run_id}/artifacts', 'artifacts')
    selected = [a for a in artifacts if a['name'] in expected(sha)]
    require(len(selected) == len(expected(sha)) and {a['name'] for a in selected} == expected(sha),
            'Missing or duplicate required packages')
    output.mkdir(parents=True, exist_ok=True)
    records = []
    for artifact in selected:
        require(not artifact['expired'] and artifact['workflow_run']['id'] == run_id and
                artifact['workflow_run']['head_sha'] == sha, 'Expired or mismatched artifact')
        require(re.fullmatch(r'sha256:[0-9a-f]{64}', artifact.get('digest') or ''), 'Artifact has no digest')
        path = output / artifact['name']
        with path.open('wb') as stream:
            subprocess.run(['gh', 'api', f'repos/{REPO}/actions/artifacts/{artifact["id"]}/zip'],
                           stdout=stream, check=True)
        require('sha256:' + digest(path) == artifact['digest'], 'Downloaded artifact digest mismatch')
        records.append(dict(id=artifact['id'], name=path.name, sha256=digest(path)))
    prerelease = '-beta.' in tag
    require(tag_commit(tag) == sha, 'Tag changed during verification')
    plan = dict(repository=REPO, tag=tag, commit=sha, run_id=run_id, run_attempt=run['run_attempt'],
                source_ref='refs/heads/' + run['head_branch'], prerelease=prerelease,
                artifacts=records)
    (output / 'promotion.json').write_text(json.dumps(plan, indent=2) + '\n')
    with open(os.environ.get('GITHUB_OUTPUT', os.devnull), 'a') as stream:
        stream.write('mac-artifacts=' + ','.join(str(r['id']) for r in records if r['name'].startswith('macos-app-')) + '\n')
    return plan


def draft(directory):
    plan = json.loads((directory / 'promotion.json').read_text())
    require(plan['repository'] == REPO and TAG.fullmatch(plan['tag']), 'Invalid release selection')
    require(tag_commit(plan['tag']) == plan['commit'], 'Tag moved before draft creation')
    require_new_release(plan['tag'])
    files = []
    for record in plan['artifacts']:
        if record['name'].startswith('macos-app-'):
            continue
        path = directory / record['name']
        require(digest(path) == record['sha256'], 'Staged package changed')
        files.append(path)
    signed = sorted((directory / 'signed').glob('*.tar.gz'))
    require({p.name for p in signed} == {
        f'kicad-macos-{arch}-{plan["commit"]}.tar.gz' for arch in ('arm64', 'x86_64')},
        'Expected both signed macOS packages at the selected commit')
    files += signed
    sums = directory / 'SHA256SUMS'
    sums.write_text(''.join(f'{digest(p)}  {p.name}\n' for p in files))
    command = ['gh', 'release', 'create', plan['tag'], *map(str, files), str(sums),
               '--repo', REPO, '--verify-tag', '--draft', '--title', 'KiCad ' + plan['tag'],
               '--notes', f'Unofficial KiCad build with OpenAxis navigation.\n\n'
               f'Source: {plan["commit"]}\nBuild: https://github.com/{REPO}/actions/runs/{plan["run_id"]}\n\n'
               'macOS packages are Developer ID signed and notarized. Windows is unsigned. '
               'Linux uses the upstream Flatpak application identity; install its required runtime. '
               'Final manual and physical-device acceptance is pending; this release remains a draft.']
    if plan['prerelease']:
        command.append('--prerelease')
    subprocess.run(command, check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=['prepare', 'draft'])
    parser.add_argument('--tag')
    parser.add_argument('--run-id', type=int)
    parser.add_argument('--directory', type=Path, default=Path('promotion'))
    args = parser.parse_args()
    if args.command == 'prepare':
        print(json.dumps(prepare(args.tag, args.run_id, args.directory), indent=2))
    else:
        draft(args.directory)
