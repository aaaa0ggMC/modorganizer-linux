#include "mol/overwrite.hpp"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "mol/casefold.hpp"
#include "mol/linkfarm.hpp"

namespace mol {
namespace fs = std::filesystem;

std::size_t capture_overwrite(const Instance& inst) {
    const fs::path farm{std::string(inst.farm_path)};
    std::error_code ec;
    if (!fs::is_directory(farm, ec)) return 0;

    std::vector<fs::path> found;
    for (fs::recursive_directory_iterator it(farm, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        const auto st = it->symlink_status(ec);
        if (ec) break;
        if (!fs::is_regular_file(st)) continue;  // 符号链接、目录都跳过
        if (it->path().filename() == kFarmMarker && it->path().parent_path() == farm) continue;
        found.push_back(it->path());
    }
    if (ec) throw Error("io_error", "capture_overwrite: scan failed: " + ec.message(), farm.string());

    std::size_t moved = 0;
    for (const auto& f : found) {
        const fs::path rel = fs::relative(f, farm);
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
    return moved;
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
