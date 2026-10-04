#!/bin/bash
# 开发辅助：对上游源码做 -fsyntax-only，快速迭代 shim。用法: syntax_check.sh <dir> [glob...]
set -u
R=$(cd "$(dirname "$0")/.." && pwd)
B=${MOL_BUILD:-/tmp/mol-syntax}
QT=$(pkg-config --cflags Qt6Widgets Qt6Network Qt6Qml Qt6Quick Qt6QuickWidgets)
UI=$B/uibase-src; [ -d $UI ] || $R/scripts/prepare_uibase.sh $UI
GB=$R/third_party/game_bethesda/src
INC="-I$B/ui -I$R/shim/include -I$B/alias -I$UI/include -I$UI/include/uibase -I$UI/include/uibase/game_features -I$UI/src \
 -I$GB/gamebryo -I$GB/creation -I$GB/games/skyrimse"
python3 $R/scripts/gen_include_aliases.py $B/alias "$UI/src:$UI/include:$GB" "$R/shim/include:$UI/include:$UI/include/uibase:$UI/include/uibase/game_features:$GB/gamebryo:$GB/creation:$GB/games/skyrimse" >/dev/null
for f in "$@"; do
  ${CXX:-clang++} -std=c++2c -fms-extensions -include pch.h -fsyntax-only -fPIC -include winshim_prelude.h -DSPDLOG_USE_STD_FORMAT -DUIBASE_EXPORT -Wno-unknown-pragmas -w $QT $INC "$f" 2>&1 | grep -E "error|Error" | head -${MOL_N:-8}
done
