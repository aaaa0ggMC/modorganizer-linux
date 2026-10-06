#!/bin/bash
# 真实 Wine 下的 COW 检查：Windows 的 del / ren / move / copy / attrib / 追加写在注入 libmol-cow 后，
# 游戏目录与 mod 里的原文件必须一个字节都不变。没有 wine 时跳过。
# 用法：tests/cow_wine.sh [libmol-cow.so]   （WINE 可指定 wine 可执行文件，默认 wine64/wine）
set -u
LIB="$(realpath "${1:-$(dirname "$0")/../build/libmol-cow.so}")"
WINE="${WINE:-$(command -v wine64 || command -v wine || ls /usr/lib/wine/wine64 2>/dev/null)}"
[ -n "$WINE" ] && [ -x "$WINE" ] || { echo "cow_wine: no wine, skipped"; exit 0; }
SERVER="$(dirname "$(realpath "$WINE")")/wineserver"; [ -x "$SERVER" ] || SERVER=wineserver
WORK="$(mktemp -d /tmp/mol-cow-wine.XXXXXX)"; trap '"$SERVER" -k 2>/dev/null; rm -rf "$WORK"' EXIT
export WINEDEBUG=-all WINEPREFIX="$WORK/pfx"; R="$WORK/t"; FAIL=0
"$WINE" wineboot -i >/dev/null 2>&1
t() { "$SERVER" -k 2>/dev/null; sleep 0.5; rm -rf $R; mkdir -p $R/game $R/mods/M $R/farm/Data; for f in a b c d e g; do printf "$f\r\n" > $R/game/$f.txt; ln -s $R/game/$f.txt $R/farm/$f.txt; done; printf 'm\r\n' > $R/mods/M/m.txt; ln -s $R/mods/M/m.txt $R/farm/Data/m.txt; printf 'tmp\r\n' > $R/farm/new.txt; (cd $R/game; sha1sum *.txt; cd $R/mods/M; sha1sum m.txt) > $R/before; }
run() { cd $R/farm; env LD_PRELOAD="$LIB" MOL_COW_ROOT=$R/farm MOL_COW_LOG=$R/log MOL_COW_PROTECT=$R/game MOL_COW_MODS=$R/mods "$WINE" cmd /c "$1" >/dev/null 2>&1; cd /
  now=$( (cd $R/game; sha1sum *.txt 2>&1; cd $R/mods/M; sha1sum m.txt 2>&1) ); if [ "$now" == "$(cat $R/before)" ]; then r=OK; else r=CHANGED; FAIL=1; fi
  echo "[$r] $1"; echo "      farm: $(cd $R/farm && find . \( -type f -o -type l \) ! -name '.mol*' -printf '%y:%p ' | sort)"; }
t; run 'del a.txt'
t; run 'ren b.txt b2.txt'
t; run 'mkdir bak & move c.txt bak\c.txt & echo NEW>c.txt'
t; run 'move /y new.txt d.txt'
t; run 'echo x>>e.txt & attrib +r g.txt & echo y>Data\m.txt'
t; run 'del Data\m.txt & copy /y e.txt Data\m.txt'
[ $FAIL = 0 ] && echo "cow_wine: all originals intact" || echo "cow_wine: ORIGINAL FILES CHANGED"
exit $FAIL
