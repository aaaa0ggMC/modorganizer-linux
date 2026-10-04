// WP6: Win32 系统类 API 的 Linux 实现：错误消息、调试输出、模块路径、环境变量、
// 路径长度、消息框/标准句柄、进程与 shell（不真正启动）、shell 目录与回收站、
// 已知文件夹、COM 占位、日期时间格式化。
// 约定：返回给调用方的路径默认是 Unix 形式（见 unix_paths_mode()），仅在环境变量
// MOL_SHIM_UNIX_PATHS=0 时返回 Windows 风格（"C:\users\..."）。
#include "internal.hpp"

#include "knownfolders.h"
#include "shlobj.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr DWORD kErrorCallNotImplemented = 120;

// MOL_SHIM_UNIX_PATHS：未设置视为 1（默认返回 Unix 路径），显式为 "0" 返回 Windows 风格。
// 不做静态缓存：同一进程内可能切换（测试两种形态）。
bool unix_paths_mode() {
    const char* e = std::getenv("MOL_SHIM_UNIX_PATHS");
    return !(e && e[0] == '0' && e[1] == '\0');
}

std::string drive_c(const std::string& suffix) { return mol_shim::prefix() + "/drive_c/" + suffix; }

// 已知文件夹的 Windows 风格路径（与 prefix 无关，空 prefix 也照样给出）。
std::wstring win_path(std::string_view suffix) {
    std::string out = "C:\\";
    for (char c : suffix) out.push_back(c == '/' ? '\\' : c);
    return mol_shim::utf8_to_wide(out);
}

std::wstring folder_path(std::string_view suffix) {
    if (unix_paths_mode()) return mol_shim::utf8_to_wide(drive_c(std::string(suffix)));
    return win_path(suffix);
}

std::string upper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - 0x20) : c);
    return out;
}

// 内置 + getenv 的两级查找。找不到返回空串。
std::wstring lookup_var(const std::string& name) {
    struct Item {
        const char* name;
        const char* suffix;  // "%u" = 用户名
    };
    static const Item kItems[] = {
        {"USERPROFILE", "users/%u"},
        {"APPDATA", "users/%u/AppData/Roaming"},
        {"LOCALAPPDATA", "users/%u/AppData/Local"},
        {"PROGRAMDATA", "ProgramData"},
        {"PROGRAMFILES", "Program Files"},
        {"PROGRAMFILES(X86)", "Program Files (x86)"},
        {"SYSTEMROOT", "windows"},
    };
    std::string up = upper(name);
    for (const auto& it : kItems) {
        if (up != it.name) continue;
        std::string suffix = it.suffix;
        auto pos = suffix.find("%u");
        if (pos != std::string::npos) suffix.replace(pos, 2, mol_shim::user());
        return folder_path(suffix);
    }
    const char* v = std::getenv(name.c_str());
    if (v && *v) return mol_shim::utf8_to_wide(v);
    return {};
}

bool known_builtin_var(const std::string& name) {
    static const char* kNames[] = {"USERPROFILE", "APPDATA",       "LOCALAPPDATA", "PROGRAMDATA",
                                   "PROGRAMFILES", "PROGRAMFILES(X86)", "SYSTEMROOT"};
    for (const char* n : kNames)
        if (upper(name) == n) return true;
    return false;
}

const wchar_t* system_message(DWORD code) {
    struct Entry {
        DWORD code;
        const wchar_t* msg;
    };
    static const Entry kTable[] = {
        {0, L"The operation completed successfully."},
        {2, L"The system cannot find the file specified."},
        {3, L"The system cannot find the path specified."},
        {5, L"Access is denied."},
        {6, L"The handle is invalid."},
        {8, L"Not enough memory resources are available to complete this operation."},
        {11, L"An attempt was made to load a program with an incorrect format."},
        {17, L"The system cannot move the file to a different disk drive."},
        {19, L"The media is write protected."},
        {31, L"A device attached to the system is not functioning."},
        {87, L"The parameter is incorrect."},
        {111, L"The file name is too long."},
        {112, L"There is not enough space on the disk."},
        {122, L"The data area passed to a system call is too small."},
        {161, L"The specified path is invalid."},
        {183, L"Cannot create a file when that file already exists."},
        {193, L"The program is not a valid Win32 application."},
        {234, L"More data is available."},
        {740, L"The requested operation requires elevation."},
        {1168, L"Element not found."},
        {1223, L"The operation was canceled by the user."},
        {1630, L"The data is invalid."},
    };
    for (const auto& e : kTable)
        if (e.code == code) return e.msg;
    return nullptr;
}

// FROM_STRING：展开 "%1".."%9"（ARGUMENT_ARRAY 时按 wchar_t* 解释 args 数组）与 "%n"。
std::wstring render_message(DWORD flags, LPCVOID src, DWORD msgId, void* args) {
    std::wstring text;
    if (flags & FORMAT_MESSAGE_FROM_STRING) {
        if (!src) return {};
        const auto* f = static_cast<LPCWSTR>(src);
        const auto* arr = static_cast<const DWORD_PTR*>(args);
        for (const wchar_t* p = f; *p;) {
            if (*p != L'%') {
                text.push_back(*p++);
                continue;
            }
            ++p;
            if (!*p) {  // 结尾的孤立 '%'
                text.push_back(L'%');
                break;
            }
            if (*p == L'%') { text.push_back(L'%'); ++p; continue; }
            if (*p == L'!') ++p;  // "%1!" 形式的前导感叹号
            if (*p >= L'0' && *p <= L'9') {
                unsigned n = 0;
                wchar_t digits[8];
                int ndig = 0;
                while (*p >= L'0' && *p <= L'9' && ndig < 2) {
                    n = n * 10 + static_cast<unsigned>(*p - L'0');
                    digits[ndig++] = *p;
                    ++p;
                }
                const wchar_t* sub = (n >= 1 && (flags & FORMAT_MESSAGE_ARGUMENT_ARRAY) && arr)
                                         ? reinterpret_cast<const wchar_t*>(arr[n - 1])
                                         : nullptr;
                if (sub) {
                    text.append(sub);  // 成功替换
                } else {  // 无参数可用：原样保留插入符
                    text.push_back(L'%');
                    for (int i = 0; i < ndig; ++i) text.push_back(digits[i]);
                }
                continue;
            }
            if (*p == L'n') { text.push_back(L'\r'); text.push_back(L'\n'); ++p; continue; }
            text.push_back(L'%');
            text.push_back(*p++);
        }
        return text;
    }
    if (const wchar_t* m = system_message(msgId)) return m;
    wchar_t buf[64];
    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"Unknown error 0x%08X", msgId);
    return buf;
}

// 返回写入字符数（含 NUL）；缓冲不足返回 0 + ERROR_INSUFFICIENT_BUFFER；cch==0 返回所需大小。
int write_wide(const std::wstring& s, LPWSTR out, int cch) {
    int needed = static_cast<int>(s.size()) + 1;
    if (cch == 0) return needed;
    if (needed > cch) {
        mol_shim::last_error_set(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::wmemcpy(out, s.c_str(), static_cast<size_t>(needed));
    return needed;
}

void now_local(SYSTEMTIME& st) {
    time_t t = ::time(nullptr);
    struct tm tmb {};
    ::localtime_r(&t, &tmb);
    st.wYear = static_cast<WORD>(tmb.tm_year + 1900);
    st.wMonth = static_cast<WORD>(tmb.tm_mon + 1);
    st.wDayOfWeek = static_cast<WORD>(tmb.tm_wday);
    st.wDay = static_cast<WORD>(tmb.tm_mday);
    st.wHour = static_cast<WORD>(tmb.tm_hour);
    st.wMinute = static_cast<WORD>(tmb.tm_min);
    st.wSecond = static_cast<WORD>(tmb.tm_sec);
    st.wMilliseconds = 0;
}

// 常用占位符：d dd M MM yyyy HH mm ss（M = 月、m = 分，按 Windows 常规约定）。
void expand_placeholders(std::wstring_view fmt, const SYSTEMTIME& st, std::wstring& out) {
    auto two = [&](unsigned v) {
        out.push_back(static_cast<wchar_t>(L'0' + v / 10));
        out.push_back(static_cast<wchar_t>(L'0' + v % 10));
    };
    for (size_t i = 0; i < fmt.size();) {
        wchar_t c = fmt[i];
        size_t n = 1;
        while (i + n < fmt.size() && fmt[i + n] == c) ++n;
        i += n;
        switch (c) {
            case L'd': case L'D':
                if (n >= 2) two(st.wDay);
                else out.push_back(static_cast<wchar_t>(L'0' + st.wDay));
                break;
            case L'm':
                if (n >= 2) two(st.wMinute);
                else out.push_back(static_cast<wchar_t>(L'0' + st.wMinute));
                break;
            case L'M':
                if (n >= 2) two(st.wMonth);
                else out.push_back(static_cast<wchar_t>(L'0' + st.wMonth));
                break;
            case L'y': case L'Y':
                if (n >= 4) out += std::to_wstring(static_cast<unsigned>(st.wYear));
                else two(static_cast<unsigned>(st.wYear % 100));
                break;
            case L'H': case L'h':
                if (n >= 2) two(st.wHour);
                else out.push_back(static_cast<wchar_t>(L'0' + st.wHour));
                break;
            case L's': case L'S':
                if (n >= 2) two(st.wSecond);
                else out.push_back(static_cast<wchar_t>(L'0' + st.wSecond));
                break;
            case L't': out += (st.wHour < 12 ? L"AM" : L"PM"); break;
            default: out.append(fmt.substr(i - n, n)); break;
        }
    }
}

int format_date_impl(const SYSTEMTIME& st, DWORD flags, LPCWSTR fmt, LPWSTR out, int cch) {
    UNREFERENCED_PARAMETER(flags);  // DATE_SHORTDATE / DATE_LONGDATE 一律用默认格式
    std::wstring s;
    if (!fmt) {
        s += std::to_wstring(static_cast<unsigned>(st.wYear));
        s.push_back(L'-');
        s.push_back(static_cast<wchar_t>(L'0' + st.wMonth / 10));
        s.push_back(static_cast<wchar_t>(L'0' + st.wMonth % 10));
        s.push_back(L'-');
        s.push_back(static_cast<wchar_t>(L'0' + st.wDay / 10));
        s.push_back(static_cast<wchar_t>(L'0' + st.wDay % 10));
    } else {
        expand_placeholders(fmt, st, s);
    }
    return write_wide(s, out, cch);
}

int format_time_impl(const SYSTEMTIME& st, DWORD flags, LPCWSTR fmt, LPWSTR out, int cch) {
    std::wstring s;
    bool noSec = (flags & TIME_NOSECONDS) != 0;
    if (!fmt) {
        s.push_back(static_cast<wchar_t>(L'0' + st.wHour / 10));
        s.push_back(static_cast<wchar_t>(L'0' + st.wHour % 10));
        s.push_back(L':');
        s.push_back(static_cast<wchar_t>(L'0' + st.wMinute / 10));
        s.push_back(static_cast<wchar_t>(L'0' + st.wMinute % 10));
        if (!noSec) {
            s.push_back(L':');
            s.push_back(static_cast<wchar_t>(L'0' + st.wSecond / 10));
            s.push_back(static_cast<wchar_t>(L'0' + st.wSecond % 10));
        }
    } else {
        expand_placeholders(fmt, st, s);
    }
    return write_wide(s, out, cch);
}

int format_date_narrow(const SYSTEMTIME& st, DWORD flags, LPCSTR fmt, LPSTR out, int cch) {
    std::wstring wfmt;
    if (fmt) wfmt = mol_shim::utf8_to_wide(fmt);
    int need = format_date_impl(st, flags, wfmt.empty() ? nullptr : wfmt.c_str(), nullptr, 0);
    if (cch == 0) return need;
    std::wstring buf(static_cast<size_t>(need), L'\0');
    format_date_impl(st, flags, wfmt.empty() ? nullptr : wfmt.c_str(), buf.data(), need);
    std::string narrow = mol_shim::wide_to_utf8(buf.c_str());
    int nlen = static_cast<int>(narrow.size()) + 1;
    if (nlen > cch) {
        mol_shim::last_error_set(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::memcpy(out, narrow.c_str(), static_cast<size_t>(nlen));
    return nlen;
}

int format_time_narrow(const SYSTEMTIME& st, DWORD flags, LPCSTR fmt, LPSTR out, int cch) {
    std::wstring wfmt;
    if (fmt) wfmt = mol_shim::utf8_to_wide(fmt);
    int need = format_time_impl(st, flags, wfmt.empty() ? nullptr : wfmt.c_str(), nullptr, 0);
    if (cch == 0) return need;
    std::wstring buf(static_cast<size_t>(need), L'\0');
    format_time_impl(st, flags, wfmt.empty() ? nullptr : wfmt.c_str(), buf.data(), need);
    std::string narrow = mol_shim::wide_to_utf8(buf.c_str());
    int nlen = static_cast<int>(narrow.size()) + 1;
    if (nlen > cch) {
        mol_shim::last_error_set(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::memcpy(out, narrow.c_str(), static_cast<size_t>(nlen));
    return nlen;
}

// FILETIME -> (自 1601 的秒、毫秒、100ns 刻度)。越界返回 false。
bool filetime_to_time(const FILETIME& ft, time_t& secs, long& millis) {
    uint64_t v = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    constexpr uint64_t kMaxTicks = 2650467743990000000ULL;  // 9999-12-31T23:59:59
    if (v > kMaxTicks) return false;
    secs = static_cast<time_t>(v / 10000000ULL - 11644473600ULL);
    millis = static_cast<long>((v % 10000000ULL) / 10000ULL);
    return true;
}

// freedesktop 回收站：$XDG_DATA_HOME（或 ~/.local/share）下的 Trash/files 与 Trash/info。
// 返回 0 成功，否则 Win32 错误码。
int trash_path(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string data = (xdg && *xdg) ? std::string(xdg) : std::string();
    if (data.empty()) {
        const char* home = std::getenv("HOME");
        if (home && *home) data = std::string(home) + "/.local/share";
    }
    if (data.empty()) data = "/tmp";
    fs::path files = fs::path(data) / "Trash" / "files";
    fs::path info = fs::path(data) / "Trash" / "info";
    fs::create_directories(files, ec);
    fs::create_directories(info, ec);

    fs::path src(path);
    std::string base = src.filename().string();
    if (base.empty() || base == "." || base == "..") return ERROR_BAD_PATHNAME;
    fs::path dest = files / base;
    int suffix = 1;
    while (fs::exists(dest, ec) || fs::exists(fs::symlink_status(dest, ec))) {
        ++suffix;  // 重名加 ".2" ".3" 后缀
        dest = files / (base + "." + std::to_string(suffix));
    }
    fs::rename(src, dest, ec);
    if (ec) {  // 跨设备：复制后删除
        auto opts = fs::is_directory(src, ec) ? fs::copy_options::recursive
                                             : fs::copy_options::overwrite_existing;
        ec.clear();
        fs::copy(src, dest, opts, ec);
        if (ec) return static_cast<int>(mol_shim::errno_to_win(ec.value()));
        fs::remove_all(src, ec);
        if (ec) return static_cast<int>(mol_shim::errno_to_win(ec.value()));
    }
    // trashinfo：<name>.trashinfo，含 Path 与 DeletionDate。
    fs::path infopath = info / (dest.filename().string() + ".trashinfo");
    char ts[64];
    time_t t = ::time(nullptr);
    struct tm tmb {};
    ::localtime_r(&t, &tmb);
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tmb);
    FILE* f = std::fopen(infopath.c_str(), "w");
    if (!f) return static_cast<int>(mol_shim::errno_to_win(errno));
    std::fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", fs::absolute(src).c_str(), ts);
    std::fclose(f);
    return 0;
}

}  // namespace

// ---- 错误消息 -----------------------------------------------------------------
DWORD FormatMessageW(DWORD flags, LPCVOID src, DWORD msgId, DWORD langId, LPWSTR buf, DWORD size,
                     void* args) {
    UNREFERENCED_PARAMETER(langId);
    if (!(flags & (FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_STRING))) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::wstring text = render_message(flags, src, msgId, args);
    DWORD needed = static_cast<DWORD>(text.size());
    bool alloc = (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) != 0;
    if (buf == nullptr) {
        if (alloc) {  // ALLOCATE_BUFFER 需要一个非 NULL 的出指针
            mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
            return 0;
        }
        return needed;  // size == 0：查询所需大小
    }
    if (alloc) {
        // buf 指向调用方的 LPWSTR 变量；用 malloc 分配，调用方用 LocalFree 释放。
        auto* mem =
            static_cast<LPWSTR>(std::malloc((static_cast<size_t>(needed) + 1) * sizeof(wchar_t)));
        if (!mem) {
            mol_shim::last_error_set(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        std::wmemcpy(mem, text.c_str(), static_cast<size_t>(needed) + 1);
        *reinterpret_cast<LPWSTR*>(buf) = mem;
        return needed;
    }
    if (size == 0) return needed;  // 查询所需大小
    if (needed + 1 > size) {  // size 计入可写字符数（含 NUL）
        std::wmemcpy(buf, text.c_str(), static_cast<size_t>(size - 1));
        buf[size - 1] = 0;
        mol_shim::last_error_set(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::wmemcpy(buf, text.c_str(), static_cast<size_t>(needed) + 1);
    return needed;
}

void* LocalFree(void* p) {
    std::free(p);
    return nullptr;
}

// ---- 调试输出 / 模块 ----------------------------------------------------------
BOOL IsDebuggerPresent() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return FALSE;
    char line[512];
    int tracer = -1;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "TracerPid:", 10) == 0) {
            tracer = std::atoi(line + 10);
            break;
        }
    }
    std::fclose(f);
    return tracer > 0 ? TRUE : FALSE;
}

void OutputDebugStringW(LPCWSTR s) {
    if (!s) return;
    std::fputws(s, stderr);
    std::fflush(stderr);
}

void OutputDebugStringA(LPCSTR s) {
    if (!s) return;
    std::fputs(s, stderr);
    std::fflush(stderr);
}

HMODULE GetModuleHandleW(LPCWSTR) {
    // 不返回 nullptr（调用方常用它判断"能否解析"），给一个稳定的假句柄。
    static char storage;
    return reinterpret_cast<HMODULE>(&storage);
}

DWORD GetModuleFileNameW(HMODULE module, LPWSTR buf, DWORD n) {
    UNREFERENCED_PARAMETER(module);
    char raw[4096];
    ssize_t r = ::readlink("/proc/self/exe", raw, sizeof(raw) - 1);
    std::string path =
        (r > 0) ? std::string(raw, static_cast<size_t>(r)) : std::string("/proc/self/exe");
    std::wstring w = unix_paths_mode() ? mol_shim::utf8_to_wide(path)
                                      : mol_shim::to_windows_path(path);
    DWORD needed = static_cast<DWORD>(w.size());
    if (n == 0) return needed;
    if (needed + 1 > n) mol_shim::last_error_set(ERROR_INSUFFICIENT_BUFFER);
    DWORD copy = needed < n - 1 ? needed : n - 1;
    std::wmemcpy(buf, w.c_str(), static_cast<size_t>(copy));
    buf[copy] = 0;
    return needed;
}

DWORD ExpandEnvironmentStringsW(LPCWSTR src, LPWSTR buf, DWORD n) {
    if (!src) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::wstring out;
    for (const wchar_t* p = src; *p;) {
        if (*p != L'%') {
            out.push_back(*p++);
            continue;
        }
        const wchar_t* q = p + 1;
        while (*q && *q != L'%') ++q;
        if (*q != L'%') {  // 无闭合百分号：原样保留
            out.push_back(*p++);
            continue;
        }
        if (q == p + 1) {  // "%%"
            out.push_back(L'%');
            p = q + 1;
            continue;
        }
        std::string name = mol_shim::wide_to_utf8(std::wstring(p + 1, q).c_str());
        std::wstring val = lookup_var(name);
        if (val.empty() && !known_builtin_var(name)) {  // 未知变量原样保留
            out.push_back(L'%');
            out.append(mol_shim::utf8_to_wide(name));
            out.push_back(L'%');
        } else {
            out.append(val);
        }
        p = q + 1;
    }
    DWORD needed = static_cast<DWORD>(out.size()) + 1;  // 含 NUL
    if (n == 0) return needed;
    if (needed > n) {
        mol_shim::last_error_set(ERROR_BUFFER_OVERFLOW);
        std::wmemcpy(buf, out.c_str(), static_cast<size_t>(n - 1));
        buf[n - 1] = 0;
        return needed;
    }
    std::wmemcpy(buf, out.c_str(), static_cast<size_t>(needed));
    return needed;
}

DWORD GetLongPathNameW(LPCWSTR src, LPWSTR buf, DWORD n) {
    if (!src) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::wstring wsrc = mol_shim::utf8_to_wide(mol_shim::wide_to_utf8(src));
    DWORD needed = static_cast<DWORD>(wsrc.size()) + 1;
    if (n == 0) return needed;  // 查询
    if (needed > n) return needed;  // 缓冲不足：不写入，返回所需长度
    std::wmemcpy(buf, wsrc.c_str(), static_cast<size_t>(needed));
    return needed - 1;
}

DWORD GetShortPathNameW(LPCWSTR src, LPWSTR buf, DWORD n) {
    if (!src) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::wstring wsrc = mol_shim::utf8_to_wide(mol_shim::wide_to_utf8(src));
    DWORD needed = static_cast<DWORD>(wsrc.size()) + 1;
    if (n == 0) return needed;
    if (needed > n) return needed;
    std::wmemcpy(buf, wsrc.c_str(), static_cast<size_t>(needed));
    return needed - 1;
}

// ---- 消息框 / 标准句柄 / 控制台 ------------------------------------------------
int MessageBoxW(HWND, LPCWSTR text, LPCWSTR caption, UINT) {
    if (caption) std::fwprintf(stderr, L"[%s] ", caption);
    if (text) std::fwprintf(stderr, L"%s\n", text);
    else std::fwprintf(stderr, L"\n");
    std::fflush(stderr);
    return 1;  // IDOK
}

HANDLE GetStdHandle(DWORD id) {
    if (id == STD_INPUT_HANDLE) return reinterpret_cast<HANDLE>(static_cast<intptr_t>(0));
    if (id == STD_OUTPUT_HANDLE) return reinterpret_cast<HANDLE>(static_cast<intptr_t>(1));
    if (id == STD_ERROR_HANDLE) return reinterpret_cast<HANDLE>(static_cast<intptr_t>(2));
    mol_shim::last_error_set(ERROR_INVALID_HANDLE);
    return nullptr;
}

BOOL GetConsoleMode(HANDLE, LPDWORD) {
    mol_shim::last_error_set(ERROR_INVALID_HANDLE);
    return FALSE;
}

// ---- 进程 / shell：不真正启动（由 mo-linux 自己的 runner 负责） -----------------
BOOL ShellExecuteExW(SHELLEXECUTEINFOW* se) {
    if (se) {
        se->hInstApp = reinterpret_cast<HMODULE>(static_cast<intptr_t>(2));
        se->hProcess = nullptr;
    }
    mol_shim::last_error_set(kErrorCallNotImplemented);
    return FALSE;
}

HMODULE ShellExecuteW(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, int) {
    mol_shim::last_error_set(kErrorCallNotImplemented);
    return reinterpret_cast<HMODULE>(static_cast<intptr_t>(2));  // <=32 视为失败
}

BOOL CreateProcessW(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                    LPVOID, LPCWSTR, STARTUPINFOW*, PROCESS_INFORMATION* pi) {
    if (pi) {
        pi->hProcess = nullptr;
        pi->hThread = nullptr;
        pi->dwProcessId = 0;
        pi->dwThreadId = 0;
    }
    mol_shim::last_error_set(kErrorCallNotImplemented);
    return FALSE;
}

// ---- shell 目录 ---------------------------------------------------------------
int SHCreateDirectory(HWND, LPCWSTR path) {
    if (!path) return ERROR_BAD_PATHNAME;
    std::string p = mol_shim::native_path(path);
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return ERROR_ALREADY_EXISTS;
    if (std::filesystem::create_directories(p, ec)) return ERROR_SUCCESS;
    if (!ec) return ERROR_BAD_PATHNAME;
    return static_cast<int>(mol_shim::errno_to_win(ec.value()));
}

// ---- SHFileOperationW：FO_COPY / FO_MOVE / FO_RENAME ---------------------------------
// 上游 uibase 的 shellCopy/shellMove/shellRename 全部走这里（例如 initializeProfile 的 copyToProfile）。
// 语义取 Windows 行为的无界面子集：覆盖已有文件、必要时创建父目录、目录递归、源的最后一个成分可含 * ?。
namespace {

std::vector<std::wstring> split_multi_nul(const wchar_t* p) {
    std::vector<std::wstring> out;
    while (p && *p) {
        size_t len = std::wcslen(p);
        out.emplace_back(p, len);
        p += len + 1;
    }
    return out;
}

bool glob_match_ci(std::string_view pat, std::string_view name) {
    auto lc = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; };
    size_t p = 0, n = 0, star = std::string_view::npos, mark = 0;
    while (n < name.size()) {
        if (p < pat.size() && (pat[p] == '?' || lc(pat[p]) == lc(name[n]))) { ++p; ++n; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = n; }
        else if (star != std::string_view::npos) { p = star + 1; n = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

// 展开一个源：含通配符则枚举父目录（字典序），否则原样。
std::vector<std::filesystem::path> expand_source(const std::wstring& w) {
    namespace fs = std::filesystem;
    std::string unix = mol_shim::native_path(w.c_str());
    fs::path p(unix);
    std::string leaf = p.filename().string();
    std::vector<fs::path> out;
    if (leaf.find_first_of("*?") == std::string::npos) {
        out.push_back(p);
        return out;
    }
    std::error_code ec;
    for (fs::directory_iterator it(p.parent_path(), ec), end; !ec && it != end; it.increment(ec))
        if (glob_match_ci(leaf, it->path().filename().string())) out.push_back(it->path());
    std::sort(out.begin(), out.end());
    return out;
}

int transfer_one(const std::filesystem::path& src, const std::filesystem::path& dst, bool move) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::file_status st = fs::symlink_status(src, ec);
    if (ec || !fs::exists(st)) return ERROR_FILE_NOT_FOUND;
    if (!dst.parent_path().empty()) {
        fs::create_directories(dst.parent_path(), ec);
        if (ec) return static_cast<int>(mol_shim::errno_to_win(ec.value()));
    }
    if (move) {
        fs::rename(src, dst, ec);
        if (!ec) return 0;
        if (ec != std::errc::cross_device_link && ec != std::errc::file_exists && ec != std::errc::directory_not_empty)
            return static_cast<int>(mol_shim::errno_to_win(ec.value()));
    }
    if (fs::is_directory(st) && !fs::is_symlink(st)) {
        fs::create_directories(dst, ec);
        if (!ec)
            fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing |
                                   fs::copy_options::copy_symlinks, ec);
    } else if (fs::is_symlink(st)) {
        fs::remove(dst, ec);
        ec.clear();
        fs::copy_symlink(src, dst, ec);
    } else {
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    }
    if (ec) return static_cast<int>(mol_shim::errno_to_win(ec.value()));
    if (move) {
        fs::remove_all(src, ec);
        if (ec) return static_cast<int>(mol_shim::errno_to_win(ec.value()));
    }
    return 0;
}

int shell_transfer(const SHFILEOPSTRUCTW* op) {
    namespace fs = std::filesystem;
    const bool move = op->wFunc == FO_MOVE || op->wFunc == FO_RENAME;
    const auto froms = split_multi_nul(op->pFrom);
    const auto tos = split_multi_nul(op->pTo);
    if (froms.empty() || tos.empty()) return ERROR_INVALID_PARAMETER;
    const bool multi = (op->fFlags & FOF_MULTIDESTFILES) != 0;
    if (multi && tos.size() != froms.size()) return ERROR_INVALID_PARAMETER;

    for (size_t i = 0; i < froms.size(); ++i) {
        const auto sources = expand_source(froms[i]);
        if (sources.empty()) return ERROR_FILE_NOT_FOUND;
        const std::wstring& to_w = multi ? tos[i] : tos[0];
        const fs::path to(mol_shim::native_path(to_w.c_str()));
        std::error_code ec;
        const bool to_is_dir = fs::is_directory(to, ec);
        const bool to_trailing_slash = !to_w.empty() && (to_w.back() == L'\\' || to_w.back() == L'/');
        // 目标是目录（已存在/以分隔符结尾/多个源）→ 放进目录；否则目标就是新名字。
        const bool into_dir = !multi && (to_is_dir || to_trailing_slash || froms.size() > 1 || sources.size() > 1);
        for (const auto& src : sources) {
            fs::path dst = into_dir ? to / src.filename() : to;
            if (op->wFunc == FO_RENAME && into_dir) dst = to;
            int rc = transfer_one(src, dst, move);
            if (rc != 0) return rc;
        }
    }
    return 0;
}

}  // namespace

int SHFileOperationW(SHFILEOPSTRUCTW* op) {
    if (!op) return ERROR_INVALID_PARAMETER;
    if (op->wFunc == FO_COPY || op->wFunc == FO_MOVE || op->wFunc == FO_RENAME) return shell_transfer(op);
    if (op->wFunc != FO_DELETE) return kErrorCallNotImplemented;  // 只支持 FO_DELETE
    bool undo = (op->fFlags & FOF_ALLOWUNDO) != 0;
    if (op->pFrom) {
        // pFrom：以 NUL 分隔、双 NUL 结尾的路径列表。
        const wchar_t* p = op->pFrom;
        while (*p) {
            size_t len = std::wcslen(p);
            std::wstring item(p, len);
            std::string unix = mol_shim::native_path(item.c_str());
            std::error_code ec;
            bool present = std::filesystem::exists(std::filesystem::symlink_status(unix, ec)) ||
                           std::filesystem::exists(unix, ec);
            if (present) {  // 不存在的路径当成功
                int rc;
                if (undo) {
                    rc = trash_path(unix);
                } else {  // 直接递归删除（符号链接只删链接本身）
                    std::filesystem::remove_all(unix, ec);
                    rc = ec ? static_cast<int>(mol_shim::errno_to_win(ec.value())) : 0;
                }
                if (rc != 0) return rc;
            }
            p += len + 1;
        }
    }
    return 0;
}

// ---- 已知文件夹 / COM 占位 -----------------------------------------------------
HRESULT SHGetKnownFolderPath(REFKNOWNFOLDERID id, DWORD flags, HANDLE, wchar_t** out) {
    if (!out) return E_FAIL;
    *out = nullptr;
    const std::string user = mol_shim::user();
    const std::string roaming = "users/" + user + "/AppData/Roaming";

    std::string suffix;
    if (id == FOLDERID_Documents) suffix = "users/" + user + "/Documents";
    else if (id == FOLDERID_LocalAppData) suffix = "users/" + user + "/AppData/Local";
    else if (id == FOLDERID_RoamingAppData) suffix = roaming;
    else if (id == FOLDERID_ProgramData) suffix = "ProgramData";
    else if (id == FOLDERID_ProgramFiles) suffix = "Program Files";
    else if (id == FOLDERID_ProgramFilesX86) suffix = "Program Files (x86)";
    else if (id == FOLDERID_Desktop) suffix = "users/" + user + "/Desktop";
    else if (id == FOLDERID_Profile) suffix = "users/" + user;
    else if (id == FOLDERID_StartMenu) suffix = roaming + "/Microsoft/Windows/Start Menu";
    else if (id == FOLDERID_Windows) suffix = "windows";
    else return E_FAIL;  // 未知 GUID

    std::wstring w = folder_path(suffix);
    if ((flags & KF_FLAG_CREATE) && !mol_shim::prefix().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(drive_c(suffix), ec);
    }
    auto* mem = static_cast<wchar_t*>(std::malloc((w.size() + 1) * sizeof(wchar_t)));
    if (!mem) return E_FAIL;
    std::wmemcpy(mem, w.c_str(), w.size() + 1);
    *out = mem;
    return S_OK;
}

void CoTaskMemFree(void* p) { std::free(p); }

HRESULT CoCreateInstance(REFCLSID, void*, DWORD, REFIID, void** out) {
    if (out) *out = nullptr;
    return E_FAIL;  // Linux 上无 COM
}

HRESULT CoInitialize(void*) { return S_OK; }

void CoUninitialize() {}

// ---- 日期时间 -----------------------------------------------------------------
int GetDateFormatEx(LPCWSTR, DWORD flags, const SYSTEMTIME* st, LPCWSTR fmt, LPWSTR out, int cch,
                    LPCWSTR) {
    SYSTEMTIME local;
    if (!st) { now_local(local); st = &local; }
    return format_date_impl(*st, flags, fmt, out, cch);
}

int GetDateFormatW(DWORD, DWORD flags, const SYSTEMTIME* st, LPCWSTR fmt, LPWSTR out, int cch) {
    SYSTEMTIME local;
    if (!st) { now_local(local); st = &local; }
    return format_date_impl(*st, flags, fmt, out, cch);
}

int GetTimeFormatEx(LPCWSTR, DWORD flags, const SYSTEMTIME* st, LPCWSTR fmt, LPWSTR out, int cch) {
    SYSTEMTIME local;
    if (!st) { now_local(local); st = &local; }
    return format_time_impl(*st, flags, fmt, out, cch);
}

int GetTimeFormatW(DWORD, DWORD flags, const SYSTEMTIME* st, LPCWSTR fmt, LPWSTR out, int cch) {
    SYSTEMTIME local;
    if (!st) { now_local(local); st = &local; }
    return format_time_impl(*st, flags, fmt, out, cch);
}

int GetDateFormatA(DWORD, DWORD flags, const SYSTEMTIME* st, LPCSTR fmt, LPSTR out, int cch) {
    SYSTEMTIME local;
    if (!st) { now_local(local); st = &local; }
    return format_date_narrow(*st, flags, fmt, out, cch);
}

int GetTimeFormatA(DWORD, DWORD flags, const SYSTEMTIME* st, LPCSTR fmt, LPSTR out, int cch) {
    SYSTEMTIME local;
    if (!st) { now_local(local); st = &local; }
    return format_time_narrow(*st, flags, fmt, out, cch);
}

BOOL FileTimeToSystemTime(const FILETIME* ft, SYSTEMTIME* st) {
    if (!ft || !st) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    time_t secs;
    long millis;
    if (!filetime_to_time(*ft, secs, millis)) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    struct tm tmb {};
    if (!::gmtime_r(&secs, &tmb)) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    st->wYear = static_cast<WORD>(tmb.tm_year + 1900);
    st->wMonth = static_cast<WORD>(tmb.tm_mon + 1);
    st->wDayOfWeek = static_cast<WORD>(tmb.tm_wday);
    st->wDay = static_cast<WORD>(tmb.tm_mday);
    st->wHour = static_cast<WORD>(tmb.tm_hour);
    st->wMinute = static_cast<WORD>(tmb.tm_min);
    st->wSecond = static_cast<WORD>(tmb.tm_sec);
    st->wMilliseconds = static_cast<WORD>(millis);
    return TRUE;
}

BOOL FileTimeToLocalFileTime(const FILETIME* in, FILETIME* out) {
    if (!in || !out) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    time_t secs;
    long millis;
    if (!filetime_to_time(*in, secs, millis)) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    struct tm tmb {};
    if (!::localtime_r(&secs, &tmb)) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    tmb.tm_isdst = -1;
    time_t local = ::timegm(&tmb);  // 本地时间 -> 时间戳，得到本地时区偏移
    uint64_t v = (static_cast<uint64_t>(local) + 11644473600ULL) * 10000000ULL +
                 static_cast<uint64_t>(millis) * 10000ULL;
    out->dwLowDateTime = static_cast<DWORD>(v & 0xFFFFFFFFu);
    out->dwHighDateTime = static_cast<DWORD>(v >> 32);
    return TRUE;
}

BOOL SystemTimeToFileTime(const SYSTEMTIME* st, FILETIME* ft) {
    if (!st || !ft) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (st->wYear < 1601 || st->wMonth < 1 || st->wMonth > 12 || st->wDay < 1 || st->wDay > 31 ||
        st->wHour > 23 || st->wMinute > 59 || st->wSecond > 59) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    struct tm tmb {};
    tmb.tm_year = static_cast<int>(st->wYear) - 1900;
    tmb.tm_mon = static_cast<int>(st->wMonth) - 1;
    tmb.tm_mday = static_cast<int>(st->wDay);
    tmb.tm_hour = static_cast<int>(st->wHour);
    tmb.tm_min = static_cast<int>(st->wMinute);
    tmb.tm_sec = static_cast<int>(st->wSecond);
    time_t secs = ::timegm(&tmb);
    if (secs == static_cast<time_t>(-1)) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    uint64_t v = (static_cast<uint64_t>(secs) + 11644473600ULL) * 10000000ULL +
                 static_cast<uint64_t>(st->wMilliseconds) * 10000ULL;
    ft->dwLowDateTime = static_cast<DWORD>(v & 0xFFFFFFFFu);
    ft->dwHighDateTime = static_cast<DWORD>(v >> 32);
    return TRUE;
}
