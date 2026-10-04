#include "mol/wabbajack_install.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "mol/casefold.hpp"
#include "mol/http.hpp"
#include "mol/mod_install.hpp"
#include "mol/xxh64.hpp"

import alib6;

extern char** environ;

namespace mol::wabbajack {
namespace fs = std::filesystem;
namespace {

std::string lower(std::string_view s) { return std::string(casefold(s)); }

std::string norm(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// Directive.To / 压缩包内路径 → 相对路径；拒绝绝对路径与 ".."。
fs::path safe_rel(const std::string& p) {
    const std::string n = norm(p);
    fs::path rel;
    std::size_t i = 0;
    while (i <= n.size()) {
        std::size_t j = n.find('/', i);
        if (j == std::string::npos) j = n.size();
        const std::string comp = n.substr(i, j - i);
        i = j + 1;
        if (comp.empty() || comp == ".") { if (j == n.size()) break; continue; }
        if (comp == "..") throw Error("invalid_argument", "wabbajack: path escapes the install directory: " + p);
        rel /= comp;
        if (j == n.size()) break;
    }
    if (rel.empty()) throw Error("invalid_argument", "wabbajack: empty path");
    return rel;
}

// 大小写不敏感地在 base 下找 rel；找不到返回空。
fs::path find_ci(const fs::path& base, const fs::path& rel) {
    fs::path cur = base;
    for (const auto& comp : rel) {
        const std::string want = lower(comp.string());
        std::error_code ec;
        fs::path next;
        if (fs::exists(cur / comp, ec)) next = cur / comp;
        else
            for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec))
                if (lower(it->path().filename().string()) == want) { next = it->path(); break; }
        if (next.empty()) return {};
        cur = next;
    }
    return cur;
}

std::string to_windows(std::string_view unix_path, bool double_back, bool forward) {
    std::string p = "Z:" + std::string(unix_path);
    if (forward) return p;
    std::string out;
    for (char c : p) {
        if (c == '/') out += double_back ? "\\\\" : "\\";
        else out.push_back(c);
    }
    return out;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (std::size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) s.replace(pos, from.size(), to);
}

bool run(const std::vector<std::string>& argv) {
    std::vector<char*> av;
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return false;
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0) { if (errno != EINTR) return false; }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

bool extract_to(const fs::path& archive, const fs::path& dest) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    return run({"7z", "x", "-y", "-bd", "-o" + dest.string(), archive.string()}) || run({"7zz", "x", "-y", "-bd", "-o" + dest.string(), archive.string()}) ||
           run({"bsdtar", "-xf", archive.string(), "-C", dest.string()});
}

std::string safe_name(const std::string& s) {
    std::string o;
    for (char c : s) o.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return o;
}

// 状态文件：已处理完的压缩包 + 已校验过的下载（路径→大小）
struct State {
    std::set<std::string> done;
    std::map<std::string, std::int64_t> verified;  // 路径 → 校验时的大小
};
State load_state(const fs::path& f) {
    State s;
    std::ifstream in(f, std::ios::binary);
    if (!in) return s;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    alib6::AData doc(default_mr());
    if (!doc.load_from_memory(text) || !doc.is_object()) return s;
    if (auto it = doc.object().find("done"); it != doc.object().end() && it.second().is_array())
        for (const auto& v : it.second().array()) if (auto x = v.try_to<std::string_view>()) s.done.insert(std::string(*x));
    if (auto it = doc.object().find("verified"); it != doc.object().end() && it.second().is_object())
        for (const auto& [k, v] : it.second().object()) if (auto n = v.try_to<long long>()) s.verified[std::string(k)] = *n;
    return s;
}
void save_state(const fs::path& f, const State& s) {
    alib6::AData doc(default_mr());
    doc["version"] = 1;
    auto& d = doc["done"];
    d._set_array();
    std::ptrdiff_t i = 0;
    for (const auto& h : s.done) d[i++] = std::string_view(h);
    auto& v = doc["verified"];
    v._set_object();
    for (const auto& [k, n] : s.verified) v[std::string_view(k)] = static_cast<long long>(n);
    alib6::JSON json{alib6::JSONConfig{.dump_indent = 2, .compact_spaces = true, .sort_object = alib6::JSONConfig::sort_asc}};
    const auto t = doc.dump_to_string(json);
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    const fs::path tmp = f.string() + ".tmp";
    { std::ofstream os(tmp, std::ios::binary | std::ios::trunc); os << std::string(t.data(), t.size()) << "\n"; }
    fs::rename(tmp, f, ec);
}

}  // namespace

std::string remap_placeholders(std::string_view text, std::string_view game, std::string_view mo2, std::string_view dl) {
    std::string s(text);
    struct M { const char* name; std::string_view dir; };
    for (const M& m : {M{"GAME_PATH_MAGIC", game}, M{"MO2_PATH_MAGIC", mo2}, M{"DOWNLOAD_PATH_MAGIC", dl}}) {
        const std::string base = std::string("{--||") + m.name;
        replace_all(s, base + "_BACK||--}", to_windows(m.dir, false, false));
        replace_all(s, base + "_DOUBLE_BACK||--}", to_windows(m.dir, true, false));
        replace_all(s, base + "_FORWARD||--}", to_windows(m.dir, false, true));
    }
    return s;
}

Report install_modlist(const Modlist& list, const std::string& wj_file, const InstallOptions& opt) {
    Report rep;
    const fs::path out{opt.output_dir};
    std::error_code ec;
    fs::create_directories(out, ec);
    const fs::path work = out / ".mol-wabbajack";
    const fs::path data = work / "data";
    const fs::path dl = opt.downloads_dir.empty() ? out / "downloads" : fs::path(opt.downloads_dir);
    fs::create_directories(dl, ec);
    State st = load_state(work / "state.json");
    auto progress = [&](std::string_view stage, std::string_view name, std::uint64_t d, std::uint64_t t) { if (opt.progress) opt.progress(stage, name, d, t); };

    // 内联数据与补丁：整个 zip 解到 data/（一次；已解过则跳过）
    if (!fs::exists(data / ".extracted", ec)) {
        fs::remove_all(data, ec);
        if (!extract_to(wj_file, data)) throw Error("io_error", "cannot extract the .wabbajack file (is 7z installed?)", wj_file);
        std::ofstream(data / ".extracted") << "1";
    }
    auto write_out = [&](const std::string& to, const std::string& content) {
        const fs::path dest = out / safe_rel(to);
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream os(dest, std::ios::binary | std::ios::trunc);
        os << content;
    };
    auto read_file = [&](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };

    // ---- 1. 内联文件 ----
    std::map<std::string, std::vector<const Directive*>> by_archive;
    std::map<std::string, std::int64_t> unsupported;
    for (const auto& d : list.directives) {
        try {
            if (d.kind == Kind::InlineFile || d.kind == Kind::RemappedInlineFile) {
                const fs::path src = data / d.source_data_id;
                if (!fs::exists(src, ec)) { rep.failures.push_back("missing inline data for " + d.to); ++rep.files_failed; continue; }
                const fs::path dest = out / safe_rel(d.to);
                if (d.kind == Kind::InlineFile && fs::exists(dest, ec) && static_cast<std::int64_t>(fs::file_size(dest, ec)) == d.size && wj_file_hash(dest.string()) == d.hash) continue;
                std::string content = read_file(src);
                if (d.kind == Kind::RemappedInlineFile) content = remap_placeholders(content, opt.game_dir, out.string(), dl.string());
                write_out(d.to, content);
                ++rep.files_written;
            } else if (d.kind == Kind::FromArchive || d.kind == Kind::PatchedFromArchive) {
                if (d.archive_path.size() < 2) { rep.failures.push_back("malformed directive for " + d.to); ++rep.files_failed; continue; }
                by_archive[d.archive_path[0]].push_back(&d);
            } else if (d.kind != Kind::Ignored) {
                ++unsupported[d.type];
            }
        } catch (const Error& e) {
            rep.failures.push_back(std::string(e.what()));
            ++rep.files_failed;
        }
    }

    // ---- 2. 压缩包 ----
    std::map<std::string, const Archive*> archives;
    for (const auto& a : list.archives) archives[a.hash] = &a;
    rep.archives_total = static_cast<std::int64_t>(by_archive.size());
    std::int64_t idx = 0;
    for (const auto& [hash, dirs] : by_archive) {
        ++idx;
        auto ait = archives.find(hash);
        if (ait == archives.end()) { rep.failures.push_back("directive references an unknown archive hash " + hash); rep.files_failed += static_cast<std::int64_t>(dirs.size()); continue; }
        const Archive& a = *ait->second;
        progress("archive", a.name, static_cast<std::uint64_t>(idx), static_cast<std::uint64_t>(by_archive.size()));
        if (st.done.count(hash) > 0) { ++rep.archives_done; continue; }

        // 2a 找/取压缩包
        fs::path file;
        auto verified_ok = [&](const fs::path& p) {
            const auto key = p.string();
            const auto sz = static_cast<std::int64_t>(fs::file_size(p, ec));
            if (a.size > 0 && sz != a.size) return false;
            if (auto it = st.verified.find(key); it != st.verified.end() && it->second == sz) return true;
            if (wj_file_hash(key) != a.hash) return false;
            st.verified[key] = sz;
            return true;
        };
        {
            const fs::path cand = dl / safe_rel(a.name);
            if (fs::is_regular_file(cand, ec) && verified_ok(cand)) file = cand;
        }
        if (file.empty() && a.size > 0) {  // 下载目录里别的名字但内容相同
            for (fs::directory_iterator it(dl, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec) || static_cast<std::int64_t>(it->file_size(ec)) != a.size) continue;
                const auto ext = lower(it->path().extension().string());
                if (ext == ".meta" || ext == ".part") continue;
                if (verified_ok(it->path())) { file = it->path(); break; }
            }
        }
        if (file.empty()) {
            const Source& s = a.src;
            const fs::path dest = dl / safe_rel(a.name);
            auto prog = [&](std::uint64_t d, std::uint64_t t) { progress("download", a.name, d, t); return true; };
            try {
                if (s.kind == "http" && !s.url.empty()) {
                    std::vector<std::pair<std::string, std::string>> hdrs;
                    for (const auto& h : s.headers) if (auto c = h.find(':'); c != std::string::npos) { std::string v = h.substr(c + 1); if (!v.empty() && v[0] == ' ') v.erase(0, 1); hdrs.emplace_back(h.substr(0, c), v); }
                    http_download(s.url, dest.string(), hdrs, prog);
                    file = dest;
                } else if (s.kind == "cdn" && !s.url.empty()) {
                    download_authored(s.url, dest.string(), prog);
                    file = dest;
                } else if (s.kind == "nexus" && opt.client) {
                    const auto r = nexus_download(*opt.client, dl.string(), s.game_domain, s.mod_id, s.file_id, nullptr, prog);
                    file = fs::path(std::string(r.path));
                } else if (s.kind == "gamefile") {
                    const fs::path gf = find_ci(fs::path(opt.game_dir), safe_rel(s.game_file));
                    if (gf.empty()) {
                        rep.pending.push_back({"game_file_missing", a.name, "this file must come from the game installation: " + s.game_file, "", static_cast<std::int64_t>(dirs.size())});
                        continue;
                    }
                    file = gf;
                } else {
                    std::string detail = s.prompt.empty() ? "download this file manually and put it into the downloads directory" : s.prompt;
                    if (s.kind == "nexus") detail = "no Nexus API key available (run `mo-linux nexus login`), or download it manually";
                    std::string url = s.url;
                    if (s.kind == "nexus") url = "https://www.nexusmods.com/" + s.game_domain + "/mods/" + std::to_string(s.mod_id) + "?tab=files&file_id=" + std::to_string(s.file_id);
                    rep.pending.push_back({"manual_download", a.name + "  [" + s.kind + "]", detail, url, static_cast<std::int64_t>(dirs.size())});
                    continue;
                }
            } catch (const Error& e) {
                if (e.code == "nexus_premium")
                    rep.pending.push_back({"manual_download", a.name + "  [nexus]", "a free Nexus account cannot download this directly; download it in the browser into the downloads directory",
                                           "https://www.nexusmods.com/" + s.game_domain + "/mods/" + std::to_string(s.mod_id) + "?tab=files&file_id=" + std::to_string(s.file_id), static_cast<std::int64_t>(dirs.size())});
                else
                    rep.failures.push_back(a.name + ": " + e.code + ": " + e.what());
                continue;
            }
            if (!verified_ok(file)) {
                if (s.kind != "gamefile") fs::remove(file, ec);
                rep.failures.push_back(a.name + ": hash mismatch after download" + (s.kind == "gamefile" ? " (the game file differs from the one the list was built with)" : " (file deleted; run again)"));
                continue;
            }
        }

        // 2b 解压并执行指令
        const fs::path tmp = work / "tmp" / safe_name(hash);
        fs::remove_all(tmp, ec);
        progress("extract", a.name, 0, 0);
        if (!extract_to(file, tmp)) {
            rep.failures.push_back(a.name + ": extraction failed");
            fs::remove_all(tmp, ec);
            continue;
        }
        bool all_ok = true;
        for (const Directive* d : dirs) {
            try {
                // 嵌套压缩包：archive_path[1..n-1] 依次是「里面的压缩包」，最后一项才是目标文件
                fs::path root = tmp;
                std::vector<fs::path> nested_tmps;
                fs::path src;
                for (std::size_t k = 1; k < d->archive_path.size(); ++k) {
                    const fs::path found = find_ci(root, safe_rel(d->archive_path[k]));
                    if (found.empty()) throw Error("not_found", "file not found inside the archive: " + d->archive_path[k]);
                    if (k + 1 == d->archive_path.size()) { src = found; break; }
                    const fs::path inner = work / "tmp" / (safe_name(hash) + "_n" + std::to_string(k) + "_" + safe_name(d->to));
                    fs::remove_all(inner, ec);
                    if (!extract_to(found, inner)) throw Error("io_error", "cannot extract nested archive " + d->archive_path[k]);
                    nested_tmps.push_back(inner);
                    root = inner;
                }
                const fs::path dest = out / safe_rel(d->to);
                if (fs::exists(dest, ec) && static_cast<std::int64_t>(fs::file_size(dest, ec)) == d->size && wj_file_hash(dest.string()) == d->hash) {
                    for (const auto& n : nested_tmps) fs::remove_all(n, ec);
                    continue;
                }
                fs::create_directories(dest.parent_path(), ec);
                if (d->kind == Kind::PatchedFromArchive) {
                    const fs::path patch = data / d->patch_id;
                    if (!fs::exists(patch, ec)) throw Error("not_found", "missing patch data for " + d->to);
                    octodiff_apply(src.string(), patch.string(), dest.string());
                } else {
                    fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
                    if (ec) throw Error("io_error", "copy failed: " + ec.message(), dest.string());
                }
                for (const auto& n : nested_tmps) fs::remove_all(n, ec);
                if (!d->hash.empty() && wj_file_hash(dest.string()) != d->hash) {
                    rep.failures.push_back(d->to + ": hash mismatch after install");
                    ++rep.files_failed;
                    all_ok = false;
                    continue;
                }
                ++rep.files_written;
            } catch (const Error& e) {
                rep.failures.push_back(d->to + ": " + e.what());
                ++rep.files_failed;
                all_ok = false;
            }
        }
        fs::remove_all(tmp, ec);
        if (all_ok) { st.done.insert(hash); ++rep.archives_done; }
        save_state(work / "state.json", st);
    }
    for (const auto& [type, n] : unsupported)
        rep.pending.push_back({"unsupported", type, std::to_string(n) + " directive(s) of type " + type + " are not supported by mo-linux yet, so parts of the list are missing", "", n});
    save_state(work / "state.json", st);
    return rep;
}

}  // namespace mol::wabbajack
