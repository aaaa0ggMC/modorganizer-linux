// libwinshim —— 从 Wine 前缀的 .reg 文本文件读注册表（只读）。
//
//  * HKEY_LOCAL_MACHINE → <prefix>/system.reg，HKEY_CURRENT_USER → <prefix>/user.reg，
//    HKEY_CLASSES_ROOT 视为 HKLM\Software\Classes（并兼容 HKCU\Software\Classes）；
//  * prefix() 为空串时一律视为“键不存在”；
//  * 惰性解析 + 按文件 mtime 缓存（线程安全）；
//  * KEY_WOW64_32KEY 且路径以 Software\ 开头时先试 Software\Wow6432Node\...，
//    KEY_WOW64_64KEY 先试原路径，两者都无时互相回退一次；
//  * HKEY 句柄是堆上结构并通过“已注册句柄集合”识别，预定义伪句柄不当指针解引用。
#include "internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

using mol_shim::last_error_set;

constexpr uint32_t kMagic = 0x57524832u;  // "WRH2"
constexpr DWORD ERROR_INVALID_DATA = 13;
constexpr DWORD REG_BINARY = 3;       // 头文件未定义
constexpr DWORD REG_MULTI_SZ = 7;

// ---- 句柄 ------------------------------------------------------------------
struct RegKey {
    uint32_t magic;
    int root;            // RootKind
    std::wstring path;   // 规范化的键路径，以 '\' 分隔，无前导/尾随 '\'
    REGSAM sam;
};

// 已分配句柄的集合：HKEY 是不透明的，绝不能用“读一下魔数”的方式校验
// （预定义伪句柄 0x80000000..0x80000002 不是可解引用的指针）。
std::mutex g_keys_mu;
std::set<const RegKey*> g_keys;

enum RootKind { RK_HKCR = 0, RK_HKCU, RK_HKLM };

bool classify_root(HKEY h, int& kind) {
    if (h == HKEY_CLASSES_ROOT) {
        kind = RK_HKCR;
        return true;
    }
    if (h == HKEY_CURRENT_USER) {
        kind = RK_HKCU;
        return true;
    }
    if (h == HKEY_LOCAL_MACHINE) {
        kind = RK_HKLM;
        return true;
    }
    return false;
}

RegKey* as_key(HKEY h) {
    if (!h) return nullptr;
    int kind = 0;
    if (classify_root(h, kind)) return nullptr;  // 预定义伪句柄
    const RegKey* k = static_cast<const RegKey*>(h);
    std::lock_guard<std::mutex> lk(g_keys_mu);
    return g_keys.count(k) != 0 ? const_cast<RegKey*>(k) : nullptr;
}

bool register_key(const RegKey* k) {
    std::lock_guard<std::mutex> lk(g_keys_mu);
    return g_keys.insert(k).second;
}

void unregister_key(const RegKey* k) {
    std::lock_guard<std::mutex> lk(g_keys_mu);
    g_keys.erase(k);
}

struct KeyRef {
    int root = RK_HKLM;
    REGSAM sam = 0;
    std::wstring path;
};

bool deref_key(HKEY h, KeyRef& out) {
    int kind = 0;
    if (classify_root(h, kind)) {
        out.root = kind;
        out.path.clear();
        return true;
    }
    RegKey* k = as_key(h);
    if (!k) return false;
    out.root = k->root;
    out.path = k->path;
    out.sam = k->sam;
    return true;
}

// ---- 路径工具 ---------------------------------------------------------------
std::wstring lower_wide(std::wstring_view s) {
    std::wstring o;
    o.reserve(s.size());
    for (wchar_t c : s) o.push_back((c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c);
    return o;
}

// 规范化为小写、'\' 分隔、无前导尾随分隔符。
std::wstring norm_path(std::wstring_view p) {
    std::wstring out;
    bool pending = false;
    for (wchar_t c : p) {
        if (c == L'/' || c == L'\\') {
            pending = true;
            continue;
        }
        if (pending && !out.empty()) out.push_back(L'\\');
        pending = false;
        out.push_back(c);
    }
    return lower_wide(out);
}

std::wstring join(std::wstring_view a, std::wstring_view b) {
    if (a.empty()) return std::wstring(b);
    if (b.empty()) return std::wstring(a);
    return std::wstring(a) + L"\\" + std::wstring(b);
}

bool starts_with_ci(std::wstring_view s, std::wstring_view pre) {
    std::wstring ls = lower_wide(s), lp = lower_wide(pre);
    if (ls.size() < lp.size()) return false;
    return ls.compare(0, lp.size(), lp) == 0;
}

// ---- .reg 解析 -------------------------------------------------------------
struct RegValue {
    DWORD type = 0;
    // REG_SZ / REG_EXPAND_SZ / REG_MULTI_SZ：UTF-8；其它：原始字节（dword 为 4 字节小端）。
    std::string data;
};

struct RegFileData {
    std::map<std::wstring, std::vector<std::pair<std::wstring, RegValue>>> keys;
};

struct CachedFile {
    bool ok = false;
    long long mtime = 0;
    std::shared_ptr<const RegFileData> data;
};

std::mutex g_mu;
std::map<std::string, CachedFile> g_cache;

bool read_file_bytes(const std::string& path, std::string& out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    out.clear();
    char buf[65536];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            int e = errno;
            ::close(fd);
            out.clear();
            errno = e;
            return false;
        }
        if (n == 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return true;
}

bool odd_trailing_backslashes(const std::string& s) {
    std::size_t n = 0;
    while (n < s.size() && s[s.size() - 1 - n] == '\\') ++n;
    return (n % 2) == 1;
}

// 物理行 → 逻辑行（拼接续行），去掉 \r。
std::vector<std::wstring> logical_lines(const std::string& text) {
    std::vector<std::string> raw;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t j = text.find('\n', i);
        std::string line = (j == std::string::npos) ? text.substr(i) : text.substr(i, j - i);
        i = (j == std::string::npos) ? text.size() : j + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        while (odd_trailing_backslashes(line) && i < text.size()) {
            line.pop_back();
            std::size_t k = text.find('\n', i);
            std::string nxt = (k == std::string::npos) ? text.substr(i) : text.substr(i, k - i);
            i = (k == std::string::npos) ? text.size() : k + 1;
            while (!nxt.empty() && nxt.back() == '\r') nxt.pop_back();
            line += nxt;
        }
        raw.push_back(std::move(line));
    }
    std::vector<std::wstring> out;
    out.reserve(raw.size());
    for (const std::string& l : raw) out.push_back(mol_shim::utf8_to_wide(l));
    return out;
}

// 带转义的引号串："\\" → '\'，"\"" 转义
std::wstring read_quoted(std::wstring_view s, std::size_t& pos) {
    std::wstring out;
    while (pos < s.size()) {
        wchar_t c = s[pos];
        if (c == L'\\' && pos + 1 < s.size() && s[pos + 1] == L'\\') {
            out.push_back(L'\\');
            pos += 2;
            continue;
        }
        if (c == L'\\' && pos + 1 < s.size() && s[pos + 1] == L'"') {
            out.push_back(L'"');
            pos += 2;
            continue;
        }
        if (c == L'"') break;
        out.push_back(c);
        ++pos;
    }
    if (pos < s.size() && s[pos] == L'"') ++pos;  // 吃掉闭引号
    return out;
}

std::wstring unescape_plain(std::wstring_view s) {
    std::wstring out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size() && s[i + 1] == L'\\') {
            out.push_back(L'\\');
            ++i;
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

int hex_digit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

void utf8_push(std::string& out, char32_t c) {
    if (c < 0x80)
        out.push_back(static_cast<char>(c));
    else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

// unicode=true：按 UTF-16LE 码元逐个转 UTF-8；否则原样取字节。
void parse_hex_bytes(std::wstring_view s, std::string& out, bool unicode) {
    out.clear();
    std::size_t i = 0;
    int part = 0;
    std::string utf16;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == L',' || s[i] == L' ' || s[i] == L'\t')) ++i;
        if (i >= s.size()) break;
        int hi = hex_digit(s[i]);
        if (hi < 0) break;
        ++i;
        int lo = (i < s.size()) ? hex_digit(s[i]) : -1;
        if (lo >= 0) ++i;
        unsigned char byte = static_cast<unsigned char>((hi << 4) | (lo < 0 ? 0 : lo));
        if (!unicode) {
            out.push_back(static_cast<char>(byte));
            continue;
        }
        utf16.push_back(static_cast<char>(byte));
        if (++part == 2) {
            WORD ch = static_cast<WORD>(static_cast<unsigned char>(utf16[0]) |
                                        (static_cast<unsigned char>(utf16[1]) << 8));
            if (ch >= 0xD800 && ch <= 0xDBFF) {  // 代理对（.reg 里罕见）
                out.push_back(static_cast<char>(0xEF));
                out.push_back(static_cast<char>(0xBF));
                out.push_back(static_cast<char>(0xBD));
                part = 0;
                utf16.clear();
                continue;
            }
            utf8_push(out, ch);
            part = 0;
            utf16.clear();
        }
    }
}

// UTF-16LE → 多字符串（UTF-8，项间 NUL、结尾双 NUL）
void utf16le_to_multisz(const std::string& utf16, std::string& out) {
    out.clear();
    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = 0; i + 1 < utf16.size(); i += 2) {
        WORD ch = static_cast<WORD>(static_cast<unsigned char>(utf16[i]) |
                                    (static_cast<unsigned char>(utf16[i + 1]) << 8));
        if (ch == 0) {
            if (cur.empty() && !parts.empty()) break;  // 双 NUL：列表结束
            parts.push_back(cur);
            cur.clear();
            continue;
        }
        utf8_push(cur, ch);
    }
    if (!cur.empty()) parts.push_back(cur);
    for (const std::string& p : parts) {
        out += p;
        out.push_back('\0');
    }
    out.push_back('\0');
}

bool parse_hex_kind(std::wstring_view tail, std::wstring_view data, RegValue& v) {
    if (tail == L"hex:") {
        v.type = REG_BINARY;
        parse_hex_bytes(data, v.data, false);
        return true;
    }
    if (tail == L"hex(2):") {
        v.type = REG_EXPAND_SZ;
        parse_hex_bytes(data, v.data, true);
        return true;
    }
    if (tail == L"hex(7):") {
        v.type = REG_MULTI_SZ;
        std::string raw;
        parse_hex_bytes(data, raw, false);
        utf16le_to_multisz(raw, v.data);
        return true;
    }
    if (tail.size() == 8 && tail.rfind(L"hex(", 0) == 0 && tail.find(L"):") == 4) {  // hex(N): 其它
        v.type = REG_BINARY;
        parse_hex_bytes(data, v.data, false);
        return true;
    }
    return false;
}

std::shared_ptr<const RegFileData> parse_reg_text(std::string_view text) {
    auto out = std::make_shared<RegFileData>();
    std::wstring current;        // 当前键（规范化后的全路径）
    bool have_current = false;
    for (std::wstring line : logical_lines(std::string(text))) {
        std::size_t b = line.find_first_not_of(L" \t");
        if (b == std::wstring::npos) continue;
        line.erase(0, b);
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();
        if (line.empty()) continue;
        if (line[0] == L';' || line[0] == L'#') continue;

        if (line[0] == L'[') {  // 节头
            std::size_t rb = line.rfind(L']');
            if (rb == std::wstring::npos) continue;
            std::wstring path = unescape_plain(line.substr(1, rb - 1));
            // 容忍 "HKEY_LOCAL_MACHINE\Software\..." 形式的绝对路径
            if (starts_with_ci(path, L"HKEY_LOCAL_MACHINE"))
                path = path.substr(std::wstring(L"HKEY_LOCAL_MACHINE").size());
            else if (starts_with_ci(path, L"HKEY_CURRENT_USER"))
                path = path.substr(std::wstring(L"HKEY_CURRENT_USER").size());
            else if (starts_with_ci(path, L"HKEY_CLASSES_ROOT"))
                path = path.substr(std::wstring(L"HKEY_CLASSES_ROOT").size());
            else if (starts_with_ci(path, L"HKLM"))
                path = path.substr(std::wstring(L"HKLM").size());
            else if (starts_with_ci(path, L"HKCU"))
                path = path.substr(std::wstring(L"HKCU").size());
            while (!path.empty() && path[0] == L'\\') path.erase(0, 1);
            current = norm_path(path);
            have_current = true;
            continue;
        }
        if (!have_current) continue;
        if (line[0] != L'@' && line[0] != L'"') continue;

        std::wstring name;
        std::size_t pos = 0;
        if (line[0] == L'@') {
            pos = 1;  // 默认值（名字为空）
        } else {
            pos = 1;
            name = read_quoted(line, pos);
            while (pos < line.size() && (line[pos] == L' ' || line[pos] == L'\t')) ++pos;
        }
        if (pos >= line.size() || line[pos] != L'=') continue;
        std::wstring data = line.substr(pos + 1);
        std::size_t ds = data.find_first_not_of(L" \t");
        if (ds == std::wstring::npos) continue;
        data.erase(0, ds);

        if (data == L"-") {  // 删除值
            auto it = out->keys.find(current);
            if (it != out->keys.end()) {
                std::wstring n = lower_wide(name);
                auto& vec = it->second;
                vec.erase(std::remove_if(vec.begin(), vec.end(),
                                         [&](const std::pair<std::wstring, RegValue>& p) { return p.first == n; }),
                          vec.end());
            }
            continue;
        }

        RegValue v;
        if (data[0] == L'"') {
            v.type = REG_SZ;
            std::size_t q = 1;
            std::wstring str = read_quoted(data, q);
            v.data = mol_shim::wide_to_utf8(str.c_str());
        } else if (starts_with_ci(data, L"dword:")) {
            std::wstring hex = data.substr(6);
            DWORD val = 0;
            for (std::size_t i = 0; i < hex.size() && i < 8; ++i) {
                int d = hex_digit(hex[i]);
                if (d < 0) break;
                val = (val << 4) | static_cast<DWORD>(d);
            }
            v.type = REG_DWORD;
            v.data.resize(4);
            v.data[0] = static_cast<char>(val & 0xFF);
            v.data[1] = static_cast<char>((val >> 8) & 0xFF);
            v.data[2] = static_cast<char>((val >> 16) & 0xFF);
            v.data[3] = static_cast<char>((val >> 24) & 0xFF);
        } else if (starts_with_ci(data, L"hex")) {
            std::size_t colon = data.find(L':');
            if (colon == std::wstring::npos) continue;
            if (!parse_hex_kind(lower_wide(data.substr(0, colon + 1)), data.substr(colon + 1), v)) continue;
        } else {
            continue;  // 未知形式
        }
        out->keys[current].emplace_back(lower_wide(name), std::move(v));
    }
    return out;
}

std::shared_ptr<const RegFileData> cached_file(const std::string& path) {
    struct stat st;
    const bool exists = ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    long long mtime = 0;
    if (exists)
        mtime = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_cache.find(path);
    if (it != g_cache.end() && it->second.ok == exists && (!exists || it->second.mtime == mtime))
        return it->second.data;
    CachedFile entry;
    entry.ok = exists;
    entry.mtime = mtime;
    if (exists) {
        std::string text;
        if (read_file_bytes(path, text)) entry.data = parse_reg_text(text);
    }
    auto ptr = entry.data;
    g_cache[path] = std::move(entry);
    return ptr;
}

// 在单个文件里找键：命中精确路径，或有更深层路径以其为父
// （Wine .reg 常只写有值的键，父键也要能打开）。
bool file_has_key(const RegFileData* f, const std::wstring& path) {
    if (!f) return false;
    if (f->keys.find(path) != f->keys.end()) return true;
    if (path.empty()) return !f->keys.empty();
    const std::wstring probe = path + L"\\";
    auto it = f->keys.lower_bound(probe);
    if (it == f->keys.end()) return false;
    return it->first.size() >= probe.size() && it->first.compare(0, probe.size(), probe) == 0;
}

// ---- 键解析 ----------------------------------------------------------------
struct ResolvedKey {
    std::shared_ptr<const RegFileData> file;  // 空 = 未找到
    std::wstring path;                        // 实际命中的路径（可能带回退）
};

std::wstring wow64_path(const std::wstring& p) {
    if (!starts_with_ci(p, L"software\\")) return L"";
    return L"software\\wow6432node\\" + p.substr(std::wstring(L"software\\").size());
}

void add_candidates(std::vector<std::pair<const RegFileData*, std::wstring>>& cands,
                    const RegFileData* f, const std::wstring& rel, REGSAM sam) {
    const std::wstring w = wow64_path(rel);
    if (w.empty())
        cands.emplace_back(f, rel);
    else if (sam & KEY_WOW64_32KEY)
        { cands.emplace_back(f, w); cands.emplace_back(f, rel); }
    else
        { cands.emplace_back(f, rel); cands.emplace_back(f, w); }
}

ResolvedKey resolve_key(int root, const std::wstring& path, REGSAM sam) {
    ResolvedKey out;
    const std::string pre = mol_shim::prefix();
    if (pre.empty()) return out;  // 未配置前缀：一律视为“键不存在”

    struct Plan {
        std::string file;
        std::wstring rel;
    };
    std::vector<Plan> plan;
    if (root == RK_HKCR) {  // HKCR = HKLM\Software\Classes（也看 user.reg 的同名子树）
        const std::wstring cls = L"software\\classes";
        std::wstring rel = starts_with_ci(path, L"software\\classes")
                               ? path
                               : (path.empty() ? cls : cls + L"\\" + path);
        plan.push_back({pre + "/system.reg", rel});
        plan.push_back({pre + "/user.reg", rel});
    } else if (root == RK_HKCU) {
        plan.push_back({pre + "/user.reg", path});
    } else {
        plan.push_back({pre + "/system.reg", path});
    }

    std::vector<std::shared_ptr<const RegFileData>> keep;  // 保持解析结果存活
    for (const Plan& p : plan) {
        std::shared_ptr<const RegFileData> f = cached_file(p.file);
        if (!f) continue;
        keep.push_back(f);
        std::vector<std::pair<const RegFileData*, std::wstring>> cands;
        add_candidates(cands, f.get(), p.rel, sam);
        for (auto& c : cands)
            if (file_has_key(c.first, c.second)) {
                out.file = f;
                out.path = c.second;
                return out;
            }
    }
    return out;
}

std::vector<BYTE> wide_bytes(const std::wstring& s) {
    std::vector<BYTE> out(s.size() * sizeof(wchar_t));
    if (!s.empty()) std::memcpy(out.data(), s.data(), out.size());
    return out;
}

// UTF-8（内含 NUL 分隔、双 NUL 结尾）→ wchar 多字符串
void multisz_bytes(const std::string& utf8, std::vector<BYTE>& out) {
    std::vector<std::wstring> parts;
    std::string cur;
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        if (utf8[i] == '\0') {
            parts.push_back(mol_shim::utf8_to_wide(cur));
            cur.clear();
        } else {
            cur.push_back(utf8[i]);
        }
    }
    if (!cur.empty()) parts.push_back(mol_shim::utf8_to_wide(cur));
    if (!parts.empty() && parts.back().empty()) parts.pop_back();  // 双 NUL 结束符
    out.clear();
    for (const std::wstring& p : parts) {
        for (wchar_t c : p) {
            const BYTE* b = reinterpret_cast<const BYTE*>(&c);
            out.push_back(b[0]);
            out.push_back(b[1]);
            out.push_back(b[2]);
            out.push_back(b[3]);
        }
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);
    }
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);  // 双 NUL 结尾
}

// 变量扩展（尽力而为）：未知变量原样保留。RRF_NOEXPAND 时不做任何替换。
std::wstring expand_vars(std::wstring s) {
    const std::wstring user = mol_shim::utf8_to_wide(mol_shim::user());
    const std::wstring profile = L"C:\\users\\" + user;
    const std::vector<std::pair<std::wstring, std::wstring>> table = {
        {L"USERPROFILE", profile},
        {L"HOME", profile},
        {L"APPDATA", profile + L"\\AppData\\Roaming"},
        {L"ROAMINGAPPDATA", profile + L"\\AppData\\Roaming"},
        {L"LOCALAPPDATA", profile + L"\\AppData\\Local"},
        {L"TEMP", profile + L"\\AppData\\Local\\Temp"},
        {L"TMP", profile + L"\\AppData\\Local\\Temp"},
        {L"USERNAME", user},
        {L"SYSTEMROOT", L"C:\\windows"},
        {L"WINDIR", L"C:\\windows"},
        {L"SYSTEMDRIVE", L"C:"},
        {L"PROGRAMFILES", L"C:\\Program Files"},
        {L"COMMONPROGRAMFILES", L"C:\\Program Files\\Common Files"},
        {L"PROGRAMDATA", L"C:\\ProgramData"},
    };
    std::wstring out;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] != L'%') {
            out.push_back(s[i++]);
            continue;
        }
        std::size_t j = s.find(L'%', i + 1);
        if (j == std::wstring::npos) {
            out.push_back(s[i++]);
            continue;
        }
        std::wstring up = s.substr(i + 1, j - i - 1);
        for (auto& c : up)
            if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - 32);
        bool replaced = false;
        for (const auto& kv : table)
            if (kv.first == up) {
                out += kv.second;
                replaced = true;
                break;
            }
        if (!replaced) {
            const std::string raw = mol_shim::wide_to_utf8(up.c_str());
            const char* env = std::getenv(raw.c_str());
            if (env && *env) {
                out += mol_shim::utf8_to_wide(env);
                replaced = true;
            }
        }
        if (!replaced) out += s.substr(i, j - i + 1);  // 原样保留
        i = j + 1;
    }
    return out;
}

// ---- 值查询 ----------------------------------------------------------------
struct QueryResult {
    DWORD type = 0;
    std::vector<BYTE> data;
};

const RegValue* lookup_value(const RegFileData* f, const std::wstring& path, const std::wstring& name) {
    if (!f) return nullptr;
    auto it = f->keys.find(path);
    if (it == f->keys.end()) return nullptr;
    std::wstring n = lower_wide(name);
    for (const auto& kv : it->second)
        if (kv.first == n) return &kv.second;
    return nullptr;
}

bool build_query(const RegValue* v, bool expand, QueryResult& q) {
    if (!v) return false;
    q.type = v->type;
    switch (v->type) {
        case REG_SZ:
        case REG_EXPAND_SZ: {
            std::wstring s = mol_shim::utf8_to_wide(v->data);
            if (expand) {
                s = expand_vars(s);
                q.type = REG_SZ;  // 展开后按 REG_SZ 返回
            }
            q.data = wide_bytes(s);
            // .reg 里的字符串数据可能已自带 NUL；保证恰有一个结尾 NUL
            bool has_nul = q.data.size() >= sizeof(wchar_t) &&
                           *reinterpret_cast<const wchar_t*>(&q.data[q.data.size() - sizeof(wchar_t)]) == 0;
            if (!has_nul) {
                q.data.push_back(0);
                q.data.push_back(0);
                q.data.push_back(0);
                q.data.push_back(0);
            }
            return true;
        }
        case REG_MULTI_SZ:
            multisz_bytes(v->data, q.data);
            return true;
        default:
            q.data.assign(v->data.begin(), v->data.end());
            return true;
    }
}

DWORD rrf_mask(DWORD flags) {
    const DWORD rt = flags & (RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_RT_REG_DWORD);
    // 其它 RRF_RT_* 位（头文件未定义）
    const DWORD extra = flags & (0x1 | 0x8 | 0x20 | 0x40 | 0x80);
    return rt | extra;
}

}  // namespace

// 与 windows.h 声明保持 C++ 链接（非 extern "C"）

LSTATUS RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult) {
    UNREFERENCED_PARAMETER(ulOptions);
    mol_shim::last_error_set(ERROR_SUCCESS);
    if (!phkResult) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }
    *phkResult = nullptr;
    KeyRef ref;
    if (!deref_key(hKey, ref)) {
        mol_shim::last_error_set(ERROR_INVALID_HANDLE);
        return ERROR_INVALID_HANDLE;
    }
    const std::wstring sub = lpSubKey ? norm_path(lpSubKey) : std::wstring();
    const std::wstring full = join(ref.path, sub);
    const ResolvedKey key = resolve_key(ref.root, full, samDesired);
    if (!key.file) {
        mol_shim::last_error_set(ERROR_FILE_NOT_FOUND);
        return ERROR_FILE_NOT_FOUND;
    }
    RegKey* k = new RegKey{kMagic, ref.root, key.path, samDesired};
    register_key(k);
    *phkResult = static_cast<HKEY>(k);
    return ERROR_SUCCESS;
}

LSTATUS RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, BYTE* lpData,
                         LPDWORD lpcbData) {
    UNREFERENCED_PARAMETER(lpReserved);
    mol_shim::last_error_set(ERROR_SUCCESS);
    if (!lpcbData) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }
    KeyRef ref;
    if (!deref_key(hKey, ref)) {
        mol_shim::last_error_set(ERROR_INVALID_HANDLE);
        return ERROR_INVALID_HANDLE;
    }
    const ResolvedKey key = resolve_key(ref.root, ref.path, ref.sam);
    const RegValue* v =
        lookup_value(key.file.get(), key.path, lpValueName ? lpValueName : L"");
    if (!v) {
        mol_shim::last_error_set(ERROR_FILE_NOT_FOUND);
        return ERROR_FILE_NOT_FOUND;
    }
    QueryResult q;
    build_query(v, false, q);
    if (lpType) *lpType = q.type;
    const DWORD need = static_cast<DWORD>(q.data.size());
    if (!lpData) {
        *lpcbData = need;
        return ERROR_SUCCESS;
    }
    if (*lpcbData < need) {
        *lpcbData = need;
        mol_shim::last_error_set(ERROR_MORE_DATA);
        return ERROR_MORE_DATA;
    }
    std::memcpy(lpData, q.data.data(), need);
    *lpcbData = need;
    return ERROR_SUCCESS;
}

LSTATUS RegGetValueW(HKEY hKey, LPCWSTR lpSubKey, LPCWSTR lpValue, DWORD dwFlags, LPDWORD pdwType,
                     PVOID pvData, LPDWORD pcbData) {
    mol_shim::last_error_set(ERROR_SUCCESS);
    if (!pcbData) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return ERROR_INVALID_PARAMETER;
    }
    KeyRef ref;
    if (!deref_key(hKey, ref)) {
        mol_shim::last_error_set(ERROR_INVALID_HANDLE);
        return ERROR_INVALID_HANDLE;
    }
    const std::wstring sub = lpSubKey ? norm_path(lpSubKey) : std::wstring();
    const std::wstring full = join(ref.path, sub);
    const ResolvedKey key = resolve_key(ref.root, full, ref.sam);
    if (!key.file) {
        mol_shim::last_error_set(ERROR_FILE_NOT_FOUND);
        return ERROR_FILE_NOT_FOUND;
    }
    const RegValue* v = lookup_value(key.file.get(), key.path, lpValue ? lpValue : L"");
    if (!v) {
        mol_shim::last_error_set(ERROR_FILE_NOT_FOUND);
        return ERROR_FILE_NOT_FOUND;
    }
    const DWORD want = rrf_mask(dwFlags);
    const bool any_type = want == 0 || (dwFlags & RRF_RT_ANY) == RRF_RT_ANY;
    if (!any_type) {
        bool ok = false;
        const DWORD t = v->type;
        if (t == REG_DWORD && (want & RRF_RT_REG_DWORD)) ok = true;
        if (t == REG_SZ && (want & (RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ))) ok = true;
        if (t == REG_EXPAND_SZ && (want & (RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ))) ok = true;
        if ((t == 0 || t == REG_BINARY || t == REG_MULTI_SZ || t == 11) && (want & (0x1 | 0x8 | 0x20 | 0x40)))
            ok = true;
        if (!ok) {
            mol_shim::last_error_set(ERROR_UNSUPPORTED_TYPE);
            return ERROR_UNSUPPORTED_TYPE;
        }
    }
    QueryResult q;
    build_query(v, !(dwFlags & RRF_NOEXPAND), q);
    if (pdwType) *pdwType = q.type;
    const DWORD need = static_cast<DWORD>(q.data.size());
    if (!pvData) {
        *pcbData = need;
        return ERROR_SUCCESS;
    }
    if (*pcbData < need) {
        *pcbData = need;
        mol_shim::last_error_set(ERROR_MORE_DATA);
        return ERROR_MORE_DATA;
    }
    std::memcpy(pvData, q.data.data(), need);
    *pcbData = need;
    return ERROR_SUCCESS;
}

LSTATUS RegCloseKey(HKEY hKey) {
    mol_shim::last_error_set(ERROR_SUCCESS);
    RegKey* k = as_key(hKey);
    if (k) {
        unregister_key(k);
        delete k;  // 预定义伪句柄未被解引用
    }
    return ERROR_SUCCESS;
}
