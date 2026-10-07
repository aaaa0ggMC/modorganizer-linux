#include "mol/runner.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
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
    // 绝对路径（农场之外的工具）原样使用；否则相对农场根
    const string exe = (!exe_rel.empty() && exe_rel.front() == '/') ? string(exe_rel, mem) : join_dir(inst.farm_path, exe_rel, mem);

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
    if (!opt.cow_library.empty()) {
        string pre(opt.cow_library, mem);
        if (const char* old = std::getenv("LD_PRELOAD"); old && *old) {  // 保留用户自己的 preload（MangoHud 等）
            pre.push_back(':');
            pre.append(old);
        }
        put(spec.env, "LD_PRELOAD", pre, mem);
        put(spec.env, "MOL_COW_ROOT", inst.farm_path, mem);
        // 农场链接能指向的全部原文件所在目录：游戏、mods、overwrite、overwrite-root
        string protect(c.game_dir, mem);
        for (std::string_view d : {std::string_view(inst.overwrite_dir)}) {
            protect.push_back(':');
            protect.append(d);
        }
        protect.push_back(':');
        protect.append(inst.root);
        protect.append("/overwrite-root");
        put(spec.env, "MOL_COW_PROTECT", protect, mem);
        put(spec.env, "MOL_COW_MODS", inst.mods_dir, mem);
        if (!opt.cow_log.empty()) put(spec.env, "MOL_COW_LOG", opt.cow_log, mem);
    }
    return spec;
}

namespace {

// /proc/<pid>/environ 里的某个变量；读不到返回空。
std::string proc_env(const std::string& pid, std::string_view key) {
    std::FILE* f = std::fopen(("/proc/" + pid + "/environ").c_str(), "rb");
    if (!f) return {};
    std::string all;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    std::fclose(f);
    for (std::size_t i = 0; i < all.size();) {
        const std::size_t e = all.find('\0', i);
        const std::string_view kv(all.data() + i, (e == std::string::npos ? all.size() : e) - i);
        if (kv.size() > key.size() && kv.substr(0, key.size()) == key && kv[key.size()] == '=') return std::string(kv.substr(key.size() + 1));
        if (e == std::string::npos) break;
        i = e + 1;
    }
    return {};
}

std::string canon(std::string_view p) {
    std::error_code ec;
    const auto c = fs::weakly_canonical(fs::path(std::string(p)), ec);
    std::string s = ec ? std::string(p) : c.string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// 这个前缀下、环境与本次注入不一致的 wineserver 进程数
int foreign_wineservers(const Instance& inst, std::string_view lib) {
    const std::string prefix = canon(inst.cfg.prefix);
    const std::string farm = canon(inst.farm_path);
    int n = 0;
    std::error_code ec;
    for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string pid = it->path().filename().string();
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
        std::FILE* f = std::fopen(("/proc/" + pid + "/comm").c_str(), "rb");
        if (!f) continue;
        char comm[64] = {};
        const std::size_t r = std::fread(comm, 1, sizeof(comm) - 1, f);
        std::fclose(f);
        if (std::string_view(comm, r).rfind("wineserver", 0) != 0) continue;
        const std::string wp = proc_env(pid, "WINEPREFIX");
        if (canon(wp.empty() ? std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.wine" : wp) != prefix) continue;
        const bool same = proc_env(pid, "LD_PRELOAD").find(std::string(lib)) != std::string::npos && canon(proc_env(pid, "MOL_COW_ROOT")) == farm;
        if (!same) ++n;
    }
    return n;
}

}  // namespace

void ensure_wineserver_cow(const Instance& inst, std::string_view cow_library, int timeout_ms) {
    if (cow_library.empty() || inst.cfg.prefix.empty()) return;
    for (int waited = 0;; waited += 200) {
        if (foreign_wineservers(inst, cow_library) == 0) return;
        if (waited >= timeout_ms) break;
        ::usleep(200 * 1000);
    }
    throw Error("wine_busy",
                "a Wine server for this prefix is already running without copy-on-write; files written through the farm would change the originals "
                "(end it with `mo-linux terminate`)",
                std::string(inst.cfg.prefix));
}

string find_cow_library(mr* mem) {
    std::error_code ec;
    if (const char* e = std::getenv("MOL_COW_LIB"); e && *e) {
        if (fs::is_regular_file(e, ec)) return string(fs::absolute(e, ec).string(), mem);
        return string(mem);
    }
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        const fs::path p = self.parent_path() / "libmol-cow.so";
        if (fs::is_regular_file(p, ec)) return string(p.string(), mem);
    }
    return string(mem);
}

int spawn_launch(const LaunchSpec& spec, bool wait) {
    if (spec.argv.empty()) throw Error("invalid_argument", "empty launch argv");
    pid_t pid = ::fork();
    if (pid < 0) throw Error("io_error", std::string("fork failed: ") + std::strerror(errno));
    if (pid == 0) {
        if (!wait) ::setsid();  // 脱离控制终端，随 CLI 退出而继续运行
        if (!spec.cwd.empty() && ::chdir(spec.cwd.c_str()) != 0) ::_exit(126);
        ::dup2(STDERR_FILENO, STDOUT_FILENO);  // 游戏/工具的输出不能混进 mo-linux 的 stdout（JSON 信封）
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
