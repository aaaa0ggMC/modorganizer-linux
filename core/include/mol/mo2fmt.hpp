#pragma once
// MO2 磁盘格式的解析/写出（纯 C++，不依赖 Qt）。
#include <map>
#include <span>
#include <optional>
#include <string_view>

#include "mol/pmr.hpp"

namespace mol {

// ---- profiles/<name>/modlist.txt --------------------------------------
// 文件中第一条是**最高**优先级；本接口统一返回 低→高 优先级顺序（即文件顺序反转）。
// 行格式：'+Name' 启用，'-Name' 禁用，'*Name' 未管理（foreign，跳过不返回），
//        '#' 开头为注释，空行忽略。名称以 "_separator" 结尾的是分隔符。
struct ModEntry {
    using allocator_type = mol::allocator_type;
    string name;
    bool enabled = false;
    bool separator = false;

    explicit ModEntry(allocator_type a = {}) : name(a) {}
    ModEntry(const ModEntry& o, allocator_type a) : name(o.name, a), enabled(o.enabled), separator(o.separator) {}
    ModEntry(ModEntry&& o, allocator_type a) : name(std::move(o.name), a), enabled(o.enabled), separator(o.separator) {}
    ModEntry(const ModEntry&) = default;
    ModEntry(ModEntry&&) = default;
    ModEntry& operator=(const ModEntry&) = default;
    ModEntry& operator=(ModEntry&&) = default;
};
vector<ModEntry> read_modlist(std::string_view file, mr* mem = default_mr());  // 文件不存在 → 空
void write_modlist(std::string_view file, std::span<const ModEntry> low_to_high);

// ---- plugins.txt (SSE 风格) / loadorder.txt ----------------------------
// plugins.txt：'*Name.esp' 启用，无 '*' 为禁用，'#' 注释；顺序即加载顺序。
// loadorder.txt：每行一个插件名，'#' 注释。
struct PluginEntry {
    using allocator_type = mol::allocator_type;
    string name;
    bool enabled = false;

    explicit PluginEntry(allocator_type a = {}) : name(a) {}
    PluginEntry(const PluginEntry& o, allocator_type a) : name(o.name, a), enabled(o.enabled) {}
    PluginEntry(PluginEntry&& o, allocator_type a) : name(std::move(o.name), a), enabled(o.enabled) {}
    PluginEntry(const PluginEntry&) = default;
    PluginEntry(PluginEntry&&) = default;
    PluginEntry& operator=(const PluginEntry&) = default;
    PluginEntry& operator=(PluginEntry&&) = default;
};
vector<PluginEntry> read_plugins_txt(std::string_view file, mr* mem = default_mr());
void write_plugins_txt(std::string_view file, std::span<const PluginEntry> v);
vector<string> read_loadorder_txt(std::string_view file, mr* mem = default_mr());

// ---- ini（QSettings 风格子集）------------------------------------------
// 支持 [Section]、key=value、';'/'#' 注释、@ByteArray(...) 包装的值（读出时去壳）、
// 值内反斜杠保持原样（不做转义折叠）。重复 key 后者覆盖。区段名/键名按 casefold 比较。
class Ini {
public:
    using allocator_type = mol::allocator_type;
    explicit Ini(allocator_type a = {}) : data_(a) {}
    static Ini load(std::string_view file, mr* mem = default_mr());  // 不存在 → 空 Ini
    static Ini parse(std::string_view text, mr* mem = default_mr());
    std::optional<string> get(std::string_view section, std::string_view key, mr* mem = default_mr()) const;

private:
    using Section = std::pmr::map<string, string>;
    std::pmr::map<string, Section> data_;  // key 已 casefold
};

// ---- Wine 路径 -----------------------------------------------------------
// "Z:\\home\\u\\x" → "/home/u/x"；"C:\\Users\\u" → <prefix>/drive_c/Users/u；
// 其它盘符 "D:\\.." → <prefix>/dosdevices/d:/..；已是 Unix 绝对路径则原样返回。
// 同时接受单反斜杠与双反斜杠（QSettings 会写 "\\\\"）。
string wine_to_unix(std::string_view winpath, std::string_view prefix, mr* mem = default_mr());

}  // namespace mol
