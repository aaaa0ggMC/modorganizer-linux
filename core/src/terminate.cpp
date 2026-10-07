#include "mol/terminate.hpp"

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#include "mol/runner.hpp"

namespace mol {
namespace fs = std::filesystem;
namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string canon(std::string_view p) {
    std::error_code ec;
    std::string s = fs::weakly_canonical(fs::path(std::string(p)), ec).string();
    if (ec || s.empty()) s = std::string(p);
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

bool is_pid(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); }

int parent_of(int pid) {
    // /proc/<pid>/stat：pid (comm) state ppid …；comm 里可能有空格和括号，从最后一个 ')' 往后读
    const std::string st = slurp("/proc/" + std::to_string(pid) + "/stat");
    const auto r = st.rfind(')');
    if (r == std::string::npos) return 0;
    int ppid = 0;
    char state = 0;
    if (std::sscanf(st.c_str() + r + 1, " %c %d", &state, &ppid) != 2) return 0;
    return ppid;
}

// 僵尸或已不存在
bool gone(int pid) {
    if (::kill(pid, 0) != 0) return true;
    const std::string st = slurp("/proc/" + std::to_string(pid) + "/stat");
    const auto r = st.rfind(')');
    return r == std::string::npos || (r + 2 < st.size() && st[r + 2] == 'Z');
}

bool is_shell(const fs::path& exe) {
    static const char* const shells[] = {"bash", "sh", "zsh", "fish", "dash", "ksh", "tcsh", "csh", "nu", "xonsh"};
    const std::string n = exe.filename().string();
    return std::any_of(std::begin(shells), std::end(shells), [&](const char* s) { return n == s; });
}

}  // namespace

std::vector<InstanceProcess> instance_processes(const Instance& inst) {
    std::vector<InstanceProcess> out;
    const std::string farm = inst.farm_path.empty() ? std::string() : canon(inst.farm_path);
    std::string farm_bwd = farm;
    std::replace(farm_bwd.begin(), farm_bwd.end(), '/', '\\');
    const std::string pfx = inst.cfg.prefix.empty() ? std::string() : canon(inst.cfg.prefix);
    std::string compat = pfx;  // STEAM_COMPAT_DATA_PATH = 前缀的父目录（prefix 以 /pfx 结尾时）
    if (compat.size() > 4 && compat.ends_with("/pfx")) compat.resize(compat.size() - 4);

    std::set<int> spare;  // 自己与祖先
    for (int p = ::getpid(); p > 1 && spare.insert(p).second;) p = parent_of(p);
    const uid_t me = ::getuid();

    std::error_code ec;
    for (fs::directory_iterator it("/proc", fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!is_pid(name)) continue;
        const int pid = std::stoi(name);
        if (spare.count(pid)) continue;
        struct stat sb {};
        if (::stat(it->path().c_str(), &sb) != 0 || sb.st_uid != me) continue;
        std::error_code e2;
        const fs::path exe = fs::read_symlink(it->path() / "exe", e2);
        if (!e2 && is_shell(exe)) continue;

        std::string cmd = slurp(it->path() / "cmdline");
        std::string reason;
        if (!farm.empty()) {
            const fs::path cwd = fs::read_symlink(it->path() / "cwd", e2);
            if (!e2 && (cwd.string() == farm || cwd.string().rfind(farm + "/", 0) == 0)) reason = "farm_cwd";
            else if (cmd.find(farm + "/") != std::string::npos || cmd.find(farm_bwd + "\\") != std::string::npos) reason = "farm_cmdline";
        }
        if (reason.empty() && !pfx.empty()) {
            const std::string env = slurp(it->path() / "environ");
            for (std::size_t i = 0; i < env.size() && reason.empty();) {
                std::size_t e = env.find('\0', i);
                if (e == std::string::npos) e = env.size();
                const std::string_view kv(env.data() + i, e - i);
                if (kv.starts_with("WINEPREFIX=") && canon(kv.substr(11)) == pfx) reason = "prefix_env";
                else if (kv.starts_with("STEAM_COMPAT_DATA_PATH=") && canon(kv.substr(23)) == compat) reason = "prefix_env";
                i = e + 1;
            }
        }
        if (reason.empty()) continue;
        std::replace(cmd.begin(), cmd.end(), '\0', ' ');
        while (!cmd.empty() && cmd.back() == ' ') cmd.pop_back();
        if (cmd.size() > 160) cmd = cmd.substr(0, 160) + "…";
        out.push_back({pid, cmd, reason});
    }
    std::sort(out.begin(), out.end(), [](const InstanceProcess& a, const InstanceProcess& b) { return a.pid < b.pid; });
    return out;
}

TerminateResult terminate_instance(const Instance& inst, int timeout_ms, bool dry_run) {
    TerminateResult res;
    res.found = instance_processes(inst);
    if (dry_run) return res;

    for (const auto& p : res.found) ::kill(p.pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    std::vector<int> alive;
    for (const auto& p : res.found) alive.push_back(p.pid);
    while (true) {
        std::vector<int> still;
        for (int pid : alive) (gone(pid) ? res.terminated : still).push_back(pid);
        alive.swap(still);
        if (alive.empty() || std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    for (int pid : alive) ::kill(pid, SIGKILL);
    for (int i = 0; i < 10 && !alive.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::vector<int> still;
        for (int pid : alive) (gone(pid) ? res.killed : still).push_back(pid);
        alive.swap(still);
    }
    res.remaining = alive;

    // 前缀的 wineserver（以及它留着的 winedevice/services）：交给 wineserver -k 收尾
    if (!inst.cfg.prefix.empty()) {
        LaunchSpec spec;
        std::error_code ec;
        const fs::path proton_ws = fs::path(std::string(inst.cfg.proton_path)) / "files/bin/wineserver";
        if (inst.cfg.runner_kind == "proton" && fs::exists(proton_ws, ec)) spec.argv.emplace_back(proton_ws.string());
        else spec.argv.emplace_back("wineserver");
        spec.argv.emplace_back("-k");
        spec.env.emplace_back(string("WINEPREFIX"), string(std::string(inst.cfg.prefix)));
        try {
            res.wineserver_killed = spawn_launch(spec, true) == 0;
        } catch (const Error&) {
        }
    }
    return res;
}

}  // namespace mol
