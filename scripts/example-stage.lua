-- 示例安装脚本（scripts/example-stage.lua）：演示 mo-linux Lua 脚本能做什么、边界在哪里。
-- 跑法：mo-linux script run scripts/example-stage.lua
--       （加 --dry-run 用临时虚拟根、不碰实例；加 --local 不进后台 service）
--
-- 这个脚本完全离线、不起 exe，只演示：虚拟根里的文件 IO、state（可被 HTTP 查看/修改）、日志。
-- 真实安装脚本的形态见 docs/LUA-SCRIPTS.md：net.download → archive.extract → proc.run(安装器 exe)
--   → 把成品整理回虚拟根，再由后续版本的 mods.* API 装进实例（首版不开放实例写 API）。

local VERSION = 1

log.info('example install script v' .. VERSION .. ' starting')

-- 1) 虚拟根里搭好目录结构（所有 IO 都被限制在这里）
fs.mkdir('stage')
fs.mkdir('stage/Data')
fs.mkdir('notes')

-- 2) 写点东西：二进制也安全（string.char 里的 0 字节照样落盘）
fs.write('notes/README.txt', 'staged by example-stage.lua\n')
fs.write('stage/Data/example.esp', 'TES4' .. string.rep('\0', 32))
log.info('staged ' .. tostring(fs.size('stage/Data/example.esp')) .. ' bytes')

-- 3) 自报进度：state 里的键值可以通过 HTTP 看和改
--    GET  http://127.0.0.1:<port>/<namespace>/state
--    POST http://127.0.0.1:<port>/<namespace>/state/<key>
state.set('phase', 'staged')
state.set('version', VERSION)
state.set('files/staged', 2)

-- 4) 把虚拟根里的清单读出来核对一遍（fs.list 返回排序后的数组）
local entries = fs.list('stage/Data')
log.info('stage/Data contains: ' .. table.concat(entries, ', '))
if #entries ~= 1 then
    error('unexpected staging result: ' .. tostring(#entries) .. ' entries')
end
if fs.read('notes/README.txt') ~= 'staged by example-stage.lua\n' then
    error('round-trip check failed')
end

-- 5) 收尾
state.set('phase', 'done')
log.info('example install script finished; inspect the virtual root reported by `script run`')
