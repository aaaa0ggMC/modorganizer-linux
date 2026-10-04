// shim/tests/test_shim_ini.cpp —— Windows 私有 profile（INI）语义测试。
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

std::wstring w(const char* s) { return mol_shim::utf8_to_wide(s); }
std::string u(const wchar_t* s) { return mol_shim::wide_to_utf8(s); }

// 唯一的 /tmp 子目录
struct TmpDir {
    std::string path;
    explicit TmpDir(const char* tag) {
        char buf[512];
        static int counter = 0;
        long pid = static_cast<long>(::getpid());
        std::snprintf(buf, sizeof buf, "/tmp/mol_shim_%s_%d_%ld", tag, counter++, pid);
        path = buf;
        std::string cmd = "rm -rf '" + path + "' && mkdir -p '" + path + "'";
        if (std::system(cmd.c_str()) != 0) CHECK(false);
    }
    ~TmpDir() {
        std::string cmd = "rm -rf '" + path + "'";
        if (std::system(cmd.c_str()) != 0) CHECK(false);
    }
    std::string file(const char* name = "test.ini") const { return path + "/" + name; }
    void check_tmp() const { CHECK(path.rfind("/tmp/", 0) == 0); }
};

bool write_file(const std::string& p, const std::string& data, mode_t mode = 0644) {
    std::string cmd = "rm -f '" + p + "' && printf '%s' '" + data + "' > '" + p + "'";
    if (std::system(cmd.c_str()) != 0) return false;
    if (mode != 0644) {
        char mc[8];
        std::snprintf(mc, sizeof mc, "%o", static_cast<unsigned>(mode & 07777));
        std::string ch = std::string("chmod ") + mc + " '" + p + "'";
        if (std::system(ch.c_str()) != 0) return false;
    }
    return true;
}

std::string read_file(const std::string& p) {
    std::string cmd = "cat '" + p + "'";
    std::string out;
    FILE* f = ::popen(cmd.c_str(), "r");
    if (!f) return out;
    char buf[1024];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    ::pclose(f);
    return out;
}

bool has_substr(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// 归一化行尾，便于断言
std::string lf(const std::string& s) { return replace_all(s, "\r\n", "\n"); }

void fill_proto(TmpDir& d) {
    std::string proto = "; 注释必须在写回后保留\r\n"
                        "\r\n"
                        "[General]\r\n"
                        "sLanguage=ENGLISH\r\n"
                        "; 尾注释\r\n"
                        "uGridsToLoad=5\r\n"
                        "pathval=\"  spaced  \"\r\n"
                        "[Graphics]\r\n"
                        "bFull Screen=1\r\n"
                        "[other]\r\n"
                        "Zed=1\r\n";
    write_file(d.file(), proto);
}

TEST(ini_get_basic) {
    TmpDir d("iniget");
    d.check_tmp();
    fill_proto(d);
    wchar_t buf[256];

    // 大小写不敏感的键/节
    const DWORD n1 = GetPrivateProfileStringW(L"general", L"slanguage", L"X", buf, 256, w(d.file().c_str()).c_str());
    CHECK_EQ(n1, 7u);
    CHECK(u(buf) == "ENGLISH");

    // 不存在的键返回默认值
    const DWORD n2 = GetPrivateProfileStringW(L"General", L"Missing", L"DEF", buf, 256, w(d.file().c_str()).c_str());
    CHECK_EQ(n2, 3u);
    CHECK(u(buf) == "DEF");

    // 引号内的值：去引号
    const DWORD n3 = GetPrivateProfileStringW(L"General", L"pathval", L"X", buf, 256, w(d.file().c_str()).c_str());
    CHECK_EQ(n3, 10u);
    CHECK(u(buf) == "  spaced  ");

    // 缓冲不足：截断 + 结尾 NUL，返回 size-1
    wchar_t small[4];
    const DWORD n4 = GetPrivateProfileStringW(L"General", L"sLanguage", L"X", small, 4, w(d.file().c_str()).c_str());
    CHECK_EQ(n4, 3u);
    CHECK(std::wcslen(small) == 3);
    CHECK(small[3] == L'\0');
}

TEST(ini_get_enum) {
    TmpDir d("inienum");
    d.check_tmp();
    fill_proto(d);
    wchar_t buf[512];

    // section == nullptr：所有节名，NUL 分隔、双 NUL 结尾
    const DWORD n1 = GetPrivateProfileStringW(nullptr, nullptr, nullptr, buf, 512, w(d.file().c_str()).c_str());
    CHECK(n1 == 23);
    static const wchar_t expect[] = L"General\0Graphics\0other\0\0";
    CHECK(std::wstring(buf, buf + 24) == std::wstring(expect, 24));
    CHECK(buf[n1] == L'\0');
    CHECK(buf[static_cast<std::size_t>(n1) + 1] == L'\0');

    // key == nullptr：某节的键名
    const DWORD n2 = GetPrivateProfileStringW(L"General", nullptr, nullptr, buf, 512, w(d.file().c_str()).c_str());
    CHECK(n2 > 0);
    std::wstring keys(buf, buf + static_cast<std::size_t>(n2) + 1);
    CHECK(keys.find(L"sLanguage") != std::wstring::npos);
    CHECK(keys.find(L"uGridsToLoad") != std::wstring::npos);
    CHECK(keys.find(L"pathval") != std::wstring::npos);
    CHECK(keys.find(L"bFull Screen") == std::wstring::npos);

    // 缓冲不足（枚举）：返回 size-2 且双 NUL 结尾
    wchar_t tiny[8];
    const DWORD n3 = GetPrivateProfileStringW(nullptr, nullptr, nullptr, tiny, 8, w(d.file().c_str()).c_str());
    CHECK_EQ(n3, 6u);
    CHECK(tiny[6] == L'\0');
    CHECK(tiny[7] == L'\0');
}

TEST(ini_get_int) {
    TmpDir d("iniint");
    d.check_tmp();
    fill_proto(d);

    CHECK_EQ(GetPrivateProfileIntW(L"General", L"uGridsToLoad", -1, w(d.file().c_str()).c_str()), 5u);
    CHECK_EQ(GetPrivateProfileIntW(L"general", L"ugridstoload", -1, w(d.file().c_str()).c_str()), 5u);
    // 键/节不存在 -> 默认
    CHECK_EQ(GetPrivateProfileIntW(L"General", L"Nope", 42, w(d.file().c_str()).c_str()), 42u);
    CHECK_EQ(GetPrivateProfileIntW(L"NoSection", L"sLanguage", 7, w(d.file().c_str()).c_str()), 7u);
    // 非数字 -> 默认
    write_file(d.file("n.ini"), "[S]\r\nk=abc\r\n");
    CHECK_EQ(GetPrivateProfileIntW(L"S", L"k", 9, w(d.file("n.ini").c_str()).c_str()), 9u);
    // 文件不存在 -> 默认
    CHECK_EQ(GetPrivateProfileIntW(L"S", L"k", 11, w(d.file("nothere.ini").c_str()).c_str()), 11u);
}

TEST(ini_get_missing_file_and_wide_default) {
    TmpDir d("inimis");
    d.check_tmp();
    wchar_t buf[64];
    // 文件不存在 -> 视为空
    const DWORD n = GetPrivateProfileStringW(L"S", L"k", L"fallback", buf, 64, w(d.file("ghost.ini").c_str()).c_str());
    CHECK_EQ(n, 8u);
    CHECK(u(buf) == "fallback");
    // section/key 都不存在，默认值 nullptr
    const DWORD n2 = GetPrivateProfileStringW(L"S", L"k", nullptr, buf, 64, w(d.file("ghost.ini").c_str()).c_str());
    CHECK_EQ(n2, 0u);
    CHECK(buf[0] == L'\0');
}

TEST(ini_write_basic_roundtrip) {
    TmpDir d("iniwrite");
    d.check_tmp();
    fill_proto(d);
    const std::wstring path = w(d.file().c_str());

    // 修改已有键：只有目标键所在行被替换
    CHECK(WritePrivateProfileStringW(L"General", L"sLanguage", L"FRENCH", path.c_str()) == TRUE);
    wchar_t buf[256];
    CHECK_EQ(GetPrivateProfileStringW(L"General", L"sLanguage", nullptr, buf, 256, path.c_str()), 6u);
    CHECK(u(buf) == "FRENCH");

    // 注释、空行、换行风格、未改动行全部保留
    const std::string after = lf(read_file(d.file()));
    CHECK(has_substr(after, "; 注释必须在写回后保留\n"));
    CHECK(has_substr(after, "\n\n[General]\n"));
    CHECK(has_substr(after, "; 尾注释\n"));
    CHECK(has_substr(after, "uGridsToLoad=5\n"));
    CHECK(has_substr(after, "[Graphics]\nbFull Screen=1\n"));
    CHECK(!has_substr(after, "\n\r\n"));

    // 新建键追加到该节末尾（节头后）
    CHECK(WritePrivateProfileStringW(L"General", L"NewKey", L"nv", path.c_str()) == TRUE);
    CHECK_EQ(GetPrivateProfileStringW(L"General", L"NEWKEY", nullptr, buf, 256, path.c_str()), 2u);
    CHECK(u(buf) == "nv");
    const std::string after2 = lf(read_file(d.file()));
    // NewKey 在 sLanguage / uGridsToLoad 之后、Graphics 之前
    CHECK(after2.find("uGridsToLoad=5") < after2.find("NewKey=nv"));
    CHECK(after2.find("NewKey=nv") < after2.find("[Graphics]"));

    // 大小写不敏感的节匹配：写 Other 节只动那一节
    CHECK(WritePrivateProfileStringW(L"OTHER", L"Zed", L"2", path.c_str()) == TRUE);
    CHECK_EQ(GetPrivateProfileIntW(L"Other", L"Zed", 0, path.c_str()), 2u);
    CHECK(!has_substr(lf(read_file(d.file())), "[other2]"));
}

TEST(ini_write_new_section) {
    TmpDir d("ininewsec");
    d.check_tmp();
    const std::string proto = "; 保留\r\n[One]\r\na=1\r\n";
    write_file(d.file(), proto);
    const std::wstring path = w(d.file().c_str());

    // 新节追加到文件末尾
    CHECK(WritePrivateProfileStringW(L"Two", L"b", L"2", path.c_str()) == TRUE);
    wchar_t buf[256];
    CHECK_EQ(GetPrivateProfileStringW(L"two", L"b", nullptr, buf, 256, path.c_str()), 1u);
    CHECK(u(buf) == "2");
    const std::string after = lf(read_file(d.file()));
    CHECK(has_substr(after, "; 保留\n[One]\na=1\n[Two]\nb=2\n"));

    // 在全新文件里写：新文件用 CRLF
    CHECK(WritePrivateProfileStringW(L"S", L"k", L"v", w(d.file("fresh.ini").c_str()).c_str()) == TRUE);
    const std::string fresh = read_file(d.file("fresh.ini"));
    CHECK(fresh == "[S]\r\nk=v\r\n");
    CHECK_EQ(GetPrivateProfileStringW(L"S", L"k", nullptr, buf, 256, w(d.file("fresh.ini").c_str()).c_str()), 1u);
}

TEST(ini_write_delete) {
    TmpDir d("inidel");
    d.check_tmp();
    fill_proto(d);
    const std::wstring path = w(d.file().c_str());
    wchar_t buf[256];

    // value == nullptr：删除键
    CHECK(WritePrivateProfileStringW(L"General", L"sLanguage", nullptr, path.c_str()) == TRUE);
    CHECK_EQ(GetPrivateProfileStringW(L"General", L"sLanguage", L"gone", buf, 256, path.c_str()), 4u);
    CHECK(!has_substr(lf(read_file(d.file())), "sLanguage=ENGLISH"));

    // 删除不存在的键：仍成功
    CHECK(WritePrivateProfileStringW(L"General", L"Nope", nullptr, path.c_str()) == TRUE);

    // key == nullptr：删除整节（保留节头之外的所有内容）
    CHECK(WritePrivateProfileStringW(L"Graphics", nullptr, nullptr, path.c_str()) == TRUE);
    CHECK(!has_substr(lf(read_file(d.file())), "[Graphics]"));
    CHECK(!has_substr(lf(read_file(d.file())), "bFull Screen=1"));
    CHECK(has_substr(lf(read_file(d.file())), "[General]"));
    CHECK(has_substr(lf(read_file(d.file())), "[other]"));

    // 删除不存在的节：无副作用
    CHECK(WritePrivateProfileStringW(L"Ghost", nullptr, nullptr, path.c_str()) == TRUE);
}

TEST(ini_write_errors) {
    TmpDir d("inierr");
    d.check_tmp();

    // 目录不存在 -> FALSE + ERROR_PATH_NOT_FOUND
    CHECK(WritePrivateProfileStringW(L"S", L"k", L"v", w(d.file("nodir/x.ini").c_str()).c_str()) == FALSE);
    CHECK_EQ((DWORD)mol_shim::last_error_get(), (DWORD)ERROR_PATH_NOT_FOUND);

    // 只读文件 -> FALSE + ERROR_ACCESS_DENIED
    write_file(d.file("ro.ini"), "[S]\r\nk=v\r\n", 0444);
    CHECK(WritePrivateProfileStringW(L"S", L"k", L"v2", w(d.file("ro.ini").c_str()).c_str()) == FALSE);
    CHECK_EQ((DWORD)mol_shim::last_error_get(), (DWORD)ERROR_ACCESS_DENIED);
    CHECK_EQ(GetPrivateProfileStringW(L"S", L"k", nullptr, nullptr, 0, w(d.file("ro.ini").c_str()).c_str()), 0u);

    // 写成功 -> ERROR_SUCCESS
    write_file(d.file("ok.ini"), "[S]\r\nk=v\r\n");
    CHECK(WritePrivateProfileStringW(L"S", L"k", L"v2", w(d.file("ok.ini").c_str()).c_str()) == TRUE);
    CHECK_EQ((DWORD)mol_shim::last_error_get(), (DWORD)ERROR_SUCCESS);
}

TEST(ini_write_atomic_keeps_mode) {
    TmpDir d("inimode");
    d.check_tmp();
    write_file(d.file("m.ini"), "[S]\r\nk=v\r\n", 0600);
    struct stat st {};
    CHECK(::stat(d.file("m.ini").c_str(), &st) == 0);
    const mode_t before = st.st_mode & 07777;
    CHECK(WritePrivateProfileStringW(L"S", L"k", L"v2", w(d.file("m.ini").c_str()).c_str()) == TRUE);
    CHECK(::stat(d.file("m.ini").c_str(), &st) == 0);
    CHECK_EQ(st.st_mode & 07777, before);
    // 临时文件不残留
    CHECK(::stat(d.file("m.ini.wshim-tmp").c_str(), &st) != 0);
}

TEST(ini_write_section) {
    TmpDir d("inisec");
    d.check_tmp();
    const std::string proto = "; head\r\n[S]\r\na=1\r\nold=b\r\n; 节内注释\r\nc=3\r\n[T]\r\nz=9\r\n";
    write_file(d.file(), proto);
    const std::wstring path = w(d.file().c_str());

    // key=value 列表（NUL 分隔、双 NUL 结尾），整体替换节内容
    std::vector<wchar_t> data;
    const char* entries[] = {"a=11", "c=33", "brand=new"};
    for (const char* e : entries) {
        const std::wstring we = w(e);
        data.insert(data.end(), we.begin(), we.end());
        data.push_back(L'\0');
    }
    data.push_back(L'\0');

    CHECK(WritePrivateProfileSectionW(L"s", data.data(), path.c_str()) == TRUE);
    wchar_t buf[256];
    CHECK_EQ(GetPrivateProfileStringW(L"S", L"a", nullptr, buf, 256, path.c_str()), 2u);
    CHECK(u(buf) == "11");
    CHECK_EQ(GetPrivateProfileStringW(L"S", L"c", nullptr, buf, 256, path.c_str()), 2u);
    CHECK(u(buf) == "33");
    CHECK_EQ(GetPrivateProfileStringW(L"S", L"brand", nullptr, buf, 256, path.c_str()), 3u);
    CHECK(u(buf) == "new");
    CHECK_EQ(GetPrivateProfileStringW(L"S", L"old", L"gone", buf, 256, path.c_str()), 4u);
    CHECK(!has_substr(lf(read_file(d.file())), "old=b"));

    // 节头保留、其它节不动
    const std::string after = lf(read_file(d.file()));
    CHECK(has_substr(after, "; head\n"));
    CHECK(has_substr(after, "[S]\n"));
    CHECK(has_substr(after, "[T]\nz=9\n"));

    // data == nullptr：删节
    CHECK(WritePrivateProfileSectionW(L"S", nullptr, path.c_str()) == TRUE);
    CHECK(!has_substr(lf(read_file(d.file())), "[S]"));

    // 节不存在：创建
    CHECK(WritePrivateProfileSectionW(L"Fresh", data.data(), path.c_str()) == TRUE);
    CHECK_EQ(GetPrivateProfileStringW(L"fresh", L"brand", nullptr, buf, 256, path.c_str()), 3u);

    // 新文件 + 空 data：只写节头
    CHECK(WritePrivateProfileSectionW(L"Empty", data.data(), w(d.file("e.ini").c_str()).c_str()) == TRUE);
    CHECK(!read_file(d.file("e.ini")).empty());
}

TEST(ini_write_quoted_and_unicode) {
    TmpDir d("iniuni");
    d.check_tmp();
    wchar_t fn[64];
    // UTF-8 BOM + UTF-8 中文内容保留
    std::string proto = "\xEF\xBB\xBF; 中文注释\r\n[S]\r\nbefore=1\r\nk=old\r\n";
    write_file(d.file(), proto);
    const std::wstring path = w(d.file().c_str());
    CHECK(WritePrivateProfileStringW(L"S", L"k", L"值", path.c_str()) == TRUE);
    wchar_t buf[64];
    CHECK_EQ(GetPrivateProfileStringW(L"S", L"k", nullptr, buf, 64, path.c_str()), 1u);
    CHECK(u(buf) == "值");
    const std::string after = read_file(d.file());
    CHECK(after.rfind("\xEF\xBB\xBF", 0) == 0);  // BOM 保留
    CHECK(has_substr(lf(after), "; 中文注释\n"));
    CHECK(has_substr(lf(after), "before=1\n"));
    // 值两端带空白 -> 写引号，读回去掉
    CHECK(WritePrivateProfileStringW(L"S", L"pad", L"  a b  ", path.c_str()) == TRUE);
    CHECK_EQ(GetPrivateProfileStringW(L"S", L"pad", nullptr, buf, 64, path.c_str()), 7u);
    CHECK(u(buf) == "  a b  ");
    // A 版本（UTF-8）
    CHECK_EQ(GetPrivateProfileStringA("s", "k", "d", (LPSTR)fn, 64, d.file().c_str()), 3u);
    CHECK(std::strcmp((const char*)fn, "\xE5\x80\xBC") == 0);
    CHECK_EQ(GetPrivateProfileStringA("s", "missing", "def", (LPSTR)fn, 64, d.file().c_str()), 3u);
    CHECK(std::strcmp((const char*)fn, "def") == 0);
}

TEST(ini_lf_files_stay_lf) {
    TmpDir d("inilf");
    d.check_tmp();
    write_file(d.file(), "; c\n[S]\na=1\n");
    const std::wstring path = w(d.file().c_str());
    CHECK(WritePrivateProfileStringW(L"S", L"b", L"2", path.c_str()) == TRUE);
    CHECK(WritePrivateProfileStringW(L"T", L"c", L"3", path.c_str()) == TRUE);
    CHECK(read_file(d.file()) == "; c\n[S]\na=1\nb=2\n[T]\nc=3\n");
}

}  // namespace
