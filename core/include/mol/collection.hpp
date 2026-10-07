#pragma once
// Nexus Collections（集合/整合包）：解析 collection.json、确定安装顺序、驱动「下载 → 校验 → 安装 → 排序 → 插件」流水线。
//
// 清单格式取自 Vortex 的 ICollection（extensions/collections/types/ICollection.ts）：
//   info{name,author,domainName,gameVersions,installInstructions}、mods[{name,version,optional,phase,source{type,modId,fileId,md5,
//   fileSize,logicalFilename,url,instructions,tag},choices{type:"fomod",options:[{name,groups:[{name,choices:[{name,idx}]}]}]},patches}]、
//   modRules[{type before|after|requires|conflicts|recommends, source, reference}]、plugins[{name,enabled}]、collectionConfig。
//
// 交互设计（mo-linux 不能在中途提问）：
//   * 安装是**幂等、可续跑**的：状态写在 <实例>/collections/<slug>/state.json，每个 mod 处理完立刻落盘；
//   * 遇到需要人介入的地方不阻塞、不猜测，而是把该 mod 记为 pending 并继续处理其它 mod，最后返回 status=incomplete + pending 列表；
//     pending 的 kind：
//       manual_download   免费账号/浏览器下载/手动来源：给出页面 URL 与说明；用户下载好后用 `collection resolve --archive/--nxm` 补上
//       fomod_choices     压缩包有 FOMOD 但清单没有给出选择：用 `collection resolve --fomod FILE` 或 `--fomod-defaults` 补上
//       unsupported       暂不支持的来源（bundle/带 patches 的 mod）：用 `resolve --skip` 跳过或自行提供压缩包
//   * 用户用 resolve 记下决定（也存进 state.json），再次 `collection install` 即从中断处继续，已装好的 mod 不会重做。
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "mol/fomod.hpp"
#include "mol/instance.hpp"
#include "mol/nexus.hpp"

namespace mol::collection {

struct Source {
    std::string type;  // nexus | direct | browse | manual | bundle
    std::int64_t mod_id = 0, file_id = 0, file_size = 0;
    std::string md5, logical_filename, url, instructions, tag;
};

struct Mod {
    std::string name, version, domain;
    bool optional = false;
    int phase = 0;
    Source source;
    bool has_choices = false;
    fomod::Choices choices;   // 来自清单的 FOMOD 选择
    bool has_patches = false; // 清单带二进制补丁（暂不支持）
    // 清单的 `hashes`（Vortex 的「复刻」安装）：策展人装出来的每个文件 (相对 mod 根的路径, md5)。
    // 有它时不跑 FOMOD，而是按 md5 从压缩包里挑出这些文件放到对应路径（FOMOD 选择缺失或对不上都不影响）。
    std::vector<std::pair<std::string, std::string>> hashes;
    std::string mod_type;  // details.type：Vortex 的 mod 类型；"enb"/"dinput" 部署到游戏根目录（不是 Data/）
    bool deploys_to_root() const { return mod_type == "enb" || mod_type == "dinput"; }
    std::string key() const { return source.tag.empty() ? name : source.tag; }  // 稳定标识（状态文件里的键）
};

struct RuleRef {
    std::string md5, logical_name, file_expression, version_match;
};
struct Rule {
    std::string type;  // before | after | requires | conflicts | recommends | provides
    RuleRef source, reference;
};

struct PluginSpec {
    std::string name;
    bool enabled = true;
};

struct Info {
    std::string name, author, domain, install_instructions;
    std::vector<std::string> game_versions;
};

struct Collection {
    Info info;
    std::vector<Mod> mods;
    std::vector<Rule> rules;
    std::vector<PluginSpec> plugins;
    bool has_plugins = false;
};

// 解析 collection.json 文本；结构不对 → Error{invalid_argument}。
Collection parse_collection(std::string_view json);
// {type:"fomod",options:[…]} → Choices；不是 fomod 类型返回空。
fomod::Choices choices_from_vortex(std::string_view json_object);

// 安装顺序（下标）：先按 phase 升序（稳定），再让 `after` 规则里的 source 排到 reference 之后、`before` 规则反之。
// 规则成环时忽略造成环的那条（保持原序）。后面的 mod 优先级更高。
std::vector<std::size_t> install_order(const Collection& c);

// ---- 状态 ----------------------------------------------------------------------------
struct ModState {
    std::string name;     // 便于不读清单也能显示
    std::string status;   // pending | installed | skipped | failed
    std::string archive;  // 实际使用的压缩包路径
    std::string mod_dir;  // mods/ 下的目录名
    std::string note;     // failed 时的原因
    std::string kind;     // pending 时：manual_download | fomod_choices | unsupported | skse（GUI 据此给出对应操作）
    std::string url;      // 该 mod 的页面（Nexus 页面或清单给的外部链接）
};
struct Override {
    bool skip = false;
    bool fomod_defaults = false;
    bool has_choices = false;
    fomod::Choices choices;
    std::string archive;  // 用户自己提供的压缩包
    bool reinstall = false;  // 已装好的也重装一次（原地替换；装完清掉）
};
struct State {
    std::string slug, name;
    std::int64_t revision = 0;
    std::map<std::string, ModState> mods;      // 键 = Mod::key()
    std::map<std::string, Override> overrides;
};

std::string collection_dir(const Instance& inst, std::string_view slug);
// 由工具生成、不由任何 mod 提供的插件（FNIS.esp → 行为引擎、DynDOLOD.esp、Synthesis.esp……）：该跑的工具；不认识返回空。
std::string tool_for_generated_plugin(std::string_view plugin);  // <实例>/collections/<slug>
State load_state(const Instance& inst, std::string_view slug);            // 不存在 → 空 State
void save_state(const Instance& inst, const State& s);

// ---- 流水线 ---------------------------------------------------------------------------
struct Pending {
    std::string key, name, kind, detail, url;
};
struct ModOutcome {
    std::string key, name, status, mod_dir, note;
};
struct Report {
    std::vector<ModOutcome> mods;
    std::vector<Pending> pending;
    std::size_t installed = 0, skipped = 0, failed = 0;
    bool complete() const { return pending.empty() && failed == 0; }
    std::size_t plugins_applied = 0;
    std::vector<std::string> notes;  // 非致命提示（规则、游戏版本不一致……）
    // 复用：直接用了实例里已有的目录 / 从别的实例 reflink 过来的目录 / 安装后与已有安装去重的文件与字节
    std::size_t reused = 0, reflinked = 0, deduped_files = 0;
    std::uint64_t deduped_bytes = 0;
};

struct InstallOptions {
    std::string profile;             // 空 → 实例当前 profile
    bool include_optional = true;
    bool fomod_defaults = false;     // 清单没给选择的 FOMOD 一律用默认
    unsigned jobs = 0;               // 并行下载数（0 = MOL_JOBS 环境变量或默认 4）
    // 别的实例的 mods/ 目录：同一个 Nexus 文件、同样的 FOMOD 选择已经装在那里 → reflink 整个目录过来（btrfs/xfs 上瞬间、不占空间）；
    // 选择不同就照常安装，再与那边的同名文件去重。
    std::vector<std::string> reuse_from;
    // 进度回调：stage ∈ "download"（所有并行下载的合计字节，mod 名为空）| "downloaded"（第几个下载完）| "install"（第几个 mod）。返回 false 中止（未实现取消时可忽略）。
    std::function<void(std::string_view stage, std::string_view mod, std::uint64_t done, std::uint64_t total)> progress;
};

// client 为空（例如纯离线续跑）时，需要联网的 mod 一律 pending。
// 返回的 State 已由本函数保存。
Report install_collection(const Instance& inst, const NexusClient* client, const Collection& c, State& state,
                          const InstallOptions& opt, std::string_view game_version = {});

// ---- 校验已装的 mod（P0-2）--------------------------------------------------------------
// 用**当前的**安装逻辑重新算每个已装 mod 应有的文件，与磁盘对比（不解压、不改任何东西）：
//   * 清单给了 FOMOD 选择的：读压缩包里的 FOMOD 配置 + 压缩包目录，按选择算出目标文件集合；
//   * 按 hashes 复刻的：清单列出的每个文件都应在。
// 只比较文件路径集合（大小写不敏感），不比内容。用来发现「旧版本按别的逻辑装的、指纹没变所以不会自动重装」的 mod。
struct VerifyItem {
    std::string key, name, mod_dir, kind;      // kind: fomod | replicate
    std::vector<std::string> missing, extra;   // 应有却没有 / 有却不应有（相对 mod 根）
};
struct VerifyReport {
    std::size_t checked = 0, skipped = 0;      // skipped：没有压缩包、读不了 FOMOD……
    std::vector<VerifyItem> mismatched;
    std::vector<std::string> missing_plugins;  // 清单插件列表里启用、磁盘上（任何 mod / 游戏 Data）都没有的
};
VerifyReport verify_collection(const Instance& inst, const Collection& c, const State& state, std::string_view profile = {},
                               const std::function<void(std::size_t done, std::size_t total, std::string_view name)>& progress = {});

// 应用清单的插件列表到 profile 的 plugins.txt/loadorder.txt（只处理磁盘上存在的插件）。返回处理的插件数。
// missing（可空）：清单里启用、磁盘上却没有的插件——某个 mod 没装全（FOMOD 条件、hashes 不完整……）。
std::size_t apply_plugin_spec(const Instance& inst, const Collection& c, std::string_view profile, std::span<const string> forced = {},
                              std::vector<std::string>* missing = nullptr);

}  // namespace mol::collection
