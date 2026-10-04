#include <algorithm>

#include "minitest.hpp"
#include "mol/runner.hpp"

using namespace mol;

static Instance make(const char* runner) {
    Instance i;
    i.root.assign("/inst");
    i.mods_dir.assign("/inst/mods");
    i.overwrite_dir.assign("/inst/overwrite");
    i.farm_path.assign("/inst/farm");
    i.cfg.game_dir.assign("/steam/Skyrim");
    i.cfg.prefix.assign("/steam/compatdata/489830/pfx");
    i.cfg.steam_root.assign("/srv/steam-root");
    i.cfg.proton_path.assign("/steam/Proton 9.0");
    i.cfg.runner_kind.assign(runner);
    return i;
}
static std::string env_of(const LaunchSpec& s, const char* k) {
    for (auto& [a, b] : s.env) if (a == k) return std::string(b);
    return "<unset>";
}

TEST(proton_spec) {
    auto s = build_launch(make("proton"), "skse64_loader.exe");
    CHECK_EQ(s.argv.size(), std::size_t(3));
    CHECK_EQ(std::string(s.argv[0]), std::string("/steam/Proton 9.0/proton"));
    CHECK_EQ(std::string(s.argv[1]), std::string("run"));
    CHECK_EQ(std::string(s.argv[2]), std::string("/inst/farm/skse64_loader.exe"));
    CHECK_EQ(std::string(s.cwd), std::string("/inst/farm"));
    CHECK_EQ(env_of(s, "STEAM_COMPAT_DATA_PATH"), std::string("/steam/compatdata/489830"));  // 去掉 /pfx
    CHECK_EQ(env_of(s, "STEAM_COMPAT_CLIENT_INSTALL_PATH"), std::string("/srv/steam-root"));
    CHECK_EQ(env_of(s, "SteamAppId"), std::string("489830"));
    CHECK_EQ(env_of(s, "STEAM_COMPAT_MOUNTS"),
             std::string("/inst/farm:/inst:/inst/mods:/inst/overwrite:/steam/Skyrim"));
    CHECK_EQ(env_of(s, "PRESSURE_VESSEL_FILESYSTEMS_RW"), env_of(s, "STEAM_COMPAT_MOUNTS"));
}

TEST(proton_args_and_prefix_without_pfx) {
    auto i = make("proton");
    i.cfg.prefix.assign("/steam/compatdata/489830");
    std::string_view a[] = {"-windowed", "x"};
    LaunchOptions o;
    o.args = a;
    auto s = build_launch(i, "SkyrimSE.exe", o);
    CHECK_EQ(s.argv.size(), std::size_t(5));
    CHECK_EQ(std::string(s.argv[3]), std::string("-windowed"));
    CHECK_EQ(env_of(s, "STEAM_COMPAT_DATA_PATH"), std::string("/steam/compatdata/489830"));
}

TEST(wine_spec) {
    auto s = build_launch(make("wine"), "SkyrimSE.exe");
    CHECK_EQ(std::string(s.argv[0]), std::string("wine"));
    CHECK_EQ(std::string(s.argv[1]), std::string("/inst/farm/SkyrimSE.exe"));
    CHECK_EQ(env_of(s, "WINEPREFIX"), std::string("/steam/compatdata/489830/pfx"));
}

TEST(errors) {
    auto i = make("proton");
    i.cfg.proton_path.clear();
    bool threw = false;
    try { build_launch(i, "x.exe"); } catch (const Error& e) { threw = e.code == "config_invalid"; }
    CHECK(threw);
    threw = false;
    try { build_launch(make("bogus"), "x.exe"); } catch (const Error& e) { threw = e.code == "config_invalid"; }
    CHECK(threw);
}

TEST(spawn_wait_and_env) {
    LaunchSpec s;
    s.argv = {string("sh"), string("-c"), string("test \"$MOL_T\" = ok && exit 7")};
    s.env.emplace_back(string("MOL_T"), string("ok"));
    s.cwd.assign("/tmp");
    CHECK_EQ(spawn_launch(s, true), 7);
    LaunchSpec bad;
    bad.argv = {string("/nonexistent/binary")};
    CHECK_EQ(spawn_launch(bad, true), 127);
}
