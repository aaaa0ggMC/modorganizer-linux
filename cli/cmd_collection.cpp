// collection inspect/install/status/resolve —— Nexus Collections 导入（设计见 core/include/mol/collection.hpp）。
// 退出码：install 未完成（有 pending/failed）→ 4，ok 仍为 true。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <filesystem>
#include <fstream>
#include <iterator>

#include "mol/casefold.hpp"
#include "mol/collection.hpp"
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

// slug | https://www.nexusmods.com/games/<domain>/collections/<slug>[/…] | 本地文件
Ref parse_ref(std::string_view s, const mol::Instance& inst) {
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
        const auto g = x.find("/games/");
        const auto c = x.find("/collections/");
        if (g == std::string::npos || c == std::string::npos || c < g) throw mol::Error("invalid_argument", "unrecognised collection URL: " + std::string(s));
        r.domain = x.substr(g + 7, c - (g + 7));
        std::string rest = x.substr(c + 13);
        if (auto sl = rest.find('/'); sl != std::string::npos) rest.erase(sl);
        r.slug = rest;
    } else {
        r.slug = x;
    }
    if (r.slug.empty()) throw mol::Error("invalid_argument", "empty collection slug");
    if (r.domain.empty()) r.domain = std::string(mol::nexus_game_domain(inst.cfg.game));
    return r;
}

struct Loaded {
    col::Collection coll;
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

Loaded load_remote(Context& ctx, const mol::Instance& inst, const Ref& ref, std::int64_t revision) {
    Loaded l;
    l.slug = ref.slug;
    if (!ref.local.empty()) {
        const fs::path p(ref.local);
        l.dir = col::collection_dir(inst, ref.slug);
        const std::string ext = std::string(mol::casefold(p.extension().string()));
        l.coll = col::parse_collection(ext == ".json" ? read_file(p) : unpack_manifest(ref.local, fs::path(l.dir) / "archive-local"));
        return l;
    }
    const mol::NexusClient client = make_client();
    const auto rev = client.collection_revision(ref.domain, ref.slug, revision, ctx.mem);
    l.revision = rev.revision_number;
    l.total_size = rev.total_size;
    l.dir = col::collection_dir(inst, ref.slug);
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

}  // namespace

Result run_collection_inspect(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const Ref ref = parse_ref(ctx.args.positionals.front(), inst);
    std::int64_t revision = 0;
    if (ctx.args.has("--revision") && !parse_int(ctx.args.get("--revision", "", ctx.mem), revision))
        return make_usage_error("collection inspect: --revision needs a positive integer", ctx);
    const Loaded l = load_remote(ctx, inst, ref, revision);
    const auto st = col::load_state(inst, l.slug);

    CollectionInspectData d{.name = P(l.coll.info.name, ctx.mem), .slug = P(l.slug, ctx.mem), .author = P(l.coll.info.author, ctx.mem),
                            .domain = P(l.coll.info.domain, ctx.mem), .revision = l.revision,
                            .game_versions = std::pmr::vector<std::pmr::string>(ctx.mem), .game_version = P(game_info_string(ctx, inst, "version"), ctx.mem),
                            .mod_count = static_cast<std::int64_t>(l.coll.mods.size()), .total_size = l.total_size,
                            .plugin_count = static_cast<std::int64_t>(l.coll.plugins.size()), .rule_count = static_cast<std::int64_t>(l.coll.rules.size()),
                            .install_instructions = P(l.coll.info.install_instructions, ctx.mem), .mods = std::pmr::vector<CollectionModRow>(ctx.mem)};
    for (const auto& v : l.coll.info.game_versions) d.game_versions.push_back(P(v, ctx.mem));
    for (const std::size_t i : col::install_order(l.coll)) {
        const auto& m = l.coll.mods[i];
        d.mods.push_back(CollectionModRow{.key = P(m.key(), ctx.mem), .name = P(m.name, ctx.mem), .version = P(m.version, ctx.mem), .optional = m.optional,
                                          .source_type = P(m.source.type, ctx.mem), .mod_id = m.source.mod_id, .file_id = m.source.file_id,
                                          .has_fomod_choices = m.has_choices, .has_patches = m.has_patches, .status = P(status_of(st, m.key()), ctx.mem)});
    }
    Result r = ok(ctx);
    r.set_data(std::move(d));
    return r;
}

Result run_collection_install(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const Ref ref = parse_ref(ctx.args.positionals.front(), inst);
    std::int64_t revision = 0;
    if (ctx.args.has("--revision") && !parse_int(ctx.args.get("--revision", "", ctx.mem), revision))
        return make_usage_error("collection install: --revision needs a positive integer", ctx);

    EventSink* sink = ctx.sink;
    if (sink != nullptr) sink->start("collection install");
    try {
        const Loaded l = load_remote(ctx, inst, ref, revision);
        col::State st = col::load_state(inst, l.slug);
        if (st.revision != 0 && l.revision != 0 && st.revision != l.revision)
            st.revision = l.revision;  // 版本升级：沿用已装好的 mod（按 mod tag 对应），其余照常处理
        if (l.revision != 0) st.revision = l.revision;
        st.name = l.coll.info.name;

        std::optional<mol::NexusClient> client;
        try { client.emplace(make_client()); } catch (const mol::Error&) {}  // 没有 key：只处理本地已有的文件

        col::InstallOptions opt;
        opt.profile = std::string(ctx.profile_override());
        opt.include_optional = !ctx.args.get_bool("--no-optional", false);
        opt.fomod_defaults = ctx.args.get_bool("--fomod-defaults", false);
        if (ctx.args.has("--jobs")) {
            std::int64_t j = 0;
            if (!parse_int(ctx.args.get("--jobs", "", ctx.mem), j) || j < 1) return make_usage_error("collection install: --jobs needs an integer >= 1", ctx);
            opt.jobs = static_cast<unsigned>(j);
        }
        opt.progress = [&](std::string_view stage, std::string_view item, std::uint64_t done, std::uint64_t total) {
            if (sink != nullptr && total > 0) sink->progress(std::string(stage), done, total, item);
        };
        const std::string gv = game_info_string(ctx, inst, "version");
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
            d.pending.push_back(CollectionPendingRow{P(p.key, ctx.mem), P(p.name, ctx.mem), P(p.kind, ctx.mem), P(p.detail, ctx.mem), P(p.url, ctx.mem), P("", ctx.mem)});
        for (const auto& n : rep.notes) d.notes.push_back(P(n, ctx.mem));
        Result r = ok(ctx, rep.complete() ? 0 : 4);
        for (const auto& n : rep.notes) r.add_warning("collection_note", n, "");
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
            d.pending.push_back(CollectionPendingRow{P(key, ctx.mem), P(m.name, ctx.mem), P(m.kind.empty() ? "pending" : m.kind, ctx.mem), P(m.note, ctx.mem),
                                                     P(m.url, ctx.mem), P(have_archive ? m.archive : std::string(), ctx.mem)});
        }
    }
    Result r = ok(ctx, d.status == "complete" ? 0 : 4);
    r.set_data(std::move(d));
    return r;
}

Result run_collection_resolve(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const Ref ref = parse_ref(ctx.args.positionals.front(), inst);
    const std::string key(ctx.args.get("--mod", "", ctx.mem));
    if (key.empty()) return make_usage_error("collection resolve: --mod KEY is required", ctx);
    const bool skip = ctx.args.get_bool("--skip", false), defaults = ctx.args.get_bool("--fomod-defaults", false);
    const std::string fomod_file(ctx.args.get("--fomod", "", ctx.mem)), archive(ctx.args.get("--archive", "", ctx.mem)), nxm_s(ctx.args.get("--nxm", "", ctx.mem));
    const int given = (skip ? 1 : 0) + (defaults ? 1 : 0) + (fomod_file.empty() ? 0 : 1) + (archive.empty() ? 0 : 1) + (nxm_s.empty() ? 0 : 1);
    if (given != 1) return make_usage_error("collection resolve: give exactly one of --skip, --fomod, --fomod-defaults, --archive, --nxm", ctx);

    col::State st = col::load_state(inst, ref.slug);
    if (st.mods.find(key) == st.mods.end()) {
        // 允许用 mod 名称代替 key
        for (const auto& [k, m] : st.mods) if (mol::casefold(m.name) == mol::casefold(key)) { return make_usage_error("collection resolve: use the mod key '" + k + "' for '" + key + "'", ctx); }
        throw mol::Error("mod_not_found", "no such mod in the collection state: " + key);
    }
    col::Override& ov = st.overrides[key];
    std::string recorded, used_archive;
    if (skip) { ov = col::Override{}; ov.skip = true; recorded = "skip"; }
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
