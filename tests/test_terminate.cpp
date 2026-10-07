#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "minitest.hpp"
#include "mol/terminate.hpp"

namespace fs = std::filesystem;
using namespace mol;

namespace {
struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_term_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir / "farm/Data");
        fs::create_directories(dir / "pfx/drive_c");
        fs::create_directories(dir / "elsewhere");
    }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
};
// 在 cwd 里起一个子进程（execvp argv），可选附加一个环境变量
pid_t spawn(const fs::path& cwd, std::vector<std::string> argv, const char* env = nullptr) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        if (::chdir(cwd.c_str()) != 0) ::_exit(126);
        if (env) ::putenv(const_cast<char*>(env));
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(s.data());
        a.push_back(nullptr);
        ::execvp(a[0], a.data());
        ::_exit(127);
    }
    return pid;
}
bool has(const std::vector<InstanceProcess>& v, pid_t pid) {
    return std::any_of(v.begin(), v.end(), [&](const InstanceProcess& p) { return p.pid == pid; });
}
bool reaped_or_dead(pid_t pid) {
    int st = 0;
    for (int i = 0; i < 50; ++i) {
        if (::waitpid(pid, &st, WNOHANG) == pid) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
}  // namespace

TEST(terminate_ends_farm_and_prefix_processes_but_spares_others) {
    Tmp t;
    Instance inst;
    inst.farm_path.assign((t.dir / "farm").string());
    inst.cfg.prefix.assign((t.dir / "pfx").string());
    inst.cfg.runner_kind.assign("proton");
    inst.cfg.proton_path.assign((t.dir / "no-proton").string());  // 没有 Proton 的 wineserver：-k 那一步失败也无妨

    static std::string wp = "WINEPREFIX=" + (t.dir / "pfx").string() + "/";  // 末尾的 / 也要认
    const pid_t in_farm = spawn(t.dir / "farm/Data", {"sleep", "30"});
    const pid_t in_prefix = spawn(t.dir / "elsewhere", {"sleep", "31"}, wp.c_str());
    const pid_t unrelated = spawn(t.dir / "elsewhere", {"sleep", "32"});
    const pid_t shell = spawn(t.dir / "farm", {"sh", "-c", "exec sh -c 'sleep 33; :'"});  // 交互 shell 在农场里：不碰
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto found = instance_processes(inst);
    CHECK(has(found, in_farm));
    CHECK(has(found, in_prefix));
    CHECK(!has(found, unrelated));
    CHECK(!has(found, shell));
    CHECK(!has(found, ::getpid()));

    // dry-run 不发信号
    const auto dry = terminate_instance(inst, 2000, true);
    CHECK(has(dry.found, in_farm));
    CHECK(::kill(in_farm, 0) == 0);

    const auto res = terminate_instance(inst, 2000, false);
    CHECK(res.remaining.empty());
    CHECK(reaped_or_dead(in_farm));
    CHECK(reaped_or_dead(in_prefix));
    CHECK(::kill(unrelated, 0) == 0);  // 无关的还活着
    CHECK(::kill(shell, 0) == 0);

    ::kill(unrelated, SIGKILL);
    ::kill(-shell, SIGKILL);
    ::kill(shell, SIGKILL);
    reaped_or_dead(unrelated);
    reaped_or_dead(shell);
}

TEST(terminate_escalates_to_sigkill_after_the_timeout) {
    Tmp t;
    Instance inst;
    inst.farm_path.assign((t.dir / "farm").string());
    // 忽略 SIGTERM 的进程（perl 不在就用 python3；都没有就跳过）
    const bool have_py = std::system("command -v python3 >/dev/null 2>&1") == 0;
    if (!have_py) return;
    const pid_t stubborn = spawn(t.dir / "farm", {"python3", "-c", "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(30)"});
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const auto res = terminate_instance(inst, 300, false);
    CHECK(std::find(res.killed.begin(), res.killed.end(), stubborn) != res.killed.end());
    CHECK(reaped_or_dead(stubborn));
}
