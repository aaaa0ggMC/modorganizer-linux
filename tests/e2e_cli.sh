#!/usr/bin/env bash
# mo-linux CLI 端到端测试（WP8 联调）。
#
# 安全约束（自检脚本）：
#   * 开头断言 HOME 位于 /tmp/ 内，否则拒绝运行（避免碰真实用户配置）；
#   * 全部中间产物放在 mktemp -d 的临时目录里，退出时 trap 清理；
#   * 不 rm -rf 任何固定路径、不读写真实用户目录。
#
# 生命周期：instance init →(幂等)→ show → mods list → plan → status(3) → apply(changed)
#   → apply(unchanged) → status(0) → conflicts → enable/disable/move → 禁 ModB 后 plan+apply
#   → unlink → unlink(idempotent) → 错误路径 → --events fd:N → fifo 无读端 →
#   全局选项摆放/--/未知选项/-q 日志。
# 用法：tests/e2e_cli.sh [mo-linux 可执行路径]（默认 <repo>/build/mo-linux）
set -u

# ---- 0. 自检：HOME 必须在 /tmp/ 内 ----------------------------------------
case "$(realpath -m -- "${HOME:-/}")" in
  /tmp | /tmp/*) ;;
  *)
    printf 'e2e: refusing to run: HOME must be under /tmp/ (got: %s)\n' "${HOME:-<unset>}" >&2
    exit 2
    ;;
esac

MO="${1:-$(cd "$(dirname "$0")/.." && pwd)/build/mo-linux}"
WORK="$(mktemp -d /tmp/mol-e2e.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
GAME="$WORK/game"
INST="$WORK/instance"
LOG_ERR="$WORK/stderr.log"

PASS=0
FAIL=0

ok()  { PASS=$((PASS+1)); printf '  [ PASS ] %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf '  [ FAIL ] %s\n' "$1"; }

# check_rc <期望 rc> <实际 rc> <描述>
check_rc() {
  if [ "$1" = "$2" ]; then ok "$3 (exit $2)"; else bad "$3: 期望 exit $1，实际 $2"; fi
}
# has <JSON> <子串> <描述>
has() {
  if printf '%s' "$1" | grep -qF -- "$2"; then ok "$3"; else bad "$3: 未找到 '$2' in: $1"; fi
}
hasnt() {
  if printf '%s' "$1" | grep -qF -- "$2"; then bad "$3: 不应出现 '$2'"; else ok "$3"; fi
}

echo "== 准备环境（$WORK）=="
mkdir -p "$GAME/Data" "$INST"
printf 'PROTON=1\n' > "$GAME/Data/Skyrim.esm"      # 游戏本体层
# mod 内容直接放在 mod 根下（build_farm_model 会给 mod 层加 "Data" 前缀）
mkdir -p "$INST/mods/ModA/Textures" "$INST/mods/ModB/Textures" "$INST/mods/ModC/Scripts"
printf 'MOD-A-v0\n'     > "$INST/mods/ModA/Textures/wall.dds"
printf 'MOD-B-v0\n'     > "$INST/mods/ModB/Textures/Wall.dds"   # 与 ModA 大小写冲突
printf 'MOD-B-plugin\n' > "$INST/mods/ModB/Skyrim.esm"          # 覆盖游戏本体的同名文件
printf 'MOD-C-plugin\n' > "$INST/mods/ModC/Scripts/keep.pex"

mkdir -p "$INST/profiles/Default"
# modlist.txt：文件第一条是最高优先级（低→高 = 反序）
cat > "$INST/profiles/Default/modlist.txt" <<'EOF'
-ModC
+ModB
+_separator
+ModA
EOF

echo "== instance init =="
OUT=$("$MO" -i "$INST" instance init --game-dir "$GAME" --prefix "$WORK/prefix" --json); RC=$?
check_rc 0 $RC "instance init"
has "$OUT" '"changed":true' "init: changed=true"
has "$OUT" '"root":"'"$INST"'"' "init: root"
has "$OUT" '"game_dir":"'"$GAME"'"' "init: game_dir"
has "$OUT" '"config":{' "init: 带回 config"
OUT=$("$MO" -i "$INST" instance init --game-dir "$GAME" --prefix "$WORK/prefix" --json); RC=$?
check_rc 0 $RC "instance init 第二次（幂等）"
has "$OUT" '"changed":false' "init: 第二次 changed=false"
[ -f "$INST/profiles/Default/modlist.txt" ] && ok "init 未覆盖已有 modlist.txt" || bad "modlist.txt 被覆盖了"

echo "== instance show =="
OUT=$("$MO" -i "$INST" instance show --json); RC=$?
check_rc 0 $RC "instance show"
has "$OUT" '"farm_path":"'"$INST"'/farm"' "show: farm_path"
has "$OUT" '"mods_dir":"'"$INST"'/mods"' "show: mods_dir"
has "$OUT" '"profile":"Default"' "show: config.profile"

echo "== mods list =="
OUT=$("$MO" -i "$INST" mods list --json); RC=$?
check_rc 0 $RC "mods list"
has "$OUT" '"profile":"Default"' "list: profile"
has "$OUT" '"name":"_separator"' "list: 分隔符条目在列"
has "$OUT" '"name":"ModA"' "list: ModA"
has "$OUT" '"name":"ModC"' "list: ModC"
if printf '%s' "$OUT" | grep -q '"ModA".*"ModC"'; then ok "list: 顺序为 ModA < ModB < ModC（低→高）"; else bad "list: 顺序不符合低→高: $OUT"; fi

echo "== plan（count > 0）=="
OUT=$("$MO" -i "$INST" plan --json); RC=$?
check_rc 0 $RC "plan"
has "$OUT" '"count":' "plan: 有 count"
if printf '%s' "$OUT" | grep -q '"count":0,'; then bad "plan: count 不应为 0"; else ok "plan: count > 0"; fi
has "$OUT" '"kind":"mkdir"' "plan: 有 mkdir"
has "$OUT" '"kind":"link"' "plan: 有 link"
has "$OUT" '"warnings":' "plan: warnings 计数"
hasnt "$OUT" '"warnings":[{' "plan: 无警告时 warnings 数组为空"

echo "== status（漂移 → exit 3）=="
OUT=$("$MO" -i "$INST" status --json); RC=$?
check_rc 3 $RC "status 漂移"
has "$OUT" '"in_sync":false' "status: in_sync=false"
has "$OUT" '"farm_exists":false' "status: 农场尚未物化"

echo "== apply（第一次 changed=true）=="
OUT=$("$MO" -i "$INST" apply --json); RC=$?
check_rc 0 $RC "apply"
has "$OUT" '"changed":true' "apply: changed=true"
if printf '%s' "$OUT" | grep -q '"applied":0,'; then bad "apply: applied 不应为 0"; else ok "apply: applied > 0"; fi
[ -f "$INST/farm/.mol-farm.json" ] && ok "apply: 农场 marker 落盘" || bad "农场 marker 缺失"
[ -L "$INST/farm/Data/Textures/wall.dds" ] && ok "apply: 大小写取胜者的链接落盘" || bad "wall.dds 链接缺失"

echo "== apply 第二次（幂等 changed=false）=="
OUT=$("$MO" -i "$INST" apply --json); RC=$?
check_rc 0 $RC "apply 第二次"
has "$OUT" '"changed":false' "apply: 第二次 changed=false"
has "$OUT" '"applied":0' "apply: 第二次 applied=0"

echo "== status（同步 → exit 0）=="
OUT=$("$MO" -i "$INST" status --json); RC=$?
check_rc 0 $RC "status 同步"
has "$OUT" '"in_sync":true' "status: in_sync=true"
has "$OUT" '"pending":0' "status: pending=0"
has "$OUT" '"farm_exists":true' "status: farm_exists=true"

echo "== conflicts（层名 + 大小写冲突）=="
OUT=$("$MO" -i "$INST" conflicts --json); RC=$?
check_rc 0 $RC "conflicts"
has "$OUT" '"count":2' "conflicts: 2 个冲突文件"
has "$OUT" '"winner":"ModB"' "conflicts: winner 是 ModB"
has "$OUT" '"losers":["<game>"]' "conflicts: 层名 <game>"
has "$OUT" '"path":"Data/Skyrim.esm"' "conflicts: 与游戏本体冲突"
has "$OUT" '"losers":["ModA"]' "conflicts: losers 含 ModA"
has "$OUT" '"path":"Data/Textures/wall.dds"' "conflicts: 大小写冲突路径（规范大小写取自最先引入者 ModA）"
OUT=$("$MO" -i "$INST" conflicts --mod ModA --json); RC=$?
check_rc 0 $RC "conflicts --mod ModA"
has "$OUT" '"count":1' "conflicts --mod ModA: 命中"
OUT=$("$MO" -i "$INST" conflicts --mod ModC --json); RC=$?
check_rc 0 $RC "conflicts --mod ModC"
has "$OUT" '"count":0' "conflicts --mod ModC: 不涉及 → 0"

echo "== mods enable/disable/move =="
OUT=$("$MO" -i "$INST" mods enable ModC --json); RC=$?
check_rc 0 $RC "mods enable ModC"
has "$OUT" '"enabled":true' "enable: enabled=true"
has "$OUT" '"changed":true' "enable: changed=true"
OUT=$("$MO" -i "$INST" mods enable ModC --json); RC=$?
has "$OUT" '"changed":false' "enable 重复执行: changed=false"
OUT=$("$MO" -i "$INST" mods disable ModC --json); RC=$?
has "$OUT" '"enabled":false' "disable: enabled=false"
OUT=$("$MO" -i "$INST" mods move ModC --to 0 --json); RC=$?
check_rc 0 $RC "mods move ModC --to 0"
has "$OUT" '"changed":true' "move: changed=true"
has "$OUT" '"priority":0' "move: priority=0"
OUT=$("$MO" -i "$INST" mods move ModC --to 99 --json); RC=$?
has "$OUT" '"changed":true' "move 越界夹取: changed=true"
OUT=$("$MO" -i "$INST" mods enable Nope --json); RC=$?
check_rc 1 $RC "mods enable 不存在的 mod → mod_not_found"
has "$OUT" '"code":"mod_not_found"' "enable 不存在: code=mod_not_found"

echo "== disable ModB + plan + apply =="
"$MO" -i "$INST" mods disable ModB >/dev/null
OUT=$("$MO" -i "$INST" plan --json); RC=$?
check_rc 0 $RC "plan（禁 ModB 后）"
has "$OUT" '"kind":"relink"' "plan: 赢家改变 → relink（而非 remove）"
has "$OUT" '"target":"'"$INST"'/mods/ModA/Textures/wall.dds"' "plan: relink 到 ModA"
OUT=$("$MO" -i "$INST" apply --json); RC=$?
check_rc 0 $RC "apply（禁 ModB 后）"
has "$OUT" '"changed":true' "apply: changed=true"
if [ "$(readlink "$INST/farm/Data/Textures/wall.dds")" = "$INST/mods/ModA/Textures/wall.dds" ]; then ok "链接已改指到 ModA"; else bad "链接未改指 ModA: $(readlink "$INST/farm/Data/Textures/wall.dds")"; fi

echo "== version =="
OUT=$("$MO" version --json); RC=$?
check_rc 0 $RC "version"
has "$OUT" '"name":"mo-linux","version":"0.0.1"' "version: name/version"

echo "== unlink（两次：removed=true / false）=="
OUT=$("$MO" -i "$INST" unlink --json); RC=$?
check_rc 0 $RC "unlink"
has "$OUT" '"removed":true' "unlink: removed=true"
OUT=$("$MO" -i "$INST" unlink --json); RC=$?
check_rc 0 $RC "unlink 第二次（幂等）"
has "$OUT" '"removed":false' "unlink: removed=false"
[ -e "$INST/farm" ] && bad "农场目录应被删除" || ok "农场目录已删除"

echo "== 错误路径 =="
OUT=$("$MO" -i "$WORK/does-not-exist" plan --json); RC=$?
check_rc 1 $RC "实例不存在"
has "$OUT" '"ok":false' "实例不存在: ok=false"
has "$OUT" '"code":"instance_not_found"' "实例不存在: code"
OUT=$("$MO" frobnicate --json); RC=$?
check_rc 2 $RC "未知命令"
has "$OUT" '"code":"invalid_argument"' "未知命令: code=invalid_argument"
OUT=$("$MO" instance --json); RC=$?
check_rc 2 $RC "只有 group 节点（缺子命令）"
has "$OUT" '"code":"invalid_argument"' "group: code=invalid_argument"
OUT=$("$MO" mods enable --json); RC=$?
check_rc 2 $RC "缺位置参数"
has "$OUT" '"code":"invalid_argument"' "缺 NAME: invalid_argument"
has "$OUT" 'NAME is required' "缺 NAME: 提示信息"
OUT=$("$MO" -i "$INST" mods enable ModA ModB --json); RC=$?
check_rc 2 $RC "多余位置参数（不执行变更）"
has "$OUT" 'unexpected argument' "多余参数: 提示信息"
OUT=$("$MO" -i "$INST" plan --bogus --json); RC=$?
check_rc 2 $RC "未注册选项"
has "$OUT" 'unknown option --bogus' "未注册选项: 提示信息"
OUT=$("$MO" -i "$INST" mods list --to 3 --json); RC=$?
check_rc 2 $RC "别的命令的选项用错地方"
has "$OUT" 'unknown option --to' "跨命令选项: 提示信息"
OUT=$("$MO" -i "$INST" mods move ModA --json); RC=$?
check_rc 2 $RC "选项缺值"
has "$OUT" '--to needs a non-negative integer' "缺值: 提示信息"
OUT=$("$MO" -i "$INST" mods move ModA --to notanumber --json); RC=$?
check_rc 2 $RC "选项值类型不对"
OUT=$("$MO" -j bogus2 --json); RC=$?
check_rc 2 $RC "未知命令也尊重 -j（仍输出 envelope）"
has "$OUT" '"command":"bogus2"' "未知命令: envelope.command"
OUT=$("$MO" --events bogus plan --json 2>"$LOG_ERR"); RC=$?
check_rc 2 $RC "--events 目标非法"
has "$OUT" 'invalid --events target' "events: 用法错误信息"

echo "== --events fd:N =="
EVENTS="$WORK/events.ndjson"
"$MO" -i "$INST" --events "fd:7" apply --json >/dev/null 7>"$EVENTS"; RC=$?
# 上面 apply 已经同步过，没有 op；重新制造漂移再跑一次以拿到 progress 事件
"$MO" -i "$INST" mods enable ModB >/dev/null
"$MO" -i "$INST" --events "fd:7" apply --json >/dev/null 7>"$EVENTS"; RC=$?
check_rc 0 $RC "apply --events fd:7"
if [ -s "$EVENTS" ]; then ok "events: 产生了 NDJSON 输出"; else bad "events: 输出为空"; fi
grep -q '"event":"start"' "$EVENTS" && ok "events: 有 start" || bad "events: 缺 start"
grep -q '"event":"progress"' "$EVENTS" && ok "events: 有 progress" || bad "events: 缺 progress"
grep -q '"event":"done"' "$EVENTS" && ok "events: 有 done" || bad "events: 缺 done"
if head -1 "$EVENTS" | grep -q '"event":"start"'; then ok "events: start 是首行"; else bad "events: 首行不是 start"; fi
if tail -1 "$EVENTS" | grep -q '"event":"done".*"ok":true'; then ok "events: done 是末行且 ok"; else bad "events: 末行不是 done"; fi
if grep -q '"total":[0-9]*' "$EVENTS"; then ok "events: progress 带 total"; else bad "events: progress 缺 total"; fi
# 每一行都必须是完整 JSON 行
BADLINES=$(grep -cv '^{.*}$' "$EVENTS" || true)
if [ "$BADLINES" = "0" ]; then ok "events: 全部行均为单行 JSON"; else bad "events: $BADLINES 行不完整"; fi
echo "  events 内容:"; sed 's/^/    /' "$EVENTS" | head -8

echo "== --events fifo:（无读端不阻塞命令）=="
"$MO" -i "$INST" mods disable ModB >/dev/null   # 制造漂移
FIFO="$WORK/evt.fifo"
mkfifo "$FIFO"
OUT=$("$MO" -i "$INST" --events "fifo:$FIFO" status --json 2>"$WORK/fifo.err"); RC=$?
check_rc 3 $RC "status --events fifo（无读端）"
has "$OUT" '"in_sync":false' "fifo: 命令照常完成（退出码仍是 3）"
if grep -q "events sink disabled" "$WORK/fifo.err"; then ok "fifo: 无读端在 stderr 留一条 warn"; else bad "fifo: 缺 stderr warn"; fi
rm -f "$FIFO"

echo "== 全局选项摆放 / -- / -q =="
OUT=$("$MO" --json -i "$INST" mods list); RC=$?
check_rc 0 $RC "全局选项在命令前后都能识别"
has "$OUT" '"command":"mods list"' "选项位置: envelope 正确"
OUT=$("$MO" -i "$INST" mods list -j --profile Default --json); RC=$?
check_rc 0 $RC "选项夹杂在命令与位置参数之间"
has "$OUT" '"profile":"Default"' "选项位置: 全局选项生效"
# -- 之后一律是位置参数："-j" 不再是开关
OUT=$("$MO" -j -i "$INST" mods list -- -j --json 2>/dev/null); RC=$?
check_rc 2 $RC "-- 后的 -j 是位置参数 → 位置参数过多"
has "$OUT" 'unexpected argument' "--: 位置参数被当字面量"
# 错误时 stderr 有日志；-q 时静默；stdout 仍只有结果
OUT=$("$MO" -i "$WORK/does-not-exist" plan --json 2>"$LOG_ERR"); RC=$?
check_rc 1 $RC "默认日志（warn+）到 stderr"
if grep -q "instance_not_found" "$LOG_ERR"; then ok "stderr: 有 error 日志"; else bad "stderr: 缺 error 日志"; fi
if [ -s "$LOG_ERR" ] && ! grep -q "^{" "$LOG_ERR"; then ok "stderr: 不含 envelope（stdout 唯一）"; else bad "stderr 内容异常"; fi
OUT=$("$MO" -i "$WORK/does-not-exist" -q --json plan 2>"$LOG_ERR"); RC=$?
check_rc 1 $RC "-q 下错误路径"
if [ -s "$LOG_ERR" ]; then bad "-q 时 stderr 应为空"; else ok "-q: stderr 静默"; fi
has "$OUT" '"code":"instance_not_found"' "-q: envelope 仍在 stdout"
# 日志走 stderr 的同时 stdout 仍是纯 envelope（可被 GUI 解析）
OUT=$("$MO" -i "$WORK/does-not-exist" plan --json 2>/dev/null)
if printf '%s' "$OUT" | python3 -c 'import json,sys; s=sys.stdin.read(); assert "\n" not in s; assert json.loads(s)["schema_version"] == 1'; then ok "stdout: 单行 envelope"; else bad "stdout 不是单行"; fi
OUT=$(MOL_INSTANCE="$INST" "$MO" status --json); RC=$?
check_rc 3 $RC "MOL_INSTANCE 环境变量回退"
has "$OUT" '"farm_path":"'"$INST"'/farm"' "MOL_INSTANCE: 生效"

echo
echo "======================================"
echo "  e2e: $PASS passed, $FAIL failed"
echo "======================================"
[ "$FAIL" = "0" ]
