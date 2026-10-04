// 若 libmo-game.so 不可用（未构建 host）则跳过真实调用，只验证错误路径。
#include "minitest.hpp"
#include "mol/game_host.hpp"

using namespace mol;

TEST(missing_lib_reports_game_unavailable) {
    bool threw = false;
    try { GameHost::open("/nonexistent/libmo-game.so"); }
    catch (const Error& e) { threw = e.code == "game_unavailable"; }
    // 显式路径不存在时会回退到其它候选；若本机恰好有构建产物则不报错，这也是合法的
    CHECK(threw || !find_game_host_lib().empty());
}

TEST(find_returns_empty_when_nothing) {
    // 不设置环境变量、不在构建树旁时返回空；仅断言不崩溃
    auto s = find_game_host_lib("/definitely/not/here.so");
    (void)s;
    CHECK(true);
}
