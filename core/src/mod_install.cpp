#include "mol/mod_install.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "mol/casefold.hpp"
#include "mol/md5.hpp"
#include "mol/mo2fmt.hpp"
#include "mol/xxh64.hpp"

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

// 集合清单的「复刻」安装：按 (相对 mod 根的路径, md5) 从解压目录 src 里挑文件放进 stage。
// 先认同一路径（大小写不敏感，也认 Data/ 前缀）且 md5 相符的；否则按 md5 在整个压缩包里找（FOMOD 的选项目录结构与成品不同）。
// 同一源文件可以放到多处：用硬链接（同一文件系统），不行再复制。返回放好的文件数；找不到的记进 missing。
std::size_t replicate_files(const fs::path& src, const fs::path& stage, const std::vector<std::pair<std::string, std::string>>& want,
                            std::vector<std::string>& missing) {
    std::error_code ec;
    std::map<std::string, fs::path> by_path;  // casefold(相对路径) → 文件
    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) by_path.emplace(std::string(casefold(it->path().lexically_relative(src).generic_string())), it->path());
    std::optional<std::map<std::string, fs::path>> by_md5;  // 只在路径对不上时才算全部文件的 md5
    std::map<std::string, std::string> dirs;                // casefold(已建的目录) → 实际写法：同一目录不因大小写不同建两份
    std::size_t placed = 0;
    for (const auto& [rel_in, md5_in] : want) {
        std::string rel = rel_in;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        const std::string md5(casefold(md5_in));
        const fs::path relp = fs::path(rel).lexically_normal();
        if (rel.empty() || relp.is_absolute() || relp.begin()->string() == "..") { missing.push_back(rel_in); continue; }
        fs::path from;
        for (const std::string& cand : {relp.generic_string(), "data/" + relp.generic_string()}) {
            auto it = by_path.find(std::string(casefold(cand)));
            if (it != by_path.end() && (md5.empty() || std::string(casefold(md5_file(it->second.string()))) == md5)) { from = it->second; break; }
        }
        if (from.empty() && !md5.empty()) {
            if (!by_md5) {
                by_md5.emplace();
                for (const auto& [k, p] : by_path) by_md5->emplace(std::string(casefold(md5_file(p.string()))), p);
            }
            if (auto it = by_md5->find(md5); it != by_md5->end()) from = it->second;
        }
        if (from.empty()) { missing.push_back(rel_in); continue; }
        // 目标路径：父目录沿用已建的大小写
        fs::path to = stage;
        std::string acc;
        const fs::path parent = relp.parent_path();
        for (const auto& comp : parent) {
            acc += std::string(casefold(comp.string())) + "/";
            auto [it, fresh] = dirs.emplace(acc, (to / comp).string());
            to = fs::path(it->second);
        }
        to /= relp.filename();
        fs::create_directories(to.parent_path(), ec);
        fs::remove(to, ec);
        fs::create_hard_link(from, to, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
            if (ec) throw Error("io_error", "cannot place file: " + ec.message(), to.string());
        }
        ++placed;
    }
    return placed;
}

// Windows 上打的部分 zip 用 '\\' 作路径分隔符，Linux 的解压工具把它当文件名的一部分（"Data\\SKSE\\Plugins\\X.dll"）。
// 把这类名字拆回目录；拆出 ".." 或空段的拒绝。返回处理的条目数。
std::size_t split_backslash_names(const fs::path& root) {
    std::error_code ec;
    std::vector<fs::path> bad;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename().string().find('\\') != std::string::npos) bad.push_back(it->path());
    // 深的先处理，免得父目录先被移走
    std::sort(bad.begin(), bad.end(), [](const fs::path& a, const fs::path& b) { return a.string().size() > b.string().size(); });
    for (const auto& p : bad) {
        std::string name = p.filename().string();
        std::replace(name.begin(), name.end(), '\\', '/');
        const fs::path rel = fs::path(name).lexically_normal();
        for (const auto& c : rel)
            if (c == ".." || c.empty()) throw Error("invalid_argument", "archive entry escapes the target directory", p.string());
        if (rel.is_absolute()) throw Error("invalid_argument", "archive entry escapes the target directory", p.string());
        const fs::path to = p.parent_path() / rel;
        fs::create_directories(to.parent_path(), ec);
        fs::rename(p, to, ec);
        if (ec) throw Error("io_error", "cannot split a backslash path: " + ec.message(), p.string());
    }
    return bad.size();
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

// 与上游 SkyrimSEModDataChecker 一致（game_bethesda/src/games/skyrimse/skyrimsemoddatachecker.h）：
// 顶层出现这些目录名之一，或带这些扩展名的文件，就认为这一层已经是 Data 根。
bool looks_like_data_root(const fs::path& d) {
    static const char* const folders[] = {"fonts", "interface", "menus", "meshes", "music", "scripts", "shaders", "sound", "strings", "textures",
                                          "trees", "video", "facegen", "materials", "skse", "distantlod", "asi", "tools", "mcm", "distantland",
                                          "mits", "dllplugins", "calientetools", "netscriptframework", "shadersfx", "nemesis_engine", "platform",
                                          "grass", "lightplacer", "mainmenuwallpapers", "mainmenuvideo", "pbrmaterialobjects", "pbrnifpatcher",
                                          "pbrtexturesets", "pandora_engine"};
    static const char* const exts[] = {".esp", ".esm", ".esl", ".bsa", ".modgroups", ".ini"};
    std::error_code ec;
    for (const auto& c : children(d)) {
        const auto name = casefold(c.filename().string());
        if (name == "meta.ini") continue;  // MO2 的 mod 元数据，不是游戏数据
        if (fs::is_directory(c, ec)) {
            for (const char* f : folders) if (name == f) return true;
        } else {
            const auto ext = casefold(c.extension().string());
            for (const char* e : exts) if (ext == e) return true;
        }
    }
    return false;
}

// 目录名最长字节数。单个路径段的上限是 255 字节（ext4/btrfs），Nexus 上有 150–250 字符的 mod 名，
// 带多字节字符或集合追加的 " [tag]" 就会 ENAMETOOLONG；再留余量给 mod 里自己的长路径。
constexpr std::size_t kMaxModName = 100;

std::string sanitize(std::string s) {
    for (char& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\0') c = '_';
    while (!s.empty() && (s.front() == '.' || s.front() == ' ')) s.erase(s.begin());
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    if (s.size() > kMaxModName) {
        // 截断（按 UTF-8 字符边界）+ 全名的稳定短哈希：同一个名字每次得到同一个目录名（重跑幂等），不同长名不撞
        char tag[16];
        std::snprintf(tag, sizeof tag, " ~%08llx", static_cast<unsigned long long>(xxh64(s) & 0xffffffffULL));
        std::size_t cut = kMaxModName - std::strlen(tag);
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        s.resize(cut);
        while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
        s += tag;
    }
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

std::string sanitize_mod_name(std::string_view name) { return sanitize(std::string(name)); }

bool is_data_root_dir(std::string_view dir) { return looks_like_data_root(fs::path(std::string(dir))); }

bool mod_name_taken(const Instance& inst, std::string_view name_in, std::string_view profile) {
    const std::string want = std::string(casefold(sanitize(std::string(name_in))));
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(std::string(inst.mods_dir)), ec), end; !ec && it != end; it.increment(ec))
        if (std::string(casefold(it->path().filename().string())) == want) return true;
    for (const auto& m : list_mods(inst, profile))
        if (std::string(casefold(m.name)) == want) return true;
    return false;
}

// FOMOD 配置缓存：~/.cache/mo-linux/fomod/<xxh64(文件名|大小|mtime)>.xml。固实 7z 里只取 fomod/ 也要解压整个固实块，
// 大包要几十秒；安装时整包已经解开，顺手存一份，之后的 fomod inspect / collection verify 直接读。
static fs::path fomod_cache_path(const fs::path& archive) {
    std::error_code ec;
    const auto size = fs::file_size(archive, ec);
    const auto mtime = fs::last_write_time(archive, ec).time_since_epoch().count();
    const std::string key = archive.filename().string() + "|" + std::to_string(size) + "|" + std::to_string(mtime);
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(xxh64(key)));
    const char* x = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    const fs::path base = (x && *x) ? fs::path(x) / "mo-linux" : fs::path(home ? home : "/tmp") / ".cache/mo-linux";
    return base / "fomod" / (std::string(buf) + ".xml");
}
static void cache_fomod_config(const fs::path& archive, const std::string& cfg_path) {
    std::error_code ec;
    const fs::path dst = fomod_cache_path(archive);
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(cfg_path, dst, fs::copy_options::overwrite_existing, ec);  // 缓存失败不影响安装
}

std::vector<std::string> list_archive(std::string_view archive) {
    std::vector<std::string> argv;
    if (which("7z")) argv = {"7z", "l", "-ba", "-slt", std::string(archive)};
    else if (which("7zz")) argv = {"7zz", "l", "-ba", "-slt", std::string(archive)};
    else if (which("bsdtar")) argv = {"bsdtar", "-tf", std::string(archive)};
    else throw Error("io_error", "no archive tool found (install p7zip/7zip or libarchive's bsdtar)");
    int pipefd[2];
    if (::pipe(pipefd) != 0) throw Error("io_error", "pipe failed");
    std::vector<char*> av;
    for (auto& a : argv) av.push_back(a.data());
    av.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(pipefd[1]);
    std::string out;
    if (rc == 0) {
        char buf[65536];
        for (ssize_t n; (n = ::read(pipefd[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR);)
            if (n > 0) out.append(buf, static_cast<std::size_t>(n));
        int st = 0;
        while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    }
    ::close(pipefd[0]);
    if (rc != 0) throw Error("io_error", "cannot run the archive tool", std::string(archive));
    std::vector<std::string> names;
    std::vector<bool> is_dir;  // 与 names 一一对应（-slt 下目录条目会给 Folder = + 和/或 Attributes = D…）
    const bool slt = argv[0] != "bsdtar";
    std::size_t i = 0;
    while (i < out.size()) {
        std::size_t j = out.find('\n', i);
        if (j == std::string::npos) j = out.size();
        std::string line = out.substr(i, j - i);
        i = j + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (slt) {
            if (line.rfind("Path = ", 0) == 0) { names.push_back(line.substr(7)); is_dir.push_back(false); }
            else if (!is_dir.empty() && (line == "Folder = +" || line.rfind("Attributes = D", 0) == 0)) is_dir.back() = true;
        } else if (!line.empty() && line.back() != '/') {
            names.push_back(line);
        }
    }
    if (slt) {
        std::vector<std::string> files;
        for (std::size_t k = 0; k < names.size(); ++k) if (!is_dir[k]) files.push_back(std::move(names[k]));
        names.swap(files);
    }
    for (auto& n : names) std::replace(n.begin(), n.end(), '\\', '/');
    return names;
}

void extract_archive(std::string_view archive, std::string_view dest) { extract(fs::path(std::string(archive)), fs::path(std::string(dest))); }

void reorder_mods(const Instance& inst, std::span<const string> names, std::string_view profile) {
    const std::string pname = profile.empty() ? std::string(inst.cfg.profile) : std::string(profile);
    std::error_code ec;
    fs::path pdir;
    for (fs::directory_iterator it(fs::path(std::string(inst.profiles_dir)), ec), end; !ec && it != end; it.increment(ec))
        if (casefold(it->path().filename().string()) == casefold(pname) && it->is_directory(ec)) { pdir = it->path(); break; }
    if (pdir.empty()) throw Error("profile_not_found", "profile directory missing", (fs::path(std::string(inst.profiles_dir)) / pname).string());
    const std::string file = (pdir / "modlist.txt").string();
    auto entries = read_modlist(file);
    std::vector<ModEntry> out;
    auto is_named = [&](const ModEntry& e) {
        for (const auto& n : names) if (casefold(n) == casefold(e.name)) return true;
        return false;
    };
    for (auto& e : entries) if (!is_named(e)) out.push_back(std::move(e));
    for (const auto& n : names) {
        ModEntry e;
        e.name.assign(n);
        e.enabled = true;
        out.push_back(std::move(e));
    }
    write_modlist(file, out);
}

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
    const bool replacing = opt.replace_existing && fs::is_directory(fs::symlink_status(target, ec));
    bool listed = false;
    for (const auto& m : list_mods(inst, profile, mem))
        if (casefold(m.name) == casefold(name)) listed = true;
    if (!opt.replace_existing) {
        if (fs::exists(fs::symlink_status(target, ec))) throw Error("invalid_argument", "mod directory already exists: " + name, target.string());
        if (listed) throw Error("invalid_argument", "mod already exists in modlist: " + name);
    }
    fs::create_directories(mods, ec);

    const fs::path tmp = mods / (".mol-extract-" + std::to_string(::getpid()));
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    InstallResult res(mem);
    try {
        extract(archive, tmp);
        split_backslash_names(tmp);
        validate_tree(tmp);
        // 1) 去掉「只有一个顶层目录」的包装
        for (int guard = 0; guard < 4; ++guard) {
            auto kids = children(tmp);
            if (kids.size() != 1 || !fs::is_directory(kids[0], ec)) break;
            if (casefold(kids[0].filename().string()) == "data") break;  // Data 目录不是包装
            if (casefold(kids[0].filename().string()) == "fomod") break;  // 整个包都放在 fomod/ 里（源路径写成 fomod\…）：剥掉就认不出 FOMOD 了
            if (looks_like_data_root(tmp)) break;                         // 唯一的目录本身就是游戏数据目录（如 SKSE/、Scripts/），不能当包装剥掉
            const fs::path inner = kids[0];
            const fs::path hop = tmp / ".mol-hop";
            fs::rename(inner, hop, ec);
            if (ec) throw Error("io_error", "rename failed: " + ec.message(), inner.string());
            move_children_up(hop, tmp);
            fs::remove(hop, ec);
        }
        // 1.4) FOMOD 藏在唯一的顶层目录里（Data/FOMOD/…：Data 不当包装剥，所以上面没剥掉）→ 那一层才是模块根（与 MO2 一致）
        if (opt.fomod != FomodMode::Raw && fomod::find_module_config(tmp.string()).empty()) {
            auto kids = children(tmp);
            if (kids.size() == 1 && fs::is_directory(kids[0], ec) && !fomod::find_module_config(kids[0].string()).empty()) {
                const fs::path hop = tmp / ".mol-hop";
                fs::rename(kids[0], hop, ec);
                if (ec) throw Error("io_error", "rename failed: " + ec.message(), kids[0].string());
                move_children_up(hop, tmp);
                fs::remove(hop, ec);
            }
        }
        // 1.5) 复刻（集合清单的 hashes）：取代 FOMOD
        if (opt.fomod == FomodMode::Replicate) {
            const fs::path stage = mods / (".mol-stage-" + std::to_string(::getpid()));
            fs::remove_all(stage, ec);
            fs::create_directories(stage, ec);
            std::size_t placed = 0;
            try {
                placed = replicate_files(tmp, stage, opt.replicate, res.missing);
            } catch (...) {
                fs::remove_all(stage, ec);
                throw;
            }
            if (placed == 0 && !opt.replicate.empty()) {
                fs::remove_all(stage, ec);
                throw Error("invalid_argument", "none of the files the collection lists were found in this archive", archive.string());
            }
            fs::remove_all(tmp, ec);
            fs::rename(stage, tmp, ec);
            if (ec) throw Error("io_error", "cannot stage the replicated files: " + ec.message(), stage.string());
        }
        // 1.6) FOMOD
        else if (const std::string cfgp = fomod::find_module_config(tmp.string()); !cfgp.empty() && opt.fomod != FomodMode::Raw) {
            if (opt.fomod == FomodMode::Unset)
                throw Error("fomod_choices_required",
                            "this archive has a FOMOD installer; run `fomod inspect` and pass --fomod CHOICES.json, or --fomod-defaults, or --no-fomod",
                            archive.string());
            cache_fomod_config(archive, cfgp);
            const fomod::Config cfg = fomod::load_config(cfgp);
            fomod::Env env = opt.fomod_env;
            if (!env.file_state) env.file_state = make_file_state(inst, profile, mem);
            const fomod::Resolved r = fomod::resolve(cfg, opt.fomod == FomodMode::Choices ? opt.choices : fomod::Choices{},
                                                     opt.fomod == FomodMode::Defaults || opt.use_defaults_for_missing, env,
                                                     opt.fomod == FomodMode::Choices && opt.fomod_lenient ? &res.fomod_notes : nullptr);
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
            // 顶层有 Data 目录、旁边只有说明文档之类（不是游戏数据）→ Data 才是 mod 根（与 MO2/Vortex 一致）；
            // 说明文档一并留在根下。例：Interesting NPCs 的 Hotfix = Data/ + "Patch Notes.txt"
            auto kids = children(tmp);
            fs::path data;
            for (const auto& k : kids)
                if (fs::is_directory(k, ec) && casefold(k.filename().string()) == "data") data = k;
            if (!data.empty() && (kids.size() == 1 || !looks_like_data_root(tmp))) {
                const fs::path hop = tmp / ".mol-hop";
                fs::rename(data, hop, ec);
                move_children_up(hop, tmp);
                fs::remove(hop, ec);
            }
        }
        res.files = validate_tree(tmp);
        // FOMOD 的选择可以合法地「什么都不装」（例如只含可选补丁、一个都没选）：与 MO2 一致，装成空 mod。
        // 压缩包本身没有文件才是错误。
        if (res.files == 0 && !res.fomod) throw Error("invalid_argument", "the archive contains no files", archive.string());
        if (replacing) {  // 新内容齐了才换掉旧目录
            const fs::path old = mods / (".mol-old-" + std::to_string(::getpid()));
            fs::remove_all(old, ec);
            fs::rename(target, old, ec);
            if (ec) throw Error("io_error", "cannot move the old mod directory aside: " + ec.message(), target.string());
            fs::rename(tmp, target, ec);
            if (ec) {
                std::error_code e2;
                fs::rename(old, target, e2);
                throw Error("io_error", "cannot move extracted files into place: " + ec.message(), target.string());
            }
            fs::remove_all(old, ec);
        } else {
            fs::rename(tmp, target, ec);
            if (ec) throw Error("io_error", "cannot move extracted files into place: " + ec.message(), target.string());
        }
        if (root) mark_mod_root(target.string(), true);
        res.root = root;
    } catch (...) {
        fs::remove_all(tmp, ec);
        throw;
    }
    res.name = string(name, mem);
    res.path = string(target.string(), mem);
    if (listed) return res;  // 原地替换：modlist 里已有
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
    if (const fs::path cached = fomod_cache_path(archive); fs::is_regular_file(cached, ec)) {
        try { return fomod::load_config(cached.string()); } catch (const Error&) { fs::remove(cached, ec); }  // 坏缓存：删掉重来
    }
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
        if (!cfgp.empty()) {
            out = fomod::load_config(cfgp);
            cache_fomod_config(archive, cfgp);
        }
        fs::remove_all(tmp, ec);
        return out;
    } catch (...) {
        fs::remove_all(tmp, ec);
        throw;
    }
}

std::map<std::string, std::string> extract_fomod_images(const Instance& inst, std::string_view archive_s, const std::vector<std::string>& images,
                                                        std::string_view dest_s) {
    std::map<std::string, std::string> out;
    const fs::path archive{std::string(archive_s)};
    std::error_code ec;
    if (!fs::is_regular_file(archive, ec)) throw Error("invalid_argument", "archive not found", archive.string());
    const char* tool = which("7z") ? "7z" : which("7zz") ? "7zz" : nullptr;
    if (!tool || images.empty()) return out;

    // 配置里的路径：反斜杠 → '/'，去掉开头的 "./" 与 '/'；拒绝 ".."（只用来匹配，但不让奇怪的路径进命令行）
    auto norm = [](std::string p) {
        for (auto& c : p) if (c == '\\') c = '/';
        while (p.starts_with("./")) p.erase(0, 2);
        while (!p.empty() && p.front() == '/') p.erase(0, 1);
        return p;
    };
    std::vector<std::pair<std::string, std::string>> want;  // 原样 → 规范化
    for (const auto& img : images) {
        const std::string n = norm(img);
        if (n.empty() || n.find("..") != std::string::npos || n.find('*') != std::string::npos || n.find('?') != std::string::npos) continue;
        if (std::none_of(want.begin(), want.end(), [&](const auto& w) { return w.first == img; })) want.emplace_back(img, n);
    }
    if (want.empty()) return out;

    const fs::path tmp = fs::path(std::string(inst.downloads_dir)) / (".mol-fomod-img-" + std::to_string(::getpid()));
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    try {
        std::vector<std::string> argv{tool, "x", "-y", "-bd", "-ssc-", "-o" + tmp.string(), archive.string()};
        for (const auto& [orig, n] : want) {
            argv.push_back("-i!" + n);    // 模块根就是压缩包根
            argv.push_back("-i!*/" + n);  // 外面还包了一层目录
        }
        (void)run(argv);  // 部分图片不存在时 7z 也可能非 0：以实际解出的文件为准
        validate_tree(tmp);
        // 解出的文件：小写相对路径 → 绝对路径
        std::vector<std::pair<std::string, fs::path>> got;
        for (fs::recursive_directory_iterator it(tmp, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            got.emplace_back(std::string(casefold(fs::relative(it->path(), tmp, ec).generic_string())), it->path());
        }
        fs::create_directories(fs::path(std::string(dest_s)), ec);
        std::size_t idx = 0;
        for (const auto& [orig, n] : want) {
            const std::string key = std::string(casefold(n));
            const fs::path* hit = nullptr;
            for (const auto& [rel, abs] : got)
                if (rel == key || (rel.size() > key.size() && rel.ends_with(key) && rel[rel.size() - key.size() - 1] == '/')) { hit = &abs; break; }
            if (!hit) continue;
            // 平铺到 dest：序号 + 原文件名（不同目录下的同名图片不会互相覆盖）
            const fs::path dst = fs::path(std::string(dest_s)) / (std::to_string(idx++) + "-" + sanitize(hit->filename().string()));
            fs::copy_file(*hit, dst, fs::copy_options::overwrite_existing, ec);
            if (!ec) out[orig] = fs::absolute(dst, ec).string();
        }
        fs::remove_all(tmp, ec);
    } catch (...) {
        fs::remove_all(tmp, ec);
        throw;
    }
    return out;
}

}  // namespace mol
