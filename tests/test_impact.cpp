// PE 读取器与模组影响面分析的离线测试（tests/test_impact.cpp）。
// 用程序构造的最小 PE 夹具（imports/exports/sections 全可控），不依赖任何真实 DLL。
// 只测公共契约：mol/pe.hpp 的 parse、mol/impact.hpp 的分类与能力归类。
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "minitest.hpp"
#include "mol/impact.hpp"
#include "mol/instance.hpp"
#include "mol/pe.hpp"

namespace fs = std::filesystem;
using namespace mol;
namespace pe = mol::pe;

namespace {

struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_impact_" + std::to_string(::getpid()) + "_" +
                                           std::to_string(counter()++));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Tmp() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    static int& counter() {
        static int c = 0;
        return c;
    }
};

// mol::string(pmr) → std::string，方便断言
std::string text(const mol::string& s) { return std::string(s.data(), s.size()); }

// 把 mod 加进 profile 的 modlist（list_mods 只认 modlist 里的条目）
void enable_in_modlist(const std::string& root, const std::string& name) {
    const fs::path p = fs::path(root) / "profiles/Default/modlist.txt";
    std::ofstream(p, std::ios::app) << "+" << name << "\n";
}

std::vector<std::shared_ptr<Tmp>>& keep_alive() {
    static std::vector<std::shared_ptr<Tmp>> v;
    return v;
}

void put(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::binary) << body;
}

// ---- 最小 PE32+ 构造器 -------------------------------------------------------
// 布局：头 0x200 字节；一个节，RVA 0x1000 ↔ 文件偏移 0x200。RVA→off = rva - 0xE00。
struct PeImport {
    std::string dll;
    std::vector<std::string> names;  // 空 = 用序号
};
struct PeSpec {
    std::uint16_t machine = 0x8664;
    std::vector<std::string> exports;
    std::vector<PeImport> imports;
    bool writable = true, executable = false;
    bool no_imports = false;  // 连导入目录都不给（模拟加壳/纯资源）
};

std::string build_pe(const PeSpec& spec) {
    std::vector<unsigned char> b(0x200 + 0x800, 0);
    auto w16 = [&](std::size_t o, std::uint16_t v) { b[o] = v & 0xff; b[o + 1] = v >> 8; };
    auto w32 = [&](std::size_t o, std::uint32_t v) {
        b[o] = v & 0xff; b[o + 1] = (v >> 8) & 0xff; b[o + 2] = (v >> 16) & 0xff; b[o + 3] = (v >> 24) & 0xff;
    };
    auto w64 = [&](std::size_t o, std::uint64_t v) { w32(o, v & 0xffffffff); w32(o + 4, v >> 32); };
    // DOS 头
    b[0] = 'M'; b[1] = 'Z';
    w32(0x3c, 0x40);
    // PE 签名 + COFF
    const std::size_t pe = 0x40;
    b[pe] = 'P'; b[pe + 1] = 'E';
    w16(pe + 4, spec.machine);
    w16(pe + 6, 1);                       // NumberOfSections
    w16(pe + 20, 0xF0);                   // SizeOfOptionalHeader
    // 可选头 PE32+
    const std::size_t opt = pe + 24;
    w16(opt, 0x20b);
    w32(opt + 60, 0x200);                 // SizeOfHeaders
    w32(opt + 56, 0x2000);                // SizeOfImage
    // 数据目录：export=0, import=1
    const std::size_t dirs = opt + 112;
    // 节表
    const std::size_t sec = opt + 0xF0;
    std::memcpy(&b[sec], ".text", 5);
    w32(sec + 8, 0x800);                  // VirtualSize
    w32(sec + 12, 0x1000);                // VirtualAddress
    w32(sec + 16, 0x800);                 // SizeOfRawData
    w32(sec + 20, 0x200);                 // PointerToRawData
    w32(sec + 36, (spec.writable ? 0x80000000u : 0u) | (spec.executable ? 0x20000000u : 0u) | 0x40000000u);

    // 数据区 bump 分配（RVA 从 0x1000 起）
    std::size_t rva = 0x1000;
    auto alloc = [&](std::size_t n) {
        const std::size_t at = rva;
        rva += (n + 7) & ~std::size_t(7);
        return at;
    };
    auto off = [](std::uint32_t r) { return static_cast<std::size_t>(r) - 0xE00; };

    // 导出表
    if (!spec.exports.empty()) {
        const std::uint32_t eat = alloc(4 * spec.exports.size());
        const std::uint32_t ent = alloc(4 * spec.exports.size());
        const std::uint32_t eot = alloc(2 * spec.exports.size());
        const std::uint32_t dir = alloc(40);
        const std::uint32_t func0 = alloc(16);
        w32(dirs + 0 * 8, dir);
        w32(dirs + 0 * 8 + 4, 40);
        w32(off(dir) + 16, 1);                       // Base
        w32(off(dir) + 20, 1);                       // NumberOfFunctions
        w32(off(dir) + 24, spec.exports.size());     // NumberOfNames
        w32(off(dir) + 28, eat);
        w32(off(dir) + 32, ent);
        w32(off(dir) + 36, eot);
        w32(off(eat), func0);
        for (std::size_t i = 0; i < spec.exports.size(); ++i) {
            const std::uint32_t s = alloc(spec.exports[i].size() + 1);
            std::memcpy(&b[off(s)], spec.exports[i].c_str(), spec.exports[i].size() + 1);
            w32(off(ent) + i * 4, s);
            w16(off(eot) + i * 2, static_cast<std::uint16_t>(i));
        }
    }
    // 导入表：每个 DLL 一个 IMAGE_IMPORT_DESCRIPTOR，末尾一个全零终止项
    if (!spec.imports.empty() && !spec.no_imports) {
        const std::size_t ndll = spec.imports.size();
        const std::uint32_t dirs_start = alloc(20 * (ndll + 1));  // +1 = 零终止
        w32(dirs + 1 * 8, dirs_start);
        w32(dirs + 1 * 8 + 4, 20 * (ndll + 1));
        for (std::size_t i = 0; i < ndll; ++i) {
            const auto& im = spec.imports[i];
            const std::size_t n = im.names.size();
            const std::uint32_t oft = alloc(8 * (n + 1));  // +1 = thunk 数组零终止
            const std::uint32_t ft = alloc(8 * (n + 1));
            const std::uint32_t namer = alloc(im.dll.size() + 1);
            std::memcpy(&b[off(namer)], im.dll.c_str(), im.dll.size() + 1);
            const std::size_t d = off(dirs_start) + i * 20;
            w32(d + 0, oft);
            w32(d + 12, namer);
            w32(d + 16, ft);
            for (std::size_t k = 0; k < n; ++k) {
                const std::uint32_t hn = alloc(2 + im.names[k].size() + 1);
                w16(off(hn), 0);
                std::memcpy(&b[off(hn) + 2], im.names[k].c_str(), im.names[k].size() + 1);
                w64(off(oft) + k * 8, hn);
                w64(off(ft) + k * 8, hn);
            }
        }
    }
    return std::string(reinterpret_cast<char*>(b.data()), b.size());
}

std::optional<pe::Info> parse_bytes(const std::string& bytes, const std::string& name = "t.dll") {
    const fs::path p = fs::temp_directory_path() / ("mol_pe_" + std::to_string(::getpid()) + "_" + name);
    put(p, bytes);
    auto r = pe::parse(p.string());
    std::error_code ec;
    fs::remove(p, ec);
    return r;
}

bool has_import(const pe::Info& i, const std::string& dll, const std::string& name) {
    for (const auto& im : i.imports)
        if (im.dll == dll && im.name == name) return true;
    return false;
}

// 建一个最小实例（游戏目录 + instance init），返回实例根。
// Tmp 由 shared_ptr 持有：函数返回后目录必须还在（否则 load_instance 失败）。
std::string make_instance() {
    auto t = std::make_shared<Tmp>();
    const std::string game = (t->dir / "game").string();
    put(t->dir / "game/SkyrimSE.exe", "fixture");
    const std::string root = (t->dir / "inst").string();
    const std::string prefix = (t->dir / "inst/prefix").string();
    InitOptions io;
    io.root = root;
    io.game_dir = game;
    io.prefix = prefix;
    io.runner_kind = "wine";
    io.profile = "Default";
    init_instance(io);
    keep_alive().push_back(t);  // 进程退出才清理
    return root;
}

}  // namespace

// ---- mol::pe -----------------------------------------------------------------

TEST(pe_parses_imports_exports_and_sections) {
    PeSpec spec;
    spec.exports = {"SKSEPluginLoad", "SKSEPluginVersion"};
    spec.imports = {{"kernel32.dll", {"WriteFile", "CreateProcessW"}}, {"ws2_32.dll", {"socket"}}};
    const auto info = parse_bytes(build_pe(spec));
    CHECK(info.has_value());
    if (!info) return;
    CHECK_EQ(info->machine, std::string("amd64"));
    CHECK_EQ(info->exports.size(), std::size_t{2});
    CHECK_EQ(info->exports[0].name, std::string("SKSEPluginLoad"));
    CHECK_EQ(info->exports[0].ordinal, std::uint32_t{1});
    CHECK(has_import(*info, "kernel32.dll", "WriteFile"));
    CHECK(has_import(*info, "kernel32.dll", "CreateProcessW"));
    CHECK(has_import(*info, "ws2_32.dll", "socket"));
    CHECK_EQ(info->sections.size(), std::size_t{1});
    CHECK_EQ(info->sections[0].name, std::string(".text"));
    CHECK(info->sections[0].writable);
    CHECK(!info->sections[0].executable);
}

TEST(pe_rejects_non_pe_and_truncated) {
    CHECK(!parse_bytes("this is not a PE file at all"));
    CHECK(!parse_bytes("MZ" + std::string(10, '\0')));  // 头被截断
    PeSpec spec;
    spec.exports = {"x"};
    std::string good = build_pe(spec);
    CHECK(!parse_bytes(good.substr(0, 0x100)));  // 连 PE 头都不全
    // 32 位机器号
    PeSpec s32;
    s32.machine = 0x14c;
    const auto i = parse_bytes(build_pe(s32));
    CHECK(i.has_value());
    if (i) CHECK_EQ(i->machine, std::string("i386"));
}

TEST(pe_no_import_table_is_parsed_but_empty) {
    PeSpec spec;
    spec.exports = {"x"};
    spec.no_imports = true;
    const auto info = parse_bytes(build_pe(spec));
    CHECK(info.has_value());
    if (info) CHECK(info->imports.empty());
}

// ---- mol::impact：注入地点分类 ------------------------------------------------

TEST(impact_classifies_injection_points) {
    const std::string root = make_instance();
    Tmp t;
    const std::string mod = (fs::path(root) / "mods/M").string();
    put(mod + "/SKSE/Plugins/EngineFixes.dll", build_pe(PeSpec{}));
    put(mod + "/version.dll", build_pe(PeSpec{}));
    put(mod + "/Data/meshes/a.nif", "x");
    put(mod + "/Data/Scripts/s.pex", "x");
    put(mod + "/tool.exe", build_pe(PeSpec{}));
    put(mod + "/mod.esp", "x");
    put(mod + "/mod.ini", "x");
    enable_in_modlist(root, "M");

    Instance inst = load_instance(root);
    const auto m = impact::analyze_mod(inst, "M");
    auto find = [&](std::string_view path) -> const impact::InjectionPoint* {
        for (const auto& i : m.injections)
            if (text(i.path) == path) return &i;
        return nullptr;
    };
    const auto* skse = find("SKSE/Plugins/EngineFixes.dll");
    CHECK(skse && text(skse->kind) == "skse_plugin" && text(skse->reach) == "game-process");
    const auto* proxy = find("version.dll");
    CHECK(proxy && text(proxy->kind) == "proxy_dll" && text(proxy->reach) == "all-processes" &&
          text(proxy->loaded_by) == "windows_loader");
    const auto* tool = find("tool.exe");
    CHECK(tool && text(tool->kind) == "exe_tool" && text(tool->reach) == "offline");
    const auto* pex = find("Data/Scripts/s.pex");
    CHECK(pex && text(pex->kind) == "papyrus" && text(pex->reach) == "game-logic");
    const auto* esp = find("mod.esp");
    CHECK(esp && text(esp->kind) == "content");
    const auto* ini = find("mod.ini");
    CHECK(ini && text(ini->kind) == "config");
    // 纯网格不进注入点清单
    CHECK(find("Data/meshes/a.nif") == nullptr);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// 路径会骗人，导出表不会：躺在 mod 根、但导出 SKSEPluginLoad 的 DLL 仍是 SKSE 插件
TEST(impact_exports_beat_path_names) {
    const std::string root = make_instance();
    Tmp t;
    const std::string mod = (fs::path(root) / "mods/Liar").string();
    PeSpec spec;
    spec.exports = {"SKSEPluginLoad", "SKSEPluginQuery"};
    put(mod + "/innocent-looking.dll", build_pe(spec));
    enable_in_modlist(root, "Liar");
    Instance inst = load_instance(root);
    const auto m = impact::analyze_mod(inst, "Liar");
    CHECK_EQ(m.injections.size(), std::size_t{1});
    if (!m.injections.empty()) {
        CHECK_EQ(text(m.injections[0].kind), std::string("skse_plugin"));
        CHECK_EQ(text(m.injections[0].path), std::string("innocent-looking.dll"));
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- mol::impact：能力面 ------------------------------------------------------

TEST(impact_maps_imports_to_capabilities) {
    const std::string root = make_instance();
    Tmp t;
    const std::string mod = (fs::path(root) / "mods/Caps").string();
    PeSpec spec;
    spec.imports = {{"kernel32.dll", {"WriteFile", "CreateProcessW", "VirtualProtect", "LoadLibraryW"}},
                    {"advapi32.dll", {"RegSetValueExW"}},
                    {"winhttp.dll", {"WinHttpOpen"}}};
    put(mod + "/plugin.dll", build_pe(spec));
    enable_in_modlist(root, "Caps");
    Instance inst = load_instance(root);
    const auto m = impact::analyze_mod(inst, "Caps");
    CHECK(m.caps.writes_files);
    CHECK(m.caps.spawns_processes);
    CHECK(m.caps.memory_patch);
    CHECK(m.caps.chain_loads);
    CHECK(m.caps.registry);
    CHECK(m.caps.network);
    CHECK(!m.caps.unknown);
    CHECK(!m.caps.evidence.empty());
    CHECK(text(m.summary).find("patch 内存") != std::string::npos);
    CHECK(text(m.summary).find("写文件") != std::string::npos);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// 可写+可执行节 / 没有导入表 → 疑似加壳，能力不可静态判断
TEST(impact_flags_packed_dlls) {
    const std::string root = make_instance();
    Tmp t;
    const std::string mod = (fs::path(root) / "mods/Packed").string();
    PeSpec spec;
    spec.writable = true;
    spec.executable = true;  // W+X 节：加壳/自修改的典型特征
    put(mod + "/packed.dll", build_pe(spec));
    enable_in_modlist(root, "Packed");
    Instance inst = load_instance(root);
    const auto m = impact::analyze_mod(inst, "Packed");
    CHECK(m.packed_suspect);
    CHECK(m.caps.unknown);
    CHECK(text(m.summary).find("加壳") != std::string::npos);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// 解析不了的 DLL 仍然报注入地点，能力标 unknown（不静默跳过）
TEST(impact_unparsable_dll_degrades) {
    const std::string root = make_instance();
    Tmp t;
    const std::string mod = (fs::path(root) / "mods/Broken").string();
    put(mod + "/SKSE/Plugins/broken.dll", "MZ but the rest is garbage");
    enable_in_modlist(root, "Broken");
    Instance inst = load_instance(root);
    const auto m = impact::analyze_mod(inst, "Broken");
    CHECK_EQ(m.injections.size(), std::size_t{1});
    if (!m.injections.empty()) CHECK_EQ(text(m.injections[0].kind), std::string("skse_plugin"));
    CHECK(m.caps.unknown);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// 空 mod / 不存在的 mod
TEST(impact_empty_and_missing_mods) {
    const std::string root = make_instance();
    Tmp t;
    put((fs::path(root) / "mods/Empty/textures/t.dds").string(), "x");
    enable_in_modlist(root, "Empty");
    Instance inst = load_instance(root);
    const auto m = impact::analyze_mod(inst, "Empty");
    CHECK(m.injections.empty());
    CHECK_EQ(text(m.summary), std::string("没有可分析的注入点"));
    bool threw = false;
    try {
        (void)impact::analyze_mod(inst, "No Such Mod");
    } catch (const Error& e) {
        threw = e.code == "mod_not_found";
    }
    CHECK(threw);
    std::error_code ec;
    fs::remove_all(root, ec);
}
