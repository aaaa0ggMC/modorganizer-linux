#include "mol/plugins_sync.hpp"

#include <filesystem>
#include <string>
#include <system_error>

import alib6;

namespace mol {
namespace {
namespace fs = std::filesystem;

[[noreturn]] void io_fail(const std::string& what, const fs::path& p, const std::error_code& ec) {
    throw Error("io_error", "plugins sync: " + what + ": " + ec.message(), p.string());
}
}  // namespace

SyncReport sync_plugins(const Instance& inst, const Game& game, mr* mem) {
    SyncReport rep(mem);
    rep.profile = inst.cfg.profile;
    const fs::path pdir = fs::path(std::string(inst.profiles_dir)) / std::string(inst.cfg.profile);
    std::error_code ec;
    if (!fs::is_directory(pdir, ec))
        throw Error("profile_not_found", "profile directory missing", pdir.string());

    game.set_profile(inst.cfg.profile, pdir.string(), inst.mods_dir, inst.overwrite_dir, inst.root);

    // profile 里还没有 plugins.txt：让上游复制游戏已有的（MODS|CONFIGURATION）。
    if (!fs::exists(pdir / "plugins.txt", ec)) {
        game.initialize_profile(pdir.string(), 1u | 2u);
        rep.initialized_profile = true;
        rep.changed = true;
    }

    alib6::AData doc(mem);
    const string json = game.mappings_json(mem);
    if (!doc.load_from_memory(json) || !doc.is_array())
        throw Error("game_unavailable", "mappings: invalid JSON");

    for (const auto& item : doc.array()) {
        if (!item.is_object()) continue;
        const auto& o = item.object();
        auto sit = o.find("source");
        auto dit = o.find("destination");
        if (sit == o.end() || dit == o.end()) continue;
        auto src = sit.second().try_to<std::string_view>();
        auto dst = dit.second().try_to<std::string_view>();
        if (!src || !dst) continue;

        SyncEntry e(mem);
        e.source = string(*src, mem);
        e.destination = string(*dst, mem);
        const fs::path s{std::string(*src)}, d{std::string(*dst)};

        if (!fs::exists(s, ec)) {
            e.action = "skip-missing-source";
            rep.entries.push_back(std::move(e));
            continue;
        }

        const auto st = fs::symlink_status(d, ec);
        if (fs::is_symlink(st)) {
            const fs::path cur = fs::read_symlink(d, ec);
            if (cur == s) {
                e.action = "ok";
                rep.entries.push_back(std::move(e));
                continue;
            }
            fs::remove(d, ec);
            if (ec) io_fail("remove stale link", d, ec);
            e.action = "relink";
        } else if (fs::exists(st)) {
            fs::path bak = d;
            bak += ".mol-backup";
            if (fs::exists(fs::symlink_status(bak, ec)))
                throw Error("io_error", "plugins sync: backup already exists, refusing to overwrite", bak.string());
            fs::rename(d, bak, ec);
            if (ec) io_fail("backup existing file", d, ec);
            e.action = "backup+link";
        } else {
            e.action = "link";
        }
        fs::create_directories(d.parent_path(), ec);
        if (ec) io_fail("create parent directory", d.parent_path(), ec);
        fs::create_symlink(s, d, ec);
        if (ec) io_fail("create symlink", d, ec);
        rep.changed = true;
        rep.entries.push_back(std::move(e));
    }
    return rep;
}

}  // namespace mol
