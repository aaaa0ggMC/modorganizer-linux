#pragma once
// 启动游戏：把「实例 + 要运行的 exe」翻译成 (argv, env, cwd)。构造过程是纯函数（不碰进程/环境），
// 方便单测；真正的 exec/spawn 在 spawn_launch。
#include <string_view>
#include <utility>

#include "mol/instance.hpp"
#include "mol/pmr.hpp"

namespace mol {

struct LaunchSpec {
    using allocator_type = mol::allocator_type;
    vector<string> argv;
    vector<std::pair<string, string>> env;  // 要在继承的环境之上覆盖/追加的变量
    string cwd;
    // 子进程的 stdout/stderr 写到这里（追加）。空：等待模式下继承 stderr（stdout 也指向它）；
    // 后台模式下丢到 /dev/null——绝不能继承调用方的管道，否则游戏活多久、调用方读管道就卡多久。
    string log_file;

    explicit LaunchSpec(allocator_type a = {}) : argv(a), env(a), cwd(a), log_file(a) {}
    LaunchSpec(const LaunchSpec& o, allocator_type a) : argv(o.argv, a), env(o.env, a), cwd(o.cwd, a), log_file(o.log_file, a) {}
    LaunchSpec(LaunchSpec&& o, allocator_type a) : argv(std::move(o.argv), a), env(std::move(o.env), a), cwd(std::move(o.cwd), a), log_file(std::move(o.log_file), a) {}
    LaunchSpec(const LaunchSpec&) = default;
    LaunchSpec(LaunchSpec&&) = default;
    LaunchSpec& operator=(const LaunchSpec&) = default;
    LaunchSpec& operator=(LaunchSpec&&) = default;
};

struct LaunchOptions {
    std::string_view steam_app_id = "489830";   // Skyrim SE
    std::span<const std::string_view> args;      // 传给游戏 exe 的参数
    // 写时复制：非空时经 LD_PRELOAD 注入 libmol-cow.so（见 cow/mol_cow.c），农场里的链接被以写方式打开前
    // 先换成真实副本，原文件（游戏目录、mod）永远不被改写。空 = 不启用。
    std::string_view cow_library;
    std::string_view cow_log;                    // MOL_COW_LOG（overwrite capture 据此丢弃没改动的副本）
};

// libmol-cow.so 的位置：$MOL_COW_LIB → mo-linux 可执行文件同目录。找不到返回空串。
string find_cow_library(mr* mem = default_mr());

// Wine 的许多文件操作由 wineserver 代为执行，而 wineserver 会在最后一个程序退出后再存活几秒、被后来者复用。
// 若这个前缀已有一个**没带同样注入环境**的 wineserver（之前不带 COW 启动的程序留下的），COW 对它不生效，
// 写入会穿过链接改到原文件。启动前调用：没有 wineserver 或环境一致 → 立即返回；否则最多等 timeout_ms 让它
// 自行退出；仍在 → Error{wine_busy}。
void ensure_wineserver_cow(const Instance& inst, std::string_view cow_library, int timeout_ms = 10000);

// exe_rel：相对农场根的可执行文件（如 "skse64_loader.exe"）；以 '/' 开头则当作绝对路径原样使用。不检查文件是否存在（由调用方负责）。
// cfg.runner_kind == "proton"：<proton_path>/proton run <farm>/<exe>，设置
//   STEAM_COMPAT_DATA_PATH（prefix 以 "/pfx" 结尾则取其父目录，否则取 prefix 本身）、
//   STEAM_COMPAT_CLIENT_INSTALL_PATH（cfg.steam_root）、STEAM_COMPAT_APP_ID / SteamAppId / SteamGameId、
//   STEAM_COMPAT_MOUNTS 与 PRESSURE_VESSEL_FILESYSTEMS_RW（冒号分隔：农场、实例根、mods、overwrite、游戏目录），
//   让容器里能解析指向这些目录的符号链接。
// cfg.runner_kind == "wine"：wine <farm>/<exe>，设置 WINEPREFIX=prefix、WINEDEBUG=-all（若调用方环境未设）。
// proton_path 为空（proton 模式）→ Error{config_invalid}；未知 runner_kind → Error{config_invalid}。
// cwd = 农场根。
LaunchSpec build_launch(const Instance& inst, std::string_view exe_rel, const LaunchOptions& opt = {},
                        mr* mem = default_mr());

// 以子进程运行 spec（继承当前环境并叠加 spec.env）。wait=true 阻塞并返回退出码；
// wait=false 立即返回 0（子进程脱离，不成为僵尸；stdin/stdout/stderr 都不与调用方共享，见 LaunchSpec::log_file）。fork/exec 失败抛 Error{io_error}。
int spawn_launch(const LaunchSpec& spec, bool wait);

}  // namespace mol
