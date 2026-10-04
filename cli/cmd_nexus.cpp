// nexus login/logout/whoami/files/download。API key 永远不进输出/日志；下载文件落到实例的 downloads/。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <iostream>
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

}  // namespace cli
