#pragma once
// libmo-game.so 的 dlopen 封装：core/CLI 不在链接期依赖 Qt 或 clang 产物。
// .so 的位置：显式路径 > 环境变量 MOL_GAME_LIB > <exe目录>/libmo-game.so > <exe目录>/../lib/libmo-game.so
//            > <exe目录>/host/libmo-game.so（开发构建树）。
#include <memory>
#include <string_view>

#include "mol/error.hpp"
#include "mol/pmr.hpp"

namespace mol {

namespace detail {
struct HostImpl;
struct GameImpl;
}  // namespace detail

// 查找 .so，找不到返回空串。
string find_game_host_lib(std::string_view explicit_path = {}, mr* mem = default_mr());

class GameHost;

// 一个已创建的游戏适配器（对应 mo_game*）。注意：创建它的 GameHost 必须比它活得更久（内部持非拥有引用）。
class Game {
public:
    ~Game();
    Game(Game&&) noexcept;
    Game& operator=(Game&&) noexcept;
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
    // mo_game_info_json 的结果（UTF-8 JSON 文本）。
    string info_json(mr* mem = default_mr()) const;
    // 设置当前 profile/实例路径（mappings、initialize_profile 之前必须调用）。
    void set_profile(std::string_view name, std::string_view profile_dir, std::string_view mods_dir,
                     std::string_view overwrite_dir, std::string_view base_dir) const;
    // 上游 IPluginFileMapper::mappings() 的 JSON 数组。
    string mappings_json(mr* mem = default_mr()) const;
    // flags: 1=MODS 2=CONFIGURATION 4=SAVEGAMES 8=PREFER_DEFAULTS。失败 → Error{game_unavailable}。
    void initialize_profile(std::string_view dir, unsigned flags) const;
    // 触发 onAboutToRun 回调；返回是否全部放行。
    bool about_to_run(std::string_view binary) const;

private:
    friend class GameHost;
    explicit Game(std::unique_ptr<detail::GameImpl> i);
    std::unique_ptr<detail::GameImpl> impl_;
};

class GameHost {
public:
    // 加载失败 → Error{game_unavailable}（message 含 dlerror 文本与已尝试的路径）。
    static GameHost open(std::string_view lib_path = {});
    ~GameHost();
    GameHost(GameHost&&) noexcept;
    GameHost& operator=(GameHost&&) noexcept;
    GameHost(const GameHost&) = delete;
    GameHost& operator=(const GameHost&) = delete;

    // 创建失败 → Error{game_unavailable}（message 为 .so 返回的错误信息）。
    Game create(std::string_view game_id, std::string_view game_dir, std::string_view wine_prefix,
                std::string_view wine_user = "steamuser") const;

private:
    explicit GameHost(std::unique_ptr<detail::HostImpl> i);
    std::unique_ptr<detail::HostImpl> impl_;
};

}  // namespace mol
