#!/usr/bin/env bash
# =============================================================================
# 编译 mo-linux（GCC 主工程 + clang 子工程 libmo-game.so）并安装到 ~/Apps。
#
# 安装结果（默认）：
#   ~/Apps/mo-linux           CLI 可执行文件
#   ~/Apps/libmo-game.so      游戏宿主库
#   ~/Apps/libmol-cow.so      写时复制预载库（run 时 LD_PRELOAD 进 Wine 进程）
# CLI 会从自身所在目录旁边自动加载 libmo-game.so（见 core/src/game_host.cpp）与
# libmol-cow.so（见 core/src/runner.cpp），所以三者放在同一目录即可，无需设置
# MOL_GAME_LIB / MOL_COW_LIB。
#
# 用法：
#   scripts/install_to_apps.sh                  # 全量构建并安装
#   JOBS=4 scripts/install_to_apps.sh           # 限制并行度
#   SKIP_BUILD=1 scripts/install_to_apps.sh     # 跳过编译，只安装已有产物
#   BUILD_DIR=... APPS_DIR=... scripts/install_to_apps.sh
#
# 可用环境变量：
#   BUILD_DIR        构建目录        (默认 $HOME/.cache/mol/apps-build，勿用 /tmp：tmpfs 会吃内存)
#   APPS_DIR         安装目录        (默认 $HOME/Apps)
#   JOBS             并行任务数      (默认 nproc)
#   BUILD_TYPE       CMake 构建类型  (默认 Release)
#   MOL_BUILD_HOST   是否构建宿主库  (默认 ON)
#   MOL_ALIB6_DIR    aaaa0ggmcLib 目录 (默认 $HOME/Projs/aaaa0ggmcLib)
#   SKIP_BUILD       1 = 只安装不编译 (默认 0)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$HOME/.cache/mol/apps-build}"
APPS_DIR="${APPS_DIR:-$HOME/Apps}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
MOL_BUILD_HOST="${MOL_BUILD_HOST:-ON}"
MOL_ALIB6_DIR="${MOL_ALIB6_DIR:-$HOME/Projs/aaaa0ggmcLib}"
SKIP_BUILD="${SKIP_BUILD:-0}"

log()  { printf '\033[1;34m[mo-linux]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[mo-linux]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[mo-linux]\033[0m %s\n' "$*" >&2; exit 1; }

[[ -f "$SRC_DIR/CMakeLists.txt" ]] || die "找不到源码：$SRC_DIR/CMakeLists.txt"

if [[ "$SKIP_BUILD" != 1 ]]; then
  command -v cmake >/dev/null || die "缺少 cmake"
  command -v ninja >/dev/null || die "缺少 ninja"
  [[ -f "$MOL_ALIB6_DIR/include/alib6/main.cppm" ]] \
    || die "找不到 alib6 源码：$MOL_ALIB6_DIR（用 MOL_ALIB6_DIR=... 指定）"

  log "配置：$SRC_DIR -> $BUILD_DIR（Ninja，$BUILD_TYPE，MOL_BUILD_HOST=$MOL_BUILD_HOST）"
  cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DMOL_BUILD_HOST="$MOL_BUILD_HOST" \
    -DMOL_ALIB6_DIR="$MOL_ALIB6_DIR"

  log "编译中（-j $JOBS）…"
  cmake --build "$BUILD_DIR" -j "$JOBS"
fi

# ---- 定位产物 ---------------------------------------------------------------
CLI_BIN="$BUILD_DIR/mo-linux"
[[ -x "$CLI_BIN" ]] || die "未找到 CLI 产物：$CLI_BIN（先跑一次不带 SKIP_BUILD=1 的构建）"

# 在构建目录里定位共享库：先看常见位置，再兜底 find。
# 用法：lib="$(find_lib libmol-cow.so)"
find_lib() {
  local name="$1" cand
  for cand in "$BUILD_DIR/$name" "$BUILD_DIR/host/$name"; do
    if [[ -f "$cand" ]]; then printf '%s' "$cand"; return 0; fi
  done
  find "$BUILD_DIR" -maxdepth 3 -type f -name "$name" 2>/dev/null | head -n1
}

HOST_LIB="$(find_lib libmo-game.so)"
if [[ -z "$HOST_LIB" ]]; then
  if [[ "$MOL_BUILD_HOST" == "OFF" ]]; then
    warn "MOL_BUILD_HOST=OFF，未生成 libmo-game.so；仅安装 CLI（game info/run/doctor 的游戏层不可用）"
  else
    die "未找到 libmo-game.so（host 子工程未构建成功？）"
  fi
fi

COW_LIB="$(find_lib libmol-cow.so)"
[[ -n "$COW_LIB" ]] || die "未找到 libmol-cow.so（run 的写时复制会降级为警告）"

# ---- 安装 -------------------------------------------------------------------
mkdir -p "$APPS_DIR"
log "安装到 $APPS_DIR"
install -m 0755 "$CLI_BIN" "$APPS_DIR/mo-linux"
[[ -n "$HOST_LIB" ]] && install -m 0644 "$HOST_LIB" "$APPS_DIR/libmo-game.so"
install -m 0644 "$COW_LIB" "$APPS_DIR/libmol-cow.so"

# ---- 自检 -------------------------------------------------------------------
log "自检：$APPS_DIR/mo-linux version"
"$APPS_DIR/mo-linux" version >/dev/null || die "安装后的二进制无法运行"

log "完成。"
echo "  CLI      : $APPS_DIR/mo-linux"
[[ -n "$HOST_LIB" ]] && echo "  游戏宿主 : $APPS_DIR/libmo-game.so"
echo "  写时复制 : $APPS_DIR/libmol-cow.so"
echo "  提示     : 在实例目录里直接运行，或用 -i <dir> / MOL_INSTANCE 指定实例。"
