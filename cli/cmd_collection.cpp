// collection inspect/install/status/resolve —— Nexus Collections 导入（设计见 core/include/mol/collection.hpp）。
// 退出码：install 未完成（有 pending/failed）→ 4，ok 仍为 true。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <sys/statvfs.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <iterator>

#include "mol/casefold.hpp"
#include "mol/collection.hpp"
#include "mol/doctor.hpp"
#include "mol/http.hpp"
#include "mol/instance.hpp"
#include "mol/mod_install.hpp"
#include "mol/nexus.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {
namespace {
namespace fs = std::filesystem;
namespace col = mol::collection;

mol::NexusClient make_client() {
    const auto key = mol::load_nexus_key();
    if (!key) throw mol::Error("nexus_auth", "no Nexus API key (set NEXUS_API_KEY or run `nexus login`)");
    return mol::NexusClient(*key, "0.1");
}

struct Ref {
    std::string domain, slug, local;  // local 非空 = 本地 collection.json / 压缩包
};

// slug | https://www.nexusmods.com/games/<domain>/collections/<slug>[/…]
//      | https://next.nexusmods.com/<domain>/collections/<slug>[/…]（集合页面、README 里常见的写法）| 本地文件
Ref parse_ref(std::string_view s, std::string_view default_domain) {
    Ref r;
    std::error_code ec;
    if (fs::is_regular_file(std::string(s), ec)) {
        r.local = std::string(s);
        r.slug = fs::path(std::string(s)).stem().string();
        return r;
    }
    std::string x(s);
    if (auto q = x.find_first_of("?#"); q != std::string::npos) x.erase(q);
    if (x.rfind("http", 0) == 0) {
        const auto c = x.find("/collections/");
        const auto host_end = x.find('/', x.find("://") == std::string::npos ? 0 : x.find("://") + 3);
        if (c == std::string::npos || host_end == std::string::npos || c <= host_end) throw mol::Error("invalid_argument", "unrecognised collection URL: " + std::string(s));
        // 域名 = "/collections/" 前面那一段（"/games/<domain>" 或 "/<domain>"）
        const std::string before = x.substr(host_end, c - host_end);
        const auto slash = before.rfind('/');
        r.domain = slash == std::string::npos ? before : before.substr(slash + 1);
        if (r.domain == "games" || r.domain.empty()) throw mol::Error("invalid_argument", "unrecognised collection URL: " + std::string(s));
        std::string rest = x.substr(c + 13);
        if (auto sl = rest.find('/'); sl != std::string::npos) rest.erase(sl);
        r.slug = rest;
    } else {
        r.slug = x;
    }
    if (r.slug.empty()) throw mol::Error("invalid_argument", "empty collection slug");
    if (r.domain.empty()) r.domain = std::string(default_domain);
    return r;
}
Ref parse_ref(std::string_view s, const mol::Instance& inst) { return parse_ref(s, mol::nexus_game_domain(inst.cfg.game)); }

// 集合页面的说明（作者写的 README）。在线取到时同时缓存成 <集合目录>/readme-<rev>.md，离线也能读。
struct Readme {
    std::string summary, description, changelog, url;
};
std::string page_url_of(const Ref& ref) { return "https://next.nexusmods.com/" + ref.domain + "/collections/" + ref.slug; }

struct Loaded {
    col::Collection coll;
    Readme readme;
    std::string slug;
    std::int64_t revision = 0;
    std::int64_t total_size = 0;
    std::string dir;
};

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw mol::Error("io_error", "cannot read file", p.string());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 清单压缩包（.7z）→ <dir>/archive-<rev>/，返回 collection.json 文本
std::string unpack_manifest(const std::string& archive, const fs::path& into) {
    std::error_code ec;
    fs::remove_all(into, ec);
    fs::create_directories(into, ec);
    mol::extract_archive(archive, into.string());
    const fs::path j = into / "collection.json";
    if (!fs::exists(j, ec)) throw mol::Error("invalid_argument", "the collection archive has no collection.json", archive);
    return read_file(j);
}

std::string readme_markdown(std::string_view name, std::int64_t revision, const Readme& r) {
    std::string md = "# " + std::string(name) + "\n\n";
    if (!r.url.empty()) md += "<" + r.url + ">\n\n";
    if (!r.summary.empty()) md += "> " + r.summary + "\n\n";
    md += r.description.empty() ? std::string("(the collection page has no description)\n") : r.description + "\n";
    if (!r.changelog.empty()) md += "\n---\n\n## Changelog (revision " + std::to_string(revision) + ")\n\n" + r.changelog + "\n";
    return md;
}

void save_readme(const std::string& dir, std::int64_t revision, const Readme& r, std::string_view name) {
    if (dir.empty() || (r.description.empty() && r.summary.empty())) return;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path f = fs::path(dir) / ("readme-" + std::to_string(revision) + ".md");
    std::ofstream os(f, std::ios::binary | std::ios::trunc);
    if (os) os << readme_markdown(name, revision, r);  // 缓存失败不影响命令本身
}

// 没有实例时清单缓存在 ~/.cache/mo-linux/collections/<slug>（collection inspect 只想看看能不能装时用）
std::string user_cache_dir(std::string_view slug) {
    fs::path base;
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) base = fs::path(x) / "mo-linux";
    else base = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") / ".cache/mo-linux";
    return (base / "collections" / std::string(slug)).string();
}

Loaded load_remote(Context& ctx, const mol::Instance* inst, const Ref& ref, std::int64_t revision) {
    const auto dir_of = [&](std::string_view slug) { return inst ? col::collection_dir(*inst, slug) : user_cache_dir(slug); };
    Loaded l;
    l.slug = ref.slug;
    if (!ref.local.empty()) {
        const fs::path p(ref.local);
        l.dir = dir_of(ref.slug);
        const std::string ext = std::string(mol::casefold(p.extension().string()));
        const std::string text = ext == ".json" ? read_file(p) : unpack_manifest(ref.local, fs::path(l.dir) / "archive-local");
        l.coll = col::parse_collection(text);
        if (ext == ".json" && inst) {  // 和压缩包一样留一份在实例里：collection status / next（目标游戏版本）离线可读
            std::error_code ec;
            const fs::path keep = fs::path(l.dir) / "archive-local";
            fs::create_directories(keep, ec);
            std::ofstream(keep / "collection.json", std::ios::binary | std::ios::trunc) << text;
        }
        return l;
    }
    const mol::NexusClient client = make_client();
    const auto rev = client.collection_revision(ref.domain, ref.slug, revision, ctx.mem);
    l.revision = rev.revision_number;
    l.total_size = rev.total_size;
    l.dir = dir_of(ref.slug);
    const fs::path unpacked = fs::path(l.dir) / ("archive-" + std::to_string(rev.revision_number));
    const fs::path cached_json = unpacked / "collection.json";
    std::error_code ec;
    if (fs::exists(cached_json, ec)) {
        l.coll = col::parse_collection(read_file(cached_json));
    } else {
        const std::string url(client.collection_archive_url(rev.download_path, ctx.mem));
        fs::create_directories(l.dir, ec);
        const std::string dest = (fs::path(l.dir) / ("revision-" + std::to_string(rev.revision_number) + ".7z")).string();
        mol::http_download(url, dest);
        l.coll = col::parse_collection(unpack_manifest(dest, unpacked));
    }
    if (l.coll.info.name.empty()) l.coll.info.name = std::string(rev.name);
    l.readme = Readme{std::string(rev.summary), std::string(rev.description), std::string(rev.changelog), page_url_of(ref)};
    save_readme(l.dir, rev.revision_number, l.readme, rev.name);
    return l;
}

// 离线：读 state.json 里记录的 revision 对应的清单
Loaded load_cached(const mol::Instance& inst, const std::string& slug) {
    Loaded l;
    l.slug = slug;
    l.dir = col::collection_dir(inst, slug);
    const auto st = col::load_state(inst, slug);
    l.revision = st.revision;
    std::error_code ec;
    fs::path j = fs::path(l.dir) / ("archive-" + std::to_string(st.revision)) / "collection.json";
    if (!fs::exists(j, ec)) j = fs::path(l.dir) / "archive-local" / "collection.json";
    if (!fs::exists(j, ec)) throw mol::Error("config_invalid", "no cached collection manifest for '" + slug + "'; run `collection install` first", l.dir);
    l.coll = col::parse_collection(read_file(j));
    return l;
}

bool parse_int(std::string_view s, std::int64_t& out) {
    if (s.empty() || s.size() > 12) return false;
    std::int64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

std::pmr::string P(std::string_view s, mol::mr* m) { return std::pmr::string(s, m); }

Result ok(Context& ctx, int code = 0) {
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = code;
    r.command = ctx.command;
    return r;
}

std::string status_of(const col::State& st, const std::string& key) {
    auto it = st.mods.find(key);
    return it == st.mods.end() || it->second.status.empty() ? "new" : it->second.status;
}

std::string human_size(std::uint64_t n) {
    const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; ++i; }
    char buf[32];
    std::snprintf(buf, sizeof buf, i == 0 ? "%.0f %s" : "%.2f %s", v, u[i]);
    return buf;
}

// collection install 开始前的预检：大小、剩余空间、游戏版本、doctor 里与环境有关的错误。只提示，不拦。
struct Preflight {
    std::size_t mods = 0;
    std::uint64_t total = 0, remaining = 0;
    std::int64_t free_space = -1;  // downloads/ 所在分区的可用字节（取不到 -1）
    std::vector<std::pair<std::string, std::string>> notes;  // (code, message)
};
Preflight preflight(const mol::Instance& inst, const Loaded& l, const col::State& st, const col::InstallOptions& opt, const std::string& gv) {
    Preflight p;
    std::error_code ec;
    // downloads/ 里已有的压缩包：按大小粗配（真正安装时 install_collection 还会按 大小+md5 核对）。
    // 同样大小的文件可以配给多个清单条目，各用一次。
    std::map<std::uint64_t, int> on_disk;
    for (fs::directory_iterator it(fs::path(std::string(inst.downloads_dir)), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string ext = std::string(mol::casefold(it->path().extension().string()));
        if (ext == ".meta" || ext == ".part" || ext == ".tmp") continue;
        ++on_disk[static_cast<std::uint64_t>(it->file_size(e2))];
    }
    ec.clear();
    for (const auto& m : l.coll.mods) {
        const std::string key = m.key();
        const auto ov = st.overrides.find(key);
        if ((ov != st.overrides.end() && ov->second.skip) || (m.optional && !opt.include_optional)) continue;
        ++p.mods;
        const auto z = static_cast<std::uint64_t>(std::max<std::int64_t>(m.source.file_size, 0));
        p.total += z;
        const auto sit = st.mods.find(key);
        const bool have = sit != st.mods.end() && ((sit->second.status == "installed" && !sit->second.mod_dir.empty() &&
                                                    fs::is_directory(fs::path(std::string(inst.mods_dir)) / sit->second.mod_dir, ec)) ||
                                                   (!sit->second.archive.empty() && fs::is_regular_file(sit->second.archive, ec)));
        if (have) continue;
        if (auto it = on_disk.find(z); z > 0 && it != on_disk.end() && it->second > 0) {
            --it->second;
            continue;
        }
        p.remaining += z;
    }
    fs::path probe(std::string(inst.downloads_dir));
    while (!probe.empty() && !fs::exists(probe, ec)) probe = probe.parent_path();
    if (struct statvfs sv{}; !probe.empty() && ::statvfs(probe.c_str(), &sv) == 0)
        p.free_space = static_cast<std::int64_t>(sv.f_bavail) * static_cast<std::int64_t>(sv.f_frsize);
    // 压缩包 + 解出来的 mod 大致要两倍
    if (p.free_space >= 0 && static_cast<std::uint64_t>(p.free_space) < p.remaining * 2)
        p.notes.emplace_back("disk_space", "only " + human_size(static_cast<std::uint64_t>(p.free_space)) + " free for " + human_size(p.remaining) +
                                               " of downloads (archives plus extracted mods need roughly twice that)");
    if (!l.coll.info.game_versions.empty() && !gv.empty() &&
        std::find(l.coll.info.game_versions.begin(), l.coll.info.game_versions.end(), gv) == l.coll.info.game_versions.end())
        p.notes.emplace_back("game_version", "the collection targets game version " + l.coll.info.game_versions.front() + " but this game is " + gv +
                                                 " (read `collection readme`: it may require a downgrade)");
    for (const auto& c : mol::run_doctor(inst, gv))
        if (c.level == "error" && (c.id.starts_with("game.") || c.id.starts_with("prefix") || c.id.starts_with("runner.")))
            p.notes.emplace_back("doctor", std::string(c.message) + (c.hint.empty() ? "" : " (" + std::string(c.hint) + ")"));
    return p;
}

}  // namespace

Result run_collection_inspect(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    // 只想看看要求、多大、能不能装：没有实例也行（状态一律 "new"，清单缓存在 ~/.cache/mo-linux/collections）
    std::optional<mol::Instance> inst;
    try {
        inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    } catch (const mol::Error& e) {
        if (e.code != "instance_not_found") throw;
    }
    const Ref ref = inst ? parse_ref(ctx.args.positionals.front(), *inst) : parse_ref(ctx.args.positionals.front(), search_domain(ctx));
    std::int64_t revision = 0;
    if (ctx.args.has("--revision") && !parse_int(ctx.args.get("--revision", "", ctx.mem), revision))
        return make_usage_error("collection inspect: --revision needs a positive integer", ctx);
    const Loaded l = load_remote(ctx, inst ? &*inst : nullptr, ref, revision);
    const col::State st = inst ? col::load_state(*inst, l.slug) : col::State{};

    CollectionInspectData d{.name = P(l.coll.info.name, ctx.mem), .slug = P(l.slug, ctx.mem), .author = P(l.coll.info.author, ctx.mem),
                            .domain = P(l.coll.info.domain, ctx.mem), .revision = l.revision,
                            .game_versions = std::pmr::vector<std::pmr::string>(ctx.mem),
                            .game_version = P(inst ? game_info_string(ctx, *inst, "version") : std::string(), ctx.mem),
                            .mod_count = static_cast<std::int64_t>(l.coll.mods.size()),
                            .plugin_count = static_cast<std::int64_t>(l.coll.plugins.size()), .rule_count = static_cast<std::int64_t>(l.coll.rules.size()),
                            .install_instructions = P(l.coll.info.install_instructions, ctx.mem), .url = P(l.readme.url, ctx.mem),
                            .summary = P(l.readme.summary, ctx.mem), .description = P(l.readme.description, ctx.mem),
                            .changelog = P(l.readme.changelog, ctx.mem), .mods = std::pmr::vector<CollectionModRow>(ctx.mem)};
    d.has_instance = inst.has_value();
    d.declared_total_size = l.total_size;
    d.sources = std::pmr::vector<CollectionSourceRow>(ctx.mem);
    // 真实下载量 = 清单里各文件大小之和（Nexus 页面上的 totalSize 常常偏低）
    for (const auto& m : l.coll.mods) {
        const std::int64_t z = std::max<std::int64_t>(m.source.file_size, 0);
        d.total_size += z;
        if (m.optional) d.optional_size += z;
        if (m.has_choices) ++d.fomod_choices;
        const std::string type = m.source.type.empty() ? std::string("unknown") : m.source.type;
        auto it = std::find_if(d.sources.begin(), d.sources.end(), [&](const CollectionSourceRow& s) { return std::string_view(s.type) == type; });
        if (it == d.sources.end()) {
            d.sources.push_back(CollectionSourceRow{.type = P(type, ctx.mem)});
            it = d.sources.end() - 1;
        }
        ++it->count;
        it->size += z;
    }
    for (const auto& v : l.coll.info.game_versions) d.game_versions.push_back(P(v, ctx.mem));
    for (const std::size_t i : col::install_order(l.coll)) {
        const auto& m = l.coll.mods[i];
        d.mods.push_back(CollectionModRow{.key = P(m.key(), ctx.mem), .name = P(m.name, ctx.mem), .version = P(m.version, ctx.mem), .optional = m.optional,
                                          .source_type = P(m.source.type, ctx.mem), .mod_id = m.source.mod_id, .file_id = m.source.file_id,
                                          .has_fomod_choices = m.has_choices, .has_patches = m.has_patches, .status = P(status_of(st, m.key()), ctx.mem),
                                          .file_size = m.source.file_size});
    }
    Result r = ok(ctx);
    r.set_data(std::move(d));
    return r;
}

// collection readme：集合页面的说明（Markdown）。不需要实例；在线取不到时读实例里缓存的 readme-<rev>.md。
Result run_collection_readme(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    std::optional<mol::Instance> inst;
    try {
        inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    } catch (const mol::Error& e) {
        if (e.code != "instance_not_found") throw;
    }
    const Ref ref = inst ? parse_ref(ctx.args.positionals.front(), *inst) : parse_ref(ctx.args.positionals.front(), search_domain(ctx));
    if (!ref.local.empty()) return make_usage_error("collection readme: give a slug or a collection URL (a local manifest has no page)", ctx);
    std::int64_t revision = 0;
    if (ctx.args.has("--revision") && !parse_int(ctx.args.get("--revision", "", ctx.mem), revision))
        return make_usage_error("collection readme: --revision needs a positive integer", ctx);

    CollectionReadmeData d{.name = P("", ctx.mem), .slug = P(ref.slug, ctx.mem), .url = P(page_url_of(ref), ctx.mem), .summary = P("", ctx.mem),
                           .description = P("", ctx.mem), .changelog = P("", ctx.mem), .markdown = P("", ctx.mem), .cached = false};
    try {
        const auto rev = make_client().collection_revision(ref.domain, ref.slug, revision, ctx.mem);
        const Readme rd{std::string(rev.summary), std::string(rev.description), std::string(rev.changelog), page_url_of(ref)};
        if (inst) save_readme(col::collection_dir(*inst, ref.slug), rev.revision_number, rd, rev.name);
        d.name = P(rev.name, ctx.mem);
        d.revision = rev.revision_number;
        d.summary = P(rd.summary, ctx.mem);
        d.description = P(rd.description, ctx.mem);
        d.changelog = P(rd.changelog, ctx.mem);
        d.markdown = P(readme_markdown(rev.name, rev.revision_number, rd), ctx.mem);
    } catch (const mol::Error& e) {
        // 离线/没有 key：退回缓存（取最新的修订）
        if (!inst || (e.code != "network_error" && e.code != "nexus_auth")) throw;
        std::error_code ec;
        fs::path best;
        std::int64_t best_rev = -1;
        for (fs::directory_iterator it(fs::path(col::collection_dir(*inst, ref.slug)), ec), end; !ec && it != end; it.increment(ec)) {
            const std::string n = it->path().filename().string();
            std::int64_t rv = 0;
            if (n.rfind("readme-", 0) != 0 || n.size() < 11 || !parse_int(n.substr(7, n.size() - 10), rv)) continue;
            if ((revision > 0 && rv == revision) || (revision <= 0 && rv > best_rev)) { best = it->path(); best_rev = rv; }
        }
        if (best.empty()) throw;
        d.revision = best_rev;
        d.markdown = P(read_file(best), ctx.mem);
        d.cached = true;
    }
    Result r = ok(ctx);
    r.set_data(std::move(d));
    return r;
}

Result run_collection_install(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    if (ctx.args.has("--downloads")) {  // 共享的下载目录（别的实例/别的盘）：只影响这次从哪找、往哪下压缩包
        const std::string dl(ctx.args.get("--downloads", "", ctx.mem));
        if (dl.empty()) return make_usage_error("collection install: --downloads needs a directory", ctx);
        std::error_code ec;
        fs::create_directories(dl, ec);
        if (!fs::is_directory(dl, ec)) return make_usage_error("collection install: --downloads is not a directory: " + dl, ctx);
        inst.downloads_dir = mol::string(fs::absolute(dl, ec).lexically_normal().string(), ctx.mem);
    }
    const Ref ref = parse_ref(ctx.args.positionals.front(), inst);
    std::int64_t revision = 0;
    if (ctx.args.has("--revision") && !parse_int(ctx.args.get("--revision", "", ctx.mem), revision))
        return make_usage_error("collection install: --revision needs a positive integer", ctx);

    EventSink* sink = ctx.sink;
    if (sink != nullptr) sink->start("collection install");
    try {
        // 拿不到远端清单（key 失效、断网、限流）但本地有上次的清单：用缓存继续——要下载的 mod 会变成 pending，已装的照常处理
        std::optional<std::string> offline_reason;
        Loaded l = [&] {
            try {
                return load_remote(ctx, &inst, ref, revision);
            } catch (const mol::Error& e) {
                const std::string code(e.code);
                if (code != "nexus_auth" && code != "network_error" && code != "nexus_rate_limited") throw;
                Loaded c;
                try { c = load_cached(inst, ref.slug); } catch (const mol::Error&) { throw e; }
                if (revision != 0 && c.revision != revision) throw;
                offline_reason = code + ": " + e.what();
                return c;
            }
        }();
        col::State st = col::load_state(inst, l.slug);
        if (st.revision != 0 && l.revision != 0 && st.revision != l.revision)
            st.revision = l.revision;  // 版本升级：沿用已装好的 mod（按 mod tag 对应），其余照常处理
        if (l.revision != 0) st.revision = l.revision;
        st.name = l.coll.info.name;

        std::optional<mol::NexusClient> client;
        if (!offline_reason) try { client.emplace(make_client()); } catch (const mol::Error&) {}  // 没有 key：只处理本地已有的文件
        if (offline_reason && !(ctx.globals && ctx.globals->quiet))
            std::fprintf(stderr, "collection install: Nexus unavailable (%s); using the cached manifest of revision %lld, downloads are skipped\n",
                         offline_reason->c_str(), static_cast<long long>(l.revision));

        col::InstallOptions opt;
        opt.profile = std::string(ctx.profile_override());
        opt.include_optional = !ctx.args.get_bool("--no-optional", false);
        opt.fomod_defaults = ctx.args.get_bool("--fomod-defaults", false);
        if (ctx.args.has("--reuse-from")) {
            const std::string list(ctx.args.get("--reuse-from", "", ctx.mem));
            for (std::size_t b = 0; b <= list.size();) {
                std::size_t e = list.find(',', b);
                if (e == std::string::npos) e = list.size();
                if (e > b) {
                    const auto other = mol::load_instance(list.substr(b, e - b), {}, ctx.mem);  // 不是实例 → instance_not_found
                    opt.reuse_from.emplace_back(other.mods_dir);
                }
                b = e + 1;
            }
        }
        if (ctx.args.has("--jobs")) {
            std::int64_t j = 0;
            if (!parse_int(ctx.args.get("--jobs", "", ctx.mem), j) || j < 1) return make_usage_error("collection install: --jobs needs an integer >= 1", ctx);
            opt.jobs = static_cast<unsigned>(j);
        }
        opt.progress = [&](std::string_view stage, std::string_view item, std::uint64_t done, std::uint64_t total) {
            if (sink != nullptr && total > 0) sink->progress(std::string(stage), done, total, item);
        };
        const std::string gv = game_info_string(ctx, inst, "version");
        const Preflight pre = preflight(inst, l, st, opt, gv);
        // 开始下载前就告诉人/GUI：stderr（-q 时不打）+ note 事件；结果里另有 data.preflight 与 warnings
        if (!(ctx.globals && ctx.globals->quiet)) {
            std::fprintf(stderr, "collection install: %zu mods, %s to download (%s already here), %s free in downloads/\n", pre.mods,
                         human_size(pre.remaining).c_str(), human_size(pre.total - pre.remaining).c_str(),
                         pre.free_space < 0 ? "?" : human_size(static_cast<std::uint64_t>(pre.free_space)).c_str());
            for (const auto& [code, msg] : pre.notes) std::fprintf(stderr, "  warning [%s] %s\n", code.c_str(), msg.c_str());
            std::fflush(stderr);
        }
        if (sink != nullptr) {
            sink->note("collection install", "preflight", "mods=" + std::to_string(pre.mods) + " download=" + std::to_string(pre.total) + " remaining=" + std::to_string(pre.remaining) +
                                                              " free=" + std::to_string(pre.free_space));
            for (const auto& [code, msg] : pre.notes) sink->note("collection install", code, msg);
        }
        const auto rep = col::install_collection(inst, client ? &*client : nullptr, l.coll, st, opt, gv);

        CollectionInstallData d{.name = P(l.coll.info.name, ctx.mem), .slug = P(l.slug, ctx.mem), .revision = l.revision,
                                .profile = P(opt.profile.empty() ? std::string(inst.cfg.profile) : opt.profile, ctx.mem),
                                .status = P(rep.complete() ? "complete" : "incomplete", ctx.mem), .installed = static_cast<std::int64_t>(rep.installed),
                                .skipped = static_cast<std::int64_t>(rep.skipped), .failed = static_cast<std::int64_t>(rep.failed),
                                .plugins_applied = static_cast<std::int64_t>(rep.plugins_applied), .mods = std::pmr::vector<CollectionOutcomeRow>(ctx.mem),
                                .pending = std::pmr::vector<CollectionPendingRow>(ctx.mem), .notes = std::pmr::vector<std::pmr::string>(ctx.mem)};
        for (const auto& m : rep.mods)
            d.mods.push_back(CollectionOutcomeRow{P(m.key, ctx.mem), P(m.name, ctx.mem), P(m.status, ctx.mem), P(m.mod_dir, ctx.mem), P(m.note, ctx.mem)});
        for (const auto& p : rep.pending)
            d.pending.push_back(CollectionPendingRow{P(p.key, ctx.mem), P(p.name, ctx.mem), P(p.kind, ctx.mem), P(p.detail, ctx.mem), P(p.url, ctx.mem), P("", ctx.mem), P("", ctx.mem)});
        for (const auto& n : rep.notes) d.notes.push_back(P(n, ctx.mem));
        Result r = ok(ctx, rep.complete() ? 0 : 4);
        if (offline_reason) r.add_warning("offline", "Nexus unavailable (" + *offline_reason + "); used the cached manifest of revision " + std::to_string(l.revision), "");
        for (const auto& n : rep.notes) r.add_warning("collection_note", n, "");
        for (const auto& [code, msg] : pre.notes)
            if (code != "game_version") r.add_warning(code, msg, "");  // 版本不一致已在 collection_note 里
        d.preflight = CollectionPreflightData{.mods = static_cast<std::int64_t>(pre.mods), .download_size = static_cast<std::int64_t>(pre.total),
                                              .remaining_size = static_cast<std::int64_t>(pre.remaining), .free_space = pre.free_space};
        r.set_data(std::move(d));
        if (sink != nullptr) sink->done("collection install", rep.complete());
        return r;
    } catch (...) {
        if (sink != nullptr) sink->done("collection install", false);
        throw;
    }
}

Result run_collection_status(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const Ref ref = parse_ref(ctx.args.positionals.front(), inst);
    const auto st = col::load_state(inst, ref.slug);
    if (st.mods.empty()) throw mol::Error("config_invalid", "no installation state for collection '" + ref.slug + "'");
    CollectionInstallData d{.name = P(st.name, ctx.mem), .slug = P(ref.slug, ctx.mem), .revision = st.revision, .profile = P(inst.cfg.profile, ctx.mem),
                            .status = P("complete", ctx.mem), .mods = std::pmr::vector<CollectionOutcomeRow>(ctx.mem),
                            .pending = std::pmr::vector<CollectionPendingRow>(ctx.mem), .notes = std::pmr::vector<std::pmr::string>(ctx.mem)};
    for (const auto& [key, m] : st.mods) {
        d.mods.push_back(CollectionOutcomeRow{P(key, ctx.mem), P(m.name, ctx.mem), P(m.status, ctx.mem), P(m.mod_dir, ctx.mem), P(m.note, ctx.mem)});
        if (m.status == "installed") ++d.installed;
        else if (m.status == "skipped") ++d.skipped;
        else if (m.status == "failed") { ++d.failed; d.status = P("incomplete", ctx.mem); }
        else {
            d.status = P("incomplete", ctx.mem);
            std::error_code aec;
            const bool have_archive = !m.archive.empty() && std::filesystem::is_regular_file(m.archive, aec);
            std::string decision;
            if (const auto ot = st.overrides.find(key); ot != st.overrides.end()) {
                const auto& o = ot->second;
                decision = o.skip ? "skip" : o.has_choices ? "fomod_choices" : o.fomod_defaults ? "fomod_defaults" : !o.archive.empty() ? "archive" : o.reinstall ? "reinstall" : "";
            }
            d.pending.push_back(CollectionPendingRow{P(key, ctx.mem), P(m.name, ctx.mem), P(m.kind.empty() ? "pending" : m.kind, ctx.mem), P(m.note, ctx.mem),
                                                     P(m.url, ctx.mem), P(have_archive ? m.archive : std::string(), ctx.mem), P(decision, ctx.mem)});
        }
    }
    Result r = ok(ctx, d.status == "complete" ? 0 : 4);
    r.set_data(std::move(d));
    return r;
}

Result run_collection_verify(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    if (ctx.args.has("--downloads")) inst.downloads_dir = mol::string(ctx.args.get("--downloads", "", ctx.mem), ctx.mem);
    const Ref ref = parse_ref(ctx.args.positionals.front(), inst);
    const Loaded l = load_cached(inst, ref.slug);
    col::State st = col::load_state(inst, ref.slug);
    EventSink* sink = ctx.sink;
    const auto rep = col::verify_collection(inst, l.coll, st, ctx.profile_override(), [&](std::size_t d, std::size_t t, std::string_view name) {
        if (sink != nullptr) sink->progress("verify", d, t, name);
    });
    CollectionVerifyData d{.slug = P(ref.slug, ctx.mem), .checked = static_cast<std::int64_t>(rep.checked), .skipped = static_cast<std::int64_t>(rep.skipped),
                           .mismatched = std::pmr::vector<VerifyRow>(ctx.mem), .marked = 0, .missing_plugins = std::pmr::vector<std::pmr::string>(ctx.mem)};
    for (const auto& p : rep.missing_plugins) d.missing_plugins.push_back(P(p, ctx.mem));
    const bool fix = ctx.args.get_bool("--fix", false);
    for (const auto& m : rep.mismatched) {
        VerifyRow row{P(m.key, ctx.mem), P(m.name, ctx.mem), P(m.mod_dir, ctx.mem), P(m.kind, ctx.mem), static_cast<std::int64_t>(m.missing.size()),
                      static_cast<std::int64_t>(m.extra.size()), std::pmr::vector<std::pmr::string>(ctx.mem), std::pmr::vector<std::pmr::string>(ctx.mem)};
        for (std::size_t i = 0; i < m.missing.size() && i < 10; ++i) row.missing.push_back(P(m.missing[i], ctx.mem));
        for (std::size_t i = 0; i < m.extra.size() && i < 10; ++i) row.extra.push_back(P(m.extra[i], ctx.mem));
        d.mismatched.push_back(std::move(row));
        // 复刻缺的文件通常是压缩包里本来就没有（安装时已记进 note），重装也补不上：只标记 FOMOD 的
        if (fix && m.kind == "fomod") { st.overrides[m.key].reinstall = true; ++d.marked; }
    }
    if (fix && d.marked > 0) col::save_state(inst, st);
    Result r = ok(ctx, d.mismatched.empty() && d.missing_plugins.empty() ? 0 : 4);
    r.set_data(std::move(d));
    return r;
}

Result run_collection_resolve(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const Ref ref = parse_ref(ctx.args.positionals.front(), inst);
    const std::string key(ctx.args.get("--mod", "", ctx.mem));
    if (key.empty()) return make_usage_error("collection resolve: --mod KEY is required", ctx);
    const bool skip = ctx.args.get_bool("--skip", false), defaults = ctx.args.get_bool("--fomod-defaults", false),
               reinstall = ctx.args.get_bool("--reinstall", false);
    const std::string fomod_file(ctx.args.get("--fomod", "", ctx.mem)), archive(ctx.args.get("--archive", "", ctx.mem)), nxm_s(ctx.args.get("--nxm", "", ctx.mem));
    const int given = (skip ? 1 : 0) + (defaults ? 1 : 0) + (fomod_file.empty() ? 0 : 1) + (archive.empty() ? 0 : 1) + (nxm_s.empty() ? 0 : 1) + (reinstall ? 1 : 0);
    if (given != 1) return make_usage_error("collection resolve: give exactly one of --skip, --fomod, --fomod-defaults, --archive, --nxm, --reinstall", ctx);

    col::State st = col::load_state(inst, ref.slug);
    if (st.mods.find(key) == st.mods.end()) {
        // 允许用 mod 名称代替 key
        for (const auto& [k, m] : st.mods) if (mol::casefold(m.name) == mol::casefold(key)) { return make_usage_error("collection resolve: use the mod key '" + k + "' for '" + key + "'", ctx); }
        throw mol::Error("mod_not_found", "no such mod in the collection state: " + key);
    }
    col::Override& ov = st.overrides[key];
    std::string recorded, used_archive;
    if (skip) { ov = col::Override{}; ov.skip = true; recorded = "skip"; }
    else if (reinstall) { ov.reinstall = true; ov.skip = false; recorded = "reinstall"; }
    else if (defaults) { ov.fomod_defaults = true; ov.skip = false; recorded = "fomod_defaults"; }
    else if (!fomod_file.empty()) {
        ov.choices = mol::fomod::parse_choices_json(read_file(fomod_file));
        ov.has_choices = true;
        ov.skip = false;
        recorded = "fomod_choices";
    } else if (!archive.empty()) {
        std::error_code ec;
        const std::string abs = fs::absolute(archive, ec).string();
        if (!fs::is_regular_file(abs, ec)) throw mol::Error("invalid_argument", "archive not found", abs);
        ov.archive = abs;
        ov.skip = false;
        recorded = "archive";
        used_archive = abs;
    } else {
        const mol::NxmUrl nxm = mol::parse_nxm(nxm_s, ctx.mem);
        const Loaded l = load_cached(inst, ref.slug);
        const col::Mod* target = nullptr;
        for (const auto& m : l.coll.mods) if (m.key() == key) target = &m;
        if (!target) throw mol::Error("mod_not_found", "no such mod in the collection: " + key);
        if (target->source.mod_id != nxm.mod_id || target->source.file_id != nxm.file_id)
            throw mol::Error("invalid_argument", "the nxm link is for mod " + std::to_string(nxm.mod_id) + " file " + std::to_string(nxm.file_id) +
                                                    ", but this collection needs mod " + std::to_string(target->source.mod_id) + " file " + std::to_string(target->source.file_id));
        const mol::NexusClient client = make_client();
        const auto dl = mol::nexus_download(client, inst.downloads_dir, nxm.game, nxm.mod_id, nxm.file_id, &nxm);
        used_archive = std::string(dl.path);
        // 下载的文件走正常的 md5 校验流程：不记为用户提供的压缩包（那样会跳过校验），只要它在 downloads/ 里，install 会按大小+md5 找到它
        recorded = "archive";
    }
    col::save_state(inst, st);
    Result r = ok(ctx);
    r.set_data(CollectionResolveData{.key = P(key, ctx.mem), .recorded = P(recorded, ctx.mem), .archive = P(used_archive, ctx.mem)});
    return r;
}

Result run_collection_search(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    std::int64_t count = 10, offset = 0;
    if (ctx.args.has("--count") && !parse_int(ctx.args.get("--count", "", ctx.mem), count)) return make_usage_error("collection search: --count needs an integer", ctx);
    if (ctx.args.has("--offset") && !parse_int(ctx.args.get("--offset", "", ctx.mem), offset)) return make_usage_error("collection search: --offset needs an integer", ctx);
    const mol::string domain = search_domain(ctx);
    const mol::string sort = ctx.args.get("--sort", "endorsements", ctx.mem);
    const mol::string query = ctx.args.positionals.front();
    std::int64_t total = 0;
    const auto found = make_client().search_collections(domain, query, sort, static_cast<int>(count), static_cast<int>(offset), &total, ctx.mem);
    CollectionSearchData d{.game = domain, .query = query, .sort = sort, .total = total, .collections = std::pmr::vector<NexusCollectionRow>(ctx.mem)};
    for (const auto& c : found)
        d.collections.push_back(NexusCollectionRow{.slug = P(c.slug, ctx.mem), .name = P(c.name, ctx.mem), .summary = P(c.summary, ctx.mem), .endorsements = c.endorsements,
                                                   .downloads = c.downloads, .revision = c.revision, .mod_count = c.mod_count, .total_size = c.total_size});
    Result r = ok(ctx);
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli
