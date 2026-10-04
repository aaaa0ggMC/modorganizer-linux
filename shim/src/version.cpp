// libwinshim —— PE 版本资源（winver.h）。
//
//  * 完整 PE 解析：DOS 头 → NT 头（PE32 / PE32+）→ 数据目录[2]（资源）→
//    资源目录 type(16=RT_VERSION) → name → language 三级下降取第一个叶子；
//  * RVA → 文件偏移通过节表转换，全程边界检查（畸形/截断文件不越界、不崩溃）；
//  * GetFileVersionInfoW 拷贝原始 VS_VERSIONINFO 资源块（保持 Windows 的
//    UTF-16LE 布局）；VerQueryValueW 支持 "\"、"\VarFileInfo\Translation"、
//    "\StringFileInfo\<lang.cp>\<key>"，字符串转换为 4 字节 wchar_t 后返回。
#include "internal.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

using mol_shim::last_error_set;

// Win32 错误码补充（头文件不含，不外泄）
constexpr DWORD ERROR_RESOURCE_TYPE_NOT_FOUND = 1813;
constexpr DWORD ERROR_RESOURCE_NAME_NOT_FOUND = 1814;
constexpr DWORD ERROR_INVALID_DATA = 13;
constexpr WORD RT_VERSION = 16;

// ---- 只读视图 ---------------------------------------------------------------
struct Buf {
    const unsigned char* p = nullptr;
    std::size_t n = 0;

    bool ok(std::size_t off, std::size_t len) const { return off <= n && len <= n - off; }
    bool u16(std::size_t off, WORD& v) const {
        if (!ok(off, 2)) return false;
        v = static_cast<WORD>(p[off] | (p[off + 1] << 8));
        return true;
    }
    bool u32(std::size_t off, DWORD& v) const {
        if (!ok(off, 4)) return false;
        v = static_cast<DWORD>(p[off]) | (static_cast<DWORD>(p[off + 1]) << 8) |
            (static_cast<DWORD>(p[off + 2]) << 16) | (static_cast<DWORD>(p[off + 3]) << 24);
        return true;
    }
};

std::size_t align4(std::size_t x) { return (x + 3) & ~static_cast<std::size_t>(3); }

// UTF-16LE 字节区间 → UTF-32 宽字符串（处理代理对，非法码位用 U+FFFD）。
std::wstring utf16_to_wide(const unsigned char* p, const unsigned char* end) {
    std::wstring out;
    const unsigned char* q = p;
    while (q + 1 < end) {
        WORD ch = static_cast<WORD>(q[0] | (q[1] << 8));
        q += 2;
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (q + 1 < end) {
                WORD lo = static_cast<WORD>(q[0] | (q[1] << 8));
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    q += 2;
                    char32_t c = 0x10000 + ((static_cast<char32_t>(ch - 0xD800)) << 10) + (lo - 0xDC00);
                    out.push_back(static_cast<wchar_t>(c));
                    continue;
                }
            }
            out.push_back(static_cast<wchar_t>(0xFFFD));
            continue;
        }
        if (ch >= 0xDC00 && ch <= 0xDFFF) {
            out.push_back(static_cast<wchar_t>(0xFFFD));
            continue;
        }
        out.push_back(static_cast<wchar_t>(ch));
    }
    return out;
}

// 读取 [beg, end) 内 NUL 结尾的 UTF-16 字符串；无 NUL 则取整个区间。
std::wstring utf16_zstring(const unsigned char* beg, const unsigned char* end) {
    const unsigned char* stop = beg;
    while (stop + 1 < end) {
        if (stop[0] == 0 && stop[1] == 0) break;
        stop += 2;
    }
    return utf16_to_wide(beg, stop + 2);  // 含结尾 NUL 的两个字节
}

bool ieq16(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x = static_cast<wchar_t>(x + 32);
        if (y >= L'A' && y <= L'Z') y = static_cast<wchar_t>(y + 32);
        if (x != y) return false;
    }
    return true;
}

// ---- 文件读取 ---------------------------------------------------------------
bool load_file(LPCWSTR file, std::vector<unsigned char>& out) {
    out.clear();
    if (!file) {
        last_error_set(ERROR_INVALID_PARAMETER);
        return false;
    }
    std::string path = mol_shim::native_path(file);
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        mol_shim::set_last_error_errno(errno);
        return false;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        mol_shim::set_last_error_errno(errno);
        ::close(fd);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        // 目录等非普通文件：不是合法 PE
        ::close(fd);
        last_error_set(ERROR_BAD_EXE_FORMAT);
        return false;
    }
    out.reserve(static_cast<std::size_t>(st.st_size));
    unsigned char buf[65536];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            int e = errno;
            ::close(fd);
            out.clear();
            mol_shim::set_last_error_errno(e);
            return false;
        }
        if (n == 0) break;
        out.insert(out.end(), buf, buf + n);
    }
    ::close(fd);
    return true;
}

// ---- PE 解析 ----------------------------------------------------------------
struct ResBlock {
    std::size_t off = 0;
    DWORD size = 0;
};

struct PeLayout {
    std::size_t sec_off = 0;
    WORD num_sec = 0;
};

bool rva_to_off(const Buf& b, const PeLayout& pl, DWORD rva, std::size_t& out) {
    DWORD first_va = 0;
    bool have_first = false;
    for (WORD i = 0; i < pl.num_sec; ++i) {
        std::size_t s = pl.sec_off + static_cast<std::size_t>(i) * 40;
        if (!b.ok(s, 40)) return false;
        DWORD vsize = 0, vaddr = 0, rawsize = 0, rawptr = 0;
        if (!b.u32(s + 8, vsize) || !b.u32(s + 12, vaddr) || !b.u32(s + 16, rawsize) ||
            !b.u32(s + 20, rawptr))
            return false;
        if (i == 0) {
            first_va = vaddr;
            have_first = true;
        }
        DWORD span = std::max(vsize, rawsize);
        if (span == 0) continue;
        if (rva >= vaddr && rva < vaddr + span) {
            DWORD delta = rva - vaddr;
            if (delta >= rawsize) return false;
            std::size_t off = rawptr + delta;
            if (!b.ok(off, 1)) return false;
            out = off;
            return true;
        }
    }
    // 落在所有节之前：文件偏移 == RVA（资源放在头区，常见于手工构造的小 PE）
    if (have_first && rva < first_va) {
        if (!b.ok(rva, 1)) return false;
        out = rva;
        return true;
    }
    return false;
}

struct DirEntry {
    DWORD name_or_id = 0;
    DWORD offset = 0;
};

bool read_dir_entries(const Buf& b, std::size_t dir_off, std::vector<DirEntry>& out) {
    WORD nnamed = 0, nid = 0;
    if (!b.u16(dir_off + 12, nnamed) || !b.u16(dir_off + 14, nid)) return false;
    std::size_t total = static_cast<std::size_t>(nnamed) + nid;
    if (total > 0x100000) return false;
    if (!b.ok(dir_off + 16, total * 8)) return false;
    out.clear();
    out.reserve(total);
    for (std::size_t i = 0; i < total; ++i) {
        DirEntry e;
        if (!b.u32(dir_off + 16 + i * 8, e.name_or_id) || !b.u32(dir_off + 16 + i * 8 + 4, e.offset))
            return false;
        out.push_back(e);
    }
    return true;
}

// 解析 DOS/NT 头与节表；返回资源目录 RVA / Size 与节表位置。
bool parse_pe_headers(const Buf& b, DWORD& res_rva, DWORD& res_size, PeLayout& pl) {
    if (b.n < 0x40) return false;
    if (b.p[0] != 'M' || b.p[1] != 'Z') return false;
    DWORD e_lfanew = 0;
    if (!b.u32(0x3C, e_lfanew)) return false;
    if (e_lfanew < 0x40 || e_lfanew > b.n) return false;
    if (!b.ok(static_cast<std::size_t>(e_lfanew), 24)) return false;
    if (std::memcmp(b.p + e_lfanew, "PE\0\0", 4) != 0) return false;

    const std::size_t fh = static_cast<std::size_t>(e_lfanew) + 4;
    WORD num_sec = 0, opt_size = 0;
    if (!b.u16(fh + 2, num_sec) || !b.u16(fh + 16, opt_size)) return false;
    if (num_sec == 0 || num_sec > 4096) return false;  // 节表为 0 视为畸形
    const std::size_t opt = fh + 20;
    if (opt_size < 112 + 8) return false;
    if (!b.ok(opt, opt_size)) return false;

    WORD magic = 0;
    if (!b.u16(opt, magic)) return false;
    std::size_t dd_off;
    if (magic == 0x20B)
        dd_off = opt + 112;
    else if (magic == 0x10B)
        dd_off = opt + 96;
    else
        return false;
    DWORD ndirs = 0;
    if (!b.u32(dd_off - 4, ndirs)) return false;  // NumberOfRvaAndSizes
    if (ndirs < 3) return false;                  // 没有资源目录

    if (!b.u32(dd_off + 2 * 8, res_rva) || !b.u32(dd_off + 2 * 8 + 4, res_size)) return false;
    if (res_rva == 0 || res_size == 0) return false;

    pl.sec_off = opt + opt_size;
    pl.num_sec = num_sec;
    return b.ok(pl.sec_off, static_cast<std::size_t>(num_sec) * 40);
}

// type(16) → name → language 三级下降，取第一个叶子数据块。
bool find_version_resource(const Buf& b, ResBlock& blk) {
    DWORD res_rva = 0, res_size = 0;
    PeLayout pl;
    if (!parse_pe_headers(b, res_rva, res_size, pl)) return false;
    (void)res_size;
    std::size_t res_off = 0;
    if (!rva_to_off(b, pl, res_rva, res_off)) return false;
    if (!b.ok(res_off, 16)) return false;

    std::size_t cur = res_off;
    for (int level = 0; level <= 4; ++level) {
        std::vector<DirEntry> entries;
        if (!read_dir_entries(b, cur, entries) || entries.empty()) return false;
        const DirEntry* pick = nullptr;
        if (level == 0) {
            for (const DirEntry& e : entries)
                if (!(e.name_or_id & 0x80000000u) && (e.name_or_id & 0xFFFFu) == RT_VERSION) {
                    pick = &e;
                    break;
                }
            if (!pick) return false;
        } else {
            pick = &entries[0];
        }
        if (pick->offset & 0x80000000u) {  // 子目录
            std::size_t next = res_off + (pick->offset & 0x7FFFFFFFu);
            if (next == cur) return false;
            cur = next;
            continue;
        }
        // 叶子：IMAGE_RESOURCE_DATA_ENTRY { RVA, Size, CodePage, Reserved }
        std::size_t de = res_off + pick->offset;
        if (!b.ok(de, 16)) return false;
        DWORD rva = 0, size = 0;
        if (!b.u32(de, rva) || !b.u32(de + 4, size)) return false;
        if (size == 0 || size > b.n) return false;
        std::size_t off = 0;
        if (!rva_to_off(b, pl, rva, off)) return false;
        if (size > b.n - off) return false;  // RVA 越界
        blk.off = off;
        blk.size = size;
        return true;
    }
    return false;
}

// ---- VS_VERSIONINFO 块解析 ---------------------------------------------------
struct Block {
    std::size_t off = 0;    // 起始
    std::size_t end = 0;    // off + wLength
    std::size_t data = 0;   // 值数据偏移
    std::size_t child = 0;  // 第一个子块
    WORD wlen = 0, val_len = 0, type = 0;
    std::wstring key;
};

bool parse_block(const Buf& b, std::size_t off, Block& out) {
    out = Block{};
    out.off = off;
    if (!b.u16(off, out.wlen) || !b.u16(off + 2, out.val_len) || !b.u16(off + 4, out.type)) return false;
    if (out.wlen < 6) return false;
    out.end = off + out.wlen;
    if (out.end > b.n) return false;
    std::size_t p = off + 6;
    std::size_t key_end = p;
    for (;;) {
        if (key_end + 2 > out.end) return false;
        WORD ch = 0;
        if (!b.u16(key_end, ch)) return false;
        key_end += 2;
        if (ch == 0) break;
    }
    out.key = utf16_to_wide(b.p + p, b.p + key_end - 2);
    out.data = align4(key_end);
    if (out.data > out.end) return false;
    out.child = align4(out.data + out.val_len);
    if (out.child > out.end) return false;
    return true;
}

// 取第一个子块，或第一个 key 匹配（大小写不敏感）的子块。
bool find_child(const Buf& b, const Block& parent, const wchar_t* want, Block& out) {
    std::size_t p = parent.child;
    int guard = 0;
    while (p < parent.end) {
        if (++guard > 100000) return false;
        if (!parse_block(b, p, out)) return false;
        if (!want || ieq16(out.key, want)) return true;
        if (out.wlen == 0) return false;
        p = out.end;
    }
    return false;
}

// 返回字符串存进线程局部环形槽，避免连续查询相互覆盖。
const wchar_t* store_wstring(std::wstring s) {
    static thread_local std::vector<std::wstring> ring;
    static thread_local std::size_t idx = 0;
    if (ring.size() < 8) ring.resize(8);
    std::size_t slot = idx;
    idx = (idx + 1) % ring.size();
    ring[slot] = std::move(s);
    return ring[slot].c_str();
}

}  // namespace


DWORD GetFileVersionInfoSizeW(LPCWSTR file, LPDWORD handle) {
    mol_shim::last_error_set(ERROR_SUCCESS);
    if (handle) *handle = 0;
    std::vector<unsigned char> bytes;
    if (!load_file(file, bytes)) return 0;
    Buf b{bytes.data(), bytes.size()};
    DWORD res_rva = 0, res_size = 0;
    PeLayout pl;
    ResBlock blk;
    if (!parse_pe_headers(b, res_rva, res_size, pl)) {
        last_error_set(ERROR_BAD_EXE_FORMAT);
        return 0;
    }
    if (!find_version_resource(b, blk)) {
        last_error_set(ERROR_RESOURCE_TYPE_NOT_FOUND);
        return 0;
    }
    return blk.size;
}

BOOL GetFileVersionInfoW(LPCWSTR file, DWORD handle, DWORD len, LPVOID data) {
    mol_shim::last_error_set(ERROR_SUCCESS);
    UNREFERENCED_PARAMETER(handle);
    std::vector<unsigned char> bytes;
    if (!load_file(file, bytes)) return FALSE;
    Buf b{bytes.data(), bytes.size()};
    DWORD res_rva = 0, res_size = 0;
    PeLayout pl;
    if (!parse_pe_headers(b, res_rva, res_size, pl)) {
        last_error_set(ERROR_BAD_EXE_FORMAT);
        return FALSE;
    }
    ResBlock blk;
    if (!find_version_resource(b, blk)) {
        last_error_set(ERROR_RESOURCE_TYPE_NOT_FOUND);
        return FALSE;
    }
    if (!data) {
        last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (len < blk.size) {
        last_error_set(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::memcpy(data, b.p + blk.off, blk.size);
    return TRUE;
}

BOOL VerQueryValueW(LPCVOID block, LPCWSTR subBlock, LPVOID* buffer, UINT* len) {
    mol_shim::last_error_set(ERROR_SUCCESS);
    if (!block || !subBlock || !buffer) {
        last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *buffer = nullptr;
    if (len) *len = 0;

    const auto* bp = static_cast<const unsigned char*>(block);
    WORD root_len = 0;
    if (!Buf{bp, 6}.u16(0, root_len) || root_len < 6) {
        last_error_set(ERROR_INVALID_DATA);
        return FALSE;
    }
    Buf b{bp, root_len};  // limit = 根块 wLength

    Block root;
    if (!parse_block(b, 0, root)) {
        last_error_set(ERROR_INVALID_DATA);
        return FALSE;
    }
    // 有些工具按“字数”写 wValueLength：少于 52 字节时按字节数归一化
    if (root.val_len < 52 && root.data + 52 <= root.end) {
        root.val_len = 52;
        root.child = align4(root.data + 52);
    }
    if (!ieq16(root.key, L"VS_VERSION_INFO")) {
        last_error_set(ERROR_INVALID_DATA);
        return FALSE;
    }

    // 拆分子块路径（前导 '\' 产生一个空段）
    std::vector<std::wstring> parts;
    {
        const wchar_t* p = subBlock;
        while (*p) {
            const wchar_t* next = p;
            while (*next && *next != L'\\') ++next;
            parts.emplace_back(p, static_cast<std::size_t>(next - p));
            p = (*next == L'\\') ? next + 1 : next;
        }
    }
    if (parts.empty() || parts.size() > 4) {  // 至多 "\StringFileInfo\<lang.cp>\<key>"
        last_error_set(parts.empty() ? ERROR_INVALID_DATA : ERROR_RESOURCE_NAME_NOT_FOUND);
        return FALSE;
    }
    const bool leading_slash = parts.front().empty();
    if (!leading_slash) {
        last_error_set(ERROR_INVALID_DATA);
        return FALSE;
    }
    parts.erase(parts.begin());
    if (parts.empty()) {  // "\"：返回 VS_FIXEDFILEINFO*
        if (root.type != 0 || root.data + sizeof(VS_FIXEDFILEINFO) > root.end) {
            last_error_set(ERROR_INVALID_DATA);
            return FALSE;
        }
        *buffer = const_cast<void*>(static_cast<const void*>(bp + root.data));
        if (len) *len = sizeof(VS_FIXEDFILEINFO);
        return TRUE;
    }

    if (ieq16(parts[0], L"VarFileInfo") && parts.size() >= 2 && ieq16(parts[1], L"Translation")) {
        if (parts.size() > 2) {
            last_error_set(ERROR_RESOURCE_NAME_NOT_FOUND);
            return FALSE;
        }
        Block varinfo;
        if (!find_child(b, root, L"VarFileInfo", varinfo)) {
            last_error_set(ERROR_RESOURCE_NAME_NOT_FOUND);
            return FALSE;
        }
        Block table;
        if (!find_child(b, varinfo, nullptr, table)) {  // 第一个 StringTable
            last_error_set(ERROR_RESOURCE_NAME_NOT_FOUND);
            return FALSE;
        }
        if (table.val_len < 4 || table.data >= b.n) {
            last_error_set(ERROR_INVALID_DATA);
            return FALSE;
        }
        std::size_t avail = table.end - table.data;
        if (table.val_len > avail) {
            last_error_set(ERROR_INVALID_DATA);
            return FALSE;
        }
        *buffer = const_cast<void*>(static_cast<const void*>(bp + table.data));
        if (len) *len = table.val_len;  // 字节数
        return TRUE;
    }

    if (ieq16(parts[0], L"StringFileInfo") && parts.size() >= 2) {
        Block strinfo;
        if (!find_child(b, root, L"StringFileInfo", strinfo)) {
            last_error_set(ERROR_RESOURCE_NAME_NOT_FOUND);
            return FALSE;
        }
        Block table;
        if (!find_child(b, strinfo, parts[1].c_str(), table)) {
            last_error_set(ERROR_RESOURCE_NAME_NOT_FOUND);
            return FALSE;
        }
        if (parts.size() < 3) {  // "\StringFileInfo\<lang.cp>"
            last_error_set(ERROR_RESOURCE_NAME_NOT_FOUND);
            return FALSE;
        }
        Block str;
        if (!find_child(b, table, parts[2].c_str(), str)) {
            last_error_set(ERROR_RESOURCE_NAME_NOT_FOUND);
            return FALSE;
        }
        if (str.data >= str.end) {
            last_error_set(ERROR_INVALID_DATA);
            return FALSE;
        }
        std::wstring value = utf16_zstring(bp + str.data, bp + str.end);  // 含结尾 NUL
        *buffer = const_cast<void*>(static_cast<const void*>(store_wstring(value)));
        if (len) *len = static_cast<UINT>(value.size());  // 含结尾 NUL 的字符数
        return TRUE;
    }

    last_error_set(ERROR_RESOURCE_NAME_NOT_FOUND);
    return FALSE;
}

