#!/bin/bash
# 把上游 uibase 复制到 <dest>，再应用 patches/uibase/*.patch（上游目录保持只读/干净）。
# 用法: prepare_uibase.sh <dest>
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
dest=$1
rm -rf "$dest"; mkdir -p "$dest"
cp -a "$R/third_party/uibase/." "$dest/"
rm -rf "$dest/.git"
for p in "$R"/patches/uibase/*.patch; do
  [ -e "$p" ] || continue
  patch -s -p1 -d "$dest" < "$p"
done
