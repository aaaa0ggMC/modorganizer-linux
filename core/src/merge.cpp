#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <filesystem>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mol/casefold.hpp"
#include "mol/merge.hpp"

namespace mol {
namespace {

constexpr std::size_t kNoLayer = std::numeric_limits<std::size_t>::max();

// 递归深度上限（符号链接目录不递归，正常目录树到不了；纯防御）。
constexpr unsigned kMaxDepth = 512;

// 把 s ASCII 小写化到调用方给的栈缓冲区（常见名字零分配）；超长时才向 bump 申请。
// 返回的 view 在下次调用前有效。
std::string_view lower_into(std::string_view s, char* buf, std::size_t cap, mr* bump) {
    char* p = s.size() <= cap ? buf : static_cast<char*>(bump->allocate(s.size()));
    for (std::size_t i = 0; i != s.size(); ++i) {
        char c = s[i];
        p[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
    }
    return std::string_view(p, s.size());
}

inline char lower_char(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// key interning 的 key = 「父节点 id + 该级小写名」。哈希 / 比较只针对单级名字，
// 不对整条路径做小写拷贝，也不保存小写整串。
struct Key {
    std::size_t parent;
    std::string_view lower;
};

struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept {
        std::uint64_t h = 1469598103934665603ULL;  // FNV-1a
        for (unsigned char c : k.lower) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return static_cast<std::size_t>(h ^ (k.parent * 0x9E3779B97F4A7C15ULL));
    }
};

bool lower_eq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i != a.size(); ++i) {
        if (lower_char(a[i]) != lower_char(b[i])) return false;
    }
    return true;
}

struct KeyEq {
    bool operator()(const Key& a, const Key& b) const noexcept {
        return a.parent == b.parent && lower_eq(a.lower, b.lower);
    }
};

using KeyMap = std::pmr::unordered_map<Key, std::size_t, KeyHash, KeyEq>;

// 合并树上的节点。只存索引与 view（arena / 调用方输入，地址稳定），可平凡移动。
// 层总是按「下标升序」处理，所有「最高层」型字段天然等于最后一次写入。
struct Node {
    std::size_t parent = 0;
    std::string_view canon;              // 原始大小写（指向输入 rel，调用期间有效）
    std::string_view lower;              // 小写名（arena，地址稳定）
    std::string_view file_src;           // 胜出文件层的真实路径（指向输入 abs）
    std::string_view dir_src;            // 最早显式目录条目的真实路径（隐式目录为空）
    std::string_view intro_rel;          // 本层引入该 key 的最小 rel（定大小写用）
    std::size_t create_layer = kNoLayer; // 首次创建时的层 = canon 决定层
    std::size_t dir_layer = kNoLayer;    // 最先以目录类型引入的层（输出 layer）
    std::size_t dir_mention = kNoLayer;  // 最高「以目录出现」的层
    std::size_t dir_src_layer = kNoLayer;
    std::size_t file_layer = kNoLayer;   // 最高「以文件出现」的层（胜出者）
    std::size_t loser_head = kNoLayer;   // 被覆盖层的链（追加序 = 层号降序）
    std::uint32_t file_count = 0;        // 提供该文件的层数
    bool is_dir = false;                 // 最终类型（由最高优先级层决定）
    bool raw_self = false;               // 自底向上：自身是否可见（未考虑祖先否决）
    bool keep = false;
    bool any_child_keep = false;
};

struct LoserLink {
    std::size_t next;
    std::size_t layer;
};

// 扁平开放寻址集合：层内 casefold 去重用（只放 node id，零构造开销）。
class FlatSet {
public:
    void init(std::size_t n) {
        std::size_t cap = 16;
        while (cap < n * 2) cap <<= 1;
        slots_.assign(cap, kNoLayer);
    }
    void clear() { std::fill(slots_.begin(), slots_.end(), kNoLayer); }
    bool insert(std::size_t v) {  // 已存在 → false
        const std::size_t mask = slots_.size() - 1;
        std::size_t i = (v * 0x9E3779B97F4A7C15ULL) & mask;
        while (true) {
            if (slots_[i] == kNoLayer) {
                slots_[i] = v;
                return true;
            }
            if (slots_[i] == v) return false;
            i = (i + 1) & mask;
        }
    }
    bool contains(std::size_t v) const {
        const std::size_t mask = slots_.size() - 1;
        std::size_t i = (v * 0x9E3779B97F4A7C15ULL) & mask;
        while (true) {
            if (slots_[i] == kNoLayer) return false;
            if (slots_[i] == v) return true;
            i = (i + 1) & mask;
        }
    }

private:
    mol::vector<std::size_t> slots_{mol::allocator_type()};
};

struct Cand {
    std::size_t node;
    std::size_t idx;
};

struct PendingWarn {
    std::size_t node = 0;
    std::size_t layer_a = 0;  // intra: 层号 / mismatch: 被否决类型的最低层
    std::size_t layer_b = 0;  // mismatch: 胜出类型的最高层
    std::string_view a;       // intra: winner rel
    std::string_view b;       // intra: loser rel
    bool intra = true;
};

class Merger {
public:
    explicit Merger(mr* mem) : mem_(mem) {
        Node root;
        root.is_dir = true;  // 虚拟根本身是目录
        nodes_.push_back(root);  // 0 = 虚拟根
    }

    void add_layer(std::size_t L, const vector<ScanEntry>& entries);
    MergeResult finish();

private:
    std::size_t resolve(std::string_view rel, std::size_t L);
    std::size_t child(std::size_t parent, std::string_view comp, std::string_view rel,
                      std::size_t L, bool implicit_dir);
    void commit(std::size_t L, std::size_t nid, const ScanEntry& e);
    std::size_t push_loser(std::size_t head, std::size_t layer);
    void build_path(std::size_t id, mol::string& out);

    mr* mem_;
    // 临时数据：栈上 64KB 起步、上游 mem 兜底；monotonic 地址稳定，view 不失效。
    std::array<std::byte, 65536> scratch_{};
    std::pmr::monotonic_buffer_resource arena_{scratch_.data(), scratch_.size(), mem_};

    vector<Node> nodes_{mol::allocator_type(&arena_)};
    KeyMap map_{0, KeyHash{}, KeyEq{}, mol::allocator_type(&arena_)};
    vector<LoserLink> losers_{mol::allocator_type(&arena_)};
    vector<Cand> dups_{mol::allocator_type(&arena_)};
    vector<Cand> cands_{mol::allocator_type(&arena_)};
    vector<std::size_t> drop_{mol::allocator_type(&arena_)};
    vector<std::size_t> node_ids_{mol::allocator_type(&arena_)};
    vector<std::string_view> comps_{mol::allocator_type(&arena_)};
    vector<PendingWarn> pending_{mol::allocator_type(&arena_)};
    FlatSet flat_;
    char tmp_[1024];
};

std::size_t Merger::push_loser(std::size_t head, std::size_t layer) {
    const std::size_t id = losers_.size();
    losers_.push_back(LoserLink{head, layer});
    return id;
}

// 找 / 建「parent 下的 comp」这一级。implicit_dir = comp 是中间级（隐式目录）。
std::size_t Merger::child(std::size_t parent, std::string_view comp, std::string_view rel,
                          std::size_t L, bool implicit_dir) {
    const std::string_view low = lower_into(comp, tmp_, sizeof(tmp_), &arena_);
    const auto it = map_.find(Key{parent, low});
    if (it != map_.end()) return it->second;

    char* cp = static_cast<char*>(arena_.allocate(comp.size()));
    std::memcpy(cp, comp.data(), comp.size());
    char* lp = static_cast<char*>(arena_.allocate(low.size()));
    std::memcpy(lp, low.data(), low.size());

    Node n;
    n.parent = parent;
    n.canon = std::string_view(cp, comp.size());
    n.lower = std::string_view(lp, low.size());
    n.intro_rel = rel;
    n.create_layer = L;
    if (implicit_dir) {
        n.dir_layer = L;
        n.dir_mention = L;
    }
    nodes_.push_back(n);
    const std::size_t id = nodes_.size() - 1;
    map_.emplace(Key{parent, nodes_[id].lower}, id);
    return id;
}

// 按 '/' 逐级查表，缺则新建。返回 0 表示空 rel。
// 规范大小写与输入顺序无关：本层引入某 key 的所有条目里取 rel 字节序最小者的原始大小写。
std::size_t Merger::resolve(std::string_view rel, std::size_t L) {
    std::size_t cur = 0;
    std::size_t i = 0;
    while (i < rel.size()) {
        std::size_t j = rel.find('/', i);
        if (j == std::string_view::npos) j = rel.size();
        if (j > i) {
            const std::string_view comp = rel.substr(i, j - i);
            const bool implicit = (j != rel.size());
            cur = child(cur, comp, rel, L, implicit);
            Node& n = nodes_[cur];
            if (n.create_layer == L) {
                if (rel < n.intro_rel) {  // 同一层内更小的 rel 决定原始大小写
                    n.canon = comp;
                    n.intro_rel = rel;
                }
            }
            if (implicit) {
                if (n.dir_layer == kNoLayer) n.dir_layer = L;
                n.dir_mention = L;  // 作为父级出现 → 该层有一个目录
            }
        }
        if (j == rel.size()) break;
        i = j + 1;
    }
    return cur;
}

void Merger::commit(std::size_t L, std::size_t nid, const ScanEntry& e) {
    Node& n = nodes_[nid];
    if (e.is_dir) {
        if (n.dir_layer == kNoLayer) n.dir_layer = L;
        n.dir_mention = L;
        if (n.dir_src_layer == kNoLayer) {
            n.dir_src_layer = L;
            n.dir_src = e.abs;
        }
        return;
    }
    if (n.file_count == 0) {
        n.file_layer = L;
        n.file_src = e.abs;
        n.file_count = 1;
        return;
    }
    // 原胜出者降级为冲突方；升序处理 → 链按层号降序追加。
    n.loser_head = push_loser(n.loser_head, n.file_layer);
    n.file_layer = L;
    n.file_src = e.abs;
    ++n.file_count;
}

void Merger::add_layer(std::size_t L, const vector<ScanEntry>& entries) {
    const std::size_t n = entries.size();
    if (n == 0) return;

    node_ids_.clear();
    node_ids_.resize(n);
    std::size_t valid = 0;
    for (std::size_t i = 0; i != n; ++i) {
        const std::size_t nid = entries[i].rel.empty() ? 0 : resolve(entries[i].rel, L);
        node_ids_[i] = nid;
        if (nid != 0) ++valid;
    }
    if (valid == 0) return;

    // 规则 a：同一层内 casefold 冲突 → 保留 rel 字节序最小的一个，其余丢弃并告警。
    drop_.clear();
    dups_.clear();
    flat_.init(valid);
    bool has_dup = false;
    for (std::size_t i = 0; i != n; ++i) {
        const std::size_t nid = node_ids_[i];
        if (nid == 0) continue;
        if (!flat_.insert(nid)) {
            has_dup = true;
            dups_.push_back(Cand{nid, i});
        }
    }
    if (has_dup) {
        flat_.clear();
        for (const Cand& d : dups_) flat_.insert(d.node);
        cands_.clear();
        for (std::size_t i = 0; i != n; ++i) {
            const std::size_t nid = node_ids_[i];
            if (nid != 0 && flat_.contains(nid)) cands_.push_back(Cand{nid, i});
        }
        std::sort(cands_.begin(), cands_.end(), [&](const Cand& a, const Cand& b) {
            if (a.node != b.node) return a.node < b.node;
            if (entries[a.idx].rel != entries[b.idx].rel) {
                return std::string_view(entries[a.idx].rel) < std::string_view(entries[b.idx].rel);
            }
            if (entries[a.idx].is_dir != entries[b.idx].is_dir) return !entries[a.idx].is_dir;
            return std::string_view(entries[a.idx].abs) < std::string_view(entries[b.idx].abs);
        });
        for (std::size_t k = 0; k < cands_.size();) {
            std::size_t e = k;
            while (e < cands_.size() && cands_[e].node == cands_[k].node) ++e;
            for (std::size_t t = k + 1; t < e; ++t) {
                drop_.push_back(cands_[t].idx);
                pending_.push_back(PendingWarn{cands_[k].node, L, 0, entries[cands_[k].idx].rel,
                                               entries[cands_[t].idx].rel, true});
            }
            k = e;
        }
        std::sort(drop_.begin(), drop_.end());
    }

    for (std::size_t i = 0; i != n; ++i) {
        const std::size_t nid = node_ids_[i];
        if (nid == 0) continue;
        if (!drop_.empty() && std::binary_search(drop_.begin(), drop_.end(), i)) continue;
        commit(L, nid, entries[i]);
    }
}

// 规范路径：逐级原始大小写（每级取最先引入者），'/' 连接。
void Merger::build_path(std::size_t id, mol::string& out) {
    comps_.clear();
    for (std::size_t p = id; p != 0; p = nodes_[p].parent) comps_.push_back(nodes_[p].canon);
    std::reverse(comps_.begin(), comps_.end());
    std::size_t total = 0;
    for (const std::string_view c : comps_) total += c.size() + 1;
    out.clear();
    out.reserve(total);
    for (std::size_t k = 0; k != comps_.size(); ++k) {
        if (k != 0) out.push_back('/');
        out.append(comps_[k]);
    }
}

MergeResult Merger::finish() {
    const std::size_t count = nodes_.size();
    const mol::allocator_type alloc(mem_);

    // 规则 c/d：key 的最终类型由「最高优先级层」决定 —— 目录提及层 > 文件提及层则为目录。
    for (std::size_t id = 1; id < count; ++id) {
        Node& nd = nodes_[id];
        const bool dir = nd.dir_mention == kNoLayer
                             ? false
                             : (nd.file_layer == kNoLayer ? true : nd.dir_mention > nd.file_layer);
        nd.is_dir = dir;
    }

    // 自底向上：节点自身是否可见。文件恒可见；目录要有显式条目或可见子节点。
    for (std::size_t id = count; id-- > 1;) {
        Node& nd = nodes_[id];
        nd.raw_self = !nd.is_dir || nd.dir_src_layer != kNoLayer || nd.any_child_keep;
        if (nd.raw_self) nodes_[nd.parent].any_child_keep = true;
    }
    // 自顶向下：祖先被否决（目录输了类型之争 → 其子孙全部丢弃）或父级是文件 → 子树丢弃。
    // 父节点 id 必然小于子节点 id，故升序遍历即可。
    nodes_[0].keep = true;
    std::size_t kept = 0;
    for (std::size_t id = 1; id < count; ++id) {
        Node& nd = nodes_[id];
        const Node& par = nodes_[nd.parent];
        nd.keep = nd.raw_self && par.keep && par.is_dir;
        if (nd.keep) ++kept;
    }

    MergeResult res(alloc);
    res.entries.reserve(kept);

    mol::string path(alloc);
    for (std::size_t id = 1; id < count; ++id) {
        Node& nd = nodes_[id];
        if (!nd.keep) continue;

        build_path(id, path);
        MergedEntry entry(alloc);
        entry.path = path;
        entry.is_dir = nd.is_dir;
        if (nd.is_dir) {
            entry.layer = nd.dir_layer;
            entry.source.assign(nd.dir_src);  // 隐式目录 → 空串
        } else {
            entry.layer = nd.file_layer;
            entry.source.assign(nd.file_src);
        }

        // 规则 c：多于一层的同名文件 → 胜出者是最高层，其余升序。
        if (!nd.is_dir && nd.file_count >= 2) {
            Conflict c(alloc);
            c.path = path;
            c.winner = nd.file_layer;
            const std::size_t losers = static_cast<std::size_t>(nd.file_count) - 1;
            c.losers.resize(losers);
            std::size_t head = nd.loser_head;
            for (std::size_t k = losers; k-- > 0;) {
                c.losers[k] = head == kNoLayer ? kNoLayer : losers_[head].layer;
                if (head != kNoLayer) head = losers_[head].next;
            }
            if (std::find(c.losers.begin(), c.losers.end(), kNoLayer) == c.losers.end()) {
                res.conflicts.push_back(std::move(c));
            }
        }

        // 规则 d：目录 / 文件撞名 → 告警。
        if (nd.dir_mention != kNoLayer && nd.file_count != 0) {
            PendingWarn w;
            w.node = id;
            w.intra = false;
            if (nd.is_dir) {
                w.layer_a = nd.file_layer;
                for (std::size_t h = nd.loser_head; h != kNoLayer; h = losers_[h].next) {
                    w.layer_a = losers_[h].layer;  // 最低文件层
                }
                w.layer_b = nd.dir_mention;
            } else {
                w.layer_a = nd.dir_layer;      // 最低目录层
                w.layer_b = nd.file_layer;     // 最高文件层
            }
            pending_.push_back(w);
        }

        res.entries.push_back(std::move(entry));
    }

    // 告警（层内 casefold 冲突 + 类型撞名）。
    res.warnings.reserve(pending_.size());
    for (const PendingWarn& w : pending_) {
        if (!nodes_[w.node].keep) continue;
        Warning out(alloc);
        build_path(w.node, out.path);
        if (w.intra) {
            out.message.reserve(64 + w.a.size() + w.b.size());
            out.message.append("intra-layer casefold conflict in layer ");
            out.message.append(std::to_string(w.layer_a));
            out.message.append(": winner '");
            out.message.append(w.a);
            out.message.append("' shadows loser '");
            out.message.append(w.b);
            out.message.append("'");
        } else {
            out.message.reserve(64);
            out.message.append("dir/file kind mismatch between layer ");
            out.message.append(std::to_string(w.layer_a));
            out.message.append(" and layer ");
            out.message.append(std::to_string(w.layer_b));
        }
        res.warnings.push_back(std::move(out));
    }

    const auto by_path = [](std::string_view a, std::string_view b) { return a < b; };
    std::sort(res.entries.begin(), res.entries.end(),
              [&](const MergedEntry& a, const MergedEntry& b) {
                  return by_path(std::string_view(a.path), std::string_view(b.path));
              });
    std::sort(res.conflicts.begin(), res.conflicts.end(),
              [&](const Conflict& a, const Conflict& b) {
                  return by_path(std::string_view(a.path), std::string_view(b.path));
              });
    std::sort(res.warnings.begin(), res.warnings.end(),
              [&](const Warning& a, const Warning& b) {
                  if (std::string_view(a.path) != std::string_view(b.path)) {
                      return by_path(std::string_view(a.path), std::string_view(b.path));
                  }
                  return std::string_view(a.message) < std::string_view(b.message);
              });
    return res;
}

// ---------------------------------------------------------------- scan_layer
//
// 直接用 openat + getdents 的 d_type 遍历：常见文件系统（ext4/btrfs/xfs/tmpfs）上普通文件与目录
// 不需要任何 stat；只有符号链接（要看目标类型）与 d_type 未知的条目才 fstatat。
// 相对 dirfd 打开子目录，内核不必每次从根解析完整路径。

void append_comp(std::string& rel, std::string_view name) {
    if (!rel.empty()) rel.push_back('/');
    rel.append(name);
}

struct DirCloser {
    DIR* d;
    ~DirCloser() {
        if (d) ::closedir(d);
    }
};

// dfd 的所有权交给本函数（closedir 时关闭）。abs 为该目录的绝对路径（不以 '/' 结尾，根目录除外）。
void scan_fd(int dfd, std::string& abs, std::string& rel, vector<ScanEntry>& out, mr* mem, unsigned depth,
             unsigned max_depth) {
    DIR* d = ::fdopendir(dfd);
    if (!d) {
        ::close(dfd);
        return;
    }
    DirCloser guard{d};
    const std::size_t abs_base = abs.size();
    const std::size_t rel_base = rel.size();
    while (const dirent* de = ::readdir(d)) {
        const std::string_view name(de->d_name);
        if (name.empty() || name == "." || name == "..") continue;

        bool is_dir = false, is_link = false;
        unsigned char t = de->d_type;
        if (t == DT_UNKNOWN) {
            struct stat st {};
            if (::fstatat(dfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;  // 读不了的条目跳过
            t = S_ISDIR(st.st_mode) ? DT_DIR : S_ISLNK(st.st_mode) ? DT_LNK : DT_REG;
        }
        if (t == DT_DIR) {
            is_dir = true;
        } else if (t == DT_LNK) {
            // 符号链接按指向目标的类型归类；目录链接不递归（防环）。
            is_link = true;
            struct stat st {};
            is_dir = ::fstatat(dfd, de->d_name, &st, 0) == 0 && S_ISDIR(st.st_mode);
        }

        append_comp(rel, name);
        if (abs.empty() || abs.back() != '/') abs.push_back('/');
        abs.append(name);
        ScanEntry entry(mem);
        entry.rel.assign(rel);
        entry.is_dir = is_dir;
        entry.abs.assign(abs);
        out.push_back(std::move(entry));

        if (is_dir && !is_link && depth + 1 < max_depth) {
            const int sub = ::openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (sub >= 0) scan_fd(sub, abs, rel, out, mem, depth + 1, max_depth);
        }
        rel.resize(rel_base);
        abs.resize(abs_base);
    }
}

vector<ScanEntry> scan_impl(std::string_view root, std::string_view prefix, mr* mem, unsigned max_depth) {
    const mol::allocator_type alloc(mem);
    vector<ScanEntry> out(alloc);

    std::string abs(root);
    while (abs.size() > 1 && abs.back() == '/') abs.pop_back();
    const int fd = ::open(abs.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);  // 根本身可以是符号链接
    if (fd < 0) return out;

    std::string_view pfx = prefix;
    while (!pfx.empty() && pfx.front() == '/') pfx.remove_prefix(1);
    while (!pfx.empty() && pfx.back() == '/') pfx.remove_suffix(1);

    std::string rel(pfx);
    scan_fd(fd, abs, rel, out, mem, 0, max_depth);
    std::sort(out.begin(), out.end(), [](const ScanEntry& a, const ScanEntry& b) {
        if (std::string_view(a.rel) != std::string_view(b.rel)) {
            return std::string_view(a.rel) < std::string_view(b.rel);
        }
        if (a.is_dir != b.is_dir) return !a.is_dir;
        return std::string_view(a.abs) < std::string_view(b.abs);
    });
    return out;
}

}  // namespace

vector<ScanEntry> scan_layer(std::string_view root, std::string_view prefix, mr* mem) {
    return scan_impl(root, prefix, mem, kMaxDepth);
}

vector<ScanEntry> scan_layer_top(std::string_view root, std::string_view prefix, mr* mem) {
    return scan_impl(root, prefix, mem, 1);
}

MergeResult merge_listings(std::span<const vector<ScanEntry>> layers, mr* mem) {
    Merger merger(mem);
    for (std::size_t L = 0; L != layers.size(); ++L) merger.add_layer(L, layers[L]);
    return merger.finish();
}

}  // namespace mol
