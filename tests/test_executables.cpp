#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mol/executables.hpp"

namespace fs = std::filesystem;
using namespace mol;

namespace {
struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_ex_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
};
void put(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}
}  // namespace

TEST(split_arguments_shell_rules) {
    const auto a = split_arguments(R"(-a "two words" 'single quoted' plain\ x "esc \" quote" "")");
    CHECK_EQ(a.size(), std::size_t{7});
    CHECK_EQ(std::string(a[0]), std::string("-a"));
    CHECK_EQ(std::string(a[1]), std::string("two words"));
    CHECK_EQ(std::string(a[2]), std::string("single quoted"));
    CHECK_EQ(std::string(a[3]), std::string("plain\\"));  // 反斜杠在引号外不做转义（MO2 的参数里大多是 Windows 路径），所以 "plain\\ x" 是两个词
    CHECK_EQ(std::string(a[4]), std::string("x"));
    CHECK_EQ(std::string(a[5]), std::string("esc \" quote"));
    CHECK_EQ(std::string(a[6]), std::string(""));  // 空引号 = 一个空参数
    CHECK(split_arguments("").empty());
}

TEST(list_executables_resolves_paths_and_farm_locations) {
    Tmp t;
    Instance inst;
    inst.root.assign((t.dir / "inst").string());
    inst.mods_dir.assign((t.dir / "inst/mods").string());
    inst.profiles_dir.assign((t.dir / "inst/profiles").string());
    inst.cfg.game_dir.assign((t.dir / "game").string());
    inst.cfg.prefix.assign((t.dir / "pfx").string());
    inst.cfg.profile.assign("Default");
    fs::create_directories(t.dir / "inst/profiles/Default");
    fs::create_directories(t.dir / "game");
    put(t.dir / "inst/profiles/Default/modlist.txt", "+Tool\n+Root\n");
    put(t.dir / "inst/mods/Tool/tools/xEdit/xEdit.exe", "x");
    put(t.dir / "inst/mods/Root/loader.exe", "x");
    put(t.dir / "inst/mods/Root/meta.ini", "[General]\nmol_root=true\n");
    put(t.dir / "game/SkyrimSE.exe", "x");
    std::string g = "Z:" + std::string(inst.cfg.game_dir);
    for (char& c : g) if (c == '/') c = '\\';
    put(t.dir / "inst/ModOrganizer.ini",
        "[customExecutables]\n"
        "1\\title=SKSE\n"
        "1\\binary=@ByteArray(" + g + "\\SkyrimSE.exe)\n"
        "1\\arguments=-flag \"a b\"\n"
        "1\\hide=false\n"
        "2\\title=xEdit\n"
        "2\\binary=%BASE_DIR%/mods/Tool/tools/xEdit/xEdit.exe\n"
        "2\\workingDirectory=%BASE_DIR%/mods/Tool/tools/xEdit\n"
        "3\\title=RootTool\n"
        "3\\binary=%BASE_DIR%/mods/Root/loader.exe\n"
        "4\\title=Elsewhere\n"
        "4\\binary=%BASE_DIR%/tools/thing.exe\n"
        "5\\title=\n5\\binary=\n"
        "size=5\n");
    const auto ex = list_executables(inst);
    CHECK_EQ(ex.size(), std::size_t{4});  // 第 5 条标题/路径为空，被跳过
    CHECK_EQ(std::string(ex[0].title), std::string("SKSE"));
    CHECK_EQ(std::string(ex[0].binary), (t.dir / "game/SkyrimSE.exe").string());
    CHECK_EQ(std::string(ex[0].farm_path), std::string("SkyrimSE.exe"));          // 游戏目录下 → 农场根相对
    CHECK_EQ(std::string(ex[0].arguments), std::string("-flag \"a b\""));
    CHECK_EQ(std::string(ex[1].farm_path), std::string("Data/tools/xEdit/xEdit.exe"));  // 普通 mod → Data/ 下
    CHECK_EQ(std::string(ex[1].working_dir), (t.dir / "inst/mods/Tool/tools/xEdit").string());
    CHECK_EQ(std::string(ex[2].farm_path), std::string("loader.exe"));            // 根目录型 mod → 农场根
    CHECK(ex[3].farm_path.empty());                                               // 农场之外 → 用绝对路径
    CHECK_EQ(std::string(ex[3].binary), (t.dir / "inst/tools/thing.exe").string());
}
