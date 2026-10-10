#!/usr/bin/env python3
"""Offline Lua install-script contract; run with an isolated HOME and XDG_RUNTIME_DIR.

Covers: `script run` (local + dry-run), the sandbox refusing escapes, `serve` as a
background service (one port, per-run namespaces, `_mol` introspection, state over HTTP).
No network, no Wine: executables and downloads are only validated, never executed.
"""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

if not Path(os.environ.get('HOME', '')).resolve().is_relative_to('/tmp'):
    raise SystemExit('HOME must be under /tmp')
if not os.environ.get('XDG_RUNTIME_DIR', '').startswith('/tmp'):
    raise SystemExit('XDG_RUNTIME_DIR must be under /tmp')
exe = str(Path(sys.argv[1]).resolve())
repo = Path(__file__).resolve().parent.parent


def call(*args, code=0, timeout=180):
    result = subprocess.run([exe, '-j', *map(str, args)], capture_output=True, text=True, timeout=timeout)
    assert result.returncode == code, (args, result.returncode, result.stderr[-2000:])
    return json.loads(result.stdout)['data']


def http(port, path, token, method='GET', body=''):
    s = socket.create_connection(('127.0.0.1', port), 5)
    head = (f'{method} {path}{"?" if "?" not in path else "&"}token={token} HTTP/1.1\r\n'
            f'Host: 127.0.0.1\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n')
    s.sendall(head.encode() + body.encode())
    chunks = []
    while True:
        b = s.recv(65536)
        if not b:
            break
        chunks.append(b)
    s.close()
    raw = b''.join(chunks).decode('utf-8', 'replace')
    head, _, payload = raw.partition('\r\n\r\n')
    status = int(head.split(' ', 2)[1]) if head.startswith('HTTP/') else 0
    return status, payload


with tempfile.TemporaryDirectory(prefix='mol-script-cli-', dir='/tmp') as tmp:
    root = Path(tmp)
    inst = root / 'instance'
    game = root / 'game'
    game.mkdir()
    (game / 'SkyrimSE.exe').write_bytes(b'fixture')
    call('-i', inst, 'instance', 'init', '--game-dir', game, '--prefix', root / 'prefix')

    # 1) 本地 dry-run：示例脚本在临时虚拟根里跑通，结果字段齐全
    data = call('-i', inst, 'script', 'run', '--local', '--dry-run', repo / 'scripts/example-stage.lua')
    assert data['ok'] is True, data
    assert data['mode'] == 'local'
    assert data['ns'] == 'script_0'
    assert 'example-stage.lua' in data['script']
    assert Path(data['root']).is_dir()
    assert any('finished' in line for line in data['log']), data['log']
    states = {row['key']: row['value'] for row in data['state']}
    assert states == {'phase': 'done', 'version': '1', 'files/staged': '2'}, states
    # dry-run 不碰实例：虚拟根是临时目录
    assert not (inst / 'scripts').exists()

    # 2) 逃逸被拒：脚本一第一次坏路径就失败，退出码 1
    bad = root / 'bad.lua'
    bad.write_text("local x = fs.read('../../etc/hostname')\n")
    out = call('-i', inst, 'script', 'run', '--local', '--dry-run', bad, code=1)
    assert out['ok'] is False and 'virtual path' in out['error'], out

    # 3) 后台 service：一个端口 + namespace + _mol 自省 + HTTP state（run 进行中查询）
    call('serve', '--detach', '--idle-timeout', '30')
    try:
        spin = root / 'spin.lua'
        spin.write_text("state.set('phase', 'running')\nlocal t = os.time()\n"
                        "while os.time() - t < 20 do end\n")
        proc = subprocess.Popen(
            [exe, '-j', '-i', str(inst), 'script', 'run', '--ns', 'skyrim-main', '--timeout', '1', str(spin)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        # CLI 会把 namespace/URL/token 打到 stderr；等它出现
        token = port = None
        for _ in range(100):
            line = proc.stderr.readline()
            if not line:
                break
            if 'state at http://' in line:
                url = line.split('state at ', 1)[1].strip()
                token = url.split('token=', 1)[1]
                port = int(url.split(':', 2)[2].split('/')[0])
                break
        assert token and port, 'no service URL on stderr'
        try:
            status, body = http(port, '/_mol/scripts', token)
            assert status == 200
            runs = json.loads(body)
            assert [r['ns'] for r in runs] == ['skyrim-main'], runs
            one = next(r for r in runs if r['ns'] == 'skyrim-main')
            assert one['run_state'] == 'running', one
            assert one['instance'] == str(inst), one
            assert one['lua_line'] > 0, one  # 卡在循环里：行号在报

            status, body = http(port, '/_mol/scripts/skyrim-main', token)
            assert status == 200 and json.loads(body)['lua_line'] > 0, body

            # 用户可以通过 HTTP 看/改脚本的 state
            status, body = http(port, '/skyrim-main/state', token)
            assert status == 200, body
            assert json.loads(body) == {'ns': 'skyrim-main', 'state': {'phase': 'running'}}, body
            status, body = http(port, '/skyrim-main/state/note', token, 'POST', 'hello from the browser')
            assert status == 200, body
            status, body = http(port, '/skyrim-main/state/note', token)
            assert status == 200 and body == '"hello from the browser"', body

            # 没有 token / 乱 namespace 都进不去
            status, _ = http(port, '/_mol/ping', 'wrong')
            assert status == 401
            status, _ = http(port, '/nope/state', token)
            assert status == 404
            status, _ = http(port, '/_mol/state', token)
            assert status == 404
        finally:
            proc_stdout = proc.stdout.read()
            proc.stdout.close()
            proc.stderr.close()
            proc.wait(timeout=120)
        final = json.loads(proc_stdout)['data']
        assert final['ok'] is True and final['mode'] == 'service' and final['ns'] == 'skyrim-main', final
    finally:
        call('serve', '--stop')

print('Lua script CLI integration: local dry-run, escape refusal, service namespace/'
      'introspection/state all passed')
