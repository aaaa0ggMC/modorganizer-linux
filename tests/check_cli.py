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
    assert data['ok'] == (code in (0, 3, 4))  # 3 = 体检有错、4 = 未完成需要输入：数据都有效
    if code not in (0, 3, 4):
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


# ---- schema：每个命令都有元数据、全局选项与错误码齐全 ----
data, _ = call('-j', 'schema')
cmds = data['data']['commands']
assert len(cmds) >= 40, len(cmds)
missing = [c['name'] for c in cmds if 'summary' not in c or 'effects' not in c]
assert not missing, missing
names = {c['name'] for c in cmds}
for must in ('run', 'next', 'schema', 'doctor', 'collection install', 'nexus install', 'overwrite promote'):
    assert must in names, must
by = {c['name']: c for c in cmds}
assert by['run']['confirm'] is True and 'launch' in by['run']['effects']
assert by['overwrite promote']['confirm'] is True
assert by['status']['effects'] == ['read']
assert set(data['data']['exit_codes']) == {'0', '1', '2', '3', '4'}
assert any(e['code'] == 'nexus_auth' and e['hint'] for e in data['data']['errors'])
assert any(o['long'] == '--json' for o in data['data']['global_options'])
checks += 1
# 错误自带 hint
data, _ = call('-j', '-i', str(root / 'nowhere'), 'mods', 'list', code=1)
assert data['errors'][0]['code'] == 'instance_not_found' and 'instance init' in data['errors'][0]['hint']
checks += 1
# next：没有实例 → 第一步是 instance init；有实例 → 有 steps 与 ready 字段
data, _ = call('-j', '-i', str(root / 'nowhere'), 'next')
assert data['data']['ready'] is False and data['data']['steps'][0]['id'] == 'instance.init'
data, _ = call('-j', '-i', str(instance), 'next')
assert 'ready' in data['data'] and isinstance(data['data']['steps'], list)
checks += 1
# collection resolve：位置参数之后的 --fomod-defaults 不能被 --fomod 吞掉；status 给出 decision/kind/url
with tempfile.TemporaryDirectory(prefix='mol-cli-res-', dir='/tmp') as tmp_r:
    rr = Path(tmp_r)
    (rr / 'game').mkdir()
    ri = rr / 'instance'
    call('-j', '-i', str(ri), 'instance', 'init', '--game-dir', str(rr / 'game'), '--prefix', str(rr / 'prefix'))
    (ri / 'collections/abc').mkdir(parents=True)
    (ri / 'collections/abc/state.json').write_text(json.dumps({'version': 1, 'slug': 'abc', 'name': 'ABC', 'revision': 1,
        'mods': {'T1': {'name': 'Patch', 'status': 'pending', 'archive': '', 'mod_dir': '', 'note': 'needs choices', 'kind': 'fomod_choices', 'url': 'https://example.invalid/m/1'}},
        'overrides': {}}))
    st, _ = call('-j', '-i', str(ri), 'collection', 'status', 'abc', code=4)
    p0 = st['data']['pending'][0]
    assert (p0['kind'], p0['url'], p0['decision'], p0['archive']) == ('fomod_choices', 'https://example.invalid/m/1', '', ''), p0
    # collection readme：没有 key（测试 HOME 下）时读实例里缓存的 readme
    (ri / 'collections/abc/readme-1.md').write_text('# ABC\n\nDowngrade to 1.6.1170 first.\n')
    rd, _ = call('-j', '-i', str(ri), 'collection', 'readme', 'https://next.nexusmods.com/skyrimspecialedition/collections/abc', env=dict(os.environ, NEXUS_API_KEY=''))
    assert rd['data']['cached'] is True and 'Downgrade' in rd['data']['markdown'], rd
    r = subprocess.run([exe, '-i', str(ri), 'collection', 'readme', 'abc'], text=True, capture_output=True, env=dict(os.environ, NEXUS_API_KEY=''))
    assert r.returncode == 0 and r.stdout.startswith('# ABC'), (r.returncode, r.stdout, r.stderr)
    data, _ = call('-j', '-i', str(ri), 'collection', 'resolve', 'abc', '--mod', 'T1', '--fomod-defaults')
    assert data['data']['recorded'] == 'fomod_defaults', data
    st, _ = call('-j', '-i', str(ri), 'collection', 'status', 'abc', code=4)
    assert st['data']['pending'][0]['decision'] == 'fomod_defaults'
    data, _ = call('-j', '-i', str(ri), 'collection', 'resolve', 'abc', '--mod', 'T1', '--skip')
    st, _ = call('-j', '-i', str(ri), 'collection', 'status', 'abc', code=4)
    assert st['data']['pending'][0]['decision'] == 'skip'
checks += 1
# docs：文档在构建时编进二进制，内容与源码树逐字节一致
repo = Path(__file__).resolve().parent.parent
dl, _ = call('-j', 'docs')
assert [t['name'] for t in dl['data']['topics']] == ['guide', 'agent', 'cli', 'handbook', 'readme'] and dl['data']['markdown'] == '', dl
for t in dl['data']['topics']:
    d1, _ = call('-j', 'docs', t['name'])
    assert d1['data']['topic'] == t['name'] and d1['data']['markdown'] == (repo / t['file']).read_text(), t['name']
r = subprocess.run([exe, 'docs', 'GUIDE.md'], text=True, capture_output=True)
assert r.returncode == 0 and r.stdout.startswith('# mo-linux 操作指南'), r.stdout[:80]
call('-j', 'docs', 'nope', code=2)
# collection inspect 不需要实例；total_size = 清单文件大小之和；install 开始前有预检（note 事件 + data.preflight）
with tempfile.TemporaryDirectory(prefix='mol-cli-col-', dir='/tmp') as tmp_c:
    tc = Path(tmp_c)
    man = tc / 'tiny.json'
    man.write_text(json.dumps({'info': {'name': 'Tiny', 'author': 'me', 'domainName': 'skyrimspecialedition', 'gameVersions': ['1.6.1170']},
        'mods': [{'name': 'A', 'version': '1', 'optional': False, 'source': {'type': 'nexus', 'modId': 1, 'fileId': 2, 'fileSize': 1000, 'tag': 'a'}},
                 {'name': 'B', 'version': '1', 'optional': True, 'source': {'type': 'browse', 'url': 'http://x.invalid', 'fileSize': 500, 'tag': 'b'}}], 'modRules': []}))
    env_c = dict(os.environ, HOME=str(tc / 'home'), XDG_CONFIG_HOME=str(tc / 'cfg'), NEXUS_API_KEY='')
    env_c.pop('MOL_INSTANCE', None)
    r = subprocess.run([exe, '-j', 'collection', 'inspect', str(man)], text=True, capture_output=True, env=env_c, cwd=str(tc))
    assert r.returncode == 0, r.stderr
    d = json.loads(r.stdout)['data']
    assert (d['total_size'], d['optional_size'], d['has_instance'], d['mod_count']) == (1500, 500, False, 2), d
    assert {x['type']: (x['count'], x['size']) for x in d['sources']} == {'nexus': (1, 1000), 'browse': (1, 500)}, d['sources']
    assert [m['status'] for m in d['mods']] == ['new', 'new'] and [m['file_size'] for m in d['mods']] == [1000, 500]
    checks += 1
    ci = tc / 'inst'
    call('-j', '-i', str(ci), 'instance', 'init', '--game-dir', str(tc), '--prefix', str(tc / 'pfx'))
    ev = tc / 'ev.jsonl'
    with ev.open('w') as stream:
        r = subprocess.run([exe, '-j', '-i', str(ci), '--events', f'fd:{stream.fileno()}', 'collection', 'install', str(man)],
                           text=True, capture_output=True, pass_fds=(stream.fileno(),), env=env_c)
    assert r.returncode == 4, (r.returncode, r.stderr)
    assert 'collection install: 2 mods' in r.stderr, r.stderr
    rows = [json.loads(line) for line in ev.read_text().splitlines()]
    assert rows[0] == {'event': 'start', 'op': 'collection install'} and rows[1]['event'] == 'note' and rows[1]['code'] == 'preflight', rows[:2]
    pf = json.loads(r.stdout)['data']['preflight']
    assert (pf['mods'], pf['download_size'], pf['remaining_size']) == (2, 1500, 1500) and pf['free_space'] > 0, pf
    r = subprocess.run([exe, '-i', str(ci), 'collection', 'status', 'tiny'], text=True, capture_output=True, env=env_c)
    assert r.returncode == 4 and 'pending [' in r.stdout, r.stdout
    shared = tc / 'shared-dl'
    data, _ = call('-j', '-q', '-i', str(ci), 'collection', 'install', str(man), '--downloads', str(shared), code=4, env=env_c)
    assert shared.is_dir() and data['data']['preflight']['download_size'] == 1500
    checks += 1
# overview：一次取齐；与单独的 next/doctor 一致（上面的 with 块结束时临时目录已删除，这里自建实例）
with tempfile.TemporaryDirectory(prefix='mol-cli-ov-', dir='/tmp') as tmp_ov:
    ovr = Path(tmp_ov)
    (ovr / 'game').mkdir()
    ovi = ovr / 'instance'
    call('-j', '-i', str(ovi), 'instance', 'init', '--game-dir', str(ovr / 'game'), '--prefix', str(ovr / 'prefix'))
    (ovi / 'mods/A').mkdir(parents=True)
    (ovi / 'profiles/Default/modlist.txt').write_text('+A\n-B\n')
    data, _ = call('-j', '-i', str(ovr / 'nowhere'), 'overview')
    assert data['data']['has_instance'] is False and data['data']['next']['steps'][0]['id'] == 'instance.init'
    data, _ = call('-j', '-i', str(ovi), 'overview')
    ov = data['data']
    nx, _ = call('-j', '-i', str(ovi), 'next')
    assert ov['has_instance'] is True and ov['instance'] == str(ovi)
    assert [s['id'] for s in ov['next']['steps']] == [s['id'] for s in nx['data']['steps']]
    dr, _ = call('-j', '-i', str(ovi), 'doctor', code=3)  # 假游戏目录里没有 exe → 有 error
    assert ov['doctor']['errors'] == dr['data']['errors'] and len(ov['doctor']['checks']) == len(dr['data']['checks'])
    assert (ov['mods_total'], ov['mods_enabled'], ov['mods_missing']) == (2, 1, 0), ov
    assert ov['collections'] == [] and ov['version']
checks += 1

# ---- 默认实例 / nxm 处理器 ----
import threading, http.server, socketserver
env_home = dict(os.environ)
env_home['XDG_CONFIG_HOME'] = str(root / 'xdg')
instance = root / 'instance-nxm'
call('-j', '-i', str(instance), 'instance', 'init', '--game-dir', str(game), '--prefix', str(prefix))
data, _ = call('-j', '-i', str(instance), 'instance', 'default', env=env_home)
assert data['data']['path'] == '' and data['data']['changed'] is False
data, _ = call('-j', '-i', str(instance), 'instance', 'default', '--set', env=env_home)
assert data['data']['path'] == str(instance) and data['data']['changed'] is True
# 在一个不是实例的目录里运行也能找到默认实例
r = subprocess.run([exe, '-j', 'instance', 'show'], text=True, capture_output=True, env=env_home, cwd=str(root))
assert r.returncode == 0 and json.loads(r.stdout)['data']['root'] == str(instance), r.stdout
checks += 1
# 不是实例的目录不能设为默认
data, _ = call('-j', '-i', str(root / 'nowhere'), 'instance', 'default', '--set', env=env_home, code=1)
assert data['errors'][0]['code'] == 'instance_not_found'
checks += 1

# nxm handle：本地假 Nexus（只实现 files.json / download_link / 文件本身）
payload = b'ARCHIVE-BYTES' * 10
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, code, body, ct='application/json'):
        self.send_response(code); self.send_header('Content-Length', str(len(body))); self.send_header('Content-Type', ct); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        if self.path.startswith('/dl/'): return self._send(200, payload, 'application/octet-stream')
        if self.path.endswith('/files.json'):
            return self._send(200, json.dumps({'files': [{'file_id': 6, 'name': 'M', 'file_name': 'm-5-6.zip', 'version': '1', 'category_name': 'main', 'size_kb': 1, 'is_primary': True}]}).encode())
        if 'download_link.json' in self.path:
            assert 'key=K' in self.path and 'expires=1' in self.path, self.path
            return self._send(200, json.dumps([{'name': 'c', 'short_name': 'c', 'URI': f'http://127.0.0.1:{srv.server_address[1]}/dl/m-5-6.zip'}]).encode())
        self._send(404, b'{}')
srv = socketserver.TCPServer(('127.0.0.1', 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
env_nx = dict(env_home, NEXUS_API_KEY='x', MOL_NEXUS_API=f'http://127.0.0.1:{srv.server_address[1]}')
# 先造一个在等这个文件的集合状态
cdir = instance / 'collections' / 'demo'
(cdir / 'archive-1').mkdir(parents=True)
(cdir / 'archive-1' / 'collection.json').write_text(json.dumps({'info': {'name': 'Demo', 'domainName': 'skyrimspecialedition'},
    'mods': [{'name': 'Waiting', 'version': '1', 'optional': False, 'source': {'type': 'nexus', 'modId': 5, 'fileId': 6, 'tag': 'tg'}}], 'modRules': []}))
(cdir / 'state.json').write_text(json.dumps({'version': 1, 'slug': 'demo', 'name': 'Demo', 'revision': 1, 'mods': {'tg': {'name': 'Waiting', 'status': 'pending', 'archive': '', 'mod_dir': '', 'note': ''}}, 'overrides': {}}))
data, _ = call('-j', 'nxm', 'handle', 'nxm://skyrimspecialedition/mods/5/files/6?key=K&expires=1&user_id=9', env=env_nx)
assert Path(data['data']['path']).read_bytes() == payload
assert [m['key'] for m in data['data']['matches']] == ['tg'], data['data']
st = json.loads((cdir / 'state.json').read_text())
assert st['mods']['tg']['archive'] == data['data']['path']
checks += 1
srv.shutdown()

# nxm register：写 .desktop（HOME 在 /tmp 下，不会碰真实配置）
data, _ = call('-j', 'nxm', 'register', env=env_home)
desk = Path(data['data']['desktop_file'])
assert desk.is_file() and 'x-scheme-handler/nxm' in desk.read_text() and 'nxm handle %u' in desk.read_text()
checks += 1

print(f'CLI integration: {checks} checks passed')
