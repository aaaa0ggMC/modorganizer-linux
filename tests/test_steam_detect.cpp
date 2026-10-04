#include <unistd.h>
#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mol/steam_detect.hpp"

using namespace mol;
namespace fs = std::filesystem;

static void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}

TEST(detect_steam_finds_game_prefix_and_newest_proton) {
    const fs::path home = fs::temp_directory_path() / ("mol-steam-detect-" + std::to_string(::getpid()));
    fs::remove_all(home);
    const fs::path steam = home / ".local/share/Steam";
    const fs::path lib2 = home / "lib2";
    fs::create_directories(steam / "steamapps");
    fs::create_directories(lib2 / "steamapps/common/Skyrim Special Edition");
    fs::create_directories(lib2 / "steamapps/compatdata/489830/pfx");
    touch(steam / "steamapps/common/Proton 8.0/proton");
    touch(steam / "steamapps/common/Proton 9.0 (Beta)/proton");
    touch(steam / "steamapps/common/Proton - Experimental/proton");
    std::ofstream(steam / "steamapps/libraryfolders.vdf") << "\"libraryfolders\"\n{\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" << lib2.string() << "\"\n\t}\n}\n";

    const auto d = detect_steam(home.string());
    CHECK_EQ(std::string(d.steam_root), steam.string());
    CHECK_EQ(std::string(d.game_dir), (lib2 / "steamapps/common/Skyrim Special Edition").string());
    CHECK_EQ(std::string(d.prefix), (lib2 / "steamapps/compatdata/489830/pfx").string());
    CHECK_EQ(std::string(d.proton_path), (steam / "steamapps/common/Proton 9.0 (Beta)").string());
    fs::remove_all(home);
}

TEST(detect_steam_without_steam_is_empty) {
    const fs::path home = fs::temp_directory_path() / ("mol-steam-none-" + std::to_string(::getpid()));
    fs::create_directories(home);
    const auto d = detect_steam(home.string());
    CHECK(d.steam_root.empty() && d.game_dir.empty() && d.prefix.empty() && d.proton_path.empty());
    fs::remove_all(home);
}
