// overview：GUI 概览页需要的一切，一次取齐（只读）。同一进程里 doctor 与游戏层各只跑一次，
// 代替 GUI 先后调用 version / instance show / doctor / next / mods list / plugins list 的多进程往返。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <filesystem>

#include "mol/collection.hpp"
#include "mol/doctor.hpp"
#include "mol/instance.hpp"
#include "mol/nexus.hpp"
#include "mol/plugins.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {

Result run_overview(Context& ctx) {
    namespace fs = std::filesystem;
    OverviewData d{.version = std::pmr::string(kToolVersion, ctx.mem),
                   .instance = std::pmr::string(ctx.instance_dir, ctx.mem),
                   .profile = std::pmr::string(ctx.mem),
                   .game_dir = std::pmr::string(ctx.mem),
                   .game_version = std::pmr::string(ctx.mem),
                   .farm_path = std::pmr::string(ctx.mem),
                   .collections = std::pmr::vector<OverviewCollectionRow>(ctx.mem),
                   .doctor = DoctorData{.checks = std::pmr::vector<CheckRow>(ctx.mem)},
                   .next = NextData{.instance = std::pmr::string(ctx.mem), .steps = std::pmr::vector<NextStep>(ctx.mem)}};
    d.nexus_key = mol::load_nexus_key(ctx.mem).has_value();

    std::optional<mol::Instance> inst;
    try {
        inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    } catch (const mol::Error& e) {
        if (e.code != "instance_not_found") throw;
    }
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    if (!inst) {
        d.next = next_data(ctx, nullptr, nullptr);
        r.set_data(std::move(d));
        return r;
    }

    d.has_instance = true;
    d.instance = std::pmr::string(inst->root, ctx.mem);
    d.profile = std::pmr::string(inst->cfg.profile, ctx.mem);
    d.game_dir = std::pmr::string(inst->cfg.game_dir, ctx.mem);
    d.farm_path = std::pmr::string(inst->farm_path, ctx.mem);
    d.game_version = std::pmr::string(game_info_string(ctx, *inst, "version"), ctx.mem);

    const auto checks = mol::run_doctor(*inst, d.game_version, ctx.mem);
    d.doctor = doctor_data(ctx, checks);
    d.next = next_data(ctx, &*inst, &checks);

    for (const auto& m : mol::list_mods(*inst, ctx.profile_override(), ctx.mem)) {
        if (m.separator) continue;
        ++d.mods_total;
        if (m.enabled) {
            ++d.mods_enabled;
            if (!m.exists) ++d.mods_missing;
        }
    }
    try {
        std::vector<mol::string> forced;
        for (const auto& n : forced_plugin_names(ctx, *inst)) forced.emplace_back(n, ctx.mem);
        const auto pl = mol::load_plugins(*inst, forced, ctx.profile_override(), ctx.mem);
        d.plugins_total = pl.rows.size();
        for (const auto& p : pl.rows) d.plugins_enabled += p.enabled ? 1 : 0;
    } catch (const mol::Error&) {
        // game_dir 不存在等：doctor 里已经报了
    }

    std::error_code ec;
    const fs::path cdir = fs::path(std::string(inst->root)) / "collections";
    std::vector<std::string> slugs;
    for (fs::directory_iterator it(cdir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) slugs.push_back(it->path().filename().string());
    std::sort(slugs.begin(), slugs.end());
    for (const auto& slug : slugs) {
        mol::collection::State st;
        try { st = mol::collection::load_state(*inst, slug); } catch (const mol::Error&) { continue; }
        OverviewCollectionRow row{.slug = std::pmr::string(slug, ctx.mem), .name = std::pmr::string(st.name, ctx.mem), .revision = st.revision};
        for (const auto& [k, m] : st.mods) {
            if (m.status == "installed") ++row.installed;
            else if (m.status == "pending") ++row.pending;
            else if (m.status == "failed") ++row.failed;
            else if (m.status == "skipped") ++row.skipped;
        }
        d.collections.push_back(std::move(row));
    }
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli
