#pragma once
// MO2 实例（instance）模型：把磁盘上的 ModOrganizer.ini / mo-linux.json / profiles / mods 变成可用的结构，
// 并把它编排成「链接农场」的期望树。无状态：每个函数都只读/写磁盘，不缓存。
#include <optional>
#include <span>
#include <string_view>

#include "mol/error.hpp"
#include "mol/linkfarm.hpp"
#include "mol/merge.hpp"
#include "mol/pmr.hpp"

namespace mol {

// <instance>/mo-linux.json 的内容（mo-linux 自己的配置；MO2 本身不认识它）。
// 缺失的字段按 ModOrganizer.ini / 约定目录推导，见 load_instance。
struct InstanceConfig {
    using allocator_type = mol::allocator_type;
    string game = string{};          // "skyrimse"
    string game_dir;                 // 游戏本体目录（Unix 路径）
    string prefix;                   // Wine/Proton 前缀（含 drive_c 的目录）
    string prefix_user;              // 前缀内用户名，默认 "steamuser"
    string profile;                  // 当前 profile，默认取 ModOrganizer.ini 的 selected_profile，再默认 "Default"
    string farm_dir;                 // 农场目录；相对路径相对实例根，默认 "farm"
    string runner_kind;              // "proton"（默认）| "wine"
    string proton_path;              // Proton 目录（含 proton 脚本）
    string steam_root;               // STEAM_COMPAT_CLIENT_INSTALL_PATH，默认 ~ 下的 .steam/steam

    explicit InstanceConfig(allocator_type a = {})
        : game(a), game_dir(a), prefix(a), prefix_user(a), profile(a), farm_dir(a),
          runner_kind(a), proton_path(a), steam_root(a) {}
    InstanceConfig(const InstanceConfig&) = default;
    InstanceConfig(InstanceConfig&&) = default;
    InstanceConfig& operator=(const InstanceConfig&) = default;
    InstanceConfig& operator=(InstanceConfig&&) = default;
    InstanceConfig(const InstanceConfig& o, allocator_type a)
        : game(o.game, a), game_dir(o.game_dir, a), prefix(o.prefix, a), prefix_user(o.prefix_user, a),
          profile(o.profile, a), farm_dir(o.farm_dir, a), runner_kind(o.runner_kind, a),
          proton_path(o.proton_path, a), steam_root(o.steam_root, a) {}
    InstanceConfig(InstanceConfig&& o, allocator_type a)
        : game(std::move(o.game), a), game_dir(std::move(o.game_dir), a), prefix(std::move(o.prefix), a),
          prefix_user(std::move(o.prefix_user), a), profile(std::move(o.profile), a),
          farm_dir(std::move(o.farm_dir), a), runner_kind(std::move(o.runner_kind), a),
          proton_path(std::move(o.proton_path), a), steam_root(std::move(o.steam_root), a) {}
};

struct Instance {
    using allocator_type = mol::allocator_type;
    string root;           // 实例根（绝对路径）
    string mods_dir;       // <root>/mods（或 ModOrganizer.ini 的 mod_directory）
    string profiles_dir;   // <root>/profiles
    string downloads_dir;
    string overwrite_dir;  // <root>/overwrite
    string farm_path;      // 农场绝对路径
    InstanceConfig cfg;    // 已填好默认值

    explicit Instance(allocator_type a = {})
        : root(a), mods_dir(a), profiles_dir(a), downloads_dir(a), overwrite_dir(a), farm_path(a), cfg(a) {}
    Instance(const Instance&) = default;
    Instance(Instance&&) = default;
    Instance& operator=(const Instance&) = default;
    Instance& operator=(Instance&&) = default;
    Instance(const Instance& o, allocator_type a)
        : root(o.root, a), mods_dir(o.mods_dir, a), profiles_dir(o.profiles_dir, a),
          downloads_dir(o.downloads_dir, a), overwrite_dir(o.overwrite_dir, a),
          farm_path(o.farm_path, a), cfg(o.cfg, a) {}
    Instance(Instance&& o, allocator_type a)
        : root(std::move(o.root), a), mods_dir(std::move(o.mods_dir), a),
          profiles_dir(std::move(o.profiles_dir), a), downloads_dir(std::move(o.downloads_dir), a),
          overwrite_dir(std::move(o.overwrite_dir), a), farm_path(std::move(o.farm_path), a), cfg(std::move(o.cfg), a) {}
};

// 读取实例。root 须存在且含 ModOrganizer.ini 或 mo-linux.json（至少其一），否则抛 Error{instance_not_found}。
// 推导规则（优先级从高到低）：mo-linux.json 字段 > ModOrganizer.ini（[General] gameName/gamePath/selected_profile，
// 路径经 wine_to_unix(prefix)；mod_directory/profiles_directory/download_directory/overwrite_directory 里的
// "%BASE_DIR%" 替换为 base_directory 或实例根）> 约定目录。
// profile_override 非空时覆盖 cfg.profile。
Instance load_instance(std::string_view root, std::string_view profile_override = {}, mr* mem = default_mr());

struct InitOptions {
    std::string_view root;
    std::string_view game = "skyrimse";
    std::string_view game_dir;
    std::string_view prefix;
    std::string_view prefix_user = "steamuser";
    std::string_view profile = "Default";
    std::string_view runner_kind = "proton";
    std::string_view proton_path;
    std::string_view steam_root;
};
// 幂等：创建 mods/ profiles/<profile>/ downloads/ overwrite/，写（或原子更新）mo-linux.json。
// 已有的 modlist.txt / plugins.txt / ModOrganizer.ini 绝不覆盖；mo-linux.json 中本次未指定的已有字段保留。
// 返回 true 表示磁盘有变化，false 表示已是期望状态（调用方据此输出 changed）。
bool init_instance(const InitOptions& opt);

// 某个 mod（modlist 条目）在磁盘上的状态。
struct ModInfo {
    using allocator_type = mol::allocator_type;
    string name;
    bool enabled = false;
    bool separator = false;
    bool exists = false;     // mods/<name> 目录是否存在（分隔符恒为 false）
    std::int64_t nexus_id = 0;  // mods/<name>/meta.ini 的 [General] modid（MO2 兼容；0 = 未知）
    bool root = false;       // mods/<name>/meta.ini 的 [General] mol_root=true：目录结构镜像游戏根（映射到农场根而非 Data/）
    std::size_t priority = 0;  // 0 = 最低优先级（低→高序号）
    string path;               // 绝对路径（分隔符为空）

    explicit ModInfo(allocator_type a = {}) : name(a), path(a) {}
    ModInfo(const ModInfo& o, allocator_type a)
        : name(o.name, a), enabled(o.enabled), separator(o.separator), exists(o.exists), nexus_id(o.nexus_id), root(o.root), priority(o.priority), path(o.path, a) {}
    ModInfo(ModInfo&& o, allocator_type a)
        : name(std::move(o.name), a), enabled(o.enabled), separator(o.separator), exists(o.exists), nexus_id(o.nexus_id), root(o.root), priority(o.priority), path(std::move(o.path), a) {}
    ModInfo(const ModInfo&) = default;
    ModInfo(ModInfo&&) = default;
    ModInfo& operator=(const ModInfo&) = default;
    ModInfo& operator=(ModInfo&&) = default;
};

// profile 为空 → inst.cfg.profile。modlist.txt 不存在 → 空。低→高优先级序。
vector<ModInfo> list_mods(const Instance& inst, std::string_view profile = {}, mr* mem = default_mr());

// 新增一个 mod 条目（放在最高优先级）。已存在同名（大小写不敏感）→ Error{invalid_argument}。返回 modlist 里的名字。
void add_mod(const Instance& inst, std::string_view name, bool enabled, std::string_view profile = {});
// 把 mod 目录标记为「根目录型」（写 meta.ini 的 mol_root=true，保留其它内容）。
void mark_mod_root(std::string_view mod_dir, bool root);
// 在 mods/<name>/meta.ini 的 [General] 里设置 key=value（保留其余内容；key 大小写不敏感匹配；原子写）。
void set_mod_meta(std::string_view mod_dir, std::string_view key, std::string_view value);

// 以下三个写 modlist.txt（原子）；返回 true 表示有变化，幂等。名字大小写不敏感匹配但以 modlist 中的原名为准。
// 找不到 → Error{mod_not_found}；profile 目录不存在 → Error{profile_not_found}。
bool set_mod_enabled(const Instance& inst, std::string_view name, bool enabled, std::string_view profile = {});
// to_priority：目标优先级序号（0=最低；越界则夹到边界）。分隔符也可移动。
bool move_mod(const Instance& inst, std::string_view name, std::size_t to_priority, std::string_view profile = {});

// 农场的期望内容：层 0 = 游戏本体（prefix ""），其后是 profile 中**启用且存在**的 mod（低→高，prefix "Data"，
// 分隔符跳过），最后一层是 overwrite（prefix "Data"，目录不存在则无该层）。
struct FarmModel {
    using allocator_type = mol::allocator_type;
    vector<string> layer_names;  // 与 layer 序号一一对应：层 0 为 "<game>"，overwrite 为 "<overwrite>"
    MergeResult merged;

    explicit FarmModel(allocator_type a = {}) : layer_names(a), merged(a) {}
    FarmModel(const FarmModel& o, allocator_type a) : layer_names(o.layer_names, a), merged(o.merged, a) {}
    FarmModel(FarmModel&& o, allocator_type a) : layer_names(std::move(o.layer_names), a), merged(std::move(o.merged), a) {}
    FarmModel(const FarmModel&) = default;
    FarmModel(FarmModel&&) = default;
    FarmModel& operator=(const FarmModel&) = default;
    FarmModel& operator=(FarmModel&&) = default;
};
// cfg.game_dir 为空或不存在 → Error{config_invalid}。
FarmModel build_farm_model(const Instance& inst, std::string_view profile = {}, mr* mem = default_mr());

// 便捷：build_farm_model + plan_farm(inst.farm_path)。plan_farm 抛出的 runtime_error 被翻译成
// Error{farm_not_owned / farm_conflict}（按其 message 判断：含 "not empty"→farm_not_owned，含 "refusing"→farm_conflict）。
Plan plan_instance(const Instance& inst, const FarmModel& model, mr* mem = default_mr());
// apply_farm 的薄封装，io 失败翻译为 Error{io_error}。
void apply_instance(const Instance& inst, const Plan& plan);

}  // namespace mol
