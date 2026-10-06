// 向游戏层询问 info 里的字符串字段（取不到返回空串：host 库缺失只是降级）。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include "mol/game_host.hpp"
#include "mol/instance.hpp"
#include "mol/nexus.hpp"

#include "cmd_common.hpp"

import alib6;
import std;

namespace cli {

namespace {

// 进程内缓存游戏层的 info_json：dlopen 上游游戏插件（带 Qt）+ create 很贵，而一条命令里常要取好几个字段
// （版本、primary/dlc/cc 插件……）。键 = 会影响结果的实例配置。取不到也缓存（host 缺失时不必反复 dlopen 失败）。
struct InfoCache {
    std::string key;
    bool have = false;
    std::optional<std::string> json;
};
InfoCache& info_cache() {
    static InfoCache c;
    return c;
}

const std::optional<std::string>& game_info_json(Context& ctx, const mol::Instance& inst) {
    auto& c = info_cache();
    std::string key = std::string(inst.cfg.game) + '\n' + std::string(inst.cfg.game_dir) + '\n' + std::string(inst.cfg.prefix) + '\n' +
                      std::string(inst.cfg.prefix_user);
    if (c.have && c.key == key) return c.json;
    c.key = std::move(key);
    c.have = true;
    c.json.reset();
    try {
        const auto host = mol::GameHost::open();
        const auto game = host.create(inst.cfg.game, inst.cfg.game_dir, inst.cfg.prefix, inst.cfg.prefix_user);
        c.json = std::string(game.info_json(ctx.mem));
    } catch (const mol::Error&) {
    }
    return c.json;
}

}  // namespace

std::string game_info_string(Context& ctx, const mol::Instance& inst, std::string_view key, std::string_view sub_key) {
    const auto& js = game_info_json(ctx, inst);
    if (!js) return {};
    alib6::AData info(ctx.mem);
    if (!info.load_from_memory(*js) || !info.is_object()) return {};
    const alib6::AData* node = &info;
    auto it = info.object().find(key);
    if (it == info.object().end()) return {};
    node = &it.second();
    if (!sub_key.empty()) {
        if (!node->is_object()) return {};
        auto jt = node->object().find(sub_key);
        if (jt == node->object().end()) return {};
        node = &jt.second();
    }
    auto v = node->try_to<std::string_view>();
    return v ? std::string(*v) : std::string();
}

std::vector<std::string> game_info_list(Context& ctx, const mol::Instance& inst, std::string_view key) {
    std::vector<std::string> out;
    const auto& js = game_info_json(ctx, inst);
    if (!js) return out;
    alib6::AData info(ctx.mem);
    if (!info.load_from_memory(*js) || !info.is_object()) return out;
    auto it = info.object().find(key);
    if (it == info.object().end() || !it.second().is_array()) return out;
    for (const auto& v : it.second().array())
        if (auto s = v.try_to<std::string_view>()) out.emplace_back(*s);
    return out;
}

mol::string search_domain(Context& ctx) {
    try {
        const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
        return mol::nexus_game_domain(inst.cfg.game, ctx.mem);
    } catch (const mol::Error& e) {
        if (e.code != "instance_not_found") throw;
    }
    return mol::nexus_game_domain("skyrimse", ctx.mem);
}

std::vector<std::string> forced_plugin_names(Context& ctx, const mol::Instance& inst) {
    std::vector<std::string> out;
    auto lower = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
    for (const char* k : {"primaryPlugins", "dlcPlugins", "ccPlugins"})
        for (auto& n : game_info_list(ctx, inst, k)) {
            bool dup = false;
            for (const auto& e : out) if (lower(e) == lower(n)) { dup = true; break; }
            if (!dup) out.push_back(std::move(n));
        }
    return out;
}

}  // namespace cli
