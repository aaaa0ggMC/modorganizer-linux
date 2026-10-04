#include "mol/overwrite.hpp"

#include <filesystem>
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

}  // namespace mol
