// WP3: 链接农场。语义见 core/include/mol/linkfarm.hpp 与 docs/PLAN.md「链接农场」。
// 设计要点：
//  * plan = diff(expected.entries, root 实际状态 + manifest)；manifest 只记录我们创建过的目录/链接。
//  * 所有内部临时数据（扫描结果、manifest 条目、期望表）放在栈上 monotonic arena（上游 mem 兜底），
//    最终产物（Plan/Op）用调用方给的 mem 分配。
//  * 从不跟随符号链接递归；实际状态一律用 symlink_status。
//  * 输出确定性：ops 与 manifest 全程显式排序（字节序）。
#include "mol/linkfarm.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace mol {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kArenaSize = 48 * 1024;

fs::path to_fs(std::string_view s) { return fs::path(std::string(s)); }
std::string to_str(std::string_view s) { return std::string(s); }

[[noreturn]] void fail_op(const char* what, std::string_view path, const std::error_code& ec) {
    throw std::runtime_error(std::string(what) + " '" + to_str(path) + "': " + ec.message() +
                             " (errno " + std::to_string(ec.value()) + ")");
}

[[noreturn]] void fail_path(const char* what, const std::string& path, const std::string& detail) {
    throw std::runtime_error(std::string(what) + " '" + path + "': " + detail);
}

// 栈上 monotonic arena：先吃 std::array，超出回落到上游 mem。
struct Arena {
    std::array<std::byte, kArenaSize> buf{};
    std::pmr::monotonic_buffer_resource res;

    explicit Arena(mr* up) : res(buf.data(), buf.size(), up) {}
    mr* get() { return &res; }

    // 把字符串复制进 arena，返回指向 arena 的 view（arena 必须比 view 活得久）。
    std::string_view dup(std::string_view s) {
        if (s.empty()) return {};
        void* p = res.allocate(s.size());
        std::memcpy(p, s.data(), s.size());
        return {static_cast<const char*>(p), s.size()};
    }
};

// ---------------------------------------------------------------- 实际状态

enum class ActualKind { Missing, File, Dir, Symlink };

struct ActualEntry {
    std::string_view rel;       // 相对农场根，'/' 分隔
    ActualKind kind = ActualKind::Missing;
    std::string_view target;    // kind == Symlink 时指向 readlink 结果
};

// 不跟随符号链接递归；root 不存在（含断链）→ 空。
std::pmr::vector<ActualEntry> scan_actual(const fs::path& root, Arena& a) {
    std::pmr::vector<ActualEntry> out(a.get());
    std::error_code ec;
    fs::file_status st = fs::symlink_status(root, ec);
    if (ec || !fs::exists(st)) return out;

    std::string prefix = root.generic_string();
    if (prefix.empty() || prefix.back() != '/') prefix += '/';

    // 默认不跟随目录符号链接；skip_permission_denied 保证不会因权限中断。
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return out;
    while (it != end) {
        const fs::path p = it->path();
        const std::string g = p.generic_string();
        if (g.size() > prefix.size() && g.compare(0, prefix.size(), prefix) == 0) {
            ActualEntry e;
            e.rel = a.dup(std::string_view(g).substr(prefix.size()));
            std::error_code sec;
            fs::file_status fst = fs::symlink_status(p, sec);
            if (!sec) {
                if (fs::is_symlink(fst)) {
                    e.kind = ActualKind::Symlink;
                    fs::path tgt = fs::read_symlink(p, sec);
                    if (!sec) e.target = a.dup(tgt.generic_string());
                } else if (fs::is_directory(fst)) {
                    e.kind = ActualKind::Dir;
                } else {
                    e.kind = ActualKind::File;
                }
            }
            out.push_back(e);
        }
        it.increment(ec);
        if (ec) break;
    }
    std::sort(out.begin(), out.end(),
              [](const ActualEntry& x, const ActualEntry& y) { return x.rel < y.rel; });
    return out;
}

const ActualEntry* find_actual(const std::pmr::vector<ActualEntry>& v, std::string_view p) {
    auto it = std::lower_bound(v.begin(), v.end(), p,
                               [](const ActualEntry& e, std::string_view k) { return e.rel < k; });
    return (it != v.end() && it->rel == p) ? &*it : nullptr;
}

// ---------------------------------------------------------------- manifest

bool marker_present(const fs::path& root) {
    std::error_code ec;
    fs::file_status st = fs::symlink_status(root / kFarmMarker, ec);
    return !ec && fs::exists(st);
}

std::string manifest_path_str(const fs::path& root) { return (root / kFarmMarker).generic_string(); }

// 读取 marker 里的 manifest；调用方需先确认 marker 存在。条目按字节序排序。
std::pmr::vector<std::string_view> load_manifest(const fs::path& root, Arena& a) {
    std::pmr::vector<std::string_view> out(a.get());
    std::ifstream in(root / kFarmMarker, std::ios::binary);
    if (!in) fail_path("linkfarm: cannot read manifest", manifest_path_str(root), "open failed");
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        fail_path("linkfarm: corrupt manifest", manifest_path_str(root), e.what());
    }
    if (!j.is_object() || !j.contains("version") || !j["version"].is_number_integer() ||
        j["version"].get<long long>() != 1 || !j.contains("created") || !j["created"].is_array())
        fail_path("linkfarm: bad manifest schema", manifest_path_str(root),
                  "expected {\"version\":1,\"created\":[]}");
    for (const auto& v : j["created"]) {
        if (!v.is_string())
            fail_path("linkfarm: bad manifest schema", manifest_path_str(root),
                      "created[] must be strings");
        out.push_back(a.dup(v.get<std::string>()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// 原子写 manifest：临时文件 + rename。
void store_manifest(const fs::path& root, const std::pmr::set<std::pmr::string>& created) {
    nlohmann::json j = nlohmann::json::object();
    j["version"] = 1;
    j["created"] = nlohmann::json::array();
    for (const auto& s : created) j["created"].push_back(s);  // 已是字节序升序

    const fs::path tmp = root / (std::string(kFarmMarker) + ".tmp");
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os) fail_path("linkfarm: cannot write manifest", tmp.generic_string(), "open failed");
        os << j.dump(2) << "\n";
        os.flush();
        if (!os) fail_path("linkfarm: cannot write manifest", tmp.generic_string(), "write failed");
    }
    std::error_code ec;
    fs::rename(tmp, root / kFarmMarker, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(tmp, ec2);
        fail_op("linkfarm: cannot replace manifest", kFarmMarker, ec);
    }
}

bool contains_sv(const std::pmr::vector<std::string_view>& v, std::string_view p) {
    return std::binary_search(v.begin(), v.end(), p);
}

// ---------------------------------------------------------------- 期望树

struct ExpEntry {
    std::string_view path;
    bool is_dir = false;
    std::string_view source;  // is_dir 时为空
};

// 期望表 = expected.entries + 隐式父目录，按 path 字节序排序、去重。
// 同路径重复条目时目录优先（与合并规则一致，同时保证确定性）。
std::pmr::vector<ExpEntry> build_expected(const MergeResult& expected, Arena& a) {
    using ExpVec = std::pmr::vector<ExpEntry>;
    ExpVec raw(a.get());
    raw.reserve(expected.entries.size());
    for (const auto& e : expected.entries) {
        ExpEntry x;
        x.path = a.dup(e.path);
        x.is_dir = e.is_dir;
        x.source = a.dup(e.source);
        raw.push_back(x);
    }
    std::sort(raw.begin(), raw.end(), [](const ExpEntry& x, const ExpEntry& y) {
        if (x.path != y.path) return x.path < y.path;
        return x.is_dir > y.is_dir;  // 目录排前面，去重时保留目录
    });
    ExpVec entries(a.get());
    entries.reserve(raw.size());
    for (const auto& e : raw)
        if (entries.empty() || entries.back().path != e.path) entries.push_back(e);

    // 收集所有路径 + 所有祖先前缀，排序去重后回填条目信息；缺 info 的即隐式目录。
    std::pmr::vector<std::string_view> all(a.get());
    all.reserve(entries.size() * 2);
    for (const auto& e : entries) {
        all.push_back(e.path);
        std::size_t i = 0;
        while ((i = e.path.find('/', i)) != std::string_view::npos) {
            all.push_back(e.path.substr(0, i));
            ++i;
        }
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());

    ExpVec out(a.get());
    out.reserve(all.size());
    for (std::string_view p : all) {
        auto it = std::lower_bound(entries.begin(), entries.end(), p,
                                   [](const ExpEntry& e, std::string_view k) { return e.path < k; });
        if (it != entries.end() && it->path == p)
            out.push_back(*it);
        else
            out.push_back(ExpEntry{p, true, {}});  // 隐式目录
    }
    return out;
}

const ExpEntry* find_expected(const std::pmr::vector<ExpEntry>& v, std::string_view p) {
    auto it = std::lower_bound(v.begin(), v.end(), p,
                               [](const ExpEntry& e, std::string_view k) { return e.path < k; });
    return (it != v.end() && it->path == p) ? &*it : nullptr;
}

// 目录 dir（manifest 里、已不在期望树中）能否 Rmdir：
// 实际内容全部「在 manifest 里且不在期望树中」；否则跳过，不报错，避免误删用户内容。
bool dir_is_removable(const std::pmr::vector<ExpEntry>& exp, const std::pmr::vector<ActualEntry>& actual,
                      const std::pmr::vector<std::string_view>& manifest, std::string_view dir) {
    std::string prefix(dir);
    prefix += '/';
    auto it = std::lower_bound(actual.begin(), actual.end(), prefix,
                               [](const ActualEntry& e, std::string_view k) { return e.rel < k; });
    for (; it != actual.end() && it->rel.substr(0, prefix.size()) == prefix; ++it) {
        if (!contains_sv(manifest, it->rel)) return false;
        if (find_expected(exp, it->rel) != nullptr) return false;
    }
    return true;
}

// ---------------------------------------------------------------- op 构造

void add_op(std::pmr::vector<Op>& ops, OpKind kind, std::string_view path, std::string_view target,
            mr* mem) {
    Op op(mem);
    op.kind = kind;
    op.path.assign(path.data(), path.size());
    if (!target.empty()) op.target.assign(target.data(), target.size());
    ops.push_back(std::move(op));
}

// ---------------------------------------------------------------- 公开 API

}  // namespace

Plan plan_farm(const MergeResult& expected, std::string_view root_sv, mr* mem) {
    const fs::path root = to_fs(root_sv);
    const std::string root_str = root.generic_string();
    Arena arena(mem);  // 扫描 / manifest / 期望表全部落在栈上 arena

    // ---- 1. root 只读检查 --------------------------------------------------
    bool root_exists = false;
    {
        std::error_code ec;
        fs::file_status st = fs::status(root, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            fail_op("plan_farm: cannot stat root", root_str, ec);
        root_exists = !ec && fs::exists(st);
    }
    if (root_exists) {
        std::error_code ec;
        fs::file_status st = fs::status(root, ec);
        if (ec || !fs::is_directory(st))
            fail_path("plan_farm", root_str, "root exists and is not a directory");
    }
    const bool have_marker = root_exists && marker_present(root);

    const std::pmr::vector<ActualEntry> actual =
        root_exists ? scan_actual(root, arena) : std::pmr::vector<ActualEntry>(arena.get());
    if (root_exists && !have_marker && !actual.empty())
        fail_path("plan_farm", root_str,
                  "root is not empty and has no " + std::string(kFarmMarker) + " marker");
    const std::pmr::vector<std::string_view> manifest =
        have_marker ? load_manifest(root, arena) : std::pmr::vector<std::string_view>(arena.get());
    const std::pmr::vector<ExpEntry> exp = build_expected(expected, arena);

    // ---- 2. diff ----------------------------------------------------------
    std::pmr::vector<Op> removes(mem), creates(mem);
    removes.reserve(manifest.size());
    creates.reserve(exp.size());

    // 2a. manifest 里有、期望里没有 → 清理：Remove（文件/链接）；目录能清才 Rmdir。
    for (std::string_view m : manifest) {
        if (find_expected(exp, m) != nullptr) continue;
        const ActualEntry* a = find_actual(actual, m);
        if (a != nullptr && a->kind == ActualKind::Dir) {
            if (dir_is_removable(exp, actual, manifest, m)) add_op(removes, OpKind::Rmdir, m, {}, mem);
            // 否则跳过：可能有用户内容，不报错，条目继续留在 manifest 里。
        } else {
            add_op(removes, OpKind::Remove, m, {}, mem);
        }
    }

    // 2b. 期望树 → Mkdir/Link/Relink；类型冲突时按 manifest 决定「先删后建」还是拒绝。
    for (const auto& e : exp) {
        const ActualEntry* a = find_actual(actual, e.path);
        if (e.is_dir) {
            if (a == nullptr) {
                add_op(creates, OpKind::Mkdir, e.path, {}, mem);
            } else if (a->kind != ActualKind::Dir) {
                if (!contains_sv(manifest, e.path))
                    fail_path("plan_farm", (root / to_fs(e.path)).generic_string(),
                              "refusing to replace non-farm content with a directory");
                add_op(removes, OpKind::Remove, e.path, {}, mem);
                add_op(creates, OpKind::Mkdir, e.path, {}, mem);
            }
            continue;
        }
        if (e.source.empty()) continue;  // 无 source 的文件条目：无可链接目标，跳过
        if (a == nullptr) {
            add_op(creates, OpKind::Link, e.path, e.source, mem);
        } else if (a->kind == ActualKind::Symlink) {
            if (a->target != e.source) add_op(creates, OpKind::Relink, e.path, e.source, mem);
            // 目标一致 → 无需操作
        } else {  // 期望链接而实际是真实文件/目录
            if (!contains_sv(manifest, e.path))
                fail_path("plan_farm", (root / to_fs(e.path)).generic_string(),
                          "refusing to replace non-farm content with a symlink");
            add_op(removes, OpKind::Remove, e.path, {}, mem);
            add_op(creates, OpKind::Link, e.path, e.source, mem);
        }
    }

    // ---- 3. 排序：清理逆序（子先于父），创建正序（父先于子） ----------------
    auto by_path_desc = [](const Op& x, const Op& y) {
        if (x.path != y.path) return y.path < x.path;
        return y.kind < x.kind;
    };
    auto by_path_asc = [](const Op& x, const Op& y) {
        if (x.path != y.path) return x.path < y.path;
        return x.kind < y.kind;
    };
    std::sort(removes.begin(), removes.end(), by_path_desc);
    std::sort(creates.begin(), creates.end(), by_path_asc);

    Plan plan(mem);
    plan.ops.reserve(removes.size() + creates.size());
    for (auto& o : removes) plan.ops.push_back(std::move(o));
    for (auto& o : creates) plan.ops.push_back(std::move(o));
    return plan;
}

void apply_farm(const Plan& plan, std::string_view root_sv) {
    const fs::path root = to_fs(root_sv);

    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) fail_op("apply_farm: cannot create root", root.generic_string(), ec);

    Arena arena(default_mr());
    std::pmr::set<std::pmr::string> created(arena.get());
    if (marker_present(root))
        for (std::string_view c : load_manifest(root, arena)) created.emplace(c);
    // 以 arena 分配的临时 key 访问 set（避免落到全局 new）
    auto set_key = [&arena](const mol::string& s) { return std::pmr::string(s, arena.get()); };

    // 落盘 manifest（含 root 不存在时首次创建 marker）。
    auto commit = [&root, &created]() {
        try {
            store_manifest(root, created);
        } catch (...) {
        }  // 落盘失败不吞原始错误，原始异常随后原样抛出
    };
    // 创建 rel 的所有父目录；新建出来的隐式目录也登记进 manifest。
    auto ensure_parents = [&root, &arena, &created](std::string_view rel) {
        const std::size_t slash = rel.rfind('/');
        if (slash == std::string_view::npos) return;
        const std::string_view parent = rel.substr(0, slash);
        std::pmr::string acc(&arena.res);
        for (std::size_t i = 0; i <= parent.size();) {
            const std::size_t j = parent.find('/', i);
            const std::size_t end = (j == std::string_view::npos) ? parent.size() : j;
            if (end > i) {
                acc.assign(parent.substr(0, end));
                const fs::path pp = root / to_fs(acc);
                std::error_code pec;
                fs::file_status pst = fs::symlink_status(pp, pec);
                if (pec || !fs::exists(pst)) {
                    std::error_code cec;
                    fs::create_directory(pp, cec);
                    if (cec && cec != std::errc::file_exists) fail_op("apply_farm: mkdir", acc, cec);
                    if (!cec) created.emplace(acc);
                }
            }
            if (end >= parent.size()) break;
            i = end + 1;
        }
    };
    auto do_link = [&](const Op& op, const fs::path& full) {
        std::error_code sec;
        fs::file_status st = fs::symlink_status(full, sec);
        if (!sec && fs::exists(st)) {
            if (fs::is_symlink(st) && op.kind == OpKind::Link) {
                fs::path cur = fs::read_symlink(full, sec);
                if (!sec && cur.generic_string() == to_str(op.target)) {
                    created.emplace(set_key(op.path));  // 已经正确
                    return;
                }
            }
            std::error_code rec;
            // 非递归：真实目录里若还有内容（应是先行的 Remove/Rmdir 清过的用户内容），
            // 宁可报错也绝不 remove_all 误删。
            fs::remove(full, rec);
            if (rec) fail_op("apply_farm: remove before link", op.path, rec);
        }
        ensure_parents(op.path);
        std::error_code lec;
        fs::create_symlink(to_fs(op.target), full, lec);
        if (lec) fail_op("apply_farm: create symlink", op.path, lec);
        created.emplace(set_key(op.path));
    };

    for (const auto& op : plan.ops) {
        try {
            const fs::path full = root / to_fs(op.path);
            switch (op.kind) {
                case OpKind::Mkdir: {
                    std::error_code mec;
                    fs::create_directory(full, mec);
                    if (mec && mec != std::errc::file_exists) fail_op("apply_farm: mkdir", op.path, mec);
                    std::error_code sec;
                    fs::file_status st = fs::symlink_status(full, sec);
                    if (sec || !fs::is_directory(st))
                        fail_op("apply_farm: mkdir", op.path,
                                std::make_error_code(std::errc::file_exists));
                    created.emplace(set_key(op.path));
                    break;
                }
                case OpKind::Link:
                case OpKind::Relink:
                    do_link(op, full);
                    break;
                case OpKind::Remove: {
                    std::error_code sec;
                    fs::file_status st = fs::symlink_status(full, sec);
                    if (!sec && fs::exists(st)) {
                        std::error_code rec;
                        // 一律非递归：目录里若有内容（只能是我们先删过一轮后剩下的用户内容），
                        // 宁可报错也绝不 remove_all 误删。
                        fs::remove(full, rec);
                        if (rec) fail_op("apply_farm: remove", op.path, rec);
                    }
                    created.erase(set_key(op.path));
                    break;
                }
                case OpKind::Rmdir: {
                    std::error_code sec;
                    fs::file_status st = fs::symlink_status(full, sec);
                    if (!sec && fs::exists(st)) {
                        if (fs::is_directory(st) && !fs::is_symlink(st)) {
                            std::error_code nec;
                            if (fs::is_empty(full, nec)) {
                                std::error_code rec;
                                fs::remove(full, rec);
                                if (rec) fail_op("apply_farm: rmdir", op.path, rec);
                                created.erase(set_key(op.path));
                            }
                            // plan 之后用户又塞了内容进去：跳过，不报错，继续留在 manifest 里
                        } else {
                            std::error_code rec;
                            fs::remove(full, rec);
                            if (rec) fail_op("apply_farm: rmdir", op.path, rec);
                            created.erase(set_key(op.path));
                        }
                    }
                    break;
                }
            }
        } catch (...) {
            commit();  // 中途失败也落盘已完成的部分
            throw;
        }
    }
    commit();
}

void remove_farm(std::string_view root_sv) {
    const fs::path root = to_fs(root_sv);
    if (!marker_present(root))
        fail_path("remove_farm", root.generic_string(),
                  "not a farm (missing " + std::string(kFarmMarker) + " marker)");

    Arena arena(default_mr());
    std::pmr::vector<std::string_view> manifest = load_manifest(root, arena);
    std::pmr::vector<std::string_view> files(arena.get()), dirs(arena.get());
    for (std::string_view m : manifest) {
        std::error_code ec;
        fs::file_status st = fs::symlink_status(root / to_fs(m), ec);
        if (!ec && fs::is_directory(st) && !fs::is_symlink(st))
            dirs.push_back(m);
        else
            files.push_back(m);
    }

    // 先文件/链接，再目录；组内逆序（子先于父）。一律非递归删除，遇用户内容保留。
    auto remove_group = [&root](std::pmr::vector<std::string_view>& group) {
        std::sort(group.rbegin(), group.rend());
        for (std::string_view m : group) {
            const fs::path p = root / to_fs(m);
            std::error_code ec;
            fs::file_status st = fs::symlink_status(p, ec);
            if (ec || !fs::exists(st)) continue;  // 已被外部删除
            if (fs::is_directory(st) && !fs::is_symlink(st)) {
                std::error_code nec;
                // 非空 = 用户在我们创建的目录里放了东西：保留，不递归删，也不报错
                if (!fs::is_empty(p, nec)) continue;
            }
            fs::remove(p, ec);
            if (ec) fail_op("remove_farm", m, ec);
        }
    };
    remove_group(files);
    remove_group(dirs);

    std::error_code ec;
    fs::remove(root / kFarmMarker, ec);
    if (ec) fail_op("remove_farm", kFarmMarker, ec);
    fs::remove(root, ec);  // root 非空则失败，忽略（保留用户内容）
}

}  // namespace mol
