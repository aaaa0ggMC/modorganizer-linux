// shim/tests/test_shim_registry.cpp —— 从 Wine 前缀 .reg 文件读注册表（只读）。
#include "windows.h"
#include "internal.hpp"

#include "minitest.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr DWORD REG_BINARY = 3;      // 头文件未定义（仅本测试使用）
constexpr DWORD REG_MULTI_SZ = 7;

struct TmpDir {
    std::string path;
    explicit TmpDir(const char* tag) {
        char buf[512];
        static int counter = 0;
        long pid = static_cast<long>(::getpid());
        std::snprintf(buf, sizeof buf, "/tmp/mol_shim_reg_%s_%d_%ld", tag, counter++, pid);
        path = buf;
        std::string cmd = "rm -rf '" + path + "' && mkdir -p '" + path + "'";
        if (std::system(cmd.c_str()) != 0) CHECK(false);
    }
    ~TmpDir() {
        std::string cmd = "rm -rf '" + path + "'";
        if (std::system(cmd.c_str()) != 0) CHECK(false);
    }
    void check_tmp() const { CHECK(path.rfind("/tmp/", 0) == 0); }
    bool write(const char* rel, const std::string& data) const {
        std::string full = path + "/" + rel;
        std::size_t slash = full.find_last_of('/');
        if (slash != std::string::npos) {
            std::string mk = "mkdir -p '" + full.substr(0, slash) + "'";
            if (std::system(mk.c_str()) != 0) return false;
        }
        std::string w = "printf '%s' '" + data + "' > '" + full + "'";
        return std::system(w.c_str()) == 0;
    }
    std::string reg(const char* n) const { return path + "/" + n; }
};

// 假 Wine 前缀：system.reg + user.reg
const char* kSystemReg =
    "WINE REGEDIT VERSION 2\n"
    "\n"
    "[Software\\\\Bethesda Softworks\\\\Skyrim Special Edition] 1700000000\n"
    "\"Installed Path\"=\"Z:\\\\home\\\\u\\\\Skyrim\"\n"
    "\"Installed\"=dword:00000001\n"
    "@=\"default-value\"\n"
    "\n"
    "[Software\\\\Wow6432Node\\\\Bethesda Softworks\\\\Skyrim Special Edition] 1700000000\n"
    "\"Installed Path\"=\"Z:\\\\wow64\\\\Skyrim\"\n"
    "\n"
    "[Software\\\\Classes\\\\MO2.AssocFile.bsa] 1700000000\n"
    "\"LocalizedString\"=\"MO2 BSA\"\n"
    "\n"
    "[Software\\\\ProgramA] 1700000000\n"
    "; 行尾反斜杠续行\n"
    "\"Expand\"=hex(2):25,00,50,00,52,00,4f,00,47,00,52,00,41,00,4d,00,46,00,49,00,4c,00,45,00,53,00,25,00,\\\n"
    "5c,00,70,00,72,00,6f,00,67,00,72,00,61,00,6d,00,00,00\n"
    "\"SSE\"=hex:48,65,6c,6c,6f\n"
    "\"Multi\"=hex(7):61,00,00,00,62,00,00,00,00,00\n";

const char* kUserReg =
    "WINE REGEDIT VERSION 2\n"
    "\n"
    "[Software\\\\Menu] 1700000000\n"
    "\"Size\"=dword:0000002a\n"
    "\"OnlyInUser\"=\"yes\"\n"
    "\n"
    "[Software\\\\Classes\\\\UserOnlyClass] 1700000000\n"
    "\"Where\"=dword:0000000a\n";

// "Z:\home\u\Skyrim" 的长度（字符数，不含结尾 NUL）
constexpr std::size_t kPathLen = 16;

// 读一个字符串值，返回空表示取不到
std::wstring read_str(HKEY h, const wchar_t* value) {
    DWORD type = 0;
    std::vector<BYTE> buf(1024);
    DWORD size = static_cast<DWORD>(buf.size());
    LSTATUS st = RegQueryValueExW(h, value, nullptr, &type, buf.data(), &size);
    if (st != ERROR_SUCCESS) return {};
    CHECK(size >= sizeof(wchar_t));
    // 缓冲区 = UTF-32 字符串 + 结尾 NUL，这里只取有效字符
    return std::wstring(reinterpret_cast<wchar_t*>(buf.data()),
                        reinterpret_cast<wchar_t*>(buf.data()) + size / sizeof(wchar_t) - 1);
}

DWORD read_dword(HKEY h, const wchar_t* value) {
    DWORD type = 0;
    DWORD val = 0xFFFFFFFF;
    DWORD size = sizeof val;
    LSTATUS st = RegQueryValueExW(h, value, nullptr, &type, reinterpret_cast<BYTE*>(&val), &size);
    if (st != ERROR_SUCCESS) return 0xFFFFFFFF;
    return val;
}

void set_prefix(const TmpDir& d) { mol_shim_configure(d.path.c_str(), "testuser"); }

}  // namespace

TEST(reg_open_query_basic) {
    TmpDir d("regbasic");
    d.check_tmp();
    CHECK(d.write("system.reg", kSystemReg));
    CHECK(d.write("user.reg", kUserReg));
    set_prefix(d);

    // HKLM：键名大小写不敏感
    HKEY h = nullptr;
    CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Bethesda Softworks\\Skyrim Special Edition",
                        0, KEY_READ, &h) == ERROR_SUCCESS);
    CHECK(h != nullptr && h != HKEY_LOCAL_MACHINE);
    CHECK(read_str(h, L"installed path") == L"Z:\\home\\u\\Skyrim");
    CHECK(read_str(h, L"Installed Path") == L"Z:\\home\\u\\Skyrim");
    CHECK_EQ(read_dword(h, L"Installed"), 1u);
    // 默认值（"" 名字）
    CHECK(read_str(h, L"") == L"default-value");
    CHECK(read_dword(h, L"NoSuchValue") == 0xFFFFFFFF);
    CHECK(RegCloseKey(h) == ERROR_SUCCESS);

    // 不存在的键 → ERROR_FILE_NOT_FOUND
    CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\No\\Such\\Key", 0, KEY_READ, &h) ==
          ERROR_FILE_NOT_FOUND);
    CHECK(h == nullptr);

    // HKCU
    CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Menu", 0, KEY_READ, &h) == ERROR_SUCCESS);
    CHECK_EQ(read_dword(h, L"Size"), 42u);
    CHECK(read_str(h, L"OnlyInUser") == L"yes");
    RegCloseKey(h);

    // HKCR → system.reg 的 Software\Classes
    CHECK(RegOpenKeyExW(HKEY_CLASSES_ROOT, L"MO2.AssocFile.bsa", 0, KEY_READ, &h) == ERROR_SUCCESS);
    CHECK(read_str(h, L"LocalizedString") == L"MO2 BSA");
    RegCloseKey(h);

    // HKCU\Software\Classes 覆盖/补充（Wine 里 HKCR 也看 user.reg）
    CHECK(RegOpenKeyExW(HKEY_CLASSES_ROOT, L"UserOnlyClass", 0, KEY_READ, &h) == ERROR_SUCCESS);
    CHECK_EQ(read_dword(h, L"Where"), 10u);
    RegCloseKey(h);

    // 父键路径只需存在（子键在 .reg 里出现过）
    CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software", 0, KEY_READ, &h) == ERROR_SUCCESS);
    RegCloseKey(h);
    CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, nullptr, 0, KEY_READ, &h) == ERROR_SUCCESS);
    RegCloseKey(h);
}

TEST(reg_query_type_and_buffer) {
    TmpDir d("regbuf");
    d.check_tmp();
    CHECK(d.write("system.reg", kSystemReg));
    set_prefix(d);

    HKEY h = nullptr;
    CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\ProgramA", 0, KEY_READ, &h) == ERROR_SUCCESS);

    // 二进制
    BYTE bin[32];
    DWORD type = 0, size = sizeof bin;
    CHECK(RegQueryValueExW(h, L"SSE", nullptr, &type, bin, &size) == ERROR_SUCCESS);
    CHECK(type == REG_BINARY);
    CHECK_EQ(size, 5u);
    CHECK(std::memcmp(bin, "Hello", 5) == 0);

    // hex(2) → REG_EXPAND_SZ，UTF-16LE 解码为 "%PROGRAMFILES%\program"（22 字符）
    std::vector<BYTE> wide(512);
    static const wchar_t kExpect[] = {L'%', L'P', L'R', L'O', L'G', L'R', L'A', L'M', L'F', L'I', L'L',
                                     L'E', L'S', L'%', L'\\', L'p', L'r', L'o', L'g', L'r', L'a', L'm', 0};
    type = 0;
    size = static_cast<DWORD>(wide.size());
    CHECK(RegQueryValueExW(h, L"Expand", nullptr, &type, wide.data(), &size) == ERROR_SUCCESS);
    CHECK(type == REG_EXPAND_SZ);
    CHECK_EQ(size, (22 + 1) * sizeof(wchar_t));
    CHECK(std::wcscmp(reinterpret_cast<wchar_t*>(wide.data()), kExpect) == 0);
    type = 0;
    type = 0;
    size = static_cast<DWORD>(wide.size());
    CHECK(RegQueryValueExW(h, L"Multi", nullptr, &type, wide.data(), &size) == ERROR_SUCCESS);
    CHECK(type == REG_MULTI_SZ);
    CHECK_EQ(size, 5 * sizeof(wchar_t));  // L"a\0b\0\0"
    {
        const wchar_t* ms = reinterpret_cast<const wchar_t*>(wide.data());
        CHECK(std::wcscmp(ms, L"a") == 0);
        CHECK(ms[2] == L'b');
        CHECK(ms[3] == L'\0');
        CHECK(ms[4] == L'\0');
    }

    // 缓冲不足 → ERROR_MORE_DATA 且回填 size
    BYTE tiny[4];
    size = sizeof tiny;
    CHECK(RegQueryValueExW(h, L"SSE", nullptr, &type, tiny, &size) == ERROR_MORE_DATA);
    CHECK_EQ(size, 5u);

    // data == nullptr：只回填所需字节数
    size = 0;
    CHECK(RegQueryValueExW(h, L"SSE", nullptr, &type, nullptr, &size) == ERROR_SUCCESS);
    CHECK_EQ(size, 5u);

    // 找不到的值
    size = sizeof tiny;
    CHECK(RegQueryValueExW(h, L"Nope", nullptr, &type, tiny, &size) == ERROR_FILE_NOT_FOUND);
    RegCloseKey(h);
}

TEST(reg_get_value_filters) {
    TmpDir d("regval");
    d.check_tmp();
    CHECK(d.write("system.reg", kSystemReg));
    set_prefix(d);

    // 直接的子查询（无需先开键）
    DWORD type = 0;
    wchar_t buf[256];
    DWORD size = sizeof buf;
    LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Bethesda Softworks\\Skyrim Special Edition",
                              L"Installed Path", RRF_RT_REG_SZ, &type, buf, &size);
    CHECK(st == ERROR_SUCCESS);
    CHECK(type == REG_SZ);
    CHECK(std::wcscmp(buf, L"Z:\\home\\u\\Skyrim") == 0);
    CHECK_EQ(size, (kPathLen + 1) * sizeof(wchar_t));

    // 子键路径 + RRF_NOEXPAND：原样返回
    size = sizeof buf;
    type = 0;
    st = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\ProgramA", L"Expand", RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                       &type, buf,
                      &size);
    CHECK(st == ERROR_SUCCESS);
    CHECK(type == REG_EXPAND_SZ);

    // 类型不符 → ERROR_UNSUPPORTED_TYPE
    size = sizeof buf;
    CHECK(RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\ProgramA", L"Expand", RRF_RT_REG_DWORD, &type, buf, &size) ==
          ERROR_UNSUPPORTED_TYPE);

    // 默认 RRF_RT_ANY
    size = sizeof buf;
    CHECK(RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\ProgramA", L"SSE", RRF_RT_ANY, &type, buf, &size) ==
          ERROR_SUCCESS);
    CHECK(type == REG_BINARY);

    // RRF_RT_REG_DWORD 匹配 dword
    DWORD dv = 0;
    size = sizeof dv;
    CHECK(RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Bethesda Softworks\\Skyrim Special Edition", L"Installed",
                       RRF_RT_REG_DWORD, &type, &dv, &size) == ERROR_SUCCESS);
    CHECK_EQ(dv, 1u);

    // 找不到的键 / 值
    size = sizeof buf;
    CHECK(RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Nothing", L"X", RRF_RT_ANY, &type, buf, &size) ==
          ERROR_FILE_NOT_FOUND);
    CHECK(RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\ProgramA", L"Missing", RRF_RT_ANY, &type, buf, &size) ==
          ERROR_FILE_NOT_FOUND);

    // 缓冲不足
    wchar_t small[4];
    size = sizeof small;
    CHECK(RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Bethesda Softworks\\Skyrim Special Edition",
                       L"Installed Path", RRF_RT_REG_SZ, &type, small, &size) == ERROR_MORE_DATA);
    CHECK_EQ(size, (kPathLen + 1) * sizeof(wchar_t));

    // data == nullptr：回填尺寸
    size = 0;
    CHECK(RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Bethesda Softworks\\Skyrim Special Edition",
                       L"Installed Path", RRF_RT_REG_SZ, &type, nullptr, &size) == ERROR_SUCCESS);
    CHECK_EQ(size, (kPathLen + 1) * sizeof(wchar_t));

    // 子查询默认值
    size = sizeof buf;
    CHECK(RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Bethesda Softworks\\Skyrim Special Edition", nullptr,
                       RRF_RT_REG_SZ, &type, buf, &size) == ERROR_SUCCESS);
    CHECK(std::wcscmp(buf, L"default-value") == 0);
}

TEST(reg_wow64_redirection) {
    TmpDir d("regwow");
    d.check_tmp();
    CHECK(d.write("system.reg", kSystemReg));
    set_prefix(d);

    // KEY_WOW64_32KEY：先试 Software\Wow6432Node\...
    HKEY h = nullptr;
    CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Bethesda Softworks\\Skyrim Special Edition",
                        0, KEY_READ | KEY_WOW64_32KEY, &h) == ERROR_SUCCESS);
    CHECK(read_str(h, L"Installed Path") == L"Z:\\wow64\\Skyrim");
    RegCloseKey(h);

    // KEY_WOW64_64KEY：先试原路径
    CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Bethesda Softworks\\Skyrim Special Edition",
                        0, KEY_READ | KEY_WOW64_64KEY, &h) == ERROR_SUCCESS);
    CHECK(read_str(h, L"Installed Path") == L"Z:\\home\\u\\Skyrim");
    RegCloseKey(h);

    // 两个视图都无此键时，回退一次
    CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\ProgramA", KEY_WOW64_32KEY, KEY_READ, &h) ==
          ERROR_SUCCESS);
    CHECK(read_str(h, L"") == L"");
    RegCloseKey(h);

    // 二级查询跟随句柄记录的 sam
    HKEY base = nullptr;
    CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software", 0, KEY_READ | KEY_WOW64_32KEY, &base) ==
          ERROR_SUCCESS);
    wchar_t buf[256];
    DWORD size = sizeof buf;
    DWORD type = 0;
    CHECK(RegGetValueW(base, L"Bethesda Softworks\\Skyrim Special Edition", L"Installed Path", RRF_RT_ANY, &type,
                       buf, &size) == ERROR_SUCCESS);
    CHECK(std::wcscmp(buf, L"Z:\\wow64\\Skyrim") == 0);
    RegCloseKey(base);
}

TEST(reg_no_prefix_and_missing_file) {
    TmpDir d("regnone");
    d.check_tmp();
    CHECK(d.write("system.reg", kSystemReg));
    set_prefix(d);

    // 前缀目录里只有 system.reg（没有 user.reg）→ HKCU 视为不存在
    HKEY h = nullptr;
    CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Menu", 0, KEY_READ, &h) == ERROR_FILE_NOT_FOUND);

    // 未配置前缀 → 一律“键不存在”（换个空目录）
    {
        TmpDir empty("regempty");
        mol_shim_configure(empty.path.c_str(), nullptr);
        CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software", 0, KEY_READ, &h) == ERROR_FILE_NOT_FOUND);
        mol_shim_configure(d.path.c_str(), "testuser");
    }

    // 刷新缓存：改写 system.reg 后同一进程应看到新值
    CHECK(d.write("system.reg",
                  "WINE REGEDIT VERSION 2\n"
                  "[Software\\\\Bethesda Softworks\\\\Skyrim Special Edition] 1700000000\n"
                  "\"Installed Path\"=\"Z:\\\\reloaded\"\n"));
    CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Bethesda Softworks\\Skyrim Special Edition", 0, KEY_READ,
                        &h) == ERROR_SUCCESS);
    CHECK(read_str(h, L"Installed Path") == L"Z:\\reloaded");
    RegCloseKey(h);

    // 预定义伪句柄不会被解引用
    CHECK(RegCloseKey(HKEY_LOCAL_MACHINE) == ERROR_SUCCESS);
    CHECK(RegCloseKey(HKEY_CURRENT_USER) == ERROR_SUCCESS);
    CHECK(RegCloseKey(nullptr) == ERROR_SUCCESS);
    // 非法句柄
    CHECK(RegQueryValueExW(reinterpret_cast<HKEY>(static_cast<void*>(nullptr)), L"x", nullptr, nullptr, nullptr,
                           nullptr) == ERROR_INVALID_PARAMETER);
    std::vector<BYTE> b(16);
    DWORD size = static_cast<DWORD>(b.size());
    CHECK(RegQueryValueExW(HKEY_CURRENT_USER, L"x", nullptr, nullptr, b.data(), &size) == ERROR_FILE_NOT_FOUND);

}  // namespace
