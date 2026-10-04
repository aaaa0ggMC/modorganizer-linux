#!/usr/bin/env python3
"""重新生成 patches/uibase/0001-linux-utility.patch（保留上游 CRLF 换行）。
用法: scripts/make_uibase_patch.py   —— 从干净 uibase 复制出副本、套用下面的编辑、输出 diff。"""
import os, shutil, subprocess, sys, tempfile
R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
work = tempfile.mkdtemp(prefix='mol-uibase-')
a, b = os.path.join(work, 'a'), os.path.join(work, 'b')
for d in (a, b):
    shutil.copytree(os.path.join(R, 'third_party/uibase'), d, ignore=shutil.ignore_patterns('.git'))

def edit(rel, fn):
    p = os.path.join(b, rel)
    s = open(p, newline='').read()
    nl = '\r\n' if '\r\n' in s else '\n'
    open(p, 'w', newline='').write(fn(s, nl))

def utility(s, nl):
    def rep(s, x, y):
        x = x.replace('\n', nl); y = y.replace('\n', nl)
        assert x in s, x[:50]
        return s.replace(x, y)
    # Windows 专属：QUuid(GUID) 与 PE 图标提取
    s = rep(s, 'what.isEmpty() ? QUuid(id).toString() : what,', 'what,')
    i = s.index('QIcon iconForExecutable(const QString& filePath)')
    j = s.index('QString getFileVersion')
    s = s[:i] + ('QIcon iconForExecutable(const QString&)\n{\n'
                 '  // Linux: PE 图标提取不可用，返回通用图标\n'
                 '  return QIcon(":/MO/gui/executable");\n}\n\n').replace('\n', nl) + s[j:]
    return s
edit('src/utility.cpp', utility)

raw = subprocess.run(['diff', '-ruN', 'a', 'b'], cwd=work, capture_output=True).stdout
lines = raw.split(b'\n')  # 只按 \n 切分，保留行尾的 \r
lines = [l.split(b'\t')[0] if l.startswith((b'---', b'+++')) else l for l in lines]
dest = os.path.join(R, 'patches/uibase/0001-linux-utility.patch')
open(dest, 'wb').write(b'\n'.join(lines))
shutil.rmtree(work)
print('wrote', dest, len(lines), 'lines')
