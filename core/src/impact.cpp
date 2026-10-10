// 模组影响面分析实现：遍历 mod 目录 → 分类注入地点 → PE 能力归类 → 一句人话。
// 只读；单文件解析失败只降级该条目（记进 summary），绝不让整轮分析失败。
#include "mol/impact.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

#include "mol/casefold.hpp"
#include "mol/error.hpp"
#include "mol/xxh64.hpp"

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

// ---- 磁盘缓存 -----------------------------------------------------------------
// 键 = xxh64(实例根 | mod 名 | 目录 mtime)。目录 mtime 变（增删文件）即失效；
// 就地改文件内容不变 mtime——mod 目录的场景可接受（与 fomod 缓存同样的取舍）。
namespace {

std::string cache_root() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    const std::string base = (xdg && *xdg) ? std::string(xdg) : (home ? std::string(home) : "/tmp") + "/.cache";
    return base + "/mo-linux/impact";
}

std::uint64_t dir_stamp(const fs::path& dir) {
    std::error_code ec;
    const auto t = fs::last_write_time(dir, ec);
    if (ec) return 0;
    return static_cast<std::uint64_t>(t.time_since_epoch().count());
}

std::string cache_key(const Instance& inst, const std::string& mod, std::uint64_t stamp) {
    std::string s(inst.root.data(), inst.root.size());
    s.push_back('\n');
    s += mod;
    s.push_back('\n');
    s += std::to_string(stamp);
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(xxh64(s)));
    return buf;
}

// 极简文本格式（缓存是性能优化，坏了大不了重算）：
//   mol-impact v1\n<injection: kind|path|loaded_by|reach>\n...\n<caps bitmask>\n<packed 0|1>\n<summary>\n
std::optional<ModImpact> cache_load(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::string line;
    if (!std::getline(in, line) || line != "mol-impact v1") return std::nullopt;
    ModImpact m;
    while (std::getline(in, line)) {
        if (line.rfind("caps ", 0) == 0) {
            const unsigned long long f = std::strtoull(line.c_str() + 5, nullptr, 10);
            m.caps.writes_files = f & (1 << 0);
            m.caps.spawns_processes = f & (1 << 1);
            m.caps.network = f & (1 << 2);
            m.caps.registry = f & (1 << 3);
            m.caps.memory_patch = f & (1 << 4);
            m.caps.chain_loads = f & (1 << 5);
            m.caps.unknown = f & (1 << 6);
            continue;
        }
        if (line.rfind("packed ", 0) == 0) {
            m.packed_suspect = line.substr(7) == "1";
            continue;
        }
        if (line.rfind("summary ", 0) == 0) {
            m.summary = line.substr(8);
            break;
        }
        // injection 行：kind|path|loaded_by|reach（最后一段到行尾）
        std::array<std::string, 4> f{};
        std::size_t at = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            if (i == 3) {
                f[3] = line.substr(at);
                break;
            }
            const auto bar = line.find('|', at);
            if (bar == std::string::npos) return std::nullopt;  // 畸形：整条作废
            f[i] = line.substr(at, bar - at);
            at = bar + 1;
        }
        if (f[3].empty()) return std::nullopt;
        InjectionPoint p;
        p.kind = f[0];
        p.path = f[1];
        p.loaded_by = f[2];
        p.reach = f[3];
        m.injections.push_back(std::move(p));
    }
    if (m.injections.empty() && m.summary.empty()) return std::nullopt;
    return m;
}

void cache_store(const fs::path& file, const ModImpact& m) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return;  // 缓存失败不影响结果
    out << "mol-impact v1\n";
    for (const auto& i : m.injections)
        out << i.kind << '|' << i.path << '|' << i.loaded_by << '|' << i.reach << '\n';
    unsigned long long f = 0;
    f |= m.caps.writes_files ? (1 << 0) : 0;
    f |= m.caps.spawns_processes ? (1 << 1) : 0;
    f |= m.caps.network ? (1 << 2) : 0;
    f |= m.caps.registry ? (1 << 3) : 0;
    f |= m.caps.memory_patch ? (1 << 4) : 0;
    f |= m.caps.chain_loads ? (1 << 5) : 0;
    f |= m.caps.unknown ? (1 << 6) : 0;
    out << "caps " << f << '\n';
    out << "packed " << (m.packed_suspect ? 1 : 0) << '\n';
    out << "summary " << m.summary << '\n';
}

}  // namespace

std::vector<ModImpact> collect_impact(const Instance& inst, mr* mem) {
    const mol::vector<ModInfo> mods = list_mods(inst, {}, mem);
    std::vector<ModImpact> out;
    out.reserve(mods.size());
    std::error_code ec;
    const fs::path cdir(cache_root());
    for (const auto& m : mods) {
        if (m.separator || !m.enabled || !m.exists || m.path.empty()) continue;
        const std::string mod_path(m.path.data(), m.path.size());
        const fs::path dir(mod_path);
        if (!fs::is_directory(dir, ec)) continue;
        const std::uint64_t stamp = dir_stamp(dir);
        const std::string key = cache_key(inst, std::string(m.name.data(), m.name.size()), stamp);
        const fs::path file = cdir / key;
        if (stamp) {
            if (auto hit = cache_load(file)) {
                hit->mod = string(m.name, mem);
                out.push_back(std::move(*hit));
                continue;
            }
        }
        ModImpact imp;
        try {
            imp = analyze_mod(inst, m.name, mem);
        } catch (const std::exception&) {
            continue;  // 单个 mod 失败不牵连整轮收集
        }
        if (stamp) cache_store(file, imp);
        out.push_back(std::move(imp));
    }
    return out;
}

}  // namespace mol::impact
