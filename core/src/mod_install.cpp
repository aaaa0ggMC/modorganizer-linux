#include "mol/mod_install.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "mol/casefold.hpp"

extern char** environ;

namespace mol {
namespace fs = std::filesystem;
namespace {

bool which(const char* tool) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p(path);
    for (std::size_t b = 0; b <= p.size();) {
        std::size_t e = p.find(':', b);
        if (e == std::string::npos) e = p.size();
        const fs::path c = fs::path(p.substr(b, e - b)) / tool;
        std::error_code ec;
        if (fs::exists(c, ec) && ::access(c.c_str(), X_OK) == 0) return true;
        b = e + 1;
    }
    return false;
}

int run(const std::vector<std::string>& argv) {
    std::vector<char*> av;
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);  // 抑制解压工具的 stdout
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return 127;
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return 1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

void extract(const fs::path& archive, const fs::path& dest) {
    int rc = 127;
    if (which("7z")) rc = run({"7z", "x", "-y", "-bd", "-o" + dest.string(), archive.string()});
    else if (which("7zz")) rc = run({"7zz", "x", "-y", "-bd", "-o" + dest.string(), archive.string()});
    else if (which("bsdtar")) rc = run({"bsdtar", "-xf", archive.string(), "-C", dest.string()});
    else throw Error("io_error", "no archive tool found (install p7zip/7zip or libarchive's bsdtar)");
    if (rc != 0) throw Error("io_error", "extracting the archive failed (exit " + std::to_string(rc) + ")", archive.string());
}

// 拒绝符号链接；确认所有条目都在 dir 内。返回文件数。
std::size_t validate_tree(const fs::path& dir) {
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(dir, ec);
    std::size_t files = 0;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto st = it->symlink_status(ec);
        if (fs::is_symlink(st)) throw Error("invalid_argument", "archive contains a symbolic link; refusing", it->path().string());
        const fs::path c = fs::weakly_canonical(it->path(), ec);
        if (c.string().rfind(base.string() + "/", 0) != 0) throw Error("invalid_argument", "archive entry escapes the target directory", it->path().string());
        if (fs::is_regular_file(st)) ++files;
    }
    return files;
}

void move_children_up(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    std::vector<fs::path> kids;
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) kids.push_back(it->path());
    for (const auto& k : kids) {
        fs::rename(k, to / k.filename(), ec);
        if (ec) throw Error("io_error", "rename failed: " + ec.message(), k.string());
    }
}

std::vector<fs::path> children(const fs::path& d) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) v.push_back(it->path());
    return v;
}

bool has_exe_or_dll(const fs::path& d) {
    for (const auto& c : children(d)) {
        std::error_code ec;
        if (!fs::is_regular_file(c, ec)) continue;
        const auto ext = casefold(c.extension().string());
        if (ext == ".exe" || ext == ".dll") return true;
    }
    return false;
}

std::string sanitize(std::string s) {
    for (char& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\0') c = '_';
    while (!s.empty() && (s.front() == '.' || s.front() == ' ')) s.erase(s.begin());
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    return s;
}
}  // namespace

InstallResult install_archive(const Instance& inst, std::string_view archive_s, std::string_view name_in, bool force_root,
                              std::string_view profile, mr* mem) {
    const fs::path archive{std::string(archive_s)};
    std::error_code ec;
    if (!fs::is_regular_file(archive, ec)) throw Error("invalid_argument", "archive not found", archive.string());

    std::string name = sanitize(name_in.empty() ? archive.stem().string() : std::string(name_in));
    if (name.empty()) throw Error("invalid_argument", "empty mod name");
    const fs::path mods{std::string(inst.mods_dir)};
    const fs::path target = mods / name;
    if (fs::exists(fs::symlink_status(target, ec))) throw Error("invalid_argument", "mod directory already exists: " + name, target.string());
    for (const auto& m : list_mods(inst, profile, mem))
        if (casefold(m.name) == casefold(name)) throw Error("invalid_argument", "mod already exists in modlist: " + name);
    fs::create_directories(mods, ec);

    const fs::path tmp = mods / (".mol-extract-" + std::to_string(::getpid()));
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    InstallResult res(mem);
    try {
        extract(archive, tmp);
        validate_tree(tmp);
        // 1) 去掉「只有一个顶层目录」的包装
        for (int guard = 0; guard < 4; ++guard) {
            auto kids = children(tmp);
            if (kids.size() != 1 || !fs::is_directory(kids[0], ec)) break;
            if (casefold(kids[0].filename().string()) == "data") break;  // Data 目录不是包装
            const fs::path inner = kids[0];
            const fs::path hop = tmp / ".mol-hop";
            fs::rename(inner, hop, ec);
            if (ec) throw Error("io_error", "rename failed: " + ec.message(), inner.string());
            move_children_up(hop, tmp);
            fs::remove(hop, ec);
        }
        // 2) 布局
        bool root = force_root || has_exe_or_dll(tmp);
        if (!root) {
            auto kids = children(tmp);
            if (kids.size() == 1 && fs::is_directory(kids[0], ec) && casefold(kids[0].filename().string()) == "data") {
                const fs::path hop = tmp / ".mol-hop";
                fs::rename(kids[0], hop, ec);
                move_children_up(hop, tmp);
                fs::remove(hop, ec);
            }
        }
        res.files = validate_tree(tmp);
        if (res.files == 0) throw Error("invalid_argument", "the archive contains no files", archive.string());
        fs::rename(tmp, target, ec);
        if (ec) throw Error("io_error", "cannot move extracted files into place: " + ec.message(), target.string());
        if (root) mark_mod_root(target.string(), true);
        res.root = root;
    } catch (...) {
        fs::remove_all(tmp, ec);
        throw;
    }
    res.name = string(name, mem);
    res.path = string(target.string(), mem);
    try {
        add_mod(inst, name, true, profile);
    } catch (...) {
        fs::remove_all(target, ec);  // modlist 写失败：不留下没登记的目录
        throw;
    }
    return res;
}

}  // namespace mol
