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

// FOMOD 的文件依赖：游戏 Data 或任一已启用 mod 的 Data 里有该文件 → Active，否则 Missing。
std::function<std::string(std::string_view)> make_file_state(const Instance& inst, std::string_view profile, mr* mem) {
    std::vector<fs::path> roots;
    roots.push_back(fs::path(std::string(inst.cfg.game_dir)) / "Data");
    for (const auto& m : list_mods(inst, profile, mem))
        if (m.enabled && m.exists && !m.separator) roots.push_back(m.root ? fs::path(std::string(m.path)) / "Data" : fs::path(std::string(m.path)));
    return [roots](std::string_view file) -> std::string {
        std::string rel(file);
        std::replace(rel.begin(), rel.end(), '\\', '/');
        for (const auto& r : roots) {
            fs::path cur = r;
            bool ok = true;
            std::size_t i = 0;
            while (i <= rel.size() && ok) {
                std::size_t j = rel.find('/', i);
                if (j == std::string::npos) j = rel.size();
                const std::string comp = rel.substr(i, j - i);
                i = j + 1;
                if (comp.empty()) { if (j == rel.size()) break; continue; }
                const auto want = casefold(comp);
                fs::path next;
                std::error_code ec;
                for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec))
                    if (casefold(it->path().filename().string()) == want) { next = it->path(); break; }
                if (next.empty()) ok = false; else cur = next;
                if (j == rel.size()) break;
            }
            if (ok) return "Active";
        }
        return "Missing";
    };
}
}  // namespace

std::function<std::string(std::string_view)> fomod_file_state(const Instance& inst, std::string_view profile, mr* mem) {
    return make_file_state(inst, profile, mem);
}

InstallResult install_archive(const Instance& inst, std::string_view archive_s, std::string_view name_in, bool force_root,
                              std::string_view profile, mr* mem) {
    InstallOptions o;
    o.name = name_in;
    o.force_root = force_root;
    o.profile = profile;
    o.fomod = FomodMode::Raw;
    return install_archive(inst, archive_s, o, mem);
}

InstallResult install_archive(const Instance& inst, std::string_view archive_s, const InstallOptions& opt, mr* mem) {
    const std::string_view name_in = opt.name;
    const bool force_root = opt.force_root;
    const std::string_view profile = opt.profile;
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
        // 1.5) FOMOD
        if (const std::string cfgp = fomod::find_module_config(tmp.string()); !cfgp.empty() && opt.fomod != FomodMode::Raw) {
            if (opt.fomod == FomodMode::Unset)
                throw Error("fomod_choices_required",
                            "this archive has a FOMOD installer; run `fomod inspect` and pass --fomod CHOICES.json, or --fomod-defaults, or --no-fomod",
                            archive.string());
            const fomod::Config cfg = fomod::load_config(cfgp);
            fomod::Env env = opt.fomod_env;
            if (!env.file_state) env.file_state = make_file_state(inst, profile, mem);
            const fomod::Resolved r = fomod::resolve(cfg, opt.fomod == FomodMode::Choices ? opt.choices : fomod::Choices{},
                                                     opt.fomod == FomodMode::Defaults || opt.use_defaults_for_missing, env);
            const fs::path stage = mods / (".mol-stage-" + std::to_string(::getpid()));
            fs::remove_all(stage, ec);
            fs::create_directories(stage, ec);
            try {
                fomod::install_files(r, tmp.string(), stage.string(), &res.missing);
            } catch (...) {
                fs::remove_all(stage, ec);
                throw;
            }
            fs::remove_all(tmp, ec);
            fs::rename(stage, tmp, ec);
            if (ec) throw Error("io_error", "cannot stage FOMOD output: " + ec.message(), stage.string());
            res.fomod = true;
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

std::optional<fomod::Config> read_archive_fomod(const Instance& inst, std::string_view archive_s) {
    const fs::path archive{std::string(archive_s)};
    std::error_code ec;
    if (!fs::is_regular_file(archive, ec)) throw Error("invalid_argument", "archive not found", archive.string());
    const fs::path tmp = fs::path(std::string(inst.downloads_dir)) / (".mol-fomod-" + std::to_string(::getpid()));
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    try {
        // 只解 fomod 目录（7z 的 -ir 通配大小写敏感，所以枚举 + 精确解压会更繁琐；FOMOD 配置体量小，直接全解 fomod 前缀的两种常见写法）
        int rc = 127;
        const std::string a = archive.string(), o = "-o" + tmp.string();
        if (which("7z")) rc = run({"7z", "x", "-y", "-bd", o, a, "-ir!fomod/*", "-ir!Fomod/*", "-ir!FOMOD/*", "-ir!*/fomod/*", "-ir!*/Fomod/*", "-ir!*/FOMOD/*"});
        else if (which("7zz")) rc = run({"7zz", "x", "-y", "-bd", o, a, "-ir!fomod/*", "-ir!Fomod/*", "-ir!FOMOD/*", "-ir!*/fomod/*", "-ir!*/Fomod/*", "-ir!*/FOMOD/*"});
        else extract(archive, tmp);
        if (rc != 0 && rc != 127) {
            // 7z 在没有任何匹配时返回非 0（"No files to process"）：视为没有 FOMOD
            fs::remove_all(tmp, ec);
            return std::nullopt;
        }
        validate_tree(tmp);
        // 单层包装目录：找 fomod 的位置
        std::string cfgp = fomod::find_module_config(tmp.string());
        if (cfgp.empty()) {
            for (const auto& c : children(tmp))
                if (fs::is_directory(c, ec)) { cfgp = fomod::find_module_config(c.string()); if (!cfgp.empty()) break; }
        }
        std::optional<fomod::Config> out;
        if (!cfgp.empty()) out = fomod::load_config(cfgp);
        fs::remove_all(tmp, ec);
        return out;
    } catch (...) {
        fs::remove_all(tmp, ec);
        throw;
    }
}

}  // namespace mol
