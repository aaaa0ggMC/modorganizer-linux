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
// 启动 Steam 客户端（PATH 里的 steam，否则 <steam_root>/steam.sh；-silent，脱离本进程）并等到它登录完成
// （<steam_root>/logs/connection_log.txt 在启动后新出现 "[Logged On"）。超时返回 false（Steam 可能在等人输密码）。
bool start_steam_and_wait(std::string_view steam_root, int timeout_ms = 120000);

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

// ---- D7 崩溃与启动诊断 ---------------------------------------------------------------------
struct CrashSummary {
    std::string file;                     // 完整路径；空 = 没有
    std::filesystem::file_time_type mtime{};
    std::string exception;                // "Unhandled exception "EXCEPTION_ACCESS_VIOLATION" at 0x… SkyrimSE.exe+…"
    std::string cxx_type, cxx_info;       // C++ 异常：Type / Info（如 std::invalid_argument* / invalid stoull argument）
    std::vector<std::string> modules;     // 调用栈前几帧（[P]robable）涉及的模块（去重，按出现顺序）
    std::string first_own_frame;          // 第一个不是系统/运行库 dll 的帧，如 "SkyrimSE.exe+1235AE9"、"SomePlugin.dll+00ABCDE"
    std::vector<std::string> files;       // 栈上出现的文件名（.txt/.ini/.esp/.esm/.esl/.dll/.nif/.dds/.pex …，去重）
    std::vector<std::string> plugins;     // POSSIBLE RELEVANT OBJECTS 里提到的插件
    std::string hint;                     // 认得出的已知原因 → 建议（如 ContentCatalog.txt → `fix content-catalog`）
};
// CrashLogger 的 crash-*.log（v1.x 的 "PROBABLE CALL STACK:" 与 v1.2x 的 "CALL STACK ([P]robable / [S]tack scan):" 都认）。
CrashSummary parse_crash_log(std::string_view text);
// <My Games>/SKSE 下最新的 crash-*.log（已解析）；没有则 file 为空。
CrashSummary latest_crash(const Instance& inst);

struct ProtonException {
    std::string code;    // 十六进制，如 "e06d7363"、"c0000005"
    std::string module;  // 抛出/出错的模块（按加载基址推出）；推不出为空
    std::string thread;
};
struct ProtonDiagnosis {
    std::vector<std::pair<std::string, std::size_t>> codes;  // 值得注意的异常码 → 次数（排除线程命名、调试输出、RPC 之类的噪音）
    std::optional<ProtonException> last;                     // 最后一个值得注意的异常
    std::vector<std::string> gpus;                           // DXVK 枚举到的显卡（去重）
    std::string gpu_used;                                    // DXVK 实际建设备用的那块（"Device properties: Device : …"）
};
// 有独显却跑在核显上：返回独显名（给 DXVK_FILTER_DEVICE_NAME 用）；否则空。
std::string discrete_gpu_unused(const ProtonDiagnosis& d);
// 解析 PROTON_LOG=1 + WINEDEBUG=+seh,+loaddll 得到的 steam-<appid>.log。
// 模块：C++ 异常（e06d7363）用 info[3]（抛出模块的基址），其余用 addr，对照 "Loaded L"…" at <基址>" 找最近的一个。
ProtonDiagnosis parse_proton_log(std::string_view text);
std::string describe_exception_code(std::string_view code);  // c0000005 → "access violation" …
// 给人看的几行崩溃摘要（异常、C++ 类型/信息、第一个非系统帧、栈上的文件与插件、已知原因的建议）。
std::vector<std::string> crash_summary_lines(const CrashSummary& c);

// ---- D3 ENB --------------------------------------------------------------------------------
// 需要 ENB 二进制的迹象（找到的第一个）：农场根/游戏目录/根目录型 mod 里的 enblocal.ini、enbseries/，
// 或任一启用 mod 的 SKSE/Plugins 下的 ENBHelperSE.dll / KiENBExtender.dll。没有返回空。
std::string enb_wanted_by(const Instance& inst);
bool enb_binaries_present(const Instance& inst);  // 游戏目录或启用的根目录型 mod 顶层有 d3d11.dll

// ---- P0-3 文件在哪 ------------------------------------------------------------------------
struct FileHit {
    std::string where;  // mod 名，或 downloads/ 下的压缩包文件名
    std::string path;   // 在 mod 目录 / 压缩包里的相对路径
    bool in_archive = false;
    bool enabled = true;  // mod：是否启用
};
// 在所有 mod 目录（递归）里按文件名（大小写不敏感）找；archives=true 时再列 downloads/ 里每个压缩包的条目（慢：每个包跑一次 7z l）。
std::vector<FileHit> find_file(const Instance& inst, std::string_view file_name, bool archives = false);

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
