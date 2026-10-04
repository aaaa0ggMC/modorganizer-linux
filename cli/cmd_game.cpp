// game info 只读取实例与游戏层信息，不初始化 profile、不启动游戏。
#include "mol/game_host.hpp"
#include "mol/instance.hpp"

#include "cmd_common.hpp"

namespace cli {

Result run_game_info(Context& ctx) {
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    // Game 持非拥有的 host 引用：按此顺序声明以确保先销毁 Game。
    const auto host = mol::GameHost::open();
    const auto game = host.create(inst.cfg.game, inst.cfg.game_dir, inst.cfg.prefix,
                                  inst.cfg.prefix_user);
    const auto info = game.info_json(ctx.mem);
    Result result(ctx.mem);
    result.command = ctx.command;
    if (!result.data.load_from_memory(info) || !result.data.is_object()) {
        throw mol::Error("game_unavailable", "game host returned invalid info JSON");
    }
    return result;
}

}  // namespace cli
