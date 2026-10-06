#include "mol/overwrite.hpp"

#include <fcntl.h>
#include <fnmatch.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "mol/casefold.hpp"
#include "mol/linkfarm.hpp"

namespace mol {
namespace fs = std::filesystem;

std::string cow_log_path(const Instance& inst) { return std::string(inst.root) + "/.mol-cow.log"; }

CowStats cow_stats(const Instance& inst) {
    CowStats st;
    std::ifstream in(cow_log_path(inst));
    std::string line;
    while (std::getline(in, line)) {
        if (line.find('\t') == std::string::npos) continue;
        ++st.copies;
        if (line.size() >= 8 && line.compare(line.size() - 8, 8, "\treflink") == 0) ++st.reflinked;
    }
    return st;
}

bool clone_or_copy_file(const std::string& src, const std::string& dst) {
    const int in = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    struct stat st {};
    if (::fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(in);
        return false;
    }
    const int out = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) {
        ::close(in);
        return false;
    }
    bool ok = ::ioctl(out, FICLONE, in) == 0;  // btrfs/xfs：reflink
    ::close(out);
    ::close(in);
    if (!ok) {  // 其它文件系统：普通复制（libstdc++ 内部用 copy_file_range/sendfile）
        std::error_code ec;
        ok = fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec) && !ec;
    }
    return ok;
}

namespace {

bool same_content(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    const auto sa = fs::file_size(a, ec);
    if (ec) return false;
    const auto sb = fs::file_size(b, ec);
    if (ec || sa != sb) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    std::vector<char> ba(1 << 16), bb(1 << 16);
    while (fa && fb) {
        fa.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        fb.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        if (fa.gcount() != fb.gcount()) return false;
        if (std::memcmp(ba.data(), bb.data(), static_cast<std::size_t>(fa.gcount())) != 0) return false;
    }
    return true;
}

// 农场根下 mo-linux 自己的文件（marker、COW 锁/临时文件）不是游戏/工具的产物
bool internal_name(const fs::path& farm, const fs::path& p) {
    const std::string n = p.filename().string();
    return n.rfind(".mol-", 0) == 0 && (p.parent_path() == farm || n.rfind(".mol-cow.", 0) == 0);
}

}  // namespace

std::size_t capture_overwrite(const Instance& inst) {
    const fs::path farm{std::string(inst.farm_path)};
    std::error_code ec;
    if (!fs::is_directory(farm, ec)) return 0;

    // 1) COW 日志：工具以写方式打开过的链接已被 libmol-cow 换成真实副本。内容与原文件相同（只是以写方式
    //    打开、没改）的副本直接删掉——下次 apply 恢复成链接；改过的照常收进 overwrite。
    const std::string log = cow_log_path(inst);
    {
        std::ifstream in(log);
        std::string line;
        while (std::getline(in, line)) {
            const auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            const auto tab2 = line.find('\t', tab + 1);  // 第三列（reflink|copy）可能没有
            const fs::path copy = farm / line.substr(0, tab);
            const fs::path orig = line.substr(tab + 1, tab2 == std::string::npos ? std::string::npos : tab2 - tab - 1);
            const auto st = fs::symlink_status(copy, ec);
            if (ec || !fs::is_regular_file(st)) { ec.clear(); continue; }
            if (same_content(copy, orig)) fs::remove(copy, ec);
            ec.clear();
        }
    }

    // 2) 扫农场：真实文件 = 游戏/工具的产物；不在 manifest 里的符号链接 = 工具挪动/改名了我们的链接
    //    （例如降级补丁把 SkyrimSE.exe 挪进备份目录）→ 换成真实副本（reflink/复制），保证备份真的是备份。
    // manifest 读不出来（损坏、不是我们的农场）就完全不处理「多出来的链接」：否则所有链接都会被当成工具挪动的
    std::vector<std::string> manifest;
    bool have_manifest = false;
    try {
        for (const auto& m : farm_manifest(inst.farm_path)) manifest.emplace_back(m);
        have_manifest = !manifest.empty();
    } catch (const std::exception&) {
    }
    std::sort(manifest.begin(), manifest.end());
    std::vector<fs::path> found;
    std::vector<fs::path> stray;
    for (fs::recursive_directory_iterator it(farm, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        const auto st = it->symlink_status(ec);
        if (ec) break;
        if (internal_name(farm, it->path())) continue;
        if (fs::is_regular_file(st)) {
            found.push_back(it->path());
        } else if (have_manifest && fs::is_symlink(st)) {
            const std::string rel = it->path().lexically_relative(farm).generic_string();  // 不能用 fs::relative：它会顺着链接解析
            if (!std::binary_search(manifest.begin(), manifest.end(), rel)) stray.push_back(it->path());
        }
    }
    if (ec) throw Error("io_error", "capture_overwrite: scan failed: " + ec.message(), farm.string());
    for (const auto& l : stray) {
        std::error_code sec;
        if (!fs::is_regular_file(fs::status(l, sec)) || sec) continue;  // 断链或指向目录：不处理
        const fs::path tmp = l.parent_path() / (".mol-cow.materialize.tmp");
        if (clone_or_copy_file(fs::canonical(l, sec).string(), tmp.string())) {
            fs::rename(tmp, l, sec);
            if (!sec) found.push_back(l);
        }
        fs::remove(tmp, sec);
    }

    std::size_t moved = 0;
    for (const auto& f : found) {
        const fs::path rel = f.lexically_relative(farm);
        auto first = rel.begin();
        const bool in_data = first != rel.end() && casefold(first->string()) == "data";
        fs::path dest;
        if (in_data) {
            fs::path sub;
            for (auto i = std::next(first); i != rel.end(); ++i) sub /= *i;
            dest = fs::path(std::string(inst.overwrite_dir)) / sub;
        } else {
            dest = fs::path(std::string(inst.root)) / "overwrite-root" / rel;
        }
        fs::create_directories(dest.parent_path(), ec);
        if (ec) throw Error("io_error", "capture_overwrite: mkdir failed: " + ec.message(), dest.parent_path().string());
        if (fs::exists(fs::symlink_status(dest, ec))) {
            const fs::path bak = fs::path(std::string(inst.root)) / "overwrite-backup" / fs::relative(dest, fs::path(std::string(inst.overwrite_dir)).parent_path());
            fs::create_directories(bak.parent_path(), ec);
            fs::rename(dest, bak, ec);
            if (ec) throw Error("io_error", "capture_overwrite: backup failed: " + ec.message(), dest.string());
        }
        fs::rename(f, dest, ec);
        if (ec) {  // 跨文件系统：复制后删除
            ec.clear();
            fs::copy_file(f, dest, fs::copy_options::overwrite_existing, ec);
            if (ec) throw Error("io_error", "capture_overwrite: move failed: " + ec.message(), f.string());
            fs::remove(f, ec);
        }
        ++moved;
        // 清理因此变空的真实目录（只往上走到农场根为止）
        for (fs::path d = f.parent_path(); d != farm && d.string().size() > farm.string().size(); d = d.parent_path()) {
            if (!fs::is_empty(d, ec) || ec) break;
            fs::remove(d, ec);
        }
    }
    // 3) 本轮 COW 处理完毕：清掉日志与锁
    fs::remove(log, ec);
    fs::remove(farm / ".mol-cow.lock", ec);
    return moved;
}

namespace {
// 逐级大小写不敏感地解析：已有的目录/文件沿用磁盘上的写法，不存在的部分按原样。
fs::path resolve_ci_under(const fs::path& base, const fs::path& rel) {
    fs::path cur = base;
    for (const auto& comp : rel) {
        const auto want = casefold(comp.string());
        fs::path pick = cur / comp;
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(pick, ec))) {
            for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec)) {
                if (casefold(it->path().filename().string()) == want) { pick = it->path(); break; }
            }
        }
        cur = pick;
    }
    return cur;
}
}  // namespace

vector<PromoteEntry> promote_overwrite(const Instance& inst, std::span<const std::string> filters, bool execute, mr* mem) {
    vector<PromoteEntry> out(mem);
    if (filters.empty()) return out;
    if (inst.cfg.game_dir.empty()) throw Error("config_invalid", "game_dir is not set");
    const fs::path ow{std::string(inst.overwrite_dir)};
    const fs::path data = resolve_ci_under(fs::path(std::string(inst.cfg.game_dir)), "Data");
    std::error_code ec;
    if (!fs::is_directory(ow, ec)) return out;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(ow, ec), end; !ec && it != end; it.increment(ec)) {
        const auto st = it->symlink_status(ec);
        if (!ec && fs::is_regular_file(st)) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        const fs::path rel = fs::relative(f, ow);
        bool hit = false;
        for (const auto& pat : filters)
            if (::fnmatch(pat.c_str(), rel.string().c_str(), FNM_CASEFOLD) == 0) { hit = true; break; }
        if (!hit) continue;
        const fs::path dest = resolve_ci_under(data, rel);
        PromoteEntry e(mem);
        e.path = string(rel.string(), mem);
        e.dest = string(dest.string(), mem);
        e.skipped = fs::exists(fs::symlink_status(dest, ec));
        if (execute && !e.skipped) {
            fs::create_directories(dest.parent_path(), ec);
            if (ec) throw Error("io_error", "promote: mkdir failed: " + ec.message(), dest.parent_path().string());
            fs::rename(f, dest, ec);
            if (ec) {
                ec.clear();
                fs::copy_file(f, dest, fs::copy_options::none, ec);
                if (ec) throw Error("io_error", "promote: copy failed: " + ec.message(), dest.string());
                fs::remove(f, ec);
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

bool farm_in_use(const Instance& inst) {
    std::error_code ec;
    const fs::path farm = fs::weakly_canonical(fs::path(std::string(inst.farm_path)), ec);
    if (farm.empty()) return false;
    const std::string fwd = farm.string();
    std::string bwd = fwd;
    std::replace(bwd.begin(), bwd.end(), '/', '\\');
    const pid_t self = ::getpid();
    for (fs::directory_iterator it("/proc", fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        if (std::stol(name) == self) continue;
        std::error_code e2;
        const fs::path cwd = fs::read_symlink(it->path() / "cwd", e2);
        if (!e2) {
            const std::string c = cwd.string();
            if (c == fwd || c.rfind(fwd + "/", 0) == 0) return true;
        }
        std::ifstream in(it->path() / "cmdline", std::ios::binary);
        std::string cmd((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (cmd.find(fwd + "/") != std::string::npos || cmd.find(bwd + "\\") != std::string::npos) return true;
    }
    return false;
}

void require_farm_idle(const Instance& inst) {
    if (farm_in_use(inst))
        throw Error("farm_busy", "the farm is in use by a running process (is the game still running?)", inst.farm_path.c_str());
}

}  // namespace mol
