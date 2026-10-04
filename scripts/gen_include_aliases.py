#!/usr/bin/env python3
"""为大小写不一致的 #include 生成别名符号链接目录（不修改上游源码）。

用法: gen_include_aliases.py <alias_dir> <src_dir>[:<src_dir>...] <inc_dir>[:<inc_dir>...]
 - src_dirs: 扫描其中的 .cpp/.h/.hpp 的 #include
 - inc_dirs: 头文件搜索根（含 shim、uibase include 等）
对每个 include 名，若在 inc_dirs 与源文件所在目录都无法精确解析，但按小写能解析，
就在 alias_dir 下创建同名（含子路径）符号链接指向真实文件。
"""
import os, re, sys

alias_dir, src_dirs, inc_dirs = sys.argv[1], sys.argv[2].split(':'), sys.argv[3].split(':')
INC = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)

lower_index = {}  # 小写相对路径 -> 绝对路径
all_dirs = set()
for root in inc_dirs + src_dirs:
    for dp, _, fns in os.walk(root):
        all_dirs.add(dp)
        for fn in fns:
            ab = os.path.join(dp, fn)
            for base in (root, dp):
                rel = os.path.relpath(ab, base).replace(os.sep, '/').lower()
                lower_index.setdefault(rel, ab)

def exact(name, from_dir):
    for d in inc_dirs + [from_dir]:
        if os.path.exists(os.path.join(d, name)):
            return True
    return False

wanted = {}
for root in src_dirs:
    for dp, _, fns in os.walk(root):
        for fn in fns:
            if not fn.endswith(('.cpp', '.h', '.hpp', '.cc')):
                continue
            try:
                text = open(os.path.join(dp, fn), encoding='utf-8', errors='replace').read()
            except OSError:
                continue
            for name in INC.findall(text):
                if exact(name, dp):
                    continue
                real = lower_index.get(name.lower())
                if real:
                    wanted[name] = real

os.makedirs(alias_dir, exist_ok=True)
for name, real in sorted(wanted.items()):
    link = os.path.join(alias_dir, name)
    os.makedirs(os.path.dirname(link), exist_ok=True)
    if os.path.islink(link):
        if os.readlink(link) == real:
            continue
        os.unlink(link)
    elif os.path.exists(link):
        continue
    os.symlink(real, link)
print(f"aliases: {len(wanted)}")
