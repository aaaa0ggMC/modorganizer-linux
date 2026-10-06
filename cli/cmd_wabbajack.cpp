// wabbajack search / inspect / install。设计见 core/include/mol/wabbajack.hpp。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <filesystem>
#include <map>

#include "mol/casefold.hpp"
#include "mol/http.hpp"
#include "mol/instance.hpp"
#include "mol/mo2fmt.hpp"
#include "mol/nexus.hpp"
#include "mol/steam_detect.hpp"
#include "mol/wabbajack.hpp"
#include "mol/wabbajack_install.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {
namespace {
namespace fs = std::filesystem;
namespace wj = mol::wabbajack;

std::pmr::string P(std::string_view s, mol::mr* m) { return std::pmr::string(s, m); }

Result ok(Context& ctx, int code = 0) {
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = code;
    r.command = ctx.command;
    return r;
}

bool parse_int(std::string_view s, std::int64_t& out) {
    if (s.empty() || s.size() > 9) return false;
    std::int64_t v = 0;
    for (char c : s) { if (c < '0' || c > '9') return false; v = v * 10 + (c - '0'); }
    out = v;
    return true;
}

// 实例可以还不存在（install 的输出就是它）：取游戏 id 与（探测到的）游戏目录。
struct Env {
    std::string game = "skyrimse";
    std::string game_dir;
};
Env env_for(Context& ctx) {
    Env e;
    try {
        const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
        e.game = std::string(inst.cfg.game);
        e.game_dir = std::string(inst.cfg.game_dir);
    } catch (const mol::Error& err) {
        if (err.code != "instance_not_found") throw;
        e.game_dir = std::string(mol::detect_steam({}, ctx.mem).game_dir);
    }
    return e;
}

std::string cache_dir(Context& ctx) { return (fs::path(std::string(ctx.instance_dir)) / ".mol-wabbajack-lists").string(); }

// LIST：本地 .wabbajack 路径 | 画廊里的 machineURL / 标题 | authored-files 的下载 URL。返回本地文件路径。
std::string resolve_list(Context& ctx, const Env& /*env*/, const std::string& ref) {
    std::error_code ec;
    if (fs::is_regular_file(ref, ec)) return ref;
    std::string url;
    std::string name;
    if (ref.rfind("http", 0) == 0) {
        url = ref;
        name = fs::path(ref).filename().string();
    } else {
        const std::string want = std::string(mol::casefold(ref));
        {   // 之前下载过同名清单：不必再查画廊
            std::string guess = ref;
            for (char& c : guess) if (c == '/' || c == '\\' || c == ' ') c = '_';
            const std::string cached = (fs::path(cache_dir(ctx)) / (guess + ".wabbajack")).string();
            if (fs::is_regular_file(cached, ec)) return cached;
        }
        const auto gallery = wj::fetch_gallery("");
        const wj::GalleryEntry* hit = nullptr;
        for (const auto& g : gallery)
            if (std::string(mol::casefold(g.machine_url)) == want) { hit = &g; break; }
        if (!hit) for (const auto& g : gallery) if (std::string(mol::casefold(g.title)) == want) { hit = &g; break; }
        if (!hit) throw mol::Error("mod_not_found", "no such mod list in the Wabbajack gallery: " + ref + " (try `wabbajack search`)");
        url = hit->download_url;
        name = (hit->machine_url.empty() ? hit->title : hit->machine_url) + ".wabbajack";
    }
    for (char& c : name) if (c == '/' || c == '\\' || c == ' ') c = '_';
    if (name.size() < 10 || name.substr(name.size() - 10) != ".wabbajack") name += ".wabbajack";
    fs::create_directories(cache_dir(ctx), ec);
    const std::string dest = (fs::path(cache_dir(ctx)) / name).string();
    if (fs::is_regular_file(dest, ec)) return dest;  // 已下载过（hash 在下载时校验过）
    wj::download_authored(url, dest, [&](std::uint64_t d, std::uint64_t t) { if (ctx.sink != nullptr && t > 0) ctx.sink->progress("download", d, t); return true; });
    return dest;
}

}  // namespace

Result run_wabbajack_search(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    std::int64_t count = 20, offset = 0;
    if (ctx.args.has("--count") && !parse_int(ctx.args.get("--count", "", ctx.mem), count)) return make_usage_error("wabbajack search: --count needs an integer", ctx);
    if (ctx.args.has("--offset") && !parse_int(ctx.args.get("--offset", "", ctx.mem), offset)) return make_usage_error("wabbajack search: --offset needs an integer", ctx);
    const Env env = env_for(ctx);
    const bool all = ctx.args.get_bool("--all-games", false);
    const bool nsfw = ctx.args.get_bool("--nsfw", false);
    const std::string domain = all ? std::string() : std::string(mol::nexus_game_domain(env.game));
    const std::string q = std::string(mol::casefold(std::string(ctx.args.positionals.front())));
    auto gallery = wj::fetch_gallery(domain);
    std::erase_if(gallery, [&](const wj::GalleryEntry& g) {
        if (g.nsfw && !nsfw) return true;
        if (q.empty()) return false;
        return std::string(mol::casefold(g.title)).find(q) == std::string::npos && std::string(mol::casefold(g.description)).find(q) == std::string::npos &&
               std::string(mol::casefold(g.machine_url)).find(q) == std::string::npos;
    });
    std::sort(gallery.begin(), gallery.end(), [](const wj::GalleryEntry& a, const wj::GalleryEntry& b) { return a.archives_size < b.archives_size; });  // 小的在前：先看得到「装得起」的
    WjSearchData d{.game = P(domain, ctx.mem), .query = P(ctx.args.positionals.front(), ctx.mem), .total = static_cast<std::int64_t>(gallery.size()),
                   .lists = std::pmr::vector<WjGalleryRow>(ctx.mem)};
    for (std::int64_t i = offset; i < static_cast<std::int64_t>(gallery.size()) && i < offset + count; ++i) {
        const auto& g = gallery[static_cast<std::size_t>(i)];
        std::string desc = g.description.substr(0, 240);
        d.lists.push_back(WjGalleryRow{.title = P(g.title, ctx.mem), .machine_url = P(g.machine_url, ctx.mem), .repository = P(g.repository, ctx.mem), .author = P(g.author, ctx.mem),
                                       .version = P(g.version, ctx.mem), .description = P(desc, ctx.mem), .download_size = g.download_size, .archives_size = g.archives_size,
                                       .installed_size = g.installed_size, .archive_count = g.archive_count, .nsfw = g.nsfw, .unavailable = g.unavailable});
    }
    Result r = ok(ctx);
    r.set_data(std::move(d));
    return r;
}

Result run_wabbajack_inspect(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const Env env = env_for(ctx);
    const std::string file = resolve_list(ctx, env, std::string(ctx.args.positionals.front()));
    const wj::Modlist m = wj::parse_modlist(wj::read_modlist_json(file));

    WjInspectData d{.name = P(m.name, ctx.mem), .author = P(m.author, ctx.mem), .version = P(m.version, ctx.mem), .description = P(m.description, ctx.mem),
                    .game_type = P(m.game_type, ctx.mem), .game_id = P(wj::game_id_of(m.game_type), ctx.mem), .nsfw = m.nsfw,
                    .game_matches = wj::game_id_of(m.game_type) == env.game, .file = P(file, ctx.mem), .archive_count = static_cast<std::int64_t>(m.archives.size()),
                    .directive_count = static_cast<std::int64_t>(m.directives.size()), .sources = std::pmr::vector<WjCount>(ctx.mem), .directives = std::pmr::vector<WjCount>(ctx.mem), .verdict = std::pmr::string(ctx.mem)};
    std::map<std::string, std::pair<std::int64_t, std::int64_t>> src;
    for (const auto& a : m.archives) { auto& e = src[a.src.kind]; ++e.first; e.second += a.size; d.archive_size += a.size; }
    for (const auto& [k, v] : src)
        d.sources.push_back(WjCount{.name = P(k, ctx.mem), .count = v.first, .size = v.second, .supported = k == "nexus" || k == "http" || k == "cdn" || k == "gamefile"});
    std::map<std::string, std::pair<std::int64_t, bool>> dirs;
    for (const auto& x : m.directives) {
        auto& e = dirs[x.type];
        ++e.first;
        e.second = wj::supported(x.kind);
        if (e.second) ++d.supported_directives;
    }
    for (const auto& [k, v] : dirs) d.directives.push_back(WjCount{.name = P(k, ctx.mem), .count = v.first, .size = 0, .supported = v.second});
    d.verdict = P(d.supported_directives == d.directive_count ? "full" : d.supported_directives == 0 ? "none" : "partial", ctx.mem);
    Result r = ok(ctx);
    if (!d.game_matches) r.add_warning("wabbajack_game_mismatch", "this list is for " + m.game_type + ", not the instance's game", "");
    r.set_data(std::move(d));
    return r;
}

Result run_wabbajack_install(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    Env env = env_for(ctx);
    if (ctx.args.has("--game-dir")) env.game_dir = std::string(ctx.args.get("--game-dir", "", ctx.mem));
    if (env.game_dir.empty()) throw mol::Error("config_invalid", "no game directory: pass --game-dir (Steam was not auto-detected)");
    std::error_code ec;
    if (!fs::is_directory(env.game_dir, ec)) throw mol::Error("config_invalid", "game directory does not exist", env.game_dir);

    EventSink* sink = ctx.sink;
    if (sink != nullptr) sink->start("wabbajack install");
    try {
        const std::string file = resolve_list(ctx, env, std::string(ctx.args.positionals.front()));
        const wj::Modlist m = wj::parse_modlist(wj::read_modlist_json(file));
        if (wj::game_id_of(m.game_type) != env.game)
            throw mol::Error("invalid_argument", "this list is for " + m.game_type + ", but this instance is for " + env.game);

        std::optional<mol::NexusClient> client;
        if (auto key = mol::load_nexus_key(ctx.mem)) client.emplace(*key, "0.1");

        wj::InstallOptions opt;
        opt.output_dir = std::string(ctx.instance_dir);
        opt.game_dir = env.game_dir;
        if (ctx.args.has("--downloads")) opt.downloads_dir = std::string(ctx.args.get("--downloads", "", ctx.mem));
        if (ctx.args.has("--jobs")) {
            std::int64_t j = 0;
            if (!parse_int(ctx.args.get("--jobs", "", ctx.mem), j) || j < 1) return make_usage_error("wabbajack install: --jobs needs an integer >= 1", ctx);
            opt.jobs = static_cast<unsigned>(j);
        }
        opt.client = client ? &*client : nullptr;
        opt.progress = [&](std::string_view stage, std::string_view item, std::uint64_t d, std::uint64_t t) { if (sink != nullptr && t > 0) sink->progress(std::string(stage), d, t, item); };
        const wj::Report rep = wj::install_modlist(m, file, opt);

        // 让它成为 mo-linux 能直接用的实例（只在没有 mo-linux.json 时写；已有的配置不动）
        if (!fs::exists(fs::path(std::string(ctx.instance_dir)) / "mo-linux.json", ec)) {
            const auto det = mol::detect_steam({}, ctx.mem);
            mol::InitOptions io;
            const std::string root(ctx.instance_dir), gd = env.game_dir, pf(det.prefix), pr(det.proton_path), sr(det.steam_root);
            io.root = root;
            io.game = env.game;
            // 清单若带「Stock Game」（实例里自带一份游戏拷贝，ModOrganizer.ini 的 gamePath 指向它）就用它，而不是 Steam 目录
            std::string stock;
            if (auto gp = mol::Ini::load((fs::path(root) / "ModOrganizer.ini").string(), ctx.mem).get("General", "gamePath", ctx.mem)) {
                const std::string unix_gp(mol::wine_to_unix(*gp, pf, ctx.mem));
                std::error_code e3;
                const auto canon = fs::weakly_canonical(unix_gp, e3);
                const auto rootc = fs::weakly_canonical(root, e3);
                if (!unix_gp.empty() && fs::is_directory(unix_gp, e3) && canon.string().rfind(rootc.string() + "/", 0) == 0) stock = unix_gp;
            }
            io.game_dir = stock.empty() ? std::string_view(gd) : std::string_view(stock);
            io.prefix = pf;
            io.proton_path = pr;
            io.steam_root = sr;
            // 清单自带的 MO2 配置里选中的 profile 才是作者想让你用的（默认的 "Default" 往往是空的）
            std::string prof;
            if (auto v = mol::Ini::load((fs::path(root) / "ModOrganizer.ini").string(), ctx.mem).get("General", "selected_profile", ctx.mem)) prof = std::string(*v);
            io.profile = prof.empty() ? std::string_view("Default") : std::string_view(prof);
            if (!pf.empty()) mol::init_instance(io);
        }

        WjInstallData d{.status = P(rep.complete() ? "complete" : "incomplete", ctx.mem), .instance = P(ctx.instance_dir, ctx.mem), .archives_total = rep.archives_total,
                        .archives_done = rep.archives_done, .files_written = rep.files_written, .files_failed = rep.files_failed,
                        .pending = std::pmr::vector<CollectionPendingRow>(ctx.mem), .failures = std::pmr::vector<std::pmr::string>(ctx.mem), .notes = std::pmr::vector<std::pmr::string>(ctx.mem)};
        for (const auto& p : rep.pending) d.pending.push_back(CollectionPendingRow{P(std::to_string(p.count), ctx.mem), P(p.name, ctx.mem), P(p.kind, ctx.mem), P(p.detail, ctx.mem), P(p.url, ctx.mem), P("", ctx.mem), P("", ctx.mem)});
        for (const auto& f : rep.failures) d.failures.push_back(P(f, ctx.mem));
        for (const auto& n : rep.notes) d.notes.push_back(P(n, ctx.mem));
        Result r = ok(ctx, rep.complete() ? 0 : 4);
        for (const auto& n : rep.notes) r.add_warning("wabbajack_note", n, "");
        r.set_data(std::move(d));
        if (sink != nullptr) sink->done("wabbajack install", rep.complete());
        return r;
    } catch (...) {
        if (sink != nullptr) sink->done("wabbajack install", false);
        throw;
    }
}

}  // namespace cli
