#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mol/overwrite.hpp"

namespace fs = std::filesystem;
using namespace mol;

namespace {
struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_ow_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
};
void put(const fs::path& p, const char* body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}
std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
Instance make(const Tmp& t) {
    Instance i;
    i.root.assign((t.dir / "inst").string());
    i.overwrite_dir.assign((t.dir / "inst/overwrite").string());
    i.farm_path.assign((t.dir / "inst/farm").string());
    fs::create_directories(t.dir / "inst/farm");
    fs::create_directories(t.dir / "inst/overwrite");
    return i;
}
}  // namespace

TEST(capture_moves_real_files_and_keeps_links) {
    Tmp t;
    const Instance i = make(t);
    put(t.dir / "src/linked.txt", "L");
    fs::create_directories(t.dir / "inst/farm/Data/Meshes");
    fs::create_symlink(t.dir / "src/linked.txt", t.dir / "inst/farm/Data/linked.txt");
    put(t.dir / "inst/farm/.mol-farm.json", "{}");
    put(t.dir / "inst/farm/Data/Meshes/new.nif", "N");   // 游戏新建
    put(t.dir / "inst/farm/data/Other/x.esp", "E");      // 大小写不同的 Data
    put(t.dir / "inst/farm/crash.log", "C");             // Data 之外
    CHECK_EQ(capture_overwrite(i), std::size_t{3});
    CHECK_EQ(slurp(t.dir / "inst/overwrite/Meshes/new.nif"), std::string("N"));
    CHECK_EQ(slurp(t.dir / "inst/overwrite/Other/x.esp"), std::string("E"));
    CHECK_EQ(slurp(t.dir / "inst/overwrite-root/crash.log"), std::string("C"));
    CHECK(fs::is_symlink(t.dir / "inst/farm/Data/linked.txt"));
    CHECK(fs::exists(t.dir / "inst/farm/.mol-farm.json"));
    CHECK(!fs::exists(t.dir / "inst/farm/Data/Meshes"));  // 变空的目录被清理
    CHECK_EQ(capture_overwrite(i), std::size_t{0});        // 幂等
}

TEST(capture_backs_up_instead_of_clobbering) {
    Tmp t;
    const Instance i = make(t);
    put(t.dir / "inst/overwrite/a.txt", "OLD");
    put(t.dir / "inst/farm/Data/a.txt", "NEW");
    CHECK_EQ(capture_overwrite(i), std::size_t{1});
    CHECK_EQ(slurp(t.dir / "inst/overwrite/a.txt"), std::string("NEW"));
    CHECK_EQ(slurp(t.dir / "inst/overwrite-backup/overwrite/a.txt"), std::string("OLD"));
}

TEST(farm_in_use_detects_process_with_cwd_in_farm) {
    Tmp t;
    const Instance i = make(t);
    CHECK(!farm_in_use(i));
    const pid_t pid = ::fork();
    if (pid == 0) {
        if (::chdir((t.dir / "inst/farm").c_str()) != 0) _exit(1);
        ::execlp("sleep", "sleep", "30", static_cast<char*>(nullptr));
        _exit(127);
    }
    bool busy = false;
    for (int n = 0; n < 50 && !busy; ++n) {
        ::usleep(20000);
        busy = farm_in_use(i);
    }
    CHECK(busy);
    bool threw = false;
    try { require_farm_idle(i); } catch (const Error& e) { threw = (e.code == "farm_busy"); }
    CHECK(threw);
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    CHECK(!farm_in_use(i));
}
