#pragma once
#include <stdexcept>
#include <string>

namespace mol {

// 带稳定错误码的异常；CLI 把它原样映射成 JSON errors[] 条目。
// 约定的 code（snake_case，不可随意改名，GUI 会依赖）：
//   instance_not_found   实例目录不存在或不含 ModOrganizer.ini / mo-linux.json
//   config_invalid       mo-linux.json / ModOrganizer.ini 缺字段或类型不对
//   profile_not_found    profile 目录不存在
//   mod_not_found        modlist 中没有该 mod
//   invalid_argument     参数取值非法
//   farm_not_owned       农场目录非空且不含我们的 marker（拒绝触碰）
//   farm_conflict        期望位置被用户文件占据
//   farm_busy            农场正被运行中的游戏使用（拒绝 apply/unlink/capture）
//   network_error        传输层失败 / 下载 HTTP 错误
//   nexus_auth           缺少/无效的 Nexus API key（401）
//   nexus_premium        该操作需要 Premium 或 nxm 链接里的 key（403）
//   nexus_not_found      mod/文件不存在（404）
//   nexus_rate_limited   触发 Nexus 限流（429）
//   skse_mismatch        Nexus 上最新的 SKSE64 不支持当前游戏版本
//   fomod_choices_required  压缩包带 FOMOD 但调用方没有指明如何选择
//   io_error             其它文件系统错误
//   game_unavailable     libmo-game 未找到或初始化失败
struct Error : std::runtime_error {
    std::string code;
    std::string path;  // 相关路径，可空
    Error(std::string code_, const std::string& message, std::string path_ = {})
        : std::runtime_error(message), code(std::move(code_)), path(std::move(path_)) {}
};

}  // namespace mol
