#include "mol/plugins.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include "mol/casefold.hpp"
#include "mol/mo2fmt.hpp"

namespace mol {
namespace fs = std::filesystem;
namespace {

std::uint32_t u32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint16_t u16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

bool is_plugin_name(std::string_view n) {
    if (n.size() < 4) return false;
    const auto ext = casefold(n.substr(n.size() - 4));
    return ext == ".esp" || ext == ".esm" || ext == ".esl";
}
bool ends_esm(std::string_view n) { return n.size() >= 4 && casefold(n.substr(n.size() - 4)) == ".esm"; }

std::size_t find_idx(const PluginList& l, std::string_view name) {
    const auto want = casefold(name);
    for (std::size_t i = 0; i < l.rows.size(); ++i)
        if (casefold(l.rows[i].name) == want) return i;
    return l.rows.size();
}

// 强制插件 → ESM 区 → 其它；各区内保持原相对顺序。返回是否有变化。
bool normalize(PluginList& l) {
    auto group = [](const PluginRow& r) { return r.forced ? 0 : r.master ? 1 : 2; };
    std::vector<PluginRow> before(l.rows.begin(), l.rows.end());
    std::stable_sort(l.rows.begin(), l.rows.end(), [&](const PluginRow& a, const PluginRow& b) { return group(a) < group(b); });
    for (std::size_t i = 0; i < before.size(); ++i)
        if (before[i].name != l.rows[i].name) return true;
    return false;
}

fs::path profile_path(const Instance& inst, std::string_view profile) {
    const std::string p = profile.empty() ? std::string(inst.cfg.profile) : std::string(profile);
    const fs::path base{std::string(inst.profiles_dir)};
    std::error_code ec;
    const auto want = casefold(p);
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec))
        if (casefold(it->path().filename().string()) == want && it->is_directory(ec)) return it->path();
    throw Error("profile_not_found", "profile directory missing", (base / p).string());
}

}  // namespace

PluginHeader read_plugin_header(std::string_view path, mr* mem) {
    PluginHeader h(mem);
    std::ifstream in{std::string(path), std::ios::binary};
    if (!in) throw Error("io_error", "cannot open plugin", std::string(path));
    unsigned char hd[24];
    in.read(reinterpret_cast<char*>(hd), sizeof hd);
    if (in.gcount() != 24 || std::memcmp(hd, "TES4", 4) != 0) throw Error("invalid_argument", "not a TES4 plugin", std::string(path));
    const std::uint32_t size = u32(hd + 4), flags = u32(hd + 8);
    h.master_flag = (flags & 0x1) != 0;
    h.light_flag = (flags & 0x200) != 0;
    if (size > (16u << 20)) throw Error("invalid_argument", "implausible TES4 header size", std::string(path));
    std::vector<unsigned char> buf(size);
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
    if (static_cast<std::uint32_t>(in.gcount()) != size) throw Error("invalid_argument", "truncated TES4 header", std::string(path));
    std::size_t pos = 0;
    while (pos + 6 <= buf.size()) {
        const std::uint16_t sz = u16(&buf[pos + 4]);
        const char* type = reinterpret_cast<const char*>(&buf[pos]);
        pos += 6;
        if (pos + sz > buf.size()) break;
        if (std::memcmp(type, "MAST", 4) == 0) {
            std::size_t len = 0;
            while (len < sz && buf[pos + len] != 0) ++len;
            h.masters.emplace_back(reinterpret_cast<const char*>(&buf[pos]), len);
        }
        pos += sz;
    }
    return h;
}

vector<string> default_forced_plugins(mr* mem) {
    vector<string> v(mem);
    for (const char* n : {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"}) v.emplace_back(n);
    return v;
}

PluginList load_plugins(const Instance& inst, std::span<const string> forced_in, std::string_view profile, mr* mem) {
    PluginList out(mem);
    vector<string> forced(mem);
    if (forced_in.empty()) forced = default_forced_plugins(mem);
    else forced.assign(forced_in.begin(), forced_in.end());

    // 磁盘上可用的插件（赢家）：插件只在 Data/ 顶层，不必遍历整棵树
    const FarmModel model = build_data_top_model(inst, profile, mem);
    std::map<string, PluginRow, std::less<>> avail;  // 键：casefold 名
    std::vector<string> discovery_order;
    for (const auto& e : model.merged.entries) {
        if (e.is_dir || e.path.size() < 6 || e.path.compare(0, 5, "Data/") != 0) continue;
        const std::string_view name = std::string_view(e.path).substr(5);
        if (name.find('/') != std::string_view::npos || !is_plugin_name(name)) continue;
        PluginRow r(mem);
        r.name = string(name, mem);
        r.path = e.source;
        r.source = model.layer_names[e.layer];
        r.master = ends_esm(name);
        try {
            const PluginHeader h = read_plugin_header(r.path, mem);
            r.master = r.master || h.master_flag;
            r.light = h.light_flag || (name.size() >= 4 && casefold(name.substr(name.size() - 4)) == ".esl");
            r.masters = h.masters;
        } catch (const Error&) {
            // 头读不出来：仍列出，按扩展名判断类型，不做依赖检查
        }
        const auto key = casefold(name);
        discovery_order.push_back(key);
        avail.emplace(key, std::move(r));
    }

    // profile 里已有的顺序与启用状态
    const fs::path pdir = profile_path(inst, profile);
    const auto lo = read_loadorder_txt((pdir / "loadorder.txt").string(), mem);
    const auto pl = read_plugins_txt((pdir / "plugins.txt").string(), mem);
    std::map<string, bool, std::less<>> enabled_in_file;
    for (const auto& p : pl) enabled_in_file[casefold(p.name, mem)] = p.enabled;

    std::vector<string> order;
    if (!lo.empty()) for (const auto& n : lo) order.push_back(casefold(n, mem));
    else for (const auto& p : pl) order.push_back(casefold(p.name, mem));

    std::map<string, bool, std::less<>> placed;
    auto place = [&](const string& key) {
        auto it = avail.find(key);
        if (it == avail.end() || placed[key]) return;
        placed[key] = true;
        PluginRow r = it->second;
        const auto ef = enabled_in_file.find(key);
        r.enabled = ef != enabled_in_file.end() ? ef->second : true;  // 新发现的默认启用
        out.rows.push_back(std::move(r));
    };
    for (const auto& n : forced) place(casefold(n, mem));
    for (const auto& k : order) place(k);
    for (const auto& k : discovery_order) place(k);

    for (auto& r : out.rows) {
        for (const auto& n : forced)
            if (casefold(n) == casefold(r.name)) { r.forced = true; r.enabled = true; }
    }
    normalize(out);
    return out;
}

void save_plugins(const Instance& inst, const PluginList& list, std::string_view profile) {
    const fs::path pdir = profile_path(inst, profile);
    std::vector<PluginEntry> pe;
    std::vector<std::string> names;
    for (const auto& r : list.rows) {
        PluginEntry e;
        e.name = r.name;
        e.enabled = r.enabled;
        pe.push_back(std::move(e));
        names.emplace_back(r.name);
    }
    write_plugins_txt((pdir / "plugins.txt").string(), pe);
    // loadorder.txt：每行一个插件名
    const fs::path lo = pdir / "loadorder.txt";
    const fs::path tmp = pdir / "loadorder.txt.mol-tmp";
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os) throw Error("io_error", "cannot write loadorder.txt", tmp.string());
        os << "# This file was automatically generated by mo-linux.\r\n";
        for (const auto& n : names) os << n << "\r\n";
        os.flush();
        if (!os) throw Error("io_error", "write failed", tmp.string());
    }
    std::error_code ec;
    fs::rename(tmp, lo, ec);
    if (ec) throw Error("io_error", "cannot replace loadorder.txt: " + ec.message(), lo.string());
}

bool plugin_set_enabled(PluginList& l, std::string_view name, bool enabled) {
    const std::size_t i = find_idx(l, name);
    if (i == l.rows.size()) throw Error("mod_not_found", "no such plugin: " + std::string(name));
    if (l.rows[i].forced && !enabled) throw Error("invalid_argument", "cannot disable a game plugin: " + std::string(l.rows[i].name));
    if (l.rows[i].enabled == enabled) return false;
    l.rows[i].enabled = enabled;
    return true;
}

bool plugin_move(PluginList& l, std::string_view name, std::size_t to) {
    const std::size_t i = find_idx(l, name);
    if (i == l.rows.size()) throw Error("mod_not_found", "no such plugin: " + std::string(name));
    to = std::min(to, l.rows.size() - 1);
    std::vector<string> before;
    for (const auto& r : l.rows) before.push_back(r.name);
    PluginRow r = std::move(l.rows[i]);
    l.rows.erase(l.rows.begin() + static_cast<std::ptrdiff_t>(i));
    l.rows.insert(l.rows.begin() + static_cast<std::ptrdiff_t>(to), std::move(r));
    normalize(l);
    for (std::size_t k = 0; k < before.size(); ++k)
        if (before[k] != l.rows[k].name) return true;
    return false;
}

bool plugin_sort_by_masters(PluginList& l) {
    normalize(l);
    const std::size_t n = l.rows.size();
    std::map<string, std::size_t, std::less<>> idx;
    for (std::size_t i = 0; i < n; ++i) idx[casefold(l.rows[i].name)] = i;
    // Kahn：每次取「所有 masters 都已放好」的最靠前者（稳定）；成环的剩余部分保持原顺序追加
    std::vector<bool> done(n, false);
    std::vector<std::size_t> order;
    order.reserve(n);
    while (order.size() < n) {
        bool progressed = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (done[i]) continue;
            bool ready = true;
            for (const auto& m : l.rows[i].masters) {
                auto it = idx.find(casefold(m));
                if (it != idx.end() && !done[it->second] && it->second != i) { ready = false; break; }
            }
            if (ready) {
                done[i] = true;
                order.push_back(i);
                progressed = true;
                break;
            }
        }
        if (!progressed) {
            for (std::size_t i = 0; i < n; ++i) if (!done[i]) { done[i] = true; order.push_back(i); }
        }
    }
    bool changed = false;
    std::vector<PluginRow> out;
    out.reserve(n);
    for (std::size_t k = 0; k < n; ++k) {
        if (order[k] != k) changed = true;
        out.push_back(l.rows[order[k]]);
    }
    l.rows.assign(out.begin(), out.end());
    // 区域边界（强制/ESM/其它）不能被打破：若依赖排序造成越界，再 normalize 一次（稳定，不会破坏区内的依赖序）
    if (normalize(l)) changed = true;
    return changed;
}

vector<MasterIssue> check_masters(const PluginList& l, mr* mem) {
    vector<MasterIssue> out(mem);
    std::map<string, std::size_t, std::less<>> idx;
    for (std::size_t i = 0; i < l.rows.size(); ++i) idx[casefold(l.rows[i].name)] = i;
    for (std::size_t i = 0; i < l.rows.size(); ++i) {
        const auto& r = l.rows[i];
        if (!r.enabled) continue;
        for (const auto& m : r.masters) {
            MasterIssue is(mem);
            is.plugin = r.name;
            is.master = m;
            auto it = idx.find(casefold(m));
            if (it == idx.end()) is.kind = "missing";
            else if (!l.rows[it->second].enabled) is.kind = "disabled";
            else if (it->second > i) is.kind = "after";
            else continue;
            out.push_back(std::move(is));
        }
    }
    return out;
}

}  // namespace mol
