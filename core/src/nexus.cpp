#include "mol/nexus.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
}

string NexusClient::get_json(std::string_view pq, mr* mem) const {
    const std::vector<std::pair<std::string, std::string>> hdrs = {
        {"apikey", std::string(key_)},
        {"Application-Name", "mo-linux"},
        {"Application-Version", std::string(version_)},
        {"Accept", "application/json"},
    };
    const std::string url = std::string(base_) + std::string(pq);
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

}  // namespace mol
