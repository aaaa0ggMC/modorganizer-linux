// nexus login/logout/whoami/files/download。API key 永远不进输出/日志；下载文件落到实例的 downloads/。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <iostream>
#include <set>
#include <iterator>

#include "mol/game_host.hpp"
#include "mol/http.hpp"
#include "mol/instance.hpp"
#include "mol/nexus.hpp"
#include "mol/skse.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {
namespace {

mol::NexusClient make_client() {
    const auto key = mol::load_nexus_key();
    if (!key) throw mol::Error("nexus_auth", "no Nexus API key (set NEXUS_API_KEY or run `nexus login`)");
    return mol::NexusClient(*key, "0.1");
}

bool parse_id(std::string_view s, long long& out) {
    if (s.empty()) return false;
    long long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
        if (v > (1LL << 53)) return false;
    }
    out = v;
    return true;
}

std::string url_basename(std::string_view url) {
    std::string_view p = url;
    if (auto q = p.find_first_of("?#"); q != std::string_view::npos) p = p.substr(0, q);
    if (auto s = p.rfind('/'); s != std::string_view::npos) p = p.substr(s + 1);
    std::string out;
    for (std::size_t i = 0; i < p.size(); ++i) {
        auto hv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        if (p[i] == '%' && i + 2 < p.size() && hv(p[i + 1]) >= 0 && hv(p[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hv(p[i + 1]) * 16 + hv(p[i + 2])));
            i += 2;
        } else {
            out.push_back(p[i]);
        }
    }
    for (char& c : out) if (c == '/' || c == '\\' || c == '\0') c = '_';
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    return out;
}

Result ok_result(Context& ctx) {
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    return r;
}

bool opt_int(Context& ctx, const char* opt, long long def, long long& out) {
    out = def;
    if (!ctx.args.has(opt)) return true;
    return parse_id(ctx.args.get(opt, "", ctx.mem), out);
}

std::set<std::int64_t> installed_nexus_ids(Context& ctx, const mol::Instance& inst) {
    std::set<std::int64_t> ids;
    for (const auto& m : mol::list_mods(inst, ctx.profile_override(), ctx.mem))
        if (m.nexus_id > 0 && m.exists) ids.insert(m.nexus_id);
    return ids;
}

NexusModRow to_row(const mol::NexusModSummary& m, bool installed, mol::mr* mem) {
    return NexusModRow{.mod_id = m.mod_id, .name = std::pmr::string(m.name, mem), .author = std::pmr::string(m.author, mem),
                       .summary = std::pmr::string(m.summary, mem), .version = std::pmr::string(m.version, mem),
                       .updated_at = std::pmr::string(m.updated_at, mem), .endorsements = m.endorsements, .downloads = m.downloads,
                       .installed = installed};
}

// 选 mod 的主文件：is_primary 优先，其次 MAIN 类别里 file_id 最大者，再其次非 ARCHIVED/OLD_VERSION 里最大者。
const mol::NexusFile* pick_main_file(const mol::vector<mol::NexusFile>& files) {
    const mol::NexusFile* pick = nullptr;
    for (const auto& f : files) if (f.is_primary && (!pick || f.file_id > pick->file_id)) pick = &f;
    if (pick) return pick;
    for (const auto& f : files) if (f.category == "MAIN" && (!pick || f.file_id > pick->file_id)) pick = &f;
    if (pick) return pick;
    for (const auto& f : files)
        if (f.category != "ARCHIVED" && f.category != "OLD_VERSION" && f.category != "DELETED" && (!pick || f.file_id > pick->file_id)) pick = &f;
    return pick;
}

}  // namespace

Result run_nexus_login(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    std::string key;
    const mol::string file = ctx.args.get("--key-file", "", ctx.mem);
    if (!file.empty()) {
        std::ifstream in(std::string(file), std::ios::binary);
        if (!in) throw mol::Error("io_error", "cannot read key file", std::string(file));
        key.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else {
        key.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());  // stdin
    }
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
    if (key.empty()) return make_usage_error("nexus login: no key (use --key-file F or pipe it on stdin)", ctx);

    // 先验证再保存：错误的 key 不落盘。
    const mol::NexusClient c(key, "0.1");
    const mol::NexusUser u = c.validate(ctx.mem);
    mol::save_nexus_key(key);
    Result r = ok_result(ctx);
    r.set_data(NexusUserData{.name = mol::string(u.name, ctx.mem), .user_id = u.user_id, .is_premium = u.is_premium,
                             .is_supporter = u.is_supporter, .key_path = mol::string(mol::nexus_key_path(ctx.mem), ctx.mem)});
    return r;
}

Result run_nexus_logout(Context& ctx) {
    const bool removed = mol::remove_nexus_key();
    Result r = ok_result(ctx);
    r.set_data(NexusLogoutData{.removed = removed});
    return r;
}

Result run_nexus_whoami(Context& ctx) {
    const mol::NexusUser u = make_client().validate(ctx.mem);
    Result r = ok_result(ctx);
    r.set_data(NexusUserData{.name = mol::string(u.name, ctx.mem), .user_id = u.user_id, .is_premium = u.is_premium,
                             .is_supporter = u.is_supporter, .key_path = mol::string(ctx.mem)});
    return r;
}

Result run_nexus_files(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    long long mod = 0;
    if (!parse_id(ctx.args.get("--mod", "", ctx.mem), mod)) return make_usage_error("nexus files: --mod needs a numeric mod id", ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const mol::string domain = mol::nexus_game_domain(inst.cfg.game, ctx.mem);
    const auto files = make_client().mod_files(domain, mod, ctx.mem);
    NexusFilesData d{.game = domain, .mod_id = mod, .files = std::pmr::vector<NexusFileRow>(ctx.mem)};
    for (const auto& f : files)
        d.files.push_back(NexusFileRow{.file_id = f.file_id, .name = mol::string(f.name, ctx.mem), .file_name = mol::string(f.file_name, ctx.mem),
                                       .version = mol::string(f.version, ctx.mem), .category = mol::string(f.category, ctx.mem),
                                       .size_kb = f.size_kb, .is_primary = f.is_primary});
    Result r = ok_result(ctx);
    r.set_data(std::move(d));
    return r;
}

Result run_nexus_download(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const mol::string nxm_s = ctx.args.get("--nxm", "", ctx.mem);
    long long mod = 0, fid = 0;
    mol::NxmUrl nxm(ctx.mem);
    const bool has_nxm = !nxm_s.empty();
    if (has_nxm) {
        nxm = mol::parse_nxm(nxm_s, ctx.mem);
        mod = nxm.mod_id;
        fid = nxm.file_id;
    } else if (!parse_id(ctx.args.get("--mod", "", ctx.mem), mod) || !parse_id(ctx.args.get("--file", "", ctx.mem), fid)) {
        return make_usage_error("nexus download: give --nxm URL, or both --mod ID and --file ID", ctx);
    }
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const mol::string domain = has_nxm ? nxm.game : mol::nexus_game_domain(inst.cfg.game, ctx.mem);

    EventSink* sink = ctx.sink;
    if (sink != nullptr) sink->start("download");
    try {
        const mol::NexusClient client = make_client();
        const auto dl = mol::nexus_download(client, inst.downloads_dir, domain, mod, fid, has_nxm ? &nxm : nullptr,
                                            [&](std::uint64_t done, std::uint64_t total) {
                                                if (sink != nullptr && total > 0) sink->progress("download", done, total);
                                                return true;
                                            }, ctx.mem);
        const std::string dest(dl.path);
        const std::uint64_t size = dl.size;
        Result r = ok_result(ctx);
        r.set_data(NexusDownloadData{.path = mol::string(dest, ctx.mem), .size = size, .game = domain, .mod_id = mod, .file_id = fid});
        if (sink != nullptr) sink->done("download", true);
        return r;
    } catch (...) {
        if (sink != nullptr) sink->done("download", false);
        throw;
    }
}

Result run_skse_install(Context& ctx) {
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    // 游戏版本只能来自游戏层（host 库）：没有它就没法选对 SKSE。
    std::string version;
    {
        const auto host = mol::GameHost::open();
        const auto game = host.create(inst.cfg.game, inst.cfg.game_dir, inst.cfg.prefix, inst.cfg.prefix_user);
        alib6::AData info(ctx.mem);
        if (info.load_from_memory(game.info_json(ctx.mem)) && info.is_object()) {
            const auto& o = info.object();
            if (auto it = o.find("version"); it != o.end())
                if (auto v = it.second().try_to<std::string_view>()) version = std::string(*v);
        }
    }
    if (version.empty()) throw mol::Error("game_unavailable", "cannot determine the game version");

    EventSink* sink = ctx.sink;
    if (sink != nullptr) sink->start("skse install");
    try {
        const mol::SkseResult res = mol::install_skse(inst, version, make_client(), [&](std::uint64_t done, std::uint64_t total) {
            if (sink != nullptr && total > 0) sink->progress("download", done, total);
            return true;
        }, ctx.mem);
        Result r = ok_result(ctx);
        r.set_data(SkseInstallData{.game_version = mol::string(res.game_version, ctx.mem), .runtime_dll = mol::string(res.runtime_dll, ctx.mem),
                                   .installed = res.installed, .mod_name = mol::string(res.mod_name, ctx.mem),
                                   .file_name = mol::string(res.file_name, ctx.mem), .file_id = res.file_id, .downloaded = res.downloaded});
        if (sink != nullptr) sink->done("skse install", true);
        return r;
    } catch (...) {
        if (sink != nullptr) sink->done("skse install", false);
        throw;
    }
}

Result run_nexus_search(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    long long count = 10, offset = 0;
    if (!opt_int(ctx, "--count", 10, count) || !opt_int(ctx, "--offset", 0, offset)) return make_usage_error("nexus search: --count/--offset need non-negative integers", ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const mol::string domain = mol::nexus_game_domain(inst.cfg.game, ctx.mem);
    const mol::string sort = ctx.args.get("--sort", "relevance", ctx.mem);
    const mol::string query = ctx.args.positionals.front();
    std::int64_t total = 0;
    const auto found = make_client().search_mods(domain, query, sort, static_cast<int>(count), static_cast<int>(offset), &total, ctx.mem);
    const auto have = installed_nexus_ids(ctx, inst);
    NexusSearchData d{.game = domain, .query = query, .sort = sort, .total = total, .mods = std::pmr::vector<NexusModRow>(ctx.mem)};
    for (const auto& m : found) d.mods.push_back(to_row(m, have.count(m.mod_id) > 0, ctx.mem));
    Result r = ok_result(ctx);
    r.set_data(std::move(d));
    return r;
}

Result run_nexus_info(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    long long mod = 0;
    if (!parse_id(ctx.args.get("--mod", "", ctx.mem), mod)) return make_usage_error("nexus info: --mod needs a numeric mod id", ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const mol::string domain = mol::nexus_game_domain(inst.cfg.game, ctx.mem);
    const auto info = make_client().mod_info(domain, mod, ctx.mem);
    const auto have = installed_nexus_ids(ctx, inst);
    NexusInfoData d{.mod = to_row(info.summary, have.count(mod) > 0, ctx.mem), .category = std::pmr::string(info.category, ctx.mem),
                    .requirements = std::pmr::vector<NexusRequirementRow>(ctx.mem), .dlc_requirements = std::pmr::vector<std::pmr::string>(ctx.mem)};
    for (const auto& r : info.requirements)
        d.requirements.push_back(NexusRequirementRow{.mod_id = r.mod_id, .name = std::pmr::string(r.name, ctx.mem), .external = r.external,
                                                     .url = std::pmr::string(r.url, ctx.mem), .notes = std::pmr::string(r.notes, ctx.mem),
                                                     .installed = r.mod_id > 0 && have.count(r.mod_id) > 0});
    for (const auto& n : info.dlc_requirements) d.dlc_requirements.push_back(std::pmr::string(n, ctx.mem));
    Result r = ok_result(ctx);
    r.set_data(std::move(d));
    return r;
}

namespace {
// 安装一个 Nexus mod（含可选的递归前置）。结果追加到 out；返回是否该 mod 已处于「已安装」。
bool install_one(Context& ctx, const mol::Instance& inst, const mol::NexusClient& client, std::int64_t mod_id, std::int64_t file_id,
                 std::string_view name, bool with_req, bool top, int depth, std::set<std::int64_t>& seen, std::set<std::int64_t>& have,
                 NexusInstallData& out) {
    if (have.count(mod_id) > 0) {
        out.mods.push_back(NexusInstalledRow{mod_id, std::pmr::string(ctx.mem), std::pmr::string("already_installed", ctx.mem), std::pmr::string(ctx.mem), std::pmr::string(ctx.mem)});
        return true;
    }
    if (!seen.insert(mod_id).second) return false;
    const mol::string domain = mol::nexus_game_domain(inst.cfg.game, ctx.mem);

    // 先处理前置，保证它们的优先级低于本 mod（后装的优先级更高）
    if (with_req && depth < 4) {
        mol::NexusModInfo info = client.mod_info(domain, mod_id, ctx.mem);
        for (const auto& r : info.requirements) {
            if (r.external || r.mod_id <= 0) {
                out.external_requirements.push_back(NexusRequirementRow{0, std::pmr::string(r.name, ctx.mem), true, std::pmr::string(r.url, ctx.mem), std::pmr::string(r.notes, ctx.mem), false});
                continue;
            }
            install_one(ctx, inst, client, r.mod_id, 0, {}, true, false, depth + 1, seen, have, out);
        }
        for (const auto& n : info.dlc_requirements) out.dlc_requirements.push_back(std::pmr::string(n, ctx.mem));
    }

    NexusInstalledRow row{mod_id, std::pmr::string(ctx.mem), std::pmr::string(ctx.mem), std::pmr::string(ctx.mem), std::pmr::string(ctx.mem)};
    try {
        if (file_id == 0) {
            const auto files = client.mod_files(domain, mod_id, ctx.mem);
            const mol::NexusFile* f = pick_main_file(files);
            if (!f) throw mol::Error("mod_not_found", "no downloadable main file for Nexus mod " + std::to_string(mod_id));
            file_id = f->file_id;
        }
        const auto dl = mol::nexus_download(client, inst.downloads_dir, domain, mod_id, file_id, nullptr,
                                            [&](std::uint64_t d, std::uint64_t t) { if (ctx.sink != nullptr && t > 0) ctx.sink->progress("download", d, t); return true; }, ctx.mem);
        // 目录名：用户给的 > Nexus 上的 mod 名 > 文件名（install_archive 的默认）
        std::string dir_name(name);
        if (dir_name.empty()) {
            try { dir_name = std::string(client.mod_info(domain, mod_id, ctx.mem).summary.name); } catch (const mol::Error&) {}
        }
        mol::InstallOptions opt = top ? fomod_install_options(ctx, inst, dir_name) : [&] { mol::InstallOptions o; o.name = dir_name; o.profile = ctx.profile_override(); o.fomod = mol::FomodMode::Defaults; return o; }();
        const auto res = mol::install_archive(inst, dl.path, opt, ctx.mem);
        mol::set_mod_meta(res.path, "gameName", domain);
        mol::set_mod_meta(res.path, "modid", std::to_string(mod_id));
        mol::set_mod_meta(res.path, "fileid", std::to_string(file_id));
        try {   // 记下安装的文件版本，供 `mods outdated` 比较
            for (const auto& f : client.mod_files(domain, mod_id, ctx.mem))
                if (f.file_id == file_id && !f.version.empty()) { mol::set_mod_meta(res.path, "version", f.version); break; }
        } catch (const mol::Error&) {}
        row.status = "installed";
        row.mod_dir = std::pmr::string(res.name, ctx.mem);
        have.insert(mod_id);
        out.mods.push_back(std::move(row));
        return true;
    } catch (const mol::Error& e) {
        if (e.code == "fomod_choices_required") {
            row.status = "pending";
            row.note = "FOMOD needs choices";
            out.pending.push_back(CollectionPendingRow{std::pmr::string(std::to_string(mod_id), ctx.mem), std::pmr::string(ctx.mem), std::pmr::string("fomod_choices", ctx.mem),
                                                       std::pmr::string("run `fomod inspect <archive>` then re-run with --fomod FILE (or --fomod-defaults)", ctx.mem), std::pmr::string(e.path, ctx.mem)});
        } else if (e.code == "nexus_premium") {
            row.status = "pending";
            row.note = "needs a Premium account or an nxm:// link";
            out.pending.push_back(CollectionPendingRow{std::pmr::string(std::to_string(mod_id), ctx.mem), std::pmr::string(ctx.mem), std::pmr::string("manual_download", ctx.mem),
                                                       std::pmr::string("a free Nexus account cannot download this directly; use `nexus download --nxm LINK` then `mods install`", ctx.mem),
                                                       std::pmr::string("https://www.nexusmods.com/" + std::string(domain) + "/mods/" + std::to_string(mod_id) + "?tab=files", ctx.mem)});
        } else {
            row.status = "failed";
            row.note = std::pmr::string(std::string(e.code) + ": " + e.what(), ctx.mem);
        }
        out.mods.push_back(std::move(row));
        return false;
    }
}
}  // namespace

Result run_nexus_install(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    long long mod = 0, file = 0;
    if (!parse_id(ctx.args.get("--mod", "", ctx.mem), mod)) return make_usage_error("nexus install: --mod needs a numeric mod id", ctx);
    if (ctx.args.has("--file") && !parse_id(ctx.args.get("--file", "", ctx.mem), file)) return make_usage_error("nexus install: --file needs a numeric file id", ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const mol::NexusClient client = make_client();
    const mol::string name = ctx.args.get("--name", "", ctx.mem);
    auto have = installed_nexus_ids(ctx, inst);
    std::set<std::int64_t> seen;
    NexusInstallData d{.status = std::pmr::string(ctx.mem), .mods = std::pmr::vector<NexusInstalledRow>(ctx.mem), .pending = std::pmr::vector<CollectionPendingRow>(ctx.mem),
                       .external_requirements = std::pmr::vector<NexusRequirementRow>(ctx.mem), .dlc_requirements = std::pmr::vector<std::pmr::string>(ctx.mem)};
    EventSink* sink = ctx.sink;
    if (sink != nullptr) sink->start("nexus install");
    install_one(ctx, inst, client, mod, file, name, ctx.args.get_bool("--requirements", false), true, 0, seen, have, d);
    bool all_ok = d.pending.empty();
    for (const auto& m : d.mods) if (m.status == "failed") all_ok = false;
    d.status = std::pmr::string(all_ok ? "complete" : "incomplete", ctx.mem);
    if (sink != nullptr) sink->done("nexus install", all_ok);
    // 前置的名字：从站点取不到时留空，调用方用 mod_id 即可
    Result r = ok_result(ctx);
    r.exit_code = all_ok ? 0 : 4;
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli
