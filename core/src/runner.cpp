#include "mol/runner.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace mol {
namespace {
namespace fs = std::filesystem;

void put(vector<std::pair<string, string>>& env, std::string_view k, std::string_view v, mr* mem) {
    env.emplace_back(string(k, mem), string(v, mem));
}

string join_dir(std::string_view a, std::string_view b, mr* mem) {
    string s(a, mem);
    if (!s.empty() && s.back() != '/') s.push_back('/');
    s.append(b);
    return s;
}
}  // namespace

LaunchSpec build_launch(const Instance& inst, std::string_view exe_rel, const LaunchOptions& opt, mr* mem) {
    LaunchSpec spec(mem);
    const auto& c = inst.cfg;
    spec.cwd.assign(inst.farm_path);
    const string exe = join_dir(inst.farm_path, exe_rel, mem);

    if (c.runner_kind == "proton") {
        if (c.proton_path.empty())
            throw Error("config_invalid", "runner 'proton' requires proton_path in mo-linux.json");
        spec.argv.push_back(join_dir(c.proton_path, "proton", mem));
        spec.argv.emplace_back("run");
        spec.argv.push_back(exe);

        std::string_view compat = c.prefix;
        if (compat.size() >= 4 && compat.substr(compat.size() - 4) == "/pfx") compat.remove_suffix(4);
        if (!compat.empty()) put(spec.env, "STEAM_COMPAT_DATA_PATH", compat, mem);
        if (!c.steam_root.empty()) put(spec.env, "STEAM_COMPAT_CLIENT_INSTALL_PATH", c.steam_root, mem);
        put(spec.env, "STEAM_COMPAT_APP_ID", opt.steam_app_id, mem);
        put(spec.env, "SteamAppId", opt.steam_app_id, mem);
        put(spec.env, "SteamGameId", opt.steam_app_id, mem);
        string mounts(mem);
        for (std::string_view d : {std::string_view(inst.farm_path), std::string_view(inst.root),
                                   std::string_view(inst.mods_dir), std::string_view(inst.overwrite_dir),
                                   std::string_view(c.game_dir)}) {
            if (d.empty()) continue;
            if (!mounts.empty()) mounts.push_back(':');
            mounts.append(d);
        }
        put(spec.env, "STEAM_COMPAT_MOUNTS", mounts, mem);
        put(spec.env, "PRESSURE_VESSEL_FILESYSTEMS_RW", mounts, mem);
    } else if (c.runner_kind == "wine") {
        spec.argv.emplace_back("wine");
        spec.argv.push_back(exe);
        if (!c.prefix.empty()) put(spec.env, "WINEPREFIX", c.prefix, mem);
        if (!std::getenv("WINEDEBUG")) put(spec.env, "WINEDEBUG", "-all", mem);
    } else {
        throw Error("config_invalid", "unknown runner_kind: " + std::string(c.runner_kind));
    }
    for (std::string_view a : opt.args) spec.argv.emplace_back(a);
    return spec;
}

int spawn_launch(const LaunchSpec& spec, bool wait) {
    if (spec.argv.empty()) throw Error("invalid_argument", "empty launch argv");
    pid_t pid = ::fork();
    if (pid < 0) throw Error("io_error", std::string("fork failed: ") + std::strerror(errno));
    if (pid == 0) {
        if (!wait) ::setsid();  // 脱离控制终端，随 CLI 退出而继续运行
        if (!spec.cwd.empty() && ::chdir(spec.cwd.c_str()) != 0) ::_exit(126);
        for (const auto& [k, v] : spec.env) ::setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> av;
        for (const auto& a : spec.argv) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        ::execvp(av[0], av.data());
        ::_exit(127);
    }
    if (!wait) {
        // 双重检查：若子进程立刻失败（exec 失败）也不阻塞；僵尸由 SIGCHLD 默认忽略策略外的 waitpid(WNOHANG) 处理
        int st = 0;
        ::waitpid(pid, &st, WNOHANG);
        return 0;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) throw Error("io_error", std::string("waitpid failed: ") + std::strerror(errno));
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

}  // namespace mol
