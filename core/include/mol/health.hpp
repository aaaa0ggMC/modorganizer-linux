#pragma once
// 前缀/实例的「疑难杂症」体检与修复（docs/PLAN-autofix.md 的 D1–D4）。检测函数只读、离线、秒级；
// 修复函数会改前缀（先备份），由显式命令调用（`fix …`、`enb install`），不在 doctor 里自动执行。
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mol/instance.hpp"

namespace mol::health {

// <prefix>/drive_c/users/<user>/AppData/Local/Skyrim Special Edition
std::filesystem::path appdata_dir(const Instance& inst);
// <prefix>/drive_c/users/<user>/Documents/My Games/Skyrim Special Edition
std::filesystem::path my_games_dir(const Instance& inst);

// ---- D1 ContentCatalog.txt ---------------------------------------------------------------
// 用 Steam 跑过 1.7.x 后，ContentCatalog.txt 里会出现 "Version" : "1701307962.== Version Number ==" 这类条目，
// 1.6.x 解析时抛 C++ 异常、启动约 8 秒后静默退出。返回不是「数字(.数字)」的 Version 值（去重，按出现顺序）。
std::vector<std::string> bad_catalog_versions(std::string_view json_text);
// 游戏版本 < 1.7 时才有害（1.7.x 自己能读）。版本未知按有害处理。
bool catalog_versions_harmful(std::string_view game_version);
// 把 ContentCatalog.txt 改名为 ContentCatalog.txt.mol-bak-<UTC 时间>（游戏下次启动会重建）。返回备份路径；文件不存在返回空。
std::string move_content_catalog_aside(const Instance& inst);

// ---- D2 VC++ 运行库 -----------------------------------------------------------------------
// PE 文件 VERSIONINFO 里 VS_FIXEDFILEINFO 的 FileVersion（a.b.c.d）；读不到返回空。
std::optional<std::array<int, 4>> pe_file_version(const std::filesystem::path& file);
std::string version_string(const std::array<int, 4>& v);
struct RuntimeDll {
    std::string name;      // msvcp140.dll …
    std::string version;   // 空 = 文件不存在或读不到版本
    bool ok = false;       // ≥ 14.40（VS2022 17.10 起的插件需要；更旧的运行库上 std::mutex 一加锁就崩）
};
// 检查前缀 system32 里的 msvcp140.dll / vcruntime140.dll / vcruntime140_1.dll。
std::vector<RuntimeDll> vc_runtime(const Instance& inst);
bool vc_runtime_ok(const std::vector<RuntimeDll>& dlls);
// 微软的 x64 再发行包（与 winetricks vcrun2022 相同的地址）
constexpr std::string_view kVcRedistUrl = "https://aka.ms/vs/17/release/vc_redist.x64.exe";

// ---- Steam 客户端 ------------------------------------------------------------------------
// 本机的 Steam 客户端（steam_root/ubuntu12_32/steam，或任何 exe 名为 steam 的原生进程）是否在运行。
// 不在运行时启动游戏：SteamAPI_Init 失败，游戏（Steam DRM）发出 steam://run/489830 让 Steam 重新拉起
// Steam 库里的那份游戏——不经过 mo-linux（没有 mod、没有 COW），还会把 ContentCatalog.txt 写坏。
bool steam_client_running(std::string_view steam_root);

// ---- D4b 大小写影子 ------------------------------------------------------------------------
// AppData 里 plugins.txt / loadorder.txt 旁边只差大小写的同名条目（Wine 会优先打开大小写一致的那个，
// 游戏读到的就不是 profile 的列表）。返回这些条目的完整路径。
std::vector<std::string> case_shadows(const Instance& inst);

// ---- SKSE 日志 -----------------------------------------------------------------------------
struct SkseLog {
    bool found = false;
    std::filesystem::file_time_type mtime{};
    std::size_t loaded = 0;                 // "loaded correctly"
    std::vector<std::string> failed;        // "disabled, fatal error …" / "couldn't load plugin …"
    std::vector<std::string> incompatible;  // "reported as incompatible …"
};
SkseLog parse_skse_log(std::string_view text);
SkseLog read_skse_log(const Instance& inst);  // <My Games>/SKSE/skse64.log

// ---- D3 ENB --------------------------------------------------------------------------------
// 需要 ENB 二进制的迹象（找到的第一个）：农场根/游戏目录/根目录型 mod 里的 enblocal.ini、enbseries/，
// 或任一启用 mod 的 SKSE/Plugins 下的 ENBHelperSE.dll / KiENBExtender.dll。没有返回空。
std::string enb_wanted_by(const Instance& inst);
bool enb_binaries_present(const Instance& inst);  // 游戏目录或启用的根目录型 mod 顶层有 d3d11.dll

// ---- D4 mod 布局（旧版本装坏的） -----------------------------------------------------------
struct LayoutIssue {
    std::string mod;   // mods/ 下的目录名
    std::string kind;  // nested_data | raw_fomod | backslash_name
    std::string detail;
};
// 只看每个 mod 的顶层（秒级）：
//   nested_data    顶层有 Data/，其余都不是游戏数据（说明文档之类）→ 内容不会生效
//   raw_fomod      顶层直接有 ModuleConfig.xml → 整包装进来了、FOMOD 没被执行
//   backslash_name 顶层条目名含 '\'（Windows zip 的路径分隔符被当成文件名）
//   enb_in_data    非根目录型 mod 顶层有 enbseries/、enblocal.ini（ENB 预设落到 Data/ 下，ENB 读不到）
std::vector<LayoutIssue> scan_mod_layouts(const Instance& inst);

}  // namespace mol::health
