// 实例模型：读取 MO2 实例目录 + mo-linux.json，编排链接农场的期望树。
// 无状态：所有函数只读写磁盘，不缓存。内部临时数据用栈上 arena，结果用调用方的 mem。
#include "mol/instance.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>


#include "mol/casefold.hpp"
#include "mol/mo2fmt.hpp"

import alib6;

namespace mol {
namespace {

namespace fs = std::filesystem;

constexpr const char* kConfigFile = "mo-linux.json";
constexpr const char* kIniFile = "ModOrganizer.ini";

std::string S(std::string_view v) { return std::string(v); }
fs::path P(std::string_view v) { return fs::path(S(v)); }

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
bool path_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != y) return false;
    }
    return true;
}

// 目录 dir 下，名字与 name 大小写不敏感相等的子目录；先试精确匹配；多个候选取字典序最小者。
std::optional<fs::path> find_dir_ci(const fs::path& dir, std::string_view name) {
    if (name.empty()) return std::nullopt;
    fs::path exact = dir / S(name);
    if (is_dir(exact)) return exact;
    std::error_code ec;
    std::string best;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string n = it->path().filename().string();
        if (ieq(n, name) && it->is_directory(ec) && (best.empty() || n < best)) best = n;
    }
    if (best.empty()) return std::nullopt;
    return dir / best;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void atomic_write(const fs::path& target, const std::string& content) {
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw Error("io_error", "cannot create file", tmp.string());
        out << content;
        out.close();
        if (out.fail()) {
            std::error_code ec;
            fs::remove(tmp, ec);
            throw Error("io_error", "cannot write file", tmp.string());
        }
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(tmp, ec2);
        throw Error("io_error", "cannot rename file: " + ec.message(), target.string());
    }
}

void make_dirs(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) throw Error("io_error", "cannot create directory: " + ec.message(), p.string());
}

std::string json_str(const alib6::AData& j, const char* key, const fs::path& file) {
    const auto& obj = j.object();
    auto it = obj.find(key);
    if (it == obj.end() || it.second().is_null()) return {};
    auto v = it.second().is_value() ? it.second().try_to<std::string_view>() : std::nullopt;
    if (!v) throw Error("config_invalid", std::string("field '") + key + "' must be a string", file.string());
    return std::string(*v);
}

alib6::AData load_config_json(const fs::path& file) {
    alib6::AData j;
    if (!path_exists(file)) {
        j._set_object();
        return j;
    }
    if (!j.load_from_memory(read_file(file)))
        throw Error("config_invalid", "invalid JSON", file.string());
    if (!j.is_object()) throw Error("config_invalid", "top level must be an object", file.string());
    return j;
}

// ModOrganizer.ini 的路径值：可能是 Windows 路径，也可能含 %BASE_DIR%。
std::string ini_path_value(const Ini& ini, std::string_view key, std::string_view prefix,
                           std::string_view base) {
    auto v = ini.get("General", key);
    if (!v || v->empty()) return {};
    std::string s(*v);
    const std::string tag = "%BASE_DIR%";
    if (auto pos = s.find(tag); pos != std::string::npos) s.replace(pos, tag.size(), std::string(base));
    return std::string(wine_to_unix(s, prefix));
}

std::string short_name_from_game_name(std::string_view n) {
    if (ieq(n, "Skyrim Special Edition")) return "skyrimse";
    return {};
}

fs::path resolve_against(const fs::path& root, std::string_view v) {
    fs::path p = P(v);
    return p.is_absolute() ? p : root / p;
}

std::string_view effective_profile(const Instance& inst, std::string_view profile) {
    return profile.empty() ? std::string_view(inst.cfg.profile) : profile;
}

fs::path profile_dir_checked(const Instance& inst, std::string_view profile) {
    std::string_view name = effective_profile(inst, profile);
    auto dir = find_dir_ci(P(inst.profiles_dir), name);
    if (!dir) throw Error("profile_not_found", "profile not found: " + S(name), (P(inst.profiles_dir) / S(name)).string());
    return *dir;
}

}  // namespace

// ---------------------------------------------------------------------------
Instance load_instance(std::string_view root_sv, std::string_view profile_override, mr* mem) {
    std::error_code ec;
    fs::path root = fs::absolute(P(root_sv), ec).lexically_normal();
    if (root.has_filename() == false && root.has_parent_path()) root = root.parent_path();
    if (ec || !is_dir(root))
        throw Error("instance_not_found", "instance directory not found", root.string());
    const fs::path cfg_file = root / kConfigFile;
    const fs::path ini_file = root / kIniFile;
    if (!path_exists(cfg_file) && !path_exists(ini_file))
        throw Error("instance_not_found", std::string("neither ") + kConfigFile + " nor " + kIniFile + " found", root.string());

    const alib6::AData j = load_config_json(cfg_file);
    const Ini ini = Ini::load(S(ini_file.string()), mem);

    Instance inst(mem);
    inst.root.assign(root.string());
    InstanceConfig& c = inst.cfg;
    c.game.assign(json_str(j, "game", cfg_file));
    c.game_dir.assign(json_str(j, "game_dir", cfg_file));
    c.prefix.assign(json_str(j, "prefix", cfg_file));
    c.prefix_user.assign(json_str(j, "prefix_user", cfg_file));
    c.profile.assign(json_str(j, "profile", cfg_file));
    c.farm_dir.assign(json_str(j, "farm_dir", cfg_file));
    c.runner_kind.assign(json_str(j, "runner_kind", cfg_file));
    c.proton_path.assign(json_str(j, "proton_path", cfg_file));
    c.steam_root.assign(json_str(j, "steam_root", cfg_file));

    // 缺失字段：从 ModOrganizer.ini 推导，再取默认
    if (c.game.empty()) {
        auto gn = ini.get("General", "gameName");
        std::string sn = gn ? short_name_from_game_name(*gn) : std::string{};
        c.game.assign(sn.empty() ? "skyrimse" : sn);
    }
    if (c.game_dir.empty()) c.game_dir = wine_to_unix(ini.get("General", "gamePath").value_or(string{}), c.prefix, mem);
    if (c.prefix_user.empty()) c.prefix_user.assign("steamuser");
    if (c.profile.empty()) {
        auto sp = ini.get("General", "selected_profile");
        c.profile.assign(sp && !sp->empty() ? std::string(*sp) : std::string("Default"));
    }
    if (!profile_override.empty()) c.profile.assign(profile_override);
    if (c.farm_dir.empty()) c.farm_dir.assign("farm");
    if (c.runner_kind.empty()) c.runner_kind.assign("proton");
    if (c.steam_root.empty()) {
        const char* home = std::getenv("HOME");
        if (home && *home) c.steam_root.assign(std::string(home) + "/.steam/steam");
    }

    std::string base = ini_path_value(ini, "base_directory", c.prefix, root.string());
    fs::path base_dir = base.empty() ? root : resolve_against(root, base);
    auto dir_of = [&](std::string_view key, const char* def) {
        std::string v = ini_path_value(ini, key, c.prefix, base_dir.string());
        return v.empty() ? base_dir / def : resolve_against(root, v);
    };
    inst.mods_dir.assign(dir_of("mod_directory", "mods").lexically_normal().string());
    inst.profiles_dir.assign(dir_of("profiles_directory", "profiles").lexically_normal().string());
    inst.downloads_dir.assign(dir_of("download_directory", "downloads").lexically_normal().string());
    inst.overwrite_dir.assign(dir_of("overwrite_directory", "overwrite").lexically_normal().string());
    inst.farm_path.assign(resolve_against(root, c.farm_dir).lexically_normal().string());
    return inst;
}

bool init_instance(const InitOptions& o) {
    if (o.root.empty()) throw Error("invalid_argument", "instance root is empty");
    std::error_code ec;
    fs::path root = fs::absolute(P(o.root), ec).lexically_normal();
    bool changed = false;
    auto ensure_dir = [&](const fs::path& d) {
        if (!is_dir(d)) {
            make_dirs(d);
            changed = true;
        }
    };
    ensure_dir(root);
    ensure_dir(root / "mods");
    ensure_dir(root / "downloads");
    ensure_dir(root / "overwrite");
    ensure_dir(root / "profiles");
    if (!o.profile.empty()) ensure_dir(root / "profiles" / S(o.profile));

    const fs::path cfg_file = root / kConfigFile;
    alib6::AData j = load_config_json(cfg_file);
    auto set = [&](const char* key, std::string_view v) {
        if (!v.empty()) j[key] = v;
    };
    j["version"] = 1;
    set("game", o.game);
    set("game_dir", o.game_dir);
    set("prefix", o.prefix);
    set("prefix_user", o.prefix_user);
    set("profile", o.profile);
    set("runner_kind", o.runner_kind);
    set("proton_path", o.proton_path);
    set("steam_root", o.steam_root);
    alib6::JSON json{alib6::JSONConfig{.dump_indent = 2, .compact_spaces = true,
                                       .sort_object = alib6::JSONConfig::sort_asc}};
    const auto dumped = j.dump_to_string(json);
    const std::string next = std::string(dumped.data(), dumped.size()) + "\n";
    if (read_file(cfg_file) != next) {
        atomic_write(cfg_file, next);
        changed = true;
    }
    return changed;
}

vector<ModInfo> list_mods(const Instance& inst, std::string_view profile, mr* mem) {
    vector<ModInfo> out(mem);
    auto pdir = find_dir_ci(P(inst.profiles_dir), effective_profile(inst, profile));
    if (!pdir) return out;
    auto entries = read_modlist(S((*pdir / "modlist.txt").string()), mem);
    out.reserve(entries.size());
    std::size_t prio = 0;
    for (const auto& e : entries) {
        ModInfo m(mem);
        m.name.assign(e.name);
        m.enabled = e.enabled;
        m.separator = e.separator;
        m.priority = prio++;
        if (!e.separator) {
            if (auto d = find_dir_ci(P(inst.mods_dir), e.name)) {
                m.exists = true;
                m.path.assign(d->string());
                if (auto v = Ini::load(S((*d / "meta.ini").string()), mem).get("General", "mol_root", mem))
                    m.root = (*v == "true" || *v == "1");
            }
        }
        out.push_back(std::move(m));
    }
    return out;
}

namespace {
// 读入 modlist（低→高），返回目标 profile 的文件路径。
std::pair<std::string, vector<ModEntry>> load_modlist_for_edit(const Instance& inst, std::string_view profile) {
    fs::path pdir = profile_dir_checked(inst, profile);
    std::string file = (pdir / "modlist.txt").string();
    return {file, read_modlist(file)};
}
std::size_t find_mod(const vector<ModEntry>& v, std::string_view name) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (ieq(v[i].name, name)) return i;
    throw Error("mod_not_found", "mod not found in modlist: " + S(name));
}
}  // namespace

bool set_mod_enabled(const Instance& inst, std::string_view name, bool enabled, std::string_view profile) {
    auto [file, mods] = load_modlist_for_edit(inst, profile);
    std::size_t i = find_mod(mods, name);
    if (mods[i].enabled == enabled) return false;
    mods[i].enabled = enabled;
    write_modlist(file, mods);
    return true;
}

void add_mod(const Instance& inst, std::string_view name, bool enabled, std::string_view profile) {
    auto [file, mods] = load_modlist_for_edit(inst, profile);
    for (const auto& m : mods)
        if (ieq(m.name, name)) throw Error("invalid_argument", "mod already exists in modlist: " + S(name));
    ModEntry e;
    e.name.assign(name);
    e.enabled = enabled;
    mods.push_back(std::move(e));
    write_modlist(file, mods);
}

void mark_mod_root(std::string_view mod_dir, bool root) {
    const fs::path f = P(mod_dir) / "meta.ini";
    std::string text = read_file(f);
    // 保留原有内容，只替换/追加 mol_root 行；meta.ini 通常很小
    std::string out;
    bool in_general = false, done = false, saw_general = false;
    std::size_t pos = 0;
    const std::string line_new = std::string("mol_root=") + (root ? "true" : "false");
    while (pos <= text.size()) {
        std::size_t e = text.find('\n', pos);
        const bool last = e == std::string::npos;
        std::string line = text.substr(pos, last ? std::string::npos : e - pos);
        std::string t = line;
        while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
        if (!t.empty() && t.front() == '[') {
            if (in_general && !done) { out += line_new + "\n"; done = true; }
            in_general = ieq(t, "[General]");
            saw_general = saw_general || in_general;
        } else if (in_general && t.rfind("mol_root", 0) == 0 && t.find('=') != std::string::npos) {
            line = line_new;
            done = true;
        }
        if (!(last && line.empty())) out += line + (last ? "" : "\n");
        if (last) break;
        pos = e + 1;
    }
    if (!done) {
        if (!out.empty() && out.back() != '\n') out += "\n";
        if (!saw_general) out += "[General]\n";
        out += line_new + "\n";
    }
    atomic_write(f, out);
}

bool move_mod(const Instance& inst, std::string_view name, std::size_t to_priority, std::string_view profile) {
    auto [file, mods] = load_modlist_for_edit(inst, profile);
    std::size_t i = find_mod(mods, name);
    std::size_t to = std::min(to_priority, mods.size() - 1);
    if (i == to) return false;
    ModEntry m = std::move(mods[i]);
    mods.erase(mods.begin() + static_cast<std::ptrdiff_t>(i));
    mods.insert(mods.begin() + static_cast<std::ptrdiff_t>(to), std::move(m));
    write_modlist(file, mods);
    return true;
}

FarmModel build_farm_model(const Instance& inst, std::string_view profile, mr* mem) {
    if (inst.cfg.game_dir.empty() || !is_dir(P(inst.cfg.game_dir)))
        throw Error("config_invalid", "game_dir is not set or does not exist", S(inst.cfg.game_dir));

    FarmModel model(mem);
    vector<vector<ScanEntry>> layers(mem);
    auto add = [&](std::string_view name, std::string_view root, std::string_view prefix, bool is_mod = false) {
        model.layer_names.emplace_back(name);
        auto entries = scan_layer(root, prefix, mem);
        if (is_mod) {  // mod 根下的 meta.ini 是 MO2/我们自己的元数据，不进农场
            const std::string meta = prefix.empty() ? "meta.ini" : std::string(prefix) + "/meta.ini";
            std::erase_if(entries, [&](const ScanEntry& e) { return !e.is_dir && ieq(e.rel, meta); });
        }
        layers.push_back(std::move(entries));
    };
    add("<game>", inst.cfg.game_dir, "");
    std::vector<Warning> pre;
    for (const auto& m : list_mods(inst, profile, mem)) {
        if (m.separator || !m.enabled) continue;
        if (!m.exists) {
            Warning w(mem);
            w.path.assign(m.name);
            w.message.assign("mod directory is missing; skipped");
            pre.push_back(std::move(w));
            continue;
        }
        add(m.name, m.path, m.root ? "" : "Data", true);
    }
    if (is_dir(P(inst.overwrite_dir))) add("<overwrite>", inst.overwrite_dir, "Data");

    model.merged = merge_listings(layers, mem);
    for (auto& w : pre) model.merged.warnings.push_back(std::move(w));
    std::sort(model.merged.warnings.begin(), model.merged.warnings.end(), [](const Warning& a, const Warning& b) {
        return a.path != b.path ? a.path < b.path : a.message < b.message;
    });
    return model;
}

Plan plan_instance(const Instance& inst, const FarmModel& model, mr* mem) {
    try {
        return plan_farm(model.merged, inst.farm_path, mem);
    } catch (const std::runtime_error& e) {
        std::string m = e.what();
        if (m.find("refusing") != std::string::npos) throw Error("farm_conflict", m, S(inst.farm_path));
        if (m.find("not empty") != std::string::npos || m.find("not a directory") != std::string::npos)
            throw Error("farm_not_owned", m, S(inst.farm_path));
        throw Error("io_error", m, S(inst.farm_path));
    }
}

void apply_instance(const Instance& inst, const Plan& plan) {
    try {
        apply_farm(plan, inst.farm_path);
    } catch (const Error&) {
        throw;
    } catch (const std::runtime_error& e) {
        throw Error("io_error", e.what(), S(inst.farm_path));
    }
}

}  // namespace mol
