#pragma once
// Nexus Mods REST API v1 客户端（https://api.nexusmods.com/v1）。
// 鉴权：个人 API key（请求头 apikey）。key 来源：环境变量 NEXUS_API_KEY > ~/.config/mo-linux/nexus.key（0600）。
// 基址可用环境变量 MOL_NEXUS_API 覆盖（测试用）。key 永远不进日志、不进 JSON 输出。
#include <cstdint>
#include <optional>
#include <string_view>

#include "mol/error.hpp"
#include "mol/pmr.hpp"

namespace mol {

struct NexusUser {
    using allocator_type = mol::allocator_type;
    string name;
    std::int64_t user_id = 0;
    bool is_premium = false;
    bool is_supporter = false;
    explicit NexusUser(allocator_type a = {}) : name(a) {}
    NexusUser(const NexusUser& o, allocator_type a) : name(o.name, a), user_id(o.user_id), is_premium(o.is_premium), is_supporter(o.is_supporter) {}
    NexusUser(NexusUser&& o, allocator_type a) : name(std::move(o.name), a), user_id(o.user_id), is_premium(o.is_premium), is_supporter(o.is_supporter) {}
    NexusUser(const NexusUser&) = default;
    NexusUser(NexusUser&&) = default;
    NexusUser& operator=(const NexusUser&) = default;
    NexusUser& operator=(NexusUser&&) = default;
};

struct NexusFile {
    using allocator_type = mol::allocator_type;
    std::int64_t file_id = 0;
    string name;           // 展示名
    string file_name;      // 磁盘文件名
    string version;
    string category;       // main | update | optional | old_version | miscellaneous …
    std::int64_t size_kb = 0;
    bool is_primary = false;
    explicit NexusFile(allocator_type a = {}) : name(a), file_name(a), version(a), category(a) {}
    NexusFile(const NexusFile& o, allocator_type a) : file_id(o.file_id), name(o.name, a), file_name(o.file_name, a), version(o.version, a), category(o.category, a), size_kb(o.size_kb), is_primary(o.is_primary) {}
    NexusFile(NexusFile&& o, allocator_type a) : file_id(o.file_id), name(std::move(o.name), a), file_name(std::move(o.file_name), a), version(std::move(o.version), a), category(std::move(o.category), a), size_kb(o.size_kb), is_primary(o.is_primary) {}
    NexusFile(const NexusFile&) = default;
    NexusFile(NexusFile&&) = default;
    NexusFile& operator=(const NexusFile&) = default;
    NexusFile& operator=(NexusFile&&) = default;
};

// nxm://<game>/mods/<mod>/files/<file>?key=K&expires=E&user_id=U
struct NxmUrl {
    using allocator_type = mol::allocator_type;
    string game;
    std::int64_t mod_id = 0;
    std::int64_t file_id = 0;
    string key;      // 免费用户下载必需
    string expires;
    explicit NxmUrl(allocator_type a = {}) : game(a), key(a), expires(a) {}
    NxmUrl(const NxmUrl& o, allocator_type a) : game(o.game, a), mod_id(o.mod_id), file_id(o.file_id), key(o.key, a), expires(o.expires, a) {}
    NxmUrl(NxmUrl&& o, allocator_type a) : game(std::move(o.game), a), mod_id(o.mod_id), file_id(o.file_id), key(std::move(o.key), a), expires(std::move(o.expires), a) {}
    NxmUrl(const NxmUrl&) = default;
    NxmUrl(NxmUrl&&) = default;
    NxmUrl& operator=(const NxmUrl&) = default;
    NxmUrl& operator=(NxmUrl&&) = default;
};
// 格式不对 → Error{invalid_argument}。
NxmUrl parse_nxm(std::string_view url, mr* mem = default_mr());

// mo-linux 的游戏 id → Nexus 的 domain（"skyrimse" → "skyrimspecialedition"）；未知原样返回。
string nexus_game_domain(std::string_view game_id, mr* mem = default_mr());

// ---- API key 存取 -------------------------------------------------------------
string nexus_key_path(mr* mem = default_mr());                 // ~/.config/mo-linux/nexus.key（或 $XDG_CONFIG_HOME）
std::optional<string> load_nexus_key(mr* mem = default_mr());  // 环境变量优先；都没有 → nullopt
void save_nexus_key(std::string_view key);                     // 0600，原子写
bool remove_nexus_key();                                       // 返回是否删除了文件

class NexusClient {
public:
    // base 空 → $MOL_NEXUS_API → 官方地址。
    explicit NexusClient(std::string_view api_key, std::string_view app_version = "0.1", std::string_view base = {});

    NexusUser validate(mr* mem = default_mr()) const;
    vector<NexusFile> mod_files(std::string_view game_domain, std::int64_t mod_id, mr* mem = default_mr()) const;
    // 返回首选 CDN 的下载 URL。免费用户必须带 nxm 的 key/expires，否则 Error{nexus_premium}。
    string download_url(std::string_view game_domain, std::int64_t mod_id, std::int64_t file_id,
                        const NxmUrl* nxm = nullptr, mr* mem = default_mr()) const;
    // 带 apikey 等头的 GET（供测试/调用方复用）。status≥400 → 映射为带 code 的 Error。
    string get_json(std::string_view path_and_query, mr* mem = default_mr()) const;

private:
    string key_, version_, base_;
};

}  // namespace mol
