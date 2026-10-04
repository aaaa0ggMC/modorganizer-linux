// 向游戏层询问 info 里的字符串字段（取不到返回空串：host 库缺失只是降级）。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include "mol/game_host.hpp"
#include "mol/instance.hpp"

#include "cmd_common.hpp"

import alib6;
import std;

namespace cli {

std::string game_info_string(Context& ctx, const mol::Instance& inst, std::string_view key, std::string_view sub_key) {
    try {
        const auto host = mol::GameHost::open();
        const auto game = host.create(inst.cfg.game, inst.cfg.game_dir, inst.cfg.prefix, inst.cfg.prefix_user);
        alib6::AData info(ctx.mem);
        if (!info.load_from_memory(game.info_json(ctx.mem)) || !info.is_object()) return {};
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
    } catch (const mol::Error&) {
        return {};
    }
}

std::vector<std::string> game_info_list(Context& ctx, const mol::Instance& inst, std::string_view key) {
    std::vector<std::string> out;
    try {
        const auto host = mol::GameHost::open();
        const auto game = host.create(inst.cfg.game, inst.cfg.game_dir, inst.cfg.prefix, inst.cfg.prefix_user);
        alib6::AData info(ctx.mem);
        if (!info.load_from_memory(game.info_json(ctx.mem)) || !info.is_object()) return out;
        auto it = info.object().find(key);
        if (it == info.object().end() || !it.second().is_array()) return out;
        for (const auto& v : it.second().array())
            if (auto s = v.try_to<std::string_view>()) out.emplace_back(*s);
    } catch (const mol::Error&) {
    }
    return out;
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
