#pragma once
// 探测本机 Steam 的 Skyrim SE 安装：Steam 根、游戏目录、Proton 前缀、Proton 版本。
// 只读文件系统，找不到的字段为空串。
#include "mol/pmr.hpp"

namespace mol {

struct SteamDetect {
    using allocator_type = mol::allocator_type;
    string steam_root;
    string game_dir;
    string prefix;  // <库>/steamapps/compatdata/<appid>/pfx
    string proton_path;
    explicit SteamDetect(allocator_type a = {}) : steam_root(a), game_dir(a), prefix(a), proton_path(a) {}
    SteamDetect(const SteamDetect&) = default;
    SteamDetect(SteamDetect&&) = default;
    SteamDetect& operator=(const SteamDetect&) = default;
    SteamDetect& operator=(SteamDetect&&) = default;
    SteamDetect(const SteamDetect& o, allocator_type a) : steam_root(o.steam_root, a), game_dir(o.game_dir, a), prefix(o.prefix, a), proton_path(o.proton_path, a) {}
    SteamDetect(SteamDetect&& o, allocator_type a) : steam_root(std::move(o.steam_root), a), game_dir(std::move(o.game_dir), a), prefix(std::move(o.prefix), a), proton_path(std::move(o.proton_path), a) {}
};

// home：用户主目录（空 → $HOME）。便于测试注入假目录。
SteamDetect detect_steam(std::string_view home = {}, mr* mem = default_mr());

}  // namespace mol
