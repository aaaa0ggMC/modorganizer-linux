#pragma once
// FOMOD 安装器（ModuleConfig.xml）：解析、按选择求值、把选中的文件装配成 mod 目录。
// 语义以上游 MO2 的 installer_fomod 为准：
//   * 步骤的 <visible> 与插件类型的依赖模式，用「此前所有可见步骤里被选中插件设置的条件标志」求值，同名标志后设置者优先；
//     标志从未设置时，仅当期望值为空才算满足；
//   * 步骤/插件默认按名称升序排列（order="Explicit" 保持文件顺序，"Descending" 降序）；
//   * 安装顺序：requiredInstallFiles → 各可见步骤选中插件的文件 → 满足条件的 conditionalFileInstalls；
//     然后按 priority 稳定升序排序，后装的覆盖先装的；
//   * 与 MO2 一样忽略 alwaysInstall / installIfUsable。
// 内部结构用 pmr 字符串/向量，但未做分配器传播（体量很小、瞬时使用）。
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "mol/error.hpp"
#include "mol/pmr.hpp"
#include "mol/xml.hpp"

namespace mol::fomod {

enum class GroupType { AtLeastOne, AtMostOne, ExactlyOne, Any, All };
enum class PluginType { Required, Optional, Recommended, NotUsable, CouldBeUsable };

const char* to_string(GroupType t);
const char* to_string(PluginType t);

struct Cond {
    enum class Kind { Flag, File, Version, Composite } kind = Kind::Composite;
    std::string name;     // Flag: 标志名；File: 文件名
    std::string value;    // Flag: 期望值；File: 期望状态 Missing|Inactive|Active；Version: 要求的版本
    bool op_and = true;   // Composite
    std::vector<Cond> children;
};

struct FileEntry {
    std::string source;       // 压缩包里的路径（可含反斜杠，大小写不敏感）
    std::string destination;  // 相对 mod 根；空 = mod 根（文件则用原文件名）；以 / 或 \ 结尾表示目录
    bool folder = false;
    int priority = 0;
};

struct Pattern {
    Cond cond;
    PluginType type = PluginType::Optional;
};

struct Plugin {
    std::string name, description, image;
    std::vector<FileEntry> files;
    std::vector<std::pair<std::string, std::string>> flags;
    PluginType default_type = PluginType::Optional;
    std::vector<Pattern> patterns;
};

struct Group {
    std::string name;
    GroupType type = GroupType::Any;
    std::vector<Plugin> plugins;
};

struct Step {
    std::string name;
    bool has_visible = false;
    Cond visible;
    std::vector<Group> groups;
};

struct CondInstall {
    Cond cond;
    std::vector<FileEntry> files;
};

struct Config {
    std::string module_name;
    std::vector<FileEntry> required;
    std::vector<Step> steps;
    std::vector<CondInstall> conditional;
};

// root = <config> 元素。结构不合法 → Error{invalid_argument}。
Config parse_config(const XmlNode& root);

// 求值环境。
struct Env {
    std::function<std::string(std::string_view file)> file_state;  // 返回 "Missing"|"Inactive"|"Active"；空函数 = 一律 Missing
    std::string game_version = "0.0.0.0";                          // gameDependency 比较用
    std::string script_extender_version = "not installed";
};

// 选择：步骤名 → 组名 → 被选插件名。
// 同名的步骤（不少 FOMOD 每一步都叫 "Installation"）、同一步里同名的组：第 k 次出现（k ≥ 2）的键是 "名字 [#k]"
// （occurrence_key）。步骤按「可见步骤里第几次出现」数，组按「该步骤里第几次出现」数。
using Choices = std::map<std::string, std::map<std::string, std::set<std::string>>>;
std::string occurrence_key(std::string_view name, int k);
// 名字的宽松比较键：去首尾空白、HTML 实体、大小写（出现次数也按它数）
std::string norm_name(std::string_view s);

struct PluginState {
    std::string name, description, image;
    PluginType type = PluginType::Optional;  // 求值后的类型
    bool selected = false;
};
struct GroupState {
    std::string name;
    std::string key;  // choices 里用的键（同名组的第 k 个是 "名字 [#k]"）
    GroupType type = GroupType::Any;
    bool explicit_choice = false;  // 来自调用方的 choices（否则是默认值）
    std::vector<PluginState> plugins;
};
struct StepState {
    std::string name;
    std::string key;  // choices 里用的键（可见的同名步骤的第 k 个是 "名字 [#k]"）
    bool visible = true;
    std::vector<GroupState> groups;
};
struct Resolved {
    std::vector<StepState> steps;
    std::vector<FileEntry> files;  // 已按 priority 稳定排序，可直接顺序安装
};

// use_defaults=false 时，可见的组若在 choices 里没有条目 → Error{invalid_argument}。
// 显式选择违反组约束（ExactlyOne 选了 0/2 个等）、选了 NotUsable、或指名不存在 → Error{invalid_argument}；
// Required 插件总是被选中（不要求出现在 choices 里）。
// lenient_notes 非空 = 宽松模式（集合清单里 Vortex 记录的选择，压缩包版本可能已变）：
//   指名不存在的插件 → 忽略；显式选择违反组约束 → 该组改用默认选择；可见组没有条目 → 用默认选择；
//   每次退让都往 lenient_notes 里记一句，不报错。
// 名字匹配：先精确，再忽略大小写/首尾空白/HTML 实体。
Resolved resolve(const Config& cfg, const Choices& choices, bool use_defaults, const Env& env,
                 std::vector<std::string>* lenient_notes = nullptr);

// 把 resolved.files 从 source_root 装配到 dest_root（dest_root 须存在）。缺失的源收集到 missing（不是错误）。
// 返回复制的文件数。越界路径（..）→ Error{invalid_argument}。
std::size_t install_files(const Resolved& r, std::string_view source_root, std::string_view dest_root,
                          std::vector<std::string>* missing = nullptr);

// 在 root 下大小写不敏感地找 fomod/ModuleConfig.xml；找到返回其路径，否则空串。
std::string find_module_config(std::string_view root);

// 读文件 → parse_xml → parse_config。
Config load_config(std::string_view module_config_path);

// 解析 choices JSON：{"steps":{"<step>":{"<group>":["<plugin>",...]}}}。格式不对 → Error{invalid_argument}。
Choices parse_choices_json(std::string_view json);
// 反向：把选择序列化为同样的 JSON（紧凑、键排序）。
std::string choices_to_json(const Choices& c);

}  // namespace mol::fomod
