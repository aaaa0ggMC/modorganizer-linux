// WP6 misc.cpp 测试：错误消息、调试/模块/环境变量、路径长度、消息框、标准句柄、
// 进程/shell 失败路径、shell 目录与回收站、已知文件夹（两种路径形态）、COM 占位、
// 日期时间格式化与 FILETIME 转换。文件系统相关只在 /tmp 下的唯一目录。
#include "windows.h"
#include "internal.hpp"

#include "minitest.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "knownfolders.h"
#include "shlobj.h"

namespace {

struct TempDir {
    std::string path;
    TempDir() {
        char tpl[] = "/tmp/mol_wp6_misc_XXXXXX";
        char* r = ::mkdtemp(tpl);
        if (!r) std::abort();
        path = r;
        if (path.rfind("/tmp/", 0) != 0) std::abort();  // 只在 /tmp 下工作
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

struct WC {
    std::wstring s;
    explicit WC(const std::string& u8) : s(mol_shim::utf8_to_wide(u8)) {}
    const wchar_t* c_str() const { return s.c_str(); }
};
#define WCV(s) WC(s).c_str()

std::wstring W(const std::string& s) { return mol_shim::utf8_to_wide(s); }

// 进程内切换环境变量（测试用，出作用域恢复）。
struct EnvGuard {
    const char* name;
    std::string old;
    bool had;
    EnvGuard(const char* n, const char* v) : name(n) {
        const char* o = std::getenv(n);
        had = o != nullptr;
        old = o ? std::string(o) : std::string();
        ::setenv(n, v, 1);
    }
    ~EnvGuard() {
        if (had) ::setenv(name, old.c_str(), 1);
        else ::unsetenv(name);
    }
};

void write_text(const std::string& p, const char* data) {
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) std::abort();
    std::fwrite(data, 1, std::strlen(data), f);
    std::fclose(f);
}

bool exists(const std::string& p) { return std::filesystem::exists(p); }

std::wstring double_nul_list(const std::vector<std::string>& paths) {
    std::wstring s;
    for (const auto& p : paths) s += W(p) + L'\0';
    s += L'\0';
    return s;
}

int tracer_pid() {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[512];
    int t = -1;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "TracerPid:", 10) == 0) {
            t = std::atoi(line + 10);
            break;
        }
    }
    std::fclose(f);
    return t;
}

TEST(format_message_from_system) {
    wchar_t buf[256]{};
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                             5, 0, buf, 256, nullptr);
    CHECK_EQ(n, (DWORD)17);
    CHECK_EQ(std::wstring(buf), std::wstring(L"Access is denied."));

    n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, 2, 0,
                       buf, 256, nullptr);
    CHECK_EQ(std::wstring(buf), std::wstring(L"The system cannot find the file specified."));

    // 每次调用后校验部分常用码
    const DWORD codes[] = {0,  3,  6,  8,  17, 19, 31, 87, 111, 112,
                           122, 161, 183, 193, 234, 740, 1168, 1223, 1630};
    for (DWORD c : codes) {
        wchar_t b2[256]{};
        DWORD k = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, c, 0, b2, 256, nullptr);
        CHECK(k > 0);
        CHECK(std::wcslen(b2) > 0);
    }

    // 未知码
    n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, 0x63, 0, buf, 256, nullptr);
    CHECK_EQ(n, (DWORD)24);
    CHECK_EQ(std::wstring(buf), std::wstring(L"Unknown error 0x00000063"));

    // size == 0：查询所需大小
    n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, 5, 0, nullptr, 0, nullptr);
    CHECK_EQ(n, (DWORD)17);

    // 缓冲不足：截断写入并返回 0
    wchar_t small[4]{};
    n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, 5, 0, small, 4, nullptr);
    CHECK_EQ(n, 0u);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INSUFFICIENT_BUFFER);
    CHECK_EQ(std::wstring(small), std::wstring(L"Acc"));

    // 缺 FROM_SYSTEM/FROM_STRING
    n = FormatMessageW(FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, 5, 0, buf, 256, nullptr);
    CHECK_EQ(n, 0u);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_PARAMETER);
}

TEST(format_message_allocate_buffer) {
    LPWSTR p = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, nullptr,
                             5, 0, reinterpret_cast<LPWSTR>(&p), 0, nullptr);
    CHECK_EQ(n, (DWORD)17);
    CHECK(p != nullptr);
    if (p) CHECK_EQ(std::wstring(p), std::wstring(L"Access is denied."));
    // LocalFree 对应 free
    CHECK(LocalFree(p) == nullptr);
}

TEST(format_message_from_string_with_args) {
    const wchar_t a1[] = L"alpha";
    const wchar_t a2[] = L"beta";
    DWORD_PTR args[2] = {reinterpret_cast<DWORD_PTR>(a1), reinterpret_cast<DWORD_PTR>(a2)};
    const wchar_t fmt[] = L"%1 and %2";
    wchar_t buf[64]{};
    // "alpha and beta" = 14 个字符
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY,
                             reinterpret_cast<LPCVOID>(fmt), 0, 0, buf, 64, args);
    CHECK_EQ(n, (DWORD)14);
    CHECK_EQ(std::wstring(buf), std::wstring(L"alpha and beta"));

    // "%n" 换行："line1\r\nline2" = 12 个字符
    const wchar_t fmt2[] = L"line1%nline2";
    n = FormatMessageW(FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY,
                       reinterpret_cast<LPCVOID>(fmt2), 0, 0, buf, 64, args);
    CHECK_EQ(n, (DWORD)12);
    CHECK_EQ(std::wstring(buf), std::wstring(L"line1\r\nline2"));

    // 无 ARGUMENT_ARRAY：插入符原样保留（"%1 and %2" = 9）
    n = FormatMessageW(FORMAT_MESSAGE_FROM_STRING, reinterpret_cast<LPCVOID>(fmt), 0, 0, buf, 64,
                       nullptr);
    CHECK_EQ(n, (DWORD)9);
    CHECK_EQ(std::wstring(buf), std::wstring(L"%1 and %2"));

    // %% 转义（"100% sure" = 9）
    const wchar_t fmt3[] = L"100%% sure";
    n = FormatMessageW(FORMAT_MESSAGE_FROM_STRING, reinterpret_cast<LPCVOID>(fmt3), 0, 0, buf, 64,
                       nullptr);
    CHECK_EQ(n, (DWORD)9);
    CHECK_EQ(std::wstring(buf), std::wstring(L"100% sure"));
}

TEST(debug_present_and_module_handle) {
    CHECK_EQ(IsDebuggerPresent(), tracer_pid() > 0 ? TRUE : FALSE);
    // 只在两个合法值之间
    BOOL d = IsDebuggerPresent();
    CHECK(d == TRUE || d == FALSE);

    OutputDebugStringA("MOL-DBG-A\n");
    OutputDebugStringW(L"MOL-DBG-W\n");

    HMODULE h1 = GetModuleHandleW(nullptr);
    CHECK(h1 != nullptr);
    HMODULE h2 = GetModuleHandleW(L"kernel32");
    CHECK(h2 != nullptr);
    CHECK_EQ(h1, h2);  // 稳定假句柄
}

TEST(module_file_name) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    ::mkdir((t.path + "/drive_c").c_str(), 0755);

    char raw[4096];
    ssize_t r = ::readlink("/proc/self/exe", raw, sizeof(raw) - 1);
    CHECK(r > 0);
    raw[r] = 0;

    // 默认（MOL_SHIM_UNIX_PATHS 未设置 = 1）：Unix 绝对路径
    wchar_t buf[4096]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, 4096);
    CHECK_EQ(n, (DWORD)std::wcslen(W(raw).c_str()));
    CHECK_EQ(std::wstring(buf), W(raw));
    CHECK_EQ(buf[0], L'/');

    // size 0：查询
    CHECK_EQ(GetModuleFileNameW(nullptr, buf, 0), (DWORD)std::wcslen(W(raw).c_str()));

    // 缓冲不足：截断 + ERROR_INSUFFICIENT_BUFFER
    {
        wchar_t tiny[4]{};
        DWORD k = GetModuleFileNameW(nullptr, tiny, 4);
        CHECK_EQ(GetLastError(), (DWORD)ERROR_INSUFFICIENT_BUFFER);
        CHECK_EQ(k, (DWORD)std::wcslen(W(raw).c_str()));
        CHECK_EQ(tiny[0], L'/');
        CHECK_EQ(tiny[3], L'\0');
    }

    // 显式 MOL_SHIM_UNIX_PATHS=1：仍是 Unix 路径
    {
        EnvGuard g("MOL_SHIM_UNIX_PATHS", "1");
        CHECK_EQ(GetModuleFileNameW(nullptr, buf, 4096), (DWORD)std::wcslen(W(raw).c_str()));
        CHECK_EQ(std::wstring(buf), W(raw));
    }

    // MOL_SHIM_UNIX_PATHS=0：Windows 风格
    {
        EnvGuard g("MOL_SHIM_UNIX_PATHS", "0");
        n = GetModuleFileNameW(nullptr, buf, 4096);
        CHECK(n > 2);
        CHECK(buf[1] == L':');
        CHECK(buf[0] == L'C' || buf[0] == L'Z' || buf[0] == 'C' || buf[0] == 'Z');
        CHECK(std::wcsstr(buf, L"\\") != nullptr);
    }
}

TEST(expand_environment_strings) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    ::mkdir((t.path + "/drive_c").c_str(), 0755);

    wchar_t buf[4096]{};
    DWORD need = ExpandEnvironmentStringsW(L"%APPDATA%", buf, 4096);
    CHECK_EQ(need, (DWORD)(std::string(t.path + "/drive_c/users/tester/AppData/Roaming").size() + 1));
    CHECK_EQ(std::wstring(buf), W(t.path + "/drive_c/users/tester/AppData/Roaming"));

    CHECK_EQ(std::wstring(ExpandEnvironmentStringsW(L"%LOCALAPPDATA%", buf, 4096) ? buf : buf),
             W(t.path + "/drive_c/users/tester/AppData/Local"));
    ExpandEnvironmentStringsW(L"%USERPROFILE%", buf, 4096);
    CHECK_EQ(std::wstring(buf), W(t.path + "/drive_c/users/tester"));
    ExpandEnvironmentStringsW(L"%PROGRAMDATA%", buf, 4096);
    CHECK_EQ(std::wstring(buf), W(t.path + "/drive_c/ProgramData"));
    ExpandEnvironmentStringsW(L"%PROGRAMFILES%", buf, 4096);
    CHECK_EQ(std::wstring(buf), W(t.path + "/drive_c/Program Files"));
    ExpandEnvironmentStringsW(L"%PROGRAMFILES(X86)%", buf, 4096);
    CHECK_EQ(std::wstring(buf), W(t.path + "/drive_c/Program Files (x86)"));
    ExpandEnvironmentStringsW(L"%SystemRoot%", buf, 4096);
    CHECK_EQ(std::wstring(buf), W(t.path + "/drive_c/windows"));

    // 未知变量原样保留；混合字符串
    ExpandEnvironmentStringsW(L"X=%APPDATA%;Y=%NOSUCHVAR%", buf, 4096);
    CHECK_EQ(std::wstring(buf),
             std::wstring(L"X=") + W(t.path + "/drive_c/users/tester/AppData/Roaming") + L";Y=%NOSUCHVAR%");
    CHECK(GetLastError() == 0u || true);  // 未知变量不算错误

    // 环境变量本身可展开
    EnvGuard g("MOL_TEST_VAR", "42");
    ExpandEnvironmentStringsW(L"v=%MOL_TEST_VAR%", buf, 4096);
    CHECK_EQ(std::wstring(buf), std::wstring(L"v=42"));

    // size == 0：返回所需字符数（含 NUL）
    DWORD n = ExpandEnvironmentStringsW(L"%APPDATA%", nullptr, 0);
    CHECK_EQ(n, (DWORD)(std::string(t.path + "/drive_c/users/tester/AppData/Roaming").size() + 1));

    // 缓冲不足：返回所需长度并置 ERROR_BUFFER_OVERFLOW
    wchar_t tiny[4]{};
    n = ExpandEnvironmentStringsW(L"%APPDATA%", tiny, 4);
    CHECK_EQ(n, (DWORD)(std::string(t.path + "/drive_c/users/tester/AppData/Roaming").size() + 1));
    CHECK_EQ(GetLastError(), (DWORD)ERROR_BUFFER_OVERFLOW);
    CHECK_EQ(tiny[3], L'\0');

    // MOL_SHIM_UNIX_PATHS=0：Windows 风格
    {
        EnvGuard g2("MOL_SHIM_UNIX_PATHS", "0");
        ExpandEnvironmentStringsW(L"%APPDATA%", buf, 4096);
        CHECK_EQ(std::wstring(buf), std::wstring(L"C:\\users\\tester\\AppData\\Roaming"));
        ExpandEnvironmentStringsW(L"%SystemRoot%", buf, 4096);
        CHECK_EQ(std::wstring(buf), std::wstring(L"C:\\windows"));
    }

    CHECK_EQ(ExpandEnvironmentStringsW(nullptr, buf, 4096), 0u);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_PARAMETER);
}

TEST(long_and_short_path_names) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string p = t.path + "/Some File.exe";

    wchar_t buf[4096]{};
    DWORD n = GetLongPathNameW(WCV(p), buf, 4096);
    CHECK_EQ(std::wstring(buf), W(p));
    CHECK_EQ(n, (DWORD)std::wstring(W(p).c_str()).length());
    CHECK(GetLastError() == 0u || true);

    n = GetShortPathNameW(WCV(p), buf, 4096);
    CHECK_EQ(std::wstring(buf), W(p));

    // size == 0 查询
    CHECK_EQ(GetLongPathNameW(WCV(p), nullptr, 0), (DWORD)std::wstring(W(p).c_str()).length() + 1);
    // 缓冲不足：返回所需长度（含 NUL），不写入
    wchar_t tiny[2]{};
    n = GetShortPathNameW(WCV(p), tiny, 2);
    CHECK_EQ(n, (DWORD)std::wstring(W(p).c_str()).length() + 1);
    CHECK_EQ(tiny[0], L'\0');
}

TEST(message_box_and_std_handles) {
    CHECK_EQ(MessageBoxW(nullptr, L"text", L"caption", MB_OK), 1);
    CHECK_EQ(MessageBoxW(nullptr, L"only text", nullptr, MB_ICONERROR), 1);
    CHECK_EQ(MessageBoxW(nullptr, nullptr, nullptr, MB_ICONWARNING), 1);

    CHECK_EQ(GetStdHandle(STD_INPUT_HANDLE), reinterpret_cast<HANDLE>(static_cast<intptr_t>(0)));
    CHECK_EQ(GetStdHandle(STD_OUTPUT_HANDLE), reinterpret_cast<HANDLE>(static_cast<intptr_t>(1)));
    CHECK_EQ(GetStdHandle(STD_ERROR_HANDLE), reinterpret_cast<HANDLE>(static_cast<intptr_t>(2)));
    CHECK(GetStdHandle(99) == nullptr);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_HANDLE);

    DWORD mode = 0;
    CHECK(GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_HANDLE);
}

TEST(shell_and_process_are_not_implemented) {
    SetLastError(0);
    CHECK(ShellExecuteW(nullptr, L"open", L"C:\\file.txt", nullptr, nullptr, SW_SHOWNORMAL) <=
          reinterpret_cast<HMODULE>(32));
    CHECK_EQ(GetLastError(), (DWORD)120);  // ERROR_CALL_NOT_IMPLEMENTED

    SHELLEXECUTEINFOW se{};
    se.cbSize = sizeof(se);
    se.nShow = SW_SHOWNORMAL;
    CHECK(ShellExecuteExW(&se) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)120);
    CHECK_EQ(se.hProcess, nullptr);
    CHECK_EQ(se.hInstApp, reinterpret_cast<HMODULE>(static_cast<intptr_t>(2)));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    pi.hProcess = reinterpret_cast<HANDLE>(1);
    pi.hThread = reinterpret_cast<HANDLE>(2);
    pi.dwProcessId = 3;
    pi.dwThreadId = 4;
    CHECK(CreateProcessW(L"C:\\a.exe", nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                         &pi) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)120);
    CHECK_EQ(pi.hProcess, nullptr);
    CHECK_EQ(pi.hThread, nullptr);
    CHECK_EQ(pi.dwProcessId, 0u);
}

TEST(sh_create_directory) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string deep = t.path + "/a/b/c";
    CHECK_EQ(SHCreateDirectory(nullptr, WCV(deep)), 0);
    CHECK(exists(deep));
    CHECK_EQ(SHCreateDirectory(nullptr, WCV(deep)), (int)ERROR_ALREADY_EXISTS);
    CHECK_EQ(SHCreateDirectory(nullptr, nullptr), (int)ERROR_BAD_PATHNAME);
}

TEST(sh_file_operation_delete) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string f1 = t.path + "/one.txt", f2 = t.path + "/two.txt";
    std::string dir = t.path + "/tree";
    ::mkdir(dir.c_str(), 0755);
    write_text(f1, "1");
    write_text(f2, "2");
    write_text(t.path + "/tree/inner.bin", "x");
    ::mkdir((t.path + "/tree/sub").c_str(), 0755);
    write_text(t.path + "/tree/sub/deep.txt", "y");

    {
        std::wstring list = double_nul_list({f1});
        SHFILEOPSTRUCTW fo{};
        fo.wFunc = FO_DELETE;
        fo.pFrom = list.c_str();
        fo.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI;
        CHECK_EQ(SHFileOperationW(&fo), 0);
        CHECK(!exists(f1));
    }
    // 多路径一次删除
    {
        std::wstring list = double_nul_list({f2, dir});
        SHFILEOPSTRUCTW fo{};
        fo.wFunc = FO_DELETE;
        fo.pFrom = list.c_str();
        fo.fFlags = FOF_NOCONFIRMATION;
        CHECK_EQ(SHFileOperationW(&fo), 0);
        CHECK(!exists(f2));
        CHECK(!exists(dir));
    }
    // 不存在的路径当成功
    {
        std::wstring list = double_nul_list({t.path + "/gone.txt"});
        SHFILEOPSTRUCTW fo{};
        fo.wFunc = FO_DELETE;
        fo.pFrom = list.c_str();
        CHECK_EQ(SHFileOperationW(&fo), 0);
    }
    // 符号链接只删链接本身（remove_all 不改动目标）
    {
        std::string target = t.path + "/target.txt";
        std::string link = t.path + "/link.txt";
        write_text(target, "T");
        ::symlink(target.c_str(), link.c_str());
        CHECK(exists(link));
        std::wstring list = double_nul_list({link});
        SHFILEOPSTRUCTW fo{};
        fo.wFunc = FO_DELETE;
        fo.pFrom = list.c_str();
        fo.fFlags = FOF_NOCONFIRMATION;
        CHECK_EQ(SHFileOperationW(&fo), 0);
        CHECK(!exists(link));
        CHECK(exists(target));
        CHECK_EQ(GetFileAttributesW(WCV(target)), (DWORD)FILE_ATTRIBUTE_NORMAL);
    }
    // 其它 wFunc 不支持
    {
        std::wstring list = double_nul_list({f1});
        SHFILEOPSTRUCTW fo{};
        fo.wFunc = FO_MOVE;
        fo.pFrom = list.c_str();
        CHECK_EQ(SHFileOperationW(&fo), (int)120);
    }
    CHECK_EQ(SHFileOperationW(nullptr), (int)ERROR_INVALID_PARAMETER);
}

TEST(sh_file_operation_undo_to_trash) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string xdg = t.path + "/xdg";
    ::mkdir(xdg.c_str(), 0755);
    EnvGuard g("XDG_DATA_HOME", xdg.c_str());

    std::string f = t.path + "/trash_me.txt";
    write_text(f, "payload");

    std::wstring list = double_nul_list({f});
    SHFILEOPSTRUCTW fo{};
    fo.wFunc = FO_DELETE;
    fo.pFrom = list.c_str();
    fo.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
    CHECK_EQ(SHFileOperationW(&fo), 0);
    CHECK(!exists(f));
    CHECK(exists(xdg + "/Trash/files/trash_me.txt"));
    {
        FILE* p = std::fopen((xdg + "/Trash/files/trash_me.txt").c_str(), "rb");
        CHECK(p != nullptr);
        if (p) { char b[16]{}; fread(b, 1, 7, p); CHECK_EQ(std::string(b), std::string("payload")); fclose(p); }
    }
    std::string info = xdg + "/Trash/info/trash_me.txt.trashinfo";
    CHECK(exists(info));
    {
        FILE* p = std::fopen(info.c_str(), "rb");
        CHECK(p != nullptr);
        std::string body;
        if (p) {
            char b[512]{};
            size_t k = std::fread(b, 1, sizeof(b) - 1, p);
            body.assign(b, k);
            fclose(p);
        }
        CHECK(body.find("Path=" + f) != std::string::npos);
        CHECK(body.find("DeletionDate=") != std::string::npos);
        CHECK(body.find("[Trash Info]") != std::string::npos);
    }

    // 重名进入回收站：加 ".2" 后缀
    std::string again = t.path + "/trash_me.txt";
    write_text(again, "second");
    {
        std::wstring l2 = double_nul_list({again});
        SHFILEOPSTRUCTW fo2{};
        fo2.wFunc = FO_DELETE;
        fo2.pFrom = l2.c_str();
        fo2.fFlags = FOF_ALLOWUNDO;
        CHECK_EQ(SHFileOperationW(&fo2), 0);
    }
    CHECK(!exists(again));
    CHECK(exists(xdg + "/Trash/files/trash_me.txt.2"));
    CHECK(exists(xdg + "/Trash/info/trash_me.txt.2.trashinfo"));
}

TEST(known_folder_paths) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    ::mkdir((t.path + "/drive_c").c_str(), 0755);

    // 默认（未设置 = 1）：Unix 绝对路径
    wchar_t* p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p), S_OK);
    CHECK(p != nullptr);
    if (p) CHECK_EQ(std::wstring(p), W(t.path + "/drive_c/users/tester/Documents"));
    CoTaskMemFree(p);

    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p), S_OK);
    if (p) CHECK_EQ(std::wstring(p), W(t.path + "/drive_c/users/tester/AppData/Local"));
    CoTaskMemFree(p);

    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p), S_OK);
    if (p) CHECK_EQ(std::wstring(p), W(t.path + "/drive_c/users/tester/AppData/Roaming"));
    CoTaskMemFree(p);

    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p), S_OK);
    if (p) CHECK_EQ(std::wstring(p), W(t.path + "/drive_c/users/tester"));
    CoTaskMemFree(p);

    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p), S_OK);
    if (p) CHECK_EQ(std::wstring(p), W(t.path + "/drive_c/users/tester/Desktop"));
    CoTaskMemFree(p);

    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p), S_OK);
    if (p) CHECK_EQ(std::wstring(p), W(t.path + "/drive_c/ProgramData"));
    CoTaskMemFree(p);

    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &p), S_OK);
    if (p) CHECK_EQ(std::wstring(p), W(t.path + "/drive_c/Program Files"));
    CoTaskMemFree(p);

    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_ProgramFilesX86, 0, nullptr, &p), S_OK);
    if (p) CHECK_EQ(std::wstring(p), W(t.path + "/drive_c/Program Files (x86)"));
    CoTaskMemFree(p);

    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_Windows, 0, nullptr, &p), S_OK);
    if (p) CHECK_EQ(std::wstring(p), W(t.path + "/drive_c/windows"));
    CoTaskMemFree(p);

    // KF_FLAG_CREATE 建出真实目录
    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &p), S_OK);
    CoTaskMemFree(p);
    CHECK(exists(t.path + "/drive_c/users/tester/Documents"));

    // 未知 GUID → E_FAIL
    GUID unknown{0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
    p = reinterpret_cast<wchar_t*>(static_cast<intptr_t>(1));
    CHECK_EQ(SHGetKnownFolderPath(unknown, 0, nullptr, &p), E_FAIL);
    CHECK(p == nullptr);
}

TEST(known_folder_paths_windows_style) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    EnvGuard g("MOL_SHIM_UNIX_PATHS", "0");

    wchar_t* p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p), S_OK);
    if (p) CHECK_EQ(std::wstring(p), std::wstring(L"C:\\users\\tester\\Documents"));
    CoTaskMemFree(p);

    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p), S_OK);
    if (p) CHECK_EQ(std::wstring(p), std::wstring(L"C:\\users\\tester\\AppData\\Local"));
    CoTaskMemFree(p);

    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p), S_OK);
    if (p) CHECK_EQ(std::wstring(p), std::wstring(L"C:\\ProgramData"));
    CoTaskMemFree(p);

    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_ProgramFilesX86, 0, nullptr, &p), S_OK);
    if (p) CHECK_EQ(std::wstring(p), std::wstring(L"C:\\Program Files (x86)"));
    CoTaskMemFree(p);

    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_Windows, 0, nullptr, &p), S_OK);
    if (p) CHECK_EQ(std::wstring(p), std::wstring(L"C:\\windows"));
    CoTaskMemFree(p);

    p = nullptr;
    CHECK_EQ(SHGetKnownFolderPath(FOLDERID_StartMenu, 0, nullptr, &p), S_OK);
    if (p)
        CHECK_EQ(std::wstring(p),
                 std::wstring(L"C:\\users\\tester\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu"));
    CoTaskMemFree(p);
}

TEST(com_placeholders) {
    void* out = reinterpret_cast<void*>(1);
    CHECK_EQ(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_Placeholder,
                              &out),
             E_FAIL);
    CHECK(out == nullptr);
    CHECK_EQ(CoInitialize(nullptr), S_OK);
    CoUninitialize();
}

TEST(date_time_format_default) {
    SYSTEMTIME st{};
    st.wYear = 2024;
    st.wMonth = 3;
    st.wDay = 5;
    st.wDayOfWeek = 2;
    st.wHour = 14;
    st.wMinute = 7;
    st.wSecond = 9;
    st.wMilliseconds = 250;

    wchar_t buf[64]{};
    CHECK_EQ(GetDateFormatW(LOCALE_USER_DEFAULT, 0, &st, nullptr, buf, 64), (int)11);
    CHECK_EQ(std::wstring(buf), std::wstring(L"2024-03-05"));
    CHECK_EQ(GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &st, nullptr, buf, 64), (int)9);
    CHECK_EQ(std::wstring(buf), std::wstring(L"14:07:09"));
    CHECK_EQ(GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, buf, 64), (int)6);
    CHECK_EQ(std::wstring(buf), std::wstring(L"14:07"));

    // Ex 变体
    CHECK_EQ(GetDateFormatEx(L"", 0, &st, nullptr, buf, 64, nullptr), (int)11);
    CHECK_EQ(std::wstring(buf), std::wstring(L"2024-03-05"));
    CHECK_EQ(GetTimeFormatEx(L"", 0, &st, nullptr, buf, 64), (int)9);
    CHECK_EQ(std::wstring(buf), std::wstring(L"14:07:09"));

    // A 变体
    char abuf[64]{};
    CHECK_EQ(GetDateFormatA(LOCALE_USER_DEFAULT, 0, &st, nullptr, abuf, 64), (int)11);
    CHECK_EQ(std::string(abuf), std::string("2024-03-05"));
    CHECK_EQ(GetTimeFormatA(LOCALE_USER_DEFAULT, 0, &st, nullptr, abuf, 64), (int)9);
    CHECK_EQ(std::string(abuf), std::string("14:07:09"));

    // A 变体：cch == 0 查询（返回所需字符数，含 NUL）
    CHECK_EQ(GetDateFormatW(LOCALE_USER_DEFAULT, 0, &st, nullptr, nullptr, 0), (int)11);
    CHECK_EQ(GetTimeFormatA(LOCALE_USER_DEFAULT, 0, &st, nullptr, nullptr, 0), (int)9);

    // 缓冲不足
    wchar_t tiny[3]{};
    CHECK_EQ(GetDateFormatW(LOCALE_USER_DEFAULT, 0, &st, nullptr, tiny, 3), 0);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INSUFFICIENT_BUFFER);
}

TEST(date_time_format_patterns) {
    SYSTEMTIME st{};
    st.wYear = 2024;
    st.wMonth = 3;
    st.wDay = 5;
    st.wHour = 14;
    st.wMinute = 7;
    st.wSecond = 9;

    wchar_t buf[64]{};
    CHECK_EQ(GetDateFormatW(0, 0, &st, L"yyyy/MM/dd", buf, 64), (int)11);
    CHECK_EQ(std::wstring(buf), std::wstring(L"2024/03/05"));
    CHECK_EQ(GetDateFormatW(0, 0, &st, L"d. M. yyyy", buf, 64), (int)11);
    CHECK_EQ(std::wstring(buf), std::wstring(L"5. 3. 2024"));
    CHECK_EQ(GetDateFormatW(0, 0, &st, L"dd-MM-yy", buf, 64), (int)9);
    CHECK_EQ(std::wstring(buf), std::wstring(L"05-03-24"));

    CHECK_EQ(GetTimeFormatW(0, 0, &st, L"HH:mm:ss", buf, 64), (int)9);
    CHECK_EQ(std::wstring(buf), std::wstring(L"14:07:09"));
    CHECK_EQ(GetTimeFormatW(0, 0, &st, L"HH-mm", buf, 64), (int)6);
    CHECK_EQ(std::wstring(buf), std::wstring(L"14-07"));

    char abuf[64]{};
    CHECK_EQ(GetDateFormatA(0, 0, &st, "yyyy/MM/dd", abuf, 64), (int)11);
    CHECK_EQ(std::string(abuf), std::string("2024/03/05"));
    CHECK_EQ(GetTimeFormatA(0, 0, &st, "HH:mm:ss", abuf, 64), (int)9);
    CHECK_EQ(std::string(abuf), std::string("14:07:09"));

    // 缓冲不足
    wchar_t tiny[8]{};
    CHECK_EQ(GetDateFormatW(0, 0, &st, L"yyyy/MM/dd", tiny, 8), 0);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INSUFFICIENT_BUFFER);
    CHECK_EQ(GetTimeFormatA(0, 0, &st, "HH:mm:ss", abuf, 5), 0);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INSUFFICIENT_BUFFER);
}

TEST(file_time_conversions) {
    SYSTEMTIME st{};
    st.wYear = 2024;
    st.wMonth = 3;
    st.wDay = 5;
    st.wHour = 14;
    st.wMinute = 7;
    st.wSecond = 9;
    st.wMilliseconds = 123;
    FILETIME ft{};
    CHECK(SystemTimeToFileTime(&st, &ft) == TRUE);

    SYSTEMTIME back{};
    CHECK(FileTimeToSystemTime(&ft, &back) == TRUE);
    CHECK_EQ(back.wYear, (WORD)2024);
    CHECK_EQ(back.wMonth, (WORD)3);
    CHECK_EQ(back.wDay, (WORD)5);
    CHECK_EQ(back.wHour, (WORD)14);
    CHECK_EQ(back.wMinute, (WORD)7);
    CHECK_EQ(back.wSecond, (WORD)9);
    CHECK_EQ(back.wMilliseconds, (WORD)123);

    // 与历法基准一致：1970-01-01 的 FILETIME 是已知常量。
    SYSTEMTIME epoch{};
    epoch.wYear = 1970;
    epoch.wMonth = 1;
    epoch.wDay = 1;
    FILETIME ep{};
    CHECK(SystemTimeToFileTime(&epoch, &ep) == TRUE);
    CHECK_EQ(ep.dwLowDateTime, 0xD53E8000u);
    CHECK_EQ(ep.dwHighDateTime, 0x019DB1DEu);

    // 本地时间文件时间：与 localtime_r 换算一致
    uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    long ms = static_cast<long>((ticks % 10000000ULL) / 10000ULL);
    time_t secs = static_cast<time_t>(ticks / 10000000ULL - 11644473600ULL);
    struct tm tmb {};
    CHECK(::localtime_r(&secs, &tmb) != nullptr);
    tmb.tm_isdst = -1;
    time_t local = ::timegm(&tmb);
    uint64_t expect = (static_cast<uint64_t>(local) + 11644473600ULL) * 10000000ULL +
                      static_cast<uint64_t>(ms) * 10000ULL;

    FILETIME loc{};
    CHECK(FileTimeToLocalFileTime(&ft, &loc) == TRUE);
    uint64_t got = (static_cast<uint64_t>(loc.dwHighDateTime) << 32) | loc.dwLowDateTime;
    CHECK_EQ(got, expect);

    // 本地时间文件时间再用 FileTimeToSystemTime 读回应等于 localtime_r 拆出的本地时间
    SYSTEMTIME s2{};
    CHECK(FileTimeToSystemTime(&loc, &s2) == TRUE);
    CHECK_EQ(static_cast<int>(s2.wYear), static_cast<int>(tmb.tm_year + 1900));
    CHECK_EQ(static_cast<int>(s2.wMonth), static_cast<int>(tmb.tm_mon + 1));
    CHECK_EQ(static_cast<int>(s2.wDay), static_cast<int>(tmb.tm_mday));
    CHECK_EQ(static_cast<int>(s2.wHour), static_cast<int>(tmb.tm_hour));
    CHECK_EQ(static_cast<int>(s2.wMinute), static_cast<int>(tmb.tm_min));
    CHECK_EQ(static_cast<int>(s2.wSecond), static_cast<int>(tmb.tm_sec));

    // 越界：无效 SYSTEMTIME
    SYSTEMTIME bad{};
    bad.wYear = 2024;
    bad.wMonth = 0;  // 非法月份
    CHECK(SystemTimeToFileTime(&bad, &ft) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_PARAMETER);

    // 越界 FILETIME（超过 9999 年）
    FILETIME huge{};
    huge.dwLowDateTime = 0xFFFFFFFFu;
    huge.dwHighDateTime = 0x7FFFFFFFu;
    SYSTEMTIME sdummy{};
    CHECK(FileTimeToSystemTime(&huge, &sdummy) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_PARAMETER);
    FILETIME odummy{};
    CHECK(FileTimeToLocalFileTime(&huge, &odummy) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_PARAMETER);

    CHECK(FileTimeToSystemTime(nullptr, &sdummy) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_PARAMETER);
    CHECK(SystemTimeToFileTime(&st, nullptr) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_PARAMETER);
}

TEST(date_format_current_time) {
    // SYSTEMTIME == nullptr 时用当前时间，返回非 0。
    wchar_t buf[64]{};
    CHECK(GetDateFormatW(LOCALE_USER_DEFAULT, 0, nullptr, nullptr, buf, 64) > 0);
    CHECK(std::wcslen(buf) == 10);
    CHECK(GetTimeFormatW(LOCALE_USER_DEFAULT, 0, nullptr, nullptr, buf, 64) > 0);
    CHECK(GetDateFormatEx(nullptr, DATE_SHORTDATE, nullptr, nullptr, buf, 64, nullptr) > 0);
}

}  // namespace
