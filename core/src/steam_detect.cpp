#include "mol/steam_detect.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <system_error>
#include <vector>

namespace mol {
namespace fs = std::filesystem;
namespace {

bool isdir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

// Steam 根里 libraryfolders.vdf 的 "path" 行。
std::vector<fs::path> libraries(const fs::path& root) {
    std::vector<fs::path> out{root};
    std::ifstream in(root / "steamapps" / "libraryfolders.vdf");
    std::string line;
    static const std::regex re(R"re("path"\s+"([^"]*)")re");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            std::string p = std::regex_replace(m[1].str(), std::regex(R"(\\\\)"), "/");
            if (!p.empty() && fs::path(p) != root) out.emplace_back(p);
        }
    }
    return out;
}

// "Proton 9.0 (Beta)" → 9.0 ；其余名字排在后面。
double proton_rank(const std::string& name) {
    static const std::regex re(R"(^Proton[ -]?(\d+)(?:\.(\d+))?)");
    std::smatch m;
    if (std::regex_search(name, m, re)) return std::stod(m[1].str()) + (m[2].matched ? std::stod(m[2].str()) / 100.0 : 0.0);
    if (name.rfind("Proton", 0) == 0) return 0.5;  // Experimental / Hotfix
    return 0.1;                                    // 社区版（GE 等）
}

}  // namespace

SteamDetect detect_steam(std::string_view home_in, mr* mem) {
    SteamDetect d(mem);
    std::string home(home_in);
    if (home.empty()) {
        const char* h = std::getenv("HOME");
        if (!h) return d;
        home = h;
    }
    const fs::path h{home};
    for (const fs::path& c : {h / ".local/share/Steam", h / ".steam/steam", h / ".var/app/com.valvesoftware.Steam/.local/share/Steam"}) {
        if (isdir(c / "steamapps")) {
            std::error_code ec;
            d.steam_root = string(fs::weakly_canonical(c, ec).string(), mem);
            break;
        }
    }
    if (d.steam_root.empty()) return d;
    const fs::path root{std::string(d.steam_root)};

    for (const auto& lib : libraries(root)) {
        const fs::path g = lib / "steamapps/common/Skyrim Special Edition";
        if (!isdir(g)) continue;
        d.game_dir = string(g.string(), mem);
        const fs::path pfx = lib / "steamapps/compatdata/489830/pfx";
        if (isdir(pfx)) d.prefix = string(pfx.string(), mem);
        break;
    }

    std::vector<std::pair<double, fs::path>> cands;
    auto scan = [&](const fs::path& dir) {
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().string();
            if (isdir(it->path()) && fs::exists(it->path() / "proton", ec)) cands.emplace_back(proton_rank(name), it->path());
        }
    };
    for (const auto& lib : libraries(root)) scan(lib / "steamapps/common");
    scan(root / "compatibilitytools.d");
    std::stable_sort(cands.begin(), cands.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    if (!cands.empty()) d.proton_path = string(cands.front().second.string(), mem);
    return d;
}

}  // namespace mol
