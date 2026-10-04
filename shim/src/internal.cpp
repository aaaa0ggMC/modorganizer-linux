#include "internal.hpp"

#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <mutex>
#include <optional>

namespace mol_shim {
namespace {
std::mutex g_mu;
std::optional<std::string> g_prefix, g_user;
thread_local DWORD t_last_error = 0;

const std::string& cached(std::optional<std::string>& slot, const char* env, const char* def) {
    std::lock_guard lk(g_mu);
    if (!slot) {
        const char* e = std::getenv(env);
        slot = (e && *e) ? std::string(e) : std::string(def);
        while (slot->size() > 1 && slot->back() == '/') slot->pop_back();
    }
    return *slot;
}
}  // namespace

const std::string& prefix() { return cached(g_prefix, "MOL_WINEPREFIX", ""); }
const std::string& user() { return cached(g_user, "MOL_WINEUSER", "steamuser"); }

std::string wide_to_utf8(const wchar_t* s, std::size_t n) {
    std::string out;
    if (!s) return out;
    if (n == static_cast<std::size_t>(-1)) n = std::wcslen(s);
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        char32_t c = static_cast<char32_t>(s[i]);
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) { out.push_back(char(0xC0 | (c >> 6))); out.push_back(char(0x80 | (c & 0x3F))); }
        else if (c < 0x10000) { out.push_back(char(0xE0 | (c >> 12))); out.push_back(char(0x80 | ((c >> 6) & 0x3F))); out.push_back(char(0x80 | (c & 0x3F))); }
        else { out.push_back(char(0xF0 | (c >> 18))); out.push_back(char(0x80 | ((c >> 12) & 0x3F))); out.push_back(char(0x80 | ((c >> 6) & 0x3F))); out.push_back(char(0x80 | (c & 0x3F))); }
    }
    return out;
}

std::wstring utf8_to_wide(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        char32_t c = 0xFFFD;
        std::size_t len = 1;
        if (b < 0x80) { c = b; }
        else if ((b >> 5) == 0x6 && i + 1 < s.size()) { len = 2; c = ((b & 0x1F) << 6) | (s[i + 1] & 0x3F); }
        else if ((b >> 4) == 0xE && i + 2 < s.size()) { len = 3; c = ((b & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); }
        else if ((b >> 3) == 0x1E && i + 3 < s.size()) { len = 4; c = ((b & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); }
        out.push_back(static_cast<wchar_t>(c));
        i += len;
    }
    return out;
}

std::string to_unix_path_utf8(std::string_view p) {
    if (p.substr(0, 4) == "\\\\?\\") p.remove_prefix(4);
    char drive = 0;
    if (p.size() >= 2 && ((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':') {
        drive = static_cast<char>(p[0] | 0x20);
        p.remove_prefix(2);
    }
    std::string out;
    if (drive == 'c') out = prefix() + "/drive_c";
    else if (drive && drive != 'z') { out = prefix() + "/dosdevices/"; out.push_back(drive); out.push_back(':'); }
    else if (drive == 'z' || (!p.empty() && (p[0] == '/' || p[0] == '\\'))) { /* 绝对路径，从 '/' 开始 */ }
    bool abs = drive != 0 || (!p.empty() && (p[0] == '/' || p[0] == '\\'));
    bool first = true;
    std::size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && (p[i] == '/' || p[i] == '\\')) ++i;
        std::size_t st = i;
        while (i < p.size() && p[i] != '/' && p[i] != '\\') ++i;
        if (i > st) {
            if (!first || abs) out.push_back('/');
            out.append(p.substr(st, i - st));
            first = false;
        }
    }
    if (out.empty() && abs) out = "/";
    return out;
}

std::string to_unix_path(const wchar_t* winpath) { return to_unix_path_utf8(wide_to_utf8(winpath)); }

std::wstring to_windows_path(std::string_view u) {
    std::string pre = prefix() + "/drive_c";
    std::string out;
    if (!prefix().empty() && u.substr(0, pre.size()) == pre && (u.size() == pre.size() || u[pre.size()] == '/')) {
        out = "C:";
        u.remove_prefix(pre.size());
    } else {
        out = "Z:";
    }
    if (u.empty()) out.push_back('\\');
    for (char c : u) out.push_back(c == '/' ? '\\' : c);
    return utf8_to_wide(out);
}

namespace {
bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if ((a[i] | (a[i] >= 'A' && a[i] <= 'Z' ? 0x20 : 0)) != (b[i] | (b[i] >= 'A' && b[i] <= 'Z' ? 0x20 : 0))) return false;
    return true;
}
}  // namespace

std::string resolve_ci(const std::string& p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (p.empty() || fs::exists(fs::symlink_status(p, ec))) return p;
    std::string cur = (p[0] == '/') ? "/" : "";
    std::size_t i = (p[0] == '/') ? 1 : 0;
    bool lost = false;
    while (i <= p.size()) {
        std::size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string comp = p.substr(i, j - i);
        if (!comp.empty()) {
            std::string cand = cur.empty() ? comp : (cur.back() == '/' ? cur + comp : cur + "/" + comp);
            if (!lost && !fs::exists(fs::symlink_status(cand, ec))) {
                std::vector<std::string> hits;
                for (fs::directory_iterator it(cur.empty() ? "." : cur, ec), end; !ec && it != end; it.increment(ec)) {
                    std::string n = it->path().filename().string();
                    if (ieq(n, comp)) hits.push_back(n);
                }
                if (!hits.empty()) { std::sort(hits.begin(), hits.end()); cand = cur.empty() ? hits[0] : (cur.back() == '/' ? cur + hits[0] : cur + "/" + hits[0]); }
                else lost = true;
            }
            cur = cand;
        }
        if (j >= p.size()) break;
        i = j + 1;
    }
    return cur;
}

std::string native_path(const wchar_t* w) { return resolve_ci(to_unix_path(w)); }

DWORD errno_to_win(int e) {
    switch (e) {
        case 0: return 0;
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
        case EEXIST: return ERROR_ALREADY_EXISTS;
        case EINVAL: return ERROR_INVALID_PARAMETER;
        case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
        case ENOSPC: return ERROR_DISK_FULL;
        case EXDEV: return ERROR_NOT_SAME_DEVICE;
        case EROFS: return ERROR_WRITE_PROTECT;
        case ENAMETOOLONG: return ERROR_BUFFER_OVERFLOW;
        case EBADF: return ERROR_INVALID_HANDLE;
        default: return ERROR_GEN_FAILURE;
    }
}
void set_last_error_errno(int e) { t_last_error = errno_to_win(e); }

DWORD last_error_get() { return t_last_error; }
void last_error_set(DWORD v) { t_last_error = v; }

}  // namespace mol_shim

extern "C" void mol_shim_configure(const char* prefix, const char* user) {
    std::lock_guard lk(mol_shim::g_mu);
    if (prefix) { mol_shim::g_prefix = prefix; while (mol_shim::g_prefix->size() > 1 && mol_shim::g_prefix->back() == '/') mol_shim::g_prefix->pop_back(); }
    if (user) mol_shim::g_user = user;
}
