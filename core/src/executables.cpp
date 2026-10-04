#include <cstring>
#include "mol/executables.hpp"

#include <filesystem>

#include "mol/casefold.hpp"
#include "mol/mo2fmt.hpp"

namespace mol {
namespace fs = std::filesystem;
namespace {

std::string replace_base_dir(std::string v, const std::string& root) {
    for (const char* tag : {"%BASE_DIR%", "%base_dir%"}) {
        for (std::size_t pos = 0; (pos = v.find(tag, pos)) != std::string::npos; pos += root.size()) v.replace(pos, std::strlen(tag), root);
    }
    return v;
}

bool under(const fs::path& base, const fs::path& p, fs::path& rel) {
    std::error_code ec;
    const fs::path b = fs::weakly_canonical(base, ec), x = fs::weakly_canonical(p, ec);
    auto bi = b.begin(), xi = x.begin();
    for (; bi != b.end(); ++bi, ++xi) {
        if (xi == x.end() || casefold(bi->string()) != casefold(xi->string())) return false;
    }
    rel.clear();
    for (; xi != x.end(); ++xi) rel /= *xi;
    return !rel.empty();
}

}  // namespace

string farm_relative(const Instance& inst, std::string_view unix_path, mr* mem) {
    const fs::path p{std::string(unix_path)};
    fs::path rel;
    if (!inst.cfg.game_dir.empty() && under(fs::path(std::string(inst.cfg.game_dir)), p, rel)) return string(rel.generic_string(), mem);
    if (under(fs::path(std::string(inst.mods_dir)), p, rel)) {
        auto it = rel.begin();
        const std::string mod = it->string();
        fs::path rest;
        for (++it; it != rel.end(); ++it) rest /= *it;
        if (rest.empty()) return string(mem);
        for (const auto& m : list_mods(inst, {}, mem)) {
            if (casefold(m.name) != casefold(mod)) continue;
            return string((m.root ? rest : fs::path("Data") / rest).generic_string(), mem);
        }
    }
    return string(mem);
}

vector<Executable> list_executables(const Instance& inst, mr* mem) {
    vector<Executable> out(mem);
    const Ini ini = Ini::load((fs::path(std::string(inst.root)) / "ModOrganizer.ini").string(), mem);
    int size = 0;
    if (auto v = ini.get("customExecutables", "size", mem)) size = std::atoi(std::string(*v).c_str());
    const std::string root(inst.root);
    for (int i = 1; i <= size; ++i) {
        const std::string pre = std::to_string(i) + "\\";
        auto get = [&](const char* k) { return ini.get("customExecutables", pre + k, mem); };
        auto title = get("title");
        auto binary = get("binary");
        if (!title || !binary || title->empty() || binary->empty()) continue;
        Executable e(mem);
        e.title = *title;
        e.binary = wine_to_unix(replace_base_dir(std::string(*binary), root), inst.cfg.prefix, mem);
        if (auto a = get("arguments")) e.arguments = *a;
        if (auto w = get("workingDirectory"); w && !w->empty()) e.working_dir = wine_to_unix(replace_base_dir(std::string(*w), root), inst.cfg.prefix, mem);
        if (auto h = get("hide")) e.hide = (*h == "true" || *h == "1");
        e.farm_path = farm_relative(inst, e.binary, mem);
        out.push_back(std::move(e));
    }
    return out;
}

vector<string> split_arguments(std::string_view s, mr* mem) {
    vector<string> out(mem);
    std::string cur;
    bool have = false;
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == quote) quote = 0;
            else if (c == '\\' && quote == '"' && i + 1 < s.size() && (s[i + 1] == '"' || s[i + 1] == '\\')) cur.push_back(s[++i]);
            else cur.push_back(c);
        } else if (c == '"' || c == '\'') {
            quote = c;
            have = true;
        } else if (c == ' ' || c == '\t') {
            if (have || !cur.empty()) { out.push_back(string(cur, mem)); cur.clear(); have = false; }
        } else {
            cur.push_back(c);
        }
    }
    if (have || !cur.empty()) out.push_back(string(cur, mem));
    return out;
}

}  // namespace mol
