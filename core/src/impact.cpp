// 模组影响面分析实现：遍历 mod 目录 → 分类注入地点 → PE 能力归类 → 一句人话。
// 只读；单文件解析失败只降级该条目（记进 summary），绝不让整轮分析失败。
#include "mol/impact.hpp"

#include <algorithm>
#include <filesystem>
#include <set>

#include "mol/casefold.hpp"
#include "mol/error.hpp"

namespace mol::impact {
namespace {
namespace fs = std::filesystem;

using mol::casefold;

// mol::casefold 返回 pmr string；本模块内部一律用 std::string，包一层。
std::string fold(std::string_view s) {
    const mol::string f = casefold(s);
    return std::string(f.data(), f.size());
}

// Windows 加载器按文件名注入的「代理 DLL」名单：出现在游戏根/mod 根就意味着
// **每个**加载该 EXE 的进程都会加载它（含 Steam 覆盖层、启动器）。
constexpr const char* kProxyDlls[] = {
    "d3d9.dll",   "d3d11.dll",  "d3d12.dll",  "dxgi.dll",   "ddraw.dll",  "dsound.dll",
    "dinput.dll", "dinput8.dll", "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll",
    "winhttp.dll", "winmm.dll", "version.dll", "opengl32.dll", "nvwgf2umx.dll", "dbghelp.dll",
};

constexpr const char* kPackedSections[] = {"upx0", "upx1", ".themida", ".vmp0", ".vmp1", "aspack", ".enigma"};

constexpr const char* kWriteApis[] = {"CreateFileW",  "CreateFileA",  "CreateFile2", "WriteFile",
                                      "WriteFileEx", "DeleteFileW",  "DeleteFileA", "MoveFileW",
                                      "MoveFileA",   "MoveFileExW",  "CreateDirectoryW", "CreateDirectoryA",
                                      "RemoveDirectoryW", "RemoveDirectoryA", "SetFileAttributesW",
                                      "SetEndOfFile", "fopen", "fwrite", "fputs"};
constexpr const char* kSpawnApis[] = {"CreateProcessW", "CreateProcessA", "CreateProcessAsUserW",
                                      "ShellExecuteW", "ShellExecuteA", "ShellExecuteExW", "WinExec",
                                      "system", "_popen", "popen", "CreateProcessWithLogonW"};
constexpr const char* kRegistryApis[] = {"RegSetValueExW", "RegSetValueExA", "RegSetValueW", "RegCreateKeyExW",
                                         "RegCreateKeyExA", "RegCreateKeyW", "RegDeleteKeyW", "RegDeleteKeyA",
                                         "RegDeleteValueW", "RegDeleteValueA", "RegSetKeyValueW", "RegLoadKeyW"};
constexpr const char* kMemoryApis[] = {"VirtualProtect", "VirtualProtectEx", "WriteProcessMemory",
                                       "VirtualAllocEx", "FlushInstructionCache"};
constexpr const char* kLoadApis[] = {"LoadLibraryW", "LoadLibraryA", "LoadLibraryExW", "LoadLibraryExA",
                                     "LdrLoadDll", "GetProcAddress"};
constexpr const char* kNetworkDlls[] = {"ws2_32.dll", "wsock32.dll", "winhttp.dll", "wininet.dll",
                                        "urlmon.dll", "iphlpapi.dll"};

bool in_list(const std::string& folded, const char* const* list, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (folded == fold(list[i])) return true;
    return false;
}

void add_evidence(Capabilities& c, const std::string& dll, const std::string& api) {
    if (c.evidence.size() >= 16) return;
    c.evidence.push_back(dll + "!" + api);
}

bool has_export(const pe::Info& info, std::string_view name) {
    const std::string want = fold(name);
    for (const auto& e : info.exports)
        if (fold(e.name) == want) return true;
    return false;
}

std::string lower_ext(const fs::path& p) {
    std::string e = p.extension().string();
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// 一个文件属于哪种注入地点（导出表优先于路径：路径会骗人，PE 内容不会）
InjectionPoint classify(const fs::path& rel, const std::optional<pe::Info>& pe) {
    InjectionPoint p;
    p.path = rel.generic_string();
    const std::string leaf = fold(rel.filename().string());
    const std::string ext = lower_ext(rel);
    // 相对 mod 根的「顶层」：根目录型 mod 的文件在农场根，其余在 Data/ 下——两者都算 mod 根层
    const std::string parent = fold(rel.parent_path().generic_string());

    const bool skse_plugins_dir = parent == "skse/plugins" || parent.ends_with("/skse/plugins");
    if (ext == ".dll") {
        if (pe && (has_export(*pe, "SKSEPluginLoad") || has_export(*pe, "SKSEPluginQuery"))) {
            p.kind = "skse_plugin";  // 导出表说话：哪怕它躺在 mod 根
            p.loaded_by = "skse";
            p.reach = "game-process";
            return p;
        }
        if (skse_plugins_dir) {
            p.kind = "skse_plugin";
            p.loaded_by = "skse";
            p.reach = "game-process";
            return p;
        }
        if (in_list(leaf, kProxyDlls, sizeof(kProxyDlls) / sizeof(*kProxyDlls))) {
            p.kind = "proxy_dll";
            p.loaded_by = "windows_loader";
            p.reach = "all-processes";  // 每个加载游戏 EXE 的进程
            return p;
        }
        p.kind = "engine_dll";
        p.loaded_by = "engine";
        p.reach = "game-process";
        return p;
    }
    if (ext == ".exe") {
        p.kind = "exe_tool";
        p.loaded_by = "user";
        p.reach = "offline";  // 只写文件，不改运行态
        return p;
    }
    if (ext == ".pex" || ext == ".psc") {
        p.kind = "papyrus";
        p.loaded_by = "game_vm";
        p.reach = "game-logic";
        return p;
    }
    if (ext == ".esp" || ext == ".esm" || ext == ".esl") {
        p.kind = "content";
        p.loaded_by = "game";
        p.reach = "game-content";
        return p;
    }
    if (ext == ".ini" || ext == ".json" || ext == ".toml" || ext == ".cfg" || ext == ".yaml" ||
        ext == ".yml") {
        p.kind = "config";
        p.loaded_by = "game";
        p.reach = "game-content";
        return p;
    }
    p.kind = "other";
    p.loaded_by = "game";
    p.reach = "game-content";
    return p;
}

void scan_capabilities(const pe::Info& info, Capabilities& caps) {
    for (const auto& im : info.imports) {
        const std::string d = fold(im.dll);
        const std::string api = im.by_ordinal ? "#" + std::to_string(im.ordinal) : im.name;
        const std::string fa = fold(api);
        if (d == "kernel32.dll") {
            if (!caps.writes_files && in_list(fa, kWriteApis, sizeof(kWriteApis) / sizeof(*kWriteApis))) {
                caps.writes_files = true;
                add_evidence(caps, im.dll, api);
            }
            if (!caps.spawns_processes && in_list(fa, kSpawnApis, sizeof(kSpawnApis) / sizeof(*kSpawnApis))) {
                caps.spawns_processes = true;
                add_evidence(caps, im.dll, api);
            }
            if (!caps.memory_patch && in_list(fa, kMemoryApis, sizeof(kMemoryApis) / sizeof(*kMemoryApis))) {
                caps.memory_patch = true;
                add_evidence(caps, im.dll, api);
            }
            if (!caps.chain_loads && in_list(fa, kLoadApis, sizeof(kLoadApis) / sizeof(*kLoadApis))) {
                caps.chain_loads = true;
                add_evidence(caps, im.dll, api);
            }
        } else if (d == "advapi32.dll") {
            if (!caps.registry && in_list(fa, kRegistryApis, sizeof(kRegistryApis) / sizeof(*kRegistryApis))) {
                caps.registry = true;
                add_evidence(caps, im.dll, api);
            }
        } else if (in_list(d, kNetworkDlls, sizeof(kNetworkDlls) / sizeof(*kNetworkDlls))) {
            if (!caps.network) {
                caps.network = true;
                add_evidence(caps, im.dll, api.empty() ? "(any)" : api);
            }
        }
    }
}

bool looks_packed(const pe::Info& info) {
    bool has_wx = false;
    for (const auto& s : info.sections) {
        if (s.writable && s.executable) has_wx = true;
        const std::string n = fold(s.name);
        if (in_list(n, kPackedSections, sizeof(kPackedSections) / sizeof(*kPackedSections))) return true;
    }
    // 没有任何导入表的 DLL：要么纯资源 DLL，要么动态解析/加壳——都算不可静态判断
    return has_wx || info.imports.empty();
}

const char* kind_label(std::string_view kind) {
    if (kind == "skse_plugin") return "SKSE 插件";
    if (kind == "proxy_dll") return "代理 DLL（所有进程）";
    if (kind == "engine_dll") return "引擎/插件 DLL";
    if (kind == "exe_tool") return "离线工具";
    if (kind == "papyrus") return "Papyrus 脚本";
    if (kind == "content") return "内容插件";
    if (kind == "config") return "配置文件";
    return "其它文件";
}

std::string build_summary(const ModImpact& m) {
    if (m.injections.empty()) return "没有可分析的注入点";
    std::set<std::string> kinds;
    for (const auto& i : m.injections) kinds.insert(std::string(i.kind));
    std::string s;
    auto add = [&s](const std::string& t) {
        if (!s.empty()) s += "；";
        s += t;
    };
    if (kinds.count("proxy_dll")) add("代理 DLL：每个加载游戏 EXE 的进程都会加载它");
    if (kinds.count("skse_plugin")) add("SKSE 插件（游戏进程内）");
    if (kinds.count("engine_dll")) add("引擎/插件 DLL");
    if (kinds.count("exe_tool")) add("离线工具（只写文件）");
    if (kinds.count("papyrus")) add("Papyrus 脚本");
    if (kinds.count("content")) add("内容插件");
    if (kinds.count("config")) add("配置");
    const auto& c = m.caps;
    if (c.memory_patch) add("会 patch 内存");
    if (c.writes_files) add("会写文件");
    if (c.spawns_processes) add("会起进程");
    if (c.network) add("有网络能力");
    if (c.registry) add("会写注册表");
    if (c.chain_loads) add("会加载其它 DLL");
    if (c.unknown) add("能力不可静态判断（无导入表/疑似加壳）");
    if (m.packed_suspect) add("疑似加壳：静态分析不可信");
    return s;
}

}  // namespace

ModImpact analyze_mod(const Instance& inst, std::string_view mod_in, mr* mem) {
    const std::string want = fold(mod_in);
    const auto mods = mol::list_mods(inst, {}, mem);
    const mol::ModInfo* found = nullptr;
    for (const auto& m : mods)
        if (fold(m.name) == want) found = &m;
    if (!found) throw Error("mod_not_found", "no such mod: " + std::string(mod_in));
    ModImpact out;
    out.mod = string(found->name, mem);
    const fs::path root(found->path.empty() ? std::string(inst.mods_dir) + "/" + std::string(found->name)
                                            : std::string(found->path));
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;  // 目录不在：空结果（调用方可报 missing）

    // 只走「有意义」的文件：PE、脚本、内容、配置；贴图/网格/音声不分析（没有注入语义）。
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = lower_ext(it->path());
        static const std::set<std::string> interesting = {".dll", ".exe", ".pex", ".psc", ".esp", ".esm",
                                                          ".esl", ".ini", ".json", ".toml", ".cfg",
                                                          ".yaml", ".yml"};
        if (interesting.count(ext)) files.push_back(it->path());
        if (files.size() > 4096) break;  // 有病的大 mod：截断并继续
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        const auto rel = fs::relative(f, root, ec);
        if (ec || rel.empty()) continue;
        std::optional<pe::Info> pe;
        const std::string ext = lower_ext(f);
        if (ext == ".dll" || ext == ".exe") {
            pe = mol::pe::parse(f.string());
            if (!pe) {
                // 解析不了：仍然报注入地点（路径说了算），但能力标 unknown
                InjectionPoint p = classify(rel, std::nullopt);
                out.injections.push_back(p);
                out.caps.unknown = true;
                continue;
            }
        }
        const InjectionPoint p = classify(rel, pe);
        out.injections.push_back(p);
        if (pe) {
            scan_capabilities(*pe, out.caps);
            if (looks_packed(*pe)) {
                out.packed_suspect = true;
                out.caps.unknown = true;
            }
        }
    }
    if (out.injections.empty()) {
        out.summary = "没有可分析的注入点";  // 纯资源 mod：不是错误，只是没什么可分析
        return out;
    }
    out.summary = build_summary(out);
    return out;
}

std::vector<ModImpact> analyze_mods(const Instance& inst, std::span<const string> mods, mr* mem) {
    std::vector<ModImpact> out;
    out.reserve(mods.size());
    for (const auto& m : mods) out.push_back(analyze_mod(inst, m, mem));
    return out;
}

}  // namespace mol::impact
