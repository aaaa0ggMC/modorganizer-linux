// plugins sync：把游戏层 mappings() 物化为符号链接（见 mol/plugins_sync.hpp）。会写 profile 与前缀 AppData，不启动游戏。
#include "mol/game_host.hpp"
#include "mol/instance.hpp"
#include "mol/plugins_sync.hpp"

#include "commands.hpp"

namespace cli {

Result run_plugins_sync(Context& ctx) {
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const auto host = mol::GameHost::open();  // 先于 Game 声明，后销毁
    const auto game = host.create(inst.cfg.game, inst.cfg.game_dir, inst.cfg.prefix, inst.cfg.prefix_user);
    const auto rep = mol::sync_plugins(inst, game, ctx.mem);

    PluginsSyncData d{.profile = std::pmr::string(rep.profile, ctx.mem),
                      .initialized_profile = rep.initialized_profile,
                      .changed = rep.changed,
                      .entries = std::pmr::vector<SyncRow>(ctx.mem)};
    for (const auto& e : rep.entries)
        d.entries.push_back(SyncRow{std::pmr::string(e.source, ctx.mem), std::pmr::string(e.destination, ctx.mem),
                                    std::pmr::string(e.action, ctx.mem)});
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli
