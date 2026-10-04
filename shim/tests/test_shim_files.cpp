// WP6 files.cpp 测试：句柄、文件 I/O、属性、删除/移动、目录枚举、文件映射、PE 头。
// 所有文件系统操作只用 /tmp 下的唯一临时目录。
#include "windows.h"
#include "internal.hpp"

#include "minitest.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace {

struct TempDir {
    std::string path;
    TempDir() {
        char tpl[] = "/tmp/mol_wp6_files_XXXXXX";
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

std::wstring W(const std::string& s) { return mol_shim::utf8_to_wide(s); }

// 宽字符串转换的临时对象：配合下面的宏，可安全地在一个表达式里转换多个字符串
//（转换结果的临时对象生命周期覆盖整个完整表达式）。
struct WC {
    std::wstring s;
    explicit WC(const std::string& u8) : s(mol_shim::utf8_to_wide(u8)) {}
    const wchar_t* c_str() const { return s.c_str(); }
};
#define WCV(s) WC(s).c_str()

void write_text(const std::string& p, const char* data) {
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot create %s\n", p.c_str());
        std::abort();
    }
    std::fwrite(data, 1, std::strlen(data), f);
    std::fclose(f);
}

void write_bytes(const std::string& p, const void* data, size_t n) {
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) std::abort();
    std::fwrite(data, 1, n, f);
    std::fclose(f);
}

bool exists(const std::string& p) { return std::filesystem::exists(p); }

TEST(last_error_roundtrip) {
    SetLastError(1234);
    CHECK_EQ(GetLastError(), 1234u);
    SetLastError(0);
    CHECK_EQ(GetLastError(), 0u);
}

TEST(create_read_size_close) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string p = t.path + "/hello.txt";
    write_text(p, "Hello, world!");

    HANDLE h = CreateFileW(WCV(p), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE && h != nullptr);

    LARGE_INTEGER sz{};
    CHECK(GetFileSizeEx(h, &sz) == TRUE);
    CHECK_EQ(sz.QuadPart, (LONGLONG)13);

    char buf[64]{};
    DWORD nread = 0;
    CHECK(ReadFile(h, buf, 6, &nread, nullptr) == TRUE);
    CHECK_EQ(nread, 6u);
    CHECK_EQ(std::string(buf, 6), std::string("Hello,"));
    // 第二次读从偏移 6 继续（句柄持有文件位置）
    CHECK(ReadFile(h, buf + 6, 7, &nread, nullptr) == TRUE);
    CHECK_EQ(nread, 7u);
    CHECK_EQ(std::string(buf, 13), std::string("Hello, world!"));
    // 读到 EOF：返回 TRUE 且 0 字节
    CHECK(ReadFile(h, buf, 4, &nread, nullptr) == TRUE);
    CHECK_EQ(nread, 0u);

    CHECK(CloseHandle(h) == TRUE);
}

TEST(create_resolves_case_insensitively) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string real = t.path + "/SkyrimSE.exe";
    write_text(real, "SKYRIM-BINARY");
    CHECK(exists(real));

    // 用大小写不同的名字打开同一文件。
    HANDLE h = CreateFileW(WCV(t.path + "/skyrimse.EXE"), GENERIC_READ, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE && h != nullptr);
    char buf[32]{};
    DWORD n = 0;
    CHECK(ReadFile(h, buf, 13, &n, nullptr) == TRUE);
    CHECK_EQ(std::string(buf, 13), std::string("SKYRIM-BINARY"));
    CHECK(CloseHandle(h) == TRUE);

    // 属性查询同样大小写不敏感。
    CHECK_EQ(GetFileAttributesW(WCV(t.path + "/skyrimse.exe")), (DWORD)FILE_ATTRIBUTE_NORMAL);
}

TEST(create_accepts_windows_and_drive_paths) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string p = t.path + "/foo.txt";
    write_text(p, "DATA");

    // "\\?\" + Unix 路径
    std::wstring wp = L"\\\\?\\" + W(p);
    HANDLE h = CreateFileW(wp.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE && h != nullptr);
    CHECK(CloseHandle(h) == TRUE);

    // "C:\..." 映射到 <prefix>/drive_c
    std::filesystem::create_directories(t.path + "/drive_c");
    std::string dp = t.path + "/drive_c/win.txt";
    write_text(dp, "WIN");
    HANDLE h2 = CreateFileW(L"C:\\win.txt", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h2 != INVALID_HANDLE_VALUE && h2 != nullptr);
    CHECK(CloseHandle(h2) == TRUE);
}

TEST(create_dispositions) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string p = t.path + "/disp.txt";
    write_text(p, "abcdef");

    // OPEN_EXISTING：不存在则失败。
    HANDLE h = CreateFileW(WCV(t.path + "/missing.txt"), GENERIC_READ, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    CHECK(h == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_FILE_NOT_FOUND);

    // OPEN_ALWAYS 打开已存在文件：置 ERROR_ALREADY_EXISTS。
    h = CreateFileW(WCV(p), GENERIC_READ, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE && h != nullptr);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_ALREADY_EXISTS);
    CHECK(CloseHandle(h) == TRUE);

    // OPEN_ALWAYS 创建新文件。
    std::string np = t.path + "/created.txt";
    h = CreateFileW(WCV(np), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE && h != nullptr);
    LARGE_INTEGER sz{};
    CHECK(GetFileSizeEx(h, &sz) == TRUE);
    CHECK_EQ(sz.QuadPart, 0LL);
    CHECK(CloseHandle(h) == TRUE);
    CHECK(exists(np));

    // CREATE_NEW：已存在则失败（ERROR_FILE_EXISTS = 80）。
    h = CreateFileW(WCV(p), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    CHECK(h == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), 80u);

    // CREATE_NEW：不存在则创建。
    std::string cp = t.path + "/fresh.txt";
    h = CreateFileW(WCV(cp), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE && h != nullptr);
    CHECK(CloseHandle(h) == TRUE);
    CHECK(exists(cp));

    // CREATE_ALWAYS：截断已有文件。
    h = CreateFileW(WCV(p), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE && h != nullptr);
    CHECK(GetFileSizeEx(h, &sz) == TRUE);
    CHECK_EQ(sz.QuadPart, 0LL);
    CHECK(CloseHandle(h) == TRUE);

    // NULL 路径 / 非法 disposition。
    CHECK(CreateFileW(nullptr, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr) ==
          INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_PARAMETER);
    CHECK(CreateFileW(WCV(p), GENERIC_READ, 0, nullptr, 99, 0, nullptr) == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_PARAMETER);
}

TEST(readfile_needs_non_overlapped) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string p = t.path + "/ro.txt";
    write_text(p, "abc");
    HANDLE h = CreateFileW(WCV(p), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE && h != nullptr);
    char buf[8]{};
    DWORD n = 0;
    CHECK(ReadFile(h, buf, 3, &n, reinterpret_cast<OVERLAPPED*>(&n)) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_PARAMETER);
    CHECK(CloseHandle(h) == TRUE);
}

TEST(file_attributes) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string normal = t.path + "/normal.txt";
    write_text(normal, "x");
    std::string ro = t.path + "/ro.txt";
    write_text(ro, "x");
    std::string hidden = t.path + "/.hidden";
    write_text(hidden, "x");
    std::string dir = t.path + "/adir";
    std::filesystem::create_directories(dir);
    ::chmod(ro.c_str(), 0444);

    CHECK_EQ(GetFileAttributesW(WCV(normal)), (DWORD)FILE_ATTRIBUTE_NORMAL);
    CHECK_EQ(GetFileAttributesW(WCV(ro)), (DWORD)FILE_ATTRIBUTE_READONLY);
    CHECK_EQ(GetFileAttributesW(WCV(hidden)), (DWORD)FILE_ATTRIBUTE_HIDDEN);
    CHECK_EQ(GetFileAttributesW(WCV(dir)), (DWORD)FILE_ATTRIBUTE_DIRECTORY);
    // 大小写不敏感
    CHECK_EQ(GetFileAttributesW(WCV(t.path + "/NORMAL.TXT")), (DWORD)FILE_ATTRIBUTE_NORMAL);
    // \\?\ 前缀
    CHECK_EQ(GetFileAttributesW((L"\\\\?\\" + W(normal)).c_str()), (DWORD)FILE_ATTRIBUTE_NORMAL);

    DWORD a = GetFileAttributesW(WCV(t.path + "/nope.txt"));
    CHECK_EQ(a, INVALID_FILE_ATTRIBUTES);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_FILE_NOT_FOUND);
    ::chmod(ro.c_str(), 0644);
}

TEST(set_file_attributes_readonly) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string p = t.path + "/rw.txt";
    write_text(p, "x");

    CHECK(SetFileAttributesW(WCV(p), FILE_ATTRIBUTE_READONLY) == TRUE);
    CHECK_EQ(GetFileAttributesW(WCV(p)), (DWORD)FILE_ATTRIBUTE_READONLY);
    CHECK(::access(p.c_str(), W_OK) != 0);

    CHECK(SetFileAttributesW(WCV(p), FILE_ATTRIBUTE_NORMAL) == TRUE);
    CHECK_EQ(GetFileAttributesW(WCV(p)), (DWORD)FILE_ATTRIBUTE_NORMAL);
    CHECK(::access(p.c_str(), W_OK) == 0);

    CHECK(SetFileAttributesW(WCV(t.path + "/nope.txt"), FILE_ATTRIBUTE_READONLY) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_FILE_NOT_FOUND);
}

TEST(delete_file) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string p = t.path + "/del.txt";
    write_text(p, "x");
    CHECK(DeleteFileW(WCV(p)) == TRUE);
    CHECK(!exists(p));

    CHECK(DeleteFileW(WCV(p)) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_FILE_NOT_FOUND);

    std::string dir = t.path + "/sub";
    std::filesystem::create_directories(dir);
    CHECK(DeleteFileW(WCV(dir)) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_ACCESS_DENIED);
}

TEST(move_file_ex) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string a = t.path + "/a.txt", b = t.path + "/b.txt";
    write_text(a, "AAA");
    write_text(b, "BBB");

    // 无 MOVEFILE_REPLACE_EXISTING 时目标已存在 → 失败。
    CHECK(MoveFileExW(WCV(a), WCV(b), 0) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_ALREADY_EXISTS);

    CHECK(MoveFileExW(WCV(a), WCV(b), MOVEFILE_REPLACE_EXISTING) == TRUE);
    CHECK(!exists(a));
    CHECK(exists(b));
    std::string body;
    { FILE* f = std::fopen(b.c_str(), "rb"); char buf[32]{}; fread(buf, 1, 4, f); body = buf; fclose(f); }
    CHECK_EQ(body, std::string("AAA"));

    std::string c = t.path + "/c.txt";
    CHECK(MoveFileExW(WCV(b), WCV(c), 0) == TRUE);
    CHECK(!exists(b) && exists(c));
    CHECK(MoveFileExW(WCV(t.path + "/nope.txt"), WCV(c), MOVEFILE_REPLACE_EXISTING) == FALSE);
    CHECK(GetLastError() != 0u);
}

TEST(find_first_next_wildcards) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    write_text(t.path + "/a.txt", "1");
    write_text(t.path + "/b.txt", "22");
    write_text(t.path + "/C.TXT", "333");
    write_text(t.path + "/SkyrimSE.exe", "EXE");
    std::filesystem::create_directories(t.path + "/sub");

    // pattern 末尾 "*"："." ".." 也会出现（Windows 语义）。
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(WCV(t.path + "/*"), &fd);
    CHECK(h != INVALID_HANDLE_VALUE && h != nullptr);
    std::vector<std::string> names;
    do {
        names.push_back(mol_shim::wide_to_utf8(fd.cFileName));
        if (std::string(mol_shim::wide_to_utf8(fd.cFileName)) == "sub")
            CHECK_EQ(fd.dwFileAttributes, (DWORD)FILE_ATTRIBUTE_DIRECTORY);
        if (std::string(mol_shim::wide_to_utf8(fd.cFileName)) == "a.txt") {
            CHECK_EQ(fd.dwFileAttributes, (DWORD)FILE_ATTRIBUTE_NORMAL);
            CHECK_EQ(fd.nFileSizeLow, 1u);
            CHECK(fd.ftLastWriteTime.dwLowDateTime != 0 || fd.ftLastWriteTime.dwHighDateTime != 0);
        }
        if (std::string(mol_shim::wide_to_utf8(fd.cFileName)) == "b.txt") CHECK_EQ(fd.nFileSizeLow, 2u);
        if (std::string(mol_shim::wide_to_utf8(fd.cFileName)) == "C.TXT") CHECK_EQ(fd.nFileSizeLow, 3u);
    } while (FindNextFileW(h, &fd) == TRUE);
    CHECK_EQ(GetLastError(), 18u);  // ERROR_NO_MORE_FILES
    CHECK(FindClose(h) == TRUE);

    std::vector<std::string> want = {".", "..", "C.TXT", "SkyrimSE.exe", "a.txt", "b.txt", "sub"};
    CHECK_EQ(names.size(), want.size());
    for (size_t i = 0; i < names.size() && i < want.size(); ++i)
        CHECK_EQ(names[i], want[i]);

    // "*.txt" 大小写不敏感匹配
    h = FindFirstFileW(WCV(t.path + "/*.txt"), &fd);
    CHECK(h != INVALID_HANDLE_VALUE && h != nullptr);
    names.clear();
    do {
        names.push_back(mol_shim::wide_to_utf8(fd.cFileName));
    } while (FindNextFileW(h, &fd) == TRUE);
    CHECK(FindClose(h) == TRUE);
    CHECK_EQ(names.size(), (size_t)3);
    CHECK_EQ(names[0], std::string("C.TXT"));
    CHECK_EQ(names[1], std::string("a.txt"));
    CHECK_EQ(names[2], std::string("b.txt"));

    // "?.txt"：只匹配单字符文件名（大小写不敏感，因此 C.TXT 也算）
    h = FindFirstFileW(WCV(t.path + "/?.txt"), &fd);
    CHECK(h != INVALID_HANDLE_VALUE && h != nullptr);
    names.clear();
    do {
        names.push_back(mol_shim::wide_to_utf8(fd.cFileName));
    } while (FindNextFileW(h, &fd) == TRUE);
    CHECK(FindClose(h) == TRUE);
    CHECK_EQ(names.size(), (size_t)3);
    CHECK_EQ(names[0], std::string("C.TXT"));

    // "Skyrim*.exe"
    h = FindFirstFileW(WCV(t.path + "/Skyrim*.exe"), &fd);
    CHECK(h != INVALID_HANDLE_VALUE && h != nullptr);
    CHECK_EQ(std::string(mol_shim::wide_to_utf8(fd.cFileName)), std::string("SkyrimSE.exe"));
    CHECK_EQ(fd.nFileSizeLow, 3u);
    CHECK(FindClose(h) == TRUE);

    // 精确文件路径（gamegamebryo::getArch 就是这么用）：只返回该条目。
    h = FindFirstFileW(WCV(t.path + "/SkyrimSE.exe"), &fd);
    CHECK(h != INVALID_HANDLE_VALUE && h != nullptr);
    CHECK_EQ(std::string(mol_shim::wide_to_utf8(fd.cFileName)), std::string("SkyrimSE.exe"));
    CHECK_EQ(fd.dwFileAttributes, (DWORD)FILE_ATTRIBUTE_NORMAL);
    CHECK(FindNextFileW(h, &fd) == FALSE);
    CHECK_EQ(GetLastError(), 18u);
    CHECK(FindClose(h) == TRUE);

    // 未找到
    CHECK(FindFirstFileW(WCV(t.path + "/nosuch*.dat"), &fd) == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_FILE_NOT_FOUND);
    CHECK(FindFirstFileW(WCV(t.path + "/nope.txt"), &fd) == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_FILE_NOT_FOUND);
    CHECK(FindFirstFileW(WCV(t.path + "/nodir/*"), &fd) == INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_FILE_NOT_FOUND);
}

TEST(file_mapping_and_views) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    std::string p = t.path + "/map.bin";
    write_text(p, "0123456789");

    HANDLE f = CreateFileW(WCV(p), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(f != INVALID_HANDLE_VALUE && f != nullptr);
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CHECK(m != nullptr && m != INVALID_HANDLE_VALUE);

    LPVOID v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CHECK(v != nullptr);
    CHECK_EQ(std::memcmp(v, "0123456789", 10), 0);
    LPVOID v2 = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 8);  // 长度 8（< 文件大小）
    CHECK(v2 != nullptr);
    CHECK_EQ(std::memcmp(static_cast<const char*>(v2) + 2, "23456789", 8), 0);

    CHECK(UnmapViewOfFile(v) == TRUE);
    CHECK(UnmapViewOfFile(v2) == TRUE);
    CHECK(UnmapViewOfFile(v) == FALSE);  // 已释放
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_PARAMETER);

    // 越界长度
    CHECK(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 100) == nullptr);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_ACCESS_DENIED);

    CHECK(CloseHandle(m) == TRUE);
    CHECK(CloseHandle(f) == TRUE);

    // page-file 映射（显式大小）
    HANDLE m2 = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READONLY, 0, 4096, nullptr);
    CHECK(m2 != nullptr && m2 != INVALID_HANDLE_VALUE);
    LPVOID pv = MapViewOfFile(m2, FILE_MAP_READ, 0, 0, 4096);
    CHECK(pv != nullptr);
    CHECK_EQ(static_cast<const unsigned char*>(pv)[4095], 0);
    CHECK(CloseHandle(m2) == TRUE);  // 关闭时 munmap 未 Unmap 的视图
    CHECK(UnmapViewOfFile(pv) == FALSE);

    // 0 长度文件不能映射
    std::string empty = t.path + "/empty.bin";
    write_text(empty, "");
    HANDLE fe = CreateFileW(WCV(empty), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(fe != INVALID_HANDLE_VALUE && fe != nullptr);
    CHECK(CreateFileMappingW(fe, nullptr, PAGE_READONLY, 0, 0, nullptr) == nullptr);
    CHECK(CloseHandle(fe) == TRUE);

    // 非文件句柄
    std::string d = t.path + "/adir";
    std::filesystem::create_directories(d);
    CHECK(CreateFileMappingW(reinterpret_cast<HANDLE>(static_cast<intptr_t>(0x1000)), nullptr,
                             PAGE_READONLY, 0, 0, nullptr) == nullptr);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_HANDLE);
    CHECK(MapViewOfFile(nullptr, FILE_MAP_READ, 0, 0, 0) == nullptr);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_HANDLE);
}

TEST(image_nt_header) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");

    auto map_file = [&](const std::string& p, void*& out, HANDLE& m, HANDLE& f) {
        f = CreateFileW(WCV(p), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(f != INVALID_HANDLE_VALUE && f != nullptr);
        m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
        CHECK(m != nullptr && m != INVALID_HANDLE_VALUE);
        out = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
        CHECK(out != nullptr);
    };

    // 合法 PE
    std::string good = t.path + "/good.exe";
    {
        std::vector<char> buf(512, 0);
        buf[0] = 'M';
        buf[1] = 'Z';
        uint32_t lfanew = 0x100;
        std::memcpy(&buf[0x3C], &lfanew, 4);
        std::memcpy(&buf[0x100], "PE\0\0", 4);
        uint16_t machine = 0x8664;
        std::memcpy(&buf[0x104], &machine, 2);
        write_bytes(good, buf.data(), buf.size());
    }
    void* v{};
    HANDLE m{}, f{};
    map_file(good, v, m, f);
    PIMAGE_NT_HEADERS nt = ImageNtHeader(v);
    CHECK(nt != nullptr);
    if (nt) {
        CHECK_EQ(nt->Signature, 0x00004550u);
        CHECK_EQ((unsigned)nt->FileHeader.Machine, 0x8664u);
    }
    CHECK(UnmapViewOfFile(v) == TRUE);
    CHECK(CloseHandle(m) == TRUE);
    CHECK(CloseHandle(f) == TRUE);

    // MZ 但 e_lfanew 处不是 PE
    std::string noPE = t.path + "/nope.exe";
    {
        std::vector<char> buf(256, 0);
        buf[0] = 'M';
        buf[1] = 'Z';
        uint32_t lfanew = 0x80;
        std::memcpy(&buf[0x3C], &lfanew, 4);
        std::memcpy(&buf[0x80], "NOPE", 4);
        write_bytes(noPE, buf.data(), buf.size());
    }
    map_file(noPE, v, m, f);
    CHECK(ImageNtHeader(v) == nullptr);
    CHECK(UnmapViewOfFile(v) == TRUE);
    CHECK(CloseHandle(m) == TRUE);
    CHECK(CloseHandle(f) == TRUE);

    // e_lfanew 越界
    std::string oob = t.path + "/oob.exe";
    {
        std::vector<char> buf(256, 0);
        buf[0] = 'M';
        buf[1] = 'Z';
        uint32_t lfanew = 0xFFFFFF;
        std::memcpy(&buf[0x3C], &lfanew, 4);
        write_bytes(oob, buf.data(), buf.size());
    }
    map_file(oob, v, m, f);
    CHECK(ImageNtHeader(v) == nullptr);
    CHECK(UnmapViewOfFile(v) == TRUE);
    CHECK(CloseHandle(m) == TRUE);
    CHECK(CloseHandle(f) == TRUE);

    // 太小 / 无 MZ / nullptr
    std::string tiny = t.path + "/tiny.bin";
    {
        char buf[16] = {'M', 'Z'};
        write_bytes(tiny, buf, sizeof(buf));
    }
    map_file(tiny, v, m, f);
    CHECK(ImageNtHeader(v) == nullptr);
    CHECK(UnmapViewOfFile(v) == TRUE);
    CHECK(CloseHandle(m) == TRUE);
    CHECK(CloseHandle(f) == TRUE);

    CHECK(ImageNtHeader(nullptr) == nullptr);
}

TEST(invalid_handles) {
    TempDir t;
    mol_shim_configure(t.path.c_str(), "tester");
    CHECK(CloseHandle(nullptr) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_HANDLE);
    CHECK(CloseHandle(reinterpret_cast<HANDLE>(static_cast<intptr_t>(5))) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_HANDLE);
    CHECK(GetFileSizeEx(nullptr, nullptr) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_HANDLE);
    LARGE_INTEGER sz{};
    CHECK(GetFileSizeEx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(0x20)), &sz) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_HANDLE);
    char buf[4]{};
    DWORD n = 0;
    CHECK(ReadFile(reinterpret_cast<HANDLE>(static_cast<intptr_t>(0x21)), buf, 1, &n, nullptr) ==
          FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_HANDLE);
    CHECK(FindNextFileW(nullptr, nullptr) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_HANDLE);
    CHECK(FindClose(nullptr) == FALSE);
    CHECK_EQ(GetLastError(), (DWORD)ERROR_INVALID_HANDLE);
}

}  // namespace
