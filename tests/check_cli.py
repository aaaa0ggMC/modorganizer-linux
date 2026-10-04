#!/usr/bin/env python3
"""CLI integration checks using isolated fixtures; pass executable and optional host .so."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

if not Path(os.environ.get('HOME', '')).resolve().is_relative_to('/tmp'):
    raise SystemExit('HOME must be under /tmp')

exe = str(Path(sys.argv[1]).resolve())
host = str(Path(sys.argv[2]).resolve()) if len(sys.argv) > 2 else None
checks = 0

def call(*args, code=0, env=None):
    global checks
    result = subprocess.run([exe, *args], text=True, capture_output=True, env=env)
    assert result.returncode == code, (args, result.returncode, result.stderr)
    data = json.loads(result.stdout)
    assert set(data) == {'schema_version', 'ok', 'command', 'data', 'warnings', 'errors'}
    assert data['schema_version'] == 1
    assert data['ok'] == (code in (0, 3))
    if code not in (0, 3):
        assert data['data'] is None and data['errors']
    checks += 1
    return data, result

with tempfile.TemporaryDirectory(prefix='mol-cli-check-', dir='/tmp') as tmp:
    root = Path(tmp)
    game = root / 'game'
    game.mkdir()
    instance = root / 'instance'
    prefix = root / 'prefix'
    call('-j', '-i', str(instance), 'instance', 'init', '--game-dir', str(game), '--prefix', str(prefix))
    # Parser errors must be reported before reading/mutating an instance.
    for args in [('-j', 'version', '--bogus'), ('-j', 'version', '--to', '4'),
                 ('-j', 'mods', 'enable', 'One', 'Two'), ('-j', 'mods'),
                 ('-j', '--instance'), ('-j', 'version', '--instance')]:
        data, _ = call(*args, code=2)
        assert data['errors'][0]['code'] == 'invalid_argument'
    data, result = call('-j', '-q', 'version', '--instance', code=2)
    assert result.stderr == ''
    help_result = subprocess.run([exe, '--help'], capture_output=True, text=True)
    assert help_result.returncode == 0 and 'game' in help_result.stdout
    assert not help_result.stderr
    checks += 1
    # A leading-dash mod name remains literal after --, including registered toggles.
    mod = instance / 'mods/--json'
    mod.mkdir()
    (instance / 'profiles/Default/modlist.txt').write_text('---json\n')
    data, _ = call('-j', '-i', str(instance), 'mods', 'enable', '--', '--json')
    assert data['data']['name'] == '--json' and data['data']['enabled']
    # A later -- must not retroactively accept an unknown option before it.
    (instance / 'mods/--bogus').mkdir()
    modlist = instance / 'profiles/Default/modlist.txt'
    modlist.write_text(modlist.read_text() + '---bogus\n')
    call('-j', '-i', str(instance), 'mods', 'enable', '--bogus', '--', code=2)
    assert '---bogus' in modlist.read_text(), 'invalid argv must not enable the mod'
    data, _ = call('-j', '-i', str(instance), 'apply')
    assert data['data']['changed'] and data['data']['applied'] == 0
    assert (instance / 'farm/.mol-farm.json').is_file()
    data, _ = call('-j', '-i', str(instance), 'apply')
    assert not data['data']['changed']
    data, _ = call('-j', '-i', str(instance), 'unlink')
    assert data['data']['removed']
    # More than one apply batch: marker must retain entries from every batch.
    resources = game / 'Data'
    resources.mkdir()
    for index in range(300):
        (resources / f'file-{index}.txt').write_text(str(index))
    (resources / 'quote"\n资源.txt').write_text('JSON escaping')
    first, _ = call('-j', '-i', str(instance), 'plan')
    second, _ = call('-j', '-i', str(instance), 'plan')
    assert first == second
    assert first['data']['count'] == 302
    assert first['data']['counts']['link'] == 301
    assert first['data']['counts']['mkdir'] == 1
    data, _ = call('-j', '-i', str(instance), 'apply')
    assert data['data']['applied'] == 302
    marker = json.loads((instance / 'farm/.mol-farm.json').read_text())
    assert len(marker['created']) == 302
    data, _ = call('-j', '-i', str(instance), 'status')
    assert data['data']['in_sync'] and data['data']['pending'] == 0
    data, _ = call('-j', '-i', str(instance), 'unlink')
    assert data['data']['removed'] and not (instance / 'farm').exists()
    assert len(list(resources.iterdir())) == 301, 'unlink must preserve game sources'
    events = root / 'events.jsonl'
    with events.open('w') as stream:
        result = subprocess.run([exe, '-j', '-i', str(root / 'missing'), '--events', f'fd:{stream.fileno()}', 'apply'],
                                pass_fds=(stream.fileno(),), capture_output=True, text=True)
    assert result.returncode == 1
    rows = [json.loads(line) for line in events.read_text().splitlines()]
    assert rows == [{'event': 'start', 'op': 'apply'}, {'event': 'done', 'op': 'apply', 'ok': False}]
    checks += 1
    # Real host and upstream game plugin, entirely fake game and prefix.
    if host:
        env = dict(os.environ, MOL_GAME_LIB=host)
        data, _ = call('-j', '-i', str(instance), 'game', 'info', env=env)
        info = data['data']
        assert info['shortName'] == 'SkyrimSE'
        assert str(info['steamAppId']) == '489830'
        assert info['gameDirectory'] == str(game)
        assert Path(info['dataDirectory']).parent == game
        assert Path(info['dataDirectory']).name.lower() == 'data'
        assert info['binaryName'] == 'SkyrimSE.exe'
        assert {'skyrim.esm', 'update.esm'} <= set(info['primaryPlugins'])
        assert 'executables' in info and 'scriptExtender' in info
        assert not prefix.exists(), 'read-only game info must not create prefix'
    # Failing host loading still respects quiet and the JSON envelope.
    invalid = root / 'invalid.so'
    invalid.write_text('not an ELF shared library')
    env = dict(os.environ, MOL_GAME_LIB=str(invalid))
    data, result = call('-j', '-q', '-i', str(instance), 'game', 'info', env=env, code=1)
    assert data['errors'][0]['code'] == 'game_unavailable'
    assert result.stderr == ''

print(f'CLI integration: {checks} checks passed')
