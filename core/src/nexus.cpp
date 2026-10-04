#include "mol/nexus.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "mol/http.hpp"

import alib6;

namespace mol {
namespace fs = std::filesystem;
namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::string pct_decode(std::string_view s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
            o.push_back(static_cast<char>(hexv(s[i + 1]) * 16 + hexv(s[i + 2])));
            i += 2;
        } else {
            o.push_back(s[i]);
        }
    }
    return o;
}
std::string pct_encode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o.push_back(static_cast<char>(c));
        else { o.push_back('%'); o.push_back(hex[c >> 4]); o.push_back(hex[c & 15]); }
    }
    return o;
}

std::int64_t to_i64(std::string_view s, const char* what) {
    if (s.empty()) throw Error("invalid_argument", std::string("nxm: missing ") + what);
    std::int64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') throw Error("invalid_argument", std::string("nxm: bad ") + what);
        v = v * 10 + (c - '0');
    }
    return v;
}

[[noreturn]] void bad_json(std::string_view what) {
    throw Error("network_error", "unexpected response from Nexus: " + std::string(what));
}

std::string str_of(const alib6::AData& o, const char* k) {
    const auto& m = o.object();
    auto it = m.find(k);
    if (it == m.end()) return {};
    auto v = it.second().try_to<std::string_view>();
    return v ? std::string(*v) : std::string();
}
std::int64_t int_of(const alib6::AData& o, const char* k) {
    const auto& m = o.object();
    auto it = m.find(k);
    if (it == m.end()) return 0;
    auto v = it.second().try_to<long long>();
    return v ? *v : 0;
}
bool bool_of(const alib6::AData& o, const char* k) {
    const auto& m = o.object();
    auto it = m.find(k);
    if (it == m.end()) return false;
    auto v = it.second().try_to<bool>();
    return v ? *v : false;
}

fs::path config_dir() {
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return fs::path(x) / "mo-linux";
    const char* h = std::getenv("HOME");
    return fs::path(h ? h : "/") / ".config" / "mo-linux";
}

}  // namespace

NxmUrl parse_nxm(std::string_view url, mr* mem) {
    constexpr std::string_view scheme = "nxm://";
    if (url.substr(0, scheme.size()) != scheme) throw Error("invalid_argument", "not an nxm:// URL");
    std::string_view rest = url.substr(scheme.size());
    std::string_view query;
    if (auto q = rest.find('?'); q != std::string_view::npos) {
        query = rest.substr(q + 1);
        rest = rest.substr(0, q);
    }
    // game/mods/<id>/files/<id>
    std::vector<std::string_view> seg;
    for (std::size_t b = 0; b <= rest.size();) {
        std::size_t e = rest.find('/', b);
        if (e == std::string_view::npos) e = rest.size();
        seg.push_back(rest.substr(b, e - b));
        b = e + 1;
    }
    if (seg.size() != 5 || seg[1] != "mods" || seg[3] != "files" || seg[0].empty())
        throw Error("invalid_argument", "nxm URL must look like nxm://<game>/mods/<id>/files/<id>?key=..&expires=..");
    NxmUrl n(mem);
    n.game = string(seg[0], mem);
    n.mod_id = to_i64(seg[2], "mod id");
    n.file_id = to_i64(seg[4], "file id");
    for (std::size_t b = 0; b <= query.size() && !query.empty();) {
        std::size_t e = query.find('&', b);
        if (e == std::string_view::npos) e = query.size();
        const std::string_view kv = query.substr(b, e - b);
        if (auto eq = kv.find('='); eq != std::string_view::npos) {
            const std::string k(kv.substr(0, eq));
            const std::string v = pct_decode(kv.substr(eq + 1));
            if (k == "key") n.key = string(v, mem);
            else if (k == "expires") n.expires = string(v, mem);
        }
        b = e + 1;
    }
    return n;
}

string nexus_game_domain(std::string_view id, mr* mem) {
    if (id == "skyrimse") return string("skyrimspecialedition", mem);
    return string(id, mem);
}

string nexus_key_path(mr* mem) { return string((config_dir() / "nexus.key").string(), mem); }

std::optional<string> load_nexus_key(mr* mem) {
    if (const char* e = std::getenv("NEXUS_API_KEY"); e && *e) return string(trim(e), mem);
    std::ifstream in((config_dir() / "nexus.key"), std::ios::binary);
    if (!in) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string k = trim(text);
    if (k.empty()) return std::nullopt;
    return string(k, mem);
}

void save_nexus_key(std::string_view key) {
    const std::string k = trim(std::string(key));
    if (k.empty()) throw Error("invalid_argument", "empty API key");
    const fs::path dir = config_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw Error("io_error", "cannot create config directory: " + ec.message(), dir.string());
    const fs::path tmp = dir / "nexus.key.tmp";
    {
        // 先以 0600 创建再写入，避免出现过宽权限的窗口
        const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) throw Error("io_error", "cannot write key file", tmp.string());
        const std::string data = k + "\n";
        const ssize_t n = ::write(fd, data.data(), data.size());
        ::close(fd);
        if (n != static_cast<ssize_t>(data.size())) throw Error("io_error", "short write of key file", tmp.string());
    }
    ::chmod(tmp.c_str(), 0600);
    fs::rename(tmp, dir / "nexus.key", ec);
    if (ec) throw Error("io_error", "cannot install key file: " + ec.message(), (dir / "nexus.key").string());
}

bool remove_nexus_key() {
    std::error_code ec;
    return fs::remove(config_dir() / "nexus.key", ec);
}

NexusClient::NexusClient(std::string_view api_key, std::string_view app_version, std::string_view base)
    : key_(api_key), version_(app_version), base_(base) {
    if (key_.empty()) throw Error("nexus_auth", "no Nexus API key (set NEXUS_API_KEY or run `nexus login`)");
    if (base_.empty()) {
        const char* e = std::getenv("MOL_NEXUS_API");
        base_ = (e && *e) ? e : "https://api.nexusmods.com/v1";
    }
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
    root_ = base_;
    if (root_.size() >= 3 && root_.compare(root_.size() - 3, 3, "/v1") == 0) root_.erase(root_.size() - 3);
}

string NexusClient::get_root_json(std::string_view pq, mr* mem) const { return get_at(root_, pq, mem); }

string NexusClient::get_json(std::string_view pq, mr* mem) const { return get_at(base_, pq, mem); }

string NexusClient::get_at(std::string_view base, std::string_view pq, mr* mem) const {
    const std::vector<std::pair<std::string, std::string>> hdrs = {
        {"apikey", std::string(key_)},
        {"Application-Name", "mo-linux"},
        {"Application-Version", std::string(version_)},
        {"Accept", "application/json"},
    };
    const std::string url = std::string(base) + std::string(pq);
    // 报错信息里不带 query（可能含 nxm key），也绝不带 apikey
    const std::string safe_path = std::string(pq.substr(0, pq.find('?')));
    HttpResponse r = http_get(url, hdrs, 30, mem);
    if (r.status >= 200 && r.status < 300) return string(std::move(r.body), mem);
    switch (r.status) {
        case 401: throw Error("nexus_auth", "Nexus rejected the API key (401)", safe_path);
        case 403: throw Error("nexus_premium", "Nexus refused (403): downloading needs Premium, or an nxm:// link with key/expires", safe_path);
        case 404: throw Error("nexus_not_found", "not found on Nexus (404)", safe_path);
        case 429: throw Error("nexus_rate_limited", "Nexus rate limit reached (429)" + (r.retry_after.empty() ? std::string() : "; retry after " + std::string(r.retry_after) + "s"), safe_path);
        default: throw Error("network_error", "Nexus returned HTTP " + std::to_string(r.status), safe_path);
    }
}

namespace {
std::string json_escape(std::string_view in) {
    std::string o;
    for (unsigned char c : in) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(static_cast<char>(c)); }
        else if (c < 0x20) { static const char* hex = "0123456789abcdef"; o += "\\u00"; o.push_back(hex[c >> 4]); o.push_back(hex[c & 15]); }
        else o.push_back(static_cast<char>(c));
    }
    return o;
}
}  // namespace

string NexusClient::graphql(std::string_view query, mr* mem) const {
    const std::string body = "{\"query\":\"" + json_escape(query) + "\"}";
    const std::vector<std::pair<std::string, std::string>> hdrs = {
        {"apikey", std::string(key_)}, {"Application-Name", "mo-linux"}, {"Application-Version", std::string(version_)},
        {"Content-Type", "application/json"}, {"Accept", "application/json"}};
    HttpResponse r = http_post(root_ + "/v2/graphql", body, hdrs, 30, mem);
    if (r.status == 401) throw Error("nexus_auth", "Nexus rejected the API key (401)");
    if (r.status == 429) throw Error("nexus_rate_limited", "Nexus rate limit reached (429)" + (r.retry_after.empty() ? std::string() : "; retry after " + std::string(r.retry_after) + "s"));
    if (r.status < 200 || r.status >= 300) throw Error("network_error", "Nexus GraphQL returned HTTP " + std::to_string(r.status));
    {   // GraphQL 把错误放在 200 响应的 errors 字段里
        alib6::AData doc(mem);
        if (doc.load_from_memory(r.body) && doc.is_object()) {
            const auto& top = doc.object();
            if (auto e = top.find("errors"); e != top.end() && e.second().is_array() && !e.second().array().empty()) {
                std::string msg = "Nexus GraphQL error";
                const auto& first = e.second().array()[0];
                if (first.is_object()) if (auto m = str_of(first, "message"); !m.empty()) msg += ": " + m;
                const bool nf = msg.find("not found") != std::string::npos || msg.find("Not found") != std::string::npos;
                throw Error(nf ? "nexus_not_found" : "network_error", msg);
            }
        }
    }
    return string(std::move(r.body), mem);
}

NexusCollectionRev NexusClient::collection_revision(std::string_view domain, std::string_view slug, std::int64_t revision, mr* mem) const {
    for (char c : slug)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) throw Error("invalid_argument", "bad collection slug: " + std::string(slug));
    const std::string d = json_escape(domain), sl = json_escape(slug);
    const char* fields = "revisionNumber modCount totalSize downloadLink adultContent gameVersions{reference}";
    std::string query;
    if (revision <= 0)
        query = "{collection(slug:\"" + sl + "\",domainName:\"" + d + "\",viewAdultContent:true){name slug latestPublishedRevision{" + fields + "}}}";
    else
        query = "{collectionRevision(slug:\"" + sl + "\",revision:" + std::to_string(revision) + ",domainName:\"" + d +
                "\",viewAdultContent:true){" + fields + " collection{name slug}}}";
    const string gbody = graphql(query, mem);
    alib6::AData doc(mem);
    if (!doc.load_from_memory(gbody) || !doc.is_object()) bad_json("collection");
    const auto& top = doc.object();
    auto data = top.find("data");
    if (data == top.end() || !data.second().is_object()) bad_json("collection (no data)");
    const alib6::AData* rev = nullptr;
    std::string cname, cslug;
    if (revision <= 0) {
        auto c = data.second().object().find("collection");
        if (c == data.second().object().end() || !c.second().is_object()) throw Error("nexus_not_found", "collection not found: " + std::string(slug));
        cname = str_of(c.second(), "name");
        cslug = str_of(c.second(), "slug");
        auto lr = c.second().object().find("latestPublishedRevision");
        if (lr == c.second().object().end() || !lr.second().is_object()) throw Error("nexus_not_found", "collection has no published revision: " + std::string(slug));
        rev = &lr.second();
    } else {
        auto c = data.second().object().find("collectionRevision");
        if (c == data.second().object().end() || !c.second().is_object()) throw Error("nexus_not_found", "collection revision not found: " + std::string(slug) + " r" + std::to_string(revision));
        rev = &c.second();
        if (auto cc = rev->object().find("collection"); cc != rev->object().end() && cc.second().is_object()) {
            cname = str_of(cc.second(), "name");
            cslug = str_of(cc.second(), "slug");
        }
    }
    NexusCollectionRev out(mem);
    out.name = string(cname, mem);
    out.slug = string(cslug.empty() ? std::string(slug) : cslug, mem);
    out.revision_number = int_of(*rev, "revisionNumber");
    out.mod_count = int_of(*rev, "modCount");
    {   // totalSize 是字符串形式的整数
        const std::string ts = str_of(*rev, "totalSize");
        std::int64_t v = 0;
        for (char c : ts) { if (c < '0' || c > '9') { v = 0; break; } v = v * 10 + (c - '0'); }
        out.total_size = ts.empty() ? int_of(*rev, "totalSize") : v;
    }
    out.download_path = string(str_of(*rev, "downloadLink"), mem);
    out.adult = bool_of(*rev, "adultContent");
    if (auto gv = rev->object().find("gameVersions"); gv != rev->object().end() && gv.second().is_array())
        for (const auto& g : gv.second().array()) if (g.is_object()) out.game_versions.push_back(string(str_of(g, "reference"), mem));
    if (out.download_path.empty()) throw Error("nexus_not_found", "the collection revision has no download link");
    return out;
}

string NexusClient::collection_archive_url(std::string_view path, mr* mem) const {
    if (path.empty() || path.front() != '/') throw Error("invalid_argument", "bad collection download path");
    const string body = get_root_json(path, mem);
    alib6::AData doc(mem);
    if (!doc.load_from_memory(body) || !doc.is_object()) bad_json("collection download_link");
    auto it = doc.object().find("download_links");
    if (it == doc.object().end() || !it.second().is_array()) bad_json("collection download_link (links)");
    for (const auto& l : it.second().array())
        if (l.is_object()) if (auto u = str_of(l, "URI"); !u.empty()) return string(u, mem);
    bad_json("collection download_link (empty)");
}

namespace {
std::int64_t to_int(std::string_view s) {
    std::int64_t v = 0;
    for (char c : s) { if (c < '0' || c > '9') return 0; v = v * 10 + (c - '0'); }
    return v;
}
// GraphQL 里的 ID/大整数可能是字符串也可能是数字
std::int64_t num_of(const alib6::AData& o, const char* k) {
    if (!o.is_object()) return 0;
    auto it = o.object().find(k);
    if (it == o.object().end()) return 0;
    if (auto v = it.second().try_to<long long>()) return *v;
    if (auto sv = it.second().try_to<std::string_view>()) return to_int(*sv);
    return 0;
}
const char* sort_field(std::string_view s) {
    if (s.empty() || s == "relevance") return "relevance";
    if (s == "endorsements") return "endorsements";
    if (s == "downloads") return "downloads";
    if (s == "updatedAt" || s == "updated") return "updatedAt";
    throw Error("invalid_argument", "sort must be one of relevance|endorsements|downloads|updatedAt");
}
const alib6::AData* get_sub(const alib6::AData& o, const char* k) {
    if (!o.is_object()) return nullptr;
    auto it = o.object().find(k);
    return it == o.object().end() ? nullptr : &it.second();
}
NexusModSummary parse_mod_summary(const alib6::AData& n, mr* mem) {
    NexusModSummary m(mem);
    m.mod_id = num_of(n, "modId");
    m.name = string(str_of(n, "name"), mem);
    m.author = string(str_of(n, "author"), mem);
    m.summary = string(str_of(n, "summary"), mem);
    m.version = string(str_of(n, "version"), mem);
    m.updated_at = string(str_of(n, "updatedAt"), mem);
    m.endorsements = num_of(n, "endorsements");
    m.downloads = num_of(n, "downloads");
    return m;
}
}  // namespace

vector<NexusModSummary> NexusClient::search_mods(std::string_view domain, std::string_view text, std::string_view sort, int count, int offset, std::int64_t* total, mr* mem) const {
    count = std::clamp(count, 1, 50);
    offset = std::max(offset, 0);
    const char* sf = sort_field(sort);
    std::string filter = "gameDomainName:[{value:\"" + json_escape(domain) + "\"}]";
    if (!text.empty()) filter += ",nameStemmed:[{value:\"" + json_escape(text) + "\"}]";
    const std::string q = "{mods(filter:{" + filter + "},sort:[{" + sf + ":{direction:DESC}}],count:" + std::to_string(count) + ",offset:" + std::to_string(offset) +
                          "){totalCount nodes{modId name author endorsements downloads summary version updatedAt}}}";
    alib6::AData doc(mem);
    if (!doc.load_from_memory(graphql(q, mem)) || !doc.is_object()) bad_json("search");
    const auto* data = get_sub(doc, "data");
    const auto* mods = data ? get_sub(*data, "mods") : nullptr;
    vector<NexusModSummary> out(mem);
    if (!mods) return out;
    if (total) *total = num_of(*mods, "totalCount");
    if (const auto* nodes = get_sub(*mods, "nodes"); nodes && nodes->is_array())
        for (const auto& n : nodes->array()) out.push_back(parse_mod_summary(n, mem));
    return out;
}

NexusModInfo NexusClient::mod_info(std::string_view domain, std::int64_t mod_id, mr* mem) const {
    // gameId：先按域名查
    alib6::AData gdoc(mem);
    if (!gdoc.load_from_memory(graphql("{game(domainName:\"" + json_escape(domain) + "\"){id}}", mem))) bad_json("game");
    std::int64_t gid = 0;
    if (const auto* d = get_sub(gdoc, "data")) if (const auto* g = get_sub(*d, "game")) gid = num_of(*g, "id");
    if (gid == 0) throw Error("nexus_not_found", "unknown game domain: " + std::string(domain));
    const std::string q = "{mod(modId:\"" + std::to_string(mod_id) + "\",gameId:\"" + std::to_string(gid) +
                          "\"){modId name author endorsements downloads summary version updatedAt modCategory{name} modRequirements{nexusRequirements{nodes{modId modName url externalRequirement notes}} dlcRequirements{gameExpansion{name}}}}}";
    alib6::AData doc(mem);
    if (!doc.load_from_memory(graphql(q, mem)) || !doc.is_object()) bad_json("mod");
    const auto* data = get_sub(doc, "data");
    const auto* mod = data ? get_sub(*data, "mod") : nullptr;
    if (!mod || !mod->is_object()) throw Error("nexus_not_found", "mod " + std::to_string(mod_id) + " not found on Nexus (" + std::string(domain) + ")");
    NexusModInfo info(mem);
    info.summary = parse_mod_summary(*mod, mem);
    if (const auto* c = get_sub(*mod, "modCategory")) info.category = string(str_of(*c, "name"), mem);
    if (const auto* rq = get_sub(*mod, "modRequirements")) {
        if (const auto* nr = get_sub(*rq, "nexusRequirements"))
            if (const auto* nodes = get_sub(*nr, "nodes"); nodes && nodes->is_array())
                for (const auto& n : nodes->array()) {
                    NexusRequirement r(mem);
                    r.mod_id = num_of(n, "modId");
                    r.name = string(str_of(n, "modName"), mem);
                    r.url = string(str_of(n, "url"), mem);
                    r.notes = string(str_of(n, "notes"), mem);
                    r.external = bool_of(n, "externalRequirement");
                    info.requirements.push_back(std::move(r));
                }
        if (const auto* dl = get_sub(*rq, "dlcRequirements"); dl && dl->is_array())
            for (const auto& d : dl->array())
                if (const auto* ge = get_sub(d, "gameExpansion")) info.dlc_requirements.push_back(string(str_of(*ge, "name"), mem));
    }
    return info;
}

vector<NexusCollectionSummary> NexusClient::search_collections(std::string_view domain, std::string_view text, std::string_view sort, int count, int offset, std::int64_t* total, mr* mem) const {
    count = std::clamp(count, 1, 50);
    offset = std::max(offset, 0);
    const char* sf = sort_field(sort);
    std::string filter = "gameDomain:[{value:\"" + json_escape(domain) + "\"}]";
    if (!text.empty()) filter += ",name:[{value:\"" + json_escape(text) + "\",op:WILDCARD}]";
    const std::string q = "{collectionsV2(filter:{" + filter + "},sort:{" + sf + ":{direction:DESC}},count:" + std::to_string(count) + ",offset:" + std::to_string(offset) +
                          "){totalCount nodes{slug name summary endorsements totalDownloads latestPublishedRevision{revisionNumber modCount totalSize}}}}";
    alib6::AData doc(mem);
    if (!doc.load_from_memory(graphql(q, mem)) || !doc.is_object()) bad_json("collection search");
    const auto* data = get_sub(doc, "data");
    const auto* cs = data ? get_sub(*data, "collectionsV2") : nullptr;
    vector<NexusCollectionSummary> out(mem);
    if (!cs) return out;
    if (total) *total = num_of(*cs, "totalCount");
    if (const auto* nodes = get_sub(*cs, "nodes"); nodes && nodes->is_array())
        for (const auto& n : nodes->array()) {
            NexusCollectionSummary c(mem);
            c.slug = string(str_of(n, "slug"), mem);
            c.name = string(str_of(n, "name"), mem);
            c.summary = string(str_of(n, "summary"), mem);
            c.endorsements = num_of(n, "endorsements");
            c.downloads = num_of(n, "totalDownloads");
            if (const auto* r = get_sub(n, "latestPublishedRevision")) {
                c.revision = num_of(*r, "revisionNumber");
                c.mod_count = num_of(*r, "modCount");
                c.total_size = num_of(*r, "totalSize");
            }
            out.push_back(std::move(c));
        }
    return out;
}

NexusUser NexusClient::validate(mr* mem) const {
    const string body = get_json("/users/validate.json", mem);
    alib6::AData doc(mem);
    if (!doc.load_from_memory(body) || !doc.is_object()) bad_json("validate");
    NexusUser u(mem);
    u.name = string(str_of(doc, "name"), mem);
    u.user_id = int_of(doc, "user_id");
    u.is_premium = bool_of(doc, "is_premium");
    u.is_supporter = bool_of(doc, "is_supporter");
    return u;
}

vector<NexusFile> NexusClient::mod_files(std::string_view game, std::int64_t mod_id, mr* mem) const {
    const string body = get_json("/games/" + pct_encode(game) + "/mods/" + std::to_string(mod_id) + "/files.json", mem);
    alib6::AData doc(mem);
    if (!doc.load_from_memory(body) || !doc.is_object()) bad_json("files");
    vector<NexusFile> out(mem);
    const auto& m = doc.object();
    auto it = m.find("files");
    if (it == m.end() || !it.second().is_array()) return out;
    for (const auto& f : it.second().array()) {
        if (!f.is_object()) continue;
        NexusFile nf(mem);
        nf.file_id = int_of(f, "file_id");
        nf.name = string(str_of(f, "name"), mem);
        nf.file_name = string(str_of(f, "file_name"), mem);
        nf.version = string(str_of(f, "version"), mem);
        nf.category = string(str_of(f, "category_name"), mem);
        nf.size_kb = int_of(f, "size_kb");
        nf.is_primary = bool_of(f, "is_primary");
        out.push_back(std::move(nf));
    }
    return out;
}

string NexusClient::download_url(std::string_view game, std::int64_t mod_id, std::int64_t file_id, const NxmUrl* nxm, mr* mem) const {
    std::string pq = "/games/" + pct_encode(game) + "/mods/" + std::to_string(mod_id) + "/files/" + std::to_string(file_id) + "/download_link.json";
    if (nxm && !nxm->key.empty()) {
        pq += "?key=" + pct_encode(nxm->key);
        if (!nxm->expires.empty()) pq += "&expires=" + pct_encode(nxm->expires);
    }
    const string body = get_json(pq, mem);
    alib6::AData doc(mem);
    if (!doc.load_from_memory(body) || !doc.is_array()) bad_json("download_link");
    for (const auto& l : doc.array()) {
        if (!l.is_object()) continue;
        const std::string uri = str_of(l, "URI");
        if (!uri.empty()) return string(uri, mem);  // 列表按 Nexus 的偏好排序，取第一个
    }
    bad_json("download_link (empty)");
}

NexusDownload nexus_download(const NexusClient& client, std::string_view downloads_dir, std::string_view domain,
                             std::int64_t mod, std::int64_t fid, const NxmUrl* nxm,
                             const std::function<bool(std::uint64_t, std::uint64_t)>& progress, mr* mem) {
    std::string name;
    std::int64_t size_kb = 0;
    try {
        for (const auto& f : client.mod_files(domain, mod, mem))
            if (f.file_id == fid) { name = std::string(f.file_name); size_kb = f.size_kb; break; }
    } catch (const Error&) {
    }
    for (char& c : name) if (c == '/' || c == '\\' || c == '\0') c = '_';
    while (!name.empty() && name.front() == '.') name.erase(name.begin());

    NexusDownload out(mem);
    if (!name.empty()) {
        const fs::path dest = fs::path(std::string(downloads_dir)) / name;
        std::error_code ec;
        if (fs::is_regular_file(dest, ec) && size_kb > 0) {
            const auto sz = static_cast<std::int64_t>(fs::file_size(dest, ec));
            if (!ec && sz > 0 && std::llabs(sz - size_kb * 1024) <= 1024 + size_kb * 10) {  // size_kb 是四舍五入的 KB
                out.path = string(dest.string(), mem);
                out.size = static_cast<std::uint64_t>(sz);
                out.reused = true;
                return out;
            }
        }
    }
    const string url = client.download_url(domain, mod, fid, nxm, mem);
    if (name.empty()) {
        std::string_view p = url;
        if (auto q = p.find_first_of("?#"); q != std::string_view::npos) p = p.substr(0, q);
        if (auto sl = p.rfind('/'); sl != std::string_view::npos) p = p.substr(sl + 1);
        name = pct_decode(p);
        for (char& c : name) if (c == '/' || c == '\\' || c == '\0') c = '_';
        while (!name.empty() && name.front() == '.') name.erase(name.begin());
        if (name.empty()) name = "nexus-" + std::to_string(mod) + "-" + std::to_string(fid) + ".bin";
    }
    const std::string dest = (fs::path(std::string(downloads_dir)) / name).string();
    out.size = http_download(url, dest, {}, progress);
    out.path = string(dest, mem);
    {
        std::ofstream meta(dest + ".meta", std::ios::binary | std::ios::trunc);
        meta << "[General]\ngameName=" << std::string(domain) << "\nmodID=" << mod << "\nfileID=" << fid << "\nrepository=Nexus\n";
    }
    return out;
}

}  // namespace mol
