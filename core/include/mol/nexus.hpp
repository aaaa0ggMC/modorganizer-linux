#pragma once
// Nexus Mods REST API v1 客户端（https://api.nexusmods.com/v1）。
// 鉴权：个人 API key（请求头 apikey）。key 来源：环境变量 NEXUS_API_KEY > ~/.config/mo-linux/nexus.key（0600）。
// 基址可用环境变量 MOL_NEXUS_API 覆盖（测试用）。key 永远不进日志、不进 JSON 输出。
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
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

struct NexusCollectionRev {
    using allocator_type = mol::allocator_type;
    string name, slug;
    std::int64_t revision_number = 0;
    std::int64_t mod_count = 0;
    std::int64_t total_size = 0;
    string download_path;  // 相对 API 根的路径，经 collection_archive_url 换成可下载地址
    vector<string> game_versions;
    bool adult = false;
    // 集合页面上作者写的说明（Markdown；安装须知、降级要求、可选项……清单里的 installInstructions 常常只是「去主页看」）
    string summary, description;
    string changelog;  // 本修订的更新日志（Markdown）
    explicit NexusCollectionRev(allocator_type a = {}) : name(a), slug(a), download_path(a), game_versions(a), summary(a), description(a), changelog(a) {}
    NexusCollectionRev(const NexusCollectionRev& o, allocator_type a)
        : name(o.name, a), slug(o.slug, a), revision_number(o.revision_number), mod_count(o.mod_count), total_size(o.total_size), download_path(o.download_path, a),
          game_versions(o.game_versions, a), adult(o.adult), summary(o.summary, a), description(o.description, a), changelog(o.changelog, a) {}
    NexusCollectionRev(NexusCollectionRev&& o, allocator_type a)
        : name(std::move(o.name), a), slug(std::move(o.slug), a), revision_number(o.revision_number), mod_count(o.mod_count), total_size(o.total_size),
          download_path(std::move(o.download_path), a), game_versions(std::move(o.game_versions), a), adult(o.adult), summary(std::move(o.summary), a),
          description(std::move(o.description), a), changelog(std::move(o.changelog), a) {}
    NexusCollectionRev(const NexusCollectionRev&) = default;
    NexusCollectionRev(NexusCollectionRev&&) = default;
    NexusCollectionRev& operator=(const NexusCollectionRev&) = default;
    NexusCollectionRev& operator=(NexusCollectionRev&&) = default;
};

struct NexusModSummary {
    using allocator_type = mol::allocator_type;
    std::int64_t mod_id = 0;
    string name, author, summary, version, updated_at;
    std::int64_t endorsements = 0, downloads = 0;
    explicit NexusModSummary(allocator_type a = {}) : name(a), author(a), summary(a), version(a), updated_at(a) {}
    NexusModSummary(const NexusModSummary& o, allocator_type a) : mod_id(o.mod_id), name(o.name, a), author(o.author, a), summary(o.summary, a), version(o.version, a), updated_at(o.updated_at, a), endorsements(o.endorsements), downloads(o.downloads) {}
    NexusModSummary(NexusModSummary&& o, allocator_type a) : mod_id(o.mod_id), name(std::move(o.name), a), author(std::move(o.author), a), summary(std::move(o.summary), a), version(std::move(o.version), a), updated_at(std::move(o.updated_at), a), endorsements(o.endorsements), downloads(o.downloads) {}
    NexusModSummary(const NexusModSummary&) = default;
    NexusModSummary(NexusModSummary&&) = default;
    NexusModSummary& operator=(const NexusModSummary&) = default;
    NexusModSummary& operator=(NexusModSummary&&) = default;
};

struct NexusRequirement {
    using allocator_type = mol::allocator_type;
    std::int64_t mod_id = 0;  // 站内 mod；外部需求为 0
    string name, url, notes;
    bool external = false;
    explicit NexusRequirement(allocator_type a = {}) : name(a), url(a), notes(a) {}
    NexusRequirement(const NexusRequirement& o, allocator_type a) : mod_id(o.mod_id), name(o.name, a), url(o.url, a), notes(o.notes, a), external(o.external) {}
    NexusRequirement(NexusRequirement&& o, allocator_type a) : mod_id(o.mod_id), name(std::move(o.name), a), url(std::move(o.url), a), notes(std::move(o.notes), a), external(o.external) {}
    NexusRequirement(const NexusRequirement&) = default;
    NexusRequirement(NexusRequirement&&) = default;
    NexusRequirement& operator=(const NexusRequirement&) = default;
    NexusRequirement& operator=(NexusRequirement&&) = default;
};

struct NexusModInfo {
    using allocator_type = mol::allocator_type;
    NexusModSummary summary;
    string category;
    vector<NexusRequirement> requirements;
    vector<string> dlc_requirements;
    explicit NexusModInfo(allocator_type a = {}) : summary(a), category(a), requirements(a), dlc_requirements(a) {}
    NexusModInfo(const NexusModInfo& o, allocator_type a) : summary(o.summary, a), category(o.category, a), requirements(o.requirements, a), dlc_requirements(o.dlc_requirements, a) {}
    NexusModInfo(NexusModInfo&& o, allocator_type a) : summary(std::move(o.summary), a), category(std::move(o.category), a), requirements(std::move(o.requirements), a), dlc_requirements(std::move(o.dlc_requirements), a) {}
    NexusModInfo(const NexusModInfo&) = default;
    NexusModInfo(NexusModInfo&&) = default;
    NexusModInfo& operator=(const NexusModInfo&) = default;
    NexusModInfo& operator=(NexusModInfo&&) = default;
};

struct NexusCollectionSummary {
    using allocator_type = mol::allocator_type;
    string slug, name, summary;
    std::int64_t endorsements = 0, downloads = 0, revision = 0, mod_count = 0, total_size = 0;
    explicit NexusCollectionSummary(allocator_type a = {}) : slug(a), name(a), summary(a) {}
    NexusCollectionSummary(const NexusCollectionSummary& o, allocator_type a) : slug(o.slug, a), name(o.name, a), summary(o.summary, a), endorsements(o.endorsements), downloads(o.downloads), revision(o.revision), mod_count(o.mod_count), total_size(o.total_size) {}
    NexusCollectionSummary(NexusCollectionSummary&& o, allocator_type a) : slug(std::move(o.slug), a), name(std::move(o.name), a), summary(std::move(o.summary), a), endorsements(o.endorsements), downloads(o.downloads), revision(o.revision), mod_count(o.mod_count), total_size(o.total_size) {}
    NexusCollectionSummary(const NexusCollectionSummary&) = default;
    NexusCollectionSummary(NexusCollectionSummary&&) = default;
    NexusCollectionSummary& operator=(const NexusCollectionSummary&) = default;
    NexusCollectionSummary& operator=(NexusCollectionSummary&&) = default;
};

struct NexusDownload {
    using allocator_type = mol::allocator_type;
    string path;
    std::uint64_t size = 0;
    bool reused = false;  // 下载目录里已有同名且大小一致的文件，没有重新下载
    explicit NexusDownload(allocator_type a = {}) : path(a) {}
    NexusDownload(const NexusDownload& o, allocator_type a) : path(o.path, a), size(o.size), reused(o.reused) {}
    NexusDownload(NexusDownload&& o, allocator_type a) : path(std::move(o.path), a), size(o.size), reused(o.reused) {}
    NexusDownload(const NexusDownload&) = default;
    NexusDownload(NexusDownload&&) = default;
    NexusDownload& operator=(const NexusDownload&) = default;
    NexusDownload& operator=(NexusDownload&&) = default;
};

class NexusClient {
public:
    // base 空 → $MOL_NEXUS_API → 官方地址。
    explicit NexusClient(std::string_view api_key, std::string_view app_version = "0.1", std::string_view base = {});

    NexusUser validate(mr* mem = default_mr()) const;
    vector<NexusFile> mod_files(std::string_view game_domain, std::int64_t mod_id, mr* mem = default_mr()) const;
    // 返回首选 CDN 的下载 URL。免费用户必须带 nxm 的 key/expires，否则 Error{nexus_premium}。
    string download_url(std::string_view game_domain, std::int64_t mod_id, std::int64_t file_id,
                        const NxmUrl* nxm = nullptr, mr* mem = default_mr()) const;
    // 集合（Collections）。revision==0 → 最新已发布版本。找不到 → Error{nexus_not_found}。
    NexusCollectionRev collection_revision(std::string_view game_domain, std::string_view slug, std::int64_t revision = 0, mr* mem = default_mr()) const;
    // 集合清单压缩包（含 collection.json）的下载地址（经 download_path 向 API 换取）。
    string collection_archive_url(std::string_view download_path, mr* mem = default_mr()) const;
    // 搜索 mod。sort ∈ relevance|endorsements|downloads|updatedAt（其它值 → invalid_argument）。count 夹到 [1,50]。
    // total 返回匹配总数。
    vector<NexusModSummary> search_mods(std::string_view game_domain, std::string_view text, std::string_view sort, int count, int offset,
                                        std::int64_t* total = nullptr, mr* mem = default_mr()) const;
    // mod 详情与需求（站内需求 + 外部需求 + DLC 需求）。不存在 → Error{nexus_not_found}。
    NexusModInfo mod_info(std::string_view game_domain, std::int64_t mod_id, mr* mem = default_mr()) const;
    // 批量取 mod 的当前版本（一次请求最多 50 个，内部自动分批）。查不到的 id 不出现在结果里。
    std::map<std::int64_t, NexusModSummary> mod_summaries(std::string_view game_domain, std::span<const std::int64_t> ids, mr* mem = default_mr()) const;
    // 搜索集合。sort ∈ endorsements|downloads|updatedAt|relevance。
    vector<NexusCollectionSummary> search_collections(std::string_view game_domain, std::string_view text, std::string_view sort, int count, int offset,
                                                      std::int64_t* total = nullptr, mr* mem = default_mr()) const;
    // 带 apikey 等头的 GET（供测试/调用方复用）。status≥400 → 映射为带 code 的 Error。
    string get_json(std::string_view path_and_query, mr* mem = default_mr()) const;

    // GraphQL v2：执行 query，返回响应体（已检查 HTTP 状态与 errors 字段）。
    string graphql(std::string_view query, mr* mem = default_mr()) const;

private:
    string get_root_json(std::string_view path_and_query, mr* mem) const;
    string get_at(std::string_view base, std::string_view path_and_query, mr* mem) const;
    string key_, version_, base_, root_;
};

// 下载 mod 文件到 downloads_dir：文件名取 Nexus 的 file_name；同名且大小等于 size_kb*1024 附近（±1KB）的已有文件直接复用。
// 同时写 MO2 兼容的 <文件>.meta。progress 同 http_download。
NexusDownload nexus_download(const NexusClient& client, std::string_view downloads_dir, std::string_view game_domain,
                             std::int64_t mod_id, std::int64_t file_id, const NxmUrl* nxm = nullptr,
                             const std::function<bool(std::uint64_t, std::uint64_t)>& progress = {}, mr* mem = default_mr());

}  // namespace mol
