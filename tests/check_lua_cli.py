#!/usr/bin/env python3
"""Offline Lua rules/next/deployment contract; run with an isolated HOME."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
if not Path(os.environ.get('HOME', '')).resolve().is_relative_to('/tmp'):
    raise SystemExit('HOME must be under /tmp')
exe = str(Path(sys.argv[1]).resolve())
def call(*args, code=0):
    result = subprocess.run([exe, '-j', *map(str, args)], capture_output=True, text=True)
    assert result.returncode == code, (args, result.returncode, result.stderr)
    return json.loads(result.stdout)['data']
with tempfile.TemporaryDirectory(prefix='mol-lua-cli-', dir='/tmp') as tmp:
    root = Path(tmp)
    inst = root / 'instance'
    game = root / 'game'
    game.mkdir()
    (game / 'SkyrimSE.exe').write_bytes(b'fixture')
    call('-i', inst, 'instance', 'init', '--game-dir', game, '--prefix', root / 'prefix')
    profile = inst / 'profiles/Default'
    mod = inst / 'mods/Get Lost for Anniversary Edition'
    (mod / 'SKSE/Plugins').mkdir(parents=True)
    (mod / 'SKSE/Plugins/GetLost.dll').write_bytes(b'fixture')
    (profile / 'modlist.txt').write_text('+Get Lost for Anniversary Edition\n')
    (profile / 'rules.ini').write_text('[Preferences]\nShowPlayerWorldmapPosition=true\nAllowDisableMod=true\n')
    (inst / 'rules').mkdir()
    (inst / 'rules/extra.lua').write_text("return {api_version=1,id='extra',check=function(c) return {{id='example',level='ok',message=c.game}} end}")
    data = call('-i', inst, 'doctor', code=3)
    row = next(c for c in data['checks'] if c['id'] == 'lua.skyrim.map.player_location')
    assert row['fix'] == ['mods', 'disable', mod.name]
    assert any(c['id'] == 'lua.extra.example' for c in data['checks'])
    steps = call('-i', inst, 'next')['steps']
    assert any(s['command'] == row['fix'] for s in steps)
    call('-i', inst, *row['fix'])
    call('-i', inst, 'apply')
    assert not (inst / 'farm/Data/SKSE/Plugins/GetLost.dll').exists()
    assert (mod / 'SKSE/Plugins/GetLost.dll').exists()
    (inst / 'rules/bad.lua').write_text('while true do end')
    data = call('-i', inst, 'doctor', code=3)
    assert any(c['id'] == 'lua.runtime' for c in data['checks'])
    assert any(c['id'] == 'lua.extra.example' for c in data['checks'])
    descriptor = root / 'game.lua'
    descriptor.write_text("return {api_version=1,id='generic',executable='Game.exe',data_directory='Mods',plugin_format='none'}")
    assert call('game', 'describe', descriptor)['id'] == 'generic'
print('Lua CLI integration: doctor → next → disable → apply, local-rule isolation and game describe passed')
