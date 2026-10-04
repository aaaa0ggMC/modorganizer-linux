// shim/tests/test_shim_version.cpp —— PE 版本资源（winver.h）测试。
//
// 测试里手工拼装一个最小合法 PE32+/PE（含资源目录 + VS_VERSIONINFO），
// 以及若干畸形输入（截断、RVA 越界、节表为 0）确保不崩溃。
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

// Win32 资源错误码（头文件未定义；与 shim/src/version.cpp 一致）
constexpr DWORD ERROR_RESOURCE_TYPE_NOT_FOUND = 1813;

std::wstring w(const char* s) { return mol_shim::utf8_to_wide(s); }

struct TmpDir {
    std::string path;
    explicit TmpDir(const char* tag) {
        char buf[512];
        static int counter = 0;
        long pid = static_cast<long>(::getpid());
        std::snprintf(buf, sizeof buf, "/tmp/mol_shim_ver_%s_%d_%ld", tag, counter++, pid);
        path = buf;
        std::string cmd = "rm -rf '" + path + "' && mkdir -p '" + path + "'";
        if (std::system(cmd.c_str()) != 0) CHECK(false);
    }
    ~TmpDir() {
        std::string cmd = "rm -rf '" + path + "'";
        if (std::system(cmd.c_str()) != 0) CHECK(false);
    }
    void check_tmp() const { CHECK(path.rfind("/tmp/", 0) == 0); }
    std::string file(const char* n) const { return path + "/" + n; }
};

bool write_bytes(const std::string& p, const std::vector<BYTE>& d) {
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return false;
    bool ok = d.empty() || std::fwrite(d.data(), 1, d.size(), f) == d.size();
    std::fclose(f);
    return ok;
}

// ---- 小端写辅助 -------------------------------------------------------------
void put16(std::vector<BYTE>& b, std::size_t off, WORD v) {
    b[off] = static_cast<BYTE>(v & 0xFF);
    b[off + 1] = static_cast<BYTE>((v >> 8) & 0xFF);
}
void put32(std::vector<BYTE>& b, std::size_t off, DWORD v) {
    for (int i = 0; i < 4; ++i) b[off + i] = static_cast<BYTE>((v >> (8 * i)) & 0xFF);
}
void app16(std::vector<BYTE>& b, WORD v) {
    b.push_back(static_cast<BYTE>(v & 0xFF));
    b.push_back(static_cast<BYTE>((v >> 8) & 0xFF));
}
void put_utf16z(std::vector<BYTE>& b, const std::wstring& s) {
    for (wchar_t c : s) app16(b, static_cast<WORD>(c));
    app16(b, 0);
}
void pad4(std::vector<BYTE>& b) {
    while (b.size() % 4) b.push_back(0);
}

struct VsBlock {
    std::vector<BYTE>& b;
    std::vector<std::size_t> len_at;
    explicit VsBlock(std::vector<BYTE>& buf) : b(buf) {}
    // 开始一个块：wLength(回填) wValueLength wType szKey[utf16z] padding1
    void begin(WORD val_len, WORD type, const std::wstring& key) {
        len_at.push_back(b.size());
        app16(b, 0);
        app16(b, val_len);
        app16(b, type);
        put_utf16z(b, key);
        pad4(b);
    }
    // 写 Value 部分（原始字节，如 VS_FIXEDFILEINFO）
    void value(const void* p, std::size_t n) {
        const BYTE* pb = static_cast<const BYTE*>(p);
        b.insert(b.end(), pb, pb + n);
    }
    // 写 Value 部分（UTF-16LE 字符串 + 结尾 NUL）
    void value_str(const std::wstring& s) {
        for (wchar_t c : s) app16(b, static_cast<WORD>(c));
        app16(b, 0);
    }
    void end() {
        const std::size_t start = len_at.back();
        len_at.pop_back();
        b[start] = static_cast<BYTE>((b.size() - start) & 0xFF);
        b[start + 1] = static_cast<BYTE>(((b.size() - start) >> 8) & 0xFF);
        pad4(b);
    }
    bool open() const { return !len_at.empty(); }
};

VS_FIXEDFILEINFO make_ffi() {
    VS_FIXEDFILEINFO f{};
    f.dwSignature = 0xFEEF04BD;
    f.dwStrucVersion = 0x00010000;
    f.dwFileVersionMS = 0x00020003;  // 2.3
    f.dwFileVersionLS = 0x00040005;
    f.dwProductVersionMS = 0x000A000B;
    f.dwProductVersionLS = 0x000C000D;
    f.dwFileFlagsMask = 0;
    f.dwFileFlags = 0;
    f.dwFileOS = 4;    // VOS_NT_WINDOWS32
    f.dwFileType = 2;  // VFT_DLL
    f.dwFileSubtype = 0;
    f.dwFileDateMS = 0;
    f.dwFileDateLS = 0;
    return f;
}

// 生成一个 VS_VERSIONINFO 资源块（含 FixedFileInfo + StringFileInfo + VarFileInfo）
std::vector<BYTE> make_vs_block(const std::vector<std::pair<std::wstring, std::wstring>>& strings) {
    std::vector<BYTE> b;
    VsBlock vs(b);
    vs.begin(52, 0, L"VS_VERSION_INFO");   // 值长度 = sizeof(VS_FIXEDFILEINFO)
    VS_FIXEDFILEINFO ffi = make_ffi();
    vs.value(&ffi, sizeof ffi);
    // StringFileInfo
    vs.begin(0, 1, L"StringFileInfo");
    vs.begin(0, 1, L"040904b0");
    for (const auto& kv : strings) {
        vs.begin(0, 1, kv.first);
        vs.value_str(kv.second);
        vs.end();
    }
    vs.end();  // StringTable
    vs.end();  // StringFileInfo
    // VarFileInfo
    vs.begin(0, 0, L"VarFileInfo");
    vs.begin(8, 0, L"Translation");
    const DWORD tr[2] = {0x040904B0u, 0x04B004B0u};
    vs.value(tr, sizeof tr);
    vs.end();
    vs.end();  // VarFileInfo
    vs.end();  // VS_VERSION_INFO
    CHECK(!vs.open());
    return b;
}

// ---- 组装最小 PE64 ----------------------------------------------------------
// 布局：DOS 头 | NT 头(PE32+) | 1 个节 (.rsrc, 即资源目录 + 数据)
struct PeBuild {
    std::vector<BYTE> blob;
    std::size_t data_off = 0;    // VS_VERSIONINFO 在文件内的偏移
    std::size_t res_base = 0;    // 资源目录树根的文件偏移
    std::size_t data_rva = 0;    // VS_VERSIONINFO 的 RVA
    bool plus = true;
    DWORD sec_rva = 0x1000;
    DWORD sec_raw = 0x200;
};

void build_pe(PeBuild& out, const std::vector<BYTE>& vs) {
    std::vector<BYTE>& b = out.blob;
    b.clear();

    // ---- 资源目录树（RVA 相对资源区起点） -----------------------------------
    // 0x00 TYPE 目录(16B) + 条目(8B)
    // 0x20 NAME 目录(16B) + 条目(8B)
    // 0x40 LANG 目录(16B) + 条目(8B) -> 指向 0x58
    // 0x58 IMAGE_RESOURCE_DATA_ENTRY(16B)
    // 0x68 VS_VERSIONINFO 数据
    const std::size_t kDataRel = 0x68;
    std::vector<BYTE> r(kDataRel, 0);
    // TYPE 目录
    put16(r, 12, 0);  // NumberOfNamedEntries
    put16(r, 14, 1);  // NumberOfIdEntries
    put16(r, 16, 16);  // Id = RT_VERSION
    put32(r, 20, 0x80000000u | 0x20);
    // NAME 目录
    put16(r, 0x20 + 12, 0);
    put16(r, 0x20 + 14, 1);
    put16(r, 0x30, 1);                       // Id = 1（第一个名字）
    put32(r, 0x34, 0x80000000u | 0x40);
    // LANG 目录
    put16(r, 0x40 + 12, 0);
    put16(r, 0x40 + 14, 1);
    put16(r, 0x50, 0x0409);                  // Id = en-US
    put32(r, 0x54, 0x58);                    // 数据条目（无高位 → 叶子）
    // 数据条目
    out.data_rva = out.sec_rva + static_cast<DWORD>(kDataRel);
    put32(r, 0x58, out.data_rva);                    // OffsetToData (RVA)
    put32(r, 0x5C, static_cast<DWORD>(vs.size()));   // Size
    put32(r, 0x60, 0);                               // CodePage
    put32(r, 0x64, 0);                               // Reserved
    for (BYTE x : vs) r.push_back(x);                // VS_VERSIONINFO 数据

    // ---- PE 头 -------------------------------------------------------------
    const std::size_t opt_size = out.plus ? 240 : 224;
    b.resize(0x40, 0);                  // DOS 头
    b[0] = 'M';
    b[1] = 'Z';
    put32(b, 0x3C, 0x40);               // e_lfanew

    const std::size_t nt = 0x40;
    b.resize(nt + 24, 0);               // Signature(4) + IMAGE_FILE_HEADER(20)
    b[nt] = 'P';
    b[nt + 1] = 'E';
    b[nt + 2] = 0;
    b[nt + 3] = 0;
    put16(b, nt + 4, 0x8664);           // Machine
    put16(b, nt + 6, 1);                // NumberOfSections
    put32(b, nt + 8, 0);                // TimeDateStamp
    put32(b, nt + 12, 0);               // PointerToSymbolTable
    put32(b, nt + 16, 0);               // NumberOfSymbols
    put16(b, nt + 20, static_cast<WORD>(opt_size));
    put16(b, nt + 22, 0x0022);          // Characteristics

    const std::size_t opt = nt + 24;
    b.resize(opt + opt_size, 0);
    put16(b, opt, out.plus ? 0x020B : 0x010B);  // Magic
    put32(b, opt + 60, 0x2000);                 // SizeOfImage
    put32(b, opt + 84, 0x200);                  // SizeOfHeaders
    const std::size_t ndir = out.plus ? 108 : 92;
    put32(b, opt + ndir, 16);                   // NumberOfRvaAndSizes
    const std::size_t dd = opt + (out.plus ? 112 : 96);
    put32(b, dd + 2 * 8, out.sec_rva);          // DataDirectory[2] = 资源 RVA
    put32(b, dd + 2 * 8 + 4, static_cast<DWORD>(r.size()));

    const std::size_t sh = opt + opt_size;      // 节表
    b.resize(sh + 40, 0);
    const char name[] = ".rsrc";
    for (int i = 0; name[i] != '\0'; ++i) b[sh + i] = static_cast<BYTE>(name[i]);
    put32(b, sh + 8, static_cast<DWORD>(r.size()));   // VirtualSize
    put32(b, sh + 12, out.sec_rva);                    // VirtualAddress
    put32(b, sh + 16, static_cast<DWORD>(r.size()));   // SizeOfRawData
    put32(b, sh + 20, out.sec_raw);                    // PointerToRawData

    while (b.size() < out.sec_raw) b.push_back(0);
    out.res_base = b.size();
    out.data_off = out.res_base + kDataRel;
    for (BYTE x : r) b.push_back(x);
}

// 寄生变量说明：本测试用 native_path + resolve_ci，路径按 Unix 传入。
std::wstring pe_path(const TmpDir& d, const char* n) { return w(d.file(n).c_str()); }

}  // namespace

TEST(ver_size_and_info_and_query) {
    TmpDir d("verok");
    d.check_tmp();

    std::vector<std::pair<std::wstring, std::wstring>> strings = {
        {L"FileVersion", L"1.2.3.4"},
        {L"ProductVersion", L"5.6.7.8"},
        {L"CompanyName", L"Mod Organizer Linux"},
    };
    PeBuild pe;
    build_pe(pe, make_vs_block(strings));
    CHECK(write_bytes(d.file("a.exe"), pe.blob));

    const std::wstring p = pe_path(d, "a.exe");
    // 大小写不敏感的路径解析也应命中
    const std::wstring q = w(d.file("A.EXE").c_str());

    DWORD handle = 0;
    const DWORD sz = GetFileVersionInfoSizeW(p.c_str(), &handle);
    CHECK(sz > 0);
    CHECK(mol_shim::last_error_get() == ERROR_SUCCESS);
    std::vector<BYTE> buf(sz);
    CHECK(GetFileVersionInfoW(p.c_str(), 0, sz, buf.data()) == TRUE);

    // "\\" → VS_FIXEDFILEINFO
    UINT len = 0;
    LPVOID ptr = nullptr;
    CHECK(VerQueryValueW(buf.data(), L"\\", &ptr, &len) == TRUE);
    CHECK(len == sizeof(VS_FIXEDFILEINFO));
    CHECK(ptr != nullptr);
    const auto* ffi = static_cast<const VS_FIXEDFILEINFO*>(ptr);
    CHECK(ffi->dwSignature == 0xFEEF04BD);
    CHECK(ffi->dwFileVersionMS == 0x00020003);
    CHECK(ffi->dwFileVersionLS == 0x00040005);
    CHECK(ffi->dwProductVersionMS == 0x000A000B);
    CHECK(ffi->dwProductVersionLS == 0x000C000D);

    // "\\StringFileInfo\\<lang.cp>\\<key>"
    wchar_t sub[256] = L"\\StringFileInfo\\040904b0\\FileVersion";
    CHECK(VerQueryValueW(buf.data(), sub, &ptr, &len) == TRUE);
    CHECK(std::wcscmp(static_cast<const wchar_t*>(ptr), L"1.2.3.4") == 0);
    CHECK(len == 8);  // 字符数含 NUL

    std::wcscpy(sub, L"\\VarFileInfo\\Translation");
    CHECK(VerQueryValueW(buf.data(), sub, &ptr, &len) == TRUE);
    CHECK(ptr != nullptr);
    CHECK(len == 8);  // 两个 DWORD
    const auto* tr = static_cast<const DWORD*>(ptr);
    CHECK(tr[0] == 0x040904B0u);
    CHECK(tr[1] == 0x04B004B0u);

    // 键名大小写不敏感、缺失键返回 FALSE
    std::wcscpy(sub, L"\\stringfileinfo\\040904B0\\productversion");
    CHECK(VerQueryValueW(buf.data(), sub, &ptr, &len) == TRUE);
    CHECK(std::wcscmp(static_cast<const wchar_t*>(ptr), L"5.6.7.8") == 0);
    CHECK(VerQueryValueW(buf.data(), L"\\StringFileInfo\\040904b0\\NoSuchKey", &ptr, &len) == FALSE);
    CHECK(VerQueryValueW(buf.data(), L"\\VarFileInfo", &ptr, &len) == FALSE);

    // 同一块上连续查询互不覆盖
    const wchar_t* a = nullptr;
    UINT la = 0, lb = 0;
    wchar_t k1[64] = L"\\StringFileInfo\\040904b0\\FileVersion";
    wchar_t k2[64] = L"\\StringFileInfo\\040904b0\\CompanyName";
    CHECK(VerQueryValueW(buf.data(), k1, (LPVOID*)&a, &la) == TRUE);
    const wchar_t* b2 = nullptr;
    CHECK(VerQueryValueW(buf.data(), k2, (LPVOID*)&b2, &lb) == TRUE);
    CHECK(std::wcscmp(a, L"1.2.3.4") == 0);
    CHECK(std::wcscmp(b2, L"Mod Organizer Linux") == 0);
    CHECK((GetFileVersionInfoSizeW(q.c_str(), &handle)) == sz);

    // PE32（非 +）同样解析
    PeBuild pe32;
    pe32.plus = false;
    build_pe(pe32, make_vs_block(strings));
    CHECK(write_bytes(d.file("pe32.exe"), pe32.blob));
    const DWORD h2 = GetFileVersionInfoSizeW(pe_path(d, "pe32.exe").c_str(), nullptr);
    CHECK(h2 > 0);
    std::vector<BYTE> b32(h2);
    CHECK(GetFileVersionInfoW(pe_path(d, "pe32.exe").c_str(), 0, h2, b32.data()) == TRUE);
    CHECK(VerQueryValueW(b32.data(), L"\\StringFileInfo\\040904b0\\FileVersion", &ptr, &len) == TRUE);
    CHECK(std::wcscmp(static_cast<const wchar_t*>(ptr), L"1.2.3.4") == 0);
}

TEST(ver_missing_resource) {
    TmpDir d("vernone");
    d.check_tmp();
    // 有头、有节，但没有版本资源数据
    PeBuild pe;
    build_pe(pe, make_vs_block({{L"FileVersion", L"1"}}));
    // 资源目录里没有 RT_VERSION 条目 → “无版本资源”（头与目录本身合法）
    put16(pe.blob, pe.res_base + 16, 1);  // 资源类型 1（RT_CURSOR）而不是 16
    CHECK(write_bytes(d.file("b.exe"), pe.blob));

    DWORD h = 0;
    CHECK(GetFileVersionInfoSizeW(pe_path(d, "b.exe").c_str(), &h) == 0);
    CHECK(mol_shim::last_error_get() == ERROR_RESOURCE_TYPE_NOT_FOUND);
    // GetFileVersionInfoW 同样失败且不写缓冲
    std::vector<BYTE> out(64, 0xAB);
    CHECK(GetFileVersionInfoW(pe_path(d, "b.exe").c_str(), 0, 64, out.data()) == FALSE);
    CHECK(out[0] == (BYTE)0xAB);

    // 空文件 / 文本文件 → 不是 PE
    write_bytes(d.file("c.txt"), std::vector<BYTE>({'h', 'e', 'l', 'l', 'o'}));
    CHECK(GetFileVersionInfoSizeW(pe_path(d, "c.txt").c_str(), &h) == 0);
    CHECK(mol_shim::last_error_get() == ERROR_BAD_EXE_FORMAT);
    write_bytes(d.file("empty.exe"), std::vector<BYTE>{});
    CHECK(GetFileVersionInfoSizeW(pe_path(d, "empty.exe").c_str(), &h) == 0);
    CHECK(mol_shim::last_error_get() == ERROR_BAD_EXE_FORMAT);
}

TEST(ver_malformed) {
    TmpDir d("verbad");
    d.check_tmp();
    PeBuild pe;
    build_pe(pe, make_vs_block({{L"FileVersion", L"1.0"}}));
    const std::vector<BYTE>& good = pe.blob;

    // 1) 截断：前 N 个字节均应安全失败
    for (std::size_t n : {size_t(2), size_t(0x40), size_t(0x100), good.size() / 2, good.size() - 1}) {
        std::vector<BYTE> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK(write_bytes(d.file("t.exe"), cut));
        DWORD h = 0;
        const DWORD sz = GetFileVersionInfoSizeW(pe_path(d, "t.exe").c_str(), &h);
        CHECK(sz == 0);
        std::vector<BYTE> tmp(4096, 0);
        CHECK(GetFileVersionInfoW(pe_path(d, "t.exe").c_str(), 0, 4096, tmp.data()) == FALSE);
    }

    // 2) RVA 越界：把目录[2] 的 VirtualAddress 改到末尾之外
    {
        std::vector<BYTE> bad = good;
        const std::size_t opt = 0x40 + 24;
        const std::size_t dd = opt + (pe.plus ? 112 : 96);
        put32(bad, dd + 2 * 8, 0x7F000000);
        put32(bad, dd + 2 * 8 + 4, 0x1000);
        CHECK(write_bytes(d.file("r.exe"), bad));
        DWORD h = 0;
        CHECK(GetFileVersionInfoSizeW(pe_path(d, "r.exe").c_str(), &h) == 0);
    }
    // 3) 数据条目 RVA 越界
    {
        std::vector<BYTE> bad = good;
        put32(bad, pe.res_base + 0x58, 0x7F000000);  // IMAGE_RESOURCE_DATA_ENTRY.OffsetToData
        CHECK(write_bytes(d.file("d.exe"), bad));
        DWORD h = 0;
        CHECK(GetFileVersionInfoSizeW(pe_path(d, "d.exe").c_str(), &h) == 0);
    }
    // 4) 节表为 0
    {
        std::vector<BYTE> bad = good;
        put16(bad, 0x40 + 6, 0);  // NumberOfSections
        CHECK(write_bytes(d.file("s.exe"), bad));
        DWORD h = 0;
        CHECK(GetFileVersionInfoSizeW(pe_path(d, "s.exe").c_str(), &h) == 0);
        CHECK(mol_shim::last_error_get() == ERROR_BAD_EXE_FORMAT);
    }
    // 5) 恶意 e_lfanew / 坏签名
    {
        std::vector<BYTE> bad = good;
        put32(bad, 0x3C, 0xFFFFFF00u);  // e_lfanew 越界
        CHECK(write_bytes(d.file("f.exe"), bad));
        DWORD h = 0;
        CHECK(GetFileVersionInfoSizeW(pe_path(d, "f.exe").c_str(), &h) == 0);
        std::vector<BYTE> bad2 = good;
        bad2[0x40] = 'X';  // 坏 PE 签名
        CHECK(write_bytes(d.file("g.exe"), bad2));
        CHECK(GetFileVersionInfoSizeW(pe_path(d, "g.exe").c_str(), &h) == 0);
    }
    // 5b) 数据条目 Size 超出文件尾
    {
        std::vector<BYTE> bad = good;
        put32(bad, pe.res_base + 0x5C, 0x7F000000);
        CHECK(write_bytes(d.file("big.exe"), bad));
        DWORD h = 0;
        CHECK(GetFileVersionInfoSizeW(pe_path(d, "big.exe").c_str(), &h) == 0);
    }
    // 5c) 资源目录条目数为 0
    {
        std::vector<BYTE> bad = good;
        put16(bad, pe.res_base + 14, 0);
        CHECK(write_bytes(d.file("zero.exe"), bad));
        DWORD h = 0;
        CHECK(GetFileVersionInfoSizeW(pe_path(d, "zero.exe").c_str(), &h) == 0);
    }
    // 5d) 资源目录类型条目指向文件名（高位为 1 但不是合法偏移）
    {
        std::vector<BYTE> bad = good;
        put32(bad, pe.res_base + 20, 0x80000000u);  // 指向自身 → 死循环风险
        CHECK(write_bytes(d.file("self.exe"), bad));
        DWORD h = 0;
        CHECK(GetFileVersionInfoSizeW(pe_path(d, "self.exe").c_str(), &h) == 0);
    }
    // 5e) VS_VERSIONINFO 本体被截断：块 wLength 越界
    {
        std::vector<BYTE> bad = good;
        put16(bad, pe.data_off, 0x7F00);      // wLength 超长
        put16(bad, pe.data_off + 2, 0x7F00);  // wValueLength 超长
        CHECK(write_bytes(d.file("vs.exe"), bad));
        DWORD h = 0;
        const DWORD sz = GetFileVersionInfoSizeW(pe_path(d, "vs.exe").c_str(), &h);
        if (sz != 0) {
            std::vector<BYTE> buf(sz);
            CHECK(GetFileVersionInfoW(pe_path(d, "vs.exe").c_str(), 0, sz, buf.data()) == TRUE);
            UINT len = 0;
            LPVOID ptr = nullptr;
            CHECK(VerQueryValueW(buf.data(), L"\\", &ptr, &len) == FALSE);
            CHECK(VerQueryValueW(buf.data(), L"\\StringFileInfo\\040904b0\\FileVersion", &ptr, &len) ==
                  FALSE);
        }
    }
    // 6) 文件不存在
    CHECK(GetFileVersionInfoSizeW(pe_path(d, "ghost.exe").c_str(), nullptr) == 0);
    CHECK(mol_shim::last_error_get() == ERROR_FILE_NOT_FOUND);
    // 7) GetFileVersionInfoW 缓冲不足（用一个正常 PE）
    {
        PeBuild p2;
        build_pe(p2, make_vs_block({{L"FileVersion", L"2"}}));
        CHECK(write_bytes(d.file("ok.exe"), p2.blob));
        const DWORD sz = GetFileVersionInfoSizeW(pe_path(d, "ok.exe").c_str(), nullptr);
        CHECK(sz > 0);
        std::vector<BYTE> tiny(sz - 1, 0);
        CHECK(GetFileVersionInfoW(pe_path(d, "ok.exe").c_str(), 0, sz - 1, tiny.data()) == FALSE);
        CHECK(mol_shim::last_error_get() == ERROR_INSUFFICIENT_BUFFER);
    }
}
