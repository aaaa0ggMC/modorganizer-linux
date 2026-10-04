// WP6: Win32 文件/句柄/目录枚举/文件映射 API 的 Linux 实现。
// 约定：所有接收路径的参数先经 mol_shim::native_path()（含大小写不敏感解析）转成
// Unix 路径再调系统调用；出错时用 last_error_set()/set_last_error_errno() 设置 LastError。
#include "internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

// ---- windows.h 未定义但 Win32 语义需要的本地错误码 -------------------------------
constexpr DWORD kErrorNoMoreFiles = 18;
constexpr DWORD kErrorFileExists = 80;

// ---- 句柄模型 ------------------------------------------------------------------
// HANDLE 一律指向堆上对象，首字段是类型标记 magic；INVALID_HANDLE_VALUE 为 -1。
// CloseHandle() 按类型分派：文件→close(fd)，映射→munmap 未 Unmap 的视图并 close(fd)，
// 目录查找→closedir。未知/非法句柄返回 FALSE + ERROR_INVALID_HANDLE。
constexpr DWORD kMagicFile = 0x46494C45u;     // 'FILE'
constexpr DWORD kMagicMapping = 0x4D415050u;  // 'MAPP'
constexpr DWORD kMagicFind = 0x46494E44u;     // 'FIND'

struct ShimHandle {
    DWORD magic;
    explicit ShimHandle(DWORD m) : magic(m) {}
    virtual ~ShimHandle() = default;
    virtual void release() { delete this; }
};

// 视图登记表：base -> (所属映射句柄, 长度)。ImageNtHeader 需要知道映射长度做边界检查；
// UnmapViewOfFile / CloseHandle(映射) 也靠它找到已 munmap 的视图。
struct ViewRecord {
    ShimHandle* mapping;
    SIZE_T size;
};
std::mutex g_mu;
std::map<void*, ViewRecord> g_views;

struct FileHandle final : ShimHandle {
    explicit FileHandle(int fd) : ShimHandle(kMagicFile), fd(fd) {}
    int fd;
};

struct MappingHandle final : ShimHandle {
    MappingHandle(int fd, uint64_t size) : ShimHandle(kMagicMapping), fd(fd), size(size) {}
    int fd;         // 映射自己持有的 fd（page-file 映射为 -1）
    uint64_t size;  // 映射长度 = 文件大小（除非调用方显式给了大小）
    void release() override {
        {
            // 策略：CloseHandle(映射句柄) 时 munmap 所有尚未 UnmapViewOfFile 的视图。
            std::lock_guard lk(g_mu);
            std::vector<void*> mine;
            for (const auto& [base, rec] : g_views)
                if (rec.mapping == this) mine.push_back(base);
            for (void* base : mine) {
                auto it = g_views.find(base);
                if (it != g_views.end()) {
                    ::munmap(it->first, it->second.size);
                    g_views.erase(it);
                }
            }
        }
        if (fd >= 0) ::close(fd);
        delete this;
    }
};

struct FindHandle final : ShimHandle {
    FindHandle(std::string dir, std::vector<std::string> names)
        : ShimHandle(kMagicFind), dir(std::move(dir)), names(std::move(names)) {}
    std::string dir;          // 目录前缀（无 '/' 时为 "."）
    std::vector<std::string> names;  // 匹配到的条目名（UTF-8，已排序）
    std::size_t idx = 1;      // names[0] 已随 FindFirstFileW 返回
    // 枚举目录时 "." 与 ".." 也会出现在结果里（Windows 语义）。
};

ShimHandle* header_of(HANDLE h) {
    if (!h || reinterpret_cast<uintptr_t>(h) <= 0xFFFF) return nullptr;
    return static_cast<ShimHandle*>(h);
}

FileHandle* file_of(HANDLE h) {
    auto* b = header_of(h);
    return (b && b->magic == kMagicFile) ? static_cast<FileHandle*>(b) : nullptr;
}
MappingHandle* mapping_of(HANDLE h) {
    auto* b = header_of(h);
    return (b && b->magic == kMagicMapping) ? static_cast<MappingHandle*>(b) : nullptr;
}
FindHandle* find_of(HANDLE h) {
    auto* b = header_of(h);
    return (b && b->magic == kMagicFind) ? static_cast<FindHandle*>(b) : nullptr;
}

// ---- 属性辅助 -----------------------------------------------------------------
bool has_write_bit(mode_t m) { return (m & (S_IWUSR | S_IWGRP | S_IWOTH)) != 0; }

DWORD stat_to_attrs(std::string_view name, const struct ::stat& st) {
    DWORD a = 0;
    if (S_ISDIR(st.st_mode)) a |= FILE_ATTRIBUTE_DIRECTORY;
    if (!has_write_bit(st.st_mode)) a |= FILE_ATTRIBUTE_READONLY;
    // "." 与 ".." 不算隐藏（Windows 语义），其余以 '.' 开头视为隐藏。
    if (name != "." && name != "..") {
        auto slash = name.find_last_of('/');
        auto base = (slash == std::string_view::npos) ? name : name.substr(slash + 1);
        if (!base.empty() && base[0] == '.') a |= FILE_ATTRIBUTE_HIDDEN;
    }
    if (a == 0) a = FILE_ATTRIBUTE_NORMAL;
    return a;
}

// FILETIME = 自 1601-01-01 起的 100ns 刻度。
void unix_to_filetime(const struct ::timespec& ts, FILETIME& ft) {
    uint64_t v = (static_cast<uint64_t>(ts.tv_sec) + 11644473600ULL) * 10000000ULL +
                 static_cast<uint64_t>(ts.tv_nsec) / 100ULL;
    ft.dwLowDateTime = static_cast<DWORD>(v & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(v >> 32);
}

// ---- 通配符匹配（ASCII 大小写不敏感，'*' 任意长度，'?' 单字符） -------------------
inline char lc(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 0x20) : c; }

bool wild_match(const char* p, const char* s) {
    const char* star = nullptr;
    const char* ss = s;
    while (*s) {
        if (*p == '?' || lc(*p) == lc(*s)) { ++p; ++s; }
        else if (*p == '*') { star = p++; ss = s; }
        else if (star) { p = star + 1; s = ++ss; }
        else return false;
    }
    while (*p == '*') ++p;
    return *p == 0;
}

void fill_find_data(WIN32_FIND_DATAW* fd, const std::string& full, const std::string& name) {
    std::memset(fd, 0, sizeof(*fd));
    struct ::stat st {};
    bool ok = ::stat(full.c_str(), &st) == 0;
    if (!ok) {
        struct ::stat ls {};
        if (::lstat(full.c_str(), &ls) == 0) { st = ls; ok = true; }
    }
    fd->dwFileAttributes = ok ? stat_to_attrs(name, st) : static_cast<DWORD>(FILE_ATTRIBUTE_NORMAL);
    if (ok) {
        if (!S_ISDIR(st.st_mode)) {
            uint64_t sz = static_cast<uint64_t>(st.st_size);
            fd->nFileSizeHigh = static_cast<DWORD>(sz >> 32);
            fd->nFileSizeLow = static_cast<DWORD>(sz & 0xFFFFFFFFu);
        }
        unix_to_filetime(st.st_ctim, fd->ftCreationTime);
        unix_to_filetime(st.st_atim, fd->ftLastAccessTime);
        unix_to_filetime(st.st_mtim, fd->ftLastWriteTime);
    }
    std::wstring w = mol_shim::utf8_to_wide(name);
    std::wcsncpy(fd->cFileName, w.c_str(), MAX_PATH - 1);
    fd->cAlternateFileName[0] = 0;
}

std::string find_full_path(const FindHandle* fh, const std::string& name) {
    if (fh->dir.empty() || fh->dir == ".") return name;
    if (fh->dir == "/") return "/" + name;
    return fh->dir + "/" + name;
}

}  // namespace

// ---- LastError（后端：mol_shim::last_error_get/set，见 internal.cpp） --------------
DWORD GetLastError() { return mol_shim::last_error_get(); }
void SetLastError(DWORD code) { mol_shim::last_error_set(code); }

// ---- 文件/句柄 ----------------------------------------------------------------
HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attrs,
                   DWORD disposition, DWORD flags, HANDLE templ) {
    UNREFERENCED_PARAMETER(share);
    UNREFERENCED_PARAMETER(attrs);
    UNREFERENCED_PARAMETER(flags);
    UNREFERENCED_PARAMETER(templ);
    if (!name) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    int oflag = 0;
    bool excl = false, mayCreate = false;
    switch (disposition) {
        case CREATE_NEW: oflag = O_CREAT | O_EXCL; excl = true; mayCreate = true; break;
        case CREATE_ALWAYS: oflag = O_CREAT | O_TRUNC; mayCreate = true; break;
        case OPEN_EXISTING: break;
        case OPEN_ALWAYS: oflag = O_CREAT; mayCreate = true; break;
        default:
            mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
            return INVALID_HANDLE_VALUE;
    }

    bool rd = (access & GENERIC_READ) != 0;
    bool wr = (access & GENERIC_WRITE) != 0;
    if (rd && wr) oflag |= O_RDWR;
    else if (wr) oflag |= O_WRONLY;
    else oflag |= O_RDONLY;
    oflag |= O_CLOEXEC;

    // native_path() 做大小写不敏感解析：用 "skyrimse.EXE" 也能打开 "SkyrimSE.exe"。
    std::string path = mol_shim::native_path(name);
    bool existed = ::access(path.c_str(), F_OK) == 0;
    errno = 0;
    int fd = ::open(path.c_str(), oflag, 0666);
    if (fd < 0) {
        if (excl && errno == EEXIST) mol_shim::last_error_set(kErrorFileExists);
        else mol_shim::set_last_error_errno(errno);
        return INVALID_HANDLE_VALUE;
    }
    if (mayCreate && existed) {
        // CREATE_ALWAYS / OPEN_ALWAYS 打开已存在文件时 Windows 置此错误码。
        mol_shim::last_error_set(ERROR_ALREADY_EXISTS);
    }
    return new FileHandle(fd);
}

BOOL CloseHandle(HANDLE h) {
    auto* b = header_of(h);
    if (!b || (b->magic != kMagicFile && b->magic != kMagicMapping && b->magic != kMagicFind)) {
        mol_shim::last_error_set(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    b->release();
    return TRUE;
}

BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* out) {
    auto* fh = file_of(h);
    if (!fh) {
        mol_shim::last_error_set(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!out) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    struct ::stat st {};
    if (::fstat(fh->fd, &st) != 0) {
        mol_shim::set_last_error_errno(errno);
        return FALSE;
    }
    out->QuadPart = static_cast<LONGLONG>(st.st_size);
    return TRUE;
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD count, LPDWORD read, OVERLAPPED* overlapped) {
    auto* fh = file_of(h);
    if (!fh) {
        mol_shim::last_error_set(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (overlapped) {  // 同步句柄，本实现不支持 overlapped
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!buf || !read) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DWORD total = 0;
    char* out = static_cast<char*>(buf);
    while (total < count) {
        ssize_t n = ::read(fh->fd, out + total, count - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            *read = total;
            mol_shim::set_last_error_errno(errno);
            return FALSE;
        }
        if (n == 0) break;  // EOF
        total += static_cast<DWORD>(n);
    }
    *read = total;
    return TRUE;
}

DWORD GetFileAttributesW(LPCWSTR name) {
    if (!name) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return INVALID_FILE_ATTRIBUTES;
    }
    std::string path = mol_shim::native_path(name);
    struct ::stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        struct ::stat ls {};
        if (::lstat(path.c_str(), &ls) == 0) {  // 断链的软链接
            return stat_to_attrs(std::filesystem::path(path).filename().string(), ls);
        }
        mol_shim::set_last_error_errno(errno);
        return INVALID_FILE_ATTRIBUTES;
    }
    return stat_to_attrs(std::filesystem::path(path).filename().string(), st);
}

BOOL SetFileAttributesW(LPCWSTR name, DWORD attrs) {
    if (!name) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    // 只实现 READONLY 位：写权限位 -> chmod。
    std::string path = mol_shim::native_path(name);
    struct ::stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        mol_shim::set_last_error_errno(errno);
        return FALSE;
    }
    mode_t mode = st.st_mode;
    if (attrs & FILE_ATTRIBUTE_READONLY) mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
    else mode |= (S_IWUSR | S_IWGRP | S_IWOTH);
    if (::chmod(path.c_str(), mode) != 0) {
        mol_shim::set_last_error_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL DeleteFileW(LPCWSTR name) {
    if (!name) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::string path = mol_shim::native_path(name);
    if (::unlink(path.c_str()) == 0) return TRUE;
    if (errno == EISDIR) mol_shim::last_error_set(ERROR_ACCESS_DENIED);
    else mol_shim::set_last_error_errno(errno);
    return FALSE;
}

BOOL MoveFileExW(LPCWSTR src, LPCWSTR dst, DWORD flags) {
    if (!src || !dst) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::string s = mol_shim::native_path(src), d = mol_shim::native_path(dst);
    if (!(flags & MOVEFILE_REPLACE_EXISTING)) {
        struct ::stat st {};
        if (::stat(d.c_str(), &st) == 0) {
            mol_shim::last_error_set(ERROR_ALREADY_EXISTS);
            return FALSE;
        }
    }
    errno = 0;
    if (::rename(s.c_str(), d.c_str()) == 0) return TRUE;
    if (errno == EXDEV && (flags & MOVEFILE_COPY_ALLOWED)) {
        std::error_code ec;
        std::filesystem::copy(s, d, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            mol_shim::last_error_set(mol_shim::errno_to_win(ec.value() ? ec.value() : EIO));
            return FALSE;
        }
        std::filesystem::remove_all(s, ec);
        if (ec) {
            mol_shim::last_error_set(mol_shim::errno_to_win(ec.value() ? ec.value() : EIO));
            return FALSE;
        }
        return TRUE;
    }
    mol_shim::set_last_error_errno(errno);
    return FALSE;
}

// ---- 目录枚举 -----------------------------------------------------------------
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* data) {
    if (!pattern || !data) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    std::string p = mol_shim::native_path(pattern);
    while (p.size() > 1 && p.back() == '/') p.pop_back();

    // pattern 的最后一个成分可含 '*'/'?'。
    std::size_t slash = p.find_last_of('/');
    std::string dir, pat;
    if (slash == std::string::npos) {
        pat = p;
    } else {
        dir = p.substr(0, slash == 0 ? 1 : slash);
        pat = p.substr(slash + 1);
    }

    bool wildcard = pat.find('*') != std::string::npos || pat.find('?') != std::string::npos;
    if (!wildcard) {
        // 精确路径（gamegamebryo::getArch 就这么用）：存在则返回该条目。
        struct ::stat st {}, ls {};
        if (::stat(p.c_str(), &st) == 0 || ::lstat(p.c_str(), &ls) == 0) {
            auto* fh = new FindHandle(dir, {pat});
            fill_find_data(data, p, pat);
            return fh;
        }
        mol_shim::set_last_error_errno(errno);
        return INVALID_HANDLE_VALUE;
    }

    std::string dirp = dir.empty() ? std::string(".") : dir;
    DIR* d = ::opendir(dirp.c_str());
    if (!d) {
        mol_shim::set_last_error_errno(errno);
        return INVALID_HANDLE_VALUE;
    }
    std::vector<std::string> names;
    struct dirent* e = nullptr;
    while ((e = ::readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (wild_match(pat.c_str(), n.c_str())) names.push_back(n);
    }
    ::closedir(d);
    std::sort(names.begin(), names.end());  // 确定性顺序
    if (names.empty()) {
        mol_shim::last_error_set(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    auto* fh = new FindHandle(dir, std::move(names));
    fill_find_data(data, find_full_path(fh, fh->names[0]), fh->names[0]);
    return fh;
}

BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* data) {
    auto* fh = find_of(h);
    if (!fh || !data) {
        mol_shim::last_error_set(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (fh->idx >= fh->names.size()) {
        mol_shim::last_error_set(kErrorNoMoreFiles);
        return FALSE;
    }
    const std::string& name = fh->names[fh->idx];
    fill_find_data(data, find_full_path(fh, name), name);
    ++fh->idx;
    return TRUE;
}

BOOL FindClose(HANDLE h) {
    auto* fh = find_of(h);
    if (!fh) {
        mol_shim::last_error_set(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    fh->release();
    return TRUE;
}

// ---- 文件映射 -----------------------------------------------------------------
HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES attrs, DWORD protect, DWORD hi,
                          DWORD lo, LPCWSTR name) {
    UNREFERENCED_PARAMETER(attrs);
    UNREFERENCED_PARAMETER(protect);  // 只支持 PAGE_READONLY / SEC_IMAGE 都被忽略成只读映射
    UNREFERENCED_PARAMETER(name);     // 忽略 name
    uint64_t size = (static_cast<uint64_t>(hi) << 32) | lo;
    int fd = -1;
    if (file != INVALID_HANDLE_VALUE) {
        auto* fh = file_of(file);
        if (!fh) {
            mol_shim::last_error_set(ERROR_INVALID_HANDLE);
            return nullptr;
        }
        fd = ::dup(fh->fd);  // 映射句柄持有自己的 fd
        if (fd < 0) {
            mol_shim::set_last_error_errno(errno);
            return nullptr;
        }
        if (size == 0) {
            struct ::stat st {};
            if (::fstat(fd, &st) == 0) size = static_cast<uint64_t>(st.st_size);
        }
    }
    if (size == 0) {
        if (fd >= 0) ::close(fd);
        mol_shim::last_error_set(ERROR_NOT_ENOUGH_MEMORY);  // 不能映射 0 长度
        return nullptr;
    }
    return new MappingHandle(fd, size);
}

LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHi, DWORD offLo, SIZE_T bytes) {
    UNREFERENCED_PARAMETER(access);  // 只支持 FILE_MAP_READ
    auto* mh = mapping_of(mapping);
    if (!mh) {
        mol_shim::last_error_set(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    uint64_t off = (static_cast<uint64_t>(offHi) << 32) | offLo;
    SIZE_T len = bytes;
    if (len == 0) {
        if (off >= mh->size) {
            mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        len = static_cast<SIZE_T>(mh->size - off);
    }
    if (off > mh->size || len > mh->size - off) {
        mol_shim::last_error_set(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    int mflags = MAP_PRIVATE;
    if (mh->fd < 0) mflags |= MAP_ANONYMOUS;  // page-file 映射
    void* p = ::mmap(nullptr, len, PROT_READ, mflags, mh->fd, static_cast<off_t>(off));
    if (p == MAP_FAILED) {
        mol_shim::set_last_error_errno(errno);
        return nullptr;
    }
    {
        std::lock_guard lk(g_mu);
        g_views[p] = {mh, len};
    }
    return p;
}

BOOL UnmapViewOfFile(LPCVOID base) {
    std::lock_guard lk(g_mu);
    auto it = g_views.find(const_cast<void*>(base));
    if (it == g_views.end()) {
        mol_shim::last_error_set(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ::munmap(it->first, it->second.size);
    g_views.erase(it);
    return TRUE;
}

// ImageNtHeader：校验 "MZ" 与 e_lfanew（带边界检查，需要知道映射长度：用全局 map<base,size>
// 记录 MapViewOfFile 的视图）；再校验 "PE\0\0"，返回指向 NT 头的指针，失败 nullptr。
PIMAGE_NT_HEADERS ImageNtHeader(PVOID base) {
    if (!base) return nullptr;

    // 未登记的 base（调用方自己 mmap 的指针）时按一个内存页兜底，避免按过大长度越界读。
    constexpr SIZE_T kFallbackSize = 0x1000;
    SIZE_T size = kFallbackSize;
    {
        std::lock_guard lk(g_mu);
        auto it = g_views.find(base);
        if (it != g_views.end()) size = it->second.size;
    }
    const auto* p = static_cast<const unsigned char*>(base);
    if (size < 0x40) return nullptr;
    if (p[0] != 'M' || p[1] != 'Z') return nullptr;
    uint32_t lfanew = 0;
    std::memcpy(&lfanew, p + 0x3C, 4);
    if (lfanew < 0x40 || lfanew > size - sizeof(IMAGE_NT_HEADERS)) return nullptr;  // 边界检查
    if (std::memcmp(p + lfanew, "PE\0\0", 4) != 0) return nullptr;
    return const_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<const IMAGE_NT_HEADERS*>(p + lfanew));
}
