#include "mol/casefold.hpp"
#include "mol/plugins_sync.hpp"

#include <filesystem>
#include <fstream>
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

namespace {
std::string find_ci_name(const fs::path& dir, std::string_view want) {
    std::error_code ec;
    const auto w = casefold(want);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (casefold(it->path().filename().string()) == w) return it->path().filename().string();
    return {};
}
bool ini_true(const fs::path& settings, std::string_view key) {
    std::ifstream in(settings);
    std::string line;
    bool in_general = false;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line.front() == '[') { in_general = casefold(line) == "[general]"; continue; }
        if (!in_general) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos || casefold(std::string_view(line).substr(0, eq)) != casefold(key)) continue;
        const auto v = casefold(std::string_view(line).substr(eq + 1));
        return v == "true" || v == "1";
    }
    return false;
}
// 同一目录里只有大小写不同的同名文件（例：游戏原版运行时建的 Plugins.txt，而映射目标是 plugins.txt）。
// Linux 上两者并存，但 Wine 打开时**优先用大小写完全一致的那个**——游戏读到的是旧文件，我们的链接形同虚设。
// 建链接前把这些「影子」改名成 <原名>.mol-backup（已有备份则拒绝，与主路径一致）。返回处理的个数。
}  // namespace

std::size_t backup_case_variants(const fs::path& dst) {
    std::error_code ec;
    std::size_t n = 0;
    const auto want = casefold(dst.filename().string());
    std::vector<fs::path> shadows;
    for (fs::directory_iterator it(dst.parent_path(), ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename() != dst.filename() && casefold(it->path().filename().string()) == want) shadows.push_back(it->path());
    for (const auto& p : shadows) {
        if (fs::is_symlink(fs::symlink_status(p, ec))) { fs::remove(p, ec); ++n; continue; }  // 我们以前建的、大小写不同的链接
        fs::path bak = p;
        bak += ".mol-backup";
        if (fs::exists(fs::symlink_status(bak, ec))) throw Error("io_error", "plugins sync: backup already exists, refusing to overwrite", bak.string());
        fs::rename(p, bak, ec);
        if (ec) io_fail("backup existing file", p, ec);
        ++n;
    }
    return n;
}

void sync_profile_settings(const Instance& inst, SyncReport& rep, mr* mem) {
    std::error_code ec;
    const fs::path pdir = fs::path(std::string(inst.profiles_dir)) / std::string(inst.cfg.profile);
    const fs::path settings = pdir / "settings.ini";
    const fs::path docs = fs::path(std::string(inst.cfg.prefix)) / "drive_c/users" / std::string(inst.cfg.prefix_user) / "Documents/My Games/Skyrim Special Edition";
    auto link_one = [&](const fs::path& src, const fs::path& dst, bool is_dir) {
        SyncEntry e(mem);
        e.source = string(src.string(), mem);
        e.destination = string(dst.string(), mem);
        const auto st = fs::symlink_status(dst, ec);
        if (fs::is_symlink(st)) {
            if (fs::read_symlink(dst, ec) == src) { e.action = "ok"; rep.entries.push_back(std::move(e)); return; }
            fs::remove(dst, ec);
            e.action = "relink";
        } else if (fs::exists(st)) {
            if (is_dir) {
                std::error_code e2;
                if (!fs::is_empty(dst, e2)) { e.action = "skip-existing-directory"; rep.entries.push_back(std::move(e)); return; }  // 绝不碰有内容的真实存档目录
                fs::remove(dst, ec);
                e.action = "link";
            } else {
                fs::path bak = dst;
                bak += ".mol-backup";
                if (fs::exists(fs::symlink_status(bak, ec))) throw Error("io_error", "plugins sync: backup already exists, refusing to overwrite", bak.string());
                fs::rename(dst, bak, ec);
                if (ec) io_fail("backup existing file", dst, ec);
                e.action = "backup+link";
            }
        } else {
            e.action = "link";
        }
        fs::create_directories(dst.parent_path(), ec);
        fs::create_symlink(src, dst, ec);
        if (ec) io_fail("create symlink", dst, ec);
        rep.changed = true;
        rep.entries.push_back(std::move(e));
    };
    if (ini_true(settings, "LocalSettings")) {
        for (const char* nm : {"Skyrim.ini", "SkyrimPrefs.ini", "SkyrimCustom.ini"}) {
            const std::string have = find_ci_name(pdir, nm);
            if (have.empty()) continue;
            // 目标沿用前缀里已有的大小写写法（游戏自己创建的是 Skyrim.ini / SkyrimPrefs.ini）
            std::string target = find_ci_name(docs, nm);
            if (target.empty()) target = nm;
            link_one(pdir / have, docs / target, false);
        }
    }
    if (ini_true(settings, "LocalSaves") && fs::is_directory(pdir / "saves", ec)) link_one(pdir / "saves", docs / "Saves", true);
}

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
        if (backup_case_variants(d) > 0) rep.changed = true;

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
    sync_profile_settings(inst, rep, mem);
    return rep;
}

}  // namespace mol
