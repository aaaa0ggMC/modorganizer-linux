// libwinshim —— Windows 私有 profile（INI）语义。
//
//  * 文件按 UTF-8 字节处理；开头 UTF-8 BOM 保留；
//  * 原有换行风格（LF / CRLF）逐行保留，新增行沿用文件主导风格（新文件 CRLF）；
//  * 注释（';' 或 '#' 开头）、空行、行序、缩进原样保留，只改动目标键所在行；
//  * 节名 / 键名 ASCII 大小写不敏感；
//  * 写入原子化：同目录临时文件 + rename，保留原文件权限位与 owner。
#include "internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

using mol_shim::errno_to_win;
using mol_shim::last_error_set;

// ---- 小工具 -----------------------------------------------------------------
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

std::string trim(std::string_view s) {
    constexpr std::string_view ws = " \t\r\n\v\f";
    std::size_t a = s.find_first_not_of(ws);
    if (a == std::string_view::npos) return {};
    std::size_t b = s.find_last_not_of(ws);
    return std::string(s.substr(a, b - a + 1));
}

// 值两端空白去掉；整体被一对相同引号包住时去引号（Windows 行为）。
std::string unquote_value(std::string v) {
    if (v.size() >= 2) {
        const char f = v.front(), b = v.back();
        if ((f == '"' && b == '"') || (f == '\'' && b == '\'')) v = v.substr(1, v.size() - 2);
    }
    return v;
}

std::wstring w(std::string_view utf8) { return mol_shim::utf8_to_wide(utf8); }
std::string w2u(LPCWSTR s) { return s ? mol_shim::wide_to_utf8(s) : std::string(); }

// ---- 文档模型 ---------------------------------------------------------------
struct Line {
    std::string text;  // 不含行尾
    std::string eol;   // 原始行尾（"\r\n" / "\n" / 末行无行尾则为空）
};

struct Doc {
    std::vector<Line> lines;
    bool bom = false;
    std::string eol = "\r\n";  // 新增行使用的行尾
};

bool read_all(const std::string& path, std::string& out) {
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

void split_lines(const std::string& data, Doc& doc) {
    std::string body = data;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF) {
        doc.bom = true;
        body.erase(0, 3);
    }
    std::size_t lf = 0, crlf = 0, i = 0;
    while (i < body.size()) {
        Line ln;
        std::size_t j = body.find('\n', i);
        if (j == std::string::npos) {
            ln.text = body.substr(i);
            doc.lines.push_back(std::move(ln));
            break;
        }
        ln.text = body.substr(i, j - i);
        if (!ln.text.empty() && ln.text.back() == '\r') {
            ln.text.pop_back();
            ln.eol = "\r\n";
            ++crlf;
        } else {
            ln.eol = "\n";
            ++lf;
        }
        doc.lines.push_back(std::move(ln));
        i = j + 1;
    }
    doc.eol = (crlf == 0 && lf > 0) ? "\n" : "\r\n";  // 新文件 / 无换行信息时 CRLF
}

// 读入 ini；文件不存在/是目录视为空文档。真实读失败返回 false 并设置 LastError。
bool load_doc(const std::string& path, Doc& doc) {
    struct stat st;
    if (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        doc = Doc{};
        return true;
    }
    std::string data;
    if (!read_all(path, data)) {
        int e = errno;
        if (e == ENOENT || e == ENOTDIR || e == EISDIR) {
            doc = Doc{};
            return true;
        }
        mol_shim::set_last_error_errno(e);
        return false;
    }
    split_lines(data, doc);
    return true;
}

std::string build_content(const Doc& doc) {
    std::string out;
    if (doc.bom) out.append("\xEF\xBB\xBF");
    for (const Line& ln : doc.lines) {
        out += ln.text;
        out += ln.eol;
    }
    return out;
}

// ---- 语法判断 ---------------------------------------------------------------
bool parse_section_header(const Line& ln, std::string& name) {
    std::size_t p = ln.text.find_first_not_of(" \t");
    if (p == std::string::npos || ln.text[p] != '[') return false;
    std::size_t e = ln.text.find(']', p + 1);
    if (e == std::string::npos) return false;
    name = trim(std::string_view(ln.text).substr(p + 1, e - p - 1));
    return true;
}

bool is_comment_line(const Line& ln) {
    std::size_t p = ln.text.find_first_not_of(" \t");
    return p != std::string::npos && (ln.text[p] == ';' || ln.text[p] == '#');
}

bool parse_kv(const Line& ln, std::string& name, std::string& value) {
    std::size_t eq = ln.text.find('=');
    if (eq == std::string::npos) return false;
    name = trim(std::string_view(ln.text).substr(0, eq));
    if (name.empty()) return false;
    value = trim(std::string_view(ln.text).substr(eq + 1));
    return true;
}

bool section_body(const Doc& doc, std::string_view section, std::size_t& begin, std::size_t& end) {
    bool in = false;
    std::string name;
    for (std::size_t i = 0; i < doc.lines.size(); ++i) {
        if (parse_section_header(doc.lines[i], name)) {
            if (in) {
                end = i;
                return true;
            }
            if (ieq(name, section)) {
                in = true;
                begin = i + 1;
            }
        }
    }
    if (in) {
        end = doc.lines.size();
        return true;
    }
    return false;
}

bool find_section_header(const Doc& doc, std::string_view section, std::size_t& idx) {
    std::string name;
    for (std::size_t i = 0; i < doc.lines.size(); ++i)
        if (parse_section_header(doc.lines[i], name) && ieq(name, section)) {
            idx = i;
            return true;
        }
    return false;
}

bool find_key_line(const Doc& doc, std::size_t begin, std::size_t end, std::string_view key, std::size_t& idx) {
    for (std::size_t i = begin; i < end; ++i) {
        if (is_comment_line(doc.lines[i])) continue;
        std::string n, v;
        if (parse_kv(doc.lines[i], n, v) && ieq(n, key)) {
            idx = i;
            return true;
        }
    }
    return false;
}

bool get_value(const Doc& doc, const std::string& section, const std::string& key, std::string& out) {
    std::size_t b, e;
    if (!section_body(doc, section, b, e)) return false;
    std::size_t idx;
    if (!find_key_line(doc, b, e, key, idx)) return false;
    std::string n, v;
    if (!parse_kv(doc.lines[idx], n, v)) return false;
    out = unquote_value(std::move(v));
    return true;
}

std::vector<std::string> section_names(const Doc& doc) {
    std::vector<std::string> out;
    std::string name;
    for (const Line& ln : doc.lines)
        if (parse_section_header(ln, name)) out.push_back(name);
    return out;
}

std::vector<std::string> key_names(const Doc& doc, const std::string& section) {
    std::vector<std::string> out;
    std::size_t b, e;
    if (!section_body(doc, section, b, e)) return out;
    for (std::size_t i = b; i < e; ++i) {
        if (is_comment_line(doc.lines[i])) continue;
        std::string n, v;
        if (parse_kv(doc.lines[i], n, v)) out.push_back(n);
    }
    return out;
}

// ---- 输出拷贝（截断语义） ---------------------------------------------------
template <class C>
DWORD copy_single(C* out, DWORD size, const std::basic_string<C>& v) {
    if (!out || size == 0) return 0;
    const std::size_t need = v.size();
    if (need + 1 <= static_cast<std::size_t>(size)) {
        if (need) std::memcpy(out, v.data(), need * sizeof(C));
        out[need] = 0;
        return static_cast<DWORD>(need);
    }
    const std::size_t cp = static_cast<std::size_t>(size) - 1;
    if (cp) std::memcpy(out, v.data(), cp * sizeof(C));
    out[cp] = 0;
    return static_cast<DWORD>(cp);  // size - 1
}

template <class C>
DWORD copy_multi(C* out, DWORD size, const std::vector<std::basic_string<C>>& items) {
    std::basic_string<C> joined;
    for (const auto& s : items) {
        joined += s;
        joined.push_back(0);  // 每项自带结尾 NUL
    }
    const std::size_t total = joined.size() + 1;  // 额外的结尾 NUL（双 NUL）
    if (!out || size == 0) return 0;
    if (total <= static_cast<std::size_t>(size)) {
        std::memcpy(out, joined.data(), joined.size() * sizeof(C));
        out[joined.size()] = 0;  // 追加一个 NUL 形成双 NUL 结尾
        return static_cast<DWORD>(total - 1);
    }
    if (size < 2) {
        out[0] = 0;
        return 0;
    }
    const std::size_t cp = static_cast<std::size_t>(size) - 2;
    const std::size_t n = joined.size() < cp ? joined.size() : cp;
    if (n) std::memcpy(out, joined.data(), n * sizeof(C));
    for (std::size_t k = n; k < static_cast<std::size_t>(size); ++k) out[k] = 0;  // 保证双 NUL
    return static_cast<DWORD>(cp);                                                    // size - 2
}

// ---- 写入 -------------------------------------------------------------------
struct Target {
    std::string path;
    bool exists = false;
    struct stat st {};
    DWORD err = ERROR_SUCCESS;
};

// 前置检查：文件可写性 / 父目录存在性。
bool precheck(const std::string& path, Target& t) {
    t.path = path;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            t.err = ERROR_ACCESS_DENIED;
            return false;
        }
        t.exists = true;
        t.st = st;
        if (::access(path.c_str(), W_OK) != 0) {
            t.err = mol_shim::errno_to_win(errno);
            return false;
        }
        return true;
    }
    if (errno != ENOENT && errno != ENOTDIR) {
        t.err = mol_shim::errno_to_win(errno);
        return false;
    }
    std::string parent = ".";
    std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) parent = slash == 0 ? "/" : path.substr(0, slash);
    struct stat ps;
    if (::stat(parent.c_str(), &ps) != 0 || !S_ISDIR(ps.st_mode)) {
        t.err = ERROR_PATH_NOT_FOUND;
        return false;
    }
    if (::access(parent.c_str(), W_OK) != 0) {
        t.err = mol_shim::errno_to_win(errno);
        return false;
    }
    return true;
}

bool write_atomic(const Target& t, const std::string& content) {
    mode_t mode;
    if (t.exists)
        mode = t.st.st_mode & 07777;
    else {
        mode_t um = ::umask(0);
        ::umask(um);
        mode = 0666 & ~um;
    }
    std::string tmp = t.path + ".wshim-tmp";
    ::unlink(tmp.c_str());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        mol_shim::set_last_error_errno(errno);
        return false;
    }
    if (t.exists) {
        if (::fchown(fd, t.st.st_uid, t.st.st_gid) != 0) { /* 忽略：无权限改动 owner */ }
    }
    std::size_t off = 0;
    while (off < content.size()) {
        ssize_t written = ::write(fd, content.data() + off, content.size() - off);
        if (written < 0) {
            int e = errno;
            ::close(fd);
            ::unlink(tmp.c_str());
            mol_shim::set_last_error_errno(e);
            return false;
        }
        off += static_cast<std::size_t>(written);
    }
    if (::close(fd) != 0) {
        int e = errno;
        ::unlink(tmp.c_str());
        mol_shim::set_last_error_errno(e);
        return false;
    }
    if (::rename(tmp.c_str(), t.path.c_str()) != 0) {
        int e = errno;
        ::unlink(tmp.c_str());
        mol_shim::set_last_error_errno(e);
        return false;
    }
    return true;
}

// 生成 "key=value" 行内容（保留两端空白的值加引号以便读回一致）。
std::string format_kv(const std::string& key, const std::string& value) {
    std::string k = trim(key);
    std::string v;
    for (char c : value) {  // 行内不能出现换行
        if (c != '\r' && c != '\n') v.push_back(c);
    }
    if (v != trim(v)) v = "\"" + v + "\"";
    return k + "=" + v;
}

void ensure_trailing_eol(Doc& doc) {
    if (doc.lines.empty()) return;
    Line& last = doc.lines.back();
    if (last.eol.empty()) last.eol = doc.eol;
}

void delete_section(Doc& doc, std::string_view section) {
    std::size_t idx;
    if (!find_section_header(doc, section, idx)) return;
    std::size_t end = doc.lines.size();
    std::string name;
    for (std::size_t i = idx + 1; i < doc.lines.size(); ++i)
        if (parse_section_header(doc.lines[i], name)) {
            end = i;
            break;
        }
    doc.lines.erase(doc.lines.begin() + static_cast<std::ptrdiff_t>(idx),
                    doc.lines.begin() + static_cast<std::ptrdiff_t>(end));
}

bool delete_key(Doc& doc, const std::string& section, const std::string& key) {
    std::size_t b, e;
    if (!section_body(doc, section, b, e)) return true;
    std::size_t idx;
    if (find_key_line(doc, b, e, key, idx))
        doc.lines.erase(doc.lines.begin() + static_cast<std::ptrdiff_t>(idx));
    return true;
}

bool set_key(Doc& doc, const std::string& section, const std::string& key, const std::string& value) {
    std::size_t hidx;
    if (find_section_header(doc, section, hidx)) {
        std::size_t b, e;
        if (section_body(doc, section, b, e)) {
            std::size_t idx;
            if (find_key_line(doc, b, e, key, idx)) {
                doc.lines[idx].text = format_kv(key, value);
                return true;
            }
            // 追加在该节最后一个键值行之后（保留节尾空行/注释）
            std::size_t pos = b;
            for (std::size_t i = b; i < e; ++i) {
                if (is_comment_line(doc.lines[i])) continue;
                std::string n, v;
                if (parse_kv(doc.lines[i], n, v)) pos = i + 1;
            }
            ensure_trailing_eol(doc);
            doc.lines.insert(doc.lines.begin() + static_cast<std::ptrdiff_t>(pos),
                             Line{format_kv(key, value), doc.eol});
            return true;
        }
    }
    // 新节：追加到文件末尾
    ensure_trailing_eol(doc);
    Line header{"[" + section + "]", doc.eol};
    Line entry{format_kv(key, value), doc.eol};
    doc.lines.push_back(header);
    doc.lines.push_back(entry);
    return true;
}

}  // namespace

// 注意：shim/include/windows.h 中的声明是 C++ 链接（非 extern "C"），这里必须一致才能链接。
DWORD GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR def, LPWSTR out, DWORD size, LPCWSTR file) {
    mol_shim::last_error_set(ERROR_SUCCESS);
    if (!file) return copy_single<wchar_t>(out, size, def ? std::wstring(def) : std::wstring());
    std::string path = mol_shim::native_path(file);
    Doc doc;
    if (!load_doc(path, doc)) return 0;
    const std::wstring dflt = def ? std::wstring(def) : std::wstring();
    if (section) {
        const std::string sec = w2u(section);
        if (key) {
            std::string value;
            if (!get_value(doc, sec, w2u(key), value)) return copy_single<wchar_t>(out, size, dflt);
            return copy_single<wchar_t>(out, size, w(value));
        }
        std::vector<std::wstring> items;
        for (const std::string& k : key_names(doc, sec)) items.push_back(w(k));
        return copy_multi<wchar_t>(out, size, items);
    }
    std::vector<std::wstring> sections;
    for (const std::string& s : section_names(doc)) sections.push_back(w(s));
    return copy_multi<wchar_t>(out, size, sections);
}

DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file) {
    mol_shim::last_error_set(ERROR_SUCCESS);
    if (!file) return copy_single<char>(out, size, def ? std::string(def) : std::string());
    std::string path = mol_shim::native_path(mol_shim::utf8_to_wide(file).c_str());
    Doc doc;
    if (!load_doc(path, doc)) return 0;
    const std::string dflt = def ? std::string(def) : std::string();
    if (section) {
        const std::string sec = section ? section : "";
        if (key) {
            std::string value;
            if (!get_value(doc, sec, key ? key : "", value)) return copy_single<char>(out, size, dflt);
            return copy_single<char>(out, size, value);
        }
        std::vector<std::string> items;
        for (const std::string& k : key_names(doc, sec)) items.push_back(k);
        return copy_multi<char>(out, size, items);
    }
    std::vector<std::string> items;
    for (const std::string& s : section_names(doc)) items.push_back(s);
    return copy_multi<char>(out, size, items);
}

UINT GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, INT def, LPCWSTR file) {
    mol_shim::last_error_set(ERROR_SUCCESS);
    if (!file) return static_cast<UINT>(def);
    std::string path = mol_shim::native_path(file);
    Doc doc;
    if (!load_doc(path, doc)) return static_cast<UINT>(def);
    if (!section || !key) return static_cast<UINT>(def);
    std::string value;
    if (!get_value(doc, w2u(section), w2u(key), value)) return static_cast<UINT>(def);
    if (value.empty()) return static_cast<UINT>(def);
    const char* begin = value.c_str();
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(begin, &end, 10);
    if (end == begin) return static_cast<UINT>(def);
    return static_cast<UINT>(parsed);
}

BOOL WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR file) {
    mol_shim::last_error_set(ERROR_SUCCESS);
    if (!file) return TRUE;  // 刷新缓存，无实际文件改动
    Target target;
    if (!precheck(mol_shim::native_path(file), target)) {
        last_error_set(target.err);
        return FALSE;
    }
    Doc doc;
    if (!load_doc(target.path, doc)) return FALSE;

    if (section) {
        const std::string sec = w2u(section);
        if (!key) {  // 删除整节
            delete_section(doc, sec);
        } else if (!value) {  // 删除键
            delete_key(doc, sec, w2u(key));
        } else {
            set_key(doc, sec, w2u(key), w2u(value));
        }
    } else if (!key) {
        // 无节名且无键名：无操作（Windows 同样认为没有可删除的节）
        return TRUE;
    } else {
        set_key(doc, "", w2u(key), value ? w2u(value) : "");
    }
    if (!write_atomic(target, build_content(doc))) return FALSE;
    return TRUE;
}

BOOL WritePrivateProfileSectionW(LPCWSTR section, LPCWSTR data, LPCWSTR file) {
    mol_shim::last_error_set(ERROR_SUCCESS);
    if (!file || !section) return FALSE;
    Target target;
    if (!precheck(mol_shim::native_path(file), target)) {
        last_error_set(target.err);
        return FALSE;
    }
    Doc doc;
    if (!load_doc(target.path, doc)) return FALSE;

    std::vector<std::string> entries;
    if (data) {
        for (const wchar_t* p = data; *p;) {
            entries.push_back(mol_shim::wide_to_utf8(p));
            p += std::wcslen(p) + 1;
        }
    }
    const std::string sec = w2u(section);

    std::size_t hidx;
    if (!find_section_header(doc, sec, hidx)) {
        // 新节追加到末尾
        ensure_trailing_eol(doc);
        doc.lines.push_back(Line{"[" + sec + "]", doc.eol});
        for (const std::string& e : entries) doc.lines.push_back(Line{e, doc.eol});
    } else if (!data) {
        delete_section(doc, sec);
    } else {
        std::size_t b, e;
        if (!section_body(doc, sec, b, e)) e = doc.lines.size();
        // 保留节头，整体替换节内容
        doc.lines.erase(doc.lines.begin() + static_cast<std::ptrdiff_t>(b),
                        doc.lines.begin() + static_cast<std::ptrdiff_t>(e));
        for (std::size_t i = 0; i < entries.size(); ++i)
            doc.lines.insert(doc.lines.begin() + static_cast<std::ptrdiff_t>(b + i),
                             Line{entries[i], doc.eol});
    }
    if (!write_atomic(target, build_content(doc))) return FALSE;
    return TRUE;
}

